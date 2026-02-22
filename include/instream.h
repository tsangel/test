#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace dicom {


class InStream {
protected:
	// Absolute offsets into the root stream’s backing buffer/file.
	// startoffset_ marks the first byte of the attached region, offset_ is the
	// current cursor within [startoffset_, endoffset_), and endoffset_ is the
	// one-past-last byte. Substreams keep these absolute positions as well.
	std::size_t startoffset_{0};
	std::size_t offset_{0};
	std::size_t endoffset_{0};

	std::uint8_t* data_{nullptr};
	bool own_data_{false};
	std::size_t filesize_{0};
	std::string identifier_{"<unattached>"};

	InStream* root_stream_{this};

	virtual void release_storage();
	void reset_internal_buffer();

	InStream() = default;

public:
	virtual ~InStream();

	InStream(const InStream&) = delete;
	InStream& operator=(const InStream&) = delete;
	InStream(InStream&&) = delete;
	InStream& operator=(InStream&&) = delete;

	[[nodiscard]] inline bool is_valid() const { return data_ != nullptr; }
	[[nodiscard]] inline std::size_t tell() const { return offset_; }
	[[nodiscard]] inline bool is_eof() const { return offset_ == endoffset_; }
	[[nodiscard]] inline std::size_t bytes_remaining() const { return endoffset_ - offset_; }
	[[nodiscard]] inline std::size_t begin() const { return startoffset_; }
	[[nodiscard]] inline std::size_t end() const { return endoffset_; }
	[[nodiscard]] inline const std::string& identifier() const { return identifier_; }
	inline void set_identifier(std::string id) { identifier_ = std::move(id); }
	std::span<const std::uint8_t> read(std::size_t size);
	std::span<const std::uint8_t> peek(std::size_t size) const;
	std::size_t read_into(void* dest, std::size_t size);
	int read_4bytes(std::array<std::uint8_t, 4>& dest);
	int read_8bytes(std::array<std::uint8_t, 8>& dest);
	int peek_8bytes(std::array<std::uint8_t, 8>& dest) const;
	void skip_to_end() { offset_ = endoffset_; }
	std::size_t skip(std::size_t size);
	void* get_pointer(std::size_t offset, std::size_t size);
	std::span<const std::uint8_t> get_span(std::size_t offset, std::size_t size) const;
	std::size_t seek(std::size_t pos);
	void unread(std::size_t size);
	void rewind();

	[[nodiscard]] inline InStream* root_stream() const { return root_stream_; }
	[[nodiscard]] inline std::uint8_t* data() const { return data_; }
	[[nodiscard]] inline std::size_t attached_size() const { return endoffset_ - startoffset_; }
	[[nodiscard]] inline std::size_t end_offset() const { return endoffset_; }

};

class InStringStream : public InStream {
public:
	InStringStream();
	~InStringStream() override;

	void attach_memory(const std::uint8_t* data, std::size_t data_size, bool copy_data);
	void attach_memory(std::vector<std::uint8_t>&& buffer);

protected:
	void release_storage() override;

private:
	std::vector<std::uint8_t> owned_buffer_;
};

class InFileStream : public InStream {
public:
	InFileStream();
	~InFileStream() override;

	void attach_file(const std::string& file_path);
	void detach_file();

protected:
	void release_storage() override;

private:
	std::string filename_;

#if defined(_WIN32)
	void* file_handle_;
	void* mapping_handle_;
#else
	int fd_;
#endif
	void* mapped_view_;
};

class InSubStream : public InStream {
public:
	InSubStream(InStream* base_stream, std::size_t size);
	~InSubStream() override = default;

protected:
	void release_storage() override {}
};

inline int InStream::read_4bytes(std::array<std::uint8_t, 4>& dest) {
	constexpr std::size_t kSize = 4;
	if (bytes_remaining() < kSize) {
		return 0;
	}
#ifndef NDEBUG
	if (!data_) {
		throw std::logic_error("InStream has no backing data");
	}
#endif
	std::memcpy(dest.data(), data_ + offset_, kSize);
	offset_ += kSize;
	return static_cast<int>(kSize);
}

inline int InStream::read_8bytes(std::array<std::uint8_t, 8>& dest) {
	constexpr std::size_t kSize = 8;
	if (bytes_remaining() < kSize) {
		return 0;
	}
#ifndef NDEBUG
	if (!data_) {
		throw std::logic_error("InStream has no backing data");
	}
#endif
	std::memcpy(dest.data(), data_ + offset_, kSize);
	offset_ += kSize;
	return static_cast<int>(kSize);
}

inline int InStream::peek_8bytes(std::array<std::uint8_t, 8>& dest) const {
	constexpr std::size_t kSize = 8;
	if (bytes_remaining() < kSize) {
		return 0;
	}
#ifndef NDEBUG
	if (!data_) {
		throw std::logic_error("InStream has no backing data");
	}
#endif
	std::memcpy(dest.data(), data_ + offset_, kSize);
	return static_cast<int>(kSize);
}

}  // namespace dicom
