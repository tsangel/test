#include <dicom.h>
#include <instream.h>

#include <stdexcept>
#include <vector>

namespace dicom {

namespace {

std::unique_ptr<InFileStream> make_file_stream(const std::string& path) {
	auto stream = std::make_unique<InFileStream>();
	stream->attach_file(path);
	return stream;
}

std::unique_ptr<InStringStream> make_memory_stream(const std::uint8_t* data, std::size_t size, bool copy) {
	auto stream = std::make_unique<InStringStream>();
	stream->attach_memory(data, size, copy);
	return stream;
}

std::unique_ptr<InStringStream> make_memory_stream(std::vector<std::uint8_t>&& buffer) {
	auto stream = std::make_unique<InStringStream>();
	stream->attach_memory(std::move(buffer));
	return stream;
}

}  // namespace

DataSet::DataSet() = default;

DataSet::~DataSet() = default;

void DataSet::attach_to_file(const std::string& path) {
	auto stream = make_file_stream(path);
	reset_stream(path, std::move(stream), Backing::File);
}

void DataSet::attach_to_memory(const std::uint8_t* data, std::size_t size, bool copy) {
	auto stream = make_memory_stream(data, size, copy);
	reset_stream(std::string{"<memory>"}, std::move(stream), Backing::Memory);
}

void DataSet::attach_to_memory(const std::string& name, const std::uint8_t* data, std::size_t size, bool copy) {
	auto stream = make_memory_stream(data, size, copy);
	reset_stream(name, std::move(stream), Backing::Memory);
}

void DataSet::attach_to_memory(std::string name, std::vector<std::uint8_t>&& buffer) {
	auto stream = make_memory_stream(std::move(buffer));
	reset_stream(std::move(name), std::move(stream), Backing::Memory);
}

void DataSet::reset_stream(std::string identifier, std::unique_ptr<InStream> stream, Backing backing) {
	if (!stream) {
		throw std::runtime_error("DataSet requires a valid stream");
	}
	path_ = std::move(identifier);
	stream_ = std::move(stream);
	backing_ = backing;
}

const std::string& DataSet::path() const {
	return path_;
}

InStream& DataSet::stream() {
	return *stream_;
}

const InStream& DataSet::stream() const {
	return *stream_;
}

bool DataSet::is_memory_backed() const noexcept {
	return backing_ == Backing::Memory;
}

DataElement* DataSet::add_dataelement(Tag tag, VR vr, std::size_t length, std::size_t offset) {
	auto element = std::make_unique<DataElement>(tag, vr, length, offset, this);
	auto* raw = element.get();
	elements_.emplace_back(tag, std::move(element));
	return raw;
}

std::unique_ptr<DataSet> read_file(const std::string& path) {
	auto data_set = std::make_unique<DataSet>();
	data_set->attach_to_file(path);
	return data_set;
}

std::unique_ptr<DataSet> read_bytes(const std::uint8_t* data, std::size_t size, bool copy) {
	return read_bytes(std::string{"<memory>"}, data, size, copy);
}

std::unique_ptr<DataSet> read_bytes(const std::string& name, const std::uint8_t* data,
	    std::size_t size, bool copy) {
	auto data_set = std::make_unique<DataSet>();
	data_set->attach_to_memory(name, data, size, copy);
	return data_set;
}

std::unique_ptr<DataSet> read_bytes(std::string name, std::vector<std::uint8_t>&& buffer) {
	// Use this overload when the caller already owns the bytes in a std::vector and wants to
	// transfer ownership to dicomsdl without an extra copy.
	auto data_set = std::make_unique<DataSet>();
	data_set->attach_to_memory(std::move(name), std::move(buffer));
	return data_set;
}

} // namespace dicom
