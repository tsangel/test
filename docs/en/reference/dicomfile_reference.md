# DicomFile Reference

`DicomFile` owns the root `DataSet` plus file/session state, serialization, transfer syntax, charset, and pixel decode/encode entrypoints.

## Use `DicomFile` when

- you are reading from disk or memory into a file-level session
- you are writing bytes or files back out
- you are changing transfer syntax or specific character set for the whole root dataset
- you are decoding, transcoding, or replacing pixel data

## Python surface

- constructors and loaders: `read_file(...)`, `read_bytes(...)`
- root dataset access: `df.dataset`
- forwarded dataset operations: `add_dataelement`, `ensure_dataelement`, `ensure_loaded`, `remove_dataelement`, `get_dataelement`, `get_value`, `set_value`, `__getitem__`, `__contains__`, iteration
- file/session state: `path`, `transfer_syntax_uid`, `has_error`, `error_message`
- write operations: `write_file(...)`, `write_with_transfer_syntax(...)`, `write_bytes(...)`, `rebuild_file_meta()`
- transfer syntax and charset: `set_transfer_syntax(...)`, `set_declared_specific_charset(...)`, `set_specific_charset(...)`
- pixel decode: `create_decode_plan(...)`, `decode_into(...)`, `to_array(...)`, `to_array_view(...)`, `pixel_array(...)`, `pixel_data(...)`, `encoded_pixel_frame_view(...)`, `encoded_pixel_frame_bytes(...)`
- pixel metadata helpers: `window_transform`, `window_transform_for_frame(...)`, `rescale_transform`, `rescale_transform_for_frame(...)`, `voi_lut`, `voi_lut_for_frame(...)`, `modality_lut`, `modality_lut_for_frame(...)`, `pixel_presentation`, `palette_lut`, `supplemental_palette`, `enhanced_palette`
- pixel encode: `set_pixel_data(...)`, `reset_encapsulated_pixel_data(...)`, `set_encoded_pixel_frame(...)`, `add_encoded_pixel_frame(...)`

## C++ surface

- root dataset access: `dataset()`
- file/session state: `path()`, `stream()`, `size()`, `dump(...)`
- attach and read: `attach_to_file(...)`, `attach_to_memory(...)`, `read_attached_stream(...)`
- write operations: `write_to_stream(...)`, `write_bytes(...)`, `write_file(...)`, `write_with_transfer_syntax(...)`
- transfer syntax and charset: `set_transfer_syntax(...)`, `set_declared_specific_charset(...)`, `set_specific_charset(...)`
- forwarded dataset operations: `add_dataelement(...)`, `ensure_dataelement(...)`, `remove_dataelement(...)`, `get_dataelement(...)`, `get_value(...)`, `set_value(...)`, `operator[]`
- pixel decode: `create_decode_plan(...)`, `decode_into(...)`, `decode_all_frames_into(...)`, `pixel_buffer(...)`, `pixel_data(...)`, `encoded_pixel_frame_view(...)`, `encoded_pixel_frame_bytes(...)`
- pixel encode: `set_pixel_data(...)`, `set_native_pixel_data(...)`, `reset_encapsulated_pixel_data(...)`, `set_encoded_pixel_frame(...)`, `add_encoded_pixel_frame(...)`

## Stable behavior

- The forwarding APIs act on the root dataset. Use `df.dataset` or `dataset()` when you want that fact to be explicit.
- If you mutate pixel-affecting metadata or pixel payload, old decode plans and output assumptions are no longer reliable.
- `encoded_pixel_frame_view(...)` returns a borrowed view tied to `DicomFile`-owned pixel storage. Keep the `DicomFile` alive and stop using the view after replacing pixel bytes.
- `encoded_pixel_frame_bytes(...)` returns detached owned bytes.
- `set_encoded_pixel_frame(..., std::span<const uint8_t>)` and `add_encoded_pixel_frame(..., std::span<const uint8_t>)` copy from the caller buffer before `DicomFile` takes ownership. The `std::vector<uint8_t>&&` overloads are the move-based no-extra-copy path.
- For large write-only transcodes, prefer `write_with_transfer_syntax(...)` over `set_transfer_syntax(...)` followed by `write_file(...)`. That path now exists in both C++ and Python for file output; C++ also provides `std::ostream` variants.
- In Python, `has_error` and `error_message` are the file-level state you check after permissive reads that keep partial data.

## Related docs

- [DataSet Reference](dataset_reference.md)
- [Pixel Reference](pixel_reference.md)
- [Pixel Encode Constraints](pixel_encode_constraints.md)
- [Error Model](error_model.md)
