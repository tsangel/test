# 빠른 시작

## Python
대부분의 사용자는 PyPI 설치 경로부터 시작하면 됩니다.
1. 요구 사항: Python 3.9+, `pip`
2. PyPI에서 설치

```bash
python -m pip install --upgrade pip
pip install "dicomsdl[numpy,pil]"
```

현재 플랫폼에서 `pip`가 source build로 되돌아가면 먼저 `cmake`를 설치하세요.

```{note}
서버에서 metadata 접근, file I/O, transcode workflow만 필요하다면
`pip install dicomsdl`만으로 충분합니다.
```

source build, custom wheel, 테스트 workflow가 필요하면 [Build Python From Source](../developer/build_python_from_source.md)를 보세요.
플랫폼별 설치 세부 사항이 필요하면 [Installation](installation.md)을 보세요.

3. 메타데이터 읽기

```pycon
>>> import dicomsdl as dicom
>>> df = dicom.read_file("sample.dcm")
>>> df.PatientName
PersonName(Doe^Jane)
>>> df.Rows, df.Columns
(512, 512)
```

`DicomFile`은 root `DataSet` 접근 helper를 forwarding하므로, Python에서는 `df.Rows`, `df.PatientName` 같은 일반적인 top-level keyword 읽기가 보통 가장 짧고 추천되는 metadata read 경로입니다.
중첩된 leaf lookup에는 `df.get_value("Seq.0.Tag")`를 사용하고, 타입 값이 아니라 `DataElement` 메타데이터가 필요할 때는 `df["Rows"]` / `df.get_dataelement(...)`를 사용하세요.
알려진 keyword가 단순히 누락된 경우에는 `None`을 반환하고, 알 수 없는 keyword는 여전히 `AttributeError`를 발생시킵니다.
dataset 경계를 명시적으로 드러내고 싶다면 `df.dataset`을 쓰세요.
`PatientName`은 `PN`이므로 `df.PatientName`은 일반 Python 문자열이 아니라 `PersonName(...)` 객체로 표시됩니다.
객체 모델, metadata lookup 규칙, 전체 decode 흐름이 필요하면 [Core Objects](core_objects.md), [Python DataSet Guide](python_dataset_guide.md), [Pixel Decode](pixel_decode.md)를 보세요.
중첩된 sequence를 재귀적으로 순회해야 하면 [DataSet Walk](dataset_walk.md)를 보세요.
일부 tag만 골라 읽고 싶다면 [Selected Read](selected_read.md)를 보세요.

4. 픽셀을 NumPy 배열로 decode하기

```pycon
>>> import dicomsdl as dicom
>>> df = dicom.read_file("sample.dcm")
>>> arr = df.to_array()
>>> arr.shape
(512, 512)
>>> arr.dtype
dtype('uint16')
```

decode 옵션, frame 선택, 출력 layout 제어가 필요하면 [Pixel Decode](pixel_decode.md)를 보세요.

5. Pillow로 빠르게 이미지 미리보기

```bash
pip install "dicomsdl[numpy,pil]"
```

```python
import dicomsdl as dicom

df = dicom.read_file("sample.dcm")
image = df.to_pil_image(frame=0)
image.show()
```

`to_pil_image()`는 빠른 시각 확인을 위한 간단한 convenience helper입니다.
분석 파이프라인과 반복 가능한 처리는 `to_array()`를 우선하세요. `show()`는 로컬 GUI/viewer에 의존하므로 headless 환경에서는 동작하지 않을 수 있습니다.
decode 옵션이나 배열 중심 workflow가 필요하면 [Pixel Decode](pixel_decode.md)를 보세요.

6. `HTJ2KLossless`로 transcode해서 새 파일 쓰기

```python
from pathlib import Path

import dicomsdl as dicom

in_path = Path("in.dcm")
out_path = Path("out_htj2k_lossless.dcm")

df = dicom.read_file(in_path)
df.set_transfer_syntax("HTJ2KLossless")
df.write_file(out_path)

print("Input bytes:", in_path.stat().st_size)
print("Output bytes:", out_path.stat().st_size)
```

대표적인 파일에서는 출력이 대략 이렇게 보입니다.

```text
Input bytes: 525312
Output bytes: 287104
```

이 file-to-file transcode 경로는 기본 `pip install dicomsdl` 설치만으로도 사용할 수 있습니다. 실제 크기 변화는 source transfer syntax, pixel content, metadata에 따라 달라집니다.
lossy encode 옵션, codec 제한, streaming write 안내가 필요하면 [Pixel Encode](pixel_encode.md), [Pixel Encode Constraints](../reference/pixel_encode_constraints.md), [Encode-capable Transfer Syntax Families](../reference/codec_support_matrix.md)를 보세요.

7. `memoryview`로 `DataElement` 값 바이트 접근하기

```python
import dicomsdl as dicom

df = dicom.read_file("sample.dcm")
elem = df["PixelData"]
if elem:
    raw = elem.value_span()  # memoryview
    print("Raw bytes:", raw.nbytes)
    print("Head:", list(raw[:8]))
```

압축되지 않은 `512 x 512` `uint16` 이미지라면:

```text
Raw bytes: 524288
Head: [34, 12, 40, 12, 36, 12, 39, 12]
```

앞부분 바이트는 파일마다 달라집니다. 이 직접적인 `value_span()` view는 native / uncompressed `PixelData`용입니다. 압축된 encapsulated transfer syntax에서는 `PixelData`가 `PixelSequence`로 저장되므로 `elem.value_span()`은 비어 있고, 보통은 `df.encoded_pixel_frame_view(0)`를 사용하면 됩니다. borrowed view가 아니라 분리된 복사본이 필요할 때는 `df.encoded_pixel_frame_bytes(0)`를 사용하세요.
`raw`를 사용하는 동안에는 `df`를 살아 있게 두세요. 이 memoryview는 로드된 DICOM 객체가 소유한 바이트를 가리키며, 그 바이트가 교체되면 더 이상 유효하지 않습니다.
raw byte 의미나 encapsulated `PixelData`의 세부 사항이 필요하면 [DataElement Reference](../reference/dataelement_reference.md)와 [Pixel Reference](../reference/pixel_reference.md)를 보세요.

전체 decode safety 모델이 필요하면 [Pixel Decode](pixel_decode.md)와 [Error Handling](error_handling.md)을 보세요.

## C++
저장소 checkout에서 빌드합니다.
요구 사항: `git`, `CMake`, `C++20` 컴파일러
1. 저장소 clone

```bash
git clone https://github.com/tsangel/dicomsdl.git
cd dicomsdl
```

2. configure 및 build
```bash
cmake -S . -B build -DDICOM_BUILD_EXAMPLES=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

3. 사용 예제
```cpp
#include <dicom.h>
#include <diagnostics.h>
#include <iostream>
#include <memory>
using namespace dicom::literals;

int main() {
  auto file = dicom::read_file("sample.dcm");
  auto& ds = file->dataset();

  long rows = ds["Rows"_tag].to_long().value_or(0);
  // 존재 여부가 중요하다면 값을 꺼내기 전에 먼저 요소를 확인합니다.
  long cols = 0;
  if (auto& e = ds["Columns"_tag]; e) {
    cols = e.to_long().value_or(0);
  }
  std::cout << "Image size: " << rows << " x " << cols << '\n';
}
```

일반적인 출력은 다음과 같습니다.

```text
Image size: 512 x 512
```

더 자세한 C++ API 설명이 필요하면 [C++ API Overview](../reference/cpp_api.md)와 [DataSet Reference](../reference/dataset_reference.md)를 보세요.
재귀 순회나 하위 트리 건너뛰기가 필요하면 [DataSet Walk](dataset_walk.md)를 보세요.
디스크나 메모리에서 일부 tag만 골라 읽고 싶다면 [Selected Read](selected_read.md)를 보세요.

4. `ok &= ...`와 오류 확인으로 일괄 설정하기
```cpp
#include <dicom.h>
#include <diagnostics.h>
#include <memory>
#include <iostream>
using namespace dicom::literals;

int main() {
  dicom::DataSet ds;
  auto reporter = std::make_shared<dicom::diag::BufferingReporter>(256);
  dicom::diag::set_thread_reporter(reporter);

  bool ok = true;
  ok &= ds.add_dataelement("Rows"_tag, dicom::VR::US).from_long(512);
  ok &= ds.add_dataelement("Columns"_tag, dicom::VR::US).from_long(-1); // 실패 예시

  if (!ok) {
    for (const auto& msg : reporter->take_messages()) {
      std::cerr << msg << '\n';
    }
  }
  dicom::diag::set_thread_reporter(nullptr);
}
```

위 예제는 의도적으로 `Columns = -1`을 실패시키므로 출력은 대략 다음과 같습니다.
`VR::US`는 unsigned 값만 받기 때문에 `Columns = -1`에서 range error가 납니다.

```text
[ERROR] from_long tag=(0028,0011) vr=US reason=value out of range for VR
```

- 전체 실행 예제: `examples/batch_assign_with_error_check.cpp`
- `add_dataelement(...)`는 `DataElement&`를 반환하므로 write helper를 `.`로 이어서 호출합니다.
더 넓은 쓰기 패턴이나 실패 처리 안내가 필요하면 [C++ DataSet Guide](cpp_dataset_guide.md)와 [Error Handling](error_handling.md)을 보세요.
