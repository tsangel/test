# DICOM Segmentation Storage Support Checklist

This note captures the intended DicomSDL support scope for DICOM Segmentation
Storage, based on the sample files under:

`C:\Lab\img\dicom.sample\FDG-FBB-sample-NCM\FBB\dcm\seg\2a8c5aa325f04cf485ed46098076b3a8`

The sample contains two SEG instances:

- `SEG on CT`
- `SEG on PT`

Both are `Segmentation Storage` objects:

- SOP Class UID: `1.2.840.10008.5.1.4.1.1.66.4`
- Modality: `SEG`
- Segmentation Type: `BINARY`
- Rows x Columns: `256 x 256`
- Number of Frames: `2885`
- Number of Segments: `97`
- Bits Allocated / Stored / High Bit: `1 / 1 / 0`
- Pixel data shape after decode: `(2885, 256, 256)`
- Decoded pixel values: `0` or `1`
- Raw PixelData length: `frames * rows * columns / 8`

The two files have the same segmentation masks and segment definitions; they
differ mainly by referenced source series, one CT and one PT. Treat this source
difference as provenance, while actual overlay eligibility is determined from
`FrameOfReferenceUID` and geometry.

## Core Model

- [x] Treat SEG as a multi-frame image object.
- [x] Do not treat `PixelData` values as label numbers for `BINARY` or `FRACTIONAL` SEG.
- [x] Interpret each frame as one 2D map for one referenced segment.
- [x] In BINARY/FRACTIONAL SEG, one frame has exactly one `ReferencedSegmentNumber`.
- [ ] Multiple segment frames can exist at the same spatial slice position, but one frame should not be modeled as directly containing multiple segments.
- [x] LABELMAP SEG can contain multiple segment pixel values in one frame, but it is out of the MVP scope.
- [x] Resolve frame-to-segment mapping through:
  - `PerFrameFunctionalGroupsSequence`
  - `SegmentIdentificationSequence`
  - `ReferencedSegmentNumber`
- [x] Resolve segment metadata through:
  - `SegmentSequence`
  - `SegmentNumber`
  - `SegmentLabel`
  - `SegmentDescription`
  - `SegmentAlgorithmType`
  - `SegmentAlgorithmName`
  - `SegmentedPropertyCategoryCodeSequence`
  - `SegmentedPropertyTypeCodeSequence`
  - `AnatomicRegionSequence`
  - `RecommendedDisplayCIELabValue`
- [ ] Resolve per-frame geometry through:
  - `PlanePositionSequence`
  - `ImagePositionPatient`
  - `PlaneOrientationSequence`
  - `ImageOrientationPatient`
  - `PixelMeasuresSequence`
  - `PixelSpacing`
  - `SliceThickness`
  - `SpacingBetweenSlices`
- [ ] Resolve source references through:
  - `ReferencedSeriesSequence`
  - `ReferencedInstanceSequence`
  - `DerivationImageSequence`
  - `SourceImageSequence`

## Spatial Coordinate System And Overlay

Do not model SEG as a mask that can only be displayed on the exact source image.
For display and processing, the important question is whether the SEG and the
target image share the same patient coordinate system.

- [x] Treat `FrameOfReferenceUID (0020,0052)` as the primary spatial compatibility key.
- [x] Model a SEG instance as having one `FrameOfReferenceUID`.
- [x] Since `FrameOfReferenceUID` has `VM 1`, do not design an API where individual frames have different frames of reference.
- [x] Even when multiple `ReferencedSeriesSequence` items are present, treat the SEG mask itself as belonging to one spatial coordinate system.
- [x] Treat per-frame `SourceImageSequence` as provenance/retrieve/debug metadata, not as the mandatory display target.
- [ ] An image not directly referenced by the SEG can still be an overlay target if it has the same `FrameOfReferenceUID`.
- [ ] Actual overlay/resampling also uses geometry such as `ImagePositionPatient`, `ImageOrientationPatient`, and `PixelSpacing`.
- [ ] If the target image and SEG have different `FrameOfReferenceUID` values, do not allow direct overlay; require a separate registration step.
- [ ] A SEG without `FrameOfReferenceUID` is a fallback case governed by the DICOM same sampling/extent rule for referenced images; the first API should expose the metadata conservatively.

## Support Scope

### MVP Read Support

- [x] Recognize Segmentation Storage SOP Class UID:
  - `1.2.840.10008.5.1.4.1.1.66.4`
- [x] Parse `SegmentationType`.
- [x] Support `BINARY`.
- [x] Support `FRACTIONAL`.
- [x] Parse `SegmentationFractionalType` and `MaximumFractionalValue` as instance-level metadata.
- [x] Expose segment metadata without decoding `PixelData`.
- [x] Expose frame metadata without decoding all frames.
- [x] Decode BINARY 1-bit packed pixel data.
- [x] Decode FRACTIONAL 8-bit pixel data.
- [x] Provide frame-level decode into caller-owned buffers.
- [x] Preserve frame order as stored, but do not assume it is spatial order.
- [x] Expose geometry metadata per frame, but keep geometry-based sorting and 3D volume reconstruction out of the MVP.

### Frame-Level Convenience Support

- [x] Build segment-number to segment-index lookup.
- [x] Build frame-index to referenced-segment-number lookup.
- [x] Provide `segment_frame_count(segment_number)` without pixel decode.
- [x] Provide `frames_for_segment(segment_number) -> SegmentFrameListView` without pixel decode.
- [x] Provide frame-index based 2D decode APIs.
- [x] Do not include an API that returns a whole mask from only `segment_number` in the MVP.

### Post-MVP Volume/Derived APIs

- [ ] Consider `get_segment_mask(segment_number)` or a C++ equivalent for segment-level masks.
- [ ] Consider `get_segment_volume(segment_number)` or a C++ equivalent for volume reconstruction.
- [ ] Fill missing spatial slices explicitly when reconstructing a volume.
- [ ] Make the missing-slice policy explicit.
- [ ] Provide overlap-aware conversion to label map.
- [ ] Provide lossy conversion warnings for overlapping BINARY/FRACTIONAL segments.

### Write Support, Later

- [ ] Write BINARY Segmentation Storage.
- [ ] Write FRACTIONAL Segmentation Storage.
- [ ] Correctly pack BINARY `PixelData` as 1-bit data.
- [ ] Correctly write FRACTIONAL `PixelData` as 8-bit data.
- [ ] Generate valid `SegmentSequence`.
- [ ] Generate valid `SharedFunctionalGroupsSequence`.
- [ ] Generate valid `PerFrameFunctionalGroupsSequence`.
- [ ] Generate valid `DimensionIndexSequence`.
- [ ] Generate source image references.
- [ ] Validate `FrameOfReferenceUID` and geometry consistency.
- [ ] Prefer the same `FrameOfReferenceUID` as the source images when writing, and document that a different frame of reference requires a separate object such as Spatial Registration.
- [ ] Defer Label Map Segmentation Storage unless separately scoped:
  - SOP Class UID: `1.2.840.10008.5.1.4.1.1.66.7`

## Non-goals For Initial Support

- [ ] Do not include Surface Segmentation Storage in the first scope.
- [ ] Do not include RTSTRUCT conversion in the first scope.
- [ ] Do not include automatic registration/resampling in the first scope.
- [ ] Do not include rendering/viewer behavior in the first scope.
- [ ] Do not include WSI tiled SEG optimization in the first scope.
- [ ] Do not include segment-level 3D volume mask reconstruction in the first scope.
- [ ] Do not include derived extent values such as `z_range` or `spatial_extent` in the first scope.
- [ ] Do not include label-map conversion in the first scope.

## C++ API Shape

Prefer a zero-copy view API for C++ internals, but make the high-level
`Segmentation` object itself a heap-owned object that owns its `DicomFile`.
Use owning snapshots where lifetime safety is more important, such as Python
bindings or long-term storage.

### Module Boundary And Ownership

Keep SEG support out of `dicom.h`. Expose it through a separate public header.

```cpp
#include "dicom.h"      // core DICOM read/parse/pixel API
#include "dicom_seg.h"  // optional SEG interpretation API
```

Use `dicom::seg` as the public namespace.

```cpp
namespace dicom {

[[nodiscard]] std::unique_ptr<DicomFile>
read_file(const std::filesystem::path& path,
          ReadOptions options = {});

[[nodiscard]] std::unique_ptr<DicomFile>
read_bytes(const std::uint8_t* data,
           std::size_t size,
           ReadOptions options = {});

} // namespace dicom

namespace dicom::seg {

class Segmentation;

struct Options {
    bool allow_partial_source{false};
    bool validate_required_modules{true};
};

[[nodiscard]] bool is_segmentation_storage(const DicomFile& file) noexcept;
[[nodiscard]] bool is_segmentation_storage(const DataSet& ds) noexcept;

[[nodiscard]] std::unique_ptr<Segmentation>
from_dicomfile(std::unique_ptr<DicomFile> file,
               const Options& options = {});

} // namespace dicom::seg
```

Design rules:

- [x] `read_file` and `read_bytes` remain core `dicom` namespace responsibilities.
- [x] The SEG module only interprets an already-read `DicomFile` as a SEG object.
- [x] Prefer the public API name `from_dicomfile` over `open`/`try_open`.
- [x] Do not introduce a `try_open` / `nullptr on failure` policy.
- [x] Use `is_segmentation_storage(...)` to check whether the object is SEG.
- [x] `from_dicomfile(...)` throws for `nullptr`, non-SEG input, or required metadata errors.
- [x] If `DicomFile::has_error()` is true and `allow_partial_source == false`, `from_dicomfile(...)` fails.
- [x] Keep `ReadOptions.keep_on_error` semantics confined to `read_file/read_bytes`.
- [x] `Segmentation` owns a `std::unique_ptr<DicomFile>`.
- [x] `from_dicomfile(...)` returns `std::unique_ptr<Segmentation>`.
- [x] Do not expose borrowed `view(const DicomFile&)` in the first public API.
- [ ] If needed later, consider an advanced API named `borrow(...)`.

### Naming Note

Use `from_dicomfile`, intentionally not `from_dicom_file`.

Background:

- [ ] `DicomFile` is a DicomSDL file/session wrapper class, not a formal DICOM standard object name.
- [ ] In `is_dicom_file(path)`, `dicom_file` means DICOM File Format or a filesystem file concept.
- [x] In `from_dicomfile(std::unique_ptr<DicomFile>)`, `dicomfile` means the DicomSDL `DicomFile` class object.
- [x] Keeping `dicomfile` together distinguishes path-level DICOM file format APIs from class-object interpretation APIs.
- [ ] Existing DicomSDL APIs already treat `dataset` and `dataelement` as single compound API words.
- [ ] Existing file-format or filesystem APIs use separated `file`, such as `read_file`, `is_dicom_file`, and `write_file`.
- [ ] Because of that, `from_dicom_file(...)` could be misread as a path-level file-format API.
- [x] `from_dicomfile(...)` is chosen to make the accepted type, the DicomSDL `DicomFile` class, explicit in the name.
- [ ] If a future `DataSet`-based API is added, use `from_dataset(...)`.
- [ ] `from_dataset(...)` also matches the DICOM community's common `Dataset` spelling and library practice.
- [ ] References such as highdicom's `from_dataset(...)` and DCMTK's `loadDataset(...)` / `read(...)` support the idea of interpreting an already-read DICOM object as a higher-level object.
- [x] In DicomSDL, however, core `read_file/read_bytes` already return `std::unique_ptr<DicomFile>`, so the first API should make ownership transfer explicit with `from_dicomfile(std::unique_ptr<DicomFile>)`.

Examples:

```cpp
auto seg = dicom::seg::from_dicomfile(dicom::read_file(path));
auto sr  = dicom::sr::from_dicomfile(dicom::read_file(path));
auto pm  = dicom::pm::from_dicomfile(dicom::read_file(path));

if (dicom::is_dicom_file(path)) {
    auto file = dicom::read_file(path);
    auto seg = dicom::seg::from_dicomfile(std::move(file));
}
```

### Basic Entry Point

```cpp
class Segmentation final {
public:
    Segmentation(const Segmentation&) = delete;
    Segmentation& operator=(const Segmentation&) = delete;
    Segmentation(Segmentation&&) = delete;
    Segmentation& operator=(Segmentation&&) = delete;

    [[nodiscard]] bool is_valid() const noexcept;
    [[nodiscard]] SegmentationType segmentation_type() const;
    [[nodiscard]] SegmentationFractionalType fractional_type() const noexcept;
    [[nodiscard]] std::optional<std::uint16_t>
    maximum_fractional_value() const noexcept;
    [[nodiscard]] std::optional<std::string_view>
    frame_of_reference_uid() const noexcept;

    [[nodiscard]] const DataSet&
    shared_functional_groups_item() const;

    [[nodiscard]] SegmentListView segments() const;
    [[nodiscard]] SegmentFrameListView frames() const;

    [[nodiscard]] std::optional<SegmentView>
    segment_by_number(std::uint16_t segment_number) const;

    [[nodiscard]] SegmentFrameListView
    frames_for_segment(std::uint16_t segment_number) const;

    [[nodiscard]] std::size_t
    segment_frame_count(std::uint16_t segment_number) const;

    // MVP pixel APIs stop at frame-level decode.
    void decode_frame_into(std::size_t frame_index, std::span<std::uint8_t> out) const;

private:
    std::unique_ptr<DicomFile> file_;
    SegmentationIndex index_;
};
```

Spatial API rules:

- [x] `frame_of_reference_uid()` is the central accessor for SEG spatial compatibility.
- [x] If `referenced_series()` or `source_images()` are added, keep them as provenance-oriented helper APIs.
- [ ] A helper such as `can_overlay_directly(seg, image)` should start with `FrameOfReferenceUID` equality.
- [ ] The helper should only answer direct overlay eligibility and should not promise registration/resampling success.

Functional Groups raw access rules:

- [x] Use `shared_functional_groups_item()` for the item of `SharedFunctionalGroupsSequence (5200,9229)`.
- [x] Use `per_frame_functional_groups_item()` for the frame-specific item of `PerFrameFunctionalGroupsSequence (5200,9230)`.
- [x] DICOM keywords belong to sequence attributes, not to sequence items, so include `_item` in the API name.
- [x] `shared_functional_groups_item()` returns the item dataset containing functional group macros shared by the SEG instance.
- [x] Since `SharedFunctionalGroupsSequence` has only one item, an item accessor is more practical than a sequence view.
- [x] `per_frame_functional_groups_item()` returns the item dataset for the current frame rank.
- [x] Geometry accessors should look in the per-frame item first, then fall back to the shared item.
- [x] Raw item accessors are escape hatches for DICOM attributes that do not yet have dedicated accessors.
- [x] Raw item accessors may throw when a required item is missing, so do not mark them `noexcept`.

Segmentation type rules:

- [x] `SegmentationType` is an instance-level value.
- [x] Treat `SegmentationFractionalType` and `MaximumFractionalValue` as instance-level values for FRACTIONAL SEG.
- [x] Do not add segment-level or frame-level `segmentation_type()` accessors.
- [x] Keep `SegmentationType::labelmap` as a future/post-MVP recognition value if useful, but keep Label Map Segmentation Storage decode out of the MVP.

### Segment List View

```cpp
class SegmentListView {
public:
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;

    [[nodiscard]] SegmentView operator[](std::size_t index) const;

    [[nodiscard]] SegmentIterator begin() const;
    [[nodiscard]] SegmentIterator end() const;
};
```

### Segment Frame List View

`SegmentFrameListView` represents both the complete frame list and a filtered
frame list for one segment. Public APIs should not expose the raw frame index
array directly.

```cpp
class SegmentFrameListView {
public:
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;

    [[nodiscard]] SegmentFrameView operator[](std::size_t index) const;

    [[nodiscard]] SegmentFrameIterator begin() const;
    [[nodiscard]] SegmentFrameIterator end() const;

private:
    const Segmentation* segmentation_{nullptr};

    // nullptr means the complete frame list; a value means a filtered
    // frame-index list owned by Segmentation. An empty vector can mean no
    // matching frames for that segment.
    const std::vector<std::size_t>* frame_indices_{nullptr};
};
```

Implementation note:

- [x] `Segmentation::frames()` returns a `SegmentFrameListView` over all frames.
- [x] `Segmentation::frames_for_segment(number)` returns a `SegmentFrameListView` filtered to frames for that segment.
- [x] Internally, the filtered view may use a `segment_number -> frame-index list` index.
- [x] `SegmentFrameView::index()` always returns the original frame index in the SEG instance.
- [x] For a filtered view returned by `frames_for_segment(number)`, `operator[]` uses the filtered-list ordinal, which may differ from `SegmentFrameView::index()`.
- [ ] `SegmentView::frames()` is convenient, but it needs a parent `Segmentation` pointer; consider it as a post-MVP convenience API.

### Segment View

`SegmentView` should be cheap and non-owning. It should not own strings or
pixel data. Store a reference to the `SegmentSequence` item dataset and cache
only small values that are used frequently, especially `SegmentNumber`.

```cpp
class SegmentView {
public:
    [[nodiscard]] std::uint16_t number() const noexcept;

    // Return borrowed views where possible. Valid only while the owning
    // DicomFile/DataSet/Segmentation remains alive and unchanged.
    [[nodiscard]] std::string_view label() const;
    [[nodiscard]] std::string_view description() const;
    [[nodiscard]] std::string_view algorithm_name() const;

    [[nodiscard]] SegmentAlgorithmType algorithm_type() const;

    [[nodiscard]] std::optional<CodeView> property_category() const;
    [[nodiscard]] std::optional<CodeView> property_type() const;
    [[nodiscard]] std::optional<CodeView> anatomic_region() const;

    [[nodiscard]] std::optional<std::array<std::uint16_t, 3>>
    recommended_display_cielab() const;

    // Escape hatch for unsupported or newly added SEG attributes.
    [[nodiscard]] const DataSet& dataset() const noexcept;

private:
    const DataSet* item_{nullptr};
    std::uint16_t number_{0};
};
```

Implementation note:

- [x] Prefer storing `const DataSet* item_` plus cached `number_`.
- [x] Avoid storing one `const DataElement*` per field unless profiling proves it matters.
- [x] Avoid owning `std::string` in the default C++ view.
- [x] Parse string fields lazily from `item_`.
- [x] Return `std::string_view` for cheap C++ access.
- [ ] Provide owning snapshots for APIs that outlive the source dataset.

### Owning Segment Snapshot

Use this for Python bindings, external storage, or APIs that need independent
lifetime from the DICOM dataset.

```cpp
struct SegmentInfo {
    std::uint16_t number{};
    std::string label;
    std::string description;
    SegmentAlgorithmType algorithm_type{SegmentAlgorithmType::unknown};
    std::string algorithm_name;
    std::optional<Code> property_category;
    std::optional<Code> property_type;
    std::optional<Code> anatomic_region;
    std::optional<std::array<std::uint16_t, 3>> recommended_display_cielab;
};

class Segmentation {
public:
    [[nodiscard]] std::vector<SegmentInfo> segment_infos() const;
};
```

### Code View And Code Snapshot

```cpp
struct CodeView {
    std::string_view value;
    std::string_view scheme_designator;
    std::string_view scheme_version;
    std::string_view meaning;
};

struct Code {
    std::string value;
    std::string scheme_designator;
    std::string scheme_version;
    std::string meaning;
};
```

### Frame View

Frame metadata should be separate from segment metadata. A segment is a label
definition; a frame is one stored 2D mask/fractional map. In the MVP
BINARY/FRACTIONAL model, one `SegmentFrameView` has one
`referenced_segment_number()`.

```cpp
class SegmentFrameView {
public:
    [[nodiscard]] std::size_t index() const noexcept;
    [[nodiscard]] std::uint16_t referenced_segment_number() const;

    [[nodiscard]] std::optional<std::array<double, 3>>
    image_position_patient() const;

    [[nodiscard]] std::optional<std::array<double, 6>>
    image_orientation_patient() const;

    [[nodiscard]] std::optional<std::array<double, 2>>
    pixel_spacing() const;

    [[nodiscard]] std::optional<double> slice_thickness() const;

    // Provenance/retrieve/debug metadata. Do not use as the primary overlay key.
    [[nodiscard]] SourceImageRefListView source_images() const;

    [[nodiscard]] const DataSet&
    per_frame_functional_groups_item() const;
};
```

### Source Image Ref View

Source image references are provenance/retrieve/debug metadata, not the primary
overlay key. If exposed in the MVP, keep them as lightweight reference views.

```cpp
class SourceImageRefView {
public:
    [[nodiscard]] std::string_view sop_class_uid() const;
    [[nodiscard]] std::string_view sop_instance_uid() const;

    // Present only when the source is multi-frame and only specific frames are referenced.
    [[nodiscard]] std::span<const std::uint32_t>
    referenced_frame_numbers() const;

    [[nodiscard]] const DataSet& dataset() const noexcept;
};

class SourceImageRefListView {
public:
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] SourceImageRefView operator[](std::size_t index) const;
};
```

### Types And Enums

```cpp
enum class SegmentationType : std::uint8_t {
    unknown,
    binary,
    fractional,
    labelmap
};

enum class SegmentationFractionalType : std::uint8_t {
    none,
    probability,
    occupancy,
    unknown
};

enum class SegmentAlgorithmType : std::uint8_t {
    unknown,
    automatic_,
    semiautomatic,
    manual
};
```

Use `automatic_` or another spelling that avoids conflicts with platform macros
or style rules.

## Sample Expectations

For the sample folder, `seg->segments()` should expose 97 segment definitions.

Examples:

```cpp
auto seg = dicom::seg::from_dicomfile(dicom::read_file(path));
auto segments = seg->segments();

segments.size();                // 97

auto first = segments[0];
first.number();                 // 1
first.label();                  // "Left-Cerebral-White-Matter"
first.description();            // "Left-Cerebral-White-Matter"
first.algorithm_type();         // SegmentAlgorithmType::automatic_
first.algorithm_name();         // "NCM-Brain"

auto last = segments[96];
last.number();                  // 97
last.label();                   // "Right-Insula"
```

The sample segment groups are:

- [ ] `1-17`: left-side brain structures and CSF-related structures
- [ ] `18-31`: right-side equivalents
- [ ] `32`: `WM-hypointensities`
- [ ] `33-35`: `Midbrain`, `Pons`, `Medulla`
- [ ] `36-66`: left cortical regions
- [ ] `67-97`: right cortical regions

## Cost Boundaries

- [x] `dicom::seg::from_dicomfile(...)` should only build metadata indexes and should not decode pixels.
- [x] `seg->segments()` must be cheap.
- [x] `SegmentView::label()` and similar accessors may parse lazily but should not decode pixels.
- [x] `seg->frames()` must be metadata-only.
- [x] `segment_frame_count()` must be metadata-only.
- [x] `voxel_count` requires pixel decode and should not be part of default `SegmentView`.
- [x] MVP pixel decode is limited to frame-by-frame access.
- [x] Derived values such as `z_range`, `spatial_extent`, and `voxel_count` do not belong in the MVP API.
- [x] Volume reconstruction belongs in a post-MVP explicit API.
- [x] Label-map conversion belongs in a post-MVP explicit API because it can be lossy when segments overlap.

## Python Binding Recommendation

Python can expose a safer, more Pythonic owning API:

```python
seg = dicomsdl.seg.from_dicomfile(dicomsdl.read_file(path))
# Or a Python convenience wrapper:
seg = dicomsdl.seg.read_file(path)

seg.segments
# list[SegmentInfo]

seg.frames
# list[SegmentFrameInfo] or lazy sequence

seg.decode_frame(frame_index)
# numpy 2D array, explicit frame-level allocation
```

Checklist:

- [ ] Use `from_dicomfile(...)` in Python with the same meaning as C++.
- [ ] Consider a Python convenience API named `dicomsdl.seg.read_file(...)`.
- [ ] If a Python convenience read API exists, keep it aligned with core `dicomsdl.read_file(...)` read options and error model.
- [ ] Return Python-owned strings for `SegmentInfo`.
- [ ] Avoid exposing raw `std::string_view` lifetimes to Python users.
- [ ] MVP Python pixel APIs should stop at frame-index based 2D decode.
- [ ] Keep pixel allocation explicit.
- [ ] Keep `get_segment_mask(segment_number)` and `get_segment_volume(segment_number)` as post-MVP APIs.
- [ ] Consider a lazy Python sequence wrapper for very large SEG objects.
