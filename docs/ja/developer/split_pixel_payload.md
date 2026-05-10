# Split Pixel Payload

Split Pixel Payload is a DicomSDL private runtime convention. It stores metadata
and PixelData value bytes in separate buffers for viewers, series packers,
encryption pipelines, and cache layers.

This is not a standard DICOM interchange format. Use `write_bytes()` or
`write_with_transfer_syntax()` for normal P10 output.

## Model

- `main_bytes`: Part 10 bytes with root `(7FE0,0010) PixelData` replaced by a
  DicomSDL placeholder.
- `pixel_payload`: PixelData value bytes only. It does not include tag, VR, or
  VL bytes.

The placeholder value is fixed at 22 bytes:

```text
bytes[0:8]   = "PIXDATA1"
bytes[8:10]  = original PixelData VR text: "OB", "OW", or "UN"
bytes[10:14] = original PixelData VL as uint32 little-endian
bytes[14:22] = actual pixel_payload byte length as uint64 little-endian
```

Native payloads contain raw PixelData value bytes. Encapsulated payloads contain
the Basic Offset Table item, fragment items, and sequence delimiter.

## Byte-Preserving Split

`split_pixeldata_payload()` does not reserialize the dataset. It locates root
PixelData with the existing selected/read-until parser flow and replaces only
that element in the returned main bytes.

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

v1 supports only root `(7FE0,0010) PixelData`. Deflated, Explicit Big Endian,
missing PixelData, and malformed PixelData return empty buffers with
`result.ok == False`.

## Transcode Then Split

When transcoding is needed, write a normal in-memory P10 first, then split it.

```python
source = dicom.read_file("image.dcm")
transcoded = source.write_bytes_with_transfer_syntax("RLELossless")
split = dicom.split_pixeldata_payload(
    [], transcoded, name="image-rle"
)
```

## Reading And Decoding

`read_bytes_with_pixeldata_payload()` borrows a value-only payload and attaches it to
the main P10 placeholder. Call `detach_pixeldata_payload()` before releasing
caller-owned payload memory.

`PixelPayloadDecoder` can decode directly from `pixel_payload` plus
`PixelPayloadDecodeDescriptor`. Encapsulated descriptors use
`frame_fragments` in `offset:length,offset:length;offset:length` format, with
offsets relative to the start of `pixel_payload` and excluding item headers.

## API Summary

- `split_pixeldata_payload(...)`
- `join_pixeldata_payload(...)`
- `read_bytes_with_pixeldata_payload(...)`
- `DicomFile.write_bytes_with_transfer_syntax(...)`
- `PixelPayloadDecoder`
- `DicomFile.detach_pixeldata_payload(...)`
