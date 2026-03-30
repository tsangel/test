# Pixel Encode Constraints

This document summarizes the current constraints for the pixel encode path:

- `DicomFile::set_pixel_data(...)`
- `DicomFile::set_transfer_syntax(...)` when it triggers native -> encapsulated encoding

## Layer/Code Map Pointers

- Entry point:
  - `src/pixel/host/entrypoint/encode_entrypoint.cpp`
- Host encode orchestration:
  - `src/pixel/host/encode/*`
- Runtime dispatch and registry:
  - `src/pixel/host/adapter/host_adapter_v2.cpp`
  - `src/pixel/runtime/runtime_registry_v2.cpp`
  - `src/pixel/runtime/plugin_registry_v2.cpp`
- Codec implementations:
  - `src/pixel/codecs/*_v2/encode.cpp`
  - `src/pixel/codecs/rle/*`
- ABI contract:
  - `src/pixel/abi/pixel_encoder_plugin_abi_v2.h`
  - `src/pixel/abi/pixel_codec_plugin_abi_v2.h`

Naming conventions aligned with the refactor:

- resolver layer: `resolve_*`
- runner layer: `run_*`
- dispatch layer: `dispatch_*`
- codec implementation: `encoder_encode_frame`

## Plugin Boundary Contract (Encoder Migration)

- Plugin encode entrypoints operate on frame bytes + primitive metadata + codec options.
- Plugin boundary does not take `DicomFile`, `DataSet`, or `DataElement`.
- Plugin frame encode handlers are `noexcept` and report failures via `codec_error`:
  - `code`: `invalid_argument`, `unsupported`, `backend_error`, `internal_error`
  - `stage`: typically `parse_options`, `validate`, `encode`, `allocate`
  - `detail`: codec/backend-specific detail text
- Final exception creation is centralized in common orchestration with context:
  `file`, `transfer syntax`, and optional `frame index`.

## Encode-capable Transfer Syntax UIDs

`uid::WellKnown::supports_pixel_encode()` is currently true for:

- Uncompressed (native): `ImplicitVRLittleEndian`, `ExplicitVRLittleEndian`, `DeflatedExplicitVRLittleEndian`, `ExplicitVRBigEndian`, `Papyrus3ImplicitVRLittleEndian`
- Uncompressed (encapsulated): `EncapsulatedUncompressedExplicitVRLittleEndian`
- RLE: `RLELossless`
- JPEG: `JPEGBaseline8Bit`, `JPEGExtended12Bit`, `JPEGLossless`, `JPEGLosslessNonHierarchical15`, `JPEGLosslessSV1`
- JPEG-LS: `JPEGLSLossless`, `JPEGLSNearLossless`
- JPEG 2000: `JPEG2000Lossless`, `JPEG2000MCLossless`, `JPEG2000`, `JPEG2000MC`
- HTJ2K: `HTJ2KLossless`, `HTJ2KLosslessRPCL`, `HTJ2K`
- JPEG-XL: `JPEGXLLossless`, `JPEGXL`

Anything else is rejected by `set_pixel_data`.

## Input Contract (`pixel::PixelSource`)

- `data_type` must be one of: `u8`, `s8`, `u16`, `s16`, `u32`, `s32`
- `rows`, `cols`, `frames`, `samples_per_pixel` must be positive
- `rows` and `cols` must be `<= 65535`
- `row_stride` and `frame_stride`:
  - `0` means "use tightly packed default"
  - if explicitly provided, each must be large enough for the payload it describes
- `bytes` length must be large enough for all addressed frames/planes/rows
- `bits_stored`:
  - if `0`, it defaults to full storage width of `data_type`
  - otherwise must satisfy `1 <= bits_stored <= bits_allocated`

## Codec Option Mapping

`codec_opt` must match the selected transfer syntax family:

- Native uncompressed -> `NoCompression`
- Encapsulated uncompressed -> `NoCompression`
- RLE -> `RleOptions`
- JPEG -> `JpegOptions`
- JPEG-LS -> `JpegLsOptions`
- JPEG 2000 -> `J2kOptions`
- HTJ2K -> `Htj2kOptions`
- JPEG-XL -> `JpegXlOptions`

`AutoCodecOptions` resolves as:

- JPEG 2000 lossy / HTJ2K lossy: `target_psnr = 45.0`
- JPEG-LS near-lossless: `near_lossless_error = 2`
- JPEG-XL lossless: `distance = 0.0`
- JPEG-XL lossy: `distance = 1.0`
- Otherwise: codec-family defaults

Thread hints are validated for JPEG 2000 / HTJ2K / JPEG-XL options:

- `threads` must be `-1`, `0`, or a positive integer

## Common Bit-Depth Rules

For JPEG/JPEG-LS/JPEG2000/HTJ2K/JPEG-XL encoder paths:

- `bits_allocated <= 16`

Additional JPEG lossy rule:

- `bits_stored <= 12`

## Per-codec Constraints

### Encapsulated Uncompressed

- Each frame is written as one encapsulated item payload.
- Frame payload is tightly packed native little-endian bytes (row/frame padding from input strides is stripped).
- Layout follows metadata:
  - `PlanarConfiguration=0`: interleaved frame bytes
  - `PlanarConfiguration=1`: planar frame bytes

### RLE

- Segment layout must fit DICOM RLE limits:
  - `bytes_per_sample` in `[1, 15]`
  - `samples_per_pixel <= 15 / bytes_per_sample`
- Encoded frame size and segment offsets must fit 32-bit unsigned range.

### JPEG (libjpeg-turbo)

- `samples_per_pixel` must be `1`, `3`, or `4`
- `bits_allocated` must be `8` or `16`
- `bytes_per_sample` must match precision:
  - `bits_stored <= 8` -> 1 byte
  - `bits_stored > 8` -> 2 bytes
- `quality` must be in `[1, 100]` (`dicomconv -h` and registry schema).
- Encoder path also clamps to `[1, 100]` as a defensive guard for direct/internal calls.

### JPEG-LS (CharLS)

- `samples_per_pixel` must be `1`, `3`, or `4`
- `bits_allocated` must be `8` or `16`
- `near_lossless_error` must be in `[0, 255]`
- Transfer syntax specific:
  - `JPEGLSLossless` requires `near_lossless_error = 0`
  - `JPEGLSNearLossless` requires `near_lossless_error > 0`

### JPEG 2000 (OpenJPEG)

- `samples_per_pixel` must be `1`, `3`, or `4`
- Multicomponent transform (MCT) requires `samples_per_pixel = 3`
- `bits_allocated <= 16`
- `pixel_representation` must be `0` or `1`
- Lossy mode requires either `target_psnr > 0` or `target_bpp > 0`
- Encoder threads:
  - `threads = -1` -> auto (all CPUs)
  - `threads = 0` -> library default
  - `threads > 0` -> explicit

### HTJ2K (OpenJPH encoder path)

- `samples_per_pixel` must be `1`, `3`, or `4`
- Multicomponent transform (MCT) requires `samples_per_pixel = 3`
- Lossless path:
  - integral `8/16-bit` inputs are supported
  - signed `32-bit` inputs are also supported
  - `uint32` / full-range unsigned `32-bit` is not supported
- Lossy path:
  - only integral `8/16-bit` inputs are supported
- `pixel_representation` must be `0` or `1`
- `target_psnr` / `target_bpp` must be `>= 0`
- Lossy qstep is derived from the requested options or the built-in defaults
- `threads` is validated at API level; current OpenJPH encode path does not consume it yet

### JPEG-XL (libjxl)

- `samples_per_pixel` must be `1`, `3`, or `4`
- `bits_allocated <= 16`
- `distance` must be in `[0, 25]`
- `effort` must be in `[1, 10]`
- Transfer syntax specific:
  - `JPEGXLLossless` requires `distance = 0`
  - `JPEGXL` requires `distance > 0`
  - `JPEGXLJPEGRecompression` is decode-only (encode not supported)

## MCT/Color Transform Rules and Photometric Update

- `J2kOptions.use_color_transform` and `Htj2kOptions.use_color_transform` default to `true`.
- JPEG2000 MC transfer syntaxes (`JPEG2000MCLossless`, `JPEG2000MC`) enforce:
  - color transform must be enabled
  - `samples_per_pixel = 3`
- For non-MC JPEG2000/HTJ2K syntaxes, MCT is applied only when:
  - `use_color_transform == true`
  - `samples_per_pixel == 3`

When MCT is used, `PhotometricInterpretation` is updated to:

- lossless JPEG2000/HTJ2K -> `YBR_RCT`
- lossy JPEG2000/HTJ2K -> `YBR_ICT`

When MCT is not used, source photometric is preserved.

## Metadata Side Effects of `set_pixel_data`

The encode path rewrites or updates:

- `Rows`, `Columns`, `SamplesPerPixel`
- `BitsAllocated`, `BitsStored`, `HighBit`, `PixelRepresentation`
- `PhotometricInterpretation`
- `PlanarConfiguration` (removed when `SamplesPerPixel == 1`)
- `NumberOfFrames` (removed when `frames == 1`)
- `LossyImageCompression (0028,2110)`
  - set to `01` for lossy encodes
  - set to `00` for non-lossy encodes when no prior lossy history exists
  - once already `01`, non-lossy re-encode keeps `01` (no reset)
- `LossyImageCompressionRatio (0028,2112)` and `LossyImageCompressionMethod (0028,2114)`
  - written for lossy encodes
  - appended for repeated lossy re-encodes (stage history)
  - removed when a non-lossy encode has no prior lossy history
- File meta `(0002,0010) TransferSyntaxUID`

For RLE targets, written `PlanarConfiguration` is forced to `1`.

## `set_transfer_syntax` Transcode Limits

Current behavior:

- native -> encapsulated: supported only when target transfer syntax is encode-capable
- encapsulated -> native: supported (decode to native)
- encapsulated -> encapsulated: supported when target transfer syntax is encode-capable
- `set_transfer_syntax(...)` mutates the in-memory `DicomFile` and retains the target
  PixelData on the object
- for large pixel payloads whose end goal is writing a file/stream, prefer
  `write_with_transfer_syntax(...)` over `set_transfer_syntax(...) + write_file(...)`;
  it can reduce peak memory use by avoiding an in-memory transcode path that keeps
  decode/re-encode working buffers and the target `PixelData` alive on the object
  longer than needed

`encapsulated -> encapsulated` is implemented as decode-to-native + re-encode in a single
`set_transfer_syntax(...)` call (not codestream pass-through).

## `write_with_transfer_syntax` Streaming Write Caveats

Current behavior:

- seekable output (`std::ofstream`, `std::stringstream`, file-path variant):
  - encapsulated streaming write can backpatch `ExtendedOffsetTable` /
    `ExtendedOffsetTableLengths`
  - lossy encapsulated targets can backpatch `LossyImageCompressionRatio`
  - seekable lossy encapsulated writes are therefore one-pass at the encode stage
- non-seekable output (`stdout`, pipes, sockets, forwarding streambufs):
  - encapsulated streaming write stays valid DICOM
  - output uses an empty Basic Offset Table and omits `ExtendedOffsetTable` attributes
  - lossy encapsulated targets still require a prepass encode to compute
    `LossyImageCompressionRatio`, so those writes remain two-pass

Benchmark/stress coverage:

- `benchmarks/streaming_write_stress.cpp`
  - build with `-DDICOM_BUILD_BENCHMARKS=ON`
  - the default benchmark setup generates a synthetic large multi-frame multi-fragment
    encapsulated-uncompressed source (`--fragments-per-frame 4`)
  - multi-frame single-fragment runs use `ExtendedOffsetTable`; multi-frame
    multi-fragment runs use a populated Basic Offset Table
  - repeatedly calls `write_with_transfer_syntax(...)`
  - reports elapsed time, sampled RSS (1 ms polling), and whether the write path
    loaded any source frame caches
  - `seekable_memory` intentionally retains the full output bytes in RAM, so
    compare its RSS separately from `file` / `nonseekable_null`
