# 픽셀 인코딩

인코딩할 기본 저장 픽셀이 이미 있는 경우 `set_pixel_data()`를 사용하세요. 현재 `DicomFile`에 이미 픽셀 데이터가 있고 이를 메모리에서 트랜스코딩하려는 경우 `set_transfer_syntax()`를 사용합니다. 소스 객체를 먼저 변경하지 않고 목표가 다른 전송 구문으로 출력되는 경우 `write_with_transfer_syntax()`를 사용하십시오. 동일한 전송 구문 및 옵션 세트가 여러 호출에서 재사용되거나 더 긴 인코딩 루프를 시작하기 전에 해당 구성을 검증하려는 경우 `EncoderContext`를 생성하십시오.

## 키 인코딩 API

**C++**

- `set_pixel_data(...)`
  - `pixel::ConstPixelSpan`로 명시적으로 설명하는 레이아웃이 있는 네이티브 소스 버퍼의 픽셀 데이터를 대체합니다.
- `create_encoder_context(...)` + `set_pixel_data(...)` / `set_transfer_syntax(...)`
  - 반복되는 인코딩 또는 트랜스코드 루프 외부에 구성된 하나의 전송 구문 및 옵션 세트를 유지합니다.
- `write_with_transfer_syntax(...)`
  - 메모리 내 `DicomFile`를 변경하지 않고 파일이나 스트림에 직접 다른 전송 구문을 작성합니다.

**파이썬**

- `set_pixel_data(...)`
  - C 연속 NumPy 배열 또는 기타 연속 숫자 버퍼에서 픽셀 데이터를 바꿉니다.
- `create_encoder_context(...)` + `set_pixel_data(...)` / `set_transfer_syntax(...)`
  - 하나의 Python `options` 객체를 먼저 구문 분석하고 검증한 다음 반복 호출에서 결과 컨텍스트를 재사용합니다.
- `write_with_transfer_syntax(...)`
  - 소스 객체를 먼저 변경하지 않고 파일에 직접 다른 전송 구문을 작성합니다.
- `set_transfer_syntax(...)`
  - 나중에 동일한 개체에서 읽거나 쓸 때 새 구문을 사용하려면 메모리에서 현재 `DicomFile`를 트랜스코딩하세요.

## 관련 DICOM 표준 섹션

- 인코딩된 데이터와 일관성을 유지해야 하는 픽셀 메타데이터는 [DICOM PS3.3 섹션 C.7.6.3, 이미지 픽셀 모듈](https://dicom.nema.org/medical/dicom/current/output/chtml/part03/sect_C.7.6.3.html)에 정의되어 있습니다.
- 기본 대 캡슐화된 픽셀 데이터 인코딩과 코덱별 8.2.x 규칙은 [DICOM PS3.5 8장, 픽셀, 오버레이 및 파형 데이터 인코딩](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/chapter_8.html) 및 [섹션 8.2, 기본 또는 캡슐화된 형식에 정의되어 있습니다. 인코딩](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/sect_8.2.html).
- 캡슐화된 전송 구문 및 조각 규칙은 [DICOM PS3.5 섹션 A.4, 인코딩된 픽셀 데이터 캡슐화를 위한 전송 구문](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/sect_A.4.html)에 정의되어 있습니다.
- 파일 기반 인코딩 및 트랜스코드 워크플로에서 결과 전송 구문 UID는 [DICOM PS3.10 7장, DICOM 파일 형식](https://dicom.nema.org/medical/dicom/current/output/chtml/part10/chapter_7.html)에 정의된 파일 메타 정보로 전달됩니다.

## C++

### `set_pixel_data()`를 호출하기 전에 소스 픽셀을 명시적으로 설명하세요.

```cpp
#include <cstdint>
#include <dicom.h>
#include <random>
#include <span>
#include <vector>

using namespace dicom::literals;

auto file = dicom::read_file("sample.dcm");

const std::uint32_t rows = 256;
const std::uint32_t cols = 256;
const std::uint32_t frames = 1;

std::vector<std::uint16_t> pixels(rows * cols * frames);
std::mt19937 rng(0);
std::uniform_int_distribution<int> dist(0, 4095);
for (auto& px : pixels) {
    px = static_cast<std::uint16_t>(dist(rng));
}

const dicom::pixel::ConstPixelSpan source{
    .layout = dicom::pixel::PixelLayout{
        .data_type = dicom::pixel::DataType::u16,
        .photometric = dicom::pixel::Photometric::monochrome2,
        .planar = dicom::pixel::Planar::interleaved,
        .reserved = 0,
        .rows = rows,
        .cols = cols,
        .frames = frames,
        .samples_per_pixel = 1,
        .bits_stored = 12,
        .row_stride = cols * sizeof(std::uint16_t),
        .frame_stride = rows * cols * sizeof(std::uint16_t),
    },
    .bytes = std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(pixels.data()),
        pixels.size() * sizeof(std::uint16_t)),
};

// set_pixel_data()는 기본 소스 복구를 통해 위의 리소스 사용을 살펴봅니다.
// DicomFile에 일치하는 이미지를 사서함 데이터를 다시 작성합니다.
file->set_pixel_data("RLELossless"_uid, source);
```

### 반복되는 쓰기 루프 외부에 사전 구성된 컨텍스트 하나를 유지합니다.

```cpp
#include <array>
#include <dicom.h>
#include <diagnostics.h>
#include <iostream>
#include <span>

using namespace dicom::literals;

auto file = dicom::read_file("sample.dcm");

const std::array<dicom::pixel::CodecOptionTextKv, 3> j2k_options{{
    {"target_psnr", "45"},
    {"threads", "4"},
    {"color_transform", "true"},
}};

// 반복되는 루프 외부에 재사용 가능한 JPEG 2000 컨텍스트 하나를 구축합니다.
// 이렇게 하면 전송 구문과 옵션 세트가 대신 한 곳에 유지됩니다.
// 각 호출 사이트에서 동일한 옵션 목록을 다시 작성합니다.
auto j2k_ctx = dicom::pixel::create_encoder_context(
    "JPEG2000"_uid,
    std::span<const dicom::pixel::CodecOptionTextKv>(j2k_options));

try {
    for (const char* path : {"out_j2k_1.dcm", "out_j2k_2.dcm"}) {
        file->write_with_transfer_syntax(path, "JPEG2000"_uid, j2k_ctx);
    }
} catch (const dicom::diag::DicomException& ex) {
    // 인코딩 또는 configure 단계가 실패하면 예외 메시지에
    // 실패한 호출의 단계와 이유가 함께 담깁니다. 보통 ex.what()만
    // 기록해도 첫 번째 디버깅 단서로는 충분합니다.
    std::cerr << ex.what() << '\n';
}
```

### 출력에 직접 다른 전송 구문 작성

```cpp
#include <dicom.h>

using namespace dicom::literals;

auto file = dicom::read_file("sample.dcm");

// write_with_transfer_syntax()는 출력 지향 transcode 경로입니다.
// 대상 전송 구문은 직렬화된 결과에만 영향을 줍니다.
file->write_with_transfer_syntax("out_rle.dcm", "RLELossless"_uid);

// 같은 API 계열에는 C++의 std::ostream 오버로드도 있습니다.
```

### 명시적 코덱 옵션 전달

```cpp
#include <array>
#include <dicom.h>

using namespace dicom::literals;

auto file = dicom::read_file("sample.dcm");

const std::array<dicom::pixel::CodecOptionTextKv, 1> lossy_options{{
    {"target_psnr", "45"},
}};

// 손실 압축 대상이라면 원하는 코덱 옵션을 명시적으로 전달하세요.
// 의도한 출력과 맞지 않을 수 있는 기본값에 의존하지 않는 편이 좋습니다.
// 이런 직접 방식은 일회성 쓰기에는 충분하고, 같은 옵션 세트를 여러 번
// 재사용한다면 EncoderContext를 쓰는 편이 더 낫습니다.
file->write_with_transfer_syntax(
    "out_j2k_lossy.dcm", "JPEG2000"_uid,
    std::span<const dicom::pixel::CodecOptionTextKv>(lossy_options));
```

## 파이썬

### NumPy 배열의 픽셀 데이터 바꾸기

```python
import dicomsdl as dicom
import numpy as np

df = dicom.read_file("sample.dcm")

# set_pixel_data()는 C-contiguous 숫자 배열을 기대합니다.
# 배열 shape과 dtype으로부터 Rows, Columns,
# SamplesPerPixel, NumberOfFrames, bit depth 메타데이터를 결정합니다.
rng = np.random.default_rng(0)
arr = rng.integers(0, 4096, size=(256, 256), dtype=np.uint16)
df.set_pixel_data("ExplicitVRLittleEndian", arr)

df.write_file("native_replaced.dcm")
```

### 명시적 코덱 옵션을 `set_pixel_data()`에 전달

```python
import dicomsdl as dicom
import numpy as np

df = dicom.read_file("sample.dcm")
rng = np.random.default_rng(0)
arr = rng.integers(0, 4096, size=(256, 256), dtype=np.uint16)

# 손실이 있는 대상의 경우 코덱 옵션을 명시적으로 전달하여 인코딩 설정이
# 호출 사이트에서 볼 수 있습니다.
df.set_pixel_data(
    "JPEG2000",
    arr,
    options={"type": "j2k", "target_psnr": 45.0},
)
```

### Python 옵션 사전을 한 번 구문 분석하고 검증한 다음 컨텍스트를 재사용합니다.

```python
import dicomsdl as dicom

df = dicom.read_file("sample.dcm")

# create_encoder_context()는 여기에서 Python 옵션을 넣고 분석하고 검증합니다.
# 반복된 쓰기 루프가 시작되기 전에 한 번.
j2k_ctx = dicom.create_encoder_context(
    "JPEG2000",
    options={
        "type": "j2k",
        "target_psnr": 45.0,
        "threads": 4,
        "color_transform": True,
    },
)

# 반복되는 출력에 대해 동일한 검증된 전송 구문 및 옵션 세트를 재사용합니다.
for path in ("out_j2k_1.dcm", "out_j2k_2.dcm"):
    df.write_with_transfer_syntax(path, "JPEG2000", encoder_context=j2k_ctx)
```

### 인코딩 루프가 시작되기 전에 구성 오류를 검사하세요.

```python
import dicomsdl as dicom

try:
    dicom.create_encoder_context(
        "JPEG2000",
        options={
            "type": "j2k",
            "target_psnr": -1.0,
        },
    )
except ValueError as exc:
    # 장기 실행 인코딩 루프가 시작되기 전에 잘못된 옵션이 실패합니다.
    print(exc)
```

### 소스 객체를 변경하지 않고 다른 전송 구문 작성

```python
import dicomsdl as dicom

df = dicom.read_file("sample.dcm")

# write_with_transfer_syntax()는 직렬로 변환되어 변경됩니다.
# 내 메모리 DicomFile은 현재 전송 및 문자열 상태를 유지합니다.
df.write_with_transfer_syntax("out_rle.dcm", "RLELossless", options="rle")
```

## 예외

**C++**

| API | 예외 | 일반적인 이유 |
| --- | --- | --- |
| `create_encoder_context(...)` / `EncoderContext::configure(...)` | `dicom::diag::DicomException` | 전송 구문이 잘못되었거나 인코딩이 지원되지 않습니다. C++에서 대부분의 코덱 옵션 의미 체계는 나중에 인코딩 또는 트랜스코드 호출이 런타임 인코더를 구성할 때 유효성이 검사됩니다. |
| `set_pixel_data(...)` | `dicom::diag::DicomException` | 소스 레이아웃과 소스 바이트가 일치하지 않거나, 인코더 컨텍스트가 누락되거나 일치하지 않거나, 인코더 바인딩을 사용할 수 없거나, 백엔드가 현재 코덱 옵션이나 픽셀 레이아웃을 거부하거나, 인코딩 후 전송 구문 메타데이터 업데이트가 실패합니다. |
| `set_transfer_syntax(...)` | `dicom::diag::DicomException` | 전송 구문 선택이 잘못되었거나, 인코더 컨텍스트가 요청된 구문과 일치하지 않거나, 트랜스코드 경로가 지원되지 않거나, 백엔드 인코딩이 실패했습니다. |
| `write_with_transfer_syntax(...)` | `dicom::diag::DicomException` | 전송 구문 선택이 잘못되었거나, 인코더 컨텍스트가 요청된 구문과 일치하지 않거나, 트랜스코드 경로가 지원되지 않거나, 백엔드 인코딩이 실패하거나, 파일/스트림 출력이 실패합니다. |

C++ 인코딩 메시지에는 일반적으로 `invalid_argument`, `unsupported`, `backend_error` 또는 `internal_error`와 같은 상태와 함께 `status=...`, `stage=...` 및 `reason=...`가 포함됩니다.

**파이썬**

| API | 레이즈 | 일반적인 이유 |
| --- | --- | --- |
| `create_encoder_context(...)` | `TypeError`, `ValueError`, `RuntimeError` | `options`에 잘못된 컨테이너 또는 값 유형이 있거나, 옵션 키 또는 값이 유효하지 않거나, 전송 구문 텍스트를 알 수 없거나, 기본 C++ 구성 단계가 여전히 실패합니다. |
| `set_pixel_data(...)` | `TypeError`, `ValueError`, `RuntimeError` | `source`는 지원되는 버퍼 개체가 아니거나, C 연속적이지 않거나, 추론된 소스 형태 또는 dtype이 잘못되었거나, 인코딩 옵션이 잘못되었거나, 런타임 인코더/데이터 세트 업데이트가 실패합니다. |
| `set_transfer_syntax(...)` | `TypeError`, `ValueError`, `RuntimeError` | 전송 구문 텍스트가 잘못되었거나, `options` 개체 유형이 잘못되었거나, 옵션 값이 잘못되었거나, 인코더 컨텍스트가 요청된 구문과 일치하지 않거나, 트랜스코드 경로/백엔드가 실패했습니다. |
| `write_with_transfer_syntax(...)` | `TypeError`, `ValueError`, `RuntimeError` | 경로 또는 `options` 유형이 잘못되었거나, 전송 구문 텍스트 또는 옵션 값이 잘못되었거나, 인코더 컨텍스트가 요청된 구문과 일치하지 않거나, 쓰기/트랜스코딩이 실패했습니다. |

## 메모

- C++에서 `set_pixel_data()`는 사용자가 제공하는 `pixel::ConstPixelSpan` 레이아웃에서 기본 픽셀을 읽습니다. 소스 바이트에 행 또는 프레임 간격이 있는 경우 레이아웃은 해당 간격을 정확하게 설명해야 합니다.
- Python에서 `set_pixel_data()`는 C 연속 숫자 버퍼를 기대합니다. 배열이 현재 스트라이드되었거나 연속되지 않은 경우 먼저 `np.ascontiguousarray(...)`를 사용하세요.
- `set_pixel_data()`는 `Rows`, `Columns`, `SamplesPerPixel`, `BitsAllocated`, `BitsStored`, `PhotometricInterpretation`, `NumberOfFrames` 및 전송 구문 상태와 같은 관련 이미지 픽셀 메타데이터를 다시 작성합니다.
- `set_transfer_syntax()`는 메모리 내 `DicomFile`를 변경합니다. `write_with_transfer_syntax()`는 목표가 다르게 인코딩된 출력 파일 또는 스트림일 때 더 나은 경로입니다.
- 동일한 전송 구문 및 코덱 옵션이 반복적으로 적용되는 경우 `EncoderContext`를 재사용합니다. Python에서 `create_encoder_context(..., options=...)`는 `options` 개체를 미리 구문 분석하고 유효성을 검사합니다. C++에서 `EncoderContext`는 하나의 전송 구문과 옵션 세트를 함께 유지하지만 자세한 오류는 여전히 `dicom::diag::DicomException`로 나타납니다.
- 정확한 코덱 규칙, 옵션 이름 및 전송별 구문 제약 조건에 대해서는 간단한 예를 통해 추측하는 대신 참조 페이지를 사용하세요.

## 관련 문서

- [픽셀 디코드](pixel_decode.md)
- [파일 I/O](file_io.md)
- [픽셀 인코딩 제약 조건](../reference/pixel_encode_constraints.md)
