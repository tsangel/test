# DICOM Segmentation Storage 지원 체크리스트

이 메모는 DicomSDL에서 DICOM Segmentation Storage를 지원할 때의 범위와
API 설계 방향을 정리한 문서다. 기준 샘플은 아래 폴더다.

`C:\Lab\img\dicom.sample\FDG-FBB-sample-NCM\FBB\dcm\seg\2a8c5aa325f04cf485ed46098076b3a8`

샘플 폴더에는 SEG 인스턴스가 2개 있다.

- `SEG on CT`
- `SEG on PT`

두 파일 모두 `Segmentation Storage` 객체다.

- SOP Class UID: `1.2.840.10008.5.1.4.1.1.66.4`
- Modality: `SEG`
- Segmentation Type: `BINARY`
- Rows x Columns: `256 x 256`
- Number of Frames: `2885`
- Number of Segments: `97`
- Bits Allocated / Stored / High Bit: `1 / 1 / 0`
- 디코딩 후 pixel 배열 shape: `(2885, 256, 256)`
- 디코딩 후 pixel 값: `0` 또는 `1`
- Raw PixelData 길이: `frames * rows * columns / 8`

두 파일은 segment 정의와 실제 mask 데이터가 동일하다. 주된 차이는 참조하는
source series가 하나는 CT, 하나는 PT라는 점이다. 이 source 차이는 provenance로
보고, 실제 overlay 가능성은 `FrameOfReferenceUID`와 geometry로 판단한다.

## 기본 모델

- [x] SEG는 multi-frame image 객체로 취급한다.
- [x] `BINARY` 또는 `FRACTIONAL` SEG에서 `PixelData` 값을 label 번호로 취급하지 않는다.
- [x] 각 frame은 특정 segment에 대한 하나의 2D mask 또는 fractional map으로 해석한다.
- [x] BINARY/FRACTIONAL SEG에서 하나의 frame은 하나의 `ReferencedSegmentNumber`만 가진다.
- [ ] 같은 spatial slice 위치에 여러 segment frame이 존재할 수는 있지만, 한 frame이 여러 segment를 직접 담는 구조로 보지 않는다.
- [x] LABELMAP SEG에서는 한 frame 안에 여러 segment pixel value가 있을 수 있으나 MVP 범위에서 제외한다.
- [x] frame과 segment의 연결은 아래 태그로 해석한다.
  - `PerFrameFunctionalGroupsSequence`
  - `SegmentIdentificationSequence`
  - `ReferencedSegmentNumber`
- [x] segment metadata는 아래 태그로 해석한다.
  - `SegmentSequence`
  - `SegmentNumber`
  - `SegmentLabel`
  - `SegmentDescription`
  - `SegmentAlgorithmType`
  - `SegmentAlgorithmName`
  - `SegmentedPropertyCategoryCodeSequence`
  - `SegmentedPropertyTypeCodeSequence`
  - `AnatomicRegionSequence`
  - `RecommendedDisplayCIELabValue`
- [ ] frame별 geometry는 아래 태그로 해석한다.
  - `PlanePositionSequence`
  - `ImagePositionPatient`
  - `PlaneOrientationSequence`
  - `ImageOrientationPatient`
  - `PixelMeasuresSequence`
  - `PixelSpacing`
  - `SliceThickness`
  - `SpacingBetweenSlices`
- [ ] source image reference는 아래 태그로 해석한다.
  - `ReferencedSeriesSequence`
  - `ReferencedInstanceSequence`
  - `DerivationImageSequence`
  - `SourceImageSequence`

## 공간 좌표계와 overlay 기준

SEG를 특정 source image에만 붙어 있는 mask로 보지 않는다. 실제 display나
processing에서는 원래 참조한 CT/PET이 아니라 같은 환자 좌표계에 있는 다른
image 위에 overlay할 수 있어야 한다.

- [x] 공간 호환성의 1차 기준은 `FrameOfReferenceUID (0020,0052)`이다.
- [x] 하나의 SEG instance는 하나의 `FrameOfReferenceUID`를 가진다고 모델링한다.
- [x] `FrameOfReferenceUID`는 `VM 1`이므로 frame마다 다른 frame of reference를 갖는 구조로 API를 만들지 않는다.
- [x] 여러 `ReferencedSeriesSequence` item을 가질 수 있어도 SEG mask 자체의 공간 좌표계는 하나로 본다.
- [x] frame별 `SourceImageSequence`는 provenance/retrieve/debug 용도로 취급하고, overlay 대상 선택의 필수 조건으로 만들지 않는다.
- [ ] 같은 `FrameOfReferenceUID`를 가진 image라면 SEG가 직접 reference하지 않은 image 위에도 overlay 가능한 후보로 본다.
- [ ] 실제 overlay/resampling에는 `ImagePositionPatient`, `ImageOrientationPatient`, `PixelSpacing` 등 geometry도 함께 사용한다.
- [ ] target image와 SEG의 `FrameOfReferenceUID`가 다르면 direct overlay는 금지하고 registration 정보가 필요한 별도 단계로 둔다.
- [ ] `FrameOfReferenceUID`가 없는 특수한 SEG는 source image와 동일 sampling/extent라는 DICOM 조건을 따르는 fallback case로 다루며, 초기 API에서는 보수적으로 metadata를 노출만 한다.

## 지원 범위

### 1차 목표: 읽기 지원

- [x] Segmentation Storage SOP Class UID를 인식한다.
  - `1.2.840.10008.5.1.4.1.1.66.4`
- [x] `SegmentationType`을 파싱한다.
- [x] `BINARY`를 지원한다.
- [x] `FRACTIONAL`을 지원한다.
- [x] `SegmentationFractionalType`과 `MaximumFractionalValue`를 instance-level metadata로 파싱한다.
- [x] `PixelData`를 디코딩하지 않고 segment metadata를 조회할 수 있게 한다.
- [x] 전체 frame을 디코딩하지 않고 frame metadata를 조회할 수 있게 한다.
- [x] BINARY의 1-bit packed pixel data를 디코딩한다.
- [x] FRACTIONAL의 8-bit pixel data를 디코딩한다.
- [x] caller가 제공한 buffer로 frame 단위 디코딩을 지원한다.
- [x] 저장된 frame 순서는 보존하되, 이것이 공간 순서라고 가정하지 않는다.
- [x] geometry metadata는 frame 단위로 노출하되, geometry 기준 정렬과 3D volume 재구성은 MVP에서 제외한다.

### 2차 목표: frame 단위 편의 API

- [x] segment number에서 segment index로 가는 lookup을 만든다.
- [x] frame index에서 referenced segment number로 가는 lookup을 만든다.
- [x] `segment_frame_count(segment_number)`는 pixel decode 없이 제공한다.
- [x] `frames_for_segment(segment_number)`는 pixel decode 없이 `SegmentFrameListView`를 반환한다.
- [x] frame index 단위 2D decode API를 제공한다.
- [x] `segment_number`만으로 전체 mask를 반환하는 API는 MVP에서 제외한다.

### MVP 이후: volume/derived API

- [ ] `get_segment_mask(segment_number)` 또는 C++에 맞는 segment 단위 mask API를 검토한다.
- [ ] `get_segment_volume(segment_number)` 또는 C++에 맞는 volume API를 검토한다.
- [ ] volume 재구성 시 누락된 spatial slice를 명시적으로 처리한다.
- [ ] 누락 slice 처리 정책을 API 옵션으로 드러낸다.
- [ ] overlap을 고려한 label map 변환 API를 제공한다.
- [ ] overlapping BINARY/FRACTIONAL segment를 label map으로 변환할 때 손실 가능성을 알린다.

### 3차 목표: 쓰기 지원

- [ ] BINARY Segmentation Storage를 쓴다.
- [ ] FRACTIONAL Segmentation Storage를 쓴다.
- [ ] BINARY `PixelData`를 1-bit로 정확히 packing한다.
- [ ] FRACTIONAL `PixelData`를 8-bit로 정확히 쓴다.
- [ ] 유효한 `SegmentSequence`를 생성한다.
- [ ] 유효한 `SharedFunctionalGroupsSequence`를 생성한다.
- [ ] 유효한 `PerFrameFunctionalGroupsSequence`를 생성한다.
- [ ] 유효한 `DimensionIndexSequence`를 생성한다.
- [ ] source image reference를 생성한다.
- [ ] `FrameOfReferenceUID`와 geometry 일관성을 검증한다.
- [ ] 가능하면 source image와 동일한 `FrameOfReferenceUID`를 쓰고, 다른 frame of reference를 참조해야 하면 Spatial Registration 같은 별도 객체가 필요하다는 점을 문서화한다.
- [ ] Label Map Segmentation Storage는 별도 범위로 둔다.
  - SOP Class UID: `1.2.840.10008.5.1.4.1.1.66.7`

## 초기 범위에서 제외할 것

- [ ] Surface Segmentation Storage는 초기 범위에 넣지 않는다.
- [ ] RTSTRUCT 변환은 초기 범위에 넣지 않는다.
- [ ] registration/resampling 자동 보정은 초기 범위에 넣지 않는다.
- [ ] rendering/viewer 기능은 초기 범위에 넣지 않는다.
- [ ] WSI tiled SEG 최적화는 초기 범위에 넣지 않는다.
- [ ] segment 단위 3D volume mask 재구성은 초기 범위에 넣지 않는다.
- [ ] `z_range`, `spatial_extent` 같은 derived extent 값은 초기 범위에 넣지 않는다.
- [ ] label map 변환은 초기 범위에 넣지 않는다.

## C++ API 형태

C++ 코어는 zero-copy view API를 기본으로 둔다. 다만 higher-level 객체인
`Segmentation` 자체는 `DicomFile`을 소유하는 heap object로 둔다. Python
binding이나 장기 보관이 필요한 경우에는 owning snapshot API를 별도로 제공한다.

### 모듈 경계와 ownership

SEG 지원은 기존 `dicom.h`에 직접 넣지 않고 별도 public header로 분리한다.

```cpp
#include "dicom.h"      // core DICOM read/parse/pixel API
#include "dicom_seg.h"  // optional SEG interpretation API
```

기본 namespace는 `dicom::seg`로 둔다.

```cpp
namespace dicom {

[[nodiscard]] std::unique_ptr<DicomFile>
read_file(const std::filesystem::path& path,
          ReadOptions options = {});

[[nodiscard]] std::unique_ptr<DicomFile>
read_bytes(const std::uint8_t* data,
           std::size_t size,
           ReadOptions options = {});

} // namespace dicom

namespace dicom::seg {

class Segmentation;

struct Options {
    bool allow_partial_source{false};
    bool validate_required_modules{true};
};

[[nodiscard]] bool is_segmentation_storage(const DicomFile& file) noexcept;
[[nodiscard]] bool is_segmentation_storage(const DataSet& ds) noexcept;

[[nodiscard]] std::unique_ptr<Segmentation>
from_dicomfile(std::unique_ptr<DicomFile> file,
               const Options& options = {});

} // namespace dicom::seg
```

설계 규칙:

- [x] `read_file`과 `read_bytes`는 core `dicom` namespace가 책임진다.
- [x] SEG module은 이미 읽힌 `DicomFile`을 SEG 객체로 해석하는 책임만 가진다.
- [x] public API 이름은 `open`/`try_open`보다 `from_dicomfile`을 우선한다.
- [x] `try_open`/`nullptr on failure` 정책은 만들지 않는다.
- [x] SEG 여부 확인은 `is_segmentation_storage(...)`로 한다.
- [x] `from_dicomfile(...)`은 `nullptr`, non-SEG, 필수 metadata 오류에서 exception을 던진다.
- [x] `DicomFile::has_error()`가 true이고 `allow_partial_source == false`이면 `from_dicomfile(...)`은 실패한다.
- [x] `ReadOptions.keep_on_error`의 의미는 `read_file/read_bytes`에만 둔다.
- [x] `Segmentation`은 `std::unique_ptr<DicomFile>`을 소유한다.
- [x] `from_dicomfile(...)`은 `std::unique_ptr<Segmentation>`을 반환한다.
- [x] 첫 public API에서는 borrowed `view(const DicomFile&)`를 제공하지 않는다.
- [ ] 필요해지면 나중에 advanced API로 `borrow(...)` 같은 이름을 고려한다.

### 네이밍 메모

`from_dicomfile`은 일부러 `from_dicom_file`이 아니라 붙여 쓴다.

배경:

- [ ] `DicomFile`은 DICOM 표준의 공식 객체명이라기보다 DicomSDL의 file/session wrapper class 이름이다.
- [ ] `is_dicom_file(path)`에서 `dicom_file`은 DICOM File Format 또는 filesystem file 개념을 뜻한다.
- [x] `from_dicomfile(std::unique_ptr<DicomFile>)`에서 `dicomfile`은 DicomSDL의 `DicomFile` class object를 뜻한다.
- [x] 이름을 붙여 씀으로써 path-level DICOM file format API와 class-object 변환 API를 구분한다.
- [ ] 기존 DicomSDL 코드에서는 `dataset`, `dataelement`를 한 단어처럼 쓰는 API가 이미 있다.
- [ ] 반면 `read_file`, `is_dicom_file`, `write_file`처럼 filesystem/file-format 의미의 `file`은 `_file`로 분리되어 있다.
- [ ] 그래서 `from_dicom_file(...)`은 `read_file(...)` 계열의 파일 포맷/경로 API처럼 읽힐 여지가 있다.
- [x] `from_dicomfile(...)`은 입력 타입이 DicomSDL의 `DicomFile` class라는 점을 이름에서 드러내는 쪽으로 선택한다.
- [ ] 나중에 `DataSet` 기반 API가 생기면 `from_dataset(...)`을 쓴다.
- [ ] `from_dataset(...)`은 DICOM 커뮤니티와 여러 라이브러리에서 자연스럽게 쓰이는 `Dataset` 표기와도 잘 맞는다.
- [ ] highdicom의 `from_dataset(...)`, DCMTK의 `loadDataset(...)`/`read(...)` 같은 사례는 "이미 읽힌 DICOM 객체를 더 높은 수준의 객체로 해석한다"는 방향을 참고할 만하다.
- [x] 다만 DicomSDL에서는 core `read_file/read_bytes`가 이미 `std::unique_ptr<DicomFile>`을 반환하므로, 첫 API는 소유권 이전이 명확한 `from_dicomfile(std::unique_ptr<DicomFile>)`로 둔다.

예:

```cpp
auto seg = dicom::seg::from_dicomfile(dicom::read_file(path));
auto sr  = dicom::sr::from_dicomfile(dicom::read_file(path));
auto pm  = dicom::pm::from_dicomfile(dicom::read_file(path));

if (dicom::is_dicom_file(path)) {
    auto file = dicom::read_file(path);
    auto seg = dicom::seg::from_dicomfile(std::move(file));
}
```

### 기본 진입점

```cpp
class Segmentation final {
public:
    Segmentation(const Segmentation&) = delete;
    Segmentation& operator=(const Segmentation&) = delete;
    Segmentation(Segmentation&&) = delete;
    Segmentation& operator=(Segmentation&&) = delete;

    [[nodiscard]] bool is_valid() const noexcept;
    [[nodiscard]] SegmentationType segmentation_type() const;
    [[nodiscard]] SegmentationFractionalType fractional_type() const noexcept;
    [[nodiscard]] std::optional<std::uint16_t>
    maximum_fractional_value() const noexcept;
    [[nodiscard]] std::optional<std::string_view>
    frame_of_reference_uid() const noexcept;

    [[nodiscard]] const DataSet&
    shared_functional_groups_item() const;

    [[nodiscard]] SegmentListView segments() const;
    [[nodiscard]] SegmentFrameListView frames() const;

    [[nodiscard]] std::optional<SegmentView>
    segment_by_number(std::uint16_t segment_number) const;

    [[nodiscard]] SegmentFrameListView
    frames_for_segment(std::uint16_t segment_number) const;

    [[nodiscard]] std::size_t
    segment_frame_count(std::uint16_t segment_number) const;

    // MVP pixel API는 frame 단위 decode까지만 제공한다.
    void decode_frame_into(std::size_t frame_index, std::span<std::uint8_t> out) const;

private:
    std::unique_ptr<DicomFile> file_;
    SegmentationIndex index_;
};
```

공간 관련 API 규칙:

- [x] `frame_of_reference_uid()`는 SEG의 spatial compatibility를 판단하는 핵심 accessor로 둔다.
- [x] `referenced_series()`나 `source_images()`가 추가되더라도 provenance 성격의 보조 API로 둔다.
- [ ] `can_overlay_directly(seg, image)` 같은 helper를 만든다면 기본 판정은 `FrameOfReferenceUID` 일치 여부로 시작한다.
- [ ] helper는 이름 그대로 "direct" overlay 가능성만 판단하고, registration/resampling 성공 여부까지 보장하지 않는다.

Functional Groups 원시 접근 규칙:

- [x] `SharedFunctionalGroupsSequence (5200,9229)`에 대응되는 accessor는 `shared_functional_groups_item()`로 둔다.
- [x] `PerFrameFunctionalGroupsSequence (5200,9230)`의 frame별 item에 대응되는 accessor는 `per_frame_functional_groups_item()`로 둔다.
- [x] DICOM keyword는 sequence attribute에만 있고 sequence item 자체에는 keyword가 없으므로, API 이름에 `_item`을 붙인다.
- [x] `shared_functional_groups_item()`은 SEG instance 전체에 공통인 functional group macro들의 item dataset을 반환한다.
- [x] `SharedFunctionalGroupsSequence`는 하나의 item만 가지므로 sequence view보다 item accessor가 더 실용적이다.
- [x] `per_frame_functional_groups_item()`은 현재 frame rank에 해당하는 item dataset을 반환한다.
- [x] geometry accessor는 per-frame item을 먼저 보고, 없으면 shared item을 fallback으로 본다.
- [x] raw item accessor는 전용 accessor가 아직 없는 DICOM attribute를 꺼내기 위한 escape hatch로 둔다.
- [x] raw item accessor는 필수 item이 없으면 exception을 던질 수 있으므로 `noexcept`로 만들지 않는다.

Segmentation type 규칙:

- [x] `SegmentationType`은 instance-level 값이다.
- [x] `SegmentationFractionalType`과 `MaximumFractionalValue`도 FRACTIONAL SEG의 instance-level 값으로 취급한다.
- [x] segment별 또는 frame별 `segmentation_type()` accessor는 만들지 않는다.
- [x] `SegmentationType::labelmap` enum 값은 future/post-MVP 인식을 위해 둘 수 있지만, Label Map Segmentation Storage decode는 MVP 범위에 넣지 않는다.

### SegmentListView

```cpp
class SegmentListView {
public:
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;

    [[nodiscard]] SegmentView operator[](std::size_t index) const;

    [[nodiscard]] SegmentIterator begin() const;
    [[nodiscard]] SegmentIterator end() const;
};
```

### SegmentFrameListView

`SegmentFrameListView`는 전체 frame 목록과 특정 segment에 속한 frame 목록을 같은
타입으로 표현한다. public API에서는 frame index 배열을 직접 노출하지 않는다.

```cpp
class SegmentFrameListView {
public:
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;

    [[nodiscard]] SegmentFrameView operator[](std::size_t index) const;

    [[nodiscard]] SegmentFrameIterator begin() const;
    [[nodiscard]] SegmentFrameIterator end() const;

private:
    const Segmentation* segmentation_{nullptr};

    // nullptr이면 전체 frame list, 값이 있으면 Segmentation이 소유한
    // 특정 segment의 frame index list. 빈 vector는 segment에 매칭되는
    // frame이 없다는 뜻일 수 있다.
    const std::vector<std::size_t>* frame_indices_{nullptr};
};
```

구현 메모:

- [x] `Segmentation::frames()`는 전체 frame에 대한 `SegmentFrameListView`를 반환한다.
- [x] `Segmentation::frames_for_segment(number)`는 해당 segment에 속한 frame만 보는 `SegmentFrameListView`를 반환한다.
- [x] 특정 segment view의 내부 표현은 `segment_number -> frame-index list` index를 사용한다.
- [x] `SegmentFrameView::index()`는 항상 원본 SEG instance의 frame index를 반환한다.
- [x] `frames_for_segment(number)`로 얻은 view의 `operator[]` index는 filtered list 안의 ordinal이고, `SegmentFrameView::index()`와 다를 수 있다.
- [ ] `SegmentView::frames()`는 편리하지만 parent `Segmentation` pointer가 필요하므로 MVP 이후 편의 API로 검토한다.

### SegmentView

`SegmentView`는 작고 cheap한 non-owning view여야 한다. 문자열이나 pixel data를
소유하지 않는다. `SegmentSequence`의 item dataset을 참조하고, 자주 쓰이는 작은
값인 `SegmentNumber` 정도만 캐시한다.

```cpp
class SegmentView {
public:
    [[nodiscard]] std::uint16_t number() const noexcept;

    // 가능한 경우 borrowed view를 반환한다. 반환된 string_view는 owning
    // DicomFile/DataSet/Segmentation이 살아 있고 변경되지 않는 동안만 유효하다.
    [[nodiscard]] std::string_view label() const;
    [[nodiscard]] std::string_view description() const;
    [[nodiscard]] std::string_view algorithm_name() const;

    [[nodiscard]] SegmentAlgorithmType algorithm_type() const;

    [[nodiscard]] std::optional<CodeView> property_category() const;
    [[nodiscard]] std::optional<CodeView> property_type() const;
    [[nodiscard]] std::optional<CodeView> anatomic_region() const;

    [[nodiscard]] std::optional<std::array<std::uint16_t, 3>>
    recommended_display_cielab() const;

    // 아직 전용 accessor가 없는 SEG attribute를 위한 escape hatch.
    [[nodiscard]] const DataSet& dataset() const noexcept;

private:
    const DataSet* item_{nullptr};
    std::uint16_t number_{0};
};
```

구현 메모:

- [x] `const DataSet* item_`과 cached `number_`를 기본으로 한다.
- [x] profiling으로 필요성이 증명되기 전까지 field별 `const DataElement*` 캐시는 피한다.
- [x] 기본 C++ view에서는 `std::string`을 소유하지 않는다.
- [x] 문자열 필드는 `item_`에서 lazy하게 읽는다.
- [x] C++ fast path에서는 `std::string_view`를 반환한다.
- [ ] 원본 dataset보다 오래 살아야 하는 API에는 owning snapshot을 제공한다.

### Owning Segment Snapshot

Python binding, 외부 저장, 원본 DICOM dataset과 독립된 lifetime이 필요한 경우에
사용한다.

```cpp
struct SegmentInfo {
    std::uint16_t number{};
    std::string label;
    std::string description;
    SegmentAlgorithmType algorithm_type{SegmentAlgorithmType::unknown};
    std::string algorithm_name;
    std::optional<Code> property_category;
    std::optional<Code> property_type;
    std::optional<Code> anatomic_region;
    std::optional<std::array<std::uint16_t, 3>> recommended_display_cielab;
};

class Segmentation {
public:
    [[nodiscard]] std::vector<SegmentInfo> segment_infos() const;
};
```

### CodeView와 Code

```cpp
struct CodeView {
    std::string_view value;
    std::string_view scheme_designator;
    std::string_view scheme_version;
    std::string_view meaning;
};

struct Code {
    std::string value;
    std::string scheme_designator;
    std::string scheme_version;
    std::string meaning;
};
```

### SegmentFrameView

frame metadata는 segment metadata와 분리한다. segment는 label 정의이고, frame은
저장된 하나의 2D mask/fractional map이다. MVP의 BINARY/FRACTIONAL 모델에서
`SegmentFrameView` 하나는 하나의 `referenced_segment_number()`를 가진다.

```cpp
class SegmentFrameView {
public:
    [[nodiscard]] std::size_t index() const noexcept;
    [[nodiscard]] std::uint16_t referenced_segment_number() const;

    [[nodiscard]] std::optional<std::array<double, 3>>
    image_position_patient() const;

    [[nodiscard]] std::optional<std::array<double, 6>>
    image_orientation_patient() const;

    [[nodiscard]] std::optional<std::array<double, 2>>
    pixel_spacing() const;

    [[nodiscard]] std::optional<double> slice_thickness() const;

    // provenance/retrieve/debug 용도. overlay의 1차 기준으로 쓰지 않는다.
    [[nodiscard]] SourceImageRefListView source_images() const;

    [[nodiscard]] const DataSet&
    per_frame_functional_groups_item() const;
};
```

### SourceImageRefView

source image reference는 overlay의 1차 기준이 아니라 provenance/retrieve/debug
metadata다. MVP에서 제공하더라도 가볍게 읽을 수 있는 reference view로 둔다.

```cpp
class SourceImageRefView {
public:
    [[nodiscard]] std::string_view sop_class_uid() const;
    [[nodiscard]] std::string_view sop_instance_uid() const;

    // source가 multi-frame image이고 일부 frame만 참조할 때만 값이 있다.
    [[nodiscard]] std::span<const std::uint32_t>
    referenced_frame_numbers() const;

    [[nodiscard]] const DataSet& dataset() const noexcept;
};

class SourceImageRefListView {
public:
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] SourceImageRefView operator[](std::size_t index) const;
};
```

### Type과 enum

```cpp
enum class SegmentationType : std::uint8_t {
    unknown,
    binary,
    fractional,
    labelmap
};

enum class SegmentationFractionalType : std::uint8_t {
    none,
    probability,
    occupancy,
    unknown
};

enum class SegmentAlgorithmType : std::uint8_t {
    unknown,
    automatic_,
    semiautomatic,
    manual
};
```

`automatic_`처럼 C++ keyword나 platform macro와 충돌하지 않는 이름을 쓴다.

## 샘플 기준 기대 동작

샘플 폴더의 SEG 파일에서 `seg->segments()`는 97개의 segment 정의를 노출해야 한다.

예:

```cpp
auto seg = dicom::seg::from_dicomfile(dicom::read_file(path));
auto segments = seg->segments();

segments.size();                // 97

auto first = segments[0];
first.number();                 // 1
first.label();                  // "Left-Cerebral-White-Matter"
first.description();            // "Left-Cerebral-White-Matter"
first.algorithm_type();         // SegmentAlgorithmType::automatic_
first.algorithm_name();         // "NCM-Brain"

auto last = segments[96];
last.number();                  // 97
last.label();                   // "Right-Insula"
```

샘플 segment 구성:

- [ ] `1-17`: left-side brain structures와 CSF 관련 구조
- [ ] `18-31`: right-side counterpart
- [ ] `32`: `WM-hypointensities`
- [ ] `33-35`: `Midbrain`, `Pons`, `Medulla`
- [ ] `36-66`: left cortical regions
- [ ] `67-97`: right cortical regions

## 비용 경계

- [x] `dicom::seg::from_dicomfile(...)`은 metadata index 구성까지만 수행하고 pixel decode는 하지 않는다.
- [x] `seg->segments()`는 cheap해야 한다.
- [x] `SegmentView::label()` 같은 accessor는 lazy parsing을 해도 되지만 pixel decode는 하지 않는다.
- [x] `seg->frames()`는 metadata-only여야 한다.
- [x] `segment_frame_count()`는 metadata-only여야 한다.
- [x] `voxel_count`는 pixel decode가 필요하므로 기본 `SegmentView`에 넣지 않는다.
- [x] MVP의 pixel decode는 frame-by-frame으로 제한한다.
- [x] `z_range`, `spatial_extent`, `voxel_count` 같은 derived 값은 MVP API에 넣지 않는다.
- [x] volume reconstruction은 MVP 이후의 명시적 API로 둔다.
- [x] label-map 변환은 segment overlap이 있으면 손실될 수 있으므로 MVP 이후의 명시적 API로 둔다.

## Python binding 권장 형태

Python 쪽은 lifetime 문제가 덜 생기도록 owning API를 기본으로 제공한다.

```python
seg = dicomsdl.seg.from_dicomfile(dicomsdl.read_file(path))
# 또는 Python 편의 wrapper:
seg = dicomsdl.seg.read_file(path)

seg.segments
# list[SegmentInfo]

seg.frames
# list[SegmentFrameInfo] 또는 lazy sequence

seg.decode_frame(frame_index)
# numpy 2D array, frame 단위 명시적 할당
```

체크리스트:

- [ ] Python에서도 C++와 같은 의미로 `from_dicomfile(...)`을 쓴다.
- [ ] Python 편의 API로 `dicomsdl.seg.read_file(...)`을 제공할지 검토한다.
- [ ] Python 편의 API가 생기더라도 내부적으로는 core `dicomsdl.read_file(...)`과 같은 read option/error model을 따른다.
- [ ] Python의 `SegmentInfo`는 Python-owned string을 반환한다.
- [ ] Python 사용자에게 raw `std::string_view` lifetime을 노출하지 않는다.
- [ ] MVP Python pixel API는 frame index 단위 2D decode까지만 제공한다.
- [ ] pixel allocation은 명시적인 API에서만 수행한다.
- [ ] `get_segment_mask(segment_number)`와 `get_segment_volume(segment_number)`는 MVP 이후 API로 둔다.
- [ ] 매우 큰 SEG 객체를 위해 lazy Python sequence wrapper를 고려한다.
