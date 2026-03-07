// Benchmark: read all DICOM files under a directory using dicom::read_file (new build).
// Usage:
//   ./read_all_dcm_cpp [root] [repeat] [source]
//   root   : directory to scan recursively (default: /Users/tsangel/Documents/workspace.dev/sample/ncc/3121/pt)
//   repeat : number of times to iterate the whole set (default: 10)
//   source : file | memory (default: file) — memory preloads each file into RAM before parsing

#include "dicom.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using namespace dicom::literals;

enum class SourceMode {
	file,
	memory,
	memory_nocache,
};

enum class WorkloadMode {
	parse_only,
	read,
	read_decode,
};

const char* source_mode_name(SourceMode mode) noexcept {
	switch (mode) {
	case SourceMode::file:
		return "file";
	case SourceMode::memory:
		return "memory";
	case SourceMode::memory_nocache:
		return "memory_nocache";
	}
	return "unknown";
}

const char* workload_mode_name(WorkloadMode mode) noexcept {
	switch (mode) {
	case WorkloadMode::parse_only:
		return "parse_only";
	case WorkloadMode::read:
		return "read";
	case WorkloadMode::read_decode:
		return "read_decode";
	}
	return "unknown";
}

SourceMode parse_source_mode(std::string_view text) {
	if (text == "file") {
		return SourceMode::file;
	}
	if (text == "memory") {
		return SourceMode::memory;
	}
	if (text == "memory_nocache") {
		return SourceMode::memory_nocache;
	}
	throw std::invalid_argument("unsupported source mode");
}

WorkloadMode parse_workload_mode(std::string_view text) {
	if (text == "parse_only") {
		return WorkloadMode::parse_only;
	}
	if (text == "read") {
		return WorkloadMode::read;
	}
	if (text == "read_decode") {
		return WorkloadMode::read_decode;
	}
	throw std::invalid_argument("unsupported workload mode");
}

std::vector<std::uint8_t> read_file_bytes(const fs::path& path, bool use_nocache) {
	const int fd = ::open(path.c_str(), O_RDONLY);
	if (fd < 0) {
		throw std::system_error(errno, std::generic_category(), "open failed");
	}

	if (use_nocache) {
#ifdef F_NOCACHE
		(void)::fcntl(fd, F_NOCACHE, 1);
#endif
	}

	struct stat st {};
	if (::fstat(fd, &st) != 0) {
		const int err = errno;
		(void)::close(fd);
		throw std::system_error(err, std::generic_category(), "fstat failed");
	}
	if (st.st_size < 0) {
		(void)::close(fd);
		throw std::runtime_error("negative file size");
	}

	std::vector<std::uint8_t> buffer(static_cast<std::size_t>(st.st_size));
	std::size_t offset = 0;
	while (offset < buffer.size()) {
		const ssize_t n = ::read(fd, buffer.data() + offset, buffer.size() - offset);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			const int err = errno;
			(void)::close(fd);
			throw std::system_error(err, std::generic_category(), "read failed");
		}
		if (n == 0) {
			break;
		}
		offset += static_cast<std::size_t>(n);
	}
	(void)::close(fd);
	if (offset != buffer.size()) {
		throw std::runtime_error("short read");
	}
	return buffer;
}

std::uint64_t touch_dataset(const dicom::DicomFile& file) {
	const auto& ds = file.dataset();
	std::uint64_t checksum = 0;
	checksum += static_cast<std::uint64_t>(ds["Rows"_tag].to_long(0));
	checksum += static_cast<std::uint64_t>(ds["Columns"_tag].to_long(0));
	checksum += static_cast<std::uint64_t>(ds["BitsAllocated"_tag].to_long(0));
	checksum += static_cast<std::uint64_t>(ds["SamplesPerPixel"_tag].to_long(0));
	checksum += static_cast<std::uint64_t>(ds["NumberOfFrames"_tag].to_long(1));
	checksum += static_cast<std::uint64_t>(ds.size());
	return checksum;
}

std::uint64_t touch_decoded_frame(const dicom::DicomFile& file) {
	const auto plan = file.create_decode_plan(dicom::pixel::DecodeOptions{});
	if (plan.strides.frame == 0) {
		throw std::runtime_error("decode plan frame stride is zero");
	}
	std::vector<std::uint8_t> decoded(plan.strides.frame);
	file.decode_into(0, std::span<std::uint8_t>(decoded), plan);

	std::uint64_t checksum = decoded.size();
	if (!decoded.empty()) {
		checksum += decoded.front();
		checksum += decoded[decoded.size() / 2];
		checksum += decoded.back();
		const std::size_t sample_count = std::min<std::size_t>(decoded.size(), 64);
		for (std::size_t i = 0; i < sample_count; ++i) {
			checksum = (checksum * 1315423911u) ^ decoded[i];
		}
	}
	return checksum;
}

std::unique_ptr<dicom::DicomFile> load_file_for_benchmark(
    const fs::path& path, SourceMode source, const std::vector<std::uint8_t>* preloaded_buffer) {
	switch (source) {
	case SourceMode::file:
		return dicom::read_file(path.string());
	case SourceMode::memory:
	{
		if (preloaded_buffer == nullptr) {
			throw std::invalid_argument("preloaded buffer is required for memory mode");
		}
		dicom::ReadOptions opts;
		opts.copy = false;
		return dicom::read_bytes(
		    path.string(), preloaded_buffer->data(), preloaded_buffer->size(), opts);
	}
	case SourceMode::memory_nocache:
		return dicom::read_bytes(path.string(), read_file_bytes(path, true));
	}
	throw std::runtime_error("unsupported source mode");
}

int main(int argc, char** argv) {
	const fs::path default_root{"/Users/tsangel/Documents/workspace.dev/sample/ncc/3121/pt"};
	const fs::path root = (argc >= 2) ? fs::path(argv[1]) : default_root;
	const int repeat = (argc >= 3) ? std::max(1, std::atoi(argv[2])) : 10;
	const SourceMode source = (argc >= 4) ? parse_source_mode(argv[3]) : SourceMode::file;
	const WorkloadMode workload =
	    (argc >= 5) ? parse_workload_mode(argv[4]) : WorkloadMode::read;

	std::cout << "impl=new (C++) source=" << source_mode_name(source)
	          << " workload=" << workload_mode_name(workload)
	          << " repeat=" << repeat << " root=" << root << "\n";

	if (!fs::exists(root)) {
		std::cerr << "root does not exist: " << root << "\n";
		return 1;
	}

	struct FileEntry {
		fs::path path;
		std::uintmax_t size;
		std::vector<std::uint8_t> buffer;  // empty when reading from file
	};

	std::vector<FileEntry> files;
	for (auto const& entry : fs::recursive_directory_iterator(root)) {
		if (!entry.is_regular_file()) continue;
		files.push_back(FileEntry{entry.path(), entry.file_size(), {}});
	}
	std::sort(files.begin(), files.end(), [](const FileEntry& lhs, const FileEntry& rhs) {
		return lhs.path.native() < rhs.path.native();
	});

	if (files.empty()) {
		std::cout << "no .dcm files found under: " << root << "\n";
		return 0;
	}

	if (source == SourceMode::memory) {
		for (auto& f : files) {
			std::ifstream is(f.path, std::ios::binary);
			f.buffer.resize(static_cast<std::size_t>(f.size));
			is.read(reinterpret_cast<char*>(f.buffer.data()), static_cast<std::streamsize>(f.buffer.size()));
		}
	}

	const std::size_t bytes_per_run = [&] {
		std::size_t sum = 0;
		for (auto const& f : files) sum += static_cast<std::size_t>(f.size);
		return sum;
	}();

	std::uint64_t checksum = 0;
	double total_seconds = 0.0;
	for (int r = 0; r < repeat; ++r) {
		const auto t0 = std::chrono::steady_clock::now();
		for (auto const& f : files) {
			auto file = load_file_for_benchmark(
			    f.path, source,
			    source == SourceMode::memory ? &f.buffer : nullptr);
			if (workload == WorkloadMode::parse_only) {
				checksum += static_cast<std::uint64_t>(file->size());
			} else {
				checksum += touch_dataset(*file);
			}
			if (workload == WorkloadMode::read_decode) {
				checksum += touch_decoded_frame(*file);
			}
		}
		const auto t1 = std::chrono::steady_clock::now();
		const double seconds = std::chrono::duration<double>(t1 - t0).count();
		total_seconds += seconds;

		const double avg_ms = (seconds / files.size()) * 1000.0;
		const double mbps = (bytes_per_run / (1024.0 * 1024.0)) / (seconds > 0 ? seconds : 1e-9);
		std::cout << "run " << (r + 1) << ": files=" << files.size()
		          << " bytes=" << bytes_per_run
		          << " time=" << std::fixed << std::setprecision(3) << seconds << "s "
		          << "avg=" << std::setprecision(2) << avg_ms << "ms/file "
		          << "throughput=" << std::setprecision(2) << mbps << " MiB/s\n";
	}

	const std::size_t total_files = files.size() * static_cast<std::size_t>(repeat);
	const std::size_t total_bytes = bytes_per_run * static_cast<std::size_t>(repeat);
	const double avg_ms = (total_seconds / total_files) * 1000.0;
	const double mbps = (total_bytes / (1024.0 * 1024.0)) / (total_seconds > 0 ? total_seconds : 1e-9);
	std::cout << "runs=" << repeat
	          << " total_files=" << total_files
	          << " total_bytes=" << total_bytes
	          << " checksum=" << checksum
	          << " time=" << std::fixed << std::setprecision(3) << total_seconds << "s "
	          << "avg=" << std::setprecision(2) << avg_ms << "ms/file "
	          << "throughput=" << std::setprecision(2) << mbps << " MiB/s\n";

	return 0;
}
