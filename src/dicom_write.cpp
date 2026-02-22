#include "dicom.h"

#include "dicom_endian.h"
#include "diagnostics.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dicom {
using namespace dicom::literals;

namespace {

constexpr Tag kItemTag{0xFFFEu, 0xE000u};
constexpr Tag kItemDelimitationTag{0xFFFEu, 0xE00Du};
constexpr Tag kSequenceDelimitationTag{0xFFFEu, 0xE0DDu};
constexpr Tag kPixelDataTag{0x7FE0u, 0x0010u};

constexpr Tag kFileMetaGroupLengthTag{0x0002u, 0x0000u};
constexpr Tag kFileMetaInformationVersionTag{0x0002u, 0x0001u};
constexpr Tag kMediaStorageSopClassUidTag{0x0002u, 0x0002u};
constexpr Tag kMediaStorageSopInstanceUidTag{0x0002u, 0x0003u};
constexpr Tag kTransferSyntaxUidTag{0x0002u, 0x0010u};
constexpr Tag kImplementationClassUidTag{0x0002u, 0x0012u};
constexpr Tag kImplementationVersionNameTag{0x0002u, 0x0013u};

constexpr Tag kSopClassUidTag{0x0008u, 0x0016u};
constexpr Tag kSopInstanceUidTag{0x0008u, 0x0018u};

struct Encoding {
	bool little_endian{true};
	bool explicit_vr{true};
};

constexpr Encoding kMetaEncoding{true, true};

struct StreamWriter {
	explicit StreamWriter(std::ostream& out) : os(out) {}

	void append(const void* data, std::size_t size) {
		if (size == 0) {
			return;
		}
		os.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
		if (!os) {
			diag::error_and_throw("write_to_stream reason=failed to write output bytes");
		}
		written += size;
	}

	void append_byte(std::uint8_t value) {
		os.put(static_cast<char>(value));
		if (!os) {
			diag::error_and_throw("write_to_stream reason=failed to write output byte");
		}
		++written;
	}

	std::ostream& os;
	std::size_t written{0};
};

struct VectorWriter {
	explicit VectorWriter(std::vector<std::uint8_t>& out) : bytes(out) {}

	void append(const void* data, std::size_t size) {
		if (size == 0) {
			return;
		}
		const auto* begin = static_cast<const std::uint8_t*>(data);
		bytes.insert(bytes.end(), begin, begin + static_cast<std::ptrdiff_t>(size));
		written += size;
	}

	void append_byte(std::uint8_t value) {
		bytes.push_back(value);
		++written;
	}

	std::vector<std::uint8_t>& bytes;
	std::size_t written{0};
};

struct CountingWriter {
	void append(const void*, std::size_t size) { written += size; }
	void append_byte(std::uint8_t) { ++written; }
	std::size_t written{0};
};

[[nodiscard]] std::uint32_t checked_u32(std::size_t value, std::string_view label) {
	if (value > std::numeric_limits<std::uint32_t>::max()) {
		diag::error_and_throw("write_to_stream reason={} exceeds 32-bit range", label);
	}
	return static_cast<std::uint32_t>(value);
}

template <typename Writer>
void write_u16(Writer& writer, std::uint16_t value, bool little_endian) {
	std::array<std::uint8_t, 2> bytes{};
	endian::store_value<std::uint16_t>(bytes.data(), value, little_endian);
	writer.append(bytes.data(), bytes.size());
}

template <typename Writer>
void write_u32(Writer& writer, std::uint32_t value, bool little_endian) {
	std::array<std::uint8_t, 4> bytes{};
	endian::store_value<std::uint32_t>(bytes.data(), value, little_endian);
	writer.append(bytes.data(), bytes.size());
}

template <typename Writer>
void write_tag(Writer& writer, Tag tag, bool little_endian) {
	write_u16(writer, tag.group(), little_endian);
	write_u16(writer, tag.element(), little_endian);
}

[[nodiscard]] VR normalize_vr_for_write(Tag tag, VR vr) {
	if (vr == VR::None) {
		const auto vr_value = lookup::tag_to_vr(tag.value());
		vr = vr_value == 0 ? VR::UN : VR(vr_value);
	}
	if (vr == VR::PX) {
		return VR::OB;
	}
	return vr;
}

template <typename Writer>
void write_element_header(Writer& writer, Tag tag, VR vr, std::uint32_t value_length,
    bool undefined_length, const Encoding& encoding) {
	write_tag(writer, tag, encoding.little_endian);

	if (!encoding.explicit_vr) {
		write_u32(writer, undefined_length ? 0xFFFFFFFFu : value_length, encoding.little_endian);
		return;
	}

	const std::uint16_t raw_vr = vr.raw_code();
	const std::array<std::uint8_t, 2> vr_bytes{
	    static_cast<std::uint8_t>((raw_vr >> 8) & 0xFFu),
	    static_cast<std::uint8_t>(raw_vr & 0xFFu)};
	writer.append(vr_bytes.data(), vr_bytes.size());

	if (!undefined_length && vr.uses_explicit_16bit_vl()) {
		if (value_length > std::numeric_limits<std::uint16_t>::max()) {
			diag::error_and_throw(
			    "write_to_stream reason=16-bit VL overflow for tag={} vr={} length={}",
			    tag.to_string(), vr.str(), value_length);
		}
		write_u16(writer, static_cast<std::uint16_t>(value_length), encoding.little_endian);
		return;
	}

	write_u16(writer, 0u, encoding.little_endian);
	write_u32(writer, undefined_length ? 0xFFFFFFFFu : value_length, encoding.little_endian);
}

template <typename Writer>
void write_item_header(Writer& writer, Tag tag, std::uint32_t value_length, bool little_endian) {
	write_tag(writer, tag, little_endian);
	write_u32(writer, value_length, little_endian);
}

[[nodiscard]] std::size_t padded_length(std::size_t raw_length) {
	return raw_length + (raw_length & 1u);
}

template <typename Writer>
void write_non_sequence_element(Writer& writer, Tag tag, VR vr, std::span<const std::uint8_t> value,
    const Encoding& encoding) {
	const auto normalized_vr = normalize_vr_for_write(tag, vr);
	const auto raw_length = value.size();
	const auto full_length = padded_length(raw_length);
	write_element_header(
	    writer, tag, normalized_vr, checked_u32(full_length, "element length"), false, encoding);
	if (!value.empty()) {
		writer.append(value.data(), value.size());
	}
	if ((raw_length & 1u) != 0u) {
		writer.append_byte(normalized_vr.padding_byte());
	}
}

template <typename Writer>
void write_dataset(const DataSet& dataset, Writer& writer, const Encoding& encoding,
    bool skip_group_0002);

template <typename Writer>
void write_sequence_element(const DataElement& element, Writer& writer, const Encoding& encoding) {
	const Sequence* sequence = element.as_sequence();
	if (!sequence) {
		diag::error_and_throw("write_to_stream reason=SQ element has null sequence pointer");
	}

	write_element_header(writer, element.tag(), VR::SQ, 0xFFFFFFFFu, true, encoding);
	for (const auto& item_dataset_ptr : *sequence) {
		if (!item_dataset_ptr) {
			continue;
		}
		write_item_header(writer, kItemTag, 0xFFFFFFFFu, encoding.little_endian);
		write_dataset(*item_dataset_ptr, writer, encoding, false);
		write_item_header(writer, kItemDelimitationTag, 0u, encoding.little_endian);
	}
	write_item_header(writer, kSequenceDelimitationTag, 0u, encoding.little_endian);
}

template <typename Writer>
void write_pixel_sequence_element(const DataElement& element, Writer& writer, const Encoding& encoding) {
	const PixelSequence* pixel_sequence = element.as_pixel_sequence();
	if (!pixel_sequence) {
		diag::error_and_throw("write_to_stream reason=PX element has null pixel sequence pointer");
	}

	const VR pixel_vr = encoding.explicit_vr ? VR::OB : VR::None;
	write_element_header(writer, element.tag(), pixel_vr, 0xFFFFFFFFu, true, encoding);

	// Keep BOT empty in Minimum Viable Product for fast write path.
	write_item_header(writer, kItemTag, 0u, encoding.little_endian);

	const InStream* seq_stream = pixel_sequence->stream();
	const auto write_fragment = [&](std::span<const std::uint8_t> fragment) {
		const auto full_length = padded_length(fragment.size());
		write_item_header(
		    writer, kItemTag, checked_u32(full_length, "pixel fragment length"), encoding.little_endian);
		if (!fragment.empty()) {
			writer.append(fragment.data(), fragment.size());
		}
		if ((fragment.size() & 1u) != 0u) {
			writer.append_byte(0x00u);
		}
	};

	for (std::size_t frame_index = 0; frame_index < pixel_sequence->number_of_frames(); ++frame_index) {
		const PixelFrame* frame = pixel_sequence->frame(frame_index);
		if (!frame) {
			continue;
		}

		const auto encoded_data = frame->encoded_data_view();
		if (!encoded_data.empty()) {
			write_fragment(encoded_data);
			continue;
		}

		const auto& fragments = frame->fragments();
		for (const auto& fragment : fragments) {
			if (!seq_stream) {
				diag::error_and_throw(
				    "write_to_stream reason=pixel fragment references stream but stream is null");
			}
			if (fragment.offset > seq_stream->end_offset() ||
			    fragment.length > seq_stream->end_offset() - fragment.offset) {
				diag::error_and_throw(
				    "write_to_stream reason=pixel fragment out of bounds offset=0x{:X} length={}",
				    fragment.offset, fragment.length);
			}
			write_fragment(seq_stream->get_span(fragment.offset, fragment.length));
		}
	}

	write_item_header(writer, kSequenceDelimitationTag, 0u, encoding.little_endian);
}

template <typename Writer>
void write_data_element(const DataElement& element, Writer& writer, const Encoding& encoding) {
	if (element.vr().is_sequence()) {
		write_sequence_element(element, writer, encoding);
		return;
	}
	if (element.vr().is_pixel_sequence()) {
		write_pixel_sequence_element(element, writer, encoding);
		return;
	}

	write_non_sequence_element(writer, element.tag(), element.vr(), element.value_span(), encoding);
}

template <typename Writer>
void write_dataset(const DataSet& dataset, Writer& writer, const Encoding& encoding,
    bool skip_group_0002) {
	for (const auto& element : dataset) {
		if (skip_group_0002 && element.tag().group() == 0x0002u) {
			continue;
		}
		write_data_element(element, writer, encoding);
	}
}

[[nodiscard]] std::optional<std::string> read_uid_element(const DataSet& dataset, Tag tag) {
	const DataElement* element = dataset.get_dataelement(tag);
	if (element->is_missing()) {
		return std::nullopt;
	}
	auto uid_value = element->to_uid_string();
	if (!uid_value || uid_value->empty()) {
		return std::nullopt;
	}
	return *uid_value;
}

[[nodiscard]] std::string_view transfer_syntax_uid_from_flags(const DataSet& dataset) {
	if (!dataset.is_little_endian()) {
		return "ExplicitVRBigEndian"_uid.value();
	}
	if (!dataset.is_explicit_vr()) {
		return "ImplicitVRLittleEndian"_uid.value();
	}
	return "ExplicitVRLittleEndian"_uid.value();
}

[[nodiscard]] std::string infer_transfer_syntax_uid(const DicomFile& file, const DataSet& dataset) {
	if (const auto file_ts = file.transfer_syntax_uid(); file_ts.valid()) {
		const auto value = file_ts.value();
		if (!value.empty()) {
			return std::string(value.data(), value.size());
		}
	}

	if (auto from_meta = read_uid_element(dataset, kTransferSyntaxUidTag)) {
		return *from_meta;
	}

	const auto fallback = transfer_syntax_uid_from_flags(dataset);
	return std::string(fallback.data(), fallback.size());
}

template <typename Fn>
void for_each_file_meta_element(const DataSet& dataset, Fn&& fn) {
	for (const auto& element : dataset) {
		const auto group = element.tag().group();
		if (group < 0x0002u) {
			continue;
		}
		if (group > 0x0002u) {
			break;
		}
		if (element.vr().is_sequence() || element.vr().is_pixel_sequence()) {
			diag::error_and_throw(
			    "write_to_stream reason=file meta element is SQ/PX tag={} vr={}",
			    element.tag().to_string(), element.vr().str());
		}
		fn(element);
	}
}

[[nodiscard]] std::uint32_t measure_meta_group_length(const DataSet& dataset) {
	CountingWriter measuring_writer;
	for_each_file_meta_element(dataset, [&](const DataElement& element) {
		if (element.tag() == kFileMetaGroupLengthTag) {
			return;
		}
		write_non_sequence_element(measuring_writer, element.tag(), element.vr(),
		    element.value_span(), kMetaEncoding);
	});
	return checked_u32(measuring_writer.written, "file meta group length");
}

void set_dataelement_uid(DataSet& dataset, Tag tag, std::string_view value) {
	DataElement* element = dataset.add_dataelement(tag, VR::UI);
	if (!element || !(*element)) {
		diag::error_and_throw("rebuild_file_meta reason=failed to add UID element tag={}",
		    tag.to_string());
	}
	if (!element->from_uid_string(value)) {
		diag::error_and_throw(
		    "rebuild_file_meta reason=invalid UID tag={} value={}", tag.to_string(), value);
	}
}

void clear_existing_meta_group(DataSet& dataset) {
	std::vector<Tag> tags;
	for (const auto& element : dataset) {
		if (element.tag().group() == 0x0002u) {
			tags.push_back(element.tag());
		}
	}
	for (Tag tag : tags) {
		dataset.remove_dataelement(tag);
	}
}

[[nodiscard]] std::string determine_transfer_syntax_uid_for_rebuild(
    const DicomFile& file, const DataSet& dataset) {
	if (auto from_meta = read_uid_element(dataset, kTransferSyntaxUidTag)) {
		const auto normalized = uid::normalize_uid_text(*from_meta);
		if (uid::is_valid_uid_text_strict(normalized)) {
			return normalized;
		}
	}
	auto inferred = uid::normalize_uid_text(infer_transfer_syntax_uid(file, dataset));
	if (uid::is_valid_uid_text_strict(inferred)) {
		return inferred;
	}
	return std::string("ExplicitVRLittleEndian"_uid.value());
}

template <typename Writer>
void write_file_meta_group(Writer& writer, const DataSet& dataset) {
	const auto meta_group_length = measure_meta_group_length(dataset);
	std::array<std::uint8_t, 4> meta_group_length_bytes{};
	endian::store_le<std::uint32_t>(meta_group_length_bytes.data(), meta_group_length);
	write_non_sequence_element(
	    writer, kFileMetaGroupLengthTag, VR::UL, meta_group_length_bytes, kMetaEncoding);

	for_each_file_meta_element(dataset, [&](const DataElement& element) {
		if (element.tag() == kFileMetaGroupLengthTag) {
			return;
		}
		write_non_sequence_element(
		    writer, element.tag(), element.vr(), element.value_span(), kMetaEncoding);
	});
}

template <typename Writer>
void write_preamble(Writer& writer) {
	static const std::array<std::uint8_t, 128> kPreamble{};
	static const std::array<std::uint8_t, 4> kMagic{
	    static_cast<std::uint8_t>('D'),
	    static_cast<std::uint8_t>('I'),
	    static_cast<std::uint8_t>('C'),
	    static_cast<std::uint8_t>('M')};
	writer.append(kPreamble.data(), kPreamble.size());
	writer.append(kMagic.data(), kMagic.size());
}

template <typename Writer>
void write_impl(DicomFile& file, Writer& writer, const WriteOptions& options) {
	if (!options.keep_existing_meta) {
		file.rebuild_file_meta();
	}

	const DataSet& dataset = file.dataset();
	dataset.ensure_loaded(Tag(0xFFFFu, 0xFFFFu));

	if (options.include_preamble) {
		write_preamble(writer);
	}

	if (options.write_file_meta) {
		write_file_meta_group(writer, dataset);
	}

	const Encoding dataset_encoding{dataset.is_little_endian(), dataset.is_explicit_vr()};
	write_dataset(dataset, writer, dataset_encoding, true);
}

}  // namespace

void DicomFile::rebuild_file_meta() {
	DataSet& dataset = this->dataset();
	dataset.ensure_loaded(Tag(0xFFFFu, 0xFFFFu));

	std::string sop_class_uid;
	if (auto value = read_uid_element(dataset, kSopClassUidTag)) {
		sop_class_uid = uid::normalize_uid_text(*value);
	} else if (auto value = read_uid_element(dataset, kMediaStorageSopClassUidTag)) {
		sop_class_uid = uid::normalize_uid_text(*value);
	} else {
		sop_class_uid = std::string("SecondaryCaptureImageStorage"_uid.value());
	}
	if (!uid::is_valid_uid_text_strict(sop_class_uid)) {
		sop_class_uid = std::string("SecondaryCaptureImageStorage"_uid.value());
	}

	std::string sop_instance_uid;
	if (auto value = read_uid_element(dataset, kSopInstanceUidTag)) {
		sop_instance_uid = uid::normalize_uid_text(*value);
	} else if (auto value = read_uid_element(dataset, kMediaStorageSopInstanceUidTag)) {
		sop_instance_uid = uid::normalize_uid_text(*value);
	} else {
		const auto generated = uid::generate_sop_instance_uid();
		const auto generated_value = generated.value();
		sop_instance_uid.assign(generated_value.data(), generated_value.size());
	}
	if (!uid::is_valid_uid_text_strict(sop_instance_uid)) {
		const auto generated = uid::generate_sop_instance_uid();
		const auto generated_value = generated.value();
		sop_instance_uid.assign(generated_value.data(), generated_value.size());
	}

	std::string transfer_syntax_uid =
	    determine_transfer_syntax_uid_for_rebuild(*this, dataset);
	if (transfer_syntax_uid == "DeflatedExplicitVRLittleEndian"_uid.value()) {
		// Writer does not emit deflated payloads; rebuild to uncompressed LE.
		transfer_syntax_uid = std::string("ExplicitVRLittleEndian"_uid.value());
	}

	clear_existing_meta_group(dataset);
	dataset.add_dataelement(kFileMetaGroupLengthTag, VR::UL);
	const std::array<std::uint8_t, 2> meta_version{{0x00u, 0x01u}};
	dataset.add_dataelement(kFileMetaInformationVersionTag, VR::OB)->set_value_bytes(meta_version);
	set_dataelement_uid(dataset, kMediaStorageSopClassUidTag, sop_class_uid);
	set_dataelement_uid(dataset, kMediaStorageSopInstanceUidTag, sop_instance_uid);
	set_dataelement_uid(dataset, kTransferSyntaxUidTag, transfer_syntax_uid);
	set_dataelement_uid(dataset, kImplementationClassUidTag, uid::implementation_class_uid());
	dataset.add_dataelement(kImplementationVersionNameTag, VR::SH)->from_string_view(
	    uid::implementation_version_name());

	const auto meta_group_length = measure_meta_group_length(dataset);
	dataset.get_dataelement(kFileMetaGroupLengthTag)->from_long(meta_group_length);
}

void DicomFile::write_to_stream(std::ostream& os, const WriteOptions& options) {
	StreamWriter writer(os);
	write_impl(*this, writer, options);
}

std::vector<std::uint8_t> DicomFile::write_bytes(const WriteOptions& options) {
	std::vector<std::uint8_t> output;
	std::size_t reserve_hint = 4096;
	if (!this->path().empty()) {
		reserve_hint = std::max(reserve_hint, this->stream().attached_size());
	}
	if (options.include_preamble) {
		reserve_hint += 132;
	}
	output.reserve(reserve_hint);

	VectorWriter writer(output);
	write_impl(*this, writer, options);
	return output;
}

void DicomFile::write_file(const std::string& path, const WriteOptions& options) {
	std::ofstream os(path, std::ios::binary | std::ios::trunc);
	if (!os) {
		diag::error_and_throw("write_file path={} reason=failed to open output file", path);
	}

	std::vector<char> file_buffer(1 << 20);
	os.rdbuf()->pubsetbuf(file_buffer.data(), static_cast<std::streamsize>(file_buffer.size()));

	this->write_to_stream(os, options);
	os.flush();
	if (!os) {
		diag::error_and_throw("write_file path={} reason=failed to flush output file", path);
	}
}

}  // namespace dicom
