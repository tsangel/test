#pragma once

#include "dicom.h"

#include <array>
#include <bit>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dicom::seg {

/// DICOM Segmentation Type (0062,0001).
/// The adapter accepts BINARY and FRACTIONAL Segmentation Storage instances and
/// LABELMAP Label Map Segmentation Storage instances.
enum class SegmentationType : std::uint8_t {
	unknown,
	binary,
	fractional,
	labelmap,
};

/// DICOM Segmentation Fractional Type (0062,0010).
/// Meaningful only when SegmentationType is FRACTIONAL.
enum class SegmentationFractionalType : std::uint8_t {
	none,
	probability,
	occupancy,
	unknown,
};

/// DICOM Segments Overlap (0062,0013).
/// Missing values are exposed as undefined, matching the DICOM attribute's
/// "overlap not known" semantics for callers choosing a conservative path.
enum class SegmentsOverlap : std::uint8_t {
	unknown,
	no,
	yes,
	undefined,
};

/// DICOM Segment Algorithm Type (0062,0008).
enum class SegmentAlgorithmType : std::uint8_t {
	unknown,
	automatic_,
	semiautomatic,
	manual,
};

/// Borrowed view of one DICOM code-sequence item.
/// String values point into the owning DicomFile/DataSet.
struct CodeView {
	std::string_view value{};
	std::string_view scheme_designator{};
	std::string_view scheme_version{};
	std::string_view meaning{};
};

/// Owning counterpart to CodeView for APIs that need detached values.
struct Code {
	std::string value{};
	std::string scheme_designator{};
	std::string scheme_version{};
	std::string meaning{};
};

/// SEG adapter options.
struct Options {
	/// Accept a DicomFile that already carries a read error.
	/// This is useful for explicitly best-effort inspection of partially read
	/// metadata, but the default keeps SEG construction strict.
	bool allow_partial_source{false};

	/// Validate the required SEG modules needed for safe frame interpretation
	/// while building the metadata index. When false, views may be empty or
	/// throw later for missing raw items.
	bool validate_required_modules{true};
};

/// Options for semantic segment-mask extraction across SEG storage forms.
struct SegmentMaskOptions {
	/// FRACTIONAL threshold in normalized [0, 1] units. The default 0.0 means
	/// "sample > 0"; otherwise samples pass when sample/MaximumFractionalValue
	/// is greater than or equal to this threshold.
	double fractional_threshold{0.0};

	/// When true, a known segment that is not present in a specific frame causes
	/// mask extraction to throw instead of returning an all-zero mask. Segment
	/// numbers absent from SegmentSequence always throw.
	bool error_when_not_present_in_frame{false};
};

/// Borrowed view over one bit-packed 1-bit BINARY SEG frame.
/// `bytes` is the minimal byte window covering the frame, not the whole
/// PixelData element. For Encapsulated Uncompressed frames the window excludes
/// the optional even-length padding byte. Pixel `i` is stored at local bit
/// `first_bit_offset + i`, using the DICOM LSB-first bit order.
struct BinaryFrameBitsView {
	std::span<const std::uint8_t> bytes{};
	std::uint8_t first_bit_offset{0};
	std::size_t bit_count{0};
	std::size_t rows{0};
	std::size_t columns{0};
};

class Segmentation;
class SegmentView;
class SegmentFrameView;
class SourceImageRefView;

namespace detail {

/// Borrowed entry for one item in SegmentSequence.
struct SegmentRecord {
	const DataSet* item{nullptr};
	std::uint16_t number{0};
};

/// Borrowed SourceImageSequence item plus cached ReferencedFrameNumber values.
/// Source images are provenance; overlay decisions should start from
/// FrameOfReferenceUID rather than assuming these are the only display targets.
struct SourceImageRefRecord {
	const DataSet* item{nullptr};
	std::vector<std::uint32_t> referenced_frame_numbers{};
};

/// Borrowed entry for one item in PerFrameFunctionalGroupsSequence.
/// BINARY/FRACTIONAL frames belong to exactly one ReferencedSegmentNumber.
/// LABELMAP frames may contain multiple labels and leave this scalar at 0.
struct SegmentFrameRecord {
	const DataSet* functional_group_item{nullptr};
	std::uint16_t referenced_segment_number{0};
	std::vector<SourceImageRefRecord> source_images{};
};

/// Immutable LABELMAP presence cache for one frame. Empty labels is a valid
/// ready state for all-background frames; `ready` distinguishes it from
/// uninitialized cache.
struct LabelmapFramePresenceCache {
	bool ready{false};
	std::shared_ptr<const std::vector<std::uint16_t>> labels{};
};

/// Immutable all-frame LABELMAP index built lazily after successful validation.
struct LabelmapFrameIndex {
	std::unordered_map<std::uint16_t, std::vector<std::size_t>>
	    frame_indices_by_segment{};
	std::vector<std::size_t> empty_frame_indices{};
};

/// Internal catalog-like lookup index built by from_dicomfile().
/// It owns only small lookup containers; DataSet pointers remain borrowed from
/// the Segmentation-owned DicomFile.
struct SegmentationIndex {
	std::vector<SegmentRecord> segments{};
	std::vector<SegmentFrameRecord> frames{};
	std::unordered_map<std::uint16_t, std::size_t> segment_index_by_number{};
	std::unordered_map<std::uint16_t, std::vector<std::size_t>>
	    frame_indices_by_segment{};
	std::vector<std::size_t> empty_frame_indices{};
};

[[nodiscard]] inline std::uint64_t load_le64_bounded(
    std::span<const std::uint8_t> bytes, std::size_t byte_offset) noexcept {
	if (byte_offset >= bytes.size()) {
		return 0;
	}
	const auto available = bytes.size() - byte_offset;
	const auto count = available < 8u ? available : 8u;
	std::uint64_t word = 0;
	for (std::size_t index = 0; index < count; ++index) {
		word |= static_cast<std::uint64_t>(bytes[byte_offset + index])
		    << (index * 8u);
	}
	return word;
}

inline void validate_binary_frame_bits_view(BinaryFrameBitsView bits) {
	if (bits.first_bit_offset > 7u) {
		throw std::invalid_argument("BinaryFrameBitsView first_bit_offset must be in [0, 7]");
	}
	if (bits.rows != 0 &&
	    bits.columns > std::numeric_limits<std::size_t>::max() / bits.rows) {
		throw std::invalid_argument("BinaryFrameBitsView rows*columns overflows size_t");
	}
	const auto expected_bit_count = bits.rows * bits.columns;
	if (bits.bit_count != expected_bit_count) {
		throw std::invalid_argument("BinaryFrameBitsView bit_count must equal rows*columns");
	}
	if (bits.bit_count >
	    std::numeric_limits<std::size_t>::max() - bits.first_bit_offset) {
		throw std::invalid_argument("BinaryFrameBitsView bit count overflows size_t");
	}
	const auto window_bits =
	    bits.bit_count + static_cast<std::size_t>(bits.first_bit_offset);
	const auto required_bytes =
	    (window_bits / 8u) + ((window_bits % 8u) != 0 ? 1u : 0u);
	if (bits.bytes.size() != required_bytes) {
		throw std::invalid_argument("BinaryFrameBitsView bytes must be the minimal frame window");
	}
}

#ifdef DICOMSDL_SEGMENTATION_TEST_HOOKS
/// Test-only instrumentation for LABELMAP scan/cache regression checks.
void reset_labelmap_frame_scan_count() noexcept;
[[nodiscard]] std::size_t labelmap_frame_scan_count() noexcept;
#endif

} // namespace detail

/// Iterate frame-local pixel indices whose native BINARY SEG bit is set.
template <class Visitor>
void for_each_binary_frame_set_bit(
    BinaryFrameBitsView bits, Visitor&& visitor) {
	detail::validate_binary_frame_bits_view(bits);
	if (bits.bit_count == 0) {
		return;
	}
	auto&& fn = visitor;
	std::size_t local_bit = 0;
	while (local_bit < bits.bit_count) {
		const auto window_bit =
		    static_cast<std::size_t>(bits.first_bit_offset) + local_bit;
		const auto byte_offset = window_bit / 8u;
		const auto bit_offset = static_cast<unsigned>(window_bit % 8u);
		auto word =
		    detail::load_le64_bounded(bits.bytes, byte_offset) >> bit_offset;
		const auto available_bits = static_cast<std::size_t>(64u - bit_offset);
		const auto remaining_bits = bits.bit_count - local_bit;
		const auto chunk_bits =
		    remaining_bits < available_bits ? remaining_bits : available_bits;
		if (chunk_bits < 64u) {
			word &= (std::uint64_t{1} << chunk_bits) - 1u;
		}
		while (word != 0) {
			const auto bit = static_cast<std::size_t>(std::countr_zero(word));
			fn(local_bit + bit);
			word &= word - 1u;
		}
		local_bit += chunk_bits;
	}
}

/// Lightweight borrowed list view over SegmentSequence items.
/// SegmentView objects returned from this view remain valid while the owning
/// Segmentation object is alive and unchanged.
class SegmentListView {
public:
	class iterator {
	public:
		using iterator_category = std::forward_iterator_tag;
		using value_type = SegmentView;
		using difference_type = std::ptrdiff_t;

		iterator() = default;
		[[nodiscard]] SegmentView operator*() const;
		iterator& operator++() noexcept;
		iterator operator++(int) noexcept;
		[[nodiscard]] bool operator==(const iterator& rhs) const noexcept;
		[[nodiscard]] bool operator!=(const iterator& rhs) const noexcept {
			return !(*this == rhs);
		}

	private:
		friend class SegmentListView;
		iterator(const Segmentation* segmentation, std::size_t index) noexcept;

		const Segmentation* segmentation_{nullptr};
		std::size_t index_{0};
	};

	[[nodiscard]] std::size_t size() const noexcept;
	[[nodiscard]] bool empty() const noexcept;
	[[nodiscard]] SegmentView operator[](std::size_t index) const;
	[[nodiscard]] iterator begin() const noexcept;
	[[nodiscard]] iterator end() const noexcept;

private:
	friend class Segmentation;
	explicit SegmentListView(const Segmentation* segmentation) noexcept;

	const Segmentation* segmentation_{nullptr};
};

/// Lightweight borrowed list view over SEG frames.
/// Without a filter it iterates every frame in stored order. When returned by
/// frames_for_segment(), ordinals are local to that segment but SegmentFrameView
/// still reports the original SEG frame index.
class SegmentFrameListView {
public:
	class iterator {
	public:
		using iterator_category = std::forward_iterator_tag;
		using value_type = SegmentFrameView;
		using difference_type = std::ptrdiff_t;

		iterator() = default;
		[[nodiscard]] SegmentFrameView operator*() const;
		iterator& operator++() noexcept;
		iterator operator++(int) noexcept;
		[[nodiscard]] bool operator==(const iterator& rhs) const noexcept;
		[[nodiscard]] bool operator!=(const iterator& rhs) const noexcept {
			return !(*this == rhs);
		}

	private:
		friend class SegmentFrameListView;
		iterator(const Segmentation* segmentation,
		    const std::vector<std::size_t>* frame_indices,
		    std::size_t ordinal) noexcept;

		const Segmentation* segmentation_{nullptr};
		const std::vector<std::size_t>* frame_indices_{nullptr};
		std::size_t ordinal_{0};
	};

	[[nodiscard]] std::size_t size() const noexcept;
	[[nodiscard]] bool empty() const noexcept;
	[[nodiscard]] SegmentFrameView operator[](std::size_t ordinal) const;
	[[nodiscard]] iterator begin() const noexcept;
	[[nodiscard]] iterator end() const noexcept;

private:
	friend class Segmentation;
	SegmentFrameListView(const Segmentation* segmentation,
	    const std::vector<std::size_t>* frame_indices) noexcept;
	[[nodiscard]] std::size_t frame_index_from_ordinal(
	    std::size_t ordinal) const;

	const Segmentation* segmentation_{nullptr};
	const std::vector<std::size_t>* frame_indices_{nullptr};
};

/// Borrowed list of SourceImageSequence references for one SEG frame.
/// These references describe derivation/provenance, not a mandatory display
/// target list.
class SourceImageRefListView {
public:
	[[nodiscard]] std::size_t size() const noexcept;
	[[nodiscard]] bool empty() const noexcept;
	[[nodiscard]] SourceImageRefView operator[](std::size_t index) const;

private:
	friend class SegmentFrameView;
	SourceImageRefListView(const Segmentation* segmentation,
	    std::size_t frame_index) noexcept;

	const Segmentation* segmentation_{nullptr};
	std::size_t frame_index_{0};
};

/// Borrowed view of one SourceImageSequence item.
class SourceImageRefView {
public:
	SourceImageRefView() = default;

	/// ReferencedSOPClassUID from the source image item.
	[[nodiscard]] std::string_view sop_class_uid() const;

	/// ReferencedSOPInstanceUID from the source image item.
	[[nodiscard]] std::string_view sop_instance_uid() const;

	/// ReferencedFrameNumber values, cached in the parent Segmentation index.
	[[nodiscard]] std::span<const std::uint32_t> referenced_frame_numbers() const noexcept;

	/// Raw item access for attributes without a dedicated accessor.
	[[nodiscard]] const DataSet& dataset() const noexcept;

private:
	friend class SourceImageRefListView;
	SourceImageRefView(const DataSet* item,
	    std::span<const std::uint32_t> referenced_frame_numbers) noexcept;

	const DataSet* item_{nullptr};
	std::span<const std::uint32_t> referenced_frame_numbers_{};
};

/// Borrowed view of one SegmentSequence item.
/// Text and code values are read lazily from the underlying DataSet and are not
/// copied unless the caller chooses to copy them.
class SegmentView {
public:
	SegmentView() = default;

	/// SegmentNumber (0062,0004), the stable identifier used by frames.
	[[nodiscard]] std::uint16_t number() const noexcept;

	/// SegmentLabel (0062,0005).
	[[nodiscard]] std::string_view label() const;

	/// SegmentDescription (0062,0006), or empty when absent.
	[[nodiscard]] std::string_view description() const;

	/// SegmentAlgorithmName (0062,0009), or empty when absent.
	[[nodiscard]] std::string_view algorithm_name() const;

	/// SegmentAlgorithmType (0062,0008).
	[[nodiscard]] SegmentAlgorithmType algorithm_type() const;

	/// SegmentedPropertyCategoryCodeSequence (0062,0003), first item only.
	[[nodiscard]] std::optional<CodeView> property_category() const;

	/// SegmentedPropertyTypeCodeSequence (0062,000F), first item only.
	[[nodiscard]] std::optional<CodeView> property_type() const;

	/// AnatomicRegionSequence (0008,2218), first item only.
	[[nodiscard]] std::optional<CodeView> anatomic_region() const;

	/// RecommendedDisplayCIELabValue (0062,000D).
	[[nodiscard]] std::optional<std::array<std::uint16_t, 3>>
	recommended_display_cielab() const;

	/// Raw item access for segment attributes without a dedicated accessor.
	[[nodiscard]] const DataSet& dataset() const noexcept;

private:
	friend class Segmentation;
	friend class SegmentListView;
	SegmentView(const DataSet* item, std::uint16_t number) noexcept;

	const DataSet* item_{nullptr};
	std::uint16_t number_{0};
};

/// Borrowed view of one stored SEG frame.
/// In DICOM SEG, each frame maps to one ReferencedSegmentNumber through
/// SegmentIdentificationSequence. This frame may correspond to a slice-like
/// plane, but the API keeps the neutral DICOM term "frame".
class SegmentFrameView {
public:
	SegmentFrameView() = default;

	/// Zero-based frame index in PixelData / PerFrameFunctionalGroupsSequence.
	[[nodiscard]] std::size_t index() const noexcept;

	/// ReferencedSegmentNumber from SegmentIdentificationSequence.
	/// LABELMAP frames may contain multiple segments; this compatibility
	/// accessor throws for LABELMAP. Use present_segment_numbers() instead.
	[[nodiscard]] std::uint16_t referenced_segment_number() const;

	/// Segment numbers represented by this frame.
	/// BINARY/FRACTIONAL returns the declared ReferencedSegmentNumber without
	/// scanning PixelData. LABELMAP returns actual non-background stored label
	/// values and validates unknown labels while scanning lazily. For LABELMAP,
	/// PixelPaddingValue, when present, identifies the background label.
	[[nodiscard]] std::span<const std::uint16_t> present_segment_numbers() const;

	/// ImagePositionPatient from PlanePositionSequence, per-frame first and then
	/// SharedFunctionalGroupsSequence fallback.
	[[nodiscard]] std::optional<std::array<double, 3>>
	image_position_patient() const;

	/// ImageOrientationPatient from PlaneOrientationSequence, with shared fallback.
	[[nodiscard]] std::optional<std::array<double, 6>>
	image_orientation_patient() const;

	/// PixelSpacing from PixelMeasuresSequence, with shared fallback.
	[[nodiscard]] std::optional<std::array<double, 2>>
	pixel_spacing() const;

	/// SliceThickness from PixelMeasuresSequence, with shared fallback.
	[[nodiscard]] std::optional<double> slice_thickness() const;

	/// Provenance/source-image references for this SEG frame.
	[[nodiscard]] SourceImageRefListView source_images() const;

	/// Raw item for PerFrameFunctionalGroupsSequence at this frame index.
	[[nodiscard]] const DataSet& per_frame_functional_groups_item() const;

	/// Decode this frame into a semantic 0/1 mask for one segment.
	[[nodiscard]] std::vector<std::uint8_t>
	mask_for_segment(std::uint16_t segment_number,
	    const SegmentMaskOptions& options = {}) const;

	/// Decode this frame into a caller-provided semantic 0/1 mask.
	void mask_for_segment_into(std::uint16_t segment_number,
	    std::span<std::uint8_t> out,
	    const SegmentMaskOptions& options = {}) const;

private:
	friend class SegmentFrameListView;
	SegmentFrameView(const Segmentation* segmentation, std::size_t frame_index) noexcept;

	const Segmentation* segmentation_{nullptr};
	std::size_t frame_index_{0};
};

/// Owning high-level adapter for a DICOM SEG or Label Map SEG instance.
/// Construction transfers ownership of a DicomFile so all returned views can
/// borrow string/data pointers without copying the underlying DICOM dataset.
class Segmentation final {
public:
	~Segmentation();

	Segmentation(const Segmentation&) = delete;
	Segmentation& operator=(const Segmentation&) = delete;
	Segmentation(Segmentation&&) = delete;
	Segmentation& operator=(Segmentation&&) = delete;

	[[nodiscard]] bool is_valid() const noexcept;

	/// Instance-level SegmentationType (0062,0001).
	[[nodiscard]] SegmentationType segmentation_type() const noexcept;

	/// Instance-level SegmentationFractionalType (0062,0010).
	[[nodiscard]] SegmentationFractionalType fractional_type() const noexcept;

	/// Instance-level SegmentsOverlap (0062,0013). Missing is undefined.
	[[nodiscard]] SegmentsOverlap segments_overlap() const noexcept;

	/// BitsAllocated for LABELMAP samples, or nullopt for non-LABELMAP SEG.
	[[nodiscard]] std::optional<std::uint16_t>
	labelmap_bits_allocated() const noexcept;

	/// MaximumFractionalValue (0062,000E), meaningful for FRACTIONAL SEG.
	[[nodiscard]] std::optional<std::uint16_t>
	maximum_fractional_value() const noexcept;

	/// FrameOfReferenceUID (0020,0052), the primary spatial compatibility key
	/// when deciding whether a SEG can be directly overlaid on another image.
	[[nodiscard]] std::optional<std::string_view>
	frame_of_reference_uid() const noexcept;

	/// The single item of SharedFunctionalGroupsSequence.
	/// Throws when the item is missing because this is a raw DICOM item accessor.
	[[nodiscard]] const DataSet& shared_functional_groups_item() const;

	/// Non-throwing raw SharedFunctionalGroupsSequence item access for adapters
	/// that need to report structured errors instead of throwing.
	[[nodiscard]] const DataSet* try_shared_functional_groups_item() const noexcept;

	/// Non-throwing raw PerFrameFunctionalGroupsSequence item access for adapters
	/// that need to report structured errors instead of throwing.
	[[nodiscard]] const DataSet*
	try_per_frame_functional_groups_item(std::size_t frame_index) const noexcept;

	/// Rows (0028,0010) for one stored SEG frame.
	[[nodiscard]] std::size_t rows() const noexcept;

	/// Columns (0028,0011) for one stored SEG frame.
	[[nodiscard]] std::size_t columns() const noexcept;

	/// Number of SegmentSequence items indexed by this adapter.
	[[nodiscard]] std::size_t segment_count() const noexcept;

	/// Number of stored frames indexed by this adapter.
	[[nodiscard]] std::size_t frame_count() const noexcept;

	/// All segments described by SegmentSequence.
	[[nodiscard]] SegmentListView segments() const noexcept;

	/// All stored SEG frames in PixelData order.
	[[nodiscard]] SegmentFrameListView frames() const noexcept;

	/// Find a segment by DICOM SegmentNumber.
	[[nodiscard]] std::optional<SegmentView>
	segment_by_number(std::uint16_t segment_number) const noexcept;

	/// Frames where `segment_number` is present.
	/// BINARY/FRACTIONAL SEG uses the declared ReferencedSegmentNumber index.
	/// LABELMAP lazily validates every frame and builds an immutable all-frame
	/// presence index from decoded non-background label values. Unknown
	/// SegmentSequence numbers return empty without scanning PixelData.
	[[nodiscard]] SegmentFrameListView
	frames_for_segment(std::uint16_t segment_number) const;

	/// Number of frames belonging to one DICOM SegmentNumber.
	[[nodiscard]] std::size_t
	segment_frame_count(std::uint16_t segment_number) const;

	/// Segment numbers represented by one frame.
	/// BINARY/FRACTIONAL returns the declared ReferencedSegmentNumber without
	/// scanning PixelData. LABELMAP returns actual non-background stored labels
	/// from a lazily populated immutable presence cache. For LABELMAP,
	/// PixelPaddingValue, when present, identifies the background label.
	[[nodiscard]] std::span<const std::uint16_t>
	present_segment_numbers(std::size_t frame_index) const;

	/// Borrow the bit-packed storage for one BINARY SEG frame.
	/// Native frames may start at a non-zero bit offset in the shared PixelData
	/// stream. Encapsulated Uncompressed frames start at bit offset zero in the
	/// frame item payload. The returned byte window is minimal for the frame and
	/// remains valid only while this Segmentation and its PixelData storage stay
	/// alive.
	[[nodiscard]] BinaryFrameBitsView
	binary_frame_bits(std::size_t frame_index) const;

	/// Iterate frame-local pixel indices whose BINARY SEG bit is set.
	template <class Visitor>
	void for_each_binary_frame_set_bit(
	    std::size_t frame_index, Visitor&& visitor) const {
		dicom::seg::for_each_binary_frame_set_bit(
		    binary_frame_bits(frame_index), std::forward<Visitor>(visitor));
	}

	/// Decode one SEG frame into caller-provided stored-representation samples.
	/// BINARY SEG is unpacked to 0/1 bytes. FRACTIONAL SEG supports 8-bit
	/// samples through the DicomFile decode path and rejects lossy decoded
	/// sources. LABELMAP is supported here only when BitsAllocated == 8 and
	/// validates stored label membership while decoding. If an exception is
	/// thrown, output contents are unspecified and may have been partially
	/// written.
	void decode_frame_into(std::size_t frame_index,
	    std::span<std::uint8_t> out) const;

	/// Decode an 8-bit LABELMAP frame as stored label values. Native and
	/// lossless encapsulated PixelData return the same label samples. Unknown
	/// non-background labels are reported while decoding.
	/// The output element type must exactly match BitsAllocated; this API does
	/// not widen or truncate. Errors may leave `out` partially written.
	void decode_labelmap_frame_into(std::size_t frame_index,
	    std::span<std::uint8_t> out) const;

	/// Decode a 16-bit LABELMAP frame as native-endian stored label values.
	/// Native and lossless encapsulated PixelData return the same label samples.
	/// Unknown non-background labels are reported while decoding.
	/// The output element type must exactly match BitsAllocated; this API does
	/// not widen or truncate. Errors may leave `out` partially written.
	void decode_labelmap_frame_into(std::size_t frame_index,
	    std::span<std::uint16_t> out) const;

	/// Decode a LABELMAP frame as validated native-endian typed sample bytes.
	/// This is not the raw PixelData value byte order; for 16-bit LABELMAP the
	/// returned bytes are the platform-native representation of uint16 samples.
	[[nodiscard]] std::vector<std::uint8_t>
	decode_labelmap_frame_bytes(std::size_t frame_index) const;

	/// Decode one SEG frame into a semantic uint8 0/1 mask for one segment.
	/// Use this common API when segment membership is needed across
	/// BINARY/FRACTIONAL/LABELMAP. SegmentSequence misses are errors; known
	/// segments absent from the frame return a zero mask unless options request
	/// an error.
	[[nodiscard]] std::vector<std::uint8_t>
	mask_for_segment(std::size_t frame_index, std::uint16_t segment_number,
	    const SegmentMaskOptions& options = {}) const;

	/// Decode one SEG frame into a caller-provided semantic uint8 0/1 mask.
	/// If an exception is thrown, output contents are unspecified and may have
	/// been partially written.
	void mask_for_segment_into(std::size_t frame_index,
	    std::uint16_t segment_number, std::span<std::uint8_t> out,
	    const SegmentMaskOptions& options = {}) const;

	/// Validate stored SEG sample semantics.
	/// LABELMAP checks stored label values against SegmentSequence.
	/// FRACTIONAL checks samples against MaximumFractionalValue.
	void validate_label_values() const;

private:
	explicit Segmentation(std::unique_ptr<DicomFile> file) noexcept;
	/// Copy instance-level metadata needed by cheap accessors and validation.
	void extract_instance_metadata(const Options& options);
	/// Build SegmentNumber lookup entries from SegmentSequence.
	void index_segment_sequence_items(const Options& options);
	/// Build frame records and SegmentNumber -> frame-index lists.
	void index_per_frame_functional_group_items(const Options& options);
	/// Return provenance/source-image refs cached during frame indexing.
	[[nodiscard]] const std::vector<detail::SourceImageRefRecord>&
	source_image_refs_for_frame(std::size_t frame_index) const;
	/// Reuse the native decode plan for repeated FRACTIONAL frame decodes.
	[[nodiscard]] const pixel::DecodePlan& fractional_decode_plan() const;
	[[nodiscard]] std::shared_ptr<const std::vector<std::uint16_t>>
	labelmap_presence_for_frame(std::size_t frame_index) const;
	[[nodiscard]] std::shared_ptr<const detail::LabelmapFrameIndex>
	ensure_labelmap_frame_index() const;

	// Own the DicomFile so every view can borrow DataSet pointers/string_views.
	std::unique_ptr<DicomFile> file_;
	SegmentationType segmentation_type_{SegmentationType::unknown};
	SegmentationFractionalType fractional_type_{SegmentationFractionalType::none};
	SegmentsOverlap segments_overlap_{SegmentsOverlap::undefined};
	std::optional<std::uint16_t> labelmap_bits_allocated_{};
	std::optional<std::uint16_t> maximum_fractional_value_{};
	std::optional<std::string_view> frame_of_reference_uid_{};
	const DataSet* shared_functional_groups_item_{nullptr};
	std::bitset<65536> labelmap_valid_labels_{};
	std::optional<std::uint16_t> labelmap_background_value_{};
	// Small catalog/index built once at construction time for fast view lookups.
	detail::SegmentationIndex index_{};
	std::size_t rows_{0};
	std::size_t columns_{0};
	mutable std::mutex fractional_decode_plan_mutex_{};
	mutable std::optional<pixel::DecodePlan> fractional_decode_plan_{};
	mutable std::mutex labelmap_cache_mutex_{};
	mutable std::vector<detail::LabelmapFramePresenceCache>
	    labelmap_presence_cache_{};
	mutable std::shared_ptr<const detail::LabelmapFrameIndex>
	    labelmap_frame_index_{};

	friend class SegmentListView;
	friend class SegmentListView::iterator;
	friend class SegmentFrameListView;
	friend class SegmentFrameListView::iterator;
	friend class SegmentFrameView;
	friend class SourceImageRefListView;
	friend std::unique_ptr<Segmentation> from_dicomfile(
	    std::unique_ptr<DicomFile> file, const Options& options);
};

/// Return true when the file/dataset declares Segmentation Storage SOP Class UID.
[[nodiscard]] bool is_segmentation_storage(const DicomFile& file) noexcept;
[[nodiscard]] bool is_segmentation_storage(const DataSet& ds) noexcept;
[[nodiscard]] bool is_labelmap_segmentation_storage(const DicomFile& file) noexcept;
[[nodiscard]] bool is_labelmap_segmentation_storage(const DataSet& ds) noexcept;
[[nodiscard]] bool is_any_segmentation_storage(const DicomFile& file) noexcept;
[[nodiscard]] bool is_any_segmentation_storage(const DataSet& ds) noexcept;

/// Transfer an already-read DicomFile into the SEG adapter.
/// This is the low-level ownership adapter for callers that already have a
/// parsed DicomFile or need a custom read flow.
[[nodiscard]] std::unique_ptr<Segmentation>
from_dicomfile(std::unique_ptr<DicomFile> file,
    const Options& options = {});

/// Read a DICOM file and adapt it as supported DICOM SEG storage.
/// This includes Segmentation Storage for BINARY/FRACTIONAL and Label Map
/// Segmentation Storage for LABELMAP.
[[nodiscard]] std::unique_ptr<Segmentation>
read_file(const std::filesystem::path& path,
    ReadOptions read_options = {}, Options options = {});

/// Read in-memory DICOM bytes and adapt them as supported DICOM SEG storage.
[[nodiscard]] std::unique_ptr<Segmentation>
read_bytes(const std::uint8_t* data, std::size_t size,
    ReadOptions read_options = {}, Options options = {});

/// Read named in-memory DICOM bytes and adapt them as supported DICOM SEG storage.
[[nodiscard]] std::unique_ptr<Segmentation>
read_bytes(const std::string& name, const std::uint8_t* data,
    std::size_t size, ReadOptions read_options = {}, Options options = {});

/// Take ownership of a byte buffer and adapt it as supported DICOM SEG storage.
[[nodiscard]] std::unique_ptr<Segmentation>
read_bytes(std::string name, std::vector<std::uint8_t>&& buffer,
    ReadOptions read_options = {}, Options options = {});

} // namespace dicom::seg
