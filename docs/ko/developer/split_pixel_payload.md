# Split Pixel Payload

Split Pixel Payload는 DICOM metadata와 큰 PixelData value bytes를 별도
buffer로 나누는 DicomSDL private runtime convention입니다. Volume viewer,
series packer, encryption pipeline처럼 metadata는 오래 유지하지만 pixel bytes는
decode 시점에만 필요한 곳을 위한 기능입니다.

이 형식은 DICOM 표준 저장 형식이 아닙니다. 외부 도구와 교환할 표준 P10이
필요하면 `write_bytes()` 또는 `write_with_transfer_syntax()`를 사용하세요.

## 모델

split 결과는 항상 두 byte buffer입니다.

- `main_bytes`: 원본 Part 10 stream에서 root `(7FE0,0010) PixelData` element를
  DicomSDL placeholder로 교체한 P10 bytes
- `pixel_payload`: PixelData element의 value bytes only

`pixel_payload`에는 `(7FE0,0010)` tag, VR, VL header가 들어가지 않습니다.
Native PixelData는 raw value bytes이고, encapsulated PixelData는 Basic Offset
Table item부터 fragment items와 sequence delimiter까지 포함한 value field입니다.

placeholder value는 22 bytes 고정입니다.

```text
bytes[0:8]   = "PIXDATA1"
bytes[8:10]  = original PixelData VR text: "OB", "OW", or "UN"
bytes[10:14] = original PixelData VL as uint32 little-endian
bytes[14:22] = actual pixel_payload byte length as uint64 little-endian
```

Explicit VR main P10에서는 placeholder element가 `(7FE0,0010) OB VL=22`로
저장됩니다. Implicit VR main P10에서는 implicit VL=22로 저장됩니다.

## Byte-Preserving Split

`split_pixeldata_payload()`는 DicomSDL writer로 dataset을 다시 serialize하지
않습니다. 기존 parser의 selected/read-until 흐름으로 PixelData 위치와 decode에
필요한 metadata만 읽고, 원본 byte stream에서 PixelData value만 분리합니다.

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

`join_pixeldata_payload(main_bytes, pixel_payload)`는 `main_bytes` 뒤쪽부터
`PIXDATA1` placeholder를 찾아 metadata와 `pixel_payload` 길이를 검증한 뒤,
원래 PixelData element를 재구성합니다.

v1은 root `(7FE0,0010) PixelData`만 지원합니다. `DeflatedExplicitVRLittleEndian`,
`ExplicitVRBigEndian`, PixelData 없음, malformed PixelData는 throw하지 않고
`main_bytes == b""`, `pixel_payload == b""`, `result.ok == False`로 반환합니다.

## Transcoding 후 Split

transcoding이 필요하면 먼저 일반 writer로 memory P10을 만든 뒤 그 결과를 split합니다.

```python
source = dicom.read_file("image.dcm")
transcoded = source.write_bytes_with_transfer_syntax("RLELossless")

split = dicom.split_pixeldata_payload(
    [], transcoded, name="image-rle"
)
```

C++에서도 같은 흐름을 사용합니다.

```cpp
using namespace dicom::literals;

auto source = dicom::read_file("image.dcm");
auto transcoded = source->write_bytes_with_transfer_syntax("RLELossless"_uid);
auto split = dicom::split_pixeldata_payload(
    dicom::DataSetSelection{}, "image-rle",
    std::span<const std::uint8_t>(transcoded.data(), transcoded.size()));
```

## Split Buffer 읽기

`read_bytes_with_pixeldata_payload()`는 value-only `pixel_payload`를 borrow해서
main P10 placeholder 위치에 attach합니다.

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

`copy=False`일 때 Python binding은 returned `DicomFile`이 살아 있는 동안
input owner reference를 붙잡습니다. C++에서는 caller가 payload memory lifetime을
직접 보장해야 하며, payload를 해제하기 전에 `detach_pixeldata_payload()`를 호출해야
합니다.

`detach_pixeldata_payload()`는 기본적으로 가벼운 `PIXDATA1` marker만 남깁니다.
`detach_pixeldata_payload(true)` 또는 Python `detach_pixeldata_payload(keep_dump=True)`를
쓰면 detach 직전 PixelData dump text를 marker 뒤에 보존합니다.

## Payload-Only Decode

seriespack처럼 decode metadata를 별도 record로 관리하는 경우에는 DicomFile을 다시
attach하지 않고 `PixelPayloadDecoder`를 사용할 수 있습니다.

```python
split = dicom.split_pixeldata_payload([], "image.dcm")
desc = split.decode_descriptor
pixel_payload = split.pixel_payload

decoder = dicom.PixelPayloadDecoder(desc, pixel_payload)
plan = decoder.create_decode_plan()
frame0 = decoder.to_array(frame=0, plan=plan)
```

`PixelPayloadDecodeDescriptor.frame_fragments`는 encapsulated payload에서 필수입니다.
형식은 `offset:length,offset:length;offset:length`이며 offset/length는
`pixel_payload` buffer 시작 기준 fragment value bytes입니다. fragment item header
8 bytes는 포함하지 않습니다. Native payload에서는 빈 문자열입니다.

## API 요약

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
