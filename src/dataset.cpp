#include <dicom.h>
#include <dicom_endian.h>
#include <diagnostics.h>
#include <instream.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <format>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <span>

using namespace dicom::literals;
namespace diag = dicom::diag;

namespace dicom {

namespace {

inline std::string tag_to_string(Tag tag) {
	return std::format("({:04X},{:04X})", tag.group(), tag.element());
}

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
	static DataElement null(Tag(0x0000, 0x0000), VR::None, 0, 0, nullptr);
	return &null;
}

std::span<const std::uint8_t> DataElement::value_span() const {
	if (storage_.ptr) {
		return std::span<const std::uint8_t>(
		    static_cast<const std::uint8_t*>(storage_.ptr), length_);
	}
	if (!parent_) {
		diag::error_and_throw(
		    "DataElement::value_span offset=0x{:X} tag={} reason=not attached to a DataSet",
		    offset_, tag_to_string(tag_));
	}
	return parent_->stream().get_span(offset_, length_);
}

void* DataElement::value_ptr() const {
	if (storage_.ptr) {
		return storage_.ptr;
	}
	if (!parent_) {
		diag::error_and_throw(
		    "DataElement::value_ptr offset=0x{:X} tag={} reason=not attached to a DataSet",
		    offset_, tag_to_string(tag_));
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
	attach_to_stream(path, std::move(stream));
}

void DataSet::attach_to_memory(const std::uint8_t* data, std::size_t size, bool copy) {
	auto stream = make_memory_stream(data, size, copy);
	attach_to_stream(std::string{"<memory>"}, std::move(stream));
}

void DataSet::attach_to_memory(const std::string& name, const std::uint8_t* data, std::size_t size, bool copy) {
	auto stream = make_memory_stream(data, size, copy);
	attach_to_stream(name, std::move(stream));
}

void DataSet::attach_to_memory(std::string name, std::vector<std::uint8_t>&& buffer) {
	auto stream = make_memory_stream(std::move(buffer));
	attach_to_stream(std::move(name), std::move(stream));
}

void DataSet::attach_to_stream(std::string identifier, std::unique_ptr<InStream> stream) {
	if (!stream) {
		diag::error_and_throw("DataSet::attach_to_stream file={} reason=null stream",
		    identifier);
	}
	if (this != root_dataset_) {
		//ERRORTHROW only root dataset can call attach to file or memory
	}
	stream->set_identifier(std::move(identifier));
	stream_ = std::move(stream);
}

const std::string& DataSet::path() const {
	static const std::string kEmpty;
	return stream_ ? stream_->identifier() : kEmpty;
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
	if (vr == VR::None) {
		const auto vr_value = lookup::tag_to_vr(tag_value);
		if (vr_value == 0) {
			diag::error_and_throw(
			    "DataSet::add_dataelement file={} tag={} reason=VR required for unknown tag",
			    root_dataset_->path(), tag_to_string(tag));
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
		if (it->vr() != VR::None)
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
		if (it->vr() != VR::None)
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
		// just set VR::None instead remove from elements_
		*vec_it = DataElement(tag, VR::None, 0, 0, this);
	} else
	if (auto map_it = element_map_.find(tag_value); map_it != element_map_.end()) {
		element_map_.erase(map_it);
	}
}

void DataSet::read_attached_stream(const ReadOptions& options) {
	if (this != root_dataset_) {
		diag::error_and_throw("DataSet::read_attached_stream reason=called on non-root dataset");
	}
	if (!stream_ || !stream_->is_valid()) {
		diag::error_and_throw("DataSet::read_attached_stream file={} reason=no valid attached stream",
		    path());
	}

	elements_.clear();
	element_map_.clear();
	last_tag_loaded_ = Tag::from_value(0);

	// parse DICOM stream, starting with skipping the 128-byte preamble.
	stream_->rewind();
	stream_->skip(128);

	// check magic number "DICM"
	std::array<std::uint8_t, 4> magic{};
	if (stream_->read_4bytes(magic) != 4 || std::memcmp(magic.data(), "DICM", 4) != 0) {
		// Some files ship without the PART 10 preamble/magic; rewind to treat them as raw streams.
		stream_->rewind();
	}

	read_elements_until(options.load_until, stream_.get());
}

void DataSet::read_elements_until(Tag load_until, InStream* stream) {
	std::array<std::uint8_t, 8> buf8{};
	std::array<std::uint8_t, 4> buf4{};

	if (!stream) {
		// stream is nullptr when second call from read_attached_stream() or calls
		// Sequence::load()
		stream = stream_.get();
		if (!stream) {
			// a DataSet is created from scratch without attaching stream.
			diag::error_and_throw("DataSet::read_elements_until file={} reason=no valid attached stream",
			    path());
		}
	}

	const bool little_endian = is_little_endian();
	const bool explicit_vr = is_explicit_vr();

	while (!stream->is_eof()) {
		if (stream->read_8bytes(buf8) != 8) {
			// Some files have short tailing bytes (possibly zero?).
        	// Let just consume bytes.
			stream->skip_to_end();
			break;
		}

		if (std::bit_cast<std::uint64_t>(buf8) == 0) {
			// tag - zero, VR - zero, length - zero...
        	// may be trailing zeros?
			stream->skip_to_end();
			break;
		}

		const std::uint16_t gggg = endian::load_value<std::uint16_t>(buf8.data(), little_endian);
		const std::uint16_t eeee = endian::load_value<std::uint16_t>(buf8.data() + 2, little_endian);

		const Tag tag{gggg, eeee};

		// PS3.5 Table 7.5-3, Item Delim. Tag
		if (gggg == 0xfffe) {
			const auto item_offset = stream->tell() - 8;
			if (tag == "(fffe,e00d)"_tag) {
				if (this != root_dataset_) {
					break;
				} else {
					diag::error(
					    "DataSet::read_elements_until file={} offset=0x{:X} tag={} reason=item delim encountered out of sequence",
					    path(), item_offset, tag_to_string(tag));
					continue;
				}
			} else if (tag == "fffe,e0dd"_tag) {
				diag::error(
				    "DataSet::read_elements_until file={} offset=0x{:X} tag={} reason=sequence delim encountered while parsing root dataset",
				    path(), item_offset, tag_to_string(tag));

				if (this != root_dataset_)
					break;
        		else
          			continue;
			}
		}

		if (this == root_dataset_ && tag > load_until)
			break;

			if (last_tag_loaded_ > tag && this != root_dataset_) {
				diag::error(
				    "DataSet::read_elements_until file={} offset=0x{:X} tag={} last_tag={} reason=tag order decreased (unexpected sequence end?)",
				    path(), stream->tell() - 8, tag_to_string(tag), tag_to_string(last_tag_loaded_));
			break;
		}

		// DATA ELEMENT STRUCTURE WITH EXPLICIT VR
		VR vr = VR::None;
		std::size_t length = 0;

		if (explicit_vr) {
			const char vr_char0 = static_cast<char>(buf8[4]);
			const char vr_char1 = static_cast<char>(buf8[5]);
			vr = VR(vr_char0, vr_char1);

			if (vr.is_known()) {
				if (vr.uses_explicit_16bit_vl()) {
					length = endian::load_value<std::uint16_t>(buf8.data() + 6, little_endian);
				} else {
					// PS3.5 Table 7.1-1. Data Element with Explicit VR other than
					// as shown in Table 7.1-2
					if (stream->read_4bytes(buf4) != 4) {
						diag::error_and_throw(
					    "DataSet::read_elements_until file={} offset=0x{:X} tag={} vr={} reason=failed to read 4-byte length",
						    path(), stream->tell(), tag_to_string(tag), vr.str());
					}
					length = endian::load_value<std::uint32_t>(buf4.data(), little_endian);
					
				// In a strange implementation, VR 'UT' takes 2 bytes for 'len'
				if (length > stream->bytes_remaining()) {
						stream->unread(4);
						length = endian::load_value<std::uint16_t>(buf8.data() + 6, little_endian);
					}
					if (length == 0 || length > stream->bytes_remaining()) {
						diag::error_and_throw(
						    "DataSet::read_elements_until file={} offset=0x{:X} tag={} vr={} length={} remaining={} reason=value length exceeds remaining bytes",
						    path(), stream->tell(), tag_to_string(tag), vr.str(), length, stream->bytes_remaining());
					}
			}
		} else {
				if (vr == VR('P', 'X')) {
					length = endian::load_value<std::uint16_t>(buf8.data() + 6, little_endian);
				} else {
					// assume this Data Element has implicit vr
					// PS 3.5-2009, Table 7.1-3
		            // DATA ELEMENT STRUCTURE WITH IMPLICIT VR
					length = endian::load_le<std::uint32_t>(buf8.data() + 4);
					const auto vr_value = lookup::tag_to_vr(tag.value());
					vr = vr_value ? VR(vr_value) : VR::UN;
				}
			}
		} else {
			// assume this Data Element has implicit vr
			// PS 3.5-2009, Table 7.1-3
			// DATA ELEMENT STRUCTURE WITH IMPLICIT VR
			const auto vr_value = lookup::tag_to_vr(tag.value());
			vr = vr_value ? VR(vr_value) : VR::UN;
			length = endian::load_le<std::uint32_t>(buf8.data() + 4);
		}

		// Data Element's values position
    	auto offset = stream->tell();
		
		// if (vr == VR::SQ) {
		// }
		// else if (tag == "7fe0,0010"_tag) {
		// }
		// else

			{
				if (length != 0xffffffff) {
					add_dataelement(tag, vr, offset, length);
					auto n = stream->skip(length);
					if (n != length) {
						diag::error_and_throw(
						    "DataSet::read_elements_until file={} offset=0x{:X} tag={} vr={} length={} reason=value length exceeds remaining bytes",
						    path(), offset, tag_to_string(tag), vr.str(), length);
					}

				} else {
				// PROBABLY SEQUENCE ELEMENT WITH IMPLICIT VR WITH ...

			}
		}

		last_tag_loaded_ = tag;

    	if (tag == load_until) break;
	} // END OF WHILE -----------------------------------------------------------

	last_tag_loaded_ = load_until;
	if (stream->is_eof())
		last_tag_loaded_ = "ffff,ffff"_tag;
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
