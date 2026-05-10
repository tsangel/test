# Split Pixel Payload

Split Pixel Payload는 DICOM metadata와 큰 Pixel Data bytes를 별도 buffer로
분리해 다루기 위한 DicomSDL runtime convention입니다. Volume viewer,
series packer, encryption pipeline, cache layer처럼 metadata는 오래 유지해야
하지만 compressed/native pixel bytes는 decode 시점에만 필요한 경우에 유용합니다.

이 기능은 DICOM 표준 교환 형식이 아닙니다. DicomSDL을 사용하는 내부 저장소나
runtime pipeline을 위한 private convention입니다. 표준 DICOM 파일이 필요하면
`write_bytes()` 또는 `write_with_transfer_syntax()`로 일반 DICOM을 쓰세요.

## 모델

Split instance는 두 byte buffer로 구성됩니다.

- **main P10 DICOM**
  - 일반 Part 10 DICOM byte stream이지만, root `(7FE0,0010) PixelData`에는
    fixed 4-byte placeholder만 들어갑니다.
  - placeholder value는 정확히 `DXP1`입니다.
  - placeholder는 `OB`로 저장됩니다.
- **PixelData payload**
  - main DICOM에서 분리된 실제 PixelData value bytes입니다.
  - native/uncompressed transfer syntax에서는 raw native PixelData value입니다.
  - encapsulated/compressed transfer syntax에서는 Basic Offset Table item,
    fragment items, sequence delimiter를 포함한 전체 encapsulated PixelData
    value field입니다.

두 buffer를 attach하면 metadata는 main P10 DICOM에서 읽고, Pixel Data는 payload
buffer에서 읽습니다.

## 언제 쓰나

다음 상황에 적합합니다.

- volume viewer가 viewer lifetime 동안 DICOM metadata를 계속 참조해야 할 때
- 일부 frame decode 후 encoded Pixel Data를 해제하고 싶을 때
- series pack 형식에서 metadata와 payload를 별도 record로 저장할 때
- main DICOM과 pixel payload를 따로 암호화/압축해야 할 때
- 큰 pixel bytes 없이 metadata를 유지하려고 DICOM JSON을 쓰기에는 부담이 클 때

다음 상황에는 쓰지 마세요.

- 출력물이 표준 DICOM 파일로 외부 도구와 교환되어야 할 때
- downstream이 DicomSDL의 `DXP1` convention을 모를 때
- `FloatPixelData`, `DoubleFloatPixelData` 분리가 필요할 때

v1은 root `(7FE0,0010) PixelData`만 지원합니다.

## Split Buffer 쓰기

### Python: transcoding 없이 분리

```python
from pathlib import Path
import dicomsdl as dicom

source = dicom.read_file("image.dcm")

main_bytes, pixel_payload = source.write_bytes_split_pixel_payload()

Path("image.main.dcm").write_bytes(main_bytes)
Path("image.pixel_payload.bin").write_bytes(pixel_payload)
```

`main_bytes`에는 `DXP1` placeholder가 들어갑니다. `pixel_payload`에는 분리된
PixelData value bytes가 들어갑니다.

### Python: target transfer syntax로 쓰면서 분리

```python
from pathlib import Path
import dicomsdl as dicom

source = dicom.read_file("image.dcm")

main_bytes, pixel_payload = source.write_with_transfer_syntax_split_pixel_payload(
    "RLELossless"
)

Path("image-rle.main.dcm").write_bytes(main_bytes)
Path("image-rle.pixel_payload.bin").write_bytes(pixel_payload)
```

저장 전 transcode가 필요할 수 있는 pipeline에서는
`write_with_transfer_syntax_split_pixel_payload()`를 사용하세요.

### C++: transcoding 없이 분리

```cpp
#include <cstdint>
#include <dicom.h>
#include <fstream>
#include <vector>

void write_file(const char* path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
}

auto source = dicom::read_file("image.dcm");
auto split = source->write_bytes_split_pixel_payload();

write_file("image.main.dcm", split.dicom_bytes);
write_file("image.pixel_payload.bin", split.pixel_payload_bytes);
```

### C++: target transfer syntax로 쓰면서 분리

```cpp
#include <dicom.h>

using namespace dicom::literals;

auto source = dicom::read_file("image.dcm");
auto split = source->write_with_transfer_syntax_split_pixel_payload(
    "RLELossless"_uid);
```

## Split Buffer 읽기

PixelData payload memory는 DicomSDL이 borrow합니다. `detach_pixel_payload()`를
호출하거나 `DicomFile`이 파괴될 때까지 payload memory가 살아 있어야 합니다.

### Python

```python
from pathlib import Path
import dicomsdl as dicom

main_bytes = bytearray(Path("image.main.dcm").read_bytes())
pixel_payload = bytearray(Path("image.pixel_payload.bin").read_bytes())

df = dicom.read_bytes_with_pixel_payload(
    main_bytes,
    pixel_payload,
    name="image-split",
    copy=False,
)

print(df.Rows, df.Columns)
frame0 = df.pixel_data(0)

# payload memory를 해제하거나 재사용하기 전에 DicomSDL reference를 끊습니다.
df.detach_pixel_payload()
pixel_payload.clear()
```

`copy=False`에서는 Python binding이 `DicomFile`이 attach되어 있는 동안
`main_bytes`와 `pixel_payload` owner reference를 유지합니다.
`detach_pixel_payload()`를 호출하면 payload owner가 제거됩니다.

### C++

```cpp
#include <cstdint>
#include <dicom.h>
#include <vector>

std::vector<std::uint8_t> main_bytes = read_all_bytes("image.main.dcm");
std::vector<std::uint8_t> pixel_payload =
    read_all_bytes("image.pixel_payload.bin");

auto file = dicom::read_bytes_with_pixel_payload(
    "image-split",
    main_bytes.data(), main_bytes.size(),
    pixel_payload.data(), pixel_payload.size());

auto frame0 = file->pixel_data(0);

file->detach_pixel_payload();
pixel_payload.clear();
pixel_payload.shrink_to_fit();
```

C++에서는 caller가 두 vector를 소유합니다. `DicomFile`에 payload가 attach된
상태에서 payload vector를 해제하거나 reallocate하지 마세요.

## DicomFile 재attach 없이 Payload Decode하기

일부 저장 layer는 TSV 같은 index record에 decode에 필요한 작은 metadata를 이미
보관합니다. 이런 경우 한 frame을 decode하려고 `DicomFile`이나 DICOM JSON
객체를 다시 조립하는 것은 불필요한 비용이 될 수 있습니다.
`PixelPayloadDecoder`는 분리된 PixelData payload와 compact descriptor만으로
바로 decode합니다.

descriptor는 일반 `DicomFile`이나 reattached `DicomFile`에서 만들 수 있습니다.

```python
source = dicom.read_file("image.dcm")
main_bytes, pixel_payload = source.write_bytes_split_pixel_payload()

desc = source.pixel_payload_decode_descriptor()
decoder = dicom.PixelPayloadDecoder(desc, pixel_payload)

plan = decoder.create_decode_plan()
frame0 = decoder.to_array(frame=0, plan=plan)
```

이미 split buffer를 읽은 뒤에도 같은 흐름을 사용할 수 있습니다.

```python
df = dicom.read_bytes_with_pixel_payload(main_bytes, pixel_payload)
desc = df.pixel_payload_decode_descriptor()

decoder = dicom.PixelPayloadDecoder(desc, pixel_payload)
out = bytearray(decoder.create_decode_plan().required_bytes(frame=0))
decoder.decode_into(out, frame=0)

df.detach_pixel_payload()
```

Python에서는 `PixelPayloadDecoder`가 `pixel_payload` object의 owner reference를
잡아 둡니다. 그래도 semantic은 borrow입니다. Decode 중 object 뒤의 foreign
memory를 mutate, resize, free하는 것은 안전하지 않습니다.

C++는 strict borrow-only semantics입니다.

```cpp
auto source = dicom::read_file("image.dcm");
auto split = source->write_bytes_split_pixel_payload();

auto desc = source->pixel_payload_decode_descriptor();
dicom::pixel::PixelPayloadDecoder decoder(
    desc,
    std::span<const std::uint8_t>(
        split.pixel_payload_bytes.data(),
        split.pixel_payload_bytes.size()));

auto plan = decoder.create_decode_plan();
auto frame0 = decoder.pixel_buffer(0, plan);
```

C++ decoder는 `pixel_payload`를 복사하지 않습니다. Decoder lifetime 전체와
실행 중인 모든 decode call 동안 backing memory를 유지해야 합니다.

### Descriptor Fields

`PixelPayloadDecodeDescriptor`에는 pixel decode path에 필요한 metadata만 들어갑니다.

- `transfer_syntax_uid`: transfer syntax UID text입니다. Decoder constructor에서
  parse합니다.
- `photometric`: DICOM `PhotometricInterpretation` text입니다. Decoder
  constructor에서 parse합니다.
- image geometry: `rows`, `cols`, `frames`, `samples_per_pixel`
- stored sample metadata: `bits_allocated`, `bits_stored`,
  `pixel_representation`, `planar_configuration`
- `expected_payload_length`: optional payload byte length check입니다. 0이 아니면
  입력 payload size와 같아야 합니다.
- `frame_fragments`: encapsulated frame/fragment index입니다. Native payload에서는
  비어 있습니다.
- `source_name`: decode error에 쓰이는 diagnostic name입니다.

descriptor의 string view는 `PixelPayloadDecoder` constructor 호출 동안만 유효하면
됩니다. Constructor가 `transfer_syntax_uid`, `photometric`, `frame_fragments`를
즉시 parse해서 내부 state로 저장합니다.

### Fragment Format

Encapsulated transfer syntax에서는 `frame_fragments`가 필수입니다.
형식은 `PixelFragment`와 같은 value-byte 의미를 사용합니다.

```text
offset:length,offset:length;offset:length
```

- `;`는 frame을 구분합니다.
- `,`는 한 frame 안의 fragment를 구분합니다.
- `offset`은 `pixel_payload` buffer 시작 기준입니다.
- `offset`은 8-byte item header가 아니라 fragment value bytes를 가리킵니다.
- `length`는 payload buffer 안에 저장된 fragment value length입니다.
- native/uncompressed payload에서는 이 field를 비워 둡니다.

`pixel_payload_decode_descriptor()`를 full source DICOM에서 호출하면,
`write_bytes_split_pixel_payload().pixel_payload_bytes`에 쓰일 offset 기준으로
계산합니다. 그래서 source DICOM의 원래 PixelSequence offset이 absolute file
offset이더라도 descriptor는 emitted split payload와 바로 호환됩니다.

## Detach와 Metadata Lifetime

`detach_pixel_payload()` 이후:

- metadata 조회는 계속 동작합니다.
- `has_attached_pixel_payload` / `has_attached_pixel_payload()`는 false가 됩니다.
- pixel decode와 encoded-frame access는 명확한 "payload is detached" 오류로
  실패합니다.
- PixelData element는 기본적으로 lightweight detached marker만 유지합니다.

diagnostics를 위해 현재 PixelData dump text를 보존하려면 Python에서는
`keep_dump=True`, C++에서는 `detach_pixel_payload(true)`를 사용하세요.

```python
df.detach_pixel_payload(keep_dump=True)
print(df.dump())
```

```cpp
file->detach_pixel_payload(true);
std::cout << file->dump();
```

dump text 보존은 frame/fragment indexing을 디버깅할 때 유용하지만, 문자열을
만들기 위해 모든 frame/fragment metadata를 순회할 수 있습니다. 기본 detach
경로는 이 비용을 피합니다.

## Dump 출력과 Offset

일반 source DICOM과 reattached split DICOM의 dump output이 byte-for-byte로
완전히 같을 필요는 없습니다.

일반 metadata element 값은 보통 동일해야 합니다. PixelData는 offset이 달라질 수
있습니다.

- 일반 DICOM은 원본 file stream 기준 offset을 보여줍니다.
- reattached encapsulated PixelData는 external payload buffer 기준 frame/fragment
  offset을 보여줍니다.
- main P10 placeholder 자체는 PixelSequence backing stream으로 사용하지 않습니다.

dump를 비교할 때는 raw offset text보다 semantic field와 frame payload bytes를
비교하세요.

```python
source = dicom.read_file("image.dcm")
main_bytes, payload = source.write_bytes_split_pixel_payload()
roundtrip = dicom.read_bytes_with_pixel_payload(main_bytes, payload)

assert roundtrip["PixelData"].vr == source["PixelData"].vr
assert roundtrip.encoded_pixel_frame_bytes(0) == source.encoded_pixel_frame_bytes(0)
```

현장 DICOM 중에는 DICOM item value length가 even이어야 한다는 규칙과 달리,
encapsulated fragment item length가 홀수인 파일도 있습니다. DicomSDL은 이런
파일을 읽을 때 허용합니다. Split payload를 다시 쓸 때는 fragment item value
length를 even으로 serialize하고 필요하면 padding을 붙입니다.

## Placeholder 검증

Split buffer를 읽으려면 main DICOM의 PixelData placeholder가 유효해야 합니다.
DicomSDL은 다음 경우를 거부합니다.

- main P10 DICOM에 PixelData가 없음
- PixelData value length가 정확히 4 bytes가 아님
- PixelData value가 `DXP1`이 아님
- external pixel payload pointer가 null이거나 payload length가 0
- encapsulated payload bytes를 PixelSequence로 indexing할 수 없음

placeholder는 직접 확인할 수 있습니다.

```python
main_only = dicom.read_bytes(main_bytes)
assert main_only["PixelData"].value_bytes() == dicom.PIXEL_PAYLOAD_PLACEHOLDER_MAGIC
```

## 예제 프로그램

저장소에는 C++와 Python 예제가 함께 들어 있습니다.

```bash
cmake -S . -B build -DDICOM_BUILD_EXAMPLES=ON
cmake --build build --target split_pixel_payload_example
```

이미 분리된 buffer 읽기:

```bash
build/_deps/dicomsdl_openjpeg-build/bin/split_pixel_payload_example \
  image.main.dcm image.pixel_payload.bin 0
```

split buffer 생성:

```bash
build/_deps/dicomsdl_openjpeg-build/bin/split_pixel_payload_example \
  --split image.dcm image.main.dcm image.pixel_payload.bin 0
```

Python:

```bash
python examples/python/split_pixel_payload_example.py \
  --split image.dcm image.main.dcm image.pixel_payload.bin
```

```bash
python examples/python/split_pixel_payload_example.py \
  --split image.dcm image-rle.main.dcm image-rle.pixel_payload.bin \
  --transfer-syntax RLELossless
```

## API 요약

Python:

- `DicomFile.write_bytes_split_pixel_payload() -> (bytes, bytes)`
- `DicomFile.write_with_transfer_syntax_split_pixel_payload(...) -> (bytes, bytes)`
- `dicomsdl.read_bytes_with_pixel_payload(main, payload, name=..., copy=...)`
- `DicomFile.pixel_payload_decode_descriptor() -> PixelPayloadDecodeDescriptor`
- `PixelPayloadDecoder(descriptor, pixel_payload)`
- `PixelPayloadDecoder.create_decode_plan(options=...)`
- `PixelPayloadDecoder.decode_into(out, frame=0, plan=...)`
- `PixelPayloadDecoder.to_array(frame=0, plan=...)`
- `PixelPayloadDecoder.pixel_array(frame=0, plan=...)` alias
- `DicomFile.detach_pixel_payload(keep_dump=False)`
- `DicomFile.has_attached_pixel_payload`

C++:

- `DicomFile::write_bytes_split_pixel_payload(...)`
- `DicomFile::write_with_transfer_syntax_split_pixel_payload(...)`
- `read_bytes_with_pixel_payload(...)`
- `DicomFile::attach_to_memory_with_pixel_payload(...)`
- `DicomFile::pixel_payload_decode_descriptor()`
- `pixel::pixel_payload_decode_descriptor(const DicomFile&)`
- `pixel::PixelPayloadDecoder`
- `pixel::parse_photometric(...)`
- `pixel::photometric_to_string(...)`
- `DicomFile::detach_pixel_payload(bool keep_dump = false)`
- `DicomFile::has_attached_pixel_payload()`
