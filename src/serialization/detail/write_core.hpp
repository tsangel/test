#pragma once

#include "dataset_deflate_codec.h"
#include "dataset_endian_converter.h"
#include "dicom.h"
#include "dicom_endian.h"
#include "diagnostics.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dicom::write_detail {
using namespace dicom::literals;

constexpr Tag kItemTag{0xFFFEu, 0xE000u};
constexpr Tag kItemDelimitationTag{0xFFFEu, 0xE00Du};
constexpr Tag kSequenceDelimitationTag{0xFFFEu, 0xE0DDu};
constexpr Tag kPixelDataTag{0x7FE0u, 0x0010u};
constexpr Tag kExtendedOffsetTableTag{0x7FE0u, 0x0001u};
constexpr Tag kExtendedOffsetTableLengthsTag{0x7FE0u, 0x0002u};

constexpr Tag kFileMetaGroupLengthTag{0x0002u, 0x0000u};
constexpr Tag kFileMetaInformationVersionTag{0x0002u, 0x0001u};
constexpr Tag kMediaStorageSopClassUidTag{0x0002u, 0x0002u};
constexpr Tag kMediaStorageSopInstanceUidTag{0x0002u, 0x0003u};
constexpr Tag kTransferSyntaxUidTag{0x0002u, 0x0010u};
constexpr Tag kImplementationClassUidTag{0x0002u, 0x0012u};
constexpr Tag kImplementationVersionNameTag{0x0002u, 0x0013u};

constexpr Tag kSpecificCharacterSetTag{0x0008u, 0x0005u};
constexpr Tag kSopClassUidTag{0x0008u, 0x0016u};
constexpr Tag kSopInstanceUidTag{0x0008u, 0x0018u};

struct DatasetEncoding {
	bool explicit_vr{true};
	bool convert_body_to_big_endian{false};
	bool deflate_body{false};
};

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

	[[nodiscard]] std::size_t position() const noexcept { return written; }

	[[nodiscard]] bool can_overwrite() { return os.tellp() != std::streampos(-1); }

	void overwrite(std::size_t position, std::span<const std::uint8_t> bytes) {
		if (bytes.empty()) {
			return;
		}
		const auto resume = os.tellp();
		if (resume == std::streampos(-1)) {
			diag::error_and_throw(
			    "write_with_transfer_syntax reason=output stream is not seekable for backpatch");
		}
		os.seekp(static_cast<std::streamoff>(position), std::ios::beg);
		if (!os) {
			diag::error_and_throw(
			    "write_with_transfer_syntax reason=failed to seek for backpatch");
		}
		os.write(reinterpret_cast<const char*>(bytes.data()),
		    static_cast<std::streamsize>(bytes.size()));
		if (!os) {
			diag::error_and_throw(
			    "write_with_transfer_syntax reason=failed to backpatch output bytes");
		}
		os.seekp(resume, std::ios::beg);
		if (!os) {
			diag::error_and_throw(
			    "write_with_transfer_syntax reason=failed to restore stream position after backpatch");
		}
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

	[[nodiscard]] std::size_t position() const noexcept { return written; }

	[[nodiscard]] bool can_overwrite() const noexcept { return true; }

	void overwrite(std::size_t position, std::span<const std::uint8_t> data) {
		if (data.empty()) {
			return;
		}
		if (position > bytes.size() || data.size() > bytes.size() - position) {
			diag::error_and_throw(
			    "write_with_transfer_syntax reason=buffer backpatch out of bounds");
		}
		std::memcpy(bytes.data() + position, data.data(), data.size());
	}

	std::vector<std::uint8_t>& bytes;
	std::size_t written{0};
};

struct CountingWriter {
	void append(const void*, std::size_t size) { written += size; }
	void append_byte(std::uint8_t) { ++written; }
	std::size_t written{0};
};

enum class StreamingWriteEncodeMode : std::uint8_t {
	use_plugin_defaults = 0,
	use_explicit_options,
	use_encoder_context,
};

[[nodiscard]] inline std::uint32_t checked_u32(std::size_t value, std::string_view label) {
	if (value > std::numeric_limits<std::uint32_t>::max()) {
		diag::error_and_throw("write_to_stream reason={} exceeds 32-bit range", label);
	}
	return static_cast<std::uint32_t>(value);
}

[[nodiscard]] inline VR native_pixel_vr_from_bits_allocated_for_write(
    int bits_allocated) noexcept {
	return bits_allocated > 8 ? VR::OW : VR::OB;
}

[[nodiscard]] inline char ascii_upper_for_write(char value) noexcept {
	return (value >= 'a' && value <= 'z')
	           ? static_cast<char>(value - ('a' - 'A'))
	           : value;
}

[[nodiscard]] inline bool ascii_iequals_for_write(
    std::string_view lhs, std::string_view rhs) noexcept {
	if (lhs.size() != rhs.size()) {
		return false;
	}
	for (std::size_t index = 0; index < lhs.size(); ++index) {
		if (ascii_upper_for_write(lhs[index]) != ascii_upper_for_write(rhs[index])) {
			return false;
		}
	}
	return true;
}

template <typename Writer>
void write_u16(Writer& writer, std::uint16_t value) {
	std::array<std::uint8_t, 2> bytes{};
	endian::store_le<std::uint16_t>(bytes.data(), value);
	writer.append(bytes.data(), bytes.size());
}

template <typename Writer>
void write_u32(Writer& writer, std::uint32_t value) {
	std::array<std::uint8_t, 4> bytes{};
	endian::store_le<std::uint32_t>(bytes.data(), value);
	writer.append(bytes.data(), bytes.size());
}

template <typename Writer>
void write_tag(Writer& writer, Tag tag) {
	write_u16(writer, tag.group());
	write_u16(writer, tag.element());
}

[[nodiscard]] inline VR normalize_vr_for_write(Tag tag, VR vr) {
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
void write_data_element(const DataElement& element, Writer& writer, bool explicit_vr);

template <typename Writer>
void write_element_header(Writer& writer, Tag tag, VR vr, std::uint32_t value_length,
    bool undefined_length, bool explicit_vr) {
	write_tag(writer, tag);

	if (!explicit_vr) {
		write_u32(writer, undefined_length ? 0xFFFFFFFFu : value_length);
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
		write_u16(writer, static_cast<std::uint16_t>(value_length));
		return;
	}

	write_u16(writer, 0u);
	write_u32(writer, undefined_length ? 0xFFFFFFFFu : value_length);
}

template <typename Writer>
void write_item_header(Writer& writer, Tag tag, std::uint32_t value_length) {
	write_tag(writer, tag);
	write_u32(writer, value_length);
}

[[nodiscard]] inline std::size_t padded_length(std::size_t raw_length) {
	return raw_length + (raw_length & 1u);
}

struct PixelSequenceOffsetTables {
	std::vector<std::uint32_t> basic_offsets{};
	std::vector<std::uint64_t> extended_offsets{};
	std::vector<std::uint64_t> extended_lengths{};
	bool use_extended{false};
};

[[nodiscard]] inline std::vector<std::uint8_t> pack_u64_values(
    const std::vector<std::uint64_t>& values) {
	std::vector<std::uint8_t> bytes{};
	if (values.empty()) {
		return bytes;
	}
	const auto total_bytes = values.size() * sizeof(std::uint64_t);
	bytes.reserve(total_bytes);
	for (const auto value : values) {
		const auto start = bytes.size();
		bytes.resize(start + sizeof(std::uint64_t));
		endian::store_le<std::uint64_t>(bytes.data() + start, value);
	}
	return bytes;
}

[[nodiscard]] inline PixelSequenceOffsetTables compute_pixel_sequence_offset_tables(
    const PixelSequence& pixel_sequence, const InStream* seq_stream) {
	constexpr std::size_t kItemHeaderBytes = 8u;
	PixelSequenceOffsetTables tables{};
	std::size_t next_frame_offset = 0u;
	bool basic_offset_table_overflow = false;
	bool eot_eligible = true;

	for (std::size_t frame_index = 0; frame_index < pixel_sequence.number_of_frames();
	     ++frame_index) {
		const PixelFrame* frame = pixel_sequence.frame(frame_index);
		if (!frame) {
			continue;
		}

		std::size_t frame_fragment_count = 0;
		std::uint64_t frame_offset = 0;
		std::uint64_t frame_length = 0;

		const auto append_fragment = [&](std::size_t fragment_length) {
			if (frame_fragment_count == 0) {
				frame_offset = static_cast<std::uint64_t>(next_frame_offset);
				if (!basic_offset_table_overflow) {
					if (next_frame_offset > std::numeric_limits<std::uint32_t>::max()) {
						basic_offset_table_overflow = true;
						tables.basic_offsets.clear();
					} else {
						tables.basic_offsets.push_back(
						    static_cast<std::uint32_t>(next_frame_offset));
					}
				}
			}
			++frame_fragment_count;
			if (frame_fragment_count > 1) {
				eot_eligible = false;
			}
			if (fragment_length >
			    std::numeric_limits<std::uint64_t>::max() - frame_length) {
				diag::error_and_throw(
				    "write_to_stream reason=encapsulated frame length exceeds uint64 range");
			}
			frame_length += static_cast<std::uint64_t>(fragment_length);

			const auto full_fragment_length = padded_length(fragment_length);
			if (next_frame_offset > std::numeric_limits<std::size_t>::max() - kItemHeaderBytes ||
			    full_fragment_length >
			        std::numeric_limits<std::size_t>::max() - next_frame_offset -
			            kItemHeaderBytes) {
				diag::error_and_throw(
				    "write_to_stream reason=encapsulated pixel frame size exceeds size_t range");
			}
			next_frame_offset += kItemHeaderBytes + full_fragment_length;
		};

		const auto encoded_data = frame->encoded_data_view();
		if (!encoded_data.empty()) {
			append_fragment(encoded_data.size());
		} else {
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
				append_fragment(fragment.length);
			}
		}

		if (frame_fragment_count == 0) {
			eot_eligible = false;
			continue;
		}
		tables.extended_offsets.push_back(frame_offset);
		tables.extended_lengths.push_back(frame_length);
	}

	if (basic_offset_table_overflow && eot_eligible && !tables.extended_offsets.empty() &&
	    tables.extended_offsets.size() == tables.extended_lengths.size()) {
		tables.use_extended = true;
		tables.basic_offsets.clear();
	} else {
		tables.extended_offsets.clear();
		tables.extended_lengths.clear();
	}
	return tables;
}

template <typename Writer>
void append_zero_bytes(Writer& writer, std::size_t count) {
	static constexpr std::array<std::uint8_t, 4096> kZeroChunk{};
	while (count != 0) {
		const auto chunk = std::min(count, kZeroChunk.size());
		writer.append(kZeroChunk.data(), chunk);
		count -= chunk;
	}
}

template <typename Writer>
void write_non_sequence_element(Writer& writer, Tag tag, VR vr,
    std::span<const std::uint8_t> value, bool explicit_vr) {
	const auto normalized_vr = normalize_vr_for_write(tag, vr);
	const auto raw_length = value.size();
	const auto full_length = padded_length(raw_length);
	write_element_header(
	    writer, tag, normalized_vr, checked_u32(full_length, "element length"), false,
	    explicit_vr);
	if (!value.empty()) {
		writer.append(value.data(), value.size());
	}
	if ((raw_length & 1u) != 0u) {
		writer.append_byte(normalized_vr.padding_byte());
	}
}

template <typename Writer>
void write_dataset(
    const DataSet& dataset, Writer& writer, bool explicit_vr, bool skip_group_0002);

template <typename Writer>
void write_sequence_element(const DataElement& element, Writer& writer, bool explicit_vr) {
	const Sequence* sequence = element.as_sequence();
	if (!sequence) {
		diag::error_and_throw("write_to_stream reason=SQ element has null sequence pointer");
	}

	write_element_header(writer, element.tag(), VR::SQ, 0xFFFFFFFFu, true, explicit_vr);
	for (const auto& item_dataset_ptr : *sequence) {
		if (!item_dataset_ptr) {
			continue;
		}
		write_item_header(writer, kItemTag, 0xFFFFFFFFu);
		write_dataset(*item_dataset_ptr, writer, explicit_vr, false);
		write_item_header(writer, kItemDelimitationTag, 0u);
	}
	write_item_header(writer, kSequenceDelimitationTag, 0u);
}

template <typename Writer>
void write_pixel_sequence_element(const DataElement& element, Writer& writer,
    bool explicit_vr) {
	const PixelSequence* pixel_sequence = element.as_pixel_sequence();
	if (!pixel_sequence) {
		diag::error_and_throw("write_to_stream reason=PX element has null pixel sequence pointer");
	}

	const InStream* seq_stream = pixel_sequence->stream();
	const auto offset_tables = compute_pixel_sequence_offset_tables(*pixel_sequence, seq_stream);
	if (offset_tables.use_extended) {
		const auto extended_offsets_bytes = pack_u64_values(offset_tables.extended_offsets);
		const auto extended_lengths_bytes = pack_u64_values(offset_tables.extended_lengths);
		write_non_sequence_element(
		    writer, kExtendedOffsetTableTag, VR::OV, extended_offsets_bytes, explicit_vr);
		write_non_sequence_element(writer, kExtendedOffsetTableLengthsTag, VR::OV,
		    extended_lengths_bytes, explicit_vr);
	}

	const VR pixel_vr = explicit_vr ? VR::OB : VR::None;
	write_element_header(writer, element.tag(), pixel_vr, 0xFFFFFFFFu, true, explicit_vr);

	const auto basic_offset_table_length =
	    checked_u32(offset_tables.basic_offsets.size() * sizeof(std::uint32_t),
	        "basic offset table length");
	write_item_header(writer, kItemTag, basic_offset_table_length);
	for (const auto offset : offset_tables.basic_offsets) {
		write_u32(writer, offset);
	}

	const auto write_fragment = [&](std::span<const std::uint8_t> fragment) {
		const auto full_length = padded_length(fragment.size());
		write_item_header(
		    writer, kItemTag, checked_u32(full_length, "pixel fragment length"));
		if (!fragment.empty()) {
			writer.append(fragment.data(), fragment.size());
		}
		if ((fragment.size() & 1u) != 0u) {
			writer.append_byte(0x00u);
		}
	};

	for (std::size_t frame_index = 0; frame_index < pixel_sequence->number_of_frames();
	     ++frame_index) {
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

	write_item_header(writer, kSequenceDelimitationTag, 0u);
}

template <typename Writer>
void write_data_element(const DataElement& element, Writer& writer, bool explicit_vr) {
	if (element.vr().is_sequence()) {
		write_sequence_element(element, writer, explicit_vr);
		return;
	}
	if (element.vr().is_pixel_sequence()) {
		write_pixel_sequence_element(element, writer, explicit_vr);
		return;
	}

	write_non_sequence_element(
	    writer, element.tag(), element.vr(), element.value_span(), explicit_vr);
}

template <typename Writer>
void write_dataset(
    const DataSet& dataset, Writer& writer, bool explicit_vr, bool skip_group_0002) {
	const auto& pixel_data_element = dataset.get_dataelement(kPixelDataTag);
	const bool has_encapsulated_pixel_data =
	    !pixel_data_element.is_missing() && pixel_data_element.vr().is_pixel_sequence();
	for (const auto& element : dataset) {
		if (skip_group_0002 && element.tag().group() == 0x0002u) {
			continue;
		}
		if (has_encapsulated_pixel_data &&
		    (element.tag() == kExtendedOffsetTableTag ||
		        element.tag() == kExtendedOffsetTableLengthsTag)) {
			continue;
		}
		write_data_element(element, writer, explicit_vr);
	}
}

template <typename Writer, typename PixelWriter>
void write_dataset_with_pixel_writer(const DataSet& dataset, Writer& writer,
    bool explicit_vr, bool skip_group_0002, PixelWriter&& pixel_writer) {
	for (const auto& element : dataset) {
		if (skip_group_0002 && element.tag().group() == 0x0002u) {
			continue;
		}
		if (element.tag() == kExtendedOffsetTableTag ||
		    element.tag() == kExtendedOffsetTableLengthsTag) {
			continue;
		}
		if (element.tag() == kPixelDataTag) {
			pixel_writer(element, writer, explicit_vr);
			continue;
		}
		write_data_element(element, writer, explicit_vr);
	}
}

[[nodiscard]] inline std::string infer_transfer_syntax_uid(
    const DicomFile& file, const DataSet& dataset) {
	if (const auto file_ts = file.transfer_syntax_uid(); file_ts.valid()) {
		const auto value = file_ts.value();
		if (!value.empty()) {
			return std::string(value.data(), value.size());
		}
	}

	if (auto from_meta = dataset[kTransferSyntaxUidTag].to_uid_string();
	    from_meta && !from_meta->empty()) {
		return *from_meta;
	}

	const auto fallback = dataset.is_explicit_vr() ? "ExplicitVRLittleEndian"_uid.value()
	                                               : "ImplicitVRLittleEndian"_uid.value();
	return std::string(fallback.data(), fallback.size());
}

[[nodiscard]] inline bool transfer_syntax_uses_explicit_vr(
    uid::WellKnown transfer_syntax) noexcept {
	if (!transfer_syntax.valid()) {
		return true;
	}
	return transfer_syntax != "ImplicitVRLittleEndian"_uid &&
	    transfer_syntax != "Papyrus3ImplicitVRLittleEndian"_uid;
}

[[nodiscard]] inline uid::WellKnown determine_target_transfer_syntax(
    const DicomFile& file, const DataSet& dataset, const WriteOptions& options) {
	if (options.write_file_meta) {
		if (auto from_meta = dataset[kTransferSyntaxUidTag].to_transfer_syntax_uid()) {
			return *from_meta;
		}
	}

	if (const auto from_file = file.transfer_syntax_uid(); from_file.valid()) {
		return from_file;
	}

	return dataset.is_explicit_vr() ? "ExplicitVRLittleEndian"_uid
	                                : "ImplicitVRLittleEndian"_uid;
}

[[nodiscard]] inline DatasetEncoding determine_dataset_encoding(
    uid::WellKnown transfer_syntax, const DataSet& dataset) {
	DatasetEncoding encoding{};
	if (!transfer_syntax.valid()) {
		encoding.explicit_vr = dataset.is_explicit_vr();
		return encoding;
	}

	encoding.explicit_vr = transfer_syntax_uses_explicit_vr(transfer_syntax);
	if (transfer_syntax == "ExplicitVRBigEndian"_uid) {
		encoding.explicit_vr = true;
		encoding.convert_body_to_big_endian = true;
	} else if (transfer_syntax == "DeflatedExplicitVRLittleEndian"_uid) {
		encoding.explicit_vr = true;
		encoding.deflate_body = true;
	}
	return encoding;
}

[[nodiscard]] inline std::optional<pixel::Photometric>
parse_photometric_from_text_for_write(std::string_view text) noexcept {
	if (ascii_iequals_for_write(text, "MONOCHROME1")) {
		return pixel::Photometric::monochrome1;
	}
	if (ascii_iequals_for_write(text, "MONOCHROME2")) {
		return pixel::Photometric::monochrome2;
	}
	if (ascii_iequals_for_write(text, "RGB")) {
		return pixel::Photometric::rgb;
	}
	if (ascii_iequals_for_write(text, "YBR_FULL")) {
		return pixel::Photometric::ybr_full;
	}
	if (ascii_iequals_for_write(text, "YBR_FULL_422")) {
		return pixel::Photometric::ybr_full_422;
	}
	if (ascii_iequals_for_write(text, "YBR_RCT")) {
		return pixel::Photometric::ybr_rct;
	}
	if (ascii_iequals_for_write(text, "YBR_ICT")) {
		return pixel::Photometric::ybr_ict;
	}
	return std::nullopt;
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

[[nodiscard]] inline std::uint32_t measure_meta_group_length(const DataSet& dataset) {
	CountingWriter measuring_writer;
	for_each_file_meta_element(dataset, [&](const DataElement& element) {
		if (element.tag() == kFileMetaGroupLengthTag) {
			return;
		}
		write_non_sequence_element(
		    measuring_writer, element.tag(), element.vr(), element.value_span(), true);
	});
	return checked_u32(measuring_writer.written, "file meta group length");
}

inline void set_dataelement_uid(DataSet& dataset, Tag tag, std::string_view value) {
	DataElement& element = dataset.add_dataelement(tag, VR::UI);
	if (!element.from_uid_string(value)) {
		diag::error_and_throw(
		    "rebuild_file_meta reason=invalid UID tag={} value={}", tag.to_string(), value);
	}
}

inline void clear_existing_meta_group(DataSet& dataset) {
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

[[nodiscard]] inline std::string determine_transfer_syntax_uid_for_rebuild(
    const DicomFile& file, const DataSet& dataset) {
	if (auto from_meta = dataset[kTransferSyntaxUidTag].to_uid_string();
	    from_meta && !from_meta->empty()) {
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
	    writer, kFileMetaGroupLengthTag, VR::UL, meta_group_length_bytes, true);

	for_each_file_meta_element(dataset, [&](const DataElement& element) {
		if (element.tag() == kFileMetaGroupLengthTag) {
			return;
		}
		write_non_sequence_element(
		    writer, element.tag(), element.vr(), element.value_span(), true);
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
void write_dataset_body(Writer& writer, const DataSet& dataset,
    const DatasetEncoding& encoding, const std::string& file_path) {
	std::vector<std::uint8_t> body;
	body.reserve(4096);
	VectorWriter body_writer(body);
	write_dataset(dataset, body_writer, encoding.explicit_vr, true);

	if (encoding.convert_body_to_big_endian && encoding.deflate_body) {
		diag::error_and_throw(
		    "write_to_stream reason=unsupported encoding pipeline: both big-endian conversion and deflate requested");
	}

	if (encoding.convert_body_to_big_endian) {
		body = convert_little_endian_dataset_to_big_endian(body, 0, file_path);
	}
	if (encoding.deflate_body) {
		body = deflate_dataset_body(body, file_path);
	}

	if (!body.empty()) {
		writer.append(body.data(), body.size());
	}
}

template <typename Writer>
void write_pixel_fragment(Writer& writer, std::span<const std::uint8_t> fragment) {
	const auto full_length = padded_length(fragment.size());
	write_item_header(
	    writer, kItemTag, checked_u32(full_length, "pixel fragment length"));
	if (!fragment.empty()) {
		writer.append(fragment.data(), fragment.size());
	}
	if ((fragment.size() & 1u) != 0u) {
		writer.append_byte(0x00u);
	}
}

template <typename Writer, typename PixelWriter>
void write_dataset_body_with_pixel_writer(Writer& writer, const DataSet& dataset,
    const DatasetEncoding& encoding, const std::string& file_path,
    PixelWriter&& pixel_writer) {
	if (encoding.convert_body_to_big_endian && encoding.deflate_body) {
		diag::error_and_throw(
		    "write_to_stream reason=unsupported encoding pipeline: both big-endian conversion and deflate requested");
	}

	if (!encoding.convert_body_to_big_endian && !encoding.deflate_body) {
		write_dataset_with_pixel_writer(
		    dataset, writer, encoding.explicit_vr, true,
		    std::forward<PixelWriter>(pixel_writer));
		return;
	}

	std::vector<std::uint8_t> body;
	body.reserve(4096);
	VectorWriter body_writer(body);
	write_dataset_with_pixel_writer(dataset, body_writer, encoding.explicit_vr, true,
	    std::forward<PixelWriter>(pixel_writer));

	if (encoding.convert_body_to_big_endian) {
		body = convert_little_endian_dataset_to_big_endian(body, 0, file_path);
	}
	if (encoding.deflate_body) {
		body = deflate_dataset_body(body, file_path);
	}
	if (!body.empty()) {
		writer.append(body.data(), body.size());
	}
}

template <typename Writer, typename FrameProvider>
void write_native_pixel_data_from_frame_provider(Writer& writer,
    const DataElement& element, bool explicit_vr, VR native_pixel_vr,
    std::size_t total_payload_bytes, std::size_t frame_count,
    std::size_t frame_payload_bytes, FrameProvider&& frame_provider) {
	const auto full_length = padded_length(total_payload_bytes);
	write_element_header(writer, element.tag(), explicit_vr ? native_pixel_vr : VR::None,
	    checked_u32(full_length, "native PixelData length"), false, explicit_vr);

	std::size_t bytes_written = 0;
	for (std::size_t frame_index = 0; frame_index < frame_count; ++frame_index) {
		const auto frame_bytes = frame_provider(frame_index);
		if (frame_bytes.size() != frame_payload_bytes) {
			diag::error_and_throw(
			    "write_with_transfer_syntax reason=native output frame size mismatch frame={} expected={} actual={}",
			    frame_index, frame_payload_bytes, frame_bytes.size());
		}
		writer.append(frame_bytes.data(), frame_bytes.size());
		if (bytes_written > std::numeric_limits<std::size_t>::max() - frame_bytes.size()) {
			diag::error_and_throw(
			    "write_with_transfer_syntax reason=native PixelData byte count overflow");
		}
		bytes_written += frame_bytes.size();
	}

	if (bytes_written != total_payload_bytes) {
		diag::error_and_throw(
		    "write_with_transfer_syntax reason=native PixelData length mismatch expected={} actual={}",
		    total_payload_bytes, bytes_written);
	}
	if ((total_payload_bytes & 1u) != 0u) {
		writer.append_byte(native_pixel_vr.padding_byte());
	}
}

}  // namespace dicom::write_detail
