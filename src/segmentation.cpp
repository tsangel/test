#include "dicom_seg.h"

#include "diagnostics.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace dicom::seg {
using namespace dicom::literals;

namespace {

#ifdef DICOMSDL_SEGMENTATION_TEST_HOOKS
std::atomic<std::size_t> g_labelmap_frame_scan_count{0};
#endif

// Small typed readers used by the SEG adapter. They intentionally keep the
// source DataSet as the single source of truth and avoid copying text values.
[[nodiscard]] bool text_equals(std::string_view lhs, std::string_view rhs) noexcept {
	return lhs == rhs;
}

[[nodiscard]] std::optional<std::string_view> string_value(
    const DataSet& dataset, Tag tag) {
	return dataset.get_dataelement(tag).to_string_view();
}

[[nodiscard]] std::string_view string_value_or_empty(
    const DataSet& dataset, Tag tag) {
	return string_value(dataset, tag).value_or(std::string_view{});
}

[[nodiscard]] const Sequence* sequence_value(
    const DataSet& dataset, Tag tag) {
	const auto& element = dataset.get_dataelement(tag);
	return element ? element.as_sequence() : nullptr;
}

[[nodiscard]] const DataSet* sequence_item(
    const DataSet& dataset, Tag sequence_tag, std::size_t index) {
	const auto* sequence = sequence_value(dataset, sequence_tag);
	if (!sequence || index >= static_cast<std::size_t>(sequence->size())) {
		return nullptr;
	}
	return sequence->get_dataset(index);
}

[[nodiscard]] const Sequence* functional_group_sequence(
    const DataSet& frame_item, const DataSet* shared_item, Tag sequence_tag) {
	// Functional group macros may be repeated per-frame or placed once in
	// SharedFunctionalGroupsSequence. Per-frame values override shared values.
	if (const auto* sequence = sequence_value(frame_item, sequence_tag)) {
		if (sequence->size() > 0) {
			return sequence;
		}
	}
	if (shared_item) {
		if (const auto* sequence = sequence_value(*shared_item, sequence_tag)) {
			if (sequence->size() > 0) {
				return sequence;
			}
		}
	}
	return nullptr;
}

[[nodiscard]] const DataSet* functional_group_first_item(
    const DataSet& frame_item, const DataSet* shared_item, Tag sequence_tag) {
	const auto* sequence = functional_group_sequence(frame_item, shared_item, sequence_tag);
	return sequence ? sequence->get_dataset(0) : nullptr;
}

[[nodiscard]] std::optional<std::uint16_t> uint16_value(
    const DataElement& element) {
	auto values = element.to_long_vector();
	if (values && !values->empty()) {
		if (values->size() != 1) {
			return std::nullopt;
		}
		const auto value = values->front();
		if (value < 0 || value > std::numeric_limits<std::uint16_t>::max()) {
			return std::nullopt;
		}
		return static_cast<std::uint16_t>(value);
	}
	if (const auto value = element.to_long()) {
		if (*value < 0 || *value > std::numeric_limits<std::uint16_t>::max()) {
			return std::nullopt;
		}
		return static_cast<std::uint16_t>(*value);
	}
	return std::nullopt;
}

[[nodiscard]] std::optional<std::uint16_t> uint16_value(
    const DataSet& dataset, Tag tag) {
	return uint16_value(dataset.get_dataelement(tag));
}

[[nodiscard]] std::optional<std::size_t> size_value(
    const DataSet& dataset, Tag tag) {
	if (const auto value = dataset.get_dataelement(tag).to_long()) {
		if (*value < 0) {
			return std::nullopt;
		}
		return static_cast<std::size_t>(*value);
	}
	return std::nullopt;
}

[[nodiscard]] std::optional<std::uint16_t> referenced_segment_number(
    const DataSet& frame_item, const DataSet* shared_item) {
	const auto* segment_id_item = functional_group_first_item(
	    frame_item, shared_item, "SegmentIdentificationSequence"_tag);
	if (!segment_id_item) {
		return std::nullopt;
	}
	return uint16_value(*segment_id_item, "ReferencedSegmentNumber"_tag);
}

[[nodiscard]] std::vector<std::uint32_t> uint32_values(
    const DataElement& element) {
	std::vector<std::uint32_t> result;
	if (auto values = element.to_long_vector()) {
		result.reserve(values->size());
		for (const auto value : *values) {
			if (value >= 0 &&
			    static_cast<unsigned long long>(value) <=
			        std::numeric_limits<std::uint32_t>::max()) {
				result.push_back(static_cast<std::uint32_t>(value));
			}
		}
		return result;
	}
	if (const auto value = element.to_long();
	    value && *value >= 0 &&
	    static_cast<unsigned long long>(*value) <=
	        std::numeric_limits<std::uint32_t>::max()) {
		result.push_back(static_cast<std::uint32_t>(*value));
	}
	return result;
}

template <std::size_t N>
[[nodiscard]] std::optional<std::array<double, N>> double_array_value(
    const DataSet& dataset, Tag tag) {
	const auto values = dataset.get_dataelement(tag).to_double_vector();
	if (!values || values->size() < N) {
		return std::nullopt;
	}
	std::array<double, N> out{};
	std::copy_n(values->begin(), N, out.begin());
	return out;
}

[[nodiscard]] std::optional<CodeView> code_view_from_sequence(
    const DataSet& dataset, Tag sequence_tag) {
	// MVP accessors expose the first code item. Multi-item code semantics can be
	// added later without changing the raw dataset escape hatch.
	const auto* item = sequence_item(dataset, sequence_tag, 0);
	if (!item) {
		return std::nullopt;
	}
	return CodeView{
	    .value = string_value_or_empty(*item, "CodeValue"_tag),
	    .scheme_designator =
	        string_value_or_empty(*item, "CodingSchemeDesignator"_tag),
	    .scheme_version =
	        string_value_or_empty(*item, "CodingSchemeVersion"_tag),
	    .meaning = string_value_or_empty(*item, "CodeMeaning"_tag),
	};
}

[[nodiscard]] SegmentationType parse_segmentation_type(
    std::string_view value) noexcept {
	if (text_equals(value, "BINARY")) {
		return SegmentationType::binary;
	}
	if (text_equals(value, "FRACTIONAL")) {
		return SegmentationType::fractional;
	}
	if (text_equals(value, "LABELMAP")) {
		return SegmentationType::labelmap;
	}
	return SegmentationType::unknown;
}

[[nodiscard]] SegmentationFractionalType parse_fractional_type(
    std::string_view value) noexcept {
	if (text_equals(value, "PROBABILITY")) {
		return SegmentationFractionalType::probability;
	}
	if (text_equals(value, "OCCUPANCY")) {
		return SegmentationFractionalType::occupancy;
	}
	if (value.empty()) {
		return SegmentationFractionalType::none;
	}
	return SegmentationFractionalType::unknown;
}

[[nodiscard]] SegmentAlgorithmType parse_algorithm_type(
    std::string_view value) noexcept {
	if (text_equals(value, "AUTOMATIC")) {
		return SegmentAlgorithmType::automatic_;
	}
	if (text_equals(value, "SEMIAUTOMATIC")) {
		return SegmentAlgorithmType::semiautomatic;
	}
	if (text_equals(value, "MANUAL")) {
		return SegmentAlgorithmType::manual;
	}
	return SegmentAlgorithmType::unknown;
}

enum class SopClassKind : std::uint8_t {
	none,
	segmentation,
	labelmap,
	conflict,
};

[[nodiscard]] SopClassKind sop_class_kind_from_value(
    std::string_view value) noexcept {
	const auto segmentation_storage = "SegmentationStorage"_uid.value();
	if (value == segmentation_storage) {
		return SopClassKind::segmentation;
	}
	const auto labelmap_segmentation_storage =
	    "LabelMapSegmentationStorage"_uid.value();
	if (value == labelmap_segmentation_storage) {
		return SopClassKind::labelmap;
	}
	return SopClassKind::none;
}

[[nodiscard]] SopClassKind declared_sop_class_kind(const DataSet& dataset) {
	const auto sop_class_uid = string_value(dataset, "SOPClassUID"_tag);
	if (const auto media_sop_class_uid =
	        string_value(dataset, "MediaStorageSOPClassUID"_tag);
	    sop_class_uid && media_sop_class_uid &&
	    *sop_class_uid != *media_sop_class_uid) {
		return SopClassKind::conflict;
	}
	if (sop_class_uid) {
		return sop_class_kind_from_value(*sop_class_uid);
	}
	if (const auto media_sop_class_uid =
	        string_value(dataset, "MediaStorageSOPClassUID"_tag)) {
		return sop_class_kind_from_value(*media_sop_class_uid);
	}
	return SopClassKind::none;
}

[[noreturn]] void throw_seg(std::string_view reason) {
	diag::error_and_throw("seg::from_dicomfile reason={}", reason);
}

[[noreturn]] void throw_decode(std::string_view reason) {
	diag::error_and_throw("seg::Segmentation::decode_frame_into reason={}", reason);
}

[[nodiscard]] std::vector<detail::SourceImageRefRecord> collect_source_image_refs(
    const DataSet& frame_item, const DataSet* shared_item) {
	// SourceImageSequence is nested under DerivationImageSequence in SEG. These
	// provenance references are cached with the frame index so const accessors
	// never need to mutate the Segmentation.
	std::vector<detail::SourceImageRefRecord> refs;
	const auto* derivation_sequence = functional_group_sequence(
	    frame_item, shared_item, "DerivationImageSequence"_tag);
	if (!derivation_sequence) {
		return refs;
	}
	for (std::size_t derivation_index = 0;
	     derivation_index < static_cast<std::size_t>(derivation_sequence->size());
	     ++derivation_index) {
		const auto* derivation_item = derivation_sequence->get_dataset(derivation_index);
		if (!derivation_item) {
			continue;
		}
		const auto* source_sequence =
		    sequence_value(*derivation_item, "SourceImageSequence"_tag);
		if (!source_sequence) {
			continue;
		}
		for (std::size_t source_index = 0;
		     source_index < static_cast<std::size_t>(source_sequence->size());
		     ++source_index) {
			const auto* source_item = source_sequence->get_dataset(source_index);
			if (!source_item) {
				continue;
			}
			refs.push_back(detail::SourceImageRefRecord{
			    .item = source_item,
			    .referenced_frame_numbers = uint32_values(
			        source_item->get_dataelement("ReferencedFrameNumber"_tag)),
			});
		}
	}
	return refs;
}

struct LabelmapFrameScanRequest {
	std::span<std::uint8_t> decoded_u8{};
	std::span<std::uint16_t> decoded_u16{};
	std::span<std::uint8_t> mask_out{};
	std::optional<std::uint16_t> target_segment{};
	bool collect_presence{true};
};

struct LabelmapFrameScanResult {
	std::shared_ptr<const std::vector<std::uint16_t>> present_labels{};
	bool decoded_written{false};
	bool mask_written{false};
	bool target_seen{false};
};

template <typename T, typename U>
[[nodiscard]] bool spans_overlap(std::span<T> lhs, std::span<U> rhs) noexcept {
	if (lhs.empty() || rhs.empty()) {
		return false;
	}
	const auto lhs_begin =
	    reinterpret_cast<std::uintptr_t>(lhs.data());
	const auto rhs_begin =
	    reinterpret_cast<std::uintptr_t>(rhs.data());
	const auto lhs_end = lhs_begin + lhs.size_bytes();
	const auto rhs_end = rhs_begin + rhs.size_bytes();
	return lhs_begin < rhs_end && rhs_begin < lhs_end;
}

[[nodiscard]] std::size_t checked_frame_sample_count(
    std::size_t rows, std::size_t columns) {
	if (rows == 0 || columns == 0 ||
	    columns > std::numeric_limits<std::size_t>::max() / rows) {
		throw_decode("invalid Rows or Columns");
	}
	return rows * columns;
}

[[nodiscard]] std::span<const std::uint8_t> labelmap_pixeldata_bytes(
    const DicomFile& file, std::size_t frame_count,
    std::size_t pixels_per_frame, std::uint16_t bits_allocated) {
	const auto transfer_syntax = file.transfer_syntax_uid();
	if (transfer_syntax == "ExplicitVRBigEndian"_uid) {
		throw_decode("Big Endian LABELMAP PixelData is not supported");
	}

	file.ensure_loaded("PixelData"_tag);
	const auto& pixel_data = file.get_dataelement("PixelData"_tag);
	if (!pixel_data) {
		throw_decode("LABELMAP PixelData is missing");
	}
	if (pixel_data.as_pixel_sequence()) {
		throw_decode("LABELMAP PixelData must be native binary, not PixelSequence");
	}
	if (dicom::detail::is_detached_pixel_payload_marker(pixel_data)) {
		throw_decode("LABELMAP PixelData payload is detached");
	}

	const auto bytes_per_sample = bits_allocated / 8u;
	if (bytes_per_sample == 0 ||
	    pixels_per_frame > std::numeric_limits<std::size_t>::max() / bytes_per_sample) {
		throw_decode("LABELMAP PixelData size overflow");
	}
	const auto bytes_per_frame = pixels_per_frame * bytes_per_sample;
	if (frame_count != 0 &&
	    bytes_per_frame > std::numeric_limits<std::size_t>::max() / frame_count) {
		throw_decode("LABELMAP PixelData size overflow");
	}
	const auto expected = bytes_per_frame * frame_count;
	const auto bytes = pixel_data.value_span();
	if (bytes.size() != expected) {
		const auto has_legal_padding =
		    (expected % 2u) == 1u && bytes.size() == expected + 1u &&
		    bytes.size() > 0 && bytes[expected] == 0u;
		if (!has_legal_padding) {
			throw_decode("LABELMAP PixelData size mismatch");
		}
	}
	return bytes;
}

enum class LabelmapSampleByteOrder : std::uint8_t {
	little_endian,
	native_endian,
};

struct LabelmapFrameDecodeContext {
	std::optional<pixel::DecodePlan> decode_plan{};
	std::vector<std::uint8_t> decoded_frame{};
	std::array<std::uint64_t, 1024> seen_words{};
	std::vector<std::uint16_t> present_labels{};
};

void require_lossless_seg_decode(const pixel::DecodeInfo& decode_info) {
	if (decode_info.encoded_lossy_state != pixel::EncodedLossyState::lossless) {
		throw_decode("SEG compressed PixelData decode requires lossless source frames");
	}
}

void validate_fractional_frame_samples_or_throw(
    std::span<const std::uint8_t> frame, std::size_t pixels_per_frame,
    std::optional<std::uint16_t> maximum_fractional_value) {
	if (!maximum_fractional_value || *maximum_fractional_value == 0) {
		throw_decode("FRACTIONAL SEG requires MaximumFractionalValue");
	}
	if (frame.size() < pixels_per_frame) {
		throw_decode("FRACTIONAL SEG decoded frame size mismatch");
	}
	const auto maximum = *maximum_fractional_value;
	for (std::size_t index = 0; index < pixels_per_frame; ++index) {
		if (frame[index] > maximum) {
			throw_decode(
			    "FRACTIONAL SEG PixelData sample exceeds MaximumFractionalValue");
		}
	}
}

[[nodiscard]] const pixel::DecodePlan& labelmap_decode_plan_or_throw(
    const DicomFile& file, std::uint16_t bits_allocated,
    std::size_t bytes_per_frame, LabelmapFrameDecodeContext& context) {
	auto create_plan = [&] {
		pixel::DecodeOptions decode_options{};
		decode_options.alignment = 1;
		decode_options.planar_out = pixel::Planar::interleaved;
		decode_options.decode_mct = false;
		auto decode_plan = file.create_decode_plan(decode_options);
		const auto expected_dtype = bits_allocated == 8 ? pixel::DataType::u8
		                                                : pixel::DataType::u16;
		if (decode_plan.output_layout.data_type != expected_dtype ||
		    decode_plan.output_layout.samples_per_pixel != 1 ||
		    decode_plan.output_layout.frame_stride < bytes_per_frame) {
			throw_decode("LABELMAP decoded frame layout mismatch");
		}
		return decode_plan;
	};

	if (!context.decode_plan) {
		context.decode_plan = create_plan();
	}
	return *context.decode_plan;
}

[[nodiscard]] LabelmapFrameScanResult decode_and_scan_labelmap_frame(
    const DicomFile& file, std::size_t frame_count, std::size_t frame_index,
    std::size_t rows, std::size_t columns, std::uint16_t bits_allocated,
    const std::bitset<65536>& valid_labels,
    std::optional<std::uint16_t> background_label,
    const LabelmapFrameScanRequest& request,
    LabelmapFrameDecodeContext* decode_context = nullptr) {
	if (frame_index >= frame_count) {
		throw_decode("frame index out of range");
	}
	if (bits_allocated != 8 && bits_allocated != 16) {
		throw_decode("LABELMAP requires BitsAllocated=8 or 16");
	}
	if (!request.decoded_u8.empty() && !request.decoded_u16.empty()) {
		throw_decode("LABELMAP decode request has multiple decoded outputs");
	}
	if (spans_overlap(request.decoded_u8, request.mask_out) ||
	    spans_overlap(request.decoded_u16, request.mask_out) ||
	    spans_overlap(request.decoded_u8, request.decoded_u16)) {
		throw_decode("LABELMAP output buffers must not overlap");
	}

	const auto pixels_per_frame = checked_frame_sample_count(rows, columns);
	if (!request.decoded_u8.empty()) {
		if (bits_allocated != 8) {
			throw_decode("LABELMAP uint8 output requires BitsAllocated=8");
		}
		if (request.decoded_u8.size() < pixels_per_frame) {
			throw_decode("destination buffer is smaller than decoded frame");
		}
	}
	if (!request.decoded_u16.empty()) {
		if (bits_allocated != 16) {
			throw_decode("LABELMAP uint16 output requires BitsAllocated=16");
		}
		if (request.decoded_u16.size() < pixels_per_frame) {
			throw_decode("destination buffer is smaller than decoded frame");
		}
	}
	if (!request.mask_out.empty() && request.mask_out.size() < pixels_per_frame) {
		throw_decode("destination buffer is smaller than decoded frame");
	}

#ifdef DICOMSDL_SEGMENTATION_TEST_HOOKS
	g_labelmap_frame_scan_count.fetch_add(1, std::memory_order_relaxed);
#endif

	const auto bytes_per_sample = static_cast<std::size_t>(bits_allocated / 8u);
	if (bytes_per_sample == 0 ||
	    pixels_per_frame > std::numeric_limits<std::size_t>::max() / bytes_per_sample) {
		throw_decode("LABELMAP PixelData size overflow");
	}
	const auto bytes_per_frame = pixels_per_frame * bytes_per_sample;

	std::optional<LabelmapFrameDecodeContext> local_decode_context{};
	if (!decode_context) {
		local_decode_context.emplace();
	}
	auto& active_decode_context =
	    decode_context ? *decode_context : *local_decode_context;

	const auto word_count = bits_allocated == 8 ? std::size_t{4} : std::size_t{1024};
	auto& seen_words = active_decode_context.seen_words;
	auto& present = active_decode_context.present_labels;
	present.clear();
	if (request.collect_presence) {
		std::fill_n(seen_words.begin(), word_count, std::uint64_t{0});
		const auto reserve_count =
		    bits_allocated == 8 ? std::size_t{256} : std::size_t{1024};
		if (present.capacity() < reserve_count) {
			present.reserve(reserve_count);
		}
	}
	const auto mark_seen = [&](std::uint16_t value) {
		if (!request.collect_presence) {
			return;
		}
		const auto word = static_cast<std::size_t>(value / 64u);
		const auto mask = std::uint64_t{1} << (value % 64u);
		if ((seen_words[word] & mask) == 0u) {
			seen_words[word] |= mask;
			present.push_back(value);
		}
	};

	const auto target = request.target_segment.value_or(0);
	const auto scan_frame_bytes =
	    [&](std::span<const std::uint8_t> frame_bytes,
	        LabelmapSampleByteOrder byte_order) {
		    if (frame_bytes.size() < bytes_per_frame) {
			    throw_decode("LABELMAP decoded frame size mismatch");
		    }

		    bool target_seen = false;
		    for (std::size_t pixel_index = 0; pixel_index < pixels_per_frame; ++pixel_index) {
			    std::uint16_t value = 0;
			    if (bits_allocated == 8) {
				    value = frame_bytes[pixel_index];
				    if (!request.decoded_u8.empty()) {
					    request.decoded_u8[pixel_index] =
					        static_cast<std::uint8_t>(value);
				    }
			    } else {
				    const auto byte_offset = pixel_index * 2u;
				    if (byte_order == LabelmapSampleByteOrder::native_endian) {
					    std::memcpy(&value, frame_bytes.data() + byte_offset,
					        sizeof(value));
				    } else {
					    value = static_cast<std::uint16_t>(
					        frame_bytes[byte_offset] |
					        (static_cast<std::uint16_t>(
					             frame_bytes[byte_offset + 1u])
					            << 8u));
				    }
				    if (!request.decoded_u16.empty()) {
					    request.decoded_u16[pixel_index] = value;
				    }
			    }

			    if (!valid_labels.test(value)) {
				    throw_decode(
				        "LABELMAP PixelData references an undefined segment number");
			    }
			    if (!background_label || value != *background_label) {
				    mark_seen(value);
			    }
			    if (!request.mask_out.empty()) {
				    if (value == target) {
					    target_seen = true;
				    }
				    request.mask_out[pixel_index] =
				        static_cast<std::uint8_t>(value == target ? 1u : 0u);
			    }
			}

		    std::sort(present.begin(), present.end());
		    return LabelmapFrameScanResult{
		        .present_labels =
		            std::make_shared<const std::vector<std::uint16_t>>(present),
		        .decoded_written =
		            !request.decoded_u8.empty() || !request.decoded_u16.empty(),
		        .mask_written = !request.mask_out.empty(),
		        .target_seen = target_seen,
		    };
	    };

	file.ensure_loaded("PixelData"_tag);
	const auto& pixel_data = file.get_dataelement("PixelData"_tag);
	if (!pixel_data) {
		throw_decode("LABELMAP PixelData is missing");
	}
	const auto transfer_syntax = file.transfer_syntax_uid();
	if (transfer_syntax == "ExplicitVRBigEndian"_uid) {
		throw_decode("Big Endian LABELMAP PixelData is not supported");
	}
	const auto* pixel_sequence = pixel_data.as_pixel_sequence();
	if (pixel_sequence != nullptr || (transfer_syntax &&
	        (!transfer_syntax.is_uncompressed() || transfer_syntax.is_encapsulated()))) {
		if (pixel_sequence == nullptr) {
			throw_decode(
			    "compressed/encapsulated Label Map SEG PixelData requires PixelSequence");
		}
		if (!transfer_syntax || !transfer_syntax.is_encapsulated()) {
			throw_decode("LABELMAP PixelSequence requires encapsulated transfer syntax");
		}

		const auto& decode_plan = labelmap_decode_plan_or_throw(
		    file, bits_allocated, bytes_per_frame, active_decode_context);
		auto& decoded_frame = active_decode_context.decoded_frame;
		if (decoded_frame.size() != decode_plan.output_layout.frame_stride) {
			decoded_frame.resize(decode_plan.output_layout.frame_stride);
		}
		pixel::DecodeInfo decode_info{};
		file.decode_into(frame_index, decoded_frame, decode_plan, decode_info);
		require_lossless_seg_decode(decode_info);
		return scan_frame_bytes(
		    std::span<const std::uint8_t>(decoded_frame.data(), bytes_per_frame),
		    LabelmapSampleByteOrder::native_endian);
	}

	const auto bytes = labelmap_pixeldata_bytes(
	    file, frame_count, pixels_per_frame, bits_allocated);
	if (frame_index >
	    std::numeric_limits<std::size_t>::max() / pixels_per_frame ||
	    frame_index * pixels_per_frame >
	        std::numeric_limits<std::size_t>::max() / bytes_per_sample) {
		throw_decode("LABELMAP frame offset overflow");
	}
	const auto frame_offset = frame_index * bytes_per_frame;
	return scan_frame_bytes(bytes.subspan(frame_offset, bytes_per_frame),
	    LabelmapSampleByteOrder::little_endian);
}

} // namespace

#ifdef DICOMSDL_SEGMENTATION_TEST_HOOKS
namespace detail {

void reset_labelmap_frame_scan_count() noexcept {
	g_labelmap_frame_scan_count.store(0, std::memory_order_relaxed);
}

std::size_t labelmap_frame_scan_count() noexcept {
	return g_labelmap_frame_scan_count.load(std::memory_order_relaxed);
}

} // namespace detail
#endif

void Segmentation::index_segment_sequence_items(const Options& options) {
	// Build the SegmentNumber catalog first; frame indexing depends on this map
	// to validate ReferencedSegmentNumber values and to group frames by segment.
	const auto& dataset = file_->dataset();
	const auto* segment_sequence = sequence_value(dataset, "SegmentSequence"_tag);
	if (!segment_sequence || segment_sequence->size() <= 0) {
		if (options.validate_required_modules) {
			throw_seg("SegmentSequence is missing or empty");
		}
		return;
	}

	index_.segments.reserve(static_cast<std::size_t>(segment_sequence->size()));
	for (std::size_t index = 0;
	     index < static_cast<std::size_t>(segment_sequence->size()); ++index) {
		const auto* item = segment_sequence->get_dataset(index);
		if (!item) {
			if (options.validate_required_modules) {
				throw_seg("SegmentSequence contains a missing item");
			}
			continue;
		}
		const auto number = uint16_value(*item, "SegmentNumber"_tag);
		if (!number) {
			if (options.validate_required_modules) {
				throw_seg("SegmentSequence item is missing SegmentNumber");
			}
			continue;
		}
		if (*number == 0 && segmentation_type_ != SegmentationType::labelmap) {
			if (options.validate_required_modules) {
				throw_seg("SegmentNumber 0 is only valid for LABELMAP background");
			}
			continue;
		}
		if (index_.segment_index_by_number.contains(*number)) {
			if (options.validate_required_modules) {
				throw_seg("SegmentSequence contains duplicate SegmentNumber");
			}
			continue;
		}
		index_.segment_index_by_number.emplace(*number, index_.segments.size());
		index_.segments.push_back(detail::SegmentRecord{.item = item, .number = *number});
		if (segmentation_type_ == SegmentationType::labelmap) {
			labelmap_valid_labels_.set(*number);
		}
	}
	if (segmentation_type_ == SegmentationType::labelmap &&
	    labelmap_background_value_ &&
	    !index_.segment_index_by_number.contains(*labelmap_background_value_) &&
	    options.validate_required_modules) {
		throw_seg(
		    "LABELMAP PixelPaddingValue references a SegmentNumber absent from SegmentSequence");
	}
}

void Segmentation::index_per_frame_functional_group_items(const Options& options) {
	// Keep frame order identical to PixelData/PerFrameFunctionalGroupsSequence.
	// Segment-specific views store only indices into this canonical frame list.
	const auto& dataset = file_->dataset();
	const auto* per_frame_sequence =
	    sequence_value(dataset, "PerFrameFunctionalGroupsSequence"_tag);
	const auto per_frame_count =
	    per_frame_sequence ? static_cast<std::size_t>(per_frame_sequence->size()) : 0u;
	const auto declared_frame_count = size_value(dataset, "NumberOfFrames"_tag);
	const std::size_t frame_count = declared_frame_count.value_or(per_frame_count);

	if (frame_count == 0) {
		if (options.validate_required_modules) {
			throw_seg("NumberOfFrames or PerFrameFunctionalGroupsSequence is missing");
		}
		return;
	}
	if (!per_frame_sequence || per_frame_count < frame_count) {
		if (options.validate_required_modules) {
			throw_seg("PerFrameFunctionalGroupsSequence has fewer items than frames");
		}
	}
	if (declared_frame_count && per_frame_sequence && per_frame_count != *declared_frame_count &&
	    options.validate_required_modules) {
		throw_seg("NumberOfFrames does not match PerFrameFunctionalGroupsSequence item count");
	}

	index_.frames.reserve(frame_count);
	for (std::size_t frame_index = 0; frame_index < frame_count; ++frame_index) {
		const auto* frame_item =
		    per_frame_sequence && frame_index < per_frame_count
		        ? per_frame_sequence->get_dataset(frame_index)
		        : nullptr;
		if (!frame_item) {
			if (options.validate_required_modules) {
				throw_seg("PerFrameFunctionalGroupsSequence contains a missing item");
			}
			index_.frames.push_back(detail::SegmentFrameRecord{});
			continue;
		}

		if (segmentation_type_ == SegmentationType::labelmap) {
			index_.frames.push_back(detail::SegmentFrameRecord{
			    .functional_group_item = frame_item,
			    .source_images = collect_source_image_refs(
			        *frame_item, shared_functional_groups_item_),
			});
			continue;
		}

		const auto segment_number =
		    referenced_segment_number(*frame_item, shared_functional_groups_item_);
		if (!segment_number || *segment_number == 0) {
			if (options.validate_required_modules) {
				throw_seg("frame is missing a single ReferencedSegmentNumber");
			}
			index_.frames.push_back(detail::SegmentFrameRecord{
			    .functional_group_item = frame_item,
			    .source_images = collect_source_image_refs(
			        *frame_item, shared_functional_groups_item_),
			});
			continue;
		}
		if (!index_.segment_index_by_number.contains(*segment_number) &&
		    options.validate_required_modules) {
			throw_seg("frame references an undefined segment number");
		}

		detail::SegmentFrameRecord frame{
		    .functional_group_item = frame_item,
		    .referenced_segment_number = *segment_number,
		    .source_images = collect_source_image_refs(
		        *frame_item, shared_functional_groups_item_),
		};
		index_.frame_indices_by_segment[*segment_number].push_back(frame_index);
		index_.frames.push_back(std::move(frame));
	}
}

void Segmentation::extract_instance_metadata(const Options& options) {
	// Cache instance-level values that are cheap and central to the API. Segment
	// attributes and most frame attributes remain lazy through borrowed DataSet
	// views; source-image provenance is indexed with each frame.
	auto& dataset = file_->dataset();

	segmentation_type_ = parse_segmentation_type(
	    string_value_or_empty(dataset, "SegmentationType"_tag));
	fractional_type_ = parse_fractional_type(
	    string_value_or_empty(dataset, "SegmentationFractionalType"_tag));
	maximum_fractional_value_ = uint16_value(dataset, "MaximumFractionalValue"_tag);
	frame_of_reference_uid_ = string_value(dataset, "FrameOfReferenceUID"_tag);
	rows_ = size_value(dataset, "Rows"_tag).value_or(0);
	columns_ = size_value(dataset, "Columns"_tag).value_or(0);
	labelmap_bits_allocated_.reset();
	labelmap_background_value_.reset();

	if (segmentation_type_ == SegmentationType::unknown &&
	    options.validate_required_modules) {
		throw_seg("SegmentationType is missing or unsupported");
	}
	if (!frame_of_reference_uid_ && options.validate_required_modules) {
		throw_seg("FrameOfReferenceUID is missing");
	}
	if ((rows_ == 0 || columns_ == 0) && options.validate_required_modules) {
		throw_seg("Rows or Columns is missing");
	}
	if (segmentation_type_ == SegmentationType::fractional &&
	    options.validate_required_modules) {
		if (fractional_type_ == SegmentationFractionalType::none ||
		    fractional_type_ == SegmentationFractionalType::unknown) {
			throw_seg("FRACTIONAL SEG requires SegmentationFractionalType");
		}
		if (!maximum_fractional_value_ || *maximum_fractional_value_ == 0) {
			throw_seg("FRACTIONAL SEG requires MaximumFractionalValue");
		}
	}
	const auto sop_kind = declared_sop_class_kind(dataset);
	if (options.validate_required_modules) {
		if (segmentation_type_ == SegmentationType::labelmap &&
		    sop_kind != SopClassKind::labelmap) {
			throw_seg("LABELMAP SegmentationType requires Label Map Segmentation Storage");
		}
		if (sop_kind == SopClassKind::labelmap &&
		    segmentation_type_ != SegmentationType::labelmap) {
			throw_seg("Label Map Segmentation Storage requires SegmentationType=LABELMAP");
		}
		if (sop_kind == SopClassKind::segmentation &&
		    segmentation_type_ == SegmentationType::labelmap) {
			throw_seg("Segmentation Storage does not support SegmentationType=LABELMAP");
		}
	}
	if (segmentation_type_ == SegmentationType::labelmap) {
		const auto bits_allocated = uint16_value(dataset, "BitsAllocated"_tag);
		const auto bits_stored = uint16_value(dataset, "BitsStored"_tag);
		const auto high_bit = uint16_value(dataset, "HighBit"_tag);
		const auto pixel_representation =
		    uint16_value(dataset, "PixelRepresentation"_tag);
		const auto samples_per_pixel =
		    uint16_value(dataset, "SamplesPerPixel"_tag);
		const auto photometric =
		    string_value_or_empty(dataset, "PhotometricInterpretation"_tag);
		if (bits_allocated) {
			labelmap_bits_allocated_ = *bits_allocated;
		}
		if (const auto& pixel_padding =
		        dataset.get_dataelement("PixelPaddingValue"_tag)) {
			const auto padding_value = uint16_value(pixel_padding);
			if (padding_value) {
				labelmap_background_value_ = *padding_value;
			} else if (options.validate_required_modules) {
				throw_seg(
				    "LABELMAP PixelPaddingValue must be a single unsigned label value");
			}
		}
		if (options.validate_required_modules) {
			if (!bits_allocated || (*bits_allocated != 8 && *bits_allocated != 16)) {
				throw_seg("LABELMAP requires BitsAllocated=8 or 16");
			}
			if (!bits_stored || *bits_stored != *bits_allocated) {
				throw_seg("LABELMAP requires BitsStored to match BitsAllocated");
			}
			if (!high_bit || *high_bit != static_cast<std::uint16_t>(*bits_allocated - 1u)) {
				throw_seg("LABELMAP requires HighBit=BitsAllocated-1");
			}
			if (!pixel_representation || *pixel_representation != 0) {
				throw_seg("LABELMAP requires PixelRepresentation=0");
			}
			if (!samples_per_pixel || *samples_per_pixel != 1) {
				throw_seg("LABELMAP requires SamplesPerPixel=1");
			}
			if (photometric != "MONOCHROME2" && photometric != "PALETTE COLOR") {
				throw_seg(
				    "LABELMAP requires PhotometricInterpretation MONOCHROME2 or PALETTE COLOR");
			}
			if (dataset.get_dataelement("PixelPaddingRangeLimit"_tag)) {
				throw_seg("LABELMAP PixelPaddingRangeLimit is not supported");
			}
			if (const auto segments_overlap =
			        string_value(dataset, "SegmentsOverlap"_tag);
			    segments_overlap && *segments_overlap != "NO") {
				throw_seg("LABELMAP requires SegmentsOverlap=NO when present");
			}
		}
	}

	const auto* shared_sequence =
	    sequence_value(dataset, "SharedFunctionalGroupsSequence"_tag);
	if (!shared_sequence || shared_sequence->size() != 1) {
		if (options.validate_required_modules) {
			throw_seg("SharedFunctionalGroupsSequence must contain exactly one item");
		}
	} else {
		shared_functional_groups_item_ = shared_sequence->get_dataset(0);
	}
}

Segmentation::~Segmentation() = default;

Segmentation::Segmentation(std::unique_ptr<DicomFile> file) noexcept
    : file_(std::move(file)) {}

bool Segmentation::is_valid() const noexcept {
	return static_cast<bool>(file_);
}

SegmentationType Segmentation::segmentation_type() const noexcept {
	return segmentation_type_;
}

SegmentationFractionalType Segmentation::fractional_type() const noexcept {
	return fractional_type_;
}

std::optional<std::uint16_t>
Segmentation::labelmap_bits_allocated() const noexcept {
	return labelmap_bits_allocated_;
}

std::optional<std::uint16_t> Segmentation::maximum_fractional_value() const noexcept {
	return maximum_fractional_value_;
}

std::optional<std::string_view> Segmentation::frame_of_reference_uid() const noexcept {
	return frame_of_reference_uid_;
}

const DataSet& Segmentation::shared_functional_groups_item() const {
	if (!shared_functional_groups_item_) {
		diag::throw_exception("seg::Segmentation::shared_functional_groups_item missing");
	}
	return *shared_functional_groups_item_;
}

const DataSet* Segmentation::try_shared_functional_groups_item() const noexcept {
	return shared_functional_groups_item_;
}

const DataSet* Segmentation::try_per_frame_functional_groups_item(
    std::size_t frame_index) const noexcept {
	if (frame_index >= index_.frames.size()) {
		return nullptr;
	}
	return index_.frames[frame_index].functional_group_item;
}

std::size_t Segmentation::rows() const noexcept {
	return rows_;
}

std::size_t Segmentation::columns() const noexcept {
	return columns_;
}

std::size_t Segmentation::segment_count() const noexcept {
	return index_.segments.size();
}

std::size_t Segmentation::frame_count() const noexcept {
	return index_.frames.size();
}

const std::vector<detail::SourceImageRefRecord>&
Segmentation::source_image_refs_for_frame(std::size_t frame_index) const {
	static const std::vector<detail::SourceImageRefRecord> empty_refs{};
	if (frame_index >= index_.frames.size()) {
		return empty_refs;
	}
	return index_.frames[frame_index].source_images;
}

const pixel::DecodePlan& Segmentation::fractional_decode_plan() const {
	std::lock_guard lock(fractional_decode_plan_mutex_);
	if (!fractional_decode_plan_) {
		fractional_decode_plan_ = file_->create_decode_plan();
	}
	return *fractional_decode_plan_;
}

std::shared_ptr<const std::vector<std::uint16_t>>
Segmentation::labelmap_presence_for_frame(std::size_t frame_index) const {
	if (segmentation_type_ != SegmentationType::labelmap) {
		throw_decode("present label cache is only available for LABELMAP SEG");
	}
	if (frame_index >= index_.frames.size()) {
		throw_decode("frame index out of range");
	}
	{
		std::lock_guard lock(labelmap_cache_mutex_);
		const auto& cache = labelmap_presence_cache_[frame_index];
		if (cache.ready) {
			return cache.labels;
		}
	}

	const auto result = decode_and_scan_labelmap_frame(*file_, index_.frames.size(),
	    frame_index, rows_, columns_, labelmap_bits_allocated_.value_or(0),
	    labelmap_valid_labels_, labelmap_background_value_,
	    LabelmapFrameScanRequest{.collect_presence = true});

	std::lock_guard lock(labelmap_cache_mutex_);
	auto& cache = labelmap_presence_cache_[frame_index];
	if (!cache.ready) {
		cache.ready = true;
		cache.labels = result.present_labels;
	}
	return cache.labels;
}

std::shared_ptr<const detail::LabelmapFrameIndex>
Segmentation::ensure_labelmap_frame_index() const {
	if (segmentation_type_ != SegmentationType::labelmap) {
		throw_decode("LABELMAP frame index requested for non-LABELMAP SEG");
	}
	std::call_once(labelmap_frame_index_once_, [this] {
		std::vector<std::shared_ptr<const std::vector<std::uint16_t>>> frame_labels(
		    index_.frames.size());
		detail::LabelmapFrameIndex local_index{};
		LabelmapFrameDecodeContext decode_context{};
		for (std::size_t frame_index = 0; frame_index < index_.frames.size();
		     ++frame_index) {
			{
				std::lock_guard lock(labelmap_cache_mutex_);
				const auto& cache = labelmap_presence_cache_[frame_index];
				if (cache.ready) {
					frame_labels[frame_index] = cache.labels;
				}
			}
			if (!frame_labels[frame_index]) {
				const auto result = decode_and_scan_labelmap_frame(*file_,
				    index_.frames.size(), frame_index, rows_, columns_,
				    labelmap_bits_allocated_.value_or(0), labelmap_valid_labels_,
				    labelmap_background_value_,
				    LabelmapFrameScanRequest{.collect_presence = true},
				    &decode_context);
				frame_labels[frame_index] = result.present_labels;
			}
			for (const auto label : *frame_labels[frame_index]) {
				local_index.frame_indices_by_segment[label].push_back(frame_index);
			}
		}

		auto published =
		    std::make_shared<const detail::LabelmapFrameIndex>(std::move(local_index));
		std::lock_guard lock(labelmap_cache_mutex_);
		for (std::size_t frame_index = 0; frame_index < frame_labels.size();
		     ++frame_index) {
			auto& cache = labelmap_presence_cache_[frame_index];
			if (!cache.ready) {
				cache.ready = true;
				cache.labels = frame_labels[frame_index];
			}
		}
		labelmap_frame_index_ = std::move(published);
	});

	std::lock_guard lock(labelmap_cache_mutex_);
	if (!labelmap_frame_index_) {
		throw_decode("LABELMAP frame index build failed");
	}
	return labelmap_frame_index_;
}

SegmentListView Segmentation::segments() const noexcept {
	return SegmentListView(this);
}

SegmentFrameListView Segmentation::frames() const noexcept {
	return SegmentFrameListView(this, nullptr);
}

std::optional<SegmentView> Segmentation::segment_by_number(
    std::uint16_t segment_number) const noexcept {
	const auto found = index_.segment_index_by_number.find(segment_number);
	if (found == index_.segment_index_by_number.end()) {
		return std::nullopt;
	}
	const auto& segment = index_.segments[found->second];
	return SegmentView(segment.item, segment.number);
}

SegmentFrameListView Segmentation::frames_for_segment(
    std::uint16_t segment_number) const {
	if (!index_.segment_index_by_number.contains(segment_number)) {
		return SegmentFrameListView(this, &index_.empty_frame_indices);
	}
	if (segmentation_type_ == SegmentationType::labelmap) {
		const auto labelmap_index = ensure_labelmap_frame_index();
		const auto found =
		    labelmap_index->frame_indices_by_segment.find(segment_number);
		if (found == labelmap_index->frame_indices_by_segment.end()) {
			return SegmentFrameListView(this, &labelmap_index->empty_frame_indices);
		}
		return SegmentFrameListView(this, &found->second);
	}
	const auto found = index_.frame_indices_by_segment.find(segment_number);
	if (found == index_.frame_indices_by_segment.end()) {
		return SegmentFrameListView(this, &index_.empty_frame_indices);
	}
	return SegmentFrameListView(this, &found->second);
}

std::size_t Segmentation::segment_frame_count(
    std::uint16_t segment_number) const {
	if (!index_.segment_index_by_number.contains(segment_number)) {
		return 0u;
	}
	if (segmentation_type_ == SegmentationType::labelmap) {
		return frames_for_segment(segment_number).size();
	}
	const auto found = index_.frame_indices_by_segment.find(segment_number);
	return found == index_.frame_indices_by_segment.end() ? 0u : found->second.size();
}

std::span<const std::uint16_t>
Segmentation::present_segment_numbers(std::size_t frame_index) const {
	if (frame_index >= index_.frames.size()) {
		throw_decode("frame index out of range");
	}
	if (segmentation_type_ == SegmentationType::labelmap) {
		const auto labels = labelmap_presence_for_frame(frame_index);
		return std::span<const std::uint16_t>(labels->data(), labels->size());
	}
	const auto value = index_.frames[frame_index].referenced_segment_number;
	if (value == 0) {
		return {};
	}
	return std::span<const std::uint16_t>(&index_.frames[frame_index].referenced_segment_number, 1);
}

BinaryFrameBitsView
Segmentation::binary_frame_bits(std::size_t frame_index) const {
	if (frame_index >= index_.frames.size()) {
		throw_decode("frame index out of range");
	}
	if (segmentation_type_ != SegmentationType::binary) {
		throw_decode("binary_frame_bits requires BINARY SEG");
	}

	file_->ensure_loaded("PixelData"_tag);

	const auto transfer_syntax = file_->transfer_syntax_uid();
	if (transfer_syntax &&
	    (!transfer_syntax.is_uncompressed() || transfer_syntax.is_encapsulated())) {
		throw_decode("compressed/encapsulated BINARY SEG PixelData is not supported");
	}
	const auto bits_allocated =
	    file_->get_dataelement("BitsAllocated"_tag).to_long().value_or(0);
	const auto bits_stored =
	    file_->get_dataelement("BitsStored"_tag).to_long().value_or(0);
	const auto pixel_representation =
	    file_->get_dataelement("PixelRepresentation"_tag).to_long().value_or(0);
	if (bits_allocated != 1 || bits_stored != 1 || pixel_representation != 0) {
		throw_decode(
		    "BINARY SEG requires BitsAllocated=1, BitsStored=1 and PixelRepresentation=0");
	}

	const auto pixels_per_frame = checked_frame_sample_count(rows_, columns_);
	if (frame_index >
	    std::numeric_limits<std::size_t>::max() / pixels_per_frame) {
		throw_decode("frame bit offset overflows size_t");
	}
	if (index_.frames.size() >
	    std::numeric_limits<std::size_t>::max() / pixels_per_frame) {
		throw_decode("total bit count overflows size_t");
	}
	const auto frame_bit_offset = frame_index * pixels_per_frame;
	const auto total_bits = index_.frames.size() * pixels_per_frame;
	const auto expected_bytes =
	    (total_bits / 8u) + ((total_bits % 8u) != 0 ? 1u : 0u);

	const auto& pixel_data = file_->get_dataelement("PixelData"_tag);
	if (!pixel_data) {
		throw_decode("BINARY SEG PixelData is missing");
	}
	if (pixel_data.as_pixel_sequence()) {
		throw_decode("compressed/encapsulated BINARY SEG PixelData is not supported");
	}
	if (dicom::detail::is_detached_pixel_payload_marker(pixel_data)) {
		throw_decode("BINARY SEG PixelData payload is detached");
	}
	const auto bytes = pixel_data.value_span();
	const bool allows_even_length_padding =
	    (expected_bytes % 2u) != 0 && bytes.size() == expected_bytes + 1u;
	if (bytes.size() != expected_bytes && !allows_even_length_padding) {
		throw_decode("BINARY SEG PixelData size mismatch");
	}
	if (allows_even_length_padding && bytes[expected_bytes] != 0) {
		throw_decode("BINARY SEG PixelData padding byte must be zero");
	}

	const auto first_byte = frame_bit_offset / 8u;
	const auto first_bit_offset =
	    static_cast<std::uint8_t>(frame_bit_offset % 8u);
	if (pixels_per_frame >
	    std::numeric_limits<std::size_t>::max() - first_bit_offset) {
		throw_decode("BINARY SEG frame bit window overflows size_t");
	}
	const auto window_bits =
	    pixels_per_frame + static_cast<std::size_t>(first_bit_offset);
	const auto window_bytes =
	    (window_bits / 8u) + ((window_bits % 8u) != 0 ? 1u : 0u);
	if (first_byte > bytes.size() || window_bytes > bytes.size() - first_byte) {
		throw_decode("BINARY SEG PixelData size mismatch");
	}
	return BinaryFrameBitsView{
	    bytes.subspan(first_byte, window_bytes),
	    first_bit_offset,
	    pixels_per_frame,
	    rows_,
	    columns_,
	};
}

void Segmentation::decode_frame_into(
    std::size_t frame_index, std::span<std::uint8_t> out) const {
	if (frame_index >= index_.frames.size()) {
		throw_decode("frame index out of range");
	}

	if (segmentation_type_ == SegmentationType::labelmap) {
		decode_labelmap_frame_into(frame_index, out);
		return;
	}

	file_->ensure_loaded("PixelData"_tag);

	if (segmentation_type_ == SegmentationType::binary) {
		// DICOM BINARY SEG stores one bit per pixel across the multi-frame
		// native PixelData stream. Expose it as one byte per pixel for callers.
		const auto bits = binary_frame_bits(frame_index);
		if (out.size() < bits.bit_count) {
			throw_decode("destination buffer is smaller than decoded frame");
		}
		std::fill_n(out.begin(), bits.bit_count, std::uint8_t{0});
		dicom::seg::for_each_binary_frame_set_bit(bits,
		    [&](std::size_t pixel_index) { out[pixel_index] = 1u; });
		return;
	}

	if (segmentation_type_ == SegmentationType::fractional) {
		// FRACTIONAL SEG commonly stores 8-bit samples with
		// MaximumFractionalValue describing the scale. The MVP keeps this frame
		// by frame and delegates normal native decoding to DicomFile.
		const auto bits_allocated =
		    file_->get_dataelement("BitsAllocated"_tag).to_long().value_or(0);
		if (bits_allocated != 8) {
			throw_decode("FRACTIONAL SEG MVP requires BitsAllocated=8");
		}
		const auto& plan = fractional_decode_plan();
		pixel::DecodeInfo decode_info{};
		file_->decode_into(frame_index, out, plan, decode_info);
		require_lossless_seg_decode(decode_info);
		validate_fractional_frame_samples_or_throw(
		    out, checked_frame_sample_count(rows_, columns_),
		    maximum_fractional_value_);
		return;
	}

	throw_decode("unsupported SegmentationType for frame decode");
}

void Segmentation::decode_labelmap_frame_into(
    std::size_t frame_index, std::span<std::uint8_t> out) const {
	if (segmentation_type_ != SegmentationType::labelmap) {
		throw_decode("decode_labelmap_frame_into requires LABELMAP SEG");
	}
	if (labelmap_bits_allocated_.value_or(0) != 8) {
		throw_decode("LABELMAP uint8 output requires BitsAllocated=8");
	}
	const auto result = decode_and_scan_labelmap_frame(*file_,
	    index_.frames.size(), frame_index, rows_, columns_,
	    labelmap_bits_allocated_.value_or(0), labelmap_valid_labels_,
	    labelmap_background_value_,
	    LabelmapFrameScanRequest{
	        .decoded_u8 = out,
	        .collect_presence = true,
	    });
	std::lock_guard lock(labelmap_cache_mutex_);
	auto& cache = labelmap_presence_cache_[frame_index];
	if (!cache.ready) {
		cache.ready = true;
		cache.labels = result.present_labels;
	}
}

void Segmentation::decode_labelmap_frame_into(
    std::size_t frame_index, std::span<std::uint16_t> out) const {
	if (segmentation_type_ != SegmentationType::labelmap) {
		throw_decode("decode_labelmap_frame_into requires LABELMAP SEG");
	}
	if (labelmap_bits_allocated_.value_or(0) != 16) {
		throw_decode("LABELMAP uint16 output requires BitsAllocated=16");
	}
	const auto result = decode_and_scan_labelmap_frame(*file_,
	    index_.frames.size(), frame_index, rows_, columns_,
	    labelmap_bits_allocated_.value_or(0), labelmap_valid_labels_,
	    labelmap_background_value_,
	    LabelmapFrameScanRequest{
	        .decoded_u16 = out,
	        .collect_presence = true,
	    });
	std::lock_guard lock(labelmap_cache_mutex_);
	auto& cache = labelmap_presence_cache_[frame_index];
	if (!cache.ready) {
		cache.ready = true;
		cache.labels = result.present_labels;
	}
}

std::vector<std::uint8_t>
Segmentation::decode_labelmap_frame_bytes(std::size_t frame_index) const {
	const auto pixels_per_frame = checked_frame_sample_count(rows_, columns_);
	if (labelmap_bits_allocated_.value_or(0) == 8) {
		std::vector<std::uint8_t> out(pixels_per_frame);
		decode_labelmap_frame_into(frame_index, out);
		return out;
	}
	if (labelmap_bits_allocated_.value_or(0) == 16) {
		std::vector<std::uint16_t> samples(pixels_per_frame);
		decode_labelmap_frame_into(frame_index, samples);
		std::vector<std::uint8_t> out(samples.size() * sizeof(std::uint16_t));
		if (!out.empty()) {
			std::memcpy(out.data(), samples.data(), out.size());
		}
		return out;
	}
	throw_decode("LABELMAP requires BitsAllocated=8 or 16");
}

void Segmentation::mask_for_segment_into(std::size_t frame_index,
    std::uint16_t segment_number, std::span<std::uint8_t> out,
    const SegmentMaskOptions& options) const {
	if (frame_index >= index_.frames.size()) {
		throw_decode("frame index out of range");
	}
	const auto pixels_per_frame = checked_frame_sample_count(rows_, columns_);
	if (out.size() < pixels_per_frame) {
		throw_decode("destination buffer is smaller than decoded frame");
	}
	if (options.fractional_threshold < 0.0 || options.fractional_threshold > 1.0) {
		throw_decode("fractional_threshold must be in [0, 1]");
	}
	if (!index_.segment_index_by_number.contains(segment_number)) {
		throw_decode("mask_for_segment requested a segment absent from SegmentSequence");
	}

	const auto fill_zero_or_throw = [&] {
		std::fill_n(out.begin(), pixels_per_frame, std::uint8_t{0});
		if (options.error_when_not_present_in_frame) {
			throw_decode("segment is not present in this frame");
		}
	};

	if (segmentation_type_ == SegmentationType::binary) {
		const auto referenced = index_.frames[frame_index].referenced_segment_number;
		if (referenced != segment_number || referenced == 0) {
			fill_zero_or_throw();
			return;
		}
		decode_frame_into(frame_index, out);
		return;
	}

	if (segmentation_type_ == SegmentationType::fractional) {
		const auto referenced = index_.frames[frame_index].referenced_segment_number;
		if (referenced != segment_number || referenced == 0) {
			fill_zero_or_throw();
			return;
		}
		decode_frame_into(frame_index, out);
		const auto maximum = maximum_fractional_value_.value_or(0);
		if (maximum == 0) {
			throw_decode("FRACTIONAL SEG requires MaximumFractionalValue");
		}
		if (options.fractional_threshold == 0.0) {
			for (std::size_t index = 0; index < pixels_per_frame; ++index) {
				out[index] = static_cast<std::uint8_t>(out[index] > 0 ? 1u : 0u);
			}
			return;
		}
		const auto threshold = options.fractional_threshold *
		    static_cast<double>(maximum);
		for (std::size_t index = 0; index < pixels_per_frame; ++index) {
			out[index] = static_cast<std::uint8_t>(
			    static_cast<double>(out[index]) >= threshold ? 1u : 0u);
		}
		return;
	}

	if (segmentation_type_ == SegmentationType::labelmap) {
		std::shared_ptr<const std::vector<std::uint16_t>> cached_labels{};
		{
			std::lock_guard lock(labelmap_cache_mutex_);
			const auto& cache = labelmap_presence_cache_[frame_index];
			if (cache.ready) {
				cached_labels = cache.labels;
			}
		}
		const auto cached_target_present =
		    cached_labels && std::binary_search(cached_labels->begin(),
		                         cached_labels->end(), segment_number);
		const auto target_is_background =
		    labelmap_background_value_ && segment_number == *labelmap_background_value_;
		if (cached_labels && !target_is_background && !cached_target_present) {
			fill_zero_or_throw();
			return;
		}

		const auto result = decode_and_scan_labelmap_frame(*file_,
		    index_.frames.size(), frame_index, rows_, columns_,
		    labelmap_bits_allocated_.value_or(0), labelmap_valid_labels_,
		    labelmap_background_value_,
		    LabelmapFrameScanRequest{
		        .mask_out = out,
		        .target_segment = segment_number,
		        .collect_presence = !cached_labels,
		    });
		if (!cached_labels) {
			std::lock_guard lock(labelmap_cache_mutex_);
			auto& cache = labelmap_presence_cache_[frame_index];
			if (!cache.ready) {
				cache.ready = true;
				cache.labels = result.present_labels;
			}
		}
		if (options.error_when_not_present_in_frame && !result.target_seen) {
			throw_decode("segment is not present in this frame");
		}
		return;
	}

	throw_decode("unsupported SegmentationType for mask decode");
}

std::vector<std::uint8_t> Segmentation::mask_for_segment(
    std::size_t frame_index, std::uint16_t segment_number,
    const SegmentMaskOptions& options) const {
	std::vector<std::uint8_t> out(checked_frame_sample_count(rows_, columns_));
	mask_for_segment_into(frame_index, segment_number, out, options);
	return out;
}

void Segmentation::validate_label_values() const {
	if (segmentation_type_ == SegmentationType::labelmap) {
		(void)ensure_labelmap_frame_index();
		return;
	}
	if (segmentation_type_ == SegmentationType::fractional) {
		std::vector<std::uint8_t> decoded(
		    checked_frame_sample_count(rows_, columns_));
		for (std::size_t frame_index = 0; frame_index < index_.frames.size();
		     ++frame_index) {
			decode_frame_into(frame_index, decoded);
		}
	}
}

SegmentListView::SegmentListView(const Segmentation* segmentation) noexcept
    : segmentation_(segmentation) {}

std::size_t SegmentListView::size() const noexcept {
	return segmentation_ ? segmentation_->index_.segments.size() : 0u;
}

bool SegmentListView::empty() const noexcept {
	return size() == 0;
}

SegmentView SegmentListView::operator[](std::size_t index) const {
	if (!segmentation_ || index >= segmentation_->index_.segments.size()) {
		diag::throw_exception("seg::SegmentListView::operator[] index out of range");
	}
	const auto& segment = segmentation_->index_.segments[index];
	return SegmentView(segment.item, segment.number);
}

SegmentListView::iterator SegmentListView::begin() const noexcept {
	return iterator(segmentation_, 0);
}

SegmentListView::iterator SegmentListView::end() const noexcept {
	return iterator(segmentation_, size());
}

SegmentListView::iterator::iterator(
    const Segmentation* segmentation, std::size_t index) noexcept
    : segmentation_(segmentation), index_(index) {}

SegmentView SegmentListView::iterator::operator*() const {
	return SegmentListView(segmentation_)[index_];
}

SegmentListView::iterator& SegmentListView::iterator::operator++() noexcept {
	++index_;
	return *this;
}

SegmentListView::iterator SegmentListView::iterator::operator++(int) noexcept {
	auto copy = *this;
	++(*this);
	return copy;
}

bool SegmentListView::iterator::operator==(const iterator& rhs) const noexcept {
	return segmentation_ == rhs.segmentation_ && index_ == rhs.index_;
}

SegmentFrameListView::SegmentFrameListView(const Segmentation* segmentation,
    const std::vector<std::size_t>* frame_indices) noexcept
    : segmentation_(segmentation), frame_indices_(frame_indices) {}

std::size_t SegmentFrameListView::size() const noexcept {
	if (frame_indices_) {
		return frame_indices_->size();
	}
	return segmentation_ ? segmentation_->index_.frames.size() : 0u;
}

bool SegmentFrameListView::empty() const noexcept {
	return size() == 0;
}

SegmentFrameView SegmentFrameListView::operator[](std::size_t ordinal) const {
	return SegmentFrameView(segmentation_, frame_index_from_ordinal(ordinal));
}

std::size_t SegmentFrameListView::frame_index_from_ordinal(
    std::size_t ordinal) const {
	if (frame_indices_) {
		if (ordinal >= frame_indices_->size()) {
			diag::throw_exception("seg::SegmentFrameListView::operator[] index out of range");
		}
		return (*frame_indices_)[ordinal];
	}
	if (!segmentation_) {
		diag::throw_exception("seg::SegmentFrameListView::operator[] invalid view");
	}
	if (ordinal >= segmentation_->index_.frames.size()) {
		diag::throw_exception("seg::SegmentFrameListView::operator[] index out of range");
	}
	return ordinal;
}

SegmentFrameListView::iterator SegmentFrameListView::begin() const noexcept {
	return iterator(segmentation_, frame_indices_, 0);
}

SegmentFrameListView::iterator SegmentFrameListView::end() const noexcept {
	return iterator(segmentation_, frame_indices_, size());
}

SegmentFrameListView::iterator::iterator(const Segmentation* segmentation,
    const std::vector<std::size_t>* frame_indices, std::size_t ordinal) noexcept
    : segmentation_(segmentation),
      frame_indices_(frame_indices),
      ordinal_(ordinal) {}

SegmentFrameView SegmentFrameListView::iterator::operator*() const {
	return SegmentFrameListView(segmentation_, frame_indices_)[ordinal_];
}

SegmentFrameListView::iterator& SegmentFrameListView::iterator::operator++() noexcept {
	++ordinal_;
	return *this;
}

SegmentFrameListView::iterator SegmentFrameListView::iterator::operator++(int) noexcept {
	auto copy = *this;
	++(*this);
	return copy;
}

bool SegmentFrameListView::iterator::operator==(const iterator& rhs) const noexcept {
	return segmentation_ == rhs.segmentation_ &&
	    frame_indices_ == rhs.frame_indices_ && ordinal_ == rhs.ordinal_;
}

SegmentView::SegmentView(const DataSet* item, std::uint16_t number) noexcept
    : item_(item), number_(number) {}

std::uint16_t SegmentView::number() const noexcept {
	return number_;
}

std::string_view SegmentView::label() const {
	return item_ ? string_value_or_empty(*item_, "SegmentLabel"_tag) : std::string_view{};
}

std::string_view SegmentView::description() const {
	return item_ ? string_value_or_empty(*item_, "SegmentDescription"_tag)
	             : std::string_view{};
}

std::string_view SegmentView::algorithm_name() const {
	return item_ ? string_value_or_empty(*item_, "SegmentAlgorithmName"_tag)
	             : std::string_view{};
}

SegmentAlgorithmType SegmentView::algorithm_type() const {
	return item_ ? parse_algorithm_type(
	                   string_value_or_empty(*item_, "SegmentAlgorithmType"_tag))
	             : SegmentAlgorithmType::unknown;
}

std::optional<CodeView> SegmentView::property_category() const {
	return item_ ? code_view_from_sequence(
	                   *item_, "SegmentedPropertyCategoryCodeSequence"_tag)
	             : std::nullopt;
}

std::optional<CodeView> SegmentView::property_type() const {
	return item_ ? code_view_from_sequence(*item_, "SegmentedPropertyTypeCodeSequence"_tag)
	             : std::nullopt;
}

std::optional<CodeView> SegmentView::anatomic_region() const {
	return item_ ? code_view_from_sequence(*item_, "AnatomicRegionSequence"_tag)
	             : std::nullopt;
}

std::optional<std::array<std::uint16_t, 3>>
SegmentView::recommended_display_cielab() const {
	if (!item_) {
		return std::nullopt;
	}
	const auto values =
	    item_->get_dataelement("RecommendedDisplayCIELabValue"_tag).to_long_vector();
	if (!values || values->size() < 3) {
		return std::nullopt;
	}
	std::array<std::uint16_t, 3> out{};
	for (std::size_t index = 0; index < out.size(); ++index) {
		const auto value = (*values)[index];
		if (value < 0 || value > std::numeric_limits<std::uint16_t>::max()) {
			return std::nullopt;
		}
		out[index] = static_cast<std::uint16_t>(value);
	}
	return out;
}

const DataSet& SegmentView::dataset() const noexcept {
	return *item_;
}

SegmentFrameView::SegmentFrameView(
    const Segmentation* segmentation, std::size_t frame_index) noexcept
    : segmentation_(segmentation), frame_index_(frame_index) {}

std::size_t SegmentFrameView::index() const noexcept {
	return frame_index_;
}

std::uint16_t SegmentFrameView::referenced_segment_number() const {
	if (!segmentation_ || frame_index_ >= segmentation_->index_.frames.size()) {
		diag::throw_exception("seg::SegmentFrameView::referenced_segment_number invalid frame");
	}
	if (segmentation_->segmentation_type_ == SegmentationType::labelmap) {
		diag::throw_exception(
		    "seg::SegmentFrameView::referenced_segment_number is not defined for LABELMAP frames; use present_segment_numbers()");
	}
	if (segmentation_->index_.frames[frame_index_].referenced_segment_number == 0) {
		diag::throw_exception(
		    "seg::SegmentFrameView::referenced_segment_number is missing");
	}
	return segmentation_->index_.frames[frame_index_].referenced_segment_number;
}

std::span<const std::uint16_t>
SegmentFrameView::present_segment_numbers() const {
	if (!segmentation_) {
		diag::throw_exception("seg::SegmentFrameView::present_segment_numbers invalid frame");
	}
	return segmentation_->present_segment_numbers(frame_index_);
}

std::optional<std::array<double, 3>>
SegmentFrameView::image_position_patient() const {
	const auto& frame_item = per_frame_functional_groups_item();
	const auto* plane_position_item = functional_group_first_item(
	    frame_item, segmentation_->shared_functional_groups_item_,
	    "PlanePositionSequence"_tag);
	return plane_position_item
	           ? double_array_value<3>(*plane_position_item, "ImagePositionPatient"_tag)
	           : std::nullopt;
}

std::optional<std::array<double, 6>>
SegmentFrameView::image_orientation_patient() const {
	const auto& frame_item = per_frame_functional_groups_item();
	const auto* plane_orientation_item = functional_group_first_item(
	    frame_item, segmentation_->shared_functional_groups_item_,
	    "PlaneOrientationSequence"_tag);
	return plane_orientation_item
	           ? double_array_value<6>(*plane_orientation_item, "ImageOrientationPatient"_tag)
	           : std::nullopt;
}

std::optional<std::array<double, 2>> SegmentFrameView::pixel_spacing() const {
	const auto& frame_item = per_frame_functional_groups_item();
	const auto* pixel_measures_item = functional_group_first_item(
	    frame_item, segmentation_->shared_functional_groups_item_,
	    "PixelMeasuresSequence"_tag);
	return pixel_measures_item
	           ? double_array_value<2>(*pixel_measures_item, "PixelSpacing"_tag)
	           : std::nullopt;
}

std::optional<double> SegmentFrameView::slice_thickness() const {
	const auto& frame_item = per_frame_functional_groups_item();
	const auto* pixel_measures_item = functional_group_first_item(
	    frame_item, segmentation_->shared_functional_groups_item_,
	    "PixelMeasuresSequence"_tag);
	return pixel_measures_item
	           ? pixel_measures_item->get_dataelement("SliceThickness"_tag).to_double()
	           : std::nullopt;
}

SourceImageRefListView SegmentFrameView::source_images() const {
	return SourceImageRefListView(segmentation_, frame_index_);
}

const DataSet& SegmentFrameView::per_frame_functional_groups_item() const {
	if (!segmentation_ || frame_index_ >= segmentation_->index_.frames.size() ||
	    !segmentation_->index_.frames[frame_index_].functional_group_item) {
		diag::throw_exception("seg::SegmentFrameView::per_frame_functional_groups_item missing");
	}
	return *segmentation_->index_.frames[frame_index_].functional_group_item;
}

std::vector<std::uint8_t> SegmentFrameView::mask_for_segment(
    std::uint16_t segment_number, const SegmentMaskOptions& options) const {
	if (!segmentation_) {
		diag::throw_exception("seg::SegmentFrameView::mask_for_segment invalid frame");
	}
	return segmentation_->mask_for_segment(frame_index_, segment_number, options);
}

void SegmentFrameView::mask_for_segment_into(std::uint16_t segment_number,
    std::span<std::uint8_t> out, const SegmentMaskOptions& options) const {
	if (!segmentation_) {
		diag::throw_exception("seg::SegmentFrameView::mask_for_segment_into invalid frame");
	}
	segmentation_->mask_for_segment_into(frame_index_, segment_number, out, options);
}

SourceImageRefListView::SourceImageRefListView(
    const Segmentation* segmentation, std::size_t frame_index) noexcept
    : segmentation_(segmentation), frame_index_(frame_index) {}

std::size_t SourceImageRefListView::size() const noexcept {
	if (!segmentation_ || frame_index_ >= segmentation_->index_.frames.size()) {
		return 0;
	}
	return segmentation_->source_image_refs_for_frame(frame_index_).size();
}

bool SourceImageRefListView::empty() const noexcept {
	return size() == 0;
}

SourceImageRefView SourceImageRefListView::operator[](std::size_t index) const {
	if (!segmentation_ || frame_index_ >= segmentation_->index_.frames.size()) {
		diag::throw_exception("seg::SourceImageRefListView::operator[] invalid frame");
	}
	const auto& refs = segmentation_->source_image_refs_for_frame(frame_index_);
	if (index >= refs.size()) {
		diag::throw_exception("seg::SourceImageRefListView::operator[] index out of range");
	}
	const auto& ref = refs[index];
	return SourceImageRefView(ref.item,
	    std::span<const std::uint32_t>(
	        ref.referenced_frame_numbers.data(), ref.referenced_frame_numbers.size()));
}

SourceImageRefView::SourceImageRefView(const DataSet* item,
    std::span<const std::uint32_t> referenced_frame_numbers) noexcept
    : item_(item), referenced_frame_numbers_(referenced_frame_numbers) {}

std::string_view SourceImageRefView::sop_class_uid() const {
	return item_ ? string_value_or_empty(*item_, "ReferencedSOPClassUID"_tag)
	             : std::string_view{};
}

std::string_view SourceImageRefView::sop_instance_uid() const {
	return item_ ? string_value_or_empty(*item_, "ReferencedSOPInstanceUID"_tag)
	             : std::string_view{};
}

std::span<const std::uint32_t>
SourceImageRefView::referenced_frame_numbers() const noexcept {
	return referenced_frame_numbers_;
}

const DataSet& SourceImageRefView::dataset() const noexcept {
	return *item_;
}

bool is_segmentation_storage(const DataSet& ds) noexcept {
	try {
		return declared_sop_class_kind(ds) == SopClassKind::segmentation;
	} catch (...) {
		return false;
	}
}

bool is_segmentation_storage(const DicomFile& file) noexcept {
	return is_segmentation_storage(file.dataset());
}

bool is_labelmap_segmentation_storage(const DataSet& ds) noexcept {
	try {
		return declared_sop_class_kind(ds) == SopClassKind::labelmap;
	} catch (...) {
		return false;
	}
}

bool is_labelmap_segmentation_storage(const DicomFile& file) noexcept {
	return is_labelmap_segmentation_storage(file.dataset());
}

bool is_any_segmentation_storage(const DataSet& ds) noexcept {
	try {
		const auto kind = declared_sop_class_kind(ds);
		return kind == SopClassKind::segmentation || kind == SopClassKind::labelmap;
	} catch (...) {
		return false;
	}
}

bool is_any_segmentation_storage(const DicomFile& file) noexcept {
	return is_any_segmentation_storage(file.dataset());
}

std::unique_ptr<Segmentation> from_dicomfile(
    std::unique_ptr<DicomFile> file, const Options& options) {
	// Higher-level modules adapt an already parsed DicomFile instead of owning a
	// separate read path. That keeps file/byte error policy in the core reader.
	if (!file) {
		throw_seg("input DicomFile is null");
	}
	if (file->has_error() && !options.allow_partial_source) {
		throw_seg("input DicomFile has a read error");
	}
	const auto sop_kind = declared_sop_class_kind(file->dataset());
	if (sop_kind == SopClassKind::conflict) {
		throw_seg("SOPClassUID and MediaStorageSOPClassUID disagree");
	}
	if (sop_kind != SopClassKind::segmentation &&
	    sop_kind != SopClassKind::labelmap) {
		throw_seg("input DicomFile is not Segmentation Storage");
	}

	auto segmentation =
	    std::unique_ptr<Segmentation>(new Segmentation(std::move(file)));
	// Build only metadata indexes here. Pixel data is decoded lazily through
	// decode_frame_into(), preserving the cost model of DicomFile.
	segmentation->extract_instance_metadata(options);
	segmentation->index_segment_sequence_items(options);
	segmentation->index_per_frame_functional_group_items(options);
	if (segmentation->segmentation_type_ == SegmentationType::labelmap) {
		segmentation->labelmap_presence_cache_.resize(
		    segmentation->index_.frames.size());
	}
	return segmentation;
}

std::unique_ptr<Segmentation> read_file(
    const std::filesystem::path& path,
    ReadOptions read_options, Options options) {
	return from_dicomfile(dicom::read_file(path, read_options), options);
}

std::unique_ptr<Segmentation> read_bytes(
    const std::uint8_t* data, std::size_t size,
    ReadOptions read_options, Options options) {
	return from_dicomfile(dicom::read_bytes(data, size, read_options), options);
}

std::unique_ptr<Segmentation> read_bytes(
    const std::string& name, const std::uint8_t* data, std::size_t size,
    ReadOptions read_options, Options options) {
	return from_dicomfile(
	    dicom::read_bytes(name, data, size, read_options), options);
}

std::unique_ptr<Segmentation> read_bytes(
    std::string name, std::vector<std::uint8_t>&& buffer,
    ReadOptions read_options, Options options) {
	return from_dicomfile(
	    dicom::read_bytes(std::move(name), std::move(buffer), read_options),
	    options);
}

} // namespace dicom::seg
