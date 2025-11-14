#include <dicom.h>
#include <instream.h>

#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <span>

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

std::span<const std::uint8_t> DataElement::value_span() const {
	if (storage_.ptr) {
		return std::span<const std::uint8_t>(
		    static_cast<const std::uint8_t*>(storage_.ptr), length_);
	}
	if (!parent_) {
		throw std::logic_error("DataElement is not attached to a DataSet");
	}
	return parent_->stream().get_span(offset_, length_);
}

void* DataElement::value_ptr() const {
	if (storage_.ptr) {
		return storage_.ptr;
	}
	if (!parent_) {
		throw std::logic_error("DataElement is not attached to a DataSet");
	}
	return parent_->stream().get_pointer(offset_, length_);
}

int DataElement::vm() const {
	// PS 3.5, 6.4 VALUE MULTIPLICITY (VM) AND DELIMITATION
	if (length_ == 0) {
		return 0;
	}

	const auto vr_value = static_cast<std::uint16_t>(vr_);
	switch (vr_value) {
	case VR::FD_val:
	case VR::SV_val:
	case VR::UV_val:
		return static_cast<int>(length_ / 8);
	case VR::AT_val:
	case VR::FL_val:
	case VR::UL_val:
	case VR::SL_val:
		return static_cast<int>(length_ / 4);
	case VR::US_val:
	case VR::SS_val:
		return static_cast<int>(length_ / 2);
	case VR::AE_val: case VR::AS_val: case VR::CS_val: case VR::DA_val:
	case VR::DS_val: case VR::DT_val: case VR::IS_val: case VR::LO_val:
	case VR::PN_val: case VR::SH_val: case VR::TM_val: case VR::UC_val:
	case VR::UI_val: {
		std::span<const std::uint8_t> data;
		data = value_span();
		if (data.empty()) {
			return 0;
		}
		int delims = 0;
		for (auto byte : data) {
			if (byte == '\\') {
				++delims;
			}
		}
		return delims + 1;
	}
	// LT, OB, OD, OF, OL, OW, SQ, ST, UN, UR or UT -> always 1
	default:
		return 1;
	}
}

DataSet::DataSet() = default;

DataSet::DataSet(DataSet* root_dataset) : root_dataset_(root_dataset ? root_dataset : this) {}

DataSet::~DataSet() = default;

void DataSet::attach_to_file(const std::string& path) {
	auto stream = make_file_stream(path);
	reset_stream(path, std::move(stream));
}

void DataSet::attach_to_memory(const std::uint8_t* data, std::size_t size, bool copy) {
	auto stream = make_memory_stream(data, size, copy);
	reset_stream(std::string{"<memory>"}, std::move(stream));
}

void DataSet::attach_to_memory(const std::string& name, const std::uint8_t* data, std::size_t size, bool copy) {
	auto stream = make_memory_stream(data, size, copy);
	reset_stream(name, std::move(stream));
}

void DataSet::attach_to_memory(std::string name, std::vector<std::uint8_t>&& buffer) {
	auto stream = make_memory_stream(std::move(buffer));
	reset_stream(std::move(name), std::move(stream));
}

void DataSet::reset_stream(std::string identifier, std::unique_ptr<InStream> stream) {
	if (!stream) {
		throw std::runtime_error("DataSet requires a valid stream");
	}
	path_ = std::move(identifier);
	stream_ = std::move(stream);
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
	if (vr == VR::NONE) {
		const auto vr_value = lookup::tag_to_vr(tag_value);
		if (vr_value == 0) {
			throw std::invalid_argument("VR is required for unknown tag");
		}
		vr = VR(vr_value);
	}

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
		if (it->vr() != VR::NONE)
			return &(*it);
		else
			return NullElement();
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
		if (it->vr() != VR::NONE)
			return &(*it);
		else
			return NullElement();
	}

	if (auto map_it = element_map_.find(tag_value); map_it != element_map_.end()) {
		return &map_it->second;
	}
	return NullElement();
}

void DataSet::remove_dataelement(Tag tag) {
	const auto tag_value = tag.value();

	auto vec_it = std::lower_bound(elements_.begin(), elements_.end(), tag_value,
	    [](const DataElement& element, std::uint32_t value) {
		return element.tag().value() < value;
	});
	if (vec_it != elements_.end() && vec_it->tag().value() == tag_value) {
		// just set VR::NONE instead remove from elements_
		*vec_it = DataElement(tag, VR::NONE, 0, 0, this);
	} else
	if (auto map_it = element_map_.find(tag_value); map_it != element_map_.end()) {
		element_map_.erase(map_it);
	}
}

void DataSet::read_attached_stream(const ReadOptions& options) {
	if (this != root_dataset_) {
		throw std::logic_error("read_attached_stream is only valid on the root DataSet");
	}
	if (!stream_ || !stream_->is_valid()) {
		throw std::logic_error("DataSet has no valid attached stream");
	}

	elements_.clear();
	element_map_.clear();
	last_tag_loaded_ = Tag::from_value(0);

	// parse DICOM stream, starting with skipping the 128-byte preamble.
	stream_->rewind();
	stream_->skip(128);
	auto magic = stream_->peek(4);
	if (magic.size() == 4 && std::memcmp(magic.data(), "DICM", 4) == 0) {
		stream_->skip(4);
	} else {
		// Some files ship without the PART 10 preamble/magic; rewind to treat them as raw streams.
		stream_->rewind();
	}

	read_elements_until(options.load_until, stream_.get());
}

void DataSet::read_elements_until(Tag load_until, InStream* stream) {
	(void)load_until;
	(void)stream;
}

void DataSet::dump_elements() const {
	std::cout << "-- elements_ --\n";
	for (const auto& element : elements_) {
		std::cout << element.tag().value() << " VR=" << element.vr().str() << " len=" << element.length() << " off=" << element.offset() << "\n";
	}
	std::cout << "-- element_map_ --\n";
	for (const auto& kv : element_map_) {
		std::cout << kv.first << " VR=" << kv.second.vr().str() << " len=" << kv.second.length() << " off=" << kv.second.offset() << "\n";
	}
}

std::unique_ptr<DataSet> read_file(const std::string& path, ReadOptions options) {
	auto data_set = std::make_unique<DataSet>();
	data_set->attach_to_file(path);
	data_set->read_attached_stream(options);
	return data_set;
}

std::unique_ptr<DataSet> read_bytes(const std::uint8_t* data, std::size_t size,
    ReadOptions options) {
	return read_bytes(std::string{"<memory>"}, data, size, options);
}

std::unique_ptr<DataSet> read_bytes(const std::string& name, const std::uint8_t* data,
    std::size_t size, ReadOptions options) {
	auto data_set = std::make_unique<DataSet>();
	data_set->attach_to_memory(name, data, size, options.copy);
	data_set->read_attached_stream(options);
	return data_set;
}

std::unique_ptr<DataSet> read_bytes(std::string name, std::vector<std::uint8_t>&& buffer,
    ReadOptions options) {
	auto data_set = std::make_unique<DataSet>();
	data_set->attach_to_memory(std::move(name), std::move(buffer));
	data_set->read_attached_stream(options);
	return data_set;
}

} // namespace dicom
