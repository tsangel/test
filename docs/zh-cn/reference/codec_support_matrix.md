# Encode-capable Transfer Syntax Families

```{note}
本页正文目前仍为英文原文。需要时请以英文版为准。
```

This page is a compact summary of the transfer-syntax families DicomSDL can target
on the writer side. The listed families are the encode/transcode targets you can
use with `set_pixel_data(...)`, `set_transfer_syntax(...)`, and C++
`write_with_transfer_syntax(...)`.

The interesting API differences are not "which family is supported", but how the
API gets pixel input, whether it mutates the source object, and whether it can
stream output directly. Those differences are documented elsewhere.

Decode support is broader and backend-dependent, so it is documented separately.

| Family | Representative transfer syntaxes | Notes |
| --- | --- | --- |
| Native uncompressed | `ImplicitVRLittleEndian`, `ExplicitVRLittleEndian`, `ExplicitVRBigEndian` | No codec step; metadata is normalized to native pixel layout |
| Encapsulated uncompressed | `EncapsulatedUncompressedExplicitVRLittleEndian` | One encapsulated payload per frame |
| RLE | `RLELossless` | Segment count and encoded-size limits apply |
| JPEG | `JPEGBaseline8Bit`, `JPEGExtended12Bit`, `JPEGLossless*` | Lossy JPEG paths have bit-depth limits |
| JPEG-LS | `JPEGLSLossless`, `JPEGLSNearLossless` | `near_lossless_error` rules depend on target syntax |
| JPEG 2000 | `JPEG2000Lossless`, `JPEG2000`, `JPEG2000MC*` | MCT and option rules apply |
| HTJ2K | `HTJ2KLossless`, `HTJ2KLosslessRPCL`, `HTJ2K` | Input dtype and lossy/lossless limits differ |
| JPEG-XL | `JPEGXLLossless`, `JPEGXL` | `JPEGXLJPEGRecompression` is not an encode target |
| Other transfer syntaxes | everything else | Not encode-capable targets; use a supported family instead |

## Helpful APIs

- Python: `transfer_syntax_uids_encode_supported()`
- C++: `uid::WellKnown::supports_pixel_encode()`

## Related docs

- [Pixel Encode Constraints](pixel_encode_constraints.md)
- [Pixel Reference](pixel_reference.md)
- [Pixel Encode](../guide/pixel_encode.md)
