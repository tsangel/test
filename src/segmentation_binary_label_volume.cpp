#include "dicom_seg_binary_label_volume.h"

#include <algorithm>
#include <array>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dicom::seg {

namespace {

constexpr std::uint32_t kInvalidBinaryLabelSetOffset =
    std::numeric_limits<std::uint32_t>::max();

[[nodiscard]] bool is_sorted_unique_binary_label_set(
    std::span<const BinaryLabelId> label_set) {
	if (label_set.empty()) {
		return false;
	}
	BinaryLabelId previous = 0;
	for (BinaryLabelId label_id : label_set) {
		if (label_id == 0 || label_id <= previous) {
			return false;
		}
		previous = label_id;
	}
	return true;
}

[[nodiscard]] BinaryLabelSetRef invalid_binary_label_set_ref() {
	return BinaryLabelSetRef{.offset = kInvalidBinaryLabelSetOffset, .count = 0};
}

void validate_binary_label_source_segment_numbers(
    std::span<const std::uint16_t> source_dicom_segment_numbers) {
	if (source_dicom_segment_numbers.empty()) {
		throw std::invalid_argument(
		    "BINARY SEG label volume requires at least one segment");
	}
	if (source_dicom_segment_numbers.size() >
	    static_cast<std::size_t>(kBinaryLabelMaxCode)) {
		throw std::invalid_argument(
		    "BINARY SEG label count exceeds uint16 range");
	}

	std::vector<std::uint16_t> sorted(
	    source_dicom_segment_numbers.begin(),
	    source_dicom_segment_numbers.end());
	std::sort(sorted.begin(), sorted.end());
	std::uint16_t previous = 0;
	for (std::uint16_t segment_number : sorted) {
		if (segment_number == 0) {
			throw std::invalid_argument(
			    "BINARY SEG DICOM SegmentNumber must be non-zero");
		}
		if (segment_number == previous) {
			throw std::invalid_argument(
			    "BINARY SEG DICOM SegmentNumber values must be unique");
		}
		previous = segment_number;
	}
}

[[nodiscard]] std::size_t checked_binary_slice_pixel_count(
    BinaryLabelVolumeSize size) {
	if (size.columns == 0 || size.rows == 0) {
		throw std::invalid_argument(
		    "BINARY SEG source rows/columns must be positive");
	}
	if (size.columns > std::numeric_limits<std::size_t>::max() / size.rows) {
		throw std::overflow_error(
		    "BINARY SEG source slice pixel count overflow");
	}
	return size.columns * size.rows;
}

void validate_binary_label_frame_metadata(
    const BinaryFrameSetBitSource& source,
    std::span<const std::size_t> frame_order) {
	const BinaryLabelVolumeSize size = source.size();
	const auto segments = source.source_dicom_segment_numbers();
	const auto frames = source.frames();
	for (std::size_t frame_ordinal : frame_order) {
		if (frame_ordinal >= frames.size()) {
			throw std::logic_error("BINARY SEG frame order is out of range");
		}
		const BinaryLabelFrame& frame = frames[frame_ordinal];
		if (frame.label_id == 0 ||
		    frame.label_id > static_cast<BinaryLabelId>(segments.size())) {
			throw std::invalid_argument(
			    "BINARY SEG frame label_id is out of range");
		}
		if (frame.slice_index >= size.slices) {
			throw std::invalid_argument(
			    "BINARY SEG frame slice_index is out of range");
		}
	}

	std::vector<std::pair<BinaryLabelId, std::size_t>> label_slice_pairs;
	label_slice_pairs.reserve(frame_order.size());
	for (std::size_t frame_ordinal : frame_order) {
		const BinaryLabelFrame& frame = frames[frame_ordinal];
		label_slice_pairs.emplace_back(frame.label_id, frame.slice_index);
	}
	std::sort(label_slice_pairs.begin(), label_slice_pairs.end());
	if (std::adjacent_find(
	        label_slice_pairs.begin(), label_slice_pairs.end()) !=
	    label_slice_pairs.end()) {
		throw std::invalid_argument(
		    "BINARY SEG duplicate (label_id, slice_index) frame");
	}
}

void validate_binary_label_source_segment_table(
    std::span<const std::uint16_t> source_segment_by_label_id,
    std::size_t source_label_count) {
	if (source_segment_by_label_id.empty()) {
		return;
	}
	if (source_segment_by_label_id.size() <= source_label_count) {
		throw std::invalid_argument(
		    "source_dicom_segment_by_label_id must cover all source label_ids");
	}
	if (source_segment_by_label_id.front() != 0) {
		throw std::invalid_argument(
		    "source_dicom_segment_by_label_id index 0 must be background");
	}
}

void reset_binary_label_stats_table(
    std::vector<BinaryLabelStats>& stats_by_label_id,
    BinaryLabelCode single_label_code_end) {
	stats_by_label_id.assign(
	    static_cast<std::size_t>(single_label_code_end) + 1u,
	    BinaryLabelStats{});
}

[[nodiscard]] std::uint64_t checked_size_to_u64(
    std::size_t value, std::string_view label) {
	if (value > static_cast<std::size_t>(
	                std::numeric_limits<std::uint64_t>::max())) {
		throw std::overflow_error(std::string(label) + " exceeds uint64 range");
	}
	return static_cast<std::uint64_t>(value);
}

[[nodiscard]] std::uint64_t checked_add_u64(
    std::uint64_t lhs, std::uint64_t rhs, std::string_view label) {
	if (lhs > std::numeric_limits<std::uint64_t>::max() - rhs) {
		throw std::overflow_error(std::string(label) + " overflow");
	}
	return lhs + rhs;
}

class BinaryLabelStatsAccumulator final {
public:
	BinaryLabelStatsAccumulator(
	    std::vector<BinaryLabelStats>* stats_by_label_id,
	    BinaryLabelVolumeSize size,
	    std::size_t slice_pixels)
	    : stats_by_label_id_(stats_by_label_id),
	      size_(size),
	      slice_pixels_(slice_pixels) {}

	void add(std::size_t voxel_index, BinaryLabelId label_id) {
		if (stats_by_label_id_ == nullptr) {
			return;
		}
		if (label_id >= stats_by_label_id_->size()) {
			throw std::out_of_range("BINARY SEG stats label_id is out of range");
		}
		const auto xyz = xyz_for_voxel(voxel_index);
		BinaryLabelStats& stats = (*stats_by_label_id_)[label_id];
		const bool first_voxel = !stats.has_voxels();
		if (stats.voxel_count == std::numeric_limits<std::uint64_t>::max()) {
			throw std::overflow_error("BINARY SEG stats voxel_count overflow");
		}
		stats.voxel_count += 1u;
		for (std::size_t axis = 0; axis < xyz.size(); ++axis) {
			stats.index_sum_xyz[axis] = checked_add_u64(
			    stats.index_sum_xyz[axis],
			    checked_size_to_u64(xyz[axis], "BINARY SEG stats index"),
			    "BINARY SEG stats index sum");
			if (first_voxel) {
				stats.min_index_xyz[axis] = xyz[axis];
				stats.max_index_xyz[axis] = xyz[axis];
			} else {
				stats.min_index_xyz[axis] =
				    std::min(stats.min_index_xyz[axis], xyz[axis]);
				stats.max_index_xyz[axis] =
				    std::max(stats.max_index_xyz[axis], xyz[axis]);
			}
		}
	}

private:
	[[nodiscard]] std::array<std::size_t, 3> xyz_for_voxel(
	    std::size_t voxel_index) {
		if (voxel_index == last_voxel_index_) {
			return last_xyz_;
		}
		const std::size_t z = voxel_index / slice_pixels_;
		const std::size_t local = voxel_index - z * slice_pixels_;
		const std::size_t y = local / size_.columns;
		const std::size_t x = local - y * size_.columns;
		last_voxel_index_ = voxel_index;
		last_xyz_ = {x, y, z};
		return last_xyz_;
	}

	std::vector<BinaryLabelStats>* stats_by_label_id_{nullptr};
	BinaryLabelVolumeSize size_{};
	std::size_t slice_pixels_{0};
	std::size_t last_voxel_index_{std::numeric_limits<std::size_t>::max()};
	std::array<std::size_t, 3> last_xyz_{0, 0, 0};
};

template <class VisitVoxel>
struct BinarySetBitCallbackContext {
	VisitVoxel* visit_voxel{nullptr};
	std::size_t slice_pixels{0};
	std::size_t voxel_base{0};
	std::uint64_t* frame_set_bit_count{nullptr};
	BinaryLabelVolumeBuildTelemetry* telemetry{nullptr};
};

template <class VisitVoxel>
void visit_binary_source_frame_set_bit(
    std::size_t pixel_index, void* user_data) {
	auto& context =
	    *static_cast<BinarySetBitCallbackContext<VisitVoxel>*>(user_data);
	if (pixel_index >= context.slice_pixels) {
		throw std::out_of_range("BINARY SEG set bit pixel_index out of range");
	}
	++*context.frame_set_bit_count;
	if (context.telemetry) {
		++context.telemetry->visited_set_bit_count;
	}
	(*context.visit_voxel)(context.voxel_base + pixel_index);
}

[[nodiscard]] BinaryLabelSegmentsOverlap binary_label_overlap_from_seg_overlap(
    SegmentsOverlap value) noexcept {
	switch (value) {
	case SegmentsOverlap::no:
		return BinaryLabelSegmentsOverlap::no;
	case SegmentsOverlap::yes:
		return BinaryLabelSegmentsOverlap::yes;
	case SegmentsOverlap::undefined:
	case SegmentsOverlap::unknown:
		return BinaryLabelSegmentsOverlap::undefined;
	}
	return BinaryLabelSegmentsOverlap::undefined;
}

} // namespace

BinaryLabelSetView BinaryLabelSetView::empty() {
	return BinaryLabelSetView{};
}

BinaryLabelSetView BinaryLabelSetView::single(BinaryLabelId label_id) {
	BinaryLabelSetView view;
	view.kind_ = BinaryLabelSetViewKind::single;
	view.single_label_id_ = label_id;
	return view;
}

BinaryLabelSetView BinaryLabelSetView::span(
    std::span<const BinaryLabelId> label_set) {
	BinaryLabelSetView view;
	view.kind_ = label_set.empty() ? BinaryLabelSetViewKind::empty
	                               : BinaryLabelSetViewKind::span;
	view.label_set_ = label_set;
	return view;
}

std::size_t BinaryLabelSetView::size() const noexcept {
	switch (kind_) {
	case BinaryLabelSetViewKind::empty:
		return 0;
	case BinaryLabelSetViewKind::single:
		return 1;
	case BinaryLabelSetViewKind::span:
		return label_set_.size();
	}
	return 0;
}

BinaryLabelId BinaryLabelSetView::operator[](std::size_t index) const {
	switch (kind_) {
	case BinaryLabelSetViewKind::empty:
		break;
	case BinaryLabelSetViewKind::single:
		if (index == 0) {
			return single_label_id_;
		}
		break;
	case BinaryLabelSetViewKind::span:
		return label_set_[index];
	}
	throw std::out_of_range("BinaryLabelSetView index out of range");
}

bool BinaryLabelSetView::contains(BinaryLabelId label_id) const {
	switch (kind_) {
	case BinaryLabelSetViewKind::empty:
		return false;
	case BinaryLabelSetViewKind::single:
		return single_label_id_ == label_id;
	case BinaryLabelSetViewKind::span:
		return std::binary_search(label_set_.begin(), label_set_.end(), label_id);
	}
	return false;
}

BinaryFrameSetBitSource::BinaryFrameSetBitSource(
    BinaryLabelVolumeSize size,
    std::span<const std::uint16_t> source_dicom_segment_numbers,
    std::span<const BinaryLabelFrame> frames,
    const void* context,
    BinaryFrameSetBitVisitFunction visit_set_bits)
    : size_(size),
      source_dicom_segment_numbers_(source_dicom_segment_numbers),
      frames_(frames),
      context_(context),
      visit_set_bits_(visit_set_bits) {
	if (!visit_set_bits_) {
		throw std::invalid_argument(
		    "BinaryFrameSetBitSource requires a set-bit visitor function");
	}
}

void BinaryFrameSetBitSource::for_each_frame_set_bit(
    std::size_t frame_ordinal,
    BinaryFrameSetBitVisitor visitor,
    void* user_data) const {
	if (!visit_set_bits_) {
		throw std::logic_error("BinaryFrameSetBitSource is not initialized");
	}
	if (!visitor) {
		throw std::invalid_argument(
		    "BinaryFrameSetBitSource visitor must not be null");
	}
	if (frame_ordinal >= frames_.size()) {
		throw std::out_of_range(
		    "BinaryFrameSetBitSource frame ordinal out of range");
	}
	visit_set_bits_(context_, frame_ordinal, visitor, user_data);
}

BinaryLabelCodeTable BinaryLabelCodeTable::create(
    BinaryLabelCode single_label_code_end,
    BinaryLabelCodeTableReserve reserve) {
	if (single_label_code_end == 0) {
		throw std::invalid_argument("single_label_code_end must be non-zero");
	}

	BinaryLabelCodeTable table;
	table.single_label_code_end_ = single_label_code_end;
	table.label_set_ref_by_label_code_.assign(
	    static_cast<std::size_t>(kBinaryLabelMaxCode) + 1,
	    invalid_binary_label_set_ref());
	table.label_codes_by_label_id_.resize(
	    static_cast<std::size_t>(single_label_code_end) + 1);
	for (BinaryLabelId label_id = 1; label_id <= single_label_code_end;
	     ++label_id) {
		table.label_codes_by_label_id_[label_id].push_back(label_id);
		if (label_id == kBinaryLabelMaxCode) {
			break;
		}
	}

	table.overlap_entries_.reserve(
	    reserve.estimated_unique_overlap_label_set_count);
	table.label_set_storage_.reserve(reserve.estimated_overlap_label_id_total);
	table.label_set_code_cache_4_.reserve(
	    reserve.estimated_2_to_4_label_set_count);
	table.label_set_code_cache_32_.reserve(
	    reserve.estimated_5_to_32_label_set_count);
	table.label_set_code_cache_128_.reserve(
	    reserve.estimated_33_to_128_label_set_count);
	table.label_set_scratch_.reserve(kBinaryLabelMaxOverlapSetSize);
	return table;
}

bool BinaryLabelCodeTable::is_valid_label_id(
    BinaryLabelId label_id) const noexcept {
	return label_id != 0 && label_id <= single_label_code_end_;
}

bool BinaryLabelCodeTable::is_single_label_code(
    BinaryLabelCode label_code) const noexcept {
	return label_code != 0 && label_code <= single_label_code_end_;
}

bool BinaryLabelCodeTable::is_overlap_label_code(
    BinaryLabelCode label_code) const {
	if (label_code <= single_label_code_end_ ||
	    label_code >= label_set_ref_by_label_code_.size()) {
		return false;
	}
	return !label_set_ref_by_label_code_[label_code].empty();
}

BinaryLabelSetView BinaryLabelCodeTable::label_set_by_label_code(
    BinaryLabelCode label_code) const {
	if (label_code == kBinaryLabelBackgroundCode) {
		return BinaryLabelSetView::empty();
	}
	if (is_single_label_code(label_code)) {
		return BinaryLabelSetView::single(label_code);
	}
	if (label_code >= label_set_ref_by_label_code_.size()) {
		return BinaryLabelSetView::empty();
	}

	const BinaryLabelSetRef ref = label_set_ref_by_label_code_[label_code];
	if (ref.empty()) {
		return BinaryLabelSetView::empty();
	}
	const std::size_t offset = ref.offset;
	const std::size_t count = ref.count;
	if (offset > label_set_storage_.size() ||
	    count > label_set_storage_.size() - offset) {
		throw std::logic_error("BINARY SEG label_set ref is out of range");
	}
	return BinaryLabelSetView::span(std::span<const BinaryLabelId>(
	    label_set_storage_.data() + offset, count));
}

std::span<const BinaryLabelCode>
BinaryLabelCodeTable::label_codes_for_label_id(
    BinaryLabelId label_id) const noexcept {
	if (!is_valid_label_id(label_id)) {
		return {};
	}
	return label_codes_by_label_id_[label_id];
}

BinaryLabelCode
BinaryLabelCodeTable::overlap_label_code_for_two_single_labels(
    BinaryLabelId a,
    BinaryLabelId b) {
	if (!is_valid_label_id(a) || !is_valid_label_id(b)) {
		throw std::invalid_argument("single label_id is out of range");
	}
	if (a == b) {
		throw std::invalid_argument(
		    "paint_absent encountered an already-present single label");
	}
	const BinaryLabelId first = std::min(a, b);
	const BinaryLabelId second = std::max(a, b);
	const std::array<BinaryLabelId, 2> label_set = {first, second};
	return overlap_label_code_for_label_set(label_set);
}

BinaryLabelCode BinaryLabelCodeTable::overlap_label_code_for_label_set(
    std::span<const BinaryLabelId> label_set) {
	if (label_set.size() < 2) {
		throw std::invalid_argument("overlap label_set must contain 2+ labels");
	}
	if (label_set.size() > kBinaryLabelMaxOverlapSetSize) {
		++telemetry_.rejected_over_128_label_set_count;
		throw std::invalid_argument(
		    "overlap label_set cardinality exceeds 128");
	}
	if (!is_sorted_unique_binary_label_set(label_set)) {
		throw std::invalid_argument(
		    "overlap label_set must be sorted unique non-zero label_ids");
	}
	for (BinaryLabelId label_id : label_set) {
		if (!is_valid_label_id(label_id)) {
			throw std::invalid_argument("overlap label_id is out of range");
		}
	}

	if (label_set.size() <= 4) {
		const auto key = make_binary_inline_label_set_key<4>(label_set);
		const auto found = label_set_code_cache_4_.find(key);
		if (found != label_set_code_cache_4_.end()) {
			++telemetry_.reused_overlap_label_set_count;
			return found->second;
		}
		return intern_overlap_label_set(label_set);
	}
	if (label_set.size() <= 32) {
		const auto key = make_binary_inline_label_set_key<32>(label_set);
		const auto found = label_set_code_cache_32_.find(key);
		if (found != label_set_code_cache_32_.end()) {
			++telemetry_.reused_overlap_label_set_count;
			return found->second;
		}
		return intern_overlap_label_set(label_set);
	}

	const auto key = make_binary_inline_label_set_key<128>(label_set);
	const auto found = label_set_code_cache_128_.find(key);
	if (found != label_set_code_cache_128_.end()) {
		++telemetry_.reused_overlap_label_set_count;
		return found->second;
	}
	return intern_overlap_label_set(label_set);
}

BinaryLabelCode
BinaryLabelCodeTable::overlap_label_code_after_adding_absent_label(
    BinaryLabelCode overlap_label_code,
    BinaryLabelId label_id) {
	const BinaryLabelSetView view = label_set_by_label_code(overlap_label_code);
	if (view.size() < 2) {
		throw std::invalid_argument("label_code is not an overlap label_code");
	}
	if (!is_valid_label_id(label_id)) {
		throw std::invalid_argument("label_id is out of range");
	}
	if (view.contains(label_id)) {
		throw std::invalid_argument(
		    "paint_absent encountered an already-present overlap label");
	}
	if (view.size() >= kBinaryLabelMaxOverlapSetSize) {
		++telemetry_.rejected_over_128_label_set_count;
		throw std::invalid_argument(
		    "overlap label_set cardinality exceeds 128");
	}

	label_set_scratch_.clear();
	label_set_scratch_.reserve(view.size() + 1);
	bool inserted = false;
	for (std::size_t index = 0; index < view.size(); ++index) {
		const BinaryLabelId existing = view[index];
		if (!inserted && label_id < existing) {
			label_set_scratch_.push_back(label_id);
			inserted = true;
		}
		label_set_scratch_.push_back(existing);
	}
	if (!inserted) {
		label_set_scratch_.push_back(label_id);
	}
	return overlap_label_code_for_label_set(label_set_scratch_);
}

BinaryLabelCode
BinaryLabelCodeTable::overlap_label_code_after_maybe_adding_label(
    BinaryLabelCode overlap_label_code,
    BinaryLabelId label_id) {
	const BinaryLabelSetView view = label_set_by_label_code(overlap_label_code);
	if (view.size() < 2) {
		throw std::invalid_argument("label_code is not an overlap label_code");
	}
	if (view.contains(label_id)) {
		return overlap_label_code;
	}
	return overlap_label_code_after_adding_absent_label(
	    overlap_label_code, label_id);
}

BinaryLabelCode BinaryLabelCodeTable::intern_overlap_label_set(
    std::span<const BinaryLabelId> label_set) {
	if (label_set.size() > kBinaryLabelMaxOverlapSetSize) {
		++telemetry_.rejected_over_128_label_set_count;
		throw std::invalid_argument(
		    "overlap label_set cardinality exceeds 128");
	}
	const std::uint64_t next_code64 =
	    static_cast<std::uint64_t>(single_label_code_end_) + 1u +
	    static_cast<std::uint64_t>(overlap_entries_.size());
	if (next_code64 > kBinaryLabelMaxCode) {
		++telemetry_.label_code_range_exhaustion_count;
		throw std::overflow_error("BINARY SEG overlap label_code range exhausted");
	}

	const BinaryLabelCode label_code =
	    static_cast<BinaryLabelCode>(next_code64);
	const BinaryLabelSetRef ref = append_label_set(label_set);
	label_set_ref_by_label_code_[label_code] = ref;
	overlap_entries_.push_back(BinaryOverlapLabelSetEntry{
	    .label_code = label_code,
	    .label_set = ref,
	});

	if (label_set.size() <= 4) {
		label_set_code_cache_4_.emplace(
		    make_binary_inline_label_set_key<4>(label_set), label_code);
		++telemetry_.overlap_label_set_count_2_to_4;
	} else if (label_set.size() <= 32) {
		label_set_code_cache_32_.emplace(
		    make_binary_inline_label_set_key<32>(label_set), label_code);
		++telemetry_.overlap_label_set_count_5_to_32;
	} else {
		label_set_code_cache_128_.emplace(
		    make_binary_inline_label_set_key<128>(label_set), label_code);
		++telemetry_.overlap_label_set_count_33_to_128;
	}
	++telemetry_.unique_overlap_label_set_count;

	for (BinaryLabelId label_id : label_set) {
		label_codes_by_label_id_[label_id].push_back(label_code);
	}
	return label_code;
}

BinaryLabelSetRef BinaryLabelCodeTable::append_label_set(
    std::span<const BinaryLabelId> label_set) {
	if (label_set_storage_.size() >
	    static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
		throw std::overflow_error("BINARY SEG label_set storage is too large");
	}
	const std::size_t next_size = label_set_storage_.size() + label_set.size();
	if (next_size >
	    static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
		throw std::overflow_error("BINARY SEG label_set storage is too large");
	}
	const auto offset =
	    static_cast<std::uint32_t>(label_set_storage_.size());
	label_set_storage_.insert(
	    label_set_storage_.end(), label_set.begin(), label_set.end());
	return BinaryLabelSetRef{
	    .offset = offset,
	    .count = static_cast<std::uint16_t>(label_set.size()),
	};
}

BinaryLabelCodeSet::BinaryLabelCodeSet()
    : marks_(static_cast<std::size_t>(kBinaryLabelMaxCode) + 1, 0) {}

void BinaryLabelCodeSet::clear() {
	if (generation_ == std::numeric_limits<std::uint32_t>::max()) {
		std::fill(marks_.begin(), marks_.end(), 0);
		generation_ = 1;
		return;
	}
	++generation_;
}

void BinaryLabelCodeSet::include(BinaryLabelCode label_code) {
	marks_[label_code] = generation_;
}

void BinaryLabelCodeSet::include_all(
    std::span<const BinaryLabelCode> label_codes) {
	for (BinaryLabelCode label_code : label_codes) {
		include(label_code);
	}
}

void BinaryLabelCodeSet::reset_to_label_id(
    const BinaryLabelCodeTable& code_table,
    BinaryLabelId label_id) {
	if (!code_table.is_valid_label_id(label_id)) {
		throw std::invalid_argument("BinaryLabelCodeSet label_id is out of range");
	}
	clear();
	include_all(code_table.label_codes_for_label_id(label_id));
}

bool BinaryLabelCodeSet::contains(BinaryLabelCode label_code) const noexcept {
	return marks_[label_code] == generation_;
}

std::size_t binary_label_volume_voxel_count(BinaryLabelVolumeSize size) {
	if (size.columns == 0 || size.rows == 0 || size.slices == 0) {
		throw std::invalid_argument(
		    "BINARY SEG label volume size must be positive");
	}
	if (size.columns > std::numeric_limits<std::size_t>::max() / size.rows) {
		throw std::overflow_error("BINARY SEG label volume row size overflow");
	}
	const std::size_t slice = size.columns * size.rows;
	if (slice > std::numeric_limits<std::size_t>::max() / size.slices) {
		throw std::overflow_error(
		    "BINARY SEG label volume voxel count overflow");
	}
	return slice * size.slices;
}

BinaryLabelCodeTable create_binary_label_code_table(
    std::span<const std::uint16_t> source_dicom_segment_numbers,
    const BinaryLabelVolumeOptions& options) {
	validate_binary_label_source_segment_numbers(source_dicom_segment_numbers);
	const auto label_count =
	    static_cast<BinaryLabelCode>(source_dicom_segment_numbers.size());
	BinaryLabelCode single_label_code_end = options.single_label_code_end;
	if (single_label_code_end == 0) {
		single_label_code_end = label_count;
	}
	if (single_label_code_end < label_count) {
		throw std::invalid_argument(
		    "single_label_code_end must cover all source labels");
	}
	return BinaryLabelCodeTable::create(single_label_code_end, options.reserve);
}

std::vector<std::uint16_t> make_binary_label_source_segment_table(
    std::span<const std::uint16_t> source_dicom_segment_numbers,
    BinaryLabelCode single_label_code_end) {
	validate_binary_label_source_segment_numbers(source_dicom_segment_numbers);
	if (single_label_code_end < source_dicom_segment_numbers.size()) {
		throw std::invalid_argument(
		    "single_label_code_end must cover all source labels");
	}
	std::vector<std::uint16_t> out(
	    static_cast<std::size_t>(single_label_code_end) + 1, 0);
	for (std::size_t index = 0; index < source_dicom_segment_numbers.size();
	     ++index) {
		const auto label_id = static_cast<BinaryLabelId>(index + 1);
		out[label_id] = source_dicom_segment_numbers[index];
	}
	return out;
}

void clear_binary_label_volume(std::span<BinaryLabelCode> label_volume) {
	std::fill(label_volume.begin(), label_volume.end(),
	    kBinaryLabelBackgroundCode);
}

BinaryLabelVolume create_empty_binary_label_volume(
    BinaryLabelVolumeSize size,
    std::span<const std::uint16_t> source_dicom_segment_numbers,
    BinaryLabelVolumeOptions options) {
	const std::size_t voxels = binary_label_volume_voxel_count(size);
	BinaryLabelVolume volume;
	volume.size = size;
	volume.label_volume.assign(voxels, kBinaryLabelBackgroundCode);
	volume.code_table =
	    create_binary_label_code_table(source_dicom_segment_numbers, options);
	volume.source_dicom_segment_by_label_id =
	    make_binary_label_source_segment_table(
	        source_dicom_segment_numbers,
	        volume.code_table.single_label_code_end());
	reset_binary_label_stats_table(
	    volume.label_stats_by_label_id,
	    volume.code_table.single_label_code_end());
	return volume;
}

void build_binary_label_volume_into(
    const BinaryFrameSetBitSource& source,
    BinaryLabelVolumeBuildTarget target,
    BinaryLabelVolumeBuildOptions options) {
	if (target.code_table == nullptr) {
		throw std::invalid_argument("BINARY SEG build target requires code_table");
	}
	if (target.size.columns != source.size().columns ||
	    target.size.rows != source.size().rows ||
	    target.size.slices != source.size().slices) {
		throw std::invalid_argument(
		    "BINARY SEG build target size must match source size");
	}
	const std::size_t voxels = binary_label_volume_voxel_count(target.size);
	if (target.label_volume.size() != voxels) {
		throw std::invalid_argument(
		    "BINARY SEG build target label_volume size mismatch");
	}
	if (target.code_table->single_label_code_end() <
	    source.source_dicom_segment_numbers().size()) {
		throw std::invalid_argument(
		    "BINARY SEG code_table does not cover all source labels");
	}
	std::vector<std::uint16_t> local_source_segment_by_label_id;
	std::span<const std::uint16_t> source_segment_by_label_id =
	    target.source_dicom_segment_by_label_id;
	if (source_segment_by_label_id.empty()) {
		local_source_segment_by_label_id =
		    make_binary_label_source_segment_table(
		        source.source_dicom_segment_numbers(),
		        target.code_table->single_label_code_end());
		source_segment_by_label_id = local_source_segment_by_label_id;
	}
	validate_binary_label_source_segment_table(source_segment_by_label_id,
	    source.source_dicom_segment_numbers().size());

	clear_binary_label_volume(target.label_volume);
	if (target.label_stats_by_label_id) {
		reset_binary_label_stats_table(
		    *target.label_stats_by_label_id,
		    target.code_table->single_label_code_end());
	}
	if (target.source_frame_map) {
		target.source_frame_map->clear();
		target.source_frame_map->reserve(source.frames().size());
	}

	const BinaryLabelVolumeSize size = target.size;
	const std::size_t slice_pixels = checked_binary_slice_pixel_count(size);
	if (target.label_volume.size() / size.slices != slice_pixels) {
		throw std::logic_error("BINARY SEG label_volume size mismatch");
	}
	BinaryLabelStatsAccumulator stats_accumulator(
	    target.label_stats_by_label_id, size, slice_pixels);

	const auto frames = source.frames();
	std::vector<std::size_t> frame_order(frames.size());
	std::iota(frame_order.begin(), frame_order.end(), std::size_t{0});
	if (options.segments_overlap != BinaryLabelSegmentsOverlap::no) {
		std::stable_sort(
		    frame_order.begin(), frame_order.end(),
		    [&](std::size_t lhs, std::size_t rhs) {
			    const BinaryLabelFrame& left = frames[lhs];
			    const BinaryLabelFrame& right = frames[rhs];
			    if (left.label_id != right.label_id) {
				    return left.label_id < right.label_id;
			    }
			    if (left.slice_index != right.slice_index) {
				    return left.slice_index < right.slice_index;
			    }
			    return left.source_frame_index < right.source_frame_index;
		    });
	}
	validate_binary_label_frame_metadata(source, frame_order);

	if (options.telemetry) {
		*options.telemetry = {};
		options.telemetry->source_frame_count =
		    static_cast<std::uint64_t>(frames.size());
	}

	for (std::size_t frame_ordinal : frame_order) {
		const BinaryLabelFrame& frame = frames[frame_ordinal];
		const std::size_t voxel_base = frame.slice_index * slice_pixels;

		std::uint64_t frame_set_bit_count = 0;
		auto visit_set_voxels = [&](auto&& visit_voxel) {
			using VisitVoxel = std::remove_reference_t<decltype(visit_voxel)>;
			BinarySetBitCallbackContext<VisitVoxel> callback_context{
			    .visit_voxel = &visit_voxel,
			    .slice_pixels = slice_pixels,
			    .voxel_base = voxel_base,
			    .frame_set_bit_count = &frame_set_bit_count,
			    .telemetry = options.telemetry,
			};
			source.for_each_frame_set_bit(frame_ordinal,
			    &visit_binary_source_frame_set_bit<VisitVoxel>,
			    &callback_context);
		};
		auto on_label_added = [&](std::size_t voxel_index,
		                          BinaryLabelId label_id) {
			stats_accumulator.add(voxel_index, label_id);
		};

		if (options.segments_overlap == BinaryLabelSegmentsOverlap::no) {
			paint_no_overlap_label_into_volume(target.label_volume,
			    frame.label_id, *target.code_table, visit_set_voxels,
			    on_label_added);
		} else {
			paint_absent_label_into_volume(target.label_volume,
			    frame.label_id, *target.code_table, visit_set_voxels,
			    on_label_added);
		}

		if (options.telemetry && frame_set_bit_count > 0) {
			++options.telemetry->non_empty_frame_count;
		}
		if (target.source_frame_map) {
			const std::size_t label_index = frame.label_id;
			const std::uint16_t dicom_segment_number =
			    source_segment_by_label_id[label_index];
			target.source_frame_map->push_back(BinaryLabelSourceFrameMapEntry{
			    .source_frame_index = frame.source_frame_index,
			    .dicom_segment_number = dicom_segment_number,
			    .label_id = frame.label_id,
			    .slice_index = frame.slice_index,
			    .non_empty_pixel_count = frame_set_bit_count,
			    .image_position_patient = frame.image_position_patient,
			});
		}
	}
	if (target.source_frame_map) {
		std::sort(target.source_frame_map->begin(),
		    target.source_frame_map->end(),
		    [](const BinaryLabelSourceFrameMapEntry& lhs,
		        const BinaryLabelSourceFrameMapEntry& rhs) {
			    return lhs.source_frame_index < rhs.source_frame_index;
		    });
	}
}

BinaryLabelVolume build_binary_label_volume(
    const BinaryFrameSetBitSource& source,
    BinaryLabelVolumeBuildOptions options) {
	BinaryLabelVolume volume = create_empty_binary_label_volume(
	    source.size(), source.source_dicom_segment_numbers(),
	    options.volume_options);
	build_binary_label_volume_into(source,
	    BinaryLabelVolumeBuildTarget{
	        .size = volume.size,
	        .label_volume = volume.label_volume,
	        .code_table = &volume.code_table,
	        .source_dicom_segment_by_label_id =
	            volume.source_dicom_segment_by_label_id,
	        .source_frame_map = &volume.source_frame_map,
	        .label_stats_by_label_id = &volume.label_stats_by_label_id,
	    },
	    options);
	return volume;
}

void restore_binary_label_mask(std::span<const BinaryLabelCode> label_volume,
    const BinaryLabelCodeTable& code_table,
    BinaryLabelId label_id,
    std::span<std::uint8_t> mask_out) {
	BinaryLabelCodeSet label_code_set;
	restore_binary_label_mask(
	    label_volume, code_table, label_id, label_code_set, mask_out);
}

void restore_binary_label_mask(std::span<const BinaryLabelCode> label_volume,
    const BinaryLabelCodeTable& code_table,
    BinaryLabelId label_id,
    BinaryLabelCodeSet& label_code_set,
    std::span<std::uint8_t> mask_out) {
	if (mask_out.size() != label_volume.size()) {
		throw std::invalid_argument(
		    "restore_binary_label_mask output size mismatch");
	}
	label_code_set.reset_to_label_id(code_table, label_id);
	for (std::size_t index = 0; index < label_volume.size(); ++index) {
		mask_out[index] =
		    label_code_set.contains(label_volume[index]) ? 1u : 0u;
	}
}

BinarySegmentationFrameSetBitSource::BinarySegmentationFrameSetBitSource(
    const Segmentation& segmentation,
    BinaryLabelVolumeSize target_size,
    std::span<const BinarySegmentationFramePlacement> frame_placements)
    : segmentation_(&segmentation), size_(target_size) {
	if (segmentation.segmentation_type() != SegmentationType::binary) {
		throw std::invalid_argument(
		    "BinarySegmentationFrameSetBitSource requires BINARY SEG");
	}
	segments_overlap_ =
	    binary_label_overlap_from_seg_overlap(segmentation.segments_overlap());

	if (target_size.columns != segmentation.columns() ||
	    target_size.rows != segmentation.rows() || target_size.slices == 0) {
		throw std::invalid_argument(
		    "BINARY SEG target size must match SEG rows/columns and have slices");
	}
	if (frame_placements.size() != segmentation.frame_count()) {
		throw std::invalid_argument(
		    "BINARY SEG frame placement count must match SEG frame count");
	}

	source_dicom_segment_numbers_.reserve(segmentation.segment_count());
	std::unordered_map<std::uint16_t, BinaryLabelId>
	    label_id_by_segment_number;
	label_id_by_segment_number.reserve(segmentation.segment_count());
	for (const SegmentView segment : segmentation.segments()) {
		const std::uint16_t segment_number = segment.number();
		if (segment_number == 0) {
			throw std::invalid_argument(
			    "BINARY SEG SegmentNumber must be non-zero");
		}
		if (source_dicom_segment_numbers_.size() >=
		    static_cast<std::size_t>(kBinaryLabelMaxCode)) {
			throw std::invalid_argument(
			    "BINARY SEG segment count exceeds uint16 label_id range");
		}
		const auto label_id = static_cast<BinaryLabelId>(
		    source_dicom_segment_numbers_.size() + 1);
		const auto inserted =
		    label_id_by_segment_number.emplace(segment_number, label_id);
		if (!inserted.second) {
			throw std::invalid_argument(
			    "BINARY SEG SegmentNumber values must be unique");
		}
		source_dicom_segment_numbers_.push_back(segment_number);
	}

	std::vector<bool> seen_frame(segmentation.frame_count(), false);
	frames_.reserve(frame_placements.size());
	for (const BinarySegmentationFramePlacement& placement : frame_placements) {
		if (placement.frame_index >= segmentation.frame_count()) {
			throw std::out_of_range(
			    "BINARY SEG frame placement index is out of range");
		}
		if (seen_frame[placement.frame_index]) {
			throw std::invalid_argument(
			    "BINARY SEG frame placements must be unique");
		}
		seen_frame[placement.frame_index] = true;
		if (placement.slice_index >= target_size.slices) {
			throw std::invalid_argument(
			    "BINARY SEG frame placement slice_index is out of range");
		}

		const SegmentFrameView frame =
		    segmentation.frames()[placement.frame_index];
		const std::uint16_t segment_number = frame.referenced_segment_number();
		const auto label_found =
		    label_id_by_segment_number.find(segment_number);
		if (label_found == label_id_by_segment_number.end()) {
			throw std::invalid_argument(
			    "BINARY SEG frame references a segment missing from SegmentSequence");
		}

		std::optional<std::array<double, 3>> image_position =
		    placement.image_position_patient;
		if (!image_position) {
			image_position = frame.image_position_patient();
		}
		frames_.push_back(BinaryLabelFrame{
		    .source_frame_index = placement.frame_index,
		    .label_id = label_found->second,
		    .slice_index = placement.slice_index,
		    .image_position_patient = image_position,
		});
	}
}

BinaryFrameSetBitSource BinarySegmentationFrameSetBitSource::source() const {
	return BinaryFrameSetBitSource(
	    size_,
	    std::span<const std::uint16_t>(source_dicom_segment_numbers_.data(),
	        source_dicom_segment_numbers_.size()),
	    std::span<const BinaryLabelFrame>(frames_.data(), frames_.size()),
	    this,
	    &BinarySegmentationFrameSetBitSource::visit_segmentation_frame_set_bits);
}

void BinarySegmentationFrameSetBitSource::visit_segmentation_frame_set_bits(
    const void* context,
    std::size_t frame_ordinal,
    BinaryFrameSetBitVisitor visitor,
    void* user_data) {
	const auto& source =
	    *static_cast<const BinarySegmentationFrameSetBitSource*>(context);
	if (frame_ordinal >= source.frames_.size()) {
		throw std::out_of_range(
		    "BINARY SEG DicomSDL frame ordinal is out of range");
	}
	const std::size_t frame_index =
	    source.frames_[frame_ordinal].source_frame_index;
	source.segmentation().for_each_binary_frame_set_bit(
	    frame_index, [&](std::size_t pixel_index) {
		    visitor(pixel_index, user_data);
	    });
}

void build_binary_label_volume_from_segmentation_into(
    const BinarySegmentationFrameSetBitSource& source,
    BinaryLabelVolumeBuildTarget target,
    BinaryLabelVolumeBuildOptions options) {
	if (options.segments_overlap == BinaryLabelSegmentsOverlap::undefined) {
		options.segments_overlap = source.segments_overlap();
	}
	build_binary_label_volume_into(source.source(), target, options);
}

BinaryLabelVolume build_binary_label_volume_from_segmentation(
    const BinarySegmentationFrameSetBitSource& source,
    BinaryLabelVolumeBuildOptions options) {
	if (options.segments_overlap == BinaryLabelSegmentsOverlap::undefined) {
		options.segments_overlap = source.segments_overlap();
	}
	return build_binary_label_volume(source.source(), options);
}

} // namespace dicom::seg
