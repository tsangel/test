#pragma once

#include "dicom.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace dicom::seg {

/// DICOM Segmentation Type (0062,0001).
/// The MVP accepts BINARY and FRACTIONAL SEG instances. LABELMAP is recognized
/// as a named value but is intentionally left for post-MVP decoding support.
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

/// Borrowed SourceImageSequence item plus copied ReferencedFrameNumber values.
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

	/// ReferencedFrameNumber values, copied into the Segmentation index so the
	/// returned span stays stable with the parent Segmentation.
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
	[[nodiscard]] std::uint16_t referenced_segment_number() const;

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
	[[nodiscard]] SegmentFrameListView
	frames_for_segment(std::uint16_t segment_number) const noexcept;

	/// Number of frames belonging to one DICOM SegmentNumber.
	[[nodiscard]] std::size_t
	segment_frame_count(std::uint16_t segment_number) const noexcept;

	/// Decode one SEG frame into caller-provided 8-bit samples.
	/// BINARY SEG is unpacked to 0/1 bytes. FRACTIONAL SEG currently supports the
	/// MVP native 8-bit case and delegates frame decode to DicomFile.
	void decode_frame_into(std::size_t frame_index,
	    std::span<std::uint8_t> out) const;

private:
	explicit Segmentation(std::unique_ptr<DicomFile> file) noexcept;
	/// Copy instance-level metadata needed by cheap accessors and validation.
	void extract_instance_metadata(const Options& options);
	/// Build SegmentNumber lookup entries from SegmentSequence.
	void index_segment_sequence_items(const Options& options);
	/// Build frame records and SegmentNumber -> frame-index lists.
	void index_per_frame_functional_group_items(const Options& options);

	// Own the DicomFile so every view can borrow DataSet pointers/string_views.
	std::unique_ptr<DicomFile> file_;
	SegmentationType segmentation_type_{SegmentationType::unknown};
	SegmentationFractionalType fractional_type_{SegmentationFractionalType::none};
	std::optional<std::uint16_t> maximum_fractional_value_{};
	std::optional<std::string_view> frame_of_reference_uid_{};
	const DataSet* shared_functional_groups_item_{nullptr};
	// Small catalog/index built once at construction time for fast view lookups.
	detail::SegmentationIndex index_{};
	std::size_t rows_{0};
	std::size_t columns_{0};

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

/// Transfer an already-read DicomFile into the SEG adapter.
/// This mirrors DicomSDL's core read_file/read_bytes ownership style: callers
/// read bytes with core APIs first, then adapt the resulting DicomFile into the
/// higher-level module.
[[nodiscard]] std::unique_ptr<Segmentation>
from_dicomfile(std::unique_ptr<DicomFile> file,
    const Options& options = {});

} // namespace dicom::seg
