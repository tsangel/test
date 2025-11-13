#include <dicom.h>
#include <instream.h>

#include <algorithm>
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

DataElement* NullElement() {
	static DataElement null(Tag(0x0000, 0x0000), VR::NONE, 0, 0, nullptr);
	return &null;
}

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

DataSet::iterator DataSet::begin() {
	return iterator(elements_.begin(), elements_.end(), element_map_.begin(), element_map_.end());
}

DataSet::iterator DataSet::end() {
	return iterator(elements_.end(), elements_.end(), element_map_.end(), element_map_.end());
}

DataSet::const_iterator DataSet::begin() const {
	return cbegin();
}

DataSet::const_iterator DataSet::end() const {
	return cend();
}

DataSet::const_iterator DataSet::cbegin() const {
	return const_iterator(elements_.cbegin(), elements_.cend(),
	    element_map_.cbegin(), element_map_.cend());
}

DataSet::const_iterator DataSet::cend() const {
	return const_iterator(elements_.cend(), elements_.cend(),
	    element_map_.cend(), element_map_.cend());
}

// Keeps elements_ strictly sorted for common sequential inserts, while element_map_
// stores out-of-order tags so we never duplicate storage for the same tag.
DataElement* DataSet::add_dataelement(Tag tag, VR vr, std::size_t length, std::size_t offset) {
	const auto tag_value = tag.value();

	if (elements_.empty() || elements_.back().tag().value() < tag_value) {
		elements_.emplace_back(tag, vr, length, offset, this);
		return &elements_.back();
	}

	const auto find_in_elements = [&](std::uint32_t value) -> DataElement* {
		auto it = std::lower_bound(elements_.begin(), elements_.end(), value,
		    [](const DataElement& element, std::uint32_t v) {
			return element.tag().value() < v;
		});
		if (it != elements_.end() && it->tag().value() == value) {
			return &(*it);
		}
		return nullptr;
	};

	if (auto* elem = find_in_elements(tag_value)) {
		*elem = DataElement(tag, vr, length, offset, this);
		return elem;
	}

	auto map_it = element_map_.lower_bound(tag_value);
	if (map_it != element_map_.end() && map_it->first == tag_value) {
		map_it->second = DataElement(tag, vr, length, offset, this);
		return &map_it->second;
	}

	auto insert_it = element_map_.emplace_hint(
	    map_it, tag_value, DataElement(tag, vr, length, offset, this));
	return &insert_it->second;
}

DataElement* DataSet::get_dataelement(Tag tag) {
	const auto tag_value = tag.value();

	auto it = std::lower_bound(elements_.begin(), elements_.end(), tag_value,
	    [](const DataElement& element, std::uint32_t value) {
		return element.tag().value() < value;
	});
	if (it != elements_.end() && it->tag().value() == tag_value) {
		return &(*it);
	}

	if (auto map_it = element_map_.find(tag_value); map_it != element_map_.end()) {
		return &map_it->second;
	}
	return NullElement();
}

const DataElement* DataSet::get_dataelement(Tag tag) const {
	const auto tag_value = tag.value();

	auto it = std::lower_bound(elements_.begin(), elements_.end(), tag_value,
	    [](const DataElement& element, std::uint32_t value) {
		return element.tag().value() < value;
	});
	if (it != elements_.end() && it->tag().value() == tag_value) {
		return &(*it);
	}

	if (auto map_it = element_map_.find(tag_value); map_it != element_map_.end()) {
		return &map_it->second;
	}
	return NullElement();
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
