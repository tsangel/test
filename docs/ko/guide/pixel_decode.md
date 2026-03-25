# 픽셀 디코드

디코딩 전에 검증된 출력 레이아웃이 필요하거나, 출력 버퍼를 직접 할당하거나 재사용해야 하거나, 디코딩된 행 또는 프레임 stride를 명시적으로 지정해야 하거나, 단일 프레임과 다중 프레임 입력에 같은 코드 경로를 쓰고 싶다면 `create_decode_plan()`과 `decode_into()`를 함께 사용하세요. 새 디코딩 결과를 가장 간단하게 얻으려면 C++에서는 `pixel_buffer()`, Python에서는 `to_array()`를 사용하면 됩니다.

## 핵심 디코드 API

**C++**

- `create_decode_plan(...)` + `decode_into(...)`
  - 호출자가 직접 준비한 출력 버퍼와 함께 검증된 재사용 가능 디코드 레이아웃이 필요할 때 이 두 함수를 함께 사용하세요. 단일 프레임 입력에서 버퍼를 미리 할당하거나 재사용하려는 경우와 `DecodeOptions`로 명시적인 출력 stride를 지정하려는 경우도 여기에 포함됩니다.
- `pixel_buffer(...)`
  - 새로운 픽셀 버퍼를 디코딩하고 반환합니다.

**파이썬**

- `create_decode_plan(...)` + `decode_into(...)`
  - 호출자가 직접 준비한 쓰기 가능한 배열 또는 버퍼와 함께 검증된 재사용 가능 디코드 레이아웃이 필요할 때 이 두 함수를 함께 사용하세요. 단일 프레임 입력에서 대상 버퍼를 미리 준비하거나 `DecodeOptions`로 명시적인 출력 stride를 지정하려는 경우도 여기에 포함됩니다.
- `to_array(...)`
  - 새로운 NumPy 배열을 디코드해 반환합니다. 가장 빠르게 결과를 확인할 수 있는 간단한 경로입니다.
- `to_array_view(...)`
  - 소스 픽셀 데이터가 압축되지 않은 전송 구문을 사용하는 경우 무복사 NumPy 뷰를 반환합니다.

## 관련 DICOM 표준 섹션

- 행, 열, 픽셀당 샘플, 광도 해석, Pixel Data를 규정하는 픽셀 속성은 [DICOM PS3.3 섹션 C.7.6.3, 이미지 픽셀 모듈](https://dicom.nema.org/medical/dicom/current/output/chtml/part03/sect_C.7.6.3.html)에 정의되어 있습니다.
- 기본 대 캡슐화된 픽셀 데이터 인코딩은 [DICOM PS3.5 8장, 픽셀, 오버레이 및 파형 데이터 인코딩](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/chapter_8.html) 및 [섹션 8.2, 기본 또는 캡슐화된 형식에 정의되어 있습니다. 인코딩](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/sect_8.2.html).
- 캡슐화된 조각/항목 레이아웃 및 전송 구문 요구 사항은 [DICOM PS3.5 섹션 A.4, 인코딩된 픽셀 데이터 캡슐화를 위한 전송 구문](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/sect_A.4.html)에 정의되어 있습니다.
- 파일 기반 워크플로에서 전송 구문 UID는 [DICOM PS3.10 7장, DICOM 파일 형식](https://dicom.nema.org/medical/dicom/current/output/chtml/part10/chapter_7.html)에 설명된 파일 메타 정보에서 가져옵니다.

## C++

### C++: 한 프레임을 디코딩하기 전에 출력 레이아웃을 검사하세요.

```cpp
#include <cstdint>
#include <dicom.h>
#include <span>
#include <vector>

auto file = dicom::read_file("single_frame.dcm");

// 계획에는 디코딩된 픽셀이 포함되지 않습니다.
// 대신 현재 파일 메타데이터의 유효성을 검사하고 무엇을 알려줍니다.
// 디코딩된 출력은 대상 메모리를 할당하기 전과 같아야 합니다.
const auto plan = file->create_decode_plan();

// 단일 프레임 디코드에서는 frame_stride가
// 이 plan으로 디코드할 때 필요한 정확한 바이트 수입니다.
std::vector<std::uint8_t> out(plan.output_layout.frame_stride);

// 프레임 0은 여기서 유일한 프레임이지만 이 호출 형태는 다음에도 적용됩니다.
// 다중 프레임 입력. 이를 통해 하나의 호출자 소유 버퍼 경로를 쉽게 유지할 수 있습니다.
// 단일 프레임 및 다중 프레임 코드 모두에 대해.
file->decode_into(0, std::span<std::uint8_t>(out), plan);

// 이제 `out`에는 plan이 설명한 레이아웃 그대로 디코드된 프레임 하나가 들어 있습니다.
```

### 여러 프레임에 걸쳐 하나의 계획과 하나의 대상 버퍼를 재사용합니다.

```cpp
#include <cstdint>
#include <dicom.h>
#include <span>
#include <vector>

auto file = dicom::read_file("multiframe.dcm");
const auto plan = file->create_decode_plan();

// 하나의 DecodePlan은 하나의 디코드 프레임 레이아웃을 의미하므로,
// 재사용 가능한 프레임 버퍼 하나를 만들고 매 프레임마다 다시 채울 수 있습니다.
std::vector<std::uint8_t> frame_bytes(plan.output_layout.frame_stride);

for (std::size_t frame = 0; frame < plan.output_layout.frames; ++frame) {
	// 다시 계산하는 대신 모든 프레임에 대해 동일한 검증된 레이아웃을 재사용합니다.
	// 메타데이터를 삭제하거나 매번 새로운 버퍼를 할당합니다.
	file->decode_into(frame, std::span<std::uint8_t>(frame_bytes), plan);

	// 다음 반복 전에 여기에서 `frame_bytes`를 처리, 복사 또는 전달합니다.
	// 다음 디코딩된 프레임으로 덮어씁니다.
}
```

### C++: DecodeOptions에서 계획 수립

```cpp
#include <cstdint>
#include <dicom.h>
#include <span>
#include <vector>

auto file = dicom::read_file("multiframe_j2k.dcm");

dicom::pixel::DecodeOptions options{};
options.alignment = 32;
// 디코딩된 이미지에 픽셀당 여러 샘플이 있는 경우 평면 출력을 요청하세요.
options.planar_out = dicom::pixel::Planar::planar;
// 백엔드가 실행될 때 코드 스트림 수준의 역 MCT/색상 변환을 적용합니다.
// 그것을 지원합니다. 이것이 기본값이자 일반적인 시작점입니다.
options.decode_mct = true;
// 외부 작업자 스케줄링은 주로 일괄 또는 다중 작업 항목 디코딩에 중요합니다.
options.worker_threads = 4;
// 지원되는 경우 최대 2개의 내부 스레드를 사용하도록 코덱 백엔드에 요청하세요.
options.codec_threads = 2;

// 계획에는 이러한 옵션이 암시하는 정확한 출력 레이아웃과 함께 캡처됩니다.
const auto plan = file->create_decode_plan(options);

// 전체 볼륨 디코딩의 경우 모든 디코딩된 프레임에 충분한 스토리지를 할당하십시오.
std::vector<std::uint8_t> volume(
    plan.output_layout.frames * plan.output_layout.frame_stride);

// decode_all_frames_into()는 동일한 검증된 계획을 사용하고 있지만 전체를 채운다.
// 한 번에 하나의 프레임 대신 출력 볼륨.
file->decode_all_frames_into(std::span<std::uint8_t>(volume), plan);
```

### C++: 명시적인 출력 스트라이드 요청

```cpp
#include <cstdint>
#include <dicom.h>
#include <span>
#include <vector>

auto file = dicom::read_file("multiframe.dcm");

const auto rows = static_cast<std::size_t>(file["Rows"_tag].to_long().value_or(0));
const auto cols = static_cast<std::size_t>(file["Columns"_tag].to_long().value_or(0));
const auto samples_per_pixel =
    static_cast<std::size_t>(file["SamplesPerPixel"_tag].to_long().value_or(1));
const auto frame_count =
    static_cast<std::size_t>(file["NumberOfFrames"_tag].to_long().value_or(1));
const auto bits_allocated =
    static_cast<std::size_t>(file["BitsAllocated"_tag].to_long().value_or(0));
const auto bytes_per_sample = (bits_allocated + 7) / 8;
const auto packed_row_bytes = cols * samples_per_pixel * bytes_per_sample;
const auto row_stride = ((packed_row_bytes + 32 + 31) / 32) * 32;
const auto frame_stride = row_stride * rows;

// 메타데이터 파생 레이아웃에서 먼저 대상 버퍼를 할당합니다.
std::vector<std::uint8_t> frame_bytes(frame_stride);
std::vector<std::uint8_t> volume_bytes(frame_count * frame_stride);

dicom::pixel::DecodeOptions options{};
// 인터리브된 출력이 기본값이지만 보폭이
// 아래 계산에서는 각 행 내의 인터리브된 샘플을 가정합니다.
options.planar_out = dicom::pixel::Planar::interleaved;
// 압축된 행 페이로드 외에 32바이트 이상을 추가한 다음 반올림하여
// 다음 32바이트 경계.
options.row_stride = row_stride;
options.frame_stride = frame_stride;

const auto plan = file->create_decode_plan(options);

// 계획은 위에서 선택한 명시적인 행/프레임 보폭을 검증합니다.
file->decode_into(0, std::span<std::uint8_t>(frame_bytes), plan);
file->decode_all_frames_into(std::span<std::uint8_t>(volume_bytes), plan);
```

## 파이썬

### Python: 한 프레임을 디코딩하기 전에 출력 레이아웃을 검사하세요.

```python
import dicomsdl as dicom
import numpy as np

df = dicom.read_file("single_frame.dcm")

# 계획은 실제 디코드가 일어나기 전에 디코드 결과의 dtype과 배열 shape를 알려줍니다.
# 호출자가 대상 배열을 먼저 할당해야 할 때 유용합니다.
plan = df.create_decode_plan()

# 먼저 계획에서 frame 0 하나의 정확한 NumPy 배열 shape를 확인합니다.
# 이렇게 하면 이후 decode_into() 호출과 같은 레이아웃 계약을 유지할 수 있습니다.
out = np.empty(plan.shape(frame=0), dtype=plan.dtype)

# 여기에서 레이아웃 메타데이터를 다시 계산하는 대신 이미 검증된 계획을 재사용하세요.
df.decode_into(out, frame=0, plan=plan)

# 이제 `out`에는 계획에서 지정한 레이아웃대로 디코드된 frame 하나가 들어 있습니다.
```

### 여러 프레임에 걸쳐 하나의 계획과 하나의 대상 어레이를 재사용합니다.

```python
import dicomsdl as dicom
import numpy as np

df = dicom.read_file("multiframe.dcm")
plan = df.create_decode_plan()

# 각 프레임은 하나의 계획에 대해 동일한 디코딩된 배열 shape를 가지므로,
# 재사용 가능한 배열 하나면 프레임별 처리 루프에 충분합니다.
frame_out = np.empty(plan.shape(frame=0), dtype=plan.dtype)

for frame in range(plan.frames):
    # decode_into()는 같은 검증된 계획을 재사용하면서
    # 매번 같은 대상 배열을 덮어씁니다.
    df.decode_into(frame_out, frame=frame, plan=plan)

    # 다음 반복 전에 여기에서 `frame_out`를 처리, 복사 또는 전달합니다.
    # 다음 디코딩된 프레임에 이를 재사용합니다.
```

### Python: DecodeOptions에서 계획 수립

```python
import dicomsdl as dicom
import numpy as np

df = dicom.read_file("multiframe_j2k.dcm")

options = dicom.DecodeOptions(
    alignment=32,
    planar_out=dicom.Planar.planar,
    # 백엔드가 실행될 때 코드 스트림 수준의 역 MCT/색상 변환을 적용합니다.
    # 그것을 지원합니다. 이것이 기본값이자 일반적인 시작점입니다.
    decode_mct=True,
    # 외부 작업자 스케줄링은 주로 배치 또는 다중 프레임 디코드에 중요합니다.
    worker_threads=4,
    # 지원되는 경우 최대 2개의 내부 스레드를 사용하도록 코덱 백엔드에 요청하세요.
    codec_threads=2,
)

# 계획은 요청된 디코드 동작을 캡처하므로 나중에 디코드 호출을 수행할 수 있습니다.
# 옵션을 반복하지 말고 `plan`을 다시 사용하세요.
plan = df.create_decode_plan(options)

# frame=-1은 "모든 프레임"을 의미합니다. 계획을 이용하면
# 대상 배열을 할당하기 전에 전체 볼륨의 정확한 shape를 알 수 있습니다.
volume = np.empty(plan.shape(frame=-1), dtype=plan.dtype)

# plan=... 이 제공되면, 계획에 캡처된 옵션이 디코드 동작을 결정합니다.
df.decode_into(volume, frame=-1, plan=plan)
```

### Python: 명시적인 출력 스트라이드 요청

```python
import dicomsdl as dicom
import numpy as np

df = dicom.read_file("test_le.dcm")

options = dicom.DecodeOptions(
    # 인터리브된 출력이 기본값이지만,
    # 예는 인터리브 레이아웃의 행 스트라이드를 설명하는 것입니다.
    planar_out=dicom.Planar.interleaved,
    # 이 작은 샘플 파일의 경우 더 큰 행 스트라이드를 사용하여 사용자 정의
    # 레이아웃이 분명합니다. 자신의 파일의 경우 압축된 것보다 더 큰 값을 선택하십시오.
    # 디코딩된 행 크기.
    row_stride=1024,
)
plan = df.create_decode_plan(options)

# to_array(plan=...)는 NumPy가 스트레이드가 계획과 일치하는 배열을 반환합니다.
# 이는 계획이 다음을 사용할 때 결과가 의도적으로 비연속적일 수 있음을 의미합니다.
# 명시적인 행 또는 프레임 스트라이드.
arr = df.to_array(frame=0, plan=plan)

# `arr.strides`는 이제 요청된 출력 스트라이드를 기반으로 하기 때문에 배열은
# 디코딩된 픽셀 값이 정확하더라도 의도적으로 비연속적일 수 있습니다.
```

### 사용자 정의 스트라이드 NumPy 뷰를 위해 원시 스토리지로 디코딩

```python
import dicomsdl as dicom
import numpy as np

df = dicom.read_file("test_le.dcm")
plan = df.create_decode_plan(
    # 이 작은 샘플은 의도적으로 큰 행 보폭을 사용하므로 사용자 지정
    # NumPyZ는 보기에 좋습니다. 자신의 파일의 경우 큰 값을 선택하십시오.
    # 하나의 디코딩된 행에 충분합니다.
    dicom.DecodeOptions(row_stride=1024)
)

# decode_into()에는 여전히 쓰기 가능한 C-연속 출력 버퍼 객체가 필요합니다.
# 사용자 정의 stride 레이아웃의 경우에는 계획이 요구하는 디코딩 바이트 수와
# 정확히 일치하는 원시 1차원 버퍼를 할당하세요.
raw = np.empty(
    plan.required_bytes(frame=0) // plan.bytes_per_sample,
    dtype=plan.dtype,
)
df.decode_into(raw, frame=0, plan=plan)

# 원시 storage를 계획과 stride가 일치하는 NumPy view로 감쌉니다.
# 이 단일 프레임 흑백 예제는 추가 픽셀 복사 없이
# 사용자 정의 stride를 가진 2차원 배열 view가 됩니다.
arr = np.ndarray(
    shape=plan.shape(frame=0),
    dtype=plan.dtype,
    buffer=raw,
    strides=(plan.row_stride, plan.bytes_per_sample),
)
```

### 먼저 NumPy 저장소를 준비한 다음 다중 프레임 출력을 해당 저장소로 디코딩합니다.

```python
import dicomsdl as dicom
import numpy as np

df = dicom.read_file("multiframe.dcm")

# 이 예에서는 부스 uint16 프레임 출력을 가정합니다.
# 느낌이 있는 dtype 또는 샘플이 다른 경우에 먼저 그러한 값을 조정하세요.
dtype = np.uint16
itemsize = np.dtype(dtype).itemsize
rows = int(df.Rows)
cols = int(df.Columns)
frame_count = int(df.NumberOfFrames)
packed_row_bytes = cols * itemsize
# 압축된 행 페이로드 외에 32바이트 이상을 추가한 다음 반올림하여
# 다음 32바이트 경계.
row_stride = ((packed_row_bytes + 32 + 31) // 32) * 32
frame_stride = row_stride * rows

# 먼저 일반 1차원 C 연속 NumPy 배열로 백업 스토리지를 준비합니다.
# 이것이 decode_into()가 실제로 써 넣을 대상 객체입니다.
backing = np.empty((frame_stride * frame_count) // itemsize, dtype=dtype)

# 디코딩하기 전에 같은 storage 위에 애플리케이션용 배열 view를 만듭니다.
# 이 예제는 프레임 우선 단색 레이아웃을 사용합니다.
#   (frames, rows, cols), strides=(frame_stride, row_stride, itemsize)
frames = np.ndarray(
    shape=(frame_count, rows, cols),
    dtype=dtype,
    buffer=backing,
    strides=(frame_stride, row_stride, itemsize),
)

# storage 레이아웃을 먼저 정한 뒤, 여기에 맞는 계획을 만듭니다.
plan = df.create_decode_plan(
    dicom.DecodeOptions(
        # 인터리브된 출력이 기본값이지만,
        # 위 storage 레이아웃이 인터리브된 샘플을 기준으로 준비되었음을 명확히 적습니다.
        planar_out=dicom.Planar.interleaved,
        row_stride=row_stride,
        frame_stride=frame_stride,
    )
)

# 계획이 수동으로 준비한 NumPy 레이아웃과 일치하는지 확인합니다.
assert plan.dtype == np.dtype(dtype)
assert plan.bytes_per_sample == itemsize
assert plan.shape(frame=-1) == frames.shape
assert plan.row_stride == row_stride
assert plan.frame_stride == frame_stride
assert plan.required_bytes(frame=-1) == backing.nbytes

# decode_into()는 대상 객체 자체가 쓰기 가능하고 C-연속이어야 합니다.
# 그래서 여기서는 `frames`가 아니라 `backing`을 전달합니다.
df.decode_into(backing, frame=-1, plan=plan)

# 이제 `frames`는 미리 준비한 NumPy 레이아웃을 통해 디코딩된 픽셀을 보여주고,
# `backing`은 기본 storage를 계속 소유합니다.
```

### C++ 디코드 실패를 명시적으로 처리

```cpp
#include <dicom.h>
#include <diagnostics.h>
#include <iostream>
#include <span>
#include <vector>

try {
    auto file = dicom::read_file("single_frame.dcm");
    const auto plan = file->create_decode_plan();

    std::vector<std::uint8_t> out(plan.output_layout.frame_stride);
    file->decode_into(0, std::span<std::uint8_t>(out), plan);
} catch (const dicom::diag::DicomException& ex) {
    // 메시지에는 일반적으로 상태=..., 단계=... 및 이유=...가 포함됩니다.
    // 따라서 하나의 로그 라인으로 오류가 발생한 위치를 확인할 수 있는 경우가 많습니다.
    // 메타데이터 검증, 대상 검증, 디코더 선택 또는
    // 백엔드 디코드 단계 자체.
    std::cerr << ex.what() << '\n';
}
```

### Python 디코드 실패를 명시적으로 처리

```python
import dicomsdl as dicom
import numpy as np

df = dicom.read_file("single_frame.dcm")

try:
    plan = df.create_decode_plan()
    out = np.empty(plan.shape(frame=0), dtype=plan.dtype)
    df.decode_into(out, frame=0, plan=plan)
except (TypeError, ValueError, IndexError) as exc:
    # 바인딩 레벨 유효성 검사 실패는 여기로 들어옵니다.
    # 잘못된 버퍼 유형, 잘못된 출력 크기, 잘못된 프레임 인덱스 등.
    print(exc)
except RuntimeError as exc:
    # RuntimeError는 보통 Python 인수 검사가 끝난 뒤
    # 기본 C++ 디코드 경로 자체가 실패했음을 의미합니다.
    print(exc)
```

## 예외

**C++**

| API | 예외 | 일반적인 이유 |
| --- | --- | --- |
| `create_decode_plan(...)` | `dicom::diag::DicomException` | 픽셀 메타데이터가 누락되었거나 일관성이 없거나, `alignment`가 유효하지 않거나, 명시적인 `row_stride`/`frame_stride`가 디코딩된 페이로드보다 작거나, 출력 레이아웃이 오버플로됩니다. |
| `decode_into(...)` | `dicom::diag::DicomException` | 계획이 더 이상 현재 파일 상태와 일치하지 않거나, 프레임 인덱스가 범위를 벗어났거나, 대상 버퍼가 너무 작거나, 디코더 바인딩을 사용할 수 없거나, 백엔드 디코딩이 실패했습니다. |
| `pixel_buffer(...)` | `dicom::diag::DicomException` | `decode_into(...)`와 동일한 오류 모드이지만 소유 버퍼 편의 경로에 있습니다. |
| `decode_all_frames_into(...)` | `dicom::diag::DicomException` | 전체 볼륨 대상이 너무 작거나, 프레임 메타데이터가 유효하지 않거나, 디코더 바인딩을 사용할 수 없거나, 백엔드 디코딩이 실패하거나, `ExecutionObserver`가 배치를 취소합니다. |

C++ 디코드 메시지에는 일반적으로 `invalid_argument`, `unsupported`, `backend_error`, `cancelled` 또는 `internal_error`와 같은 상태와 함께 `status=...`, `stage=...` 및 `reason=...`가 포함됩니다.

**파이썬**

| API | 레이즈 | 일반적인 이유 |
| --- | --- | --- |
| `create_decode_plan(...)` | `RuntimeError` | 픽셀 메타데이터가 누락되었거나, 요청된 출력 레이아웃이 유효하지 않거나, 디코딩된 레이아웃이 오버플로되어 기본 C++ 계획 생성이 실패합니다. |
| `to_array(...)` | `ValueError`, `IndexError`, `RuntimeError` | `frame < -1`, 잘못된 스레드 수, 범위를 벗어난 프레임 인덱스 또는 인수 유효성 검사 성공 후 기본 디코드 실패. |
| `decode_into(...)` | `TypeError`, `ValueError`, `IndexError`, `RuntimeError` | 대상이 쓰기 가능한 C 연속 버퍼가 아니거나, 항목 크기 또는 총 바이트 크기가 디코딩된 레이아웃과 일치하지 않거나, 프레임 인덱스가 범위를 벗어났거나, 기본 디코드 경로가 실패했습니다. |
| `to_array_view(...)` | `ValueError`, `IndexError` | 소스 전송 구문이 압축되었거나, 다중 샘플 기본 데이터가 인터리브되지 않았거나, 직접 원시 픽셀 보기를 사용할 수 없거나, 프레임 인덱스가 범위를 벗어났습니다. |

## 메모

- 단일 프레임 입력의 경우에도 `DecodePlan`는 호출 전체에서 대상 버퍼를 디코딩하거나 재사용하기 전에 출력 레이아웃을 검사하려는 경우에 유용합니다.
- `DecodePlan`를 디코딩된 픽셀의 캐시가 아닌 검증된 출력 계약으로 취급합니다.
- `DecodeOptions.row_stride` 및 `DecodeOptions.frame_stride`를 사용하면 디코딩된 출력에 대한 명시적인 행 및 프레임 스트라이드를 요청할 수 있습니다. 둘 중 하나가 0이 아니면 `alignment`가 무시됩니다.
- 명시적으로 디코딩된 스트라이드는 디코딩된 행 또는 프레임 페이로드에 맞게 충분히 커야 하며 디코딩된 샘플 크기에 맞춰 정렬되어야 합니다.
- 전송 구문, 행, 열, 픽셀당 샘플, 할당된 비트, 픽셀 표현, 평면 구성, 프레임 수 또는 픽셀 데이터 요소와 같은 픽셀에 영향을 미치는 메타데이터를 변경하는 경우 이전 디코드 레이아웃 가정을 재사용하지 마십시오.
- 픽셀에 영향을 미치는 메타데이터가 변경되면 다음 `decode_into()` 전에 새 `DecodePlan` 및 일치하는 출력 버퍼를 만듭니다.
- `decode_into()`는 벤치마크 또는 핫 루프 재사용 시나리오에 적합한 경로이거나 단일 프레임 및 다중 프레임 입력 모두에 대해 동일한 버퍼 관리 흐름을 원하는 경우에 적합합니다.
- Python에서 `to_array(plan=...)`는 계획이 명시적인 행 또는 프레임 스트라이드를 요청할 때 압축된 C 연속 배열 대신 사용자 정의 스트라이드가 있는 NumPy 배열을 반환할 수 있습니다.
- Python에서 `decode_into()`에는 쓰기 가능한 C 연속 대상 객체가 필요합니다. 사용자 정의 스트라이드 결과를 얻으려면 인접한 지원 스토리지로 디코딩한 다음 명시적인 스트라이드를 사용하여 NumPy 뷰를 통해 노출하세요.
- `to_array()`는 가장 빠른 첫 번째 성공을 위한 올바른 길입니다.

## 관련 문서

- [빠른 시작](quickstart.md)
- [픽셀 인코딩](pixel_encode.md)
- [픽셀 변환 메타데이터 해상도](../reference/pixel_transform_metadata.md)
