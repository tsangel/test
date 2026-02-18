# TODO

- Pixel decoder: implement YBR→RGB conversion (currently declined in `RawPixelDecoder::decode_into`, src/pixel_codec.cpp).
- Pixel decoder: enforce photometric-aware Rescale/LUT policy. Reference table for supported combinations:

| SamplesPerPixel | bytes_per_sample | PhotometricInterpretation | Rescale/LUT apply? | Priority |
| --- | --- | --- | --- | --- |
| 1 | 1 | MONOCHROME1/2 | apply | Must |
| 1 | 2 | MONOCHROME1/2 | apply | Must |
| 1 | 1 | PALETTE COLOR | no (palette LUT only) | Must |
| 1 | 2 | PALETTE COLOR (16-bit) | no | Should |
| 3 | 1 | RGB / YBR_FULL / YBR_PARTIAL / YBR_ICT / YBR_RCT | no | Must |
| 3 | 2 | RGB / YBR_RCT (16-bit) | no | Should |
| 4 | 1 | ARGB / CMYK | no | Should |
| 4 | 2 | ARGB / CMYK (16-bit) | no | Nice |
| 1 | 4 (float32) | MONO (Parametric Map, etc.) | generally no* | Should |
| 3/4 | 4 (float32) | Color float (rare) | no | Nice |

*float MONO datasets often already store real-world values; decide later if an opt-in rescale is needed.