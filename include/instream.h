#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dicom {

class InStream {
protected:
	std::size_t startoffset_{0};
	std::size_t offset_{0};
	std::size_t endoffset_{0};

	std::uint8_t* data_{nullptr};
	bool own_data_{false};
	std::size_t filesize_{0};
	std::size_t loaded_bytes_{0};

	InStream* basestream_{this};
	InStream* rootstream_{this};

	virtual void release_storage();
	void reset_internal_buffer();

	InStream() = default;
	InStream(InStream* basestream, std::size_t size);

public:
	virtual ~InStream();

	InStream(const InStream&) = delete;
	InStream& operator=(const InStream&) = delete;
	InStream(InStream&&) = delete;
	InStream& operator=(InStream&&) = delete;

	[[nodiscard]] inline bool is_valid() const { return data_ != nullptr || filesize_ == 0; }
	[[nodiscard]] inline std::size_t tell() const { return offset_; }
	[[nodiscard]] inline bool is_eof() const { return offset_ == endoffset_; }
	[[nodiscard]] inline std::size_t bytes_remaining() const { return endoffset_ - offset_; }
	[[nodiscard]] inline std::size_t begin() const { return startoffset_; }
	[[nodiscard]] inline std::size_t end() const { return endoffset_; }
	[[nodiscard]] inline std::size_t loaded_bytes() const { return rootstream_->loaded_bytes_; }

	std::size_t read(std::uint8_t* ptr, std::size_t size);
	std::size_t skip(std::size_t size);
	void* get_pointer(std::size_t offset, std::size_t size);
	std::size_t seek(std::size_t pos);
	void unread(std::size_t size);
	void rewind();

	[[nodiscard]] inline InStream* basestream() const { return basestream_; }
	[[nodiscard]] inline InStream* rootstream() const { return rootstream_; }
	[[nodiscard]] inline std::uint8_t* data() const { return rootstream_->data_; }
	[[nodiscard]] inline std::size_t datasize() const { return endoffset_ - startoffset_; }
	[[nodiscard]] inline std::size_t endoffset() const { return endoffset_; }

};

class InStringStream : public InStream {
public:
	InStringStream();
	~InStringStream() override;

	void attachmemory(const std::uint8_t* data, std::size_t datasize, bool copydata);
	void attachmemory(std::vector<std::uint8_t>&& buffer);

protected:
	void release_storage() override;

private:
	std::vector<std::uint8_t> owned_buffer_;
};

class InFileStream : public InStream {
public:
	InFileStream();
	~InFileStream() override;

	void attachfile(const std::string& filename);
	void detachfile();

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
	InSubStream(InStream* basestream, std::size_t size);
	~InSubStream() override = default;

protected:
	void release_storage() override {}
};

}  // namespace dicom
