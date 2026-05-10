# Split Pixel Payload

Split Pixel Payload is a DicomSDL private runtime convention for keeping DICOM
metadata separate from large PixelData value bytes. It is intended for volume
viewers, series packers, encryption pipelines, and cache layers where metadata
must stay available while pixel bytes are only needed during decode.

This is not a standard DICOM interchange format. Use `write_bytes()` or
`write_with_transfer_syntax()` when you need a normal P10 file for external
tools.

## Model

A split instance has two byte buffers.

- `main_bytes`: Part 10 bytes where the root `(7FE0,0010) PixelData` element is
  replaced by a DicomSDL placeholder.
- `pixel_payload`: PixelData value bytes only.

`pixel_payload` never includes the `(7FE0,0010)` tag or VR/VL header. Native
PixelData is the raw value bytes. Encapsulated PixelData starts at the Basic
Offset Table item and includes fragment items plus the sequence delimiter.

The placeholder value is fixed at 22 bytes.

```text
bytes[0:8]   = "PIXDATA1"
bytes[8:10]  = original PixelData VR text: "OB", "OW", or "UN"
bytes[10:14] = original PixelData VL as uint32 little-endian
bytes[14:22] = actual pixel_payload byte length as uint64 little-endian
```

Explicit VR main P10 stores the placeholder element as `(7FE0,0010) OB VL=22`.
Implicit VR main P10 stores it as implicit VL=22.

## Byte-Preserving Split

`split_pixeldata_payload()` does not reserialize the dataset. It uses the
existing selected/read-until parser flow to locate PixelData and collect the
metadata needed for decode, then splits the original byte stream.

```python
import dicomsdl as dicom

split = dicom.split_pixeldata_payload([], "image.dcm")
if not split.ok:
    raise RuntimeError(split.error_message)

main_bytes = split.main_bytes
pixel_payload = split.pixel_payload
desc = split.decode_descriptor
decoder = dicom.PixelPayloadDecoder(desc, pixel_payload)
frame0 = decoder.to_array(frame=0)

original_p10 = dicom.join_pixeldata_payload(main_bytes, pixel_payload)
```

`join_pixeldata_payload(main_bytes, pixel_payload)` scans backward for the
`PIXDATA1` placeholder, validates the stored metadata and payload length, and
rebuilds the original PixelData element.

v1 supports only root `(7FE0,0010) PixelData`. `DeflatedExplicitVRLittleEndian`,
`ExplicitVRBigEndian`, missing PixelData, and malformed PixelData return empty
buffers with `result.ok == False`.

## Transcode Then Split

When transcoding is needed, first write a normal in-memory P10, then split that
byte stream.

```python
source = dicom.read_file("image.dcm")
transcoded = source.write_bytes_with_transfer_syntax("RLELossless")

split = dicom.split_pixeldata_payload(
    [], transcoded, name="image-rle"
)
```

The C++ flow is the same.

```cpp
using namespace dicom::literals;

auto source = dicom::read_file("image.dcm");
auto transcoded = source->write_bytes_with_transfer_syntax("RLELossless"_uid);
auto split = dicom::split_pixeldata_payload(
    dicom::DataSetSelection{}, "image-rle",
    std::span<const std::uint8_t>(transcoded.data(), transcoded.size()));
```

## Reading Split Buffers

`read_bytes_with_pixeldata_payload()` borrows a value-only `pixel_payload` and
attaches it at the main P10 placeholder.

```python
df = dicom.read_bytes_with_pixeldata_payload(
    main_bytes,
    pixel_payload,
    name="image-split",
    copy=False,
)

frame0 = df.pixel_data(0)
df.detach_pixeldata_payload()
```

With `copy=False`, Python keeps owner references for the input buffers while the
returned `DicomFile` is attached. In C++, the caller owns the memory and must
keep it alive until `detach_pixeldata_payload()` or destruction.

`detach_pixeldata_payload()` leaves only a lightweight `PIXDATA1` marker by default.
Pass `keep_dump=True` in Python or `true` in C++ to retain PixelData dump text in
the marker for diagnostics.

## Payload-Only Decode

When a storage layer already has decode metadata in a compact record, use
`PixelPayloadDecoder` without reattaching a `DicomFile`.

```python
split = dicom.split_pixeldata_payload([], "image.dcm")
desc = split.decode_descriptor
pixel_payload = split.pixel_payload

decoder = dicom.PixelPayloadDecoder(desc, pixel_payload)
plan = decoder.create_decode_plan()
frame0 = decoder.to_array(frame=0, plan=plan)
```

For encapsulated payloads, `PixelPayloadDecodeDescriptor.frame_fragments` is
required. Its format is `offset:length,offset:length;offset:length`; offsets and
lengths are relative to the start of `pixel_payload` and refer to fragment value
bytes, excluding the 8-byte item header. Native payloads use an empty string.

## API Summary

Python:

- `dicomsdl.split_pixeldata_payload(selection, source, name=...) -> SplitPixelDataPayloadResult`
- `dicomsdl.join_pixeldata_payload(main_bytes, pixel_payload) -> bytes`
- `dicomsdl.read_bytes_with_pixeldata_payload(main, payload, name=..., copy=...)`
- `DicomFile.write_bytes_with_transfer_syntax(...) -> bytes`
- `PixelPayloadDecoder(descriptor, pixel_payload)`
- `DicomFile.detach_pixeldata_payload(keep_dump=False)`
- `DicomFile.has_attached_pixeldata_payload`

C++:

- `split_pixeldata_payload(...)`
- `join_pixeldata_payload(...)`
- `read_bytes_with_pixeldata_payload(...)`
- `DicomFile::write_bytes_with_transfer_syntax(...)`
- `DicomFile::attach_to_memory_with_pixeldata_payload(...)`
- `pixel::PixelPayloadDecoder`
- `DicomFile::detach_pixeldata_payload(bool keep_dump = false)`
