# CLI 도구

`dicomsdl`은 사용자용 명령줄 도구 세 가지를 제공합니다.

- `dicomdump`: 사람이 읽기 쉬운 DICOM dump 출력
- `dicomshow`: Pillow로 한 프레임 빠르게 미리보기
- `dicomconv`: transfer syntax를 바꾸어 새 파일로 저장

## 명령 설치 방법

### Python wheel로 설치

```bash
pip install dicomsdl
```

이 명령은 `dicomdump`, `dicomconv`, `dicomshow` 콘솔 스크립트를 설치합니다.

`dicomshow`는 Pillow 기반 미리보기 경로를 쓰므로, 실제로는 보통 아래 설치가 더 편합니다.

```bash
pip install "dicomsdl[numpy,pil]"
```

### 소스 빌드에서 사용

`-DDICOM_BUILD_EXAMPLES=ON`으로 C++ 예제를 빌드하면 build 트리에
`dicomdump`, `dicomconv` 바이너리가 생성됩니다.

```bash
cmake -S . -B build -DDICOM_BUILD_EXAMPLES=ON
cmake --build build

./build/dicomdump sample.dcm
./build/dicomconv in.dcm out.dcm ExplicitVRLittleEndian
```

별도의 C++용 `dicomshow` 실행 파일은 없습니다. `dicomshow`는 Python 콘솔
스크립트 entry point입니다.

## `dicomdump`

`dicomdump`는 하나 이상의 DICOM 파일을 사람이 읽기 쉬운 텍스트로 보고 싶을 때 씁니다.
각 파일을 읽어 `DicomFile.dump(...)` 결과를 출력합니다.

### `dicomdump` 사용법

```bash
dicomdump [--max-print-chars N] [--no-offset] [--with-filename] <file> [file...]
```

### `dicomdump` 레퍼런스

위치 인자:

| 인자 | 의미 |
| --- | --- |
| `paths` | 하나 이상의 입력 경로입니다. `*.dcm` 같은 wildcard 패턴도 CLI에서 확장합니다. |

옵션:

| 옵션 | 의미 |
| --- | --- |
| `--max-print-chars N` | 긴 printable value를 `N`자 뒤에서 잘라서 출력합니다. 기본값은 `80`입니다. |
| `--no-offset` | `OFFSET` 열을 숨깁니다. |
| `--with-filename` | 각 출력 줄 앞에 `filename:` 접두사를 붙입니다. 입력 파일이 여러 개면 기본으로 켜집니다. |

### `dicomdump` 예시

```bash
dicomdump sample.dcm
dicomdump a.dcm b.dcm
dicomdump --no-offset --max-print-chars 120 sample.dcm
dicomdump "*.dcm"
```

입력 파일이 여러 개면 출력이 섞여도 구분할 수 있도록 각 줄 앞에 파일명이 붙습니다.

## `dicomshow`

`dicomshow`는 셸에서 빠르게 화면 확인을 할 때 쓰는 도구입니다. DICOM 파일 하나를 읽고,
`to_pil_image(frame=...)`로 한 프레임을 만든 뒤 `Pillow.Image.show()`를 호출합니다.

### `dicomshow` 사용법

```bash
dicomshow [--frame N] <input.dcm>
```

### `dicomshow` 레퍼런스

위치 인자:

| 인자 | 의미 |
| --- | --- |
| `input` | 입력 DICOM 파일 경로입니다. |

옵션:

| 옵션 | 의미 |
| --- | --- |
| `--frame N` | 미리볼 0-based 프레임 인덱스입니다. 기본값은 `0`입니다. |

### `dicomshow` 참고

- `dicomshow`는 빠른 미리보기용이지 진단용 뷰어가 아닙니다.
- 로컬 GUI/viewer 연결에 의존하므로 headless 환경에서는 동작하지 않을 수 있습니다.
- Pillow나 NumPy가 없으면 `dicomsdl[numpy,pil]`로 설치하세요.

### `dicomshow` 예시

```bash
dicomshow sample.dcm
dicomshow --frame 5 multiframe.dcm
```

## `dicomconv`

`dicomconv`는 셸 스크립트나 터미널에서 파일 대 파일 transfer syntax 변경을 하고 싶을 때 씁니다.

내부적으로 입력 파일을 읽고 `set_transfer_syntax(...)`를 적용한 뒤 새 경로로 저장합니다.

### `dicomconv` 사용법

```bash
dicomconv <input.dcm> <output.dcm> <transfer-syntax> [options]
```

`<transfer-syntax>`에는 다음을 쓸 수 있습니다.

- `ExplicitVRLittleEndian` 같은 transfer syntax keyword
- `1.2.840.10008.1.2` 같은 dotted UID 문자열
- `jpeg`, `jpeg2k`, `htj2k-lossless`, `jpegxl` 같은 shortcut alias

### `dicomconv` 레퍼런스

위치 인자:

| 인자 | 의미 |
| --- | --- |
| `input` | 입력 DICOM 파일 경로입니다. |
| `output` | 출력 DICOM 파일 경로입니다. |
| `transfer_syntax` | 대상 transfer syntax keyword, dotted UID 문자열, 또는 shortcut alias입니다. |

옵션:

| 옵션 | 적용 대상 | 의미 |
| --- | --- | --- |
| `--codec {auto,none,rle,jpeg,jpegls,j2k,htj2k,jpegxl}` | 전체 | 대상 transfer syntax만으로 추론하지 않고 codec 옵션 계열을 직접 지정합니다. |
| `--quality N` | `jpeg` | JPEG quality, 범위 `[1, 100]`입니다. |
| `--near-lossless-error N` | `jpegls` | JPEG-LS `NEAR`, 범위 `[0, 255]`입니다. |
| `--target-psnr V` | `j2k`, `htj2k` | target PSNR입니다. |
| `--target-bpp V` | `j2k`, `htj2k` | target bits-per-pixel입니다. |
| `--threads N` | `j2k`, `htj2k`, `jpegxl` | encoder thread 설정입니다. `-1`은 auto, `0`은 라이브러리 기본값입니다. |
| `--color-transform` | `j2k`, `htj2k` | MCT color transform을 켭니다. |
| `--no-color-transform` | `j2k`, `htj2k` | MCT color transform을 끕니다. |
| `--distance V` | `jpegxl` | JPEG-XL distance, 범위 `[0, 25]`입니다. `0`은 lossless입니다. |
| `--effort N` | `jpegxl` | JPEG-XL effort, 범위 `[1, 10]`입니다. |

전체 help, 예시, 지원되는 target transfer syntax 목록은 `dicomconv -h`에서 볼 수 있습니다.

### `dicomconv` 예시

```bash
dicomconv in.dcm out.dcm ExplicitVRLittleEndian
dicomconv in.dcm out.dcm 1.2.840.10008.1.2
dicomconv in.dcm out.dcm jpeg --quality 92
dicomconv in.dcm out.dcm jpegls-near-lossless --near-lossless-error 3
dicomconv in.dcm out.dcm jpeg2k --target-psnr 45 --threads -1
dicomconv in.dcm out.dcm htj2k-lossless --no-color-transform
dicomconv in.dcm out.dcm jpegxl --distance 1.5 --effort 7 --threads -1
```

## 종료 코드

세 명령 모두 다음 규칙을 따릅니다.

- 성공 시 `0`
- 입력, parse, decode, encode, write 단계 중 하나라도 실패하면 `1`

오류 메시지는 `dicomdump:`, `dicomshow:`, `dicomconv:` 같은 도구 이름 접두사와 함께
표준 오류로 출력됩니다.

## 관련 문서

- [설치](installation.md)
- [File I/O](file_io.md)
- [픽셀 디코드](pixel_decode.md)
- [픽셀 인코드](pixel_encode.md)
- [픽셀 인코드 제약](../reference/pixel_encode_constraints.md)
