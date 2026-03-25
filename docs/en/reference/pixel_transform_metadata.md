# Pixel Transform Metadata Resolution

DicomSDL resolves post-decode pixel transform metadata with frame-aware precedence for
Enhanced multi-frame objects.

## Resolution order

For frame-aware accessors, the lookup order is:

1. `PerFrameFunctionalGroupsSequence (5200,9230)` for the requested frame
2. `SharedFunctionalGroupsSequence (5200,9229)`
3. Root dataset fallback

This applies to:

- `rescale_transform(frame)`
- `window_transform(frame)`
- `voi_lut(frame)`

The default no-argument accessors remain available and resolve as `frame 0`.

## Rescale transform

`rescale_transform(frame)` uses:

- `PixelValueTransformationSequence (0028,9145)` from the requested Per-Frame Functional Group
- then `PixelValueTransformationSequence (0028,9145)` from Shared Functional Groups
- then root-level `RescaleSlope (0028,1053)` / `RescaleIntercept (0028,1052)`

This means Enhanced multi-frame frame-specific rescale metadata overrides legacy root-level
rescale tags.

## Window transform

`window_transform(frame)` uses:

- `FrameVOILUTSequence (0028,9132)` from the requested Per-Frame Functional Group
- then `FrameVOILUTSequence (0028,9132)` from Shared Functional Groups
- then root-level `WindowCenter (0028,1050)`, `WindowWidth (0028,1051)`,
  and `VOILUTFunction (0028,1056)`

Only the first window alternative is exposed at the moment.

## VOI LUT

`voi_lut(frame)` uses:

- `FrameVOILUTSequence (0028,9132)` from the requested Per-Frame Functional Group
- then `FrameVOILUTSequence (0028,9132)` from Shared Functional Groups
- then root-level `VOILUTSequence (0028,3010)`

Only the first LUT item is exposed at the moment.

## Modality LUT

`modality_lut(frame)` is intentionally different:

- it returns the root-level `ModalityLUTSequence (0028,3000)` only
- the same root LUT is treated as shared across frames

Frame-specific modality-equivalent transforms in Enhanced multi-frame objects are represented by
`PixelValueTransformationSequence (0028,9145)` and therefore surface through
`rescale_transform(frame)`, not through `modality_lut(frame)`.

## Palette LUT

`palette_lut()` is intentionally narrower than the frame-aware transform accessors above:

- it returns the classic root-level Palette Color Lookup Table Module used with
  `Photometric Interpretation = PALETTE COLOR`
- it does not currently expose a `palette_lut(frame)` variant

This is deliberate because DICOM uses multiple palette-related models that are not equivalent:

- the classic Palette Color Lookup Table Module is a root-level indexed-pixel mapping model
- the Supplemental Palette Color Lookup Table Module is a different display model used with
  `Pixel Presentation = COLOR`
- the Enhanced Palette Color Lookup Table Module defines an Enhanced Blending and Display
  Pipeline using root-level sequences such as `DataFrameAssignmentSequence (0028,1401)` and
  `EnhancedPaletteColorLookupTableSequence (0028,140B)`

As a result, `apply_palette_lut()` and `DicomFile::palette_lut()` currently model only the
classic indexed `PALETTE COLOR` case. They do not interpret Supplemental Palette or Enhanced
Palette display-pipeline metadata, and they do not use Per-Frame/Shared Functional Group
precedence.

The classic accessor supports both discrete palette data and segmented palette data for the
root-level `PALETTE COLOR` module. The shared `PaletteLut` payload now also carries optional
alpha LUT values when present, so supplemental and enhanced palette metadata can reuse the same
palette payload type without duplicating alpha arrays.

Related root-level palette metadata is exposed separately:

- `pixel_presentation()` returns `Pixel Presentation (0008,9205)` when present
- `supplemental_palette()` returns the root-level Supplemental Palette metadata model
- `enhanced_palette()` returns the root-level Enhanced Palette display-pipeline metadata model

## C++ API

Relevant `DicomFile` accessors:

```cpp
std::optional<pixel::RescaleTransform> rescale_transform() const;
std::optional<pixel::RescaleTransform> rescale_transform(std::size_t frame_index) const;

std::optional<pixel::WindowTransform> window_transform() const;
std::optional<pixel::WindowTransform> window_transform(std::size_t frame_index) const;

std::optional<pixel::VoiLut> voi_lut() const;
std::optional<pixel::VoiLut> voi_lut(std::size_t frame_index) const;

std::optional<pixel::ModalityLut> modality_lut() const;
std::optional<pixel::ModalityLut> modality_lut(std::size_t frame_index) const;

std::optional<pixel::PixelPresentation> pixel_presentation() const;
std::optional<pixel::PaletteLut> palette_lut() const;
std::optional<pixel::SupplementalPaletteInfo> supplemental_palette() const;
std::optional<pixel::EnhancedPaletteInfo> enhanced_palette() const;
```

For independent viewers and rendering tools, the intended C++ building blocks are:

- `pixel_buffer(frame)` / `decode_into(...)` for the decoded source pixels
- `rescale_transform(frame)`, `window_transform(frame)`, `voi_lut(frame)` for frame-aware
  grayscale display metadata
- `palette_lut()`, `supplemental_palette()`, `enhanced_palette()` for palette metadata
- `apply_rescale()`, `apply_window()`, `apply_voi_lut()`, `apply_palette_lut()` for explicit
  CPU-side transform stages when the caller wants them

## Python API

Python exposes both frame-0 properties and frame-aware methods:

```python
dicom_file.rescale_transform
dicom_file.rescale_transform_for_frame(frame_index)

dicom_file.window_transform
dicom_file.window_transform_for_frame(frame_index)

dicom_file.voi_lut
dicom_file.voi_lut_for_frame(frame_index)

dicom_file.modality_lut
dicom_file.modality_lut_for_frame(frame_index)

dicom_file.pixel_presentation
dicom_file.palette_lut
dicom_file.supplemental_palette
dicom_file.enhanced_palette
```

`DicomFile.to_pil_image(frame=...)` uses the requested frame when resolving window or VOI LUT
display metadata. It also:

- applies the classic root-level palette when `Photometric Interpretation = PALETTE COLOR`
- applies the root-level Supplemental Palette display mapping when present
- renders a limited Enhanced Palette subset:
  - one `DataFrameAssignmentSequence` item
  - `DataPathAssignment = PRIMARY_SINGLE`
- one `EnhancedPaletteColorLookupTableSequence` item with `DataPathID = PRIMARY`
- no blending LUT stages
- `IDENTITY` RGB/alpha LUT transfer functions
- no modality or VOI transform stages in the same display path
- still rejects more complex Enhanced Palette blending/display pipelines

For independent Python viewers and rendering tools, the intended building blocks are:

- `to_array(frame=...)` for the decoded source pixels
- `rescale_transform_for_frame(frame)`, `window_transform_for_frame(frame)`,
  `voi_lut_for_frame(frame)` for frame-aware grayscale display metadata
- `palette_lut`, `supplemental_palette`, `enhanced_palette` for palette metadata
- `apply_rescale(...)`, `apply_window(...)`, `apply_voi_lut(...)`, `apply_palette_lut(...)`
  for explicit CPU-side transform stages when the caller wants them

`to_pil_image()` remains a convenience helper for Pillow users. More complex viewers are
expected to combine decoded source pixels and display metadata directly instead of depending on
a generic container-agnostic render API from the core library.

## Future full Enhanced Palette pipeline shape

The library is intentionally moving away from exposing a broad public display-render API in the
core surface.

Callers should not need to manually interpret or combine:

- `DataFrameAssignmentSequence (0028,1401)`
- `EnhancedPaletteColorLookupTableSequence (0028,140B)`
- blending LUT stages
- per-path modality / VOI / window stages

Instead, the preferred direction is:

- expose normalized metadata and reusable transform helpers from the core library
- keep `to_pil_image()` as a narrow convenience helper
- keep a full Enhanced Palette blending/display reference pipeline in a separate example or
  external viewer-oriented layer

That separation keeps GPU-oriented or container-specific viewers free to reuse:

- decoded source pixels
- palette metadata
- assignment metadata
- blending metadata

without being forced through one built-in CPU rendering path.
