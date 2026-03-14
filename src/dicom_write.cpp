#include "dicom.h"
#include "dataset_endian_converter.h"
#include "dicom_endian.h"
#include "dataset_deflate_codec.h"
#include "diagnostics.h"
#include "pixel/host/decode/decode_frame_dispatch.hpp"
#include "pixel/host/adapter/host_adapter_v2.hpp"
#include "pixel/host/encode/encode_metadata_updater.hpp"
#include "pixel/host/encode/encode_set_pixel_data_runner.hpp"
#include "pixel/host/encode/encode_target_policy.hpp"
#include "pixel/host/encode/multicomponent_transform_policy.hpp"
#include "pixel/host/support/dicom_pixel_support.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <cstdint>
#include <fstream>
#include <functional>
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

constexpr std::array<Tag, 7> kStandardFileMetaTags{
    kFileMetaGroupLengthTag,
    kFileMetaInformationVersionTag,
    kMediaStorageSopClassUidTag,
    kMediaStorageSopInstanceUidTag,
    kTransferSyntaxUidTag,
    kImplementationClassUidTag,
    kImplementationVersionNameTag,
};

constexpr std::array<Tag, 14> kStreamingWritePixelMetadataTags{
    kTransferSyntaxUidTag,
    "Rows"_tag,
    "Columns"_tag,
    "SamplesPerPixel"_tag,
    "BitsAllocated"_tag,
    "BitsStored"_tag,
    "HighBit"_tag,
    "PixelRepresentation"_tag,
    "PhotometricInterpretation"_tag,
    "NumberOfFrames"_tag,
    "PlanarConfiguration"_tag,
    "LossyImageCompression"_tag,
    "LossyImageCompressionRatio"_tag,
    "LossyImageCompressionMethod"_tag,
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

	[[nodiscard]] bool can_overwrite() {
		return os.tellp() != std::streampos(-1);
	}

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

struct SavedElementState {
	Tag tag{};
	bool is_present{false};
	VR vr{VR::None};
	std::vector<std::uint8_t> value_bytes{};
};

class ScopedElementStateRestore {
public:
	ScopedElementStateRestore(DicomFile& file, std::initializer_list<Tag> tags)
	    : ScopedElementStateRestore(
	          file, std::span<const Tag>(tags.begin(), tags.size())) {}

	ScopedElementStateRestore(DicomFile& file, std::span<const Tag> tags)
	    : file_(file) {
		saved_.reserve(tags.size());
		for (const auto tag : tags) {
			const auto& element = file_.dataset()[tag];
			SavedElementState state{};
			state.tag = tag;
			state.is_present = !element.is_missing();
			if (state.is_present) {
				state.vr = element.vr();
				const auto value = element.value_span();
				state.value_bytes.assign(value.begin(), value.end());
			}
			saved_.push_back(std::move(state));
		}
	}

	ScopedElementStateRestore(const ScopedElementStateRestore&) = delete;
	ScopedElementStateRestore& operator=(const ScopedElementStateRestore&) = delete;

	~ScopedElementStateRestore() {
		if (!active_) {
			return;
		}
		try {
			restore();
		} catch (...) {
		}
	}

	void restore() {
		if (!active_) {
			return;
		}
		for (const auto& state : saved_) {
			if (!state.is_present) {
				file_.remove_dataelement(state.tag);
				continue;
			}
			auto& element = file_.add_dataelement(state.tag, state.vr);
			element.set_value_bytes(std::span<const std::uint8_t>(
			    state.value_bytes.data(), state.value_bytes.size()));
		}
		active_ = false;
	}

private:
	DicomFile& file_;
	std::vector<SavedElementState> saved_{};
	bool active_{true};
};

[[nodiscard]] std::uint32_t checked_u32(std::size_t value, std::string_view label) {
	if (value > std::numeric_limits<std::uint32_t>::max()) {
		diag::error_and_throw("write_to_stream reason={} exceeds 32-bit range", label);
	}
	return static_cast<std::uint32_t>(value);
}

[[nodiscard]] uint32_t encode_codec_profile_code_from_transfer_syntax_or_throw(
    std::string_view file_path, uid::WellKnown transfer_syntax) {
	uint32_t codec_profile_code = PIXEL_CODEC_PROFILE_UNKNOWN_V2;
	if (::pixel::runtime_v2::codec_profile_code_from_transfer_syntax(
	        transfer_syntax, &codec_profile_code)) {
		return codec_profile_code;
	}
	diag::error_and_throw(
	    "write_with_transfer_syntax file={} ts={} reason=transfer syntax is not mapped to a runtime codec profile",
	    file_path, transfer_syntax.value());
}

[[nodiscard]] VR native_pixel_vr_from_bits_allocated_for_write(int bits_allocated) noexcept {
	return bits_allocated > 8 ? VR::OW : VR::OB;
}

[[nodiscard]] char ascii_upper_for_write(char value) noexcept {
	return (value >= 'a' && value <= 'z')
	           ? static_cast<char>(value - ('a' - 'A'))
	           : value;
}

[[nodiscard]] bool ascii_iequals_for_write(
    std::string_view lhs, std::string_view rhs) noexcept {
	if (lhs.size() != rhs.size()) {
		return false;
	}
	for (std::size_t index = 0; index < lhs.size(); ++index) {
		if (ascii_upper_for_write(lhs[index]) !=
		    ascii_upper_for_write(rhs[index])) {
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

[[nodiscard]] std::size_t padded_length(std::size_t raw_length) {
	return raw_length + (raw_length & 1u);
}

struct PixelSequenceOffsetTables {
	std::vector<std::uint32_t> basic_offsets{};
	std::vector<std::uint64_t> extended_offsets{};
	std::vector<std::uint64_t> extended_lengths{};
	bool use_extended{false};
};

[[nodiscard]] std::vector<std::uint8_t> pack_u64_values(
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

[[nodiscard]] PixelSequenceOffsetTables compute_pixel_sequence_offset_tables(
    const PixelSequence& pixel_sequence, const InStream* seq_stream) {
	constexpr std::size_t kItemHeaderBytes = 8u;
	PixelSequenceOffsetTables tables{};
	std::size_t next_frame_offset = 0u;
	bool basic_offset_table_overflow = false;
	bool eot_eligible = true;

	for (std::size_t frame_index = 0; frame_index < pixel_sequence.number_of_frames(); ++frame_index) {
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
						tables.basic_offsets.push_back(static_cast<std::uint32_t>(next_frame_offset));
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
			        std::numeric_limits<std::size_t>::max() - next_frame_offset - kItemHeaderBytes) {
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
void write_non_sequence_element(Writer& writer, Tag tag, VR vr, std::span<const std::uint8_t> value,
    bool explicit_vr) {
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

	write_element_header(
	    writer, element.tag(), VR::SQ, 0xFFFFFFFFu, true, explicit_vr);
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
		write_non_sequence_element(
		    writer, kExtendedOffsetTableLengthsTag, VR::OV, extended_lengths_bytes, explicit_vr);
	}

	const VR pixel_vr = explicit_vr ? VR::OB : VR::None;
	write_element_header(
	    writer, element.tag(), pixel_vr, 0xFFFFFFFFu, true, explicit_vr);

	const auto basic_offset_table_length = checked_u32(
	    offset_tables.basic_offsets.size() * sizeof(std::uint32_t), "basic offset table length");
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

	write_non_sequence_element(writer, element.tag(), element.vr(), element.value_span(), explicit_vr);
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
void write_dataset_with_pixel_writer(
    const DataSet& dataset, Writer& writer, bool explicit_vr,
    bool skip_group_0002, PixelWriter&& pixel_writer) {
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

[[nodiscard]] std::string infer_transfer_syntax_uid(const DicomFile& file, const DataSet& dataset) {
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

	const auto fallback = dataset.is_explicit_vr()
	    ? "ExplicitVRLittleEndian"_uid.value()
	    : "ImplicitVRLittleEndian"_uid.value();
	return std::string(fallback.data(), fallback.size());
}

[[nodiscard]] bool transfer_syntax_uses_explicit_vr(uid::WellKnown transfer_syntax) noexcept {
	if (!transfer_syntax.valid()) {
		return true;
	}
	return transfer_syntax != "ImplicitVRLittleEndian"_uid &&
	    transfer_syntax != "Papyrus3ImplicitVRLittleEndian"_uid;
}

[[nodiscard]] uid::WellKnown resolve_target_transfer_syntax(
    const DicomFile& file, const DataSet& dataset, const WriteOptions& options) {
	if (options.write_file_meta) {
		if (auto from_meta = dataset[kTransferSyntaxUidTag].to_transfer_syntax_uid()) {
			return *from_meta;
		}
	}

	if (const auto from_file = file.transfer_syntax_uid(); from_file.valid()) {
		return from_file;
	}

	return dataset.is_explicit_vr()
	    ? "ExplicitVRLittleEndian"_uid
	    : "ImplicitVRLittleEndian"_uid;
}

[[nodiscard]] DatasetEncoding determine_dataset_encoding(uid::WellKnown transfer_syntax,
    const DataSet& dataset) {
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

[[nodiscard]] std::optional<pixel::Photometric> parse_photometric_from_text_for_write(
    std::string_view text) noexcept {
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

[[nodiscard]] pixel::PixelSource build_set_pixel_source_from_decoded_native_frames_for_write(
    DicomFile& file, uid::WellKnown target_ts, const pixel::PixelDataInfo& info,
    const pixel::DecodePlan& decode_plan) {
	if (!info.has_pixel_data || info.rows <= 0 || info.cols <= 0 ||
	    info.frames <= 0 || info.samples_per_pixel <= 0) {
		diag::error_and_throw(
		    "write_with_transfer_syntax file={} target_ts={} reason=invalid decoded pixel metadata rows={} cols={} frames={} samples_per_pixel={}",
		    file.path(), target_ts.value(), info.rows, info.cols, info.frames,
		    info.samples_per_pixel);
	}
	if (decode_plan.strides.row == 0 || decode_plan.strides.frame == 0) {
		diag::error_and_throw(
		    "write_with_transfer_syntax file={} target_ts={} reason=decoded frame layout is empty",
		    file.path(), target_ts.value());
	}

	const auto photometric_text =
	    file.dataset()["PhotometricInterpretation"_tag].to_string_view();
	if (!photometric_text) {
		diag::error_and_throw(
		    "write_with_transfer_syntax file={} target_ts={} reason=missing or invalid PhotometricInterpretation",
		    file.path(), target_ts.value());
	}
	const auto photometric =
	    parse_photometric_from_text_for_write(*photometric_text);
	if (!photometric) {
		diag::error_and_throw(
		    "write_with_transfer_syntax file={} target_ts={} reason=unsupported PhotometricInterpretation='{}' for streaming write path",
		    file.path(), target_ts.value(), *photometric_text);
	}

	const auto parse_optional_int_tag =
	    [&](Tag tag, std::string_view name) -> std::optional<int> {
		const auto value = file.dataset()[tag].to_long();
		if (!value) {
			return std::nullopt;
		}
		if (*value < static_cast<long>(std::numeric_limits<int>::min()) ||
		    *value > static_cast<long>(std::numeric_limits<int>::max())) {
			diag::error_and_throw(
			    "write_with_transfer_syntax file={} target_ts={} reason={} value({}) is outside int range",
			    file.path(), target_ts.value(), name, *value);
		}
		return static_cast<int>(*value);
	};

	pixel::PixelSource source{};
	source.data_type = info.sv_dtype;
	source.rows = info.rows;
	source.cols = info.cols;
	source.frames = info.frames;
	source.samples_per_pixel = info.samples_per_pixel;
	source.planar = info.planar_configuration;
	source.row_stride = decode_plan.strides.row;
	source.frame_stride = decode_plan.strides.frame;
	source.photometric = *photometric;
	source.bits_stored =
	    parse_optional_int_tag("BitsStored"_tag, "BitsStored").value_or(info.bits_stored);
	return source;
}

[[nodiscard]] pixel::PixelSource build_set_pixel_source_from_native_pixel_data_for_write(
    DicomFile& file, uid::WellKnown target_ts) {
	DataSet& dataset = file.dataset();
	const auto& pixel_data = dataset["PixelData"_tag];
	if (!pixel_data || pixel_data.vr().is_pixel_sequence() ||
	    !pixel_data.vr().is_binary()) {
		diag::error_and_throw(
		    "write_with_transfer_syntax file={} target_ts={} reason=PixelData must be native binary for native source write path",
		    file.path(), target_ts.value());
	}

	const auto info = file.pixeldata_info();
	if (!info.has_pixel_data || info.rows <= 0 || info.cols <= 0 ||
	    info.frames <= 0 || info.samples_per_pixel <= 0) {
		diag::error_and_throw(
		    "write_with_transfer_syntax file={} target_ts={} reason=invalid native pixel metadata rows={} cols={} frames={} samples_per_pixel={}",
		    file.path(), target_ts.value(), info.rows, info.cols, info.frames,
		    info.samples_per_pixel);
	}

	const auto bytes_per_sample = bytes_per_sample_of(info.sv_dtype);
	if (bytes_per_sample == 0) {
		diag::error_and_throw(
		    "write_with_transfer_syntax file={} target_ts={} reason=failed to resolve bytes per sample for dtype={}",
		    file.path(), target_ts.value(), static_cast<int>(info.sv_dtype));
	}

	const auto rows = static_cast<std::size_t>(info.rows);
	const auto cols = static_cast<std::size_t>(info.cols);
	const auto frames = static_cast<std::size_t>(info.frames);
	const auto samples_per_pixel = static_cast<std::size_t>(info.samples_per_pixel);
	const bool source_is_planar =
	    info.planar_configuration == pixel::Planar::planar &&
	    samples_per_pixel > std::size_t{1};
	const std::uint64_t row_components_u64 =
	    source_is_planar ? static_cast<std::uint64_t>(cols)
	                     : static_cast<std::uint64_t>(cols) *
	                           static_cast<std::uint64_t>(samples_per_pixel);
	const std::uint64_t row_stride_u64 =
	    row_components_u64 * static_cast<std::uint64_t>(bytes_per_sample);
	const std::uint64_t plane_stride_u64 =
	    row_stride_u64 * static_cast<std::uint64_t>(rows);
	const std::uint64_t frame_stride_u64 = source_is_planar
	                                           ? plane_stride_u64 *
	                                                 static_cast<std::uint64_t>(samples_per_pixel)
	                                           : plane_stride_u64;
	if (frame_stride_u64 == 0) {
		diag::error_and_throw(
		    "write_with_transfer_syntax file={} target_ts={} reason=native source frame stride is zero",
		    file.path(), target_ts.value());
	}
	if (frame_stride_u64 >
	    static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
		diag::error_and_throw(
		    "write_with_transfer_syntax file={} target_ts={} reason=native source frame stride overflows size_t",
		    file.path(), target_ts.value());
	}
	const auto frame_stride = static_cast<std::size_t>(frame_stride_u64);
	if (frames > std::numeric_limits<std::size_t>::max() / frame_stride) {
		diag::error_and_throw(
		    "write_with_transfer_syntax file={} target_ts={} reason=native source total bytes overflow (frames={}, frame_stride={})",
		    file.path(), target_ts.value(), frames, frame_stride);
	}
	const auto required_bytes = frame_stride * frames;

	const auto source_bytes = pixel_data.value_span();
	if (source_bytes.size() < required_bytes) {
		diag::error_and_throw(
		    "write_with_transfer_syntax file={} target_ts={} reason=PixelData bytes({}) are shorter than required native frame payload({})",
		    file.path(), target_ts.value(), source_bytes.size(), required_bytes);
	}

	pixel::PixelSource source{};
	source.bytes = source_bytes.first(required_bytes);
	source.data_type = info.sv_dtype;
	source.rows = info.rows;
	source.cols = info.cols;
	source.row_stride = source_is_planar
	                        ? static_cast<std::size_t>(row_stride_u64)
	                        : static_cast<std::size_t>(row_stride_u64);
	source.frames = info.frames;
	source.frame_stride = frame_stride;
	source.samples_per_pixel = info.samples_per_pixel;
	source.planar = info.planar_configuration;
	source.bits_stored = info.bits_stored;

	const auto photometric_text =
	    dataset["PhotometricInterpretation"_tag].to_string_view();
	if (!photometric_text) {
		diag::error_and_throw(
		    "write_with_transfer_syntax file={} target_ts={} reason=missing or invalid PhotometricInterpretation for native source path",
		    file.path(), target_ts.value());
	}
	const auto photometric =
	    parse_photometric_from_text_for_write(*photometric_text);
	if (!photometric) {
		diag::error_and_throw(
		    "write_with_transfer_syntax file={} target_ts={} reason=unsupported PhotometricInterpretation='{}' for native source path",
		    file.path(), target_ts.value(), *photometric_text);
	}
	source.photometric = *photometric;
	return source;
}

[[nodiscard]] std::vector<Tag> collect_existing_meta_group_tags(
    const DataSet& dataset) {
	std::vector<Tag> tags;
	for (const auto& element : dataset) {
		if (element.tag().group() != 0x0002u) {
			if (element.tag().group() > 0x0002u) {
				break;
			}
			continue;
		}
		tags.push_back(element.tag());
	}
	return tags;
}

void append_standard_file_meta_tags(std::vector<Tag>& tags) {
	tags.insert(tags.end(), kStandardFileMetaTags.begin(), kStandardFileMetaTags.end());
}

void append_streaming_write_pixel_metadata_tags(std::vector<Tag>& tags) {
	tags.insert(tags.end(), kStreamingWritePixelMetadataTags.begin(),
	    kStreamingWritePixelMetadataTags.end());
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
		    element.value_span(), true);
	});
	return checked_u32(measuring_writer.written, "file meta group length");
}

void set_dataelement_uid(DataSet& dataset, Tag tag, std::string_view value) {
	DataElement& element = dataset.add_dataelement(tag, VR::UI);
	if (!element.from_uid_string(value)) {
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
void write_dataset_body_with_pixel_writer(
    Writer& writer, const DataSet& dataset, const DatasetEncoding& encoding,
    const std::string& file_path, PixelWriter&& pixel_writer) {
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
void write_native_pixel_data_from_frame_provider(
    Writer& writer, const DataElement& element, bool explicit_vr,
    VR native_pixel_vr, std::size_t total_payload_bytes,
    std::size_t frame_count, std::size_t frame_payload_bytes,
    FrameProvider&& frame_provider) {
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
		if (bytes_written >
		    std::numeric_limits<std::size_t>::max() - frame_bytes.size()) {
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

template <typename FrameProvider>
std::size_t measure_encoded_payload_bytes_from_frame_provider(
    DicomFile& file, uid::WellKnown transfer_syntax,
    const pixel::PixelSource& source_descriptor, uint32_t codec_profile_code,
    std::span<const pixel::CodecOptionKv> codec_options,
    bool use_multicomponent_transform, std::size_t frame_count,
    FrameProvider&& frame_provider) {
	std::size_t encoded_payload_bytes = 0;
	pixel::detail::encode_frames_from_frame_provider_with_runtime_or_throw(
	    file, transfer_syntax, source_descriptor, codec_profile_code, codec_options,
	    use_multicomponent_transform, frame_count,
	    std::forward<FrameProvider>(frame_provider),
	    [&](std::size_t, std::vector<std::uint8_t>&& encoded_frame) {
		    if (encoded_payload_bytes >
		        std::numeric_limits<std::size_t>::max() - encoded_frame.size()) {
			    diag::error_and_throw(
			        "write_with_transfer_syntax file={} target_ts={} reason=encoded payload size overflow during lossy prepass",
			        file.path(), transfer_syntax.value());
		    }
		    encoded_payload_bytes += encoded_frame.size();
	    });
	return encoded_payload_bytes;
}

template <typename Writer>
void write_current_dataset_direct(
    DicomFile& file, Writer& writer, uid::WellKnown transfer_syntax,
    const WriteOptions& options) {
	const auto& dataset = file.dataset();
	const auto encoding = determine_dataset_encoding(transfer_syntax, dataset);
	if (options.include_preamble) {
		write_preamble(writer);
	}
	if (options.write_file_meta) {
		write_file_meta_group(writer, dataset);
	}
	write_dataset_body_with_pixel_writer(
	    writer, dataset, encoding, file.path(),
	    [](const DataElement& element, auto& direct_writer, bool explicit_vr) {
		    write_data_element(element, direct_writer, explicit_vr);
	    });
}

template <typename Writer>
void write_with_transfer_syntax_impl(DicomFile& file, Writer& writer,
    uid::WellKnown target_transfer_syntax,
    StreamingWriteEncodeMode encode_mode,
    std::span<const pixel::CodecOptionTextKv> codec_opt_override,
    const pixel::EncoderContext* encoder_ctx,
    const WriteOptions& options) {
	if (!target_transfer_syntax.valid() ||
	    target_transfer_syntax.uid_type() != UidType::TransferSyntax) {
		diag::error_and_throw(
		    "write_with_transfer_syntax reason=uid must be a valid Transfer Syntax UID");
	}

	DataSet& dataset = file.dataset();
	dataset.ensure_loaded(Tag(0xFFFFu, 0xFFFFu));
	const auto source_transfer_syntax = file.transfer_syntax_uid();
	const auto& pixel_data = dataset["PixelData"_tag];
	const bool has_float_pixel_data =
	    dataset["FloatPixelData"_tag].is_present() ||
	    dataset["DoubleFloatPixelData"_tag].is_present();
	const bool same_transfer_syntax =
	    source_transfer_syntax.valid() &&
	    source_transfer_syntax == target_transfer_syntax;
	const bool has_native_pixel_data =
	    pixel_data && pixel_data.vr().is_binary() &&
	    !pixel_data.vr().is_pixel_sequence();
	const bool has_encapsulated_pixel_data =
	    pixel_data && pixel_data.vr().is_pixel_sequence();
	const bool target_is_encapsulated = target_transfer_syntax.is_encapsulated();
	const bool needs_native_to_encapsulated =
	    has_native_pixel_data && target_is_encapsulated;
	const bool needs_encapsulated_to_native =
	    has_encapsulated_pixel_data && !target_is_encapsulated;
	const bool needs_encapsulated_transcode =
	    has_encapsulated_pixel_data && target_is_encapsulated &&
	    !same_transfer_syntax;
	const bool needs_pixel_transcode =
	    needs_native_to_encapsulated || needs_encapsulated_to_native ||
	    needs_encapsulated_transcode;

	if (!needs_pixel_transcode && same_transfer_syntax && options.keep_existing_meta) {
		write_current_dataset_direct(
		    file, writer, target_transfer_syntax, options);
		return;
	}
	if (has_float_pixel_data && target_is_encapsulated) {
		diag::error_and_throw(
		    "write_with_transfer_syntax file={} target_ts={} reason=FloatPixelData/DoubleFloatPixelData cannot be written with encapsulated transfer syntaxes",
		    file.path(), target_transfer_syntax.value());
	}
	if ((needs_native_to_encapsulated || needs_encapsulated_transcode) &&
	    !target_transfer_syntax.supports_pixel_encode()) {
		diag::error_and_throw(
		    "write_with_transfer_syntax file={} source_ts={} target_ts={} reason=target transfer syntax does not support pixel encode",
		    file.path(), source_transfer_syntax.value(),
		    target_transfer_syntax.value());
	}

	pixel::PixelDataInfo info{};
	if (needs_pixel_transcode) {
		info = file.pixeldata_info();
		if (!info.has_pixel_data) {
			diag::error_and_throw(
			    "write_with_transfer_syntax file={} target_ts={} reason=PixelData metadata is not decodable for the requested transfer syntax conversion",
			    file.path(), target_transfer_syntax.value());
		}
	}

	std::vector<Tag> restore_tags;
	if (needs_pixel_transcode) {
		append_streaming_write_pixel_metadata_tags(restore_tags);
	} else {
		restore_tags.push_back(kTransferSyntaxUidTag);
	}
	if (!options.keep_existing_meta) {
		append_standard_file_meta_tags(restore_tags);
		const auto existing_meta_tags = collect_existing_meta_group_tags(dataset);
		restore_tags.insert(restore_tags.end(),
		    existing_meta_tags.begin(), existing_meta_tags.end());
	}
	ScopedElementStateRestore restore_guard(
	    file, std::span<const Tag>(restore_tags.data(), restore_tags.size()));

	try {
		if (!needs_pixel_transcode) {
			pixel::detail::update_transfer_syntax_uid_element_after_set_pixel_data_or_throw(
			    file, target_transfer_syntax);
			if (!options.keep_existing_meta) {
				file.rebuild_file_meta();
			}
			write_current_dataset_direct(file, writer, target_transfer_syntax, options);
			restore_guard.restore();
			return;
		}

		pixel::PixelSource source_descriptor{};
		std::optional<pixel::DecodePlan> decode_plan{};
		if (needs_native_to_encapsulated) {
			source_descriptor =
			    build_set_pixel_source_from_native_pixel_data_for_write(
			        file, target_transfer_syntax);
		} else {
			pixel::DecodeOptions decode_options{};
			decode_options.planar_out = info.planar_configuration;
			decode_options.alignment = 1;
			decode_options.to_modality_value = false;
			decode_options.decode_mct = false;
			decode_plan = file.create_decode_plan(decode_options);
			if (decode_plan->strides.frame == 0) {
				diag::error_and_throw(
				    "write_with_transfer_syntax file={} target_ts={} reason=calculated native frame size is zero for streaming write",
				    file.path(), target_transfer_syntax.value());
			}
			source_descriptor =
			    build_set_pixel_source_from_decoded_native_frames_for_write(
			        file, target_transfer_syntax, info, *decode_plan);
		}

		pixel::EncoderContext staged_encoder_ctx{};
		const pixel::EncoderContext* active_encoder_ctx = encoder_ctx;
		if (target_is_encapsulated) {
			if (encode_mode == StreamingWriteEncodeMode::use_plugin_defaults) {
				staged_encoder_ctx.configure(target_transfer_syntax);
				active_encoder_ctx = &staged_encoder_ctx;
			} else if (encode_mode == StreamingWriteEncodeMode::use_explicit_options) {
				staged_encoder_ctx.configure(target_transfer_syntax, codec_opt_override);
				active_encoder_ctx = &staged_encoder_ctx;
			}
			if (active_encoder_ctx == nullptr || !active_encoder_ctx->configured()) {
				diag::error_and_throw(
				    "write_with_transfer_syntax file={} target_ts={} reason=encoder context is not configured",
				    file.path(), target_transfer_syntax.value());
			}
			if (active_encoder_ctx->transfer_syntax_uid() != target_transfer_syntax) {
				diag::error_and_throw(
				    "write_with_transfer_syntax file={} target_ts={} ctx_ts={} reason=encoder context transfer syntax mismatch",
				    file.path(), target_transfer_syntax.value(),
				    active_encoder_ctx->transfer_syntax_uid().value());
			}
		}

		const auto codec_profile_code =
		    encode_codec_profile_code_from_transfer_syntax_or_throw(
		        file.path(), target_transfer_syntax);
		const auto source_layout =
		    pixel::support_detail::
		        compute_encode_source_layout_without_source_bytes_or_throw(
		            source_descriptor, file.path());
		pixel::detail::validate_encode_profile_source_constraints(
		    codec_profile_code, source_layout.bits_allocated,
		    source_layout.bits_stored, file.path());

		bool use_multicomponent_transform = false;
		pixel::Photometric output_photometric = source_descriptor.photometric;
		if (target_is_encapsulated) {
			use_multicomponent_transform =
			    pixel::detail::should_use_multicomponent_transform(
			        target_transfer_syntax, codec_profile_code,
			        active_encoder_ctx->codec_options(),
			        source_layout.samples_per_pixel, file.path());
			output_photometric =
			    pixel::detail::compute_output_photometric_for_encode_profile(
			        codec_profile_code, use_multicomponent_transform,
			        source_descriptor.photometric);
		}

		std::size_t encoded_payload_bytes = 0;
		std::vector<std::uint8_t> decoded_frame{};
		if (target_is_encapsulated &&
		    pixel::detail::encode_profile_uses_lossy_compression(codec_profile_code)) {
			if (needs_native_to_encapsulated) {
				const auto source_bytes = source_descriptor.bytes;
				encoded_payload_bytes =
				    measure_encoded_payload_bytes_from_frame_provider(
				        file, target_transfer_syntax, source_descriptor,
				        codec_profile_code, active_encoder_ctx->codec_options(),
				        use_multicomponent_transform, source_layout.frames,
				        [&](std::size_t frame_index) -> std::span<const std::uint8_t> {
					        const auto frame_offset =
					            frame_index * source_layout.source_frame_stride;
					        return source_bytes.subspan(
					            frame_offset, source_layout.source_frame_size_bytes);
				        });
			} else {
				decoded_frame.resize(decode_plan->strides.frame);
				encoded_payload_bytes =
				    measure_encoded_payload_bytes_from_frame_provider(
				        file, target_transfer_syntax, source_descriptor,
				        codec_profile_code, active_encoder_ctx->codec_options(),
				        use_multicomponent_transform, source_layout.frames,
				        [&](std::size_t frame_index) -> std::span<const std::uint8_t> {
					        auto frame_span = std::span<std::uint8_t>(
					            decoded_frame.data(), decoded_frame.size());
					        const auto prepared_source =
					            pixel::support_detail::prepare_decode_frame_source_or_throw(
					                file, info, frame_index);
					        pixel::detail::dispatch_decode_prepared_frame(
					            file.path(), frame_index, prepared_source.bytes,
					            frame_span, *decode_plan);
					        return std::span<const std::uint8_t>(
					            frame_span.data(),
					            source_layout.source_frame_size_bytes);
				        });
			}
		}

		pixel::detail::update_pixel_metadata_for_set_pixel_data(dataset, file.path(),
		    target_transfer_syntax, source_descriptor,
		    pixel::detail::is_rle_encode_profile(codec_profile_code),
		    output_photometric, source_layout.bits_allocated,
		    source_layout.bits_stored, source_layout.high_bit,
		    source_layout.pixel_representation, source_layout.source_row_stride,
		    source_layout.source_frame_stride);
		pixel::detail::update_lossy_compression_metadata_for_set_pixel_data(
		    dataset, file.path(), target_transfer_syntax, codec_profile_code,
		    source_layout.destination_total_bytes, encoded_payload_bytes);
		pixel::detail::update_transfer_syntax_uid_element_after_set_pixel_data_or_throw(
		    file, target_transfer_syntax);
		if (!options.keep_existing_meta) {
			file.rebuild_file_meta();
		}

		const auto encoding =
		    determine_dataset_encoding(target_transfer_syntax, dataset);
		if (options.include_preamble) {
			write_preamble(writer);
		}
		if (options.write_file_meta) {
			write_file_meta_group(writer, dataset);
		}

		if (target_is_encapsulated) {
			write_dataset_body_with_pixel_writer(
			    writer, dataset, encoding, file.path(),
			    [&](const DataElement& element, auto& direct_writer, bool explicit_vr) {
				    std::size_t extended_offset_table_value_offset = 0;
				    std::size_t extended_offset_table_lengths_value_offset = 0;
				    std::vector<std::uint64_t> extended_offsets{};
				    std::vector<std::uint64_t> extended_lengths{};
				    const bool write_extended_offset_table =
				        direct_writer.can_overwrite() &&
				        source_layout.frames != 0;
				    if (write_extended_offset_table) {
					    if (source_layout.frames >
					        std::numeric_limits<std::size_t>::max() /
					            sizeof(std::uint64_t)) {
						    diag::error_and_throw(
						        "write_with_transfer_syntax file={} target_ts={} reason=ExtendedOffsetTable size overflow",
						        file.path(), target_transfer_syntax.value());
					    }
					    const auto extended_value_length =
					        source_layout.frames * sizeof(std::uint64_t);
					    write_element_header(direct_writer, kExtendedOffsetTableTag,
					        explicit_vr ? VR::OV : VR::None,
					        checked_u32(extended_value_length,
					            "ExtendedOffsetTable length"),
					        false, explicit_vr);
					    extended_offset_table_value_offset =
					        direct_writer.position();
					    append_zero_bytes(direct_writer, extended_value_length);

					    write_element_header(direct_writer,
					        kExtendedOffsetTableLengthsTag,
					        explicit_vr ? VR::OV : VR::None,
					        checked_u32(extended_value_length,
					            "ExtendedOffsetTableLengths length"),
					        false, explicit_vr);
					    extended_offset_table_lengths_value_offset =
					        direct_writer.position();
					    append_zero_bytes(direct_writer, extended_value_length);

					    extended_offsets.reserve(source_layout.frames);
					    extended_lengths.reserve(source_layout.frames);
				    }

				    const VR pixel_vr = explicit_vr ? VR::OB : VR::None;
				    write_element_header(
				        direct_writer, element.tag(), pixel_vr, 0xFFFFFFFFu, true,
				        explicit_vr);
				    write_item_header(direct_writer, kItemTag, 0u);
				    std::uint64_t next_frame_offset = 0;

				    if (needs_native_to_encapsulated) {
					    const auto source_bytes = source_descriptor.bytes;
					    pixel::detail::encode_frames_from_frame_provider_with_runtime_or_throw(
					        file, target_transfer_syntax, source_descriptor,
					        codec_profile_code, active_encoder_ctx->codec_options(),
					        use_multicomponent_transform, source_layout.frames,
					        [&](std::size_t frame_index) -> std::span<const std::uint8_t> {
						        const auto frame_offset =
						            frame_index * source_layout.source_frame_stride;
						        return source_bytes.subspan(
						            frame_offset,
						            source_layout.source_frame_size_bytes);
					        },
					        [&](std::size_t, std::vector<std::uint8_t>&& encoded_frame) {
						        if (write_extended_offset_table) {
							        extended_offsets.push_back(next_frame_offset);
							        extended_lengths.push_back(encoded_frame.size());
						        }
						        write_pixel_fragment(direct_writer,
						            std::span<const std::uint8_t>(
						                encoded_frame.data(),
						                encoded_frame.size()));
						        next_frame_offset += 8u +
						            static_cast<std::uint64_t>(
						                padded_length(encoded_frame.size()));
					        });
				    } else {
					    if (decoded_frame.size() != decode_plan->strides.frame) {
						    decoded_frame.resize(decode_plan->strides.frame);
					    }
					    pixel::detail::encode_frames_from_frame_provider_with_runtime_or_throw(
					        file, target_transfer_syntax, source_descriptor,
					        codec_profile_code, active_encoder_ctx->codec_options(),
					        use_multicomponent_transform, source_layout.frames,
					        [&](std::size_t frame_index) -> std::span<const std::uint8_t> {
						        auto frame_span = std::span<std::uint8_t>(
						            decoded_frame.data(), decoded_frame.size());
						        const auto prepared_source =
						            pixel::support_detail::prepare_decode_frame_source_or_throw(
						                file, info, frame_index);
						        pixel::detail::dispatch_decode_prepared_frame(
						            file.path(), frame_index, prepared_source.bytes,
						            frame_span, *decode_plan);
						        return std::span<const std::uint8_t>(
						            frame_span.data(),
						            source_layout.source_frame_size_bytes);
					        },
					        [&](std::size_t, std::vector<std::uint8_t>&& encoded_frame) {
						        if (write_extended_offset_table) {
							        extended_offsets.push_back(next_frame_offset);
							        extended_lengths.push_back(encoded_frame.size());
						        }
						        write_pixel_fragment(direct_writer,
						            std::span<const std::uint8_t>(
						                encoded_frame.data(),
						                encoded_frame.size()));
						        next_frame_offset += 8u +
						            static_cast<std::uint64_t>(
						                padded_length(encoded_frame.size()));
					        });
				    }

				    write_item_header(direct_writer, kSequenceDelimitationTag, 0u);
				    if (write_extended_offset_table) {
					    const auto extended_offsets_bytes =
					        pack_u64_values(extended_offsets);
					    const auto extended_lengths_bytes =
					        pack_u64_values(extended_lengths);
					    direct_writer.overwrite(extended_offset_table_value_offset,
					        std::span<const std::uint8_t>(
					            extended_offsets_bytes.data(),
					            extended_offsets_bytes.size()));
					    direct_writer.overwrite(
					        extended_offset_table_lengths_value_offset,
					        std::span<const std::uint8_t>(
					            extended_lengths_bytes.data(),
					            extended_lengths_bytes.size()));
				    }
			    });
		} else {
			if (decoded_frame.size() != decode_plan->strides.frame) {
				decoded_frame.resize(decode_plan->strides.frame);
			}
			const auto native_pixel_vr =
			    native_pixel_vr_from_bits_allocated_for_write(
			        source_layout.bits_allocated);
			write_dataset_body_with_pixel_writer(
			    writer, dataset, encoding, file.path(),
			    [&](const DataElement& element, auto& direct_writer, bool explicit_vr) {
				    write_native_pixel_data_from_frame_provider(
				        direct_writer, element, explicit_vr, native_pixel_vr,
				        source_layout.destination_total_bytes,
				        source_layout.frames,
				        source_layout.destination_frame_payload,
				        [&](std::size_t frame_index) -> std::span<const std::uint8_t> {
					        auto frame_span = std::span<std::uint8_t>(
					            decoded_frame.data(), decoded_frame.size());
					        const auto prepared_source =
					            pixel::support_detail::prepare_decode_frame_source_or_throw(
					                file, info, frame_index);
					        pixel::detail::dispatch_decode_prepared_frame(
					            file.path(), frame_index, prepared_source.bytes,
					            frame_span, *decode_plan);
					        return std::span<const std::uint8_t>(
					            frame_span.data(),
					            source_layout.destination_frame_payload);
				        });
			    });
		}

		restore_guard.restore();
	} catch (...) {
		try {
			restore_guard.restore();
		} catch (...) {
		}
		throw;
	}
}

template <typename Writer>
void write_impl(DicomFile& file, Writer& writer, const WriteOptions& options) {
	if (!options.keep_existing_meta) {
		file.rebuild_file_meta();
	}

	const DataSet& dataset = file.dataset();
	dataset.ensure_loaded(Tag(0xFFFFu, 0xFFFFu));
	const auto target_transfer_syntax = resolve_target_transfer_syntax(file, dataset, options);
	const auto encoding = determine_dataset_encoding(target_transfer_syntax, dataset);

	if (options.include_preamble) {
		write_preamble(writer);
	}

	if (options.write_file_meta) {
		write_file_meta_group(writer, dataset);
	}

	write_dataset_body(writer, dataset, encoding, file.path());
}

}  // namespace

void DicomFile::rebuild_file_meta() {
	DataSet& dataset = this->dataset();
	dataset.ensure_loaded(Tag(0xFFFFu, 0xFFFFu));

	std::string sop_class_uid;
	if (auto value = dataset[kSopClassUidTag].to_uid_string(); value && !value->empty()) {
		sop_class_uid = uid::normalize_uid_text(*value);
	} else if (auto value = dataset[kMediaStorageSopClassUidTag].to_uid_string();
	           value && !value->empty()) {
		sop_class_uid = uid::normalize_uid_text(*value);
	} else {
		sop_class_uid = std::string("SecondaryCaptureImageStorage"_uid.value());
	}
	if (!uid::is_valid_uid_text_strict(sop_class_uid)) {
		sop_class_uid = std::string("SecondaryCaptureImageStorage"_uid.value());
	}

	std::string sop_instance_uid;
	if (auto value = dataset[kSopInstanceUidTag].to_uid_string(); value && !value->empty()) {
		sop_instance_uid = uid::normalize_uid_text(*value);
	} else if (auto value = dataset[kMediaStorageSopInstanceUidTag].to_uid_string();
	           value && !value->empty()) {
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

	clear_existing_meta_group(dataset);
	dataset.add_dataelement(kFileMetaGroupLengthTag, VR::UL);
	const std::array<std::uint8_t, 2> meta_version{{0x00u, 0x01u}};
	dataset.add_dataelement(kFileMetaInformationVersionTag, VR::OB)
	    .set_value_bytes(meta_version);
	set_dataelement_uid(dataset, kMediaStorageSopClassUidTag, sop_class_uid);
	set_dataelement_uid(dataset, kMediaStorageSopInstanceUidTag, sop_instance_uid);
	set_dataelement_uid(dataset, kTransferSyntaxUidTag, transfer_syntax_uid);
	set_dataelement_uid(dataset, kImplementationClassUidTag, uid::implementation_class_uid());
	dataset.add_dataelement(kImplementationVersionNameTag, VR::SH).from_string_view(
	    uid::implementation_version_name());

	const auto meta_group_length = measure_meta_group_length(dataset);
	dataset.get_dataelement(kFileMetaGroupLengthTag).from_long(meta_group_length);
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

void DicomFile::write_with_transfer_syntax(std::ostream& os,
    uid::WellKnown transfer_syntax, const WriteOptions& options) {
	StreamWriter writer(os);
	write_with_transfer_syntax_impl(*this, writer, transfer_syntax,
	    StreamingWriteEncodeMode::use_plugin_defaults,
	    std::span<const pixel::CodecOptionTextKv>{}, nullptr, options);
}

void DicomFile::write_with_transfer_syntax(std::ostream& os,
    uid::WellKnown transfer_syntax, const pixel::EncoderContext& encoder_ctx,
    const WriteOptions& options) {
	StreamWriter writer(os);
	write_with_transfer_syntax_impl(*this, writer, transfer_syntax,
	    StreamingWriteEncodeMode::use_encoder_context,
	    std::span<const pixel::CodecOptionTextKv>{}, &encoder_ctx, options);
}

void DicomFile::write_with_transfer_syntax(std::ostream& os,
    uid::WellKnown transfer_syntax,
    std::span<const pixel::CodecOptionTextKv> codec_opt,
    const WriteOptions& options) {
	StreamWriter writer(os);
	write_with_transfer_syntax_impl(*this, writer, transfer_syntax,
	    StreamingWriteEncodeMode::use_explicit_options,
	    codec_opt, nullptr, options);
}

void DicomFile::write_with_transfer_syntax(const std::string& path,
    uid::WellKnown transfer_syntax, const WriteOptions& options) {
	std::ofstream os(path, std::ios::binary | std::ios::trunc);
	if (!os) {
		diag::error_and_throw(
		    "write_with_transfer_syntax path={} reason=failed to open output file",
		    path);
	}

	std::vector<char> file_buffer(1 << 20);
	os.rdbuf()->pubsetbuf(
	    file_buffer.data(), static_cast<std::streamsize>(file_buffer.size()));

	write_with_transfer_syntax(os, transfer_syntax, options);
	os.flush();
	if (!os) {
		diag::error_and_throw(
		    "write_with_transfer_syntax path={} reason=failed to flush output file",
		    path);
	}
}

void DicomFile::write_with_transfer_syntax(const std::string& path,
    uid::WellKnown transfer_syntax, const pixel::EncoderContext& encoder_ctx,
    const WriteOptions& options) {
	std::ofstream os(path, std::ios::binary | std::ios::trunc);
	if (!os) {
		diag::error_and_throw(
		    "write_with_transfer_syntax path={} reason=failed to open output file",
		    path);
	}

	std::vector<char> file_buffer(1 << 20);
	os.rdbuf()->pubsetbuf(
	    file_buffer.data(), static_cast<std::streamsize>(file_buffer.size()));

	write_with_transfer_syntax(os, transfer_syntax, encoder_ctx, options);
	os.flush();
	if (!os) {
		diag::error_and_throw(
		    "write_with_transfer_syntax path={} reason=failed to flush output file",
		    path);
	}
}

void DicomFile::write_with_transfer_syntax(const std::string& path,
    uid::WellKnown transfer_syntax,
    std::span<const pixel::CodecOptionTextKv> codec_opt,
    const WriteOptions& options) {
	std::ofstream os(path, std::ios::binary | std::ios::trunc);
	if (!os) {
		diag::error_and_throw(
		    "write_with_transfer_syntax path={} reason=failed to open output file",
		    path);
	}

	std::vector<char> file_buffer(1 << 20);
	os.rdbuf()->pubsetbuf(
	    file_buffer.data(), static_cast<std::streamsize>(file_buffer.size()));

	write_with_transfer_syntax(os, transfer_syntax, codec_opt, options);
	os.flush();
	if (!os) {
		diag::error_and_throw(
		    "write_with_transfer_syntax path={} reason=failed to flush output file",
		    path);
	}
}

}  // namespace dicom
