#include <dicom.h>
#include <dicom_endian.h>
#include <diagnostics.h>
#include <instream.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <fmt/format.h>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <span>
#include <cctype>
#include <limits>

#include <libdeflate.h>

using namespace dicom::literals;
namespace diag = dicom::diag;

namespace dicom {

inline const uid::WellKnown kExplicitVrLittleEndian =
    uid::lookup("ExplicitVRLittleEndian").value_or(uid::WellKnown{});
inline const uid::WellKnown kImplicitVrLittleEndianUid =
    uid::lookup("ImplicitVRLittleEndian").value_or(uid::WellKnown{});
inline const uid::WellKnown kPapyrusImplicitVrLittleEndianUid =
	uid::lookup("Papyrus3ImplicitVRLittleEndian").value_or(uid::WellKnown{});
inline const uid::WellKnown kExplicitVrBigEndianUid =
	uid::lookup("ExplicitVRBigEndian").value_or(uid::WellKnown{});
inline const uid::WellKnown kDeflatedExplicitVrLittleEndianUid =
	uid::lookup("DeflatedExplicitVRLittleEndian").value_or(uid::WellKnown{});

void apply_transfer_syntax_flags(uid::WellKnown transfer_syntax, bool& little_endian,
    bool& explicit_vr) {
	little_endian = true;
	explicit_vr = true;

	if (!transfer_syntax.valid()) {
		return;
	}
	if (transfer_syntax == kExplicitVrBigEndianUid) {
		little_endian = false;
		return;
	}
	if (transfer_syntax == kImplicitVrLittleEndianUid ||
	    transfer_syntax == kPapyrusImplicitVrLittleEndianUid) {
		explicit_vr = false;
	}
}

const char* libdeflate_result_name(enum libdeflate_result result) noexcept {
	switch (result) {
	case LIBDEFLATE_SUCCESS:
		return "LIBDEFLATE_SUCCESS";
	case LIBDEFLATE_BAD_DATA:
		return "LIBDEFLATE_BAD_DATA";
	case LIBDEFLATE_SHORT_OUTPUT:
		return "LIBDEFLATE_SHORT_OUTPUT";
	case LIBDEFLATE_INSUFFICIENT_SPACE:
		return "LIBDEFLATE_INSUFFICIENT_SPACE";
	default:
		return "LIBDEFLATE_UNKNOWN";
	}
}

std::vector<std::uint8_t> inflate_deflated_image(std::span<const std::uint8_t> full_input,
    std::size_t deflated_start_offset, const std::string& file_path) {
	if (deflated_start_offset > full_input.size()) {
		diag::error_and_throw(
		    fmt::format(
		        "DataSet::read_attached_stream file={} offset=0x{:X} reason=invalid deflated data start offset",
		        file_path, deflated_start_offset));
	}

	const auto compressed_input = full_input.subspan(deflated_start_offset);
	if (compressed_input.empty()) {
		return std::vector<std::uint8_t>(full_input.begin(),
		    full_input.begin() + static_cast<std::ptrdiff_t>(deflated_start_offset));
	}

	struct libdeflate_decompressor* decompressor = libdeflate_alloc_decompressor();
	if (!decompressor) {
		diag::error_and_throw(
		    fmt::format(
		        "DataSet::read_attached_stream file={} offset=0x{:X} reason=failed to allocate libdeflate decompressor",
		        file_path, deflated_start_offset));
	}

	std::size_t tail_capacity = std::max<std::size_t>(compressed_input.size() * 4, 1u << 20);
	if (deflated_start_offset > std::numeric_limits<std::size_t>::max() - tail_capacity) {
		libdeflate_free_decompressor(decompressor);
		diag::error_and_throw(
		    fmt::format(
		        "DataSet::read_attached_stream file={} offset=0x{:X} reason=deflated output too large",
		        file_path, deflated_start_offset));
	}

	std::vector<std::uint8_t> output(deflated_start_offset + tail_capacity);
	if (deflated_start_offset > 0) {
		std::memcpy(output.data(), full_input.data(), deflated_start_offset);
	}

	size_t actual_out = 0;
	enum libdeflate_result result = LIBDEFLATE_INSUFFICIENT_SPACE;
	while (true) {
		result = libdeflate_deflate_decompress(decompressor, compressed_input.data(),
		    compressed_input.size(), output.data() + deflated_start_offset,
		    output.size() - deflated_start_offset, &actual_out);
		if (result == LIBDEFLATE_SUCCESS) {
			output.resize(deflated_start_offset + actual_out);
			break;
		}
		if (result != LIBDEFLATE_INSUFFICIENT_SPACE) {
			libdeflate_free_decompressor(decompressor);
			diag::error_and_throw(
			    fmt::format(
			        "DataSet::read_attached_stream file={} offset=0x{:X} reason=deflate decompression failed result={}",
			        file_path, deflated_start_offset, libdeflate_result_name(result)));
		}

		const auto current_tail_capacity = output.size() - deflated_start_offset;
		if (current_tail_capacity > std::numeric_limits<std::size_t>::max() / 2) {
			libdeflate_free_decompressor(decompressor);
			diag::error_and_throw(
			    fmt::format(
			        "DataSet::read_attached_stream file={} offset=0x{:X} reason=deflated output too large",
			        file_path, deflated_start_offset));
		}

		const auto next_tail_capacity = current_tail_capacity * 2;
		if (deflated_start_offset > std::numeric_limits<std::size_t>::max() - next_tail_capacity) {
			libdeflate_free_decompressor(decompressor);
			diag::error_and_throw(
			    fmt::format(
			        "DataSet::read_attached_stream file={} offset=0x{:X} reason=deflated output too large",
			        file_path, deflated_start_offset));
		}
		output.resize(deflated_start_offset + next_tail_capacity);
	}

	libdeflate_free_decompressor(decompressor);
	return output;
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

// Trim leading/trailing ASCII whitespace.
inline std::string_view trim(std::string_view sv) {
	while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front()))) {
		sv.remove_prefix(1);
	}
	while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.back()))) {
		sv.remove_suffix(1);
	}
	return sv;
}

// Remove outer parentheses if both present.
inline std::string_view strip_parens(std::string_view sv) {
	if (sv.size() >= 2 && sv.front() == '(' && sv.back() == ')') {
		return sv.substr(1, sv.size() - 2);
	}
	return sv;
}

inline std::uint8_t hex_digit_value_local(char c) {
	if (c >= '0' && c <= '9') {
		return static_cast<std::uint8_t>(c - '0');
	}
	return static_cast<std::uint8_t>((c & 0xDF) - 'A' + 10);
}

inline std::optional<std::uint32_t> parse_hex_prefix(std::string_view sv) {
	std::uint32_t value = 0;
	int digits = 0;
	for (char c : sv) {
		if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) {
			value = (value << 4) | hex_digit_value_local(c);
			++digits;
			continue;
		}
		break;  // stop at first non-hex
	}
	if (digits == 0) {
		return std::nullopt;
	}
	return value;
}

template <typename DataSetPtr>
std::optional<Tag> parse_private_creator_tag(DataSetPtr* dataset, std::string_view token) {
	token = strip_parens(trim(token));

	const auto comma = token.find(',');
	if (comma == std::string_view::npos) {
		return std::nullopt;  // not a private-creator pattern
	}

	const auto second_comma = token.find(',', comma + 1);
	if (second_comma != std::string_view::npos) {
		// Recommended style: gggg,xxee,CREATOR  (quotes optional, xx literal allowed)
		auto group_sv = trim(token.substr(0, comma));
		auto element_sv = trim(token.substr(comma + 1, second_comma - comma - 1));
		auto creator_sv = trim(token.substr(second_comma + 1));

		if (group_sv.empty() || element_sv.empty() || creator_sv.empty()) {
			diag::error_and_throw("Malformed private creator tag string");
		}

		// Strip optional quotes
		if (creator_sv.size() >= 2 &&
		    ((creator_sv.front() == '"' && creator_sv.back() == '"') ||
		     (creator_sv.front() == '\'' && creator_sv.back() == '\''))) {
			creator_sv = creator_sv.substr(1, creator_sv.size() - 2);
		}

		auto group_parsed = parse_hex_prefix(group_sv);
		if (!group_parsed) {
			diag::error_and_throw("Malformed private creator tag string");
		}
		const auto group = *group_parsed;
		if ((group & 0x1u) == 0) {
			diag::error_and_throw("Private creator tag requires an odd group");
		}

		// Remove literal "xx" placeholder if present.
		if (element_sv.size() >= 2 &&
		    (element_sv[0] == 'x' || element_sv[0] == 'X') &&
		    (element_sv[1] == 'x' || element_sv[1] == 'X')) {
			element_sv.remove_prefix(2);
		}

		if (element_sv.empty()) {
			diag::error_and_throw("Malformed private creator tag string");
		}

		auto element_parsed = parse_hex_prefix(element_sv);
		if (!element_parsed) {
			diag::error_and_throw("Malformed private creator tag string");
		}
		const auto element_low = *element_parsed & 0xFFu;

		const auto desired_creator = std::string(creator_sv);
		for (std::uint32_t block = 0x10; block <= 0xFF; ++block) {
			Tag creator_tag(static_cast<std::uint16_t>(group), static_cast<std::uint16_t>(block));
			auto* creator_el = dataset->get_dataelement(creator_tag);
			if (creator_el == NullElement()) {
				continue;
			}
			auto value = creator_el->to_string_view();
			if (value && *value == desired_creator) {
				const auto element = static_cast<std::uint16_t>((block << 8) | element_low);
				return Tag(static_cast<std::uint16_t>(group), element);
			}
		}
		return std::nullopt;  // creator not found
	}
	return std::nullopt;
}
DataSet::DataSet() : root_dataset_(this) {
	apply_transfer_syntax_flags(kExplicitVrLittleEndian, little_endian_, explicit_vr_);
}

DataSet::DataSet(DataSet* root_dataset)
    : root_file_(root_dataset ? root_dataset->root_file_ : nullptr),
      root_dataset_(root_dataset ? root_dataset->root_dataset_ : this) {
	apply_transfer_syntax_flags(kExplicitVrLittleEndian, little_endian_, explicit_vr_);
	if (root_dataset_ && root_dataset_ != this) {
		little_endian_ = root_dataset_->little_endian_;
		explicit_vr_ = root_dataset_->explicit_vr_;
	}
}

DataSet::DataSet(DicomFile* root_file) : root_file_(root_file), root_dataset_(this) {
	apply_transfer_syntax_flags(kExplicitVrLittleEndian, little_endian_, explicit_vr_);
}

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

void DataSet::attach_to_substream(InStream* basestream, std::size_t size) {
	if (!basestream) {
		diag::error_and_throw("DataSet::attach_to_stream reason=null basestream");
	}
	const auto base_identifier = basestream->identifier();
	const auto sub_identifier = fmt::format("{}@0x{:X}+{}",
	    base_identifier.empty() ? "<substream>" : base_identifier,
	    basestream->tell(), size);
	auto substream = std::make_unique<InSubStream>(basestream, size);
	attach_to_stream(sub_identifier, std::move(substream));
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
	// Returns stream identifier; if no stream is attached, returns an empty string.
	static const std::string kEmpty;
	return stream_ ? stream_->identifier() : kEmpty;
}

InStream& DataSet::stream() {
	return *stream_;
}

const InStream& DataSet::stream() const {
	return *stream_;
}

uid::WellKnown DataSet::transfer_syntax_uid() const {
	if (!root_file_) {
		return uid::WellKnown{};
	}
	return root_file_->transfer_syntax_uid_;
}

DataSet* DataSet::root_dataset() const noexcept {
	return root_dataset_ ? root_dataset_ : const_cast<DataSet*>(this);
}

std::size_t DataSet::size() const {
	std::size_t count = 0;
	for (auto it = cbegin(); it != cend(); ++it) {
		++count;
	}
	return count;
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
DataElement* DataSet::add_dataelement(Tag tag, VR vr, std::size_t offset, std::size_t length) {
	const auto tag_value = tag.value();

	if (vr == VR::None) {
		const auto vr_value = lookup::tag_to_vr(tag_value);
		if (vr_value == 0) {
			diag::error_and_throw(
			    "DataSet::add_dataelement file={} tag={} reason=VR required for unknown tag",
			    root_dataset()->path(), tag.to_string());
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

// Parse a tag path expressed as text and resolve it to a DataElement.
// Supported forms:
//   - Hex tag with or without parens/comma: "00100010", "(0010,0010)"
//   - Keyword: "PatientName"
//   - Private creator (recommended style): "gggg,xxee,CREATOR" where gggg is odd,
//     xx is the reserved block (literal "xx" placeholder allowed), and CREATOR is the
//     Private Creator string present in (gggg,00xx); e.g., "0009,xx1e,GEMS_GENIE_1"
//   - Nested sequences: "00082112.0.00081190" (sequence tag . index . child tag ...),
//     keyword-friendly paths like "RadiopharmaceuticalInformationSequence.0.RadionuclideTotalDose" work too
// Behavior:
//   - Returns NullElement() when the tag (or nested dataset) is missing
//   - Uses diag::error_and_throw for malformed strings, non-sequence traversal, bad indices, etc.
DataElement* DataSet::get_dataelement(std::string_view tag_path) {
	DataSet* current = this;
	std::string_view remaining = trim(tag_path);

	while (!remaining.empty()) {
		const auto dot_pos = remaining.find('.');
		auto tag_token = strip_parens(trim(remaining.substr(0, dot_pos)));
		remaining = (dot_pos == std::string_view::npos) ? std::string_view{} : remaining.substr(dot_pos + 1);

		Tag tag{};
		try {
			tag = Tag(tag_token);
		} catch (const std::invalid_argument&) {
			auto private_tag = parse_private_creator_tag(current, tag_token);
			if (!private_tag) {
				return NullElement();
			}
			tag = *private_tag;
		}

		DataElement* element = current->get_dataelement(tag);
		if (element == NullElement()) {
			return element;
		}
		if (dot_pos == std::string_view::npos) {
			return element;
		}

		const auto next_dot = remaining.find('.');
		auto index_token = trim(remaining.substr(0, next_dot));
		remaining = (next_dot == std::string_view::npos) ? std::string_view{} : remaining.substr(next_dot + 1);

		if (!element->vr().is_sequence()) {
			diag::error_and_throw("Element {} is not a sequence; cannot index into it",
			    element->tag().to_string());
		}

		auto* seq = element->as_sequence();
		if (!seq) {
			return NullElement();
		}

		std::size_t idx = 0;
		try {
			idx = static_cast<std::size_t>(std::stoul(std::string(index_token), nullptr, 10));
		} catch (const std::exception&) {
			diag::error_and_throw("Malformed sequence index in tag path");
		}

		current = seq->get_dataset(idx);
		if (!current) {
			return NullElement();
		}
	}

	return NullElement();
}

const DataElement* DataSet::get_dataelement(std::string_view tag_path) const {
	const DataSet* current = this;
	std::string_view remaining = trim(tag_path);

	while (!remaining.empty()) {
		const auto dot_pos = remaining.find('.');
		auto tag_token = strip_parens(trim(remaining.substr(0, dot_pos)));
		remaining = (dot_pos == std::string_view::npos) ? std::string_view{} : remaining.substr(dot_pos + 1);

		Tag tag{};
		try {
			tag = Tag(tag_token);
		} catch (const std::invalid_argument&) {
			auto private_tag = parse_private_creator_tag(current, tag_token);
			if (!private_tag) {
				return NullElement();
			}
			tag = *private_tag;
		}

		const DataElement* element = current->get_dataelement(tag);
		if (element == NullElement()) {
			return element;
		}
		if (dot_pos == std::string_view::npos) {
			return element;
		}

		const auto next_dot = remaining.find('.');
		auto index_token = trim(remaining.substr(0, next_dot));
		remaining = (next_dot == std::string_view::npos) ? std::string_view{} : remaining.substr(next_dot + 1);

		if (!element->vr().is_sequence()) {
			diag::error_and_throw("Element {} is not a sequence; cannot index into it",
			    element->tag().to_string());
		}

		auto* seq = element->as_sequence();
		if (!seq) {
			return NullElement();
		}

		std::size_t idx = 0;
		try {
			idx = static_cast<std::size_t>(std::stoul(std::string(index_token), nullptr, 10));
		} catch (const std::exception&) {
			diag::error_and_throw("Malformed sequence index in tag path");
		}

		current = seq->get_dataset(idx);
		if (!current) {
			return NullElement();
		}
	}

	return NullElement();
}

DataElement& DataSet::operator[](Tag tag) {
	return *get_dataelement(tag);
}

const DataElement& DataSet::operator[](Tag tag) const {
	return *get_dataelement(tag);
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
	if (root_file_) {
		root_file_->invalidate_pixel_info_cache();
	}
	last_tag_loaded_ = Tag::from_value(0);
	if (root_file_) {
		root_file_->set_transfer_syntax(kExplicitVrLittleEndian);
	} else {
		apply_transfer_syntax_flags(kExplicitVrLittleEndian, little_endian_, explicit_vr_);
	}

	// parse DICOM stream, starting with skipping the 128-byte preamble.
	stream_->rewind();
	stream_->skip(128);

	// check magic number "DICM"
	std::array<std::uint8_t, 4> magic{};
	if (stream_->read_4bytes(magic) != 4 || std::memcmp(magic.data(), "DICM", 4) != 0) {
		// Some files ship without the PART 10 preamble/magic; rewind to treat them as raw streams.
		stream_->rewind();
	}

	read_elements_until("(0002,FFFF)"_tag, stream_.get());

	const auto* transfer_syntax = get_dataelement("(0002,0010)"_tag);
	if (auto well_known = transfer_syntax->to_transfer_syntax_uid()) {
		if (root_file_) {
			root_file_->set_transfer_syntax(*well_known);
		} else {
			apply_transfer_syntax_flags(*well_known, little_endian_, explicit_vr_);
		}
	} else if (auto uid_value = transfer_syntax->to_uid_string()) {
		diag::error(
		    "DataSet::read_attached_stream file={} transfer_syntax_uid={} reason=unknown transfer syntax UID",
		    path(), *uid_value);
	}

	if (transfer_syntax_uid() == kDeflatedExplicitVrLittleEndianUid) {
		std::size_t deflated_start_offset = stream_->tell();
		if (const auto* meta_group_length = get_dataelement("(0002,0000)"_tag); *meta_group_length) {
			if (auto group_length = meta_group_length->to_long();
			    group_length && *group_length >= 0) {
				const auto offset_candidate = meta_group_length->offset() +
				    meta_group_length->length() + static_cast<std::size_t>(*group_length);
				if (offset_candidate <= stream_->endoffset()) {
					deflated_start_offset = offset_candidate;
				}
			}
		}

		const auto full_size = stream_->endoffset();
		const auto full_span = stream_->get_span(0, full_size);
		if (deflated_start_offset > full_span.size()) {
			diag::error_and_throw(
			    fmt::format(
			        "DataSet::read_attached_stream file={} offset=0x{:X} reason=invalid deflated data start offset",
			        path(), deflated_start_offset));
		}

		auto inflated_image = inflate_deflated_image(full_span, deflated_start_offset, path());

		const std::string stream_identifier = path();
		attach_to_memory(stream_identifier, std::move(inflated_image));
		stream_->seek(deflated_start_offset);
	}

	read_elements_until(options.load_until, stream_.get());
}

void DataSet::read_elements_until(Tag load_until, InStream* stream) {
	std::array<std::uint8_t, 8> buf8{};
	std::array<std::uint8_t, 4> buf4{};

	if (!stream) {
		// stream is nullptr when second call from read_attached_stream() or calls
		// Sequence::read_from_stream()
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
					    path(), item_offset, tag.to_string());
					continue;
				}
			} else if (tag == "fffe,e0dd"_tag) {
				diag::error(
				    "DataSet::read_elements_until file={} offset=0x{:X} tag={} reason=sequence delim encountered while parsing root dataset",
				    path(), item_offset, tag.to_string());

				if (this != root_dataset_)
					break;
        		else
          			continue;
			}
		}

		if (this == root_dataset_ && tag > load_until) {
			stream->unread(8);
			break;
		}

		if (last_tag_loaded_ > tag && this != root_dataset_) {
			diag::error(
				"DataSet::read_elements_until file={} offset=0x{:X} tag={} last_tag={} reason=tag order decreased (unexpected sequence end?)",
				path(), stream->tell() - 8, tag.to_string(), last_tag_loaded_.to_string());
			stream->unread(8);
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
						    path(), stream->tell(), tag.to_string(), vr.str());
					}
					length = endian::load_value<std::uint32_t>(buf4.data(), little_endian);
					
					// Some non‑conforming writers store VR UT with a 2‑byte length; salvage it.
					if (length != 0xffffffff && length > stream->bytes_remaining()) {
						stream->unread(4);
						length = endian::load_value<std::uint16_t>(buf8.data() + 6, little_endian);
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

		if (vr == VR::SQ) {
			if (length == 0xffffffff) {
				length = stream->bytes_remaining();
			}

			DataElement *elem = add_dataelement(tag, VR::SQ, offset, length);
			Sequence *seq = elem->as_sequence();
			InSubStream subs(stream, length);

			size_t offset_end;
			seq->read_from_stream(&subs);
			offset_end= subs.tell();
			length = offset_end - offset;
			elem->set_length(length);
			stream->skip(length);
		}
		else if (tag == "7fe0,0010"_tag) {
			if (length != 0xffffffff) {
				if ((get_dataelement("BitsAllocated"_tag)->to_long().value_or(0) > 8) && (vr == VR::OB))
					vr = VR::OW;
				add_dataelement(tag, vr, offset, length);
				if (stream->skip(length) != length) {
					diag::error_and_throw(
					    "DataSet::read_elements_until file={} offset=0x{:X} tag={} vr={} length={} reason=failed to skip pixel data bytes",
					    path(), offset, tag.to_string(), vr.str(), length);
				}
			} else {
				vr = VR::PX;

				DataElement *elem = add_dataelement(tag, VR::PX, offset, length);
				PixelSequence *pixseq = elem->as_pixel_sequence();

				// process basic offset table of the pixel sequence
				pixseq->attach_to_stream(stream, stream->bytes_remaining());
				pixseq->read_attached_stream();
				size_t offset_end = pixseq->stream()->tell();
				length = offset_end - offset;
				elem->set_length(length);
				stream->skip(length);
			}
		}
		else {
			if (length != 0xffffffff) {
				add_dataelement(tag, vr, offset, length);
				auto n = stream->skip(length);
				if (n != length) {
					diag::error_and_throw(
						"DataSet::read_elements_until file={} offset=0x{:X} tag={} vr={} length={} reason=value length exceeds remaining bytes",
						path(), offset, tag.to_string(), vr.str(), length);
				}

			} else {
				// PROBABLY SEQUENCE ELEMENT WITH IMPLICIT VR WITH ...
				length = stream->bytes_remaining();
				vr = VR::SQ;
				
				DataElement *elem = add_dataelement(tag, VR::SQ, offset, length);
				Sequence *seq = elem->as_sequence();
				InSubStream subs(stream, length);

				size_t offset_end;
				seq->read_from_stream(&subs);
				offset_end= subs.tell();
				length = offset_end - offset;
				elem->set_length(length);
				stream->skip(length);
			}
		}

		last_tag_loaded_ = tag;

    	if (tag == load_until) break;
	} // END OF WHILE -----------------------------------------------------------

	last_tag_loaded_ = load_until;
	if (stream->is_eof())
		last_tag_loaded_ = "ffff,ffff"_tag;
}

void DataSet::ensure_loaded(Tag tag) {
	if (tag > last_tag_loaded_) {
		read_elements_until(tag, nullptr);
	}
}

void DataSet::ensure_loaded(Tag tag) const {
	const_cast<DataSet*>(this)->ensure_loaded(tag);
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

} // namespace dicom
