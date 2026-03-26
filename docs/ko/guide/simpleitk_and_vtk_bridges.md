# SimpleITK 및 VTK 브리지

DicomSDL은 DICOM 픽셀 데이터를 디코드한 뒤, 나머지 Python 워크플로를 크게 바꾸지 않고도 결과를 SimpleITK나 VTK 같은 toolkit 객체로 넘길 수 있습니다.

브리지 모듈은 다음 두 가지입니다.

- `dicomsdl.simpleitk_bridge`
- `dicomsdl.vtk_bridge`

DicomSDL이 DICOM 파싱, 슬라이스 정렬, 픽셀 디코드, rescale 처리를 담당하고, 이후 처리는 SimpleITK나 VTK에서 이어가고 싶을 때 이 모듈들을 사용하면 됩니다.

## 이 브리지들이 하는 일

두 브리지 모듈은 내부적으로 같은 volume 생성 경로를 공유하고, 마지막 변환 단계만 다릅니다.

이 브리지들은 다음을 수행합니다.

- 단일 파일 2D 이미지 또는 파일 기반 DICOM 시리즈 읽기
- stack 형태의 단일 파일 multiframe grayscale volume 읽기
- 슬라이스 정렬 및 volume 구성
- DICOM geometry에서 `spacing`, `origin`, `direction` 계산
- `to_modality_value=True`일 때 frame별 modality rescale 적용
- 디코드된 volume을 `SimpleITK.Image` 또는 `vtkImageData`로 변환

이 브리지들은 `sitk.ReadImage(...)`나 `vtkDICOMImageReader`의 완전한 drop-in replacement는 아닙니다.
DicomSDL 위에 얇게 얹힌 편의용 진입점으로 보는 편이 맞습니다.

## 설치

필요한 toolkit만 설치하면 됩니다.

```bash
pip install "dicomsdl[numpy]" SimpleITK vtk
```

하나만 쓴다면 해당 toolkit만 설치해도 됩니다.

```bash
pip install "dicomsdl[numpy]" SimpleITK
pip install "dicomsdl[numpy]" vtk
```

## 공통 동작

### 입력

다음 둘 중 하나를 넘길 수 있습니다.

- 파일 기반 DICOM 시리즈가 들어 있는 디렉터리
- 2D 이미지를 담은 단일 DICOM 파일
- stack 형태의 multiframe grayscale volume을 담은 단일 DICOM 파일

현재 예제와 가이드는 단일 파일 2D 이미지, stack 형태의 multiframe grayscale volume, file-per-slice 시리즈를 중심으로 설명합니다.
non-stack multiframe localizer나 더 복잡한 enhanced multi-frame 레이아웃이 중요한 워크플로라면 별도로 검증하는 것이 좋습니다.

디렉터리에 sidecar 파일이나 DICOM이 아닌 파일이 섞여 있으면, 브리지는
DICOM이 아닌 항목을 무시합니다. 디렉터리에 여러 `SeriesInstanceUID`가
있으면, 조용히 합치지 않고 오류를 발생시킵니다.

### Geometry

브리지는 다음 geometry 정보를 보존합니다.

- `spacing`
- `origin`
- `direction`

이 값들은 가능한 경우 DICOM orientation 및 position metadata에서 계산됩니다.

stack 형태의 3D 입력에서는 volume geometry를 물리 순서 기준으로 정규화합니다.

- 슬라이스와 프레임은 base slice normal에 projection한 값으로 정렬됩니다
- 출력의 첫 슬라이스는 projected position이 가장 작은 슬라이스입니다
- `spacing[2]`는 항상 양수로 유지됩니다
- stack 방향은 `direction`에 담습니다

파일 기반 series에 orientation이 섞여 있으면 주된 orientation 그룹만 사용하고,
localizer처럼 다른 방향의 outlier는 제외합니다.

### 출력 dtype 정책

브리지는 모든 출력을 무조건 float로 올리기보다, 실용적인 scalar type을 유지하려고 합니다.

- rescale 없음: stored dtype 유지
- `slope ~= 1` 이고 intercept가 정수: 정수 dtype 유지
- fractional rescale: `float32`로 승격

그래서 toolkit 기본 reader와 비교하면 출력 dtype이 다를 수 있습니다.
reader마다 다른 scalar type 정책을 가질 수 있기 때문입니다.

### Modality value

두 공개 API 모두 기본값은 `to_modality_value=True`입니다.

특히 PET에서는 frame별 rescale이 중요한 경우가 많습니다.
원래 저장된 값이 필요하다면 `to_modality_value=False`를 넘기면 됩니다.

### 2D 이미지

단일 파일 2D 이미지는 단일 슬라이스 이미지로 다뤄집니다.
RGB 같은 vector pixel data는 component axis를 유지합니다.

### Multiframe volume

stack 형태의 단일 파일 multiframe grayscale volume은 `(z, y, x)` 형태로 다뤄집니다.
이 경우에도 file-per-slice series와 같은 물리 순서 정규화를 적용합니다.
frame position이 하나의 stack 방향과 일치하지 않으면, 브리지는 geometry를 추정하지 않고 현재는 `NotImplementedError`를 발생시킵니다.

## SimpleITK

공개 API는 다음과 같습니다.

- `read_series_image(path, *, to_modality_value=True) -> SimpleITK.Image`
- `to_simpleitk_image(volume) -> SimpleITK.Image`

예제:

```python
from dicomsdl.simpleitk_bridge import read_series_image

image = read_series_image(
    r"..\sample\PT\00013 Torso PET AC OSEM",
    to_modality_value=True,
)

print(image.GetSize())
print(image.GetSpacing())
```

중간의 디코드된 volume을 먼저 다루고 싶다면:

```python
from dicomsdl.simpleitk_bridge import read_series_volume, to_simpleitk_image

volume = read_series_volume(r"..\sample\PT\00013 Torso PET AC OSEM")
image = to_simpleitk_image(volume)
```

반환값은 일반적인 `SimpleITK.Image`이므로, 기존 SimpleITK filter나 pipeline에 그대로 넘길 수 있습니다.

## VTK

공개 API는 다음과 같습니다.

- `read_series_image_data(path, *, to_modality_value=True, copy=False) -> vtkImageData`
- `to_vtk_image_data(volume, *, copy=False) -> vtkImageData`

예제:

```python
from dicomsdl.vtk_bridge import read_series_image_data

image = read_series_image_data(
    r"..\sample\PT\00013 Torso PET AC OSEM",
    to_modality_value=True,
    copy=False,
)

print(image.GetDimensions())
print(image.GetSpacing())
```

중간의 디코드된 volume을 먼저 다루고 싶다면:

```python
from dicomsdl.vtk_bridge import read_series_volume, to_vtk_image_data

volume = read_series_volume(r"..\sample\PT\00013 Torso PET AC OSEM")
image = to_vtk_image_data(volume, copy=False)
```

`copy=False`는 가능한 경우 zero-copy를 사용하는 빠른 경로입니다.
원본 NumPy 기반 bridge 객체와 독립된 VTK 소유 복사본이 필요하다면 `copy=True`를 사용하세요.

## Native reader와 브리지 선택

reader 클래스 자체의 API 호환성이 중요하다면 toolkit 기본 reader를 쓰는 편이 맞습니다.

다음이 중요하다면 DicomSDL bridge가 더 잘 맞습니다.

- DicomSDL이 decode와 rescale 동작을 직접 제어하는 것
- 두 toolkit에서 같은 geometry와 dtype 정책을 유지하는 것
- 양수 slice spacing을 유지한 물리 순서 정규화 3D volume이 필요한 것
- 예제나 벤치마크에서 DicomSDL과 기본 reader를 쉽게 비교하는 것

실무적으로는 다음 점을 기억하면 됩니다.

- 일반적인 SimpleITK DICOM series 경로는 이미 GDCM을 사용합니다
- VTK는 환경에 따라 `vtkDICOMImageReader` 또는 `vtkgdcm`과 비교하게 됩니다
- bridge는 volume을 노출하기 전에 stack order를 정규화하므로,
  geometry가 native reader의 저장 순서 표현과 의도적으로 다를 수 있습니다

## 예제와 노트북

이 저장소에는 바로 실행할 수 있는 예제와 노트북이 포함되어 있습니다.

- `examples/python/itk_vtk/dicomsdl_to_simpleitk_pet_volume.py`
- `examples/python/itk_vtk/dicomsdl_to_vtk_pet_volume.py`
- `tutorials/basic_vtk3d.ipynb`
- `tutorials/timeit_itk.ipynb`
- `tutorials/timeit_vtk.ipynb`
- `benchmarks/python/benchmark_pet_volume_readers.py`
- `benchmarks/python/benchmark_wg04_readers.py`
