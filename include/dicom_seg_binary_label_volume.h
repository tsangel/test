#pragma once

#include "dicom_seg.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dicom::seg {

/// Dense runtime label id used by packed BINARY SEG label volumes.
/// Label id 0 is reserved for background; 1..N map to DICOM SegmentNumber
/// through the source segment table.
using BinaryLabelId = std::uint16_t;

/// Stored code in a packed BINARY SEG label volume.
/// 0 means background, 1..single_label_code_end are direct single-label codes,
/// and larger codes refer to overlap label sets in BinaryLabelCodeTable.
using BinaryLabelCode = std::uint16_t;

constexpr BinaryLabelCode kBinaryLabelBackgroundCode = 0;
constexpr BinaryLabelCode kBinaryLabelMaxCode =
    std::numeric_limits<BinaryLabelCode>::max();
constexpr std::size_t kBinaryLabelMaxOverlapSetSize = 128;

struct BinaryLabelRgba8 {
	std::uint8_t r{0};
	std::uint8_t g{0};
	std::uint8_t b{0};
	std::uint8_t a{0};

	friend bool operator==(
	    const BinaryLabelRgba8& lhs,
	    const BinaryLabelRgba8& rhs) = default;
};

struct BinaryLabelVolumeSize {
	std::size_t columns{0};
	std::size_t rows{0};
	std::size_t slices{0};
};

enum class BinaryLabelSetViewKind : std::uint8_t {
	empty,
	single,
	span,
};

/// Small view over a label set represented by one label code.
/// Single-label codes are represented without table storage.
class BinaryLabelSetView final {
public:
	static BinaryLabelSetView empty();
	static BinaryLabelSetView single(BinaryLabelId label_id);
	static BinaryLabelSetView span(std::span<const BinaryLabelId> label_set);

	[[nodiscard]] BinaryLabelSetViewKind kind() const noexcept { return kind_; }
	[[nodiscard]] bool is_empty() const noexcept { return size() == 0; }
	[[nodiscard]] std::size_t size() const noexcept;
	[[nodiscard]] BinaryLabelId operator[](std::size_t index) const;
	[[nodiscard]] bool contains(BinaryLabelId label_id) const;

private:
	BinaryLabelSetView() = default;

	BinaryLabelSetViewKind kind_{BinaryLabelSetViewKind::empty};
	BinaryLabelId single_label_id_{0};
	std::span<const BinaryLabelId> label_set_{};
};

struct BinaryLabelSetRef {
	std::uint32_t offset{0};
	std::uint16_t count{0};

	[[nodiscard]] bool empty() const noexcept { return count == 0; }
};

struct BinaryOverlapLabelSetEntry {
	BinaryLabelCode label_code{0};
	BinaryLabelSetRef label_set{};
};

struct BinaryLabelCodeTableReserve {
	std::size_t estimated_unique_overlap_label_set_count{0};
	std::size_t estimated_overlap_label_id_total{0};
	std::size_t estimated_2_to_4_label_set_count{0};
	std::size_t estimated_5_to_32_label_set_count{0};
	std::size_t estimated_33_to_128_label_set_count{0};
};

struct BinaryLabelCodeTelemetry {
	std::uint64_t unique_overlap_label_set_count{0};
	std::uint64_t overlap_label_set_count_2_to_4{0};
	std::uint64_t overlap_label_set_count_5_to_32{0};
	std::uint64_t overlap_label_set_count_33_to_128{0};
	std::uint64_t reused_overlap_label_set_count{0};
	std::uint64_t rejected_over_128_label_set_count{0};
	std::uint64_t label_code_range_exhaustion_count{0};
};

enum class BinaryLabelSegmentsOverlap : std::uint8_t {
	no,
	yes,
	undefined,
};

struct BinaryLabelFrame {
	std::size_t source_frame_index{0};
	BinaryLabelId label_id{0};
	std::size_t slice_index{0};
	std::optional<std::array<double, 3>> image_position_patient{};
};

using BinaryFrameSetBitVisitor = void (*)(std::size_t pixel_index,
    void* user_data);
using BinaryFrameSetBitVisitFunction = void (*)(
    const void* context,
    std::size_t frame_ordinal,
    BinaryFrameSetBitVisitor visitor,
    void* user_data);

/// Abstract source of BINARY SEG frame set bits for label-volume packing.
/// The source does not own the spans it exposes; callers must keep backing
/// storage alive for the duration of build_binary_label_volume_into().
class BinaryFrameSetBitSource final {
public:
	BinaryFrameSetBitSource() = default;
	BinaryFrameSetBitSource(
	    BinaryLabelVolumeSize size,
	    std::span<const std::uint16_t> source_dicom_segment_numbers,
	    std::span<const BinaryLabelFrame> frames,
	    const void* context,
	    BinaryFrameSetBitVisitFunction visit_set_bits);

	[[nodiscard]] BinaryLabelVolumeSize size() const noexcept { return size_; }
	[[nodiscard]] std::span<const std::uint16_t>
	source_dicom_segment_numbers() const noexcept {
		return source_dicom_segment_numbers_;
	}
	[[nodiscard]] std::span<const BinaryLabelFrame> frames() const noexcept {
		return frames_;
	}
	void for_each_frame_set_bit(std::size_t frame_ordinal,
	    BinaryFrameSetBitVisitor visitor, void* user_data) const;

private:
	BinaryLabelVolumeSize size_{};
	std::span<const std::uint16_t> source_dicom_segment_numbers_{};
	std::span<const BinaryLabelFrame> frames_{};
	const void* context_{nullptr};
	BinaryFrameSetBitVisitFunction visit_set_bits_{nullptr};
};

template <std::size_t N>
struct BinaryInlineLabelSetKey {
	std::array<BinaryLabelId, N> label_ids{};

	friend bool operator==(const BinaryInlineLabelSetKey& lhs,
	    const BinaryInlineLabelSetKey& rhs) noexcept {
		return lhs.label_ids == rhs.label_ids;
	}
};

template <std::size_t N>
struct BinaryInlineLabelSetKeyHash {
	std::size_t operator()(
	    const BinaryInlineLabelSetKey<N>& key) const noexcept {
		std::uint64_t hash = 1469598103934665603ull;
		for (BinaryLabelId label_id : key.label_ids) {
			hash ^= static_cast<std::uint64_t>(label_id & 0x00ffu);
			hash *= 1099511628211ull;
			hash ^= static_cast<std::uint64_t>(label_id >> 8u);
			hash *= 1099511628211ull;
		}
		return static_cast<std::size_t>(hash);
	}
};

template <>
struct BinaryInlineLabelSetKeyHash<4> {
	std::size_t operator()(
	    const BinaryInlineLabelSetKey<4>& key) const noexcept {
		static_assert(sizeof(key.label_ids) == sizeof(std::uint64_t));
		const std::uint64_t packed =
		    std::bit_cast<std::uint64_t>(key.label_ids);
		return std::hash<std::uint64_t>{}(packed);
	}
};

template <std::size_t N>
BinaryInlineLabelSetKey<N> make_binary_inline_label_set_key(
    std::span<const BinaryLabelId> label_set) {
	if (label_set.size() > N) {
		throw std::invalid_argument("label_set is too large for inline key");
	}
	BinaryInlineLabelSetKey<N> key;
	std::copy(label_set.begin(), label_set.end(), key.label_ids.begin());
	return key;
}

/// Maps packed label codes to semantic label sets and maintains reverse
/// label_id -> label_code lists for fast mask restoration.
class BinaryLabelCodeTable final {
public:
	static BinaryLabelCodeTable create(
	    BinaryLabelCode single_label_code_end,
	    BinaryLabelCodeTableReserve reserve = {});

	[[nodiscard]] BinaryLabelCode single_label_code_end() const noexcept {
		return single_label_code_end_;
	}
	[[nodiscard]] bool is_valid_label_id(BinaryLabelId label_id) const noexcept;
	[[nodiscard]] bool is_single_label_code(
	    BinaryLabelCode label_code) const noexcept;
	[[nodiscard]] bool is_overlap_label_code(
	    BinaryLabelCode label_code) const;
	[[nodiscard]] BinaryLabelSetView label_set_by_label_code(
	    BinaryLabelCode label_code) const;
	[[nodiscard]] std::span<const BinaryLabelCode> label_codes_for_label_id(
	    BinaryLabelId label_id) const noexcept;

	BinaryLabelCode overlap_label_code_for_two_single_labels(
	    BinaryLabelId a, BinaryLabelId b);
	BinaryLabelCode overlap_label_code_for_label_set(
	    std::span<const BinaryLabelId> label_set);
	BinaryLabelCode overlap_label_code_after_adding_absent_label(
	    BinaryLabelCode overlap_label_code, BinaryLabelId label_id);
	BinaryLabelCode overlap_label_code_after_maybe_adding_label(
	    BinaryLabelCode overlap_label_code, BinaryLabelId label_id);

	[[nodiscard]] const BinaryLabelCodeTelemetry& telemetry() const noexcept {
		return telemetry_;
	}
	[[nodiscard]] std::size_t label_set_ref_table_size() const noexcept {
		return label_set_ref_by_label_code_.size();
	}
	[[nodiscard]] std::size_t label_set_storage_size() const noexcept {
		return label_set_storage_.size();
	}
	[[nodiscard]] std::size_t overlap_entry_count() const noexcept {
		return overlap_entries_.size();
	}
	[[nodiscard]] std::size_t label_code_count() const noexcept {
		return static_cast<std::size_t>(single_label_code_end_) + 1u +
		    overlap_entries_.size();
	}

private:
	using LabelSetCodeCache4 =
	    std::unordered_map<BinaryInlineLabelSetKey<4>,
	        BinaryLabelCode, BinaryInlineLabelSetKeyHash<4>>;
	using LabelSetCodeCache32 =
	    std::unordered_map<BinaryInlineLabelSetKey<32>,
	        BinaryLabelCode, BinaryInlineLabelSetKeyHash<32>>;
	using LabelSetCodeCache128 =
	    std::unordered_map<BinaryInlineLabelSetKey<128>,
	        BinaryLabelCode, BinaryInlineLabelSetKeyHash<128>>;

	BinaryLabelCode intern_overlap_label_set(
	    std::span<const BinaryLabelId> label_set);
	BinaryLabelSetRef append_label_set(
	    std::span<const BinaryLabelId> label_set);

	BinaryLabelCode single_label_code_end_{0};
	std::vector<BinaryLabelId> label_set_storage_{};
	std::vector<BinaryLabelSetRef> label_set_ref_by_label_code_{};
	std::vector<std::vector<BinaryLabelCode>> label_codes_by_label_id_{};
	std::vector<BinaryOverlapLabelSetEntry> overlap_entries_{};
	LabelSetCodeCache4 label_set_code_cache_4_{};
	LabelSetCodeCache32 label_set_code_cache_32_{};
	LabelSetCodeCache128 label_set_code_cache_128_{};
	std::vector<BinaryLabelId> label_set_scratch_{};
	BinaryLabelCodeTelemetry telemetry_{};
};

/// Generation-marked dense label-code set for repeated mask restoration.
class BinaryLabelCodeSet final {
public:
	BinaryLabelCodeSet();

	void clear();
	void include(BinaryLabelCode label_code);
	void include_all(std::span<const BinaryLabelCode> label_codes);
	void reset_to_label_id(
	    const BinaryLabelCodeTable& code_table, BinaryLabelId label_id);
	[[nodiscard]] bool contains(BinaryLabelCode label_code) const noexcept;

private:
	std::vector<std::uint32_t> marks_{};
	std::uint32_t generation_{1};
};

struct BinaryLabelVolumeOptions {
	/// 0 means "use source segment count". Larger values reserve direct
	/// single-label codes for app-side editing before overlap codes begin.
	BinaryLabelCode single_label_code_end{0};
	BinaryLabelCodeTableReserve reserve{};
};

struct BinaryLabelSourceFrameMapEntry {
	std::size_t source_frame_index{0};
	std::uint16_t dicom_segment_number{0};
	BinaryLabelId label_id{0};
	std::size_t slice_index{0};
	std::uint64_t non_empty_pixel_count{0};
	std::optional<std::array<double, 3>> image_position_patient{};
};

/// Owning convenience result. Apps that already own a CPU/GPU staging buffer can
/// call build_binary_label_volume_into() instead.
struct BinaryLabelVolume {
	BinaryLabelVolumeSize size{};
	std::vector<BinaryLabelCode> label_volume{};
	BinaryLabelCodeTable code_table{};
	std::vector<std::uint16_t> source_dicom_segment_by_label_id{};
	std::vector<BinaryLabelSourceFrameMapEntry> source_frame_map{};
};

struct BinaryLabelVolumeBuildTelemetry {
	std::uint64_t source_frame_count{0};
	std::uint64_t visited_set_bit_count{0};
	std::uint64_t non_empty_frame_count{0};
};

struct BinaryLabelVolumeBuildOptions {
	BinaryLabelVolumeOptions volume_options{};
	BinaryLabelSegmentsOverlap segments_overlap{
	    BinaryLabelSegmentsOverlap::undefined};
	BinaryLabelVolumeBuildTelemetry* telemetry{nullptr};
};

struct BinaryLabelVolumeBuildTarget {
	BinaryLabelVolumeSize size{};
	std::span<BinaryLabelCode> label_volume{};
	BinaryLabelCodeTable* code_table{nullptr};
	std::span<const std::uint16_t> source_dicom_segment_by_label_id{};
	std::vector<BinaryLabelSourceFrameMapEntry>* source_frame_map{nullptr};
};

[[nodiscard]] std::size_t binary_label_volume_voxel_count(
    BinaryLabelVolumeSize size);
[[nodiscard]] BinaryLabelCodeTable create_binary_label_code_table(
    std::span<const std::uint16_t> source_dicom_segment_numbers,
    const BinaryLabelVolumeOptions& options = {});
[[nodiscard]] std::vector<std::uint16_t>
make_binary_label_source_segment_table(
    std::span<const std::uint16_t> source_dicom_segment_numbers,
    BinaryLabelCode single_label_code_end);
void clear_binary_label_volume(std::span<BinaryLabelCode> label_volume);

[[nodiscard]] BinaryLabelVolume create_empty_binary_label_volume(
    BinaryLabelVolumeSize size,
    std::span<const std::uint16_t> source_dicom_segment_numbers,
    BinaryLabelVolumeOptions options = {});
void build_binary_label_volume_into(
    const BinaryFrameSetBitSource& source,
    BinaryLabelVolumeBuildTarget target,
    BinaryLabelVolumeBuildOptions options = {});
[[nodiscard]] BinaryLabelVolume build_binary_label_volume(
    const BinaryFrameSetBitSource& source,
    BinaryLabelVolumeBuildOptions options = {});

void restore_binary_label_mask(std::span<const BinaryLabelCode> label_volume,
    const BinaryLabelCodeTable& code_table, BinaryLabelId label_id,
    std::span<std::uint8_t> mask_out);
void restore_binary_label_mask(std::span<const BinaryLabelCode> label_volume,
    const BinaryLabelCodeTable& code_table, BinaryLabelId label_id,
    BinaryLabelCodeSet& label_code_set, std::span<std::uint8_t> mask_out);

/// Fill a dense label-code -> RGBA table for CPU/GPU display.
/// The resolver decides colors and overlap blending policy for every non-empty
/// label set. `out_lut` must contain at least code_table.label_code_count()
/// RGBA entries. Applications can pad or reshape the result for their GPU API.
template <class ResolveColor>
void build_binary_label_rgba8_lut(
    const BinaryLabelCodeTable& code_table,
    std::span<BinaryLabelRgba8> out_lut,
    ResolveColor&& resolve_color,
    BinaryLabelRgba8 background_color = {}) {
	const auto required_size = code_table.label_code_count();
	if (out_lut.size() < required_size) {
		throw std::invalid_argument(
		    "BINARY SEG RGBA LUT does not cover used label codes");
	}

	std::fill(out_lut.begin(), out_lut.begin() + required_size,
	    background_color);
	auto&& resolver = resolve_color;
	for (std::size_t code = 1; code < required_size; ++code) {
		const auto label_code = static_cast<BinaryLabelCode>(code);
		const BinaryLabelSetView label_set =
		    code_table.label_set_by_label_code(label_code);
		if (!label_set.is_empty()) {
			out_lut[code] = resolver(label_code, label_set);
		}
	}
}

/// Fast path for sources declaring SegmentsOverlap=NO.
template <class VisitSetVoxel>
void paint_no_overlap_label_into_volume(
    std::span<BinaryLabelCode> label_volume,
    BinaryLabelId label_id,
    BinaryLabelCodeTable& code_table,
    VisitSetVoxel&& visit_set_voxel) {
	if (!code_table.is_valid_label_id(label_id)) {
		throw std::invalid_argument("paint_no_overlap label_id is out of range");
	}
	std::forward<VisitSetVoxel>(visit_set_voxel)([&](std::size_t voxel_index) {
		if (voxel_index >= label_volume.size()) {
			throw std::out_of_range("paint_no_overlap voxel index out of range");
		}
		BinaryLabelCode& current = label_volume[voxel_index];
		if (current == kBinaryLabelBackgroundCode) {
			current = label_id;
			return;
		}
		if (current == label_id) {
			throw std::invalid_argument(
			    "BINARY SEG duplicate same-label voxel in no-overlap path");
		}
		throw std::invalid_argument(
		    "BINARY SEG overlap found despite SegmentsOverlap=NO");
	});
}

/// Fast path when the caller knows this label has not been painted yet.
template <class VisitSetVoxel>
void paint_absent_label_into_volume(
    std::span<BinaryLabelCode> label_volume,
    BinaryLabelId label_id,
    BinaryLabelCodeTable& code_table,
    VisitSetVoxel&& visit_set_voxel) {
	if (!code_table.is_valid_label_id(label_id)) {
		throw std::invalid_argument("paint_absent label_id is out of range");
	}
	std::forward<VisitSetVoxel>(visit_set_voxel)([&](std::size_t voxel_index) {
		if (voxel_index >= label_volume.size()) {
			throw std::out_of_range("paint_absent voxel index out of range");
		}
		BinaryLabelCode& current = label_volume[voxel_index];
		if (current == kBinaryLabelBackgroundCode) {
			current = label_id;
			return;
		}
		if (code_table.is_single_label_code(current)) {
			current = code_table.overlap_label_code_for_two_single_labels(
			    current, label_id);
			return;
		}
		current = code_table.overlap_label_code_after_adding_absent_label(
		    current, label_id);
	});
}

/// Slower path when this label may already be present in destination voxels.
template <class VisitSetVoxel>
void paint_maybe_present_label_into_volume(
    std::span<BinaryLabelCode> label_volume,
    BinaryLabelId label_id,
    BinaryLabelCodeTable& code_table,
    VisitSetVoxel&& visit_set_voxel) {
	if (!code_table.is_valid_label_id(label_id)) {
		throw std::invalid_argument("paint_maybe_present label_id is out of range");
	}
	std::forward<VisitSetVoxel>(visit_set_voxel)([&](std::size_t voxel_index) {
		if (voxel_index >= label_volume.size()) {
			throw std::out_of_range("paint_maybe_present voxel index out of range");
		}
		BinaryLabelCode& current = label_volume[voxel_index];
		if (current == kBinaryLabelBackgroundCode) {
			current = label_id;
			return;
		}
		if (code_table.is_single_label_code(current)) {
			if (current != label_id) {
				current = code_table.overlap_label_code_for_two_single_labels(
				    current, label_id);
			}
			return;
		}
		current = code_table.overlap_label_code_after_maybe_adding_label(
		    current, label_id);
	});
}

struct BinarySegmentationFramePlacement {
	std::size_t frame_index{0};
	std::size_t slice_index{0};
	std::optional<std::array<double, 3>> image_position_patient{};
};

/// Adapter from dicom::seg::Segmentation BINARY frames to BinaryFrameSetBitSource.
/// The caller supplies frame placements because grid choice/resampling belongs
/// to the application/viewer.
class BinarySegmentationFrameSetBitSource final {
public:
	BinarySegmentationFrameSetBitSource(
	    const Segmentation& segmentation,
	    BinaryLabelVolumeSize target_size,
	    std::span<const BinarySegmentationFramePlacement> frame_placements);

	BinarySegmentationFrameSetBitSource(
	    const BinarySegmentationFrameSetBitSource&) = delete;
	BinarySegmentationFrameSetBitSource& operator=(
	    const BinarySegmentationFrameSetBitSource&) = delete;
	BinarySegmentationFrameSetBitSource(
	    BinarySegmentationFrameSetBitSource&&) = delete;
	BinarySegmentationFrameSetBitSource& operator=(
	    BinarySegmentationFrameSetBitSource&&) = delete;

	[[nodiscard]] const Segmentation& segmentation() const noexcept {
		return *segmentation_;
	}
	[[nodiscard]] BinaryLabelVolumeSize size() const noexcept { return size_; }
	[[nodiscard]] std::span<const std::uint16_t>
	source_dicom_segment_numbers() const noexcept {
		return source_dicom_segment_numbers_;
	}
	[[nodiscard]] std::span<const BinaryLabelFrame> frames() const noexcept {
		return frames_;
	}
	[[nodiscard]] BinaryLabelSegmentsOverlap segments_overlap() const noexcept {
		return segments_overlap_;
	}
	[[nodiscard]] BinaryFrameSetBitSource source() const;

private:
	static void visit_segmentation_frame_set_bits(
	    const void* context,
	    std::size_t frame_ordinal,
	    BinaryFrameSetBitVisitor visitor,
	    void* user_data);

	const Segmentation* segmentation_{nullptr};
	BinaryLabelVolumeSize size_{};
	BinaryLabelSegmentsOverlap segments_overlap_{
	    BinaryLabelSegmentsOverlap::undefined};
	std::vector<std::uint16_t> source_dicom_segment_numbers_{};
	std::vector<BinaryLabelFrame> frames_{};
};

void build_binary_label_volume_from_segmentation_into(
    const BinarySegmentationFrameSetBitSource& source,
    BinaryLabelVolumeBuildTarget target,
    BinaryLabelVolumeBuildOptions options = {});
[[nodiscard]] BinaryLabelVolume build_binary_label_volume_from_segmentation(
    const BinarySegmentationFrameSetBitSource& source,
    BinaryLabelVolumeBuildOptions options = {});

} // namespace dicom::seg
