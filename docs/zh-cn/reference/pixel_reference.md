# Pixel Reference

```{note}
本页正文目前仍为英文原文。需要时请以英文版为准。
```

DicomSDL keeps pixel decode, pixel encode, and frame-aware pixel metadata on `DicomFile`.

## Decode surface

- Python:
  - `create_decode_plan(...)`
  - `decode_into(out, ...)`
  - `to_array(...)`
  - `to_array_view(...)`
  - `pixel_array(...)`
  - `pixel_data(...)`
- C++:
  - `create_decode_plan(...)`
  - `decode_into(...)`
  - `decode_all_frames_into(...)`
  - `pixel_buffer(...)`
  - `pixel_data(...)`
  - `native_pixel_layout()`

## Encode and transcode surface

- `set_pixel_data(...)`: replace pixel payload from normalized native source bytes plus metadata
- `set_transfer_syntax(...)`: mutate the in-memory file to a new transfer syntax
- `write_with_transfer_syntax(...)` in C++: stream a target transfer syntax without keeping the transcoded payload on the source object
- low-level C++ helpers: `set_native_pixel_data(...)`, `reset_encapsulated_pixel_data(...)`, `set_encoded_pixel_frame(...)`, `add_encoded_pixel_frame(...)`

## Frame-aware metadata helpers

- `window_transform` / `window_transform_for_frame(...)`
- `rescale_transform` / `rescale_transform_for_frame(...)`
- `voi_lut` / `voi_lut_for_frame(...)`
- `modality_lut` / `modality_lut_for_frame(...)`
- `pixel_presentation`
- `palette_lut`, `supplemental_palette`, `enhanced_palette`

## Important notes

- Reuse decode plans only while pixel-affecting metadata stays unchanged.
- `decode_into(...)` is the best fit when you want to control allocation and reuse an existing output buffer.
- `to_array(...)` is the best fit when you want a new Python array returned directly.
- Writer-side codec limits, option mapping, and metadata side effects are documented separately.

## Related docs

- [Pixel Transform Metadata Resolution](pixel_transform_metadata.md)
- [Pixel Encode Constraints](pixel_encode_constraints.md)
- [Encode-capable Transfer Syntax Families](codec_support_matrix.md)
- [Pixel Decode](../guide/pixel_decode.md)
- [Pixel Encode](../guide/pixel_encode.md)
