#include "dicom.h"

#include <algorithm>
#include <fstream>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace dicom {
namespace {

constexpr std::string_view kUidRemapperJournalHeader =
    "# dicomsdl-uid-remapper-v1\t";

[[nodiscard]] std::string normalize_strict_uid_or_throw(
    std::string_view uid, std::string_view name) {
    if (!uid::is_valid_uid_text_strict(uid)) {
        throw std::runtime_error(
            std::string("UidRemapper ") + std::string(name) +
            " must be a strict-valid UID");
    }
    return std::string(uid);
}

[[nodiscard]] std::string normalize_lock_key(
    const std::filesystem::path& path) {
    std::error_code ec;
    auto normalized = std::filesystem::absolute(path, ec);
    if (ec) {
        normalized = path;
    }
    normalized = normalized.lexically_normal();
    return normalized.generic_string();
}

std::mutex g_active_lock_paths_mutex;
std::unordered_set<std::string> g_active_lock_paths;

class JournalLock {
public:
    explicit JournalLock(std::filesystem::path lock_path)
        : lock_path_(std::move(lock_path)),
          normalized_key_(normalize_lock_key(lock_path_)) {
        acquire_process_slot();
        try {
            acquire_os_lock();
            owns_lock_ = true;
        } catch (...) {
            release_process_slot();
            throw;
        }
    }

    JournalLock(const JournalLock&) = delete;
    JournalLock& operator=(const JournalLock&) = delete;

    ~JournalLock() {
        try {
            release();
        } catch (...) {
        }
    }

    void release() {
        if (!owns_lock_) {
            return;
        }
        release_os_lock();
        release_process_slot();
        owns_lock_ = false;
    }

    [[nodiscard]] bool owns_lock() const noexcept { return owns_lock_; }

private:
    void acquire_process_slot() {
        std::lock_guard lock(g_active_lock_paths_mutex);
        const auto [it, inserted] = g_active_lock_paths.insert(normalized_key_);
        if (!inserted) {
            throw std::runtime_error(
                "UidRemapper journal is already open in this process: " +
                lock_path_.string());
        }
        owns_process_slot_ = true;
    }

    void release_process_slot() noexcept {
        if (!owns_process_slot_) {
            return;
        }
        std::lock_guard lock(g_active_lock_paths_mutex);
        g_active_lock_paths.erase(normalized_key_);
        owns_process_slot_ = false;
    }

    void acquire_os_lock() {
#ifdef _WIN32
        handle_ = CreateFileW(
            lock_path_.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            const auto err = static_cast<int>(GetLastError());
            throw std::system_error(
                err,
                std::system_category(),
                "UidRemapper failed to acquire journal lock");
        }
#else
        fd_ = ::open(lock_path_.c_str(), O_RDWR | O_CREAT, 0666);
        if (fd_ < 0) {
            throw std::system_error(
                errno,
                std::system_category(),
                "UidRemapper failed to open journal lock file");
        }
        struct flock lock_spec {};
        lock_spec.l_type = F_WRLCK;
        lock_spec.l_whence = SEEK_SET;
        lock_spec.l_start = 0;
        lock_spec.l_len = 0;
        if (::fcntl(fd_, F_SETLK, &lock_spec) != 0) {
            const int err = errno;
            ::close(fd_);
            fd_ = -1;
            throw std::system_error(
                err,
                std::system_category(),
                "UidRemapper failed to acquire journal lock");
        }
#endif
    }

    void release_os_lock() {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
#else
        if (fd_ >= 0) {
            struct flock lock_spec {};
            lock_spec.l_type = F_UNLCK;
            lock_spec.l_whence = SEEK_SET;
            lock_spec.l_start = 0;
            lock_spec.l_len = 0;
            (void)::fcntl(fd_, F_SETLK, &lock_spec);
            ::close(fd_);
            fd_ = -1;
        }
#endif
    }

    std::filesystem::path lock_path_;
    std::string normalized_key_;
    bool owns_process_slot_ = false;
    bool owns_lock_ = false;
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int fd_ = -1;
#endif
};

}  // namespace

class UidRemapper::Impl {
public:
    Impl(
        std::filesystem::path journal_path,
        std::string uid_root,
        bool flush_on_each_insert)
        : journal_path_(std::move(journal_path)),
          uid_root_(std::move(uid_root)),
          flush_on_each_insert_(flush_on_each_insert) {
        if (journal_path_.empty()) {
            throw std::runtime_error(
                "UidRemapper journal_path must not be empty");
        }
        auto lock_path = journal_path_;
        lock_path += ".lock";
        journal_lock_ = std::make_unique<JournalLock>(std::move(lock_path));
        load_or_initialize_journal();
        open_append_stream();
    }

    [[nodiscard]] std::string map_uid(std::string_view source_uid) {
        const auto normalized_source =
            normalize_strict_uid_or_throw(source_uid, "source_uid");

        {
            std::shared_lock read_lock(mutex_);
            if (closed_) {
                throw std::runtime_error("UidRemapper is closed");
            }
            const auto existing = source_to_target_.find(normalized_source);
            if (existing != source_to_target_.end()) {
                return existing->second;
            }
        }

        std::unique_lock write_lock(mutex_);
        if (closed_) {
            throw std::runtime_error("UidRemapper is closed");
        }
        const auto existing = source_to_target_.find(normalized_source);
        if (existing != source_to_target_.end()) {
            return existing->second;
        }

        std::string target_uid;
        for (int attempts = 0; target_uid.empty() && attempts < 16; ++attempts) {
            const auto generated =
                uid::detail::try_generate_uid_validated_root(uid_root_);
            if (!generated) {
                throw std::runtime_error(
                    "UidRemapper failed to generate target UID");
            }
            target_uid.assign(generated->value());
            const auto existing_target = target_to_source_.find(target_uid);
            if (existing_target == target_to_source_.end()) {
                break;
            }
            if (existing_target->second == normalized_source) {
                return target_uid;
            }
            target_uid.clear();
        }
        if (target_uid.empty()) {
            throw std::runtime_error(
                "UidRemapper failed to find a unique target UID");
        }

        const auto [source_it, inserted_source] =
            source_to_target_.try_emplace(normalized_source, target_uid);
        if (!inserted_source) {
            return source_it->second;
        }
        const auto [target_it, inserted_target] =
            target_to_source_.try_emplace(target_uid, normalized_source);
        if (!inserted_target) {
            source_to_target_.erase(source_it);
            throw std::runtime_error(
                "UidRemapper detected conflicting target mapping");
        }

        try {
            append_mapping_locked(normalized_source, target_uid);
        } catch (...) {
            target_to_source_.erase(target_it);
            source_to_target_.erase(source_it);
            throw;
        }

        return source_it->second;
    }

    void close() {
        std::unique_lock write_lock(mutex_);
        close_locked();
    }

    [[nodiscard]] bool is_valid() const noexcept {
        std::shared_lock read_lock(mutex_);
        return !closed_;
    }

private:
    void load_or_initialize_journal() {
        std::error_code ec;
        const bool exists = std::filesystem::exists(journal_path_, ec);
        if (ec) {
            throw std::runtime_error(
                "UidRemapper failed to inspect journal path: " +
                journal_path_.string());
        }
        if (!exists) {
            initialize_empty_journal();
            return;
        }

        const auto file_size = std::filesystem::file_size(journal_path_, ec);
        if (!ec) {
            const auto estimated_entries =
                static_cast<std::size_t>(std::max<std::uintmax_t>(
                    16, file_size / 48));
            source_to_target_.reserve(estimated_entries);
            target_to_source_.reserve(estimated_entries);
        }

        std::ifstream input(journal_path_, std::ios::binary);
        if (!input.is_open()) {
            throw std::runtime_error(
                "UidRemapper failed to open journal for reading: " +
                journal_path_.string());
        }

        if (input.peek() == std::ifstream::traits_type::eof()) {
            input.close();
            initialize_empty_journal();
            return;
        }

        load_mapping_stream(input, journal_path_.string());
    }

    void initialize_empty_journal() const {
        std::ofstream output(
            journal_path_, std::ios::binary | std::ios::trunc);
        if (!output.is_open()) {
            throw std::runtime_error(
                "UidRemapper failed to create journal: " +
                journal_path_.string());
        }
        output << kUidRemapperJournalHeader << uid_root_ << '\n';
        if (!output.good()) {
            throw std::runtime_error(
                "UidRemapper failed to initialize journal: " +
                journal_path_.string());
        }
        output.flush();
        if (!output.good()) {
            throw std::runtime_error(
                "UidRemapper failed to flush journal header: " +
                journal_path_.string());
        }
    }

    void open_append_stream() {
        if (closed_) {
            throw std::runtime_error(
                "UidRemapper cannot reopen journal after close");
        }
        journal_.open(journal_path_, std::ios::binary | std::ios::app);
        if (!journal_.is_open()) {
            throw std::runtime_error(
                "UidRemapper failed to open journal for append: " +
                journal_path_.string());
        }
    }

    void append_mapping_locked(
        const std::string& source_uid, const std::string& target_uid) {
        if (closed_) {
            throw std::runtime_error("UidRemapper is closed");
        }
        journal_ << source_uid << '\t' << target_uid << '\n';
        if (!journal_.good()) {
            throw std::runtime_error(
                "UidRemapper failed to append mapping to journal");
        }
        if (flush_on_each_insert_) {
            journal_.flush();
            if (!journal_.good()) {
                throw std::runtime_error(
                    "UidRemapper failed to flush journal append");
            }
        }
    }

    void load_mapping_stream(
        std::istream& input, std::string_view stream_name) {
        std::string header;
        if (!std::getline(input, header)) {
            throw std::runtime_error(
                "UidRemapper journal is missing header: " +
                std::string(stream_name));
        }
        if (!header.starts_with(kUidRemapperJournalHeader)) {
            throw std::runtime_error(
                "UidRemapper journal has invalid header: " +
                std::string(stream_name));
        }

        const auto root_text = header.substr(kUidRemapperJournalHeader.size());
        if (const auto file_root =
                normalize_strict_uid_or_throw(root_text, "journal uid_root");
            file_root != uid_root_) {
            throw std::runtime_error(
                "UidRemapper journal uid_root does not match requested root");
        }

        std::string line;
        while (std::getline(input, line)) {
            if (line.empty()) {
                continue;
            }
            const auto separator = line.find('\t');
            if (separator == std::string::npos || line.find('\t', separator + 1) != std::string::npos) {
                throw std::runtime_error(
                    "UidRemapper journal contains invalid mapping row");
            }
            const auto source_uid = normalize_strict_uid_or_throw(
                std::string_view(line).substr(0, separator), "journal source_uid");
            const auto target_uid = normalize_strict_uid_or_throw(
                std::string_view(line).substr(separator + 1), "journal target_uid");

            if (const auto existing = source_to_target_.find(source_uid);
                existing != source_to_target_.end() &&
                existing->second != target_uid) {
                throw std::runtime_error(
                    "UidRemapper journal contains conflicting source mapping");
            }
            if (const auto existing_target = target_to_source_.find(target_uid);
                existing_target != target_to_source_.end() &&
                existing_target->second != source_uid) {
                throw std::runtime_error(
                    "UidRemapper journal contains conflicting target mapping");
            }

            source_to_target_.try_emplace(source_uid, target_uid);
            target_to_source_.try_emplace(target_uid, source_uid);
        }
        if (!input.eof()) {
            throw std::runtime_error(
                "UidRemapper journal read failed: " + std::string(stream_name));
        }
    }

    void close_locked() {
        if (closed_) {
            return;
        }
        std::string close_error;
        if (journal_.is_open()) {
            journal_.flush();
            if (!journal_.good() && close_error.empty()) {
                close_error = "UidRemapper failed to flush journal during close";
            }
            journal_.close();
            if (journal_.fail() && close_error.empty()) {
                close_error = "UidRemapper failed to close journal";
            }
            journal_.clear();
        }
        if (journal_lock_) {
            journal_lock_->release();
        }
        closed_ = true;
        if (!close_error.empty()) {
            throw std::runtime_error(close_error);
        }
    }

    std::filesystem::path journal_path_;
    std::string uid_root_;
    std::unordered_map<std::string, std::string> source_to_target_;
    std::unordered_map<std::string, std::string> target_to_source_;
    std::ofstream journal_;
    std::unique_ptr<JournalLock> journal_lock_;
    mutable std::shared_mutex mutex_;
    bool flush_on_each_insert_ = true;
    bool closed_ = false;
};

UidRemapper UidRemapper::in_memory(
    const std::filesystem::path& journal_path,
    std::string_view uid_root,
    bool flush_on_each_insert) {
    return UidRemapper(std::make_shared<Impl>(
        journal_path,
        normalize_strict_uid_or_throw(uid_root, "uid_root"),
        flush_on_each_insert));
}

std::string UidRemapper::map_uid(std::string_view source_uid) {
    if (!impl_) {
        throw std::runtime_error("UidRemapper is not initialized");
    }
    return impl_->map_uid(source_uid);
}

void UidRemapper::close() {
    if (!impl_) {
        return;
    }
    impl_->close();
}

bool UidRemapper::is_valid() const noexcept {
    return impl_ && impl_->is_valid();
}

}  // namespace dicom
