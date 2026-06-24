#pragma once

#include "dicom.h"

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
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

	/// Validate the required SEG modules needed by this MVP while building the
	/// metadata index. When false, views may be empty or throw later for missing
	/// raw items.
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
/// A SEG frame belongs to exactly one referenced segment number, even though the
/// same segment may span many frames.
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

} // namespace detail

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
	/// values and validates unknown labels while scanning lazily.
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

/// Owning high-level adapter for a DICOM Segmentation Storage instance.
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

	/// Frames whose ReferencedSegmentNumber matches `segment_number`.
	/// For LABELMAP this lazily validates every frame and builds an immutable
	/// all-frame presence index. Unknown SegmentSequence numbers return empty.
	[[nodiscard]] SegmentFrameListView
	frames_for_segment(std::uint16_t segment_number) const;

	/// Number of frames belonging to one DICOM SegmentNumber.
	[[nodiscard]] std::size_t
	segment_frame_count(std::uint16_t segment_number) const;

	/// Segment numbers represented by one frame.
	[[nodiscard]] std::span<const std::uint16_t>
	present_segment_numbers(std::size_t frame_index) const;

	/// Decode one SEG frame into caller-provided 8-bit samples.
	/// BINARY SEG is unpacked to 0/1 bytes. FRACTIONAL SEG currently supports the
	/// MVP native 8-bit case and reuses one DicomFile decode plan. LABELMAP is
	/// supported here only when BitsAllocated == 8.
	void decode_frame_into(std::size_t frame_index,
	    std::span<std::uint8_t> out) const;

	/// Decode an 8-bit LABELMAP frame as stored label values.
	void decode_labelmap_frame_into(std::size_t frame_index,
	    std::span<std::uint8_t> out) const;

	/// Decode a 16-bit LABELMAP frame as native-endian stored label values.
	void decode_labelmap_frame_into(std::size_t frame_index,
	    std::span<std::uint16_t> out) const;

	/// Decode a LABELMAP frame as validated native-endian typed sample bytes.
	/// This is not the raw PixelData value byte order.
	[[nodiscard]] std::vector<std::uint8_t>
	decode_labelmap_frame_bytes(std::size_t frame_index) const;

	/// Decode one SEG frame into a semantic 0/1 mask for one segment.
	[[nodiscard]] std::vector<std::uint8_t>
	mask_for_segment(std::size_t frame_index, std::uint16_t segment_number,
	    const SegmentMaskOptions& options = {}) const;

	/// Decode one SEG frame into a caller-provided semantic 0/1 mask.
	void mask_for_segment_into(std::size_t frame_index,
	    std::uint16_t segment_number, std::span<std::uint8_t> out,
	    const SegmentMaskOptions& options = {}) const;

	/// Validate all LABELMAP stored label values against SegmentSequence.
	/// Non-LABELMAP SEG instances are a no-op.
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
	std::optional<std::uint16_t> labelmap_bits_allocated_{};
	std::optional<std::uint16_t> maximum_fractional_value_{};
	std::optional<std::string_view> frame_of_reference_uid_{};
	const DataSet* shared_functional_groups_item_{nullptr};
	std::bitset<65536> labelmap_valid_labels_{};
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

/// Read a DICOM file and adapt it as DICOM Segmentation Storage.
[[nodiscard]] std::unique_ptr<Segmentation>
read_file(const std::filesystem::path& path,
    ReadOptions read_options = {}, Options options = {});

/// Read in-memory DICOM bytes and adapt them as DICOM Segmentation Storage.
[[nodiscard]] std::unique_ptr<Segmentation>
read_bytes(const std::uint8_t* data, std::size_t size,
    ReadOptions read_options = {}, Options options = {});

/// Read named in-memory DICOM bytes and adapt them as DICOM Segmentation Storage.
[[nodiscard]] std::unique_ptr<Segmentation>
read_bytes(const std::string& name, const std::uint8_t* data,
    std::size_t size, ReadOptions read_options = {}, Options options = {});

/// Take ownership of a byte buffer and adapt it as DICOM Segmentation Storage.
[[nodiscard]] std::unique_ptr<Segmentation>
read_bytes(std::string name, std::vector<std::uint8_t>&& buffer,
    ReadOptions read_options = {}, Options options = {});

} // namespace dicom::seg
