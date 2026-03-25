# 파일 I/O

이 페이지에서는 디스크 및 메모리 입력, 부분 로딩, 파일, 바이트 및 스트림의 기본 출력 경로를 다룹니다.

## 파일 I/O 작동 방식

- `read_file(...)` 및 `read_bytes(...)`는 `DicomFile`를 생성하고 최대 `load_until`까지 입력을 즉시 구문 분석합니다.
- `write_file(...)` 및 `write_bytes(...)`는 `DicomFile` 개체를 파일 또는 바이트로 직렬화합니다.
- `write_with_transfer_syntax(...)`는 다른 전송 구문을 사용하여 파일이나 스트림에 직접 쓰기 위한 출력 지향 트랜스코드 경로입니다. 이는 예를 들어 `HTJ2KLossless`와 같이 픽셀 압축을 변경할 때 원하는 경우가 많습니다. 소스 객체를 먼저 변경하지 않습니다. C++에서는 동일한 API 제품군에도 스트림 오버로드가 있습니다.

## 디스크에서 읽기

**C++**

```cpp
#include <dicom.h>

auto file = dicom::read_file("in.dcm");
```

**파이썬**

```python
import dicomsdl as dicom

df = dicom.read_file("in.dcm")
```

참고:
- 필요한 태그가 파일 앞부분에 있고 읽지 않은 꼬리가 바로 필요하지 않은 경우 `load_until`를 사용하세요. 전체 데이터 세트를 미리 구문 분석하지 않고 읽기 비용을 줄일 수 있습니다.
- 이후 태그 액세스는 암시적으로 구문 분석을 계속하지 않습니다. C++와 Python 모두에서 나중에 더 필요할 때 `ensure_loaded(tag)`를 호출하세요. C++에서 `ensure_loaded(...)`는 `"Rows"_tag`, `"(0028,FFFF)"_tag` 또는 `dicom::Tag(0x0028, 0x0010)`와 같은 `Tag`를 사용합니다. Python에서 `ensure_loaded(...)`는 `Tag`, 압축된 `int` 또는 단일 태그에 대한 키워드 문자열을 허용합니다. 점으로 구분된 태그 경로 문자열은 지원되지 않습니다.
- 즉각적인 예외 대신 부분적으로 읽은 데이터를 보관하려면 `keep_on_error=True`를 사용하십시오. 그런 다음 `has_error` 및 `error_message`를 검사하십시오.
- Python에서 `path`는 `str` 및 `os.PathLike`를 허용합니다. C++에서 `read_file(...)`, `write_file(...)` 및 `write_with_transfer_syntax(...)`와 같은 디스크 경로 API는 `std::filesystem::path`를 사용합니다.

## 메모리에서 읽기

**C++**

```cpp
#include <dicom.h>
#include <vector>

std::vector<std::uint8_t> payload = /* full DICOM byte stream */;
auto file = dicom::read_bytes("in-memory in.dcm", std::move(payload));
```

**파이썬**

```python
from pathlib import Path
import dicomsdl as dicom

payload = Path("in.dcm").read_bytes()
df = dicom.read_bytes(payload, name="in-memory in.dcm")
```

참고:
- `name`는 `path()` / `path` 및 진단에 의해 보고된 식별자가 됩니다.
- `load_until`는 메모리 내 입력과 동일한 방식으로 작동합니다. 데이터 세트의 초기 부분만 필요하지만 읽지 않은 테일 데이터가 나중에 암시적으로 로드되지 않는 경우에 유용합니다.
- Python에서 `read_bytes(..., copy=False)`는 복사하는 대신 호출자 버퍼에 대한 참조를 유지합니다. `DicomFile`이 그 버퍼를 계속 참조하는 동안에는 버퍼를 살아 있게 유지하고 내용을 바꾸지 마세요.
- C++에서 `read_bytes(...)`는 원시 포인터에서 복사하거나 이동된 `std::vector<std::uint8_t>`의 소유권을 가져올 수 있습니다.

## 단계적 읽기

**C++**

```cpp
#include <dicom.h>
using namespace dicom::literals;

dicom::ReadOptions opts;
opts.load_until = "0028,ffff"_tag;

auto file = dicom::read_file("in.dcm", opts);  // initial partial parse

auto& ds = file->dataset();
ds.ensure_loaded("PixelData"_tag);  // later, advance farther
```

**파이썬**

```python
import dicomsdl as dicom

df = dicom.read_file("in.dcm", load_until=dicom.Tag("0028,ffff"))
df.ensure_loaded("PixelData")
```

참고:
- 초기 구문 분석을 조기에 중지하려면 `options.load_until`를 설정하십시오.
- 부분 읽기 후 `ensure_loaded(tag)`를 사용하여 더 많은 데이터 요소를 구문 분석합니다. C++에서는 `"Rows"_tag`, `"(0028,FFFF)"_tag` 또는 `dicom::Tag(...)`와 같은 `Tag`를 전달합니다.
- 부분적으로 로드된 데이터 세트에서 아직 구문 분석되지 않은 데이터 요소는 나중에 조회하거나 쓰기 위해 암시적으로 로드되지 않습니다.
- Python에서 `ensure_loaded(...)`는 `Tag`, 압축된 `int` 또는 단일 태그에 대한 키워드 문자열을 허용합니다. 중첩된 점으로 구분된 태그 경로 문자열은 지원되지 않습니다.
- `read_bytes(...)`에서도 동일한 단계적 읽기 패턴이 작동합니다. 제로 카피 메모리 입력을 원할 때 `copy=false`를 사용하십시오.
- `read_bytes(..., copy=false)`를 사용하면 호출자 소유 버퍼가 `DicomFile`보다 오래 지속되어야 합니다.

## 부분 로딩 및 허용 읽기

- `load_until`는 요청된 태그를 읽은 후 구문 분석을 중지합니다.
- `keep_on_error`는 부분적으로 읽은 데이터를 유지하고 읽기 실패를 `DicomFile`에 기록합니다.
- 파일이나 메모리에서 로드된 부분적으로 로드된 데이터 세트에서 조회 및 돌연변이 API는 아직 구문 분석되지 않은 데이터 요소를 암시적으로 계속 로드하지 않습니다.
- 실제로 이는 이후 태그 액세스가 누락 또는 발생으로 동작할 수 있으며 이후 태그 쓰기가 읽지 않은 데이터를 자동으로 변경하는 대신 발생할 수 있음을 의미합니다.

**C++**

```cpp
#include <dicom.h>
using namespace dicom::literals;

dicom::ReadOptions opts;
opts.load_until = "0002,ffff"_tag;  // stop after file meta

auto file = dicom::read_file("in.dcm", opts);
auto& ds = file->dataset();

ds.ensure_loaded("0028,0011"_tag);  // advance through Columns

long rows = ds.get_value<long>("0028,0010"_tag, -1L);  // Rows
long cols = ds.get_value<long>("0028,0011"_tag, -1L);  // Columns
long bits = ds.get_value<long>("0028,0100"_tag, -1L);  // BitsAllocated

// 이제 행과 열을 사용할 수 있습니다
// (0028,0100)이 아직 구문 분석되지 않았기 때문에 비트는 여전히 -1입니다.

ds.ensure_loaded("0028,ffff"_tag);
bits = ds.get_value<long>("0028,0100"_tag, -1L);  // now available
```

참고:
- 필요한 태그가 데이터세트 앞부분에 모여 있을 때 유용합니다.
- 또한 전체 데이터세트나 픽셀 페이로드를 건드리지 않고 메타데이터 인덱스나 데이터베이스를 구축하는 등 많은 DICOM 파일에 대한 빠른 스캔에도 적합합니다.
- Python은 일반 태그 및 키워드에 대해 동일한 `ensure_loaded(...)` 연속 패턴을 지원합니다.

## `DicomFile` 객체를 파일 또는 바이트로 직렬화

**C++**

```cpp
#include <dicom.h>

auto file = dicom::read_file("in.dcm");

dicom::WriteOptions opts;
opts.include_preamble = true;
opts.write_file_meta = true;
opts.keep_existing_meta = false;

file->write_file("out.dcm", opts);
auto payload = file->write_bytes(opts);
```

**파이썬**

```python
import dicomsdl as dicom

df = dicom.read_file("in.dcm")
payload = df.write_bytes(keep_existing_meta=False)
df.write_file("out.dcm", keep_existing_meta=False)
```

참고:
- 기본 옵션을 사용하면 `write_file()` 및 `write_bytes()`는 프리앰블 및 파일 메타 정보가 포함된 일반 Part 10 스타일 출력을 생성합니다.
- `write_file_meta=False`는 파일 메타 그룹을 생략합니다.
- `include_preamble=False`는 128바이트 프리앰블을 생략합니다.
- `keep_existing_meta=False`는 쓰기 전에 파일 메타를 다시 작성합니다. 해당 단계가 직렬화 전에 명시적으로 발생하도록 하려면 `rebuild_file_meta()`를 사용하세요.
- 이 API는 `DicomFile` 객체를 파일 또는 바이트로 직렬화합니다. 별도의 출력 전용 트랜스코드 경로를 제공하지 않습니다.

## 파일이나 스트림으로 직접 트랜스코딩

**C++**

```cpp
#include <dicom.h>

auto file = dicom::read_file("in.dcm");
file->write_with_transfer_syntax(
    "out_htj2k_lossless.dcm",
    dicom::uid::WellKnown::HTJ2KLossless
);
```

**파이썬**

```python
from pathlib import Path
import dicomsdl as dicom

df = dicom.read_file("in.dcm")
df.write_with_transfer_syntax(Path("out_htj2k_lossless.dcm"), "HTJ2KLossless")
```

참고:
- `write_with_transfer_syntax(...)`는 대상 전송 구문을 사용하여 출력으로 직접 트랜스코딩합니다. 이는 소스 `DicomFile`를 변경하지 않고 픽셀 압축을 예를 들어 `HTJ2KLossless`로 변경하는 데 자주 사용됩니다.
- 실제 목표가 출력 파일이나 스트림인 경우, 특히 큰 픽셀 페이로드의 경우 선호됩니다. 디코딩 작업 버퍼와 재인코딩된 대상 `PixelData`를 필요 이상으로 오래 유지하는 메모리 내 트랜스코드 경로를 피함으로써 최대 메모리 사용을 줄일 수 있습니다.
- Python에서 `write_with_transfer_syntax(...)`는 경로 기반 출력 전용 트랜스코드 API입니다. C++에서는 동일한 API 제품군이 직접 스트림 출력도 지원합니다.
- 검색 가능한 출력은 필요할 때 `ExtendedOffsetTable` 데이터를 백패치할 수 있습니다. 탐색할 수 없는 출력은 유효한 DICOM으로 유지되지만 해당 테이블을 생략하고 빈 기본 오프셋 테이블을 사용할 수 있습니다.
- 일반적인 검색 가능한 출력은 로컬 디스크의 일반 파일입니다. 검색할 수 없는 일반적인 출력은 파이프, 소켓, stdout, HTTP 응답 스트림 또는 zip 항목 스타일 스트림입니다.

## 어떤 API를 사용해야 하나요?

- 로컬 파일, 요청된 경계까지 즉시 구문 분석: `read_file(...)`
- 이미 메모리에 있는 바이트: `read_bytes(...)`
- Python의 제로 복사 메모리 입력: `read_bytes(..., copy=False)`
- C++의 파일 지원 단계적 읽기: `read_file(...)`와 `load_until`, 그 다음에는 `ensure_loaded(...)`
- C++의 메모리에서 제로 복사 단계적 읽기: `copy=false` 및 선택적 `load_until`가 포함된 `read_bytes(...)`, 이후 `ensure_loaded(...)`
- `DicomFile` 객체를 파일 또는 바이트로 직렬화: `write_file(...)` 또는 `write_bytes(...)`
- 새로운 전송 구문을 경로에 직접 작성: `write_with_transfer_syntax(...)`
- C++에서는 출력 스트림에 직접 새로운 전송 구문을 작성합니다: `write_with_transfer_syntax(...)`

## 관련 문서

- [핵심 개체](core_objects.md)
- [C++ 데이터세트 가이드](cpp_dataset_guide.md)
- [Python 데이터세트 가이드](python_dataset_guide.md)
- [픽셀 디코드](pixel_decode.md)
- [픽셀 인코딩](pixel_encode.md)
- [C++ API 개요](../reference/cpp_api.md)
- [데이터세트 참조](../reference/dataset_reference.md)
- [디콤파일 참고](../reference/dicomfile_reference.md)
- [오류 처리](error_handling.md)
