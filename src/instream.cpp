#include "instream.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <vector>
#include <diagnostics.h>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace dicom {

namespace {

#if defined(_WIN32)
std::size_t file_size_from_handle(void* handle) {
	LARGE_INTEGER size;
	if (!GetFileSizeEx(reinterpret_cast<HANDLE>(handle), &size)) {
		throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "GetFileSizeEx failed");
	}
	if (size.QuadPart < 0 || static_cast<unsigned long long>(size.QuadPart) > std::numeric_limits<std::size_t>::max()) {
		throw std::runtime_error("File too large to map");
	}
	return static_cast<std::size_t>(size.QuadPart);
}
#else
std::size_t file_size_from_descriptor(int fd) {
	struct stat st;
	if (::fstat(fd, &st) != 0) {
		throw std::system_error(errno, std::generic_category(), "fstat failed");
	}
	if (st.st_size < 0 || static_cast<unsigned long long>(st.st_size) > std::numeric_limits<std::size_t>::max()) {
		throw std::runtime_error("File too large to map");
	}
	return static_cast<std::size_t>(st.st_size);
}
#endif

}  // namespace

InStream::~InStream() {
	reset_internal_buffer();
}

void InStream::release_storage() {}

void InStream::reset_internal_buffer() {
	if (own_data_) {
		release_storage();
	}
	rootstream_ = this;
	startoffset_ = offset_ = endoffset_ = 0;
	data_ = nullptr;
	filesize_ = 0;
	own_data_ = false;
}

std::span<const std::uint8_t> InStream::read(std::size_t size) {
	auto view = peek(size);
	if (!view.empty()) {
		offset_ += size;
	}
	return view;
}

std::span<const std::uint8_t> InStream::peek(std::size_t size) const {
	if (size > bytes_remaining()) {
		return std::span<const std::uint8_t>();
	}
#ifndef NDEBUG
	if (!data_) {
		throw std::logic_error("InStream has no backing data");
	}
#endif
	return std::span<const std::uint8_t>(data_ + offset_, size);	
}

std::size_t InStream::read_into(void* dest, std::size_t size) {
	if (size > bytes_remaining()) {
		return 0;
	}
#ifndef NDEBUG
	if (!data_) {
		throw std::logic_error("InStream has no backing data");
	}
#endif
	std::memcpy(dest, data_ + offset_, size);
	offset_ += size;
	return size;
}

std::size_t InStream::skip(std::size_t size) {
	if (size <= bytes_remaining()) {
		offset_ += size;
		return size;
	}
	return 0;
}

void* InStream::get_pointer(std::size_t offset, std::size_t size) {
	if (offset + size > endoffset_) {
		throw std::out_of_range("InStream::get_pointer out of range");
	}
	if (!data_) {
		throw std::logic_error("InStream has no backing data");
	}
	return data_ + offset;
}

std::span<const std::uint8_t> InStream::get_span(std::size_t offset, std::size_t size) const {
	if (offset + size > endoffset_) {
		throw std::out_of_range("InStream::get_span out of range");
	}
	if (!data_) {
		throw std::logic_error("InStream has no backing data");
	}
	return std::span<const std::uint8_t>(data_ + offset, size);
}

std::size_t InStream::seek(std::size_t pos) {
	if (pos < startoffset_ || pos > endoffset_) {
		throw std::out_of_range("InStream::seek out of range");
	}
	offset_ = pos;
	return offset_;
}

void InStream::unread(std::size_t size) {
	if (offset_ >= size + startoffset_) {
		offset_ -= size;
	} else {
		offset_ = startoffset_;
	}
}

void InStream::rewind() {
	offset_ = startoffset_;
}

InStringStream::InStringStream() = default;

InStringStream::~InStringStream() {
	reset_internal_buffer();
}

void InStringStream::release_storage() {
	owned_buffer_.clear();
}

void InStringStream::attach_memory(std::vector<std::uint8_t>&& buffer) {
	reset_internal_buffer();
	owned_buffer_ = std::move(buffer);
	data_ = owned_buffer_.empty() ? nullptr : owned_buffer_.data();
	startoffset_ = 0;
	offset_ = 0;
	endoffset_ = owned_buffer_.size();
	filesize_ = owned_buffer_.size();
	rootstream_ = this;
	own_data_ = true;
}

void InStringStream::attach_memory(const std::uint8_t* data, std::size_t datasize, bool copydata) {
	if (datasize > 0 && data == nullptr) {
		throw std::invalid_argument("Data pointer cannot be null when datasize > 0");
	}
	if (copydata) {
		std::vector<std::uint8_t> buffer(datasize);
		if (datasize > 0) {
			std::memcpy(buffer.data(), data, datasize);
		}
		attach_memory(std::move(buffer));
		return;
	}
	reset_internal_buffer();
	data_ = const_cast<std::uint8_t*>(data);
	startoffset_ = 0;
	offset_ = 0;
	endoffset_ = datasize;
	filesize_ = datasize;
	rootstream_ = this;
	own_data_ = false;
}

InFileStream::InFileStream()
#if defined(_WIN32)
	: file_handle_(INVALID_HANDLE_VALUE), mapping_handle_(nullptr), mapped_view_(nullptr) {}
#else
	: fd_(-1), mapped_view_(nullptr) {}
#endif

InFileStream::~InFileStream() {
	reset_internal_buffer();
}

void InFileStream::release_storage() {
#if defined(_WIN32)
	if (mapped_view_) {
		UnmapViewOfFile(mapped_view_);
		mapped_view_ = nullptr;
	}
	if (mapping_handle_) {
		CloseHandle(reinterpret_cast<HANDLE>(mapping_handle_));
		mapping_handle_ = nullptr;
	}
	if (file_handle_ && file_handle_ != INVALID_HANDLE_VALUE) {
		CloseHandle(reinterpret_cast<HANDLE>(file_handle_));
		file_handle_ = INVALID_HANDLE_VALUE;
	}
#else
	if (mapped_view_ && filesize_ > 0) {
		munmap(mapped_view_, filesize_);
	}
	if (fd_ >= 0) {
		close(fd_);
		fd_ = -1;
	}
	mapped_view_ = nullptr;
#endif
}

void InFileStream::attach_file(const std::string& filename) {
	detach_file();
	if (filename.empty()) {
		throw std::invalid_argument("Filename cannot be empty");
	}
#if defined(_WIN32)
	file_handle_ = CreateFileA(filename.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file_handle_ == INVALID_HANDLE_VALUE) {
		throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Failed to open file");
	}
	const std::size_t filesize = file_size_from_handle(file_handle_);
	own_data_ = true;
	if (filesize > 0) {
		mapping_handle_ = CreateFileMappingA(reinterpret_cast<HANDLE>(file_handle_), nullptr, PAGE_READONLY, 0, 0, nullptr);
		if (!mapping_handle_) {
			const int err = static_cast<int>(GetLastError());
			CloseHandle(reinterpret_cast<HANDLE>(file_handle_));
			file_handle_ = INVALID_HANDLE_VALUE;
			throw std::system_error(err, std::system_category(), "CreateFileMapping failed");
		}
		mapped_view_ = MapViewOfFile(reinterpret_cast<HANDLE>(mapping_handle_), FILE_MAP_READ, 0, 0, 0);
		if (!mapped_view_) {
			const int err = static_cast<int>(GetLastError());
			CloseHandle(reinterpret_cast<HANDLE>(mapping_handle_));
			mapping_handle_ = nullptr;
			CloseHandle(reinterpret_cast<HANDLE>(file_handle_));
			file_handle_ = INVALID_HANDLE_VALUE;
			throw std::system_error(err, std::system_category(), "MapViewOfFile failed");
		}
		data_ = static_cast<std::uint8_t*>(mapped_view_);
		own_data_ = true;
	} else {
		data_ = nullptr;
	}
	filesize_ = filesize;
	startoffset_ = 0;
	offset_ = 0;
	endoffset_ = filesize;
	rootstream_ = this;
	filename_ = filename;
#else
	fd_ = ::open(filename.c_str(), O_RDONLY);
	if (fd_ < 0) {
		throw std::system_error(errno, std::generic_category(), "Failed to open file");
	}
	const std::size_t filesize = file_size_from_descriptor(fd_);
	void* mapped = nullptr;
	if (filesize > 0) {
		mapped = mmap(nullptr, filesize, PROT_READ, MAP_PRIVATE, fd_, 0);
		if (mapped == MAP_FAILED) {
			const int err = errno;
			close(fd_);
			fd_ = -1;
			throw std::system_error(err, std::generic_category(), "mmap failed");
		}
	}
	if (fd_ >= 0) {
		close(fd_);
		fd_ = -1;
	}
	mapped_view_ = mapped == MAP_FAILED ? nullptr : mapped;
	data_ = static_cast<std::uint8_t*>(mapped_view_);
	filesize_ = filesize;
	startoffset_ = 0;
	offset_ = 0;
	endoffset_ = filesize;
	rootstream_ = this;
	filename_ = filename;
	own_data_ = filesize > 0;
#endif
}

void InFileStream::detach_file() {
	reset_internal_buffer();
	filename_.clear();
}

InSubStream::InSubStream(InStream* basestream, std::size_t size) {
	if (!basestream) {
		diag::error_and_throw("InSubStream::InSubStream reason=null basestream");
	}
	rootstream_ = basestream->rootstream();
	startoffset_ = basestream->tell();
	endoffset_ = startoffset_ + size;
	if (endoffset_ > basestream->endoffset())
		endoffset_ = basestream->endoffset();
	offset_ = startoffset_;
	filesize_ = endoffset_ - startoffset_;
	std::uint8_t* root_data = rootstream_ ? rootstream_->data() : nullptr;
	if (!root_data) {
		diag::error_and_throw("InSubStream::InSubStream reason=basestream missing backing data");
	}
	data_ = root_data;
	own_data_ = false;
}

}  // namespace dicom
