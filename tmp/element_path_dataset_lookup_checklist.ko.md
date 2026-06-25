# ElementPath / DataSet Nested Lookup 구현 체크리스트

## 목표

- [x] DICOM nested sequence 안의 Data Element를 string path 없이 typed path로 표현한다.
- [x] public API는 `ElementPath`와 `DataSet` method 중심으로 단순하게 둔다.
- [x] `ElementPath`는 sequence item parent chain과 leaf element tag를 분리해서 표현한다.
- [ ] geometry, SR, metadata resolver가 같은 nested lookup 표현을 재사용할 수 있게 한다.
- [x] 일반 lookup 경로에서 heap allocation과 path string parsing을 피한다.
- [x] public MVP에서는 실패를 `NullElement` / falsey `DataElement`로 표현한다.
- [x] 사람이 읽는 path 문자열은 debug/log용으로만 lazy 생성한다.

## 이름과 책임

- [x] `SequenceItemStep`: parent dataset까지 내려가기 위한 `sequence_tag + item_index`.
- [x] `ElementPath`: DICOM Data Element 위치를 표현하는 owning value type. 구조는 `parents + leaf_tag`.
- [x] `ElementPathView`: `SequenceItemStep` span과 leaf tag를 가진 valid path view.
- [x] `BasicElementPath<N>`: inline capacity를 명시할 수 있는 path template.
- [x] `ElementPath = BasicElementPath<16>`: 일반 image/SEG/enhanced metadata용 기본 path.
- [x] SR처럼 더 깊은 구조는 SR 도메인에서 `using SrElementPath = BasicElementPath<32>;`처럼 별도 alias로 둔다.
- [x] `DataSet::get_dataelement(...)`: path를 실제 `DataSet` 위에서 resolve하는 기본 public API.
- [x] `DicomFile::get_dataelement(...)`: root dataset으로 forwarding하는 파일 단위 편의 API.
- [x] `DataSet::sequence_item(...)`: 단일 sequence item으로 내려가는 얇은 helper.

## 구현 전 확정 결정

- [x] `ElementPathView`의 default constructor는 invalid view를 만든다.
  - `ElementPathView::valid()`를 제공한다.
  - `get_dataelement(ElementPathView{})`는 `NullElement`를 반환한다.
  - `ElementPathView`는 owning `BasicElementPath<N>`에서 `ok()==true`일 때 만든 view 또는 명시적으로 valid한 `parents + leaf_tag` view만 정상 입력으로 본다.
- [x] `BasicElementPath<N>::ok()`는 완성된 유효 path만 true다.
  - `ElementPath{}`는 `ok()==false`.
  - `ElementPath{}.item(seq, 0)`도 leaf가 없으므로 `ok()==false`.
  - 내부적으로는 `ok_ && has_leaf_`를 반환한다.
- [x] `Tag{}` 입력은 invalid로 처리한다.
  - `item(Tag{}, i)`는 `ok_ = false`.
  - `element(Tag{})`는 `ok_ = false`.
  - leaf tag가 `(0000,0000)`인 애매한 lookup을 허용하지 않는다.
- [x] template overload는 `NullElement()`를 직접 호출하지 않는다.
  - `NullElement()`는 public header에 노출된 API가 아니므로 template 구현에서 직접 부르지 않는다.
  - invalid owning path는 `get_dataelement(ElementPathView{})`로 전달하고, non-template overload가 `NullElement`를 반환한다.
- [x] 기존 `get_dataelement(Tag)` / string path API와 일관되게 mutable/const overload를 모두 제공한다.
  - mutable overload도 missing/intermediate failure에서 새 element를 만들지 않고 `NullElement`를 반환한다.
  - 생성은 기존 `ensure_dataelement(...)` 계열의 책임으로 유지한다.

## Public API 초안

- [x] core 헤더 후보: 기존 `dicom.h` 또는 새 `dicom_element_path.h`.
- [x] namespace: `dicom`.
- [x] `ElementPath`는 `DataSet*` / `DataElement*`를 저장하지 않는다.
- [x] `DataSet` lookup method는 `ElementPathView`와 `BasicElementPath<N>` overload를 제공한다.
- [x] `DicomFile` lookup method도 같은 `ElementPathView`와 `BasicElementPath<N>` overload를 제공한다.
- [x] `ElementPathView`는 valid path만 표현한다고 명시한다.
- [x] invalid/capacity-failed owning path는 `DataSet::get_dataelement(const BasicElementPath<N>&)` overload에서 `NullElement`로 처리한다.
- [x] MVP에서는 value extraction helper를 추가하지 않고, 찾은 `DataElement`의 기존 변환 API를 사용한다.
- [ ] 값 추출 helper가 필요해지면 allocation 정책을 별도 설계한다.

```cpp
namespace dicom {

struct SequenceItemStep {
    Tag sequence_tag;
    std::uint32_t item_index = 0;
};

class ElementPathView {
public:
    constexpr ElementPathView() = default;
    constexpr ElementPathView(std::span<const SequenceItemStep> parents,
                              Tag leaf_tag) noexcept;

    constexpr bool valid() const noexcept;
    constexpr std::span<const SequenceItemStep> parents() const noexcept;
    constexpr Tag leaf_tag() const noexcept;
    constexpr std::size_t depth() const noexcept;

private:
    std::span<const SequenceItemStep> parents_;
    Tag leaf_tag_{};
};

template <std::size_t InlineSteps = 16>
class BasicElementPath {
public:
    static constexpr std::size_t kMaxInlineSteps = InlineSteps;

    constexpr BasicElementPath() = default;

    constexpr BasicElementPath& item(Tag sequence_tag,
                                     std::uint32_t item_index) noexcept;
    constexpr BasicElementPath& element(Tag leaf_tag) noexcept;

    constexpr bool ok() const noexcept;
    constexpr bool has_leaf() const noexcept;
    constexpr std::size_t depth() const noexcept;
    constexpr bool empty() const noexcept;
    constexpr ElementPathView view() const noexcept;
    constexpr std::span<const SequenceItemStep> parents() const noexcept;
    constexpr Tag leaf_tag() const noexcept;

private:
    std::array<SequenceItemStep, InlineSteps> parents_{};
    std::uint16_t parent_count_ = 0;
    Tag leaf_tag_{};
    bool has_leaf_ = false;
    bool ok_ = true;
};

using ElementPath = BasicElementPath<16>;

class DataSet {
public:
    DataElement& get_dataelement(ElementPathView path);
    const DataElement& get_dataelement(ElementPathView path) const;

    template <std::size_t N>
    DataElement& get_dataelement(
        const BasicElementPath<N>& path) {
        return get_dataelement(path.ok() ? path.view() : ElementPathView{});
    }

    template <std::size_t N>
    const DataElement& get_dataelement(
        const BasicElementPath<N>& path) const {
        return get_dataelement(path.ok() ? path.view() : ElementPathView{});
    }

    DataSet* sequence_item(Tag sequence_tag,
                           std::uint32_t item_index);
    const DataSet* sequence_item(Tag sequence_tag,
                                 std::uint32_t item_index) const;
};

class DicomFile {
public:
    DataElement& get_dataelement(ElementPathView path);
    const DataElement& get_dataelement(ElementPathView path) const;

    template <std::size_t N>
    DataElement& get_dataelement(const BasicElementPath<N>& path) {
        return dataset().get_dataelement(path);
    }

    template <std::size_t N>
    const DataElement& get_dataelement(const BasicElementPath<N>& path) const {
        return dataset().get_dataelement(path);
    }
};

} // namespace dicom
```

## 사용 예

```cpp
auto path = dicom::ElementPath{}
    .item("PerFrameFunctionalGroupsSequence"_tag, frame_index)
    .item("PETFrameTypeSequence"_tag, 0)
    .element("VolumetricProperties"_tag);

const DataElement& elem = dataset.get_dataelement(path);
if (elem.is_missing()) {
    // Not found, invalid path, or not loaded/selected.
}
```

## 성능 계약

- [x] `ElementPath` 생성/복사는 fixed inline storage 안에서 수행하고 heap allocation을 하지 않는다.
- [x] `ElementPathView`는 parent span + leaf tag 수준의 trivial-copy view로 유지한다.
- [x] `SequenceItemStep`은 `sequence_tag + item_index`만 저장한다.
- [x] 큰 sequence item index는 depth를 늘리지 않는다. 예: `ContentSequence[4999]`도 path step 하나다.
- [x] `DataSet::get_dataelement(ElementPath...)` / `sequence_item(...)`은 string parsing과 heap allocation을 하지 않는다.
- [x] `DataSet::get_dataelement(ElementPath...)`는 실패 시 `NullElement`를 반환하는 빠른 경로다.
- [x] `get_dataelement(ElementPath...)`의 실패 판정은 기존 DicomSDL 스타일에 맞춰 `element.is_missing()` / falsey element로 한다.
- [x] MVP에서는 value extraction helper를 추가하지 않고, hot path에서는 `get_dataelement()` 후 직접 `DataElement` API를 사용한다.
- [x] debug string 변환은 lookup path에서 호출하지 않는다.
- [x] `ElementPath` resolver는 매 호출 root부터 sequence item chain을 따라간다. 수천 frame 반복 metadata 접근에서는 domain reader가 `sequence_item()`으로 per-frame item `DataSet*`를 캐시한 뒤 leaf lookup을 반복한다.
- [ ] 수천 frame 반복 metadata 접근은 `FrameGeometryReader`처럼 caller-specific reader가 sequence pointer를 캐시해서 처리한다.

## 구현 체크리스트

- [x] `SequenceItemStep` 정의
- [x] `ElementPathView` 정의
- [x] `BasicElementPath<N>` 정의
- [x] `ElementPath = BasicElementPath<16>` alias 추가
- [x] core public API에는 `SrElementPath` alias를 추가하지 않는다.
- [x] `ElementPathView::valid()` 추가
- [x] `DataSet::get_dataelement(ElementPathView)` 추가
- [x] `DataSet::get_dataelement(ElementPathView) const` 추가
- [x] `DataSet::get_dataelement(const BasicElementPath<N>&)` template overload 추가
- [x] `DicomFile::get_dataelement(ElementPathView)` forwarding 추가
- [x] `DicomFile::get_dataelement(ElementPathView) const` forwarding 추가
- [x] `DicomFile::get_dataelement(const BasicElementPath<N>&)` template forwarding 추가
- [x] `DataSet::sequence_item(Tag, std::uint32_t)` 추가
- [x] value extraction helper는 MVP에서 제외한다.
- [x] `ElementPath` capacity 초과 시 `ok()==false`가 되도록 구현
- [x] `element()` 없이 leaf tag가 없는 path는 `ok()==false`가 되도록 한다.
- [x] `element()` 호출 뒤 다시 `item()`을 호출하거나 `element()`를 두 번 호출하면 `ok()==false`가 되도록 한다.
- [x] `item(Tag{}, index)` / `element(Tag{})`는 `ok()==false`가 되도록 한다.
- [x] template overload는 invalid path에서 `NullElement()`를 직접 호출하지 않고 `ElementPathView{}`를 non-template overload로 넘긴다.
- [x] invalid/capacity-failed path를 `get_dataelement()`에 넘기면 `NullElement`를 반환한다.
- [x] `ElementPath` debug string helper는 별도 함수로 두고 lookup 경로에서 호출하지 않는다.

## Test Plan

- [x] `ElementPath{}.element(tag)`가 leaf element path를 표현하는지 검증
- [x] `ElementPath{}.item(seq, i).element(tag)`가 sequence item 경로를 표현하는지 검증
- [x] `ElementPath{}.element(a).element(b)`가 invalid path가 되는지 검증
- [x] `ElementPath{}.element(a).item(seq, i)`가 invalid path가 되는지 검증
- [x] `ElementPath{}`와 `ElementPath{}.item(seq, i)`는 `ok()==false`인지 검증
- [x] `item(Tag{}, i)`와 `element(Tag{})`는 `ok()==false`인지 검증
- [x] `item_index`가 큰 값이어도 path depth가 증가하지 않는지 검증
- [ ] `ElementPath` 생성/복사/view 변환이 allocation 없이 수행되는지 검증
- [x] `BasicElementPath<1>`에서 capacity 초과 시 `ok()==false`가 되는지 검증
- [x] `BasicElementPath<32>`가 16 step보다 깊은 path를 표현할 수 있는지 검증
- [x] `get_dataelement(ElementPath...)`가 정상 nested element를 찾는지 검증
- [x] `DicomFile::get_dataelement(ElementPath...)`가 root dataset으로 forwarding하는지 검증
- [x] `get_dataelement(ElementPathView{})`가 `NullElement`를 반환하는지 검증
- [x] mutable `get_dataelement(ElementPath...)`도 missing/intermediate failure에서 새 element를 만들지 않고 `NullElement`를 반환하는지 검증
- [x] `NullElement`에 쓰기성 API를 시도해도 sentinel이 오염되지 않는지 검증
- [x] missing element는 `NullElement` / `is_missing()`으로 표현되는지 검증
- [x] sequence가 아닌 element에 item step을 적용하면 `NullElement`로 표현되는지 검증
- [x] item index 범위를 벗어나면 `NullElement`로 표현되는지 검증
- [x] invalid/capacity-failed path는 `NullElement`로 표현되는지 검증
- [ ] `get_dataelement(ElementPath...)` 반복 호출이 allocation 없이 동작하는지 검증
- [x] debug string helper가 lookup 경로에서 호출되지 않는지 code review로 확인
- [ ] geometry의 `VolumetricPropertiesInfo::source`가 `ElementPath`로 source location을 보존하는지 검증

## Assumptions

- [ ] 이 문서는 구현 전 작업 메모이므로 `tmp/` 아래에 둔다.
- [ ] `ElementPath`는 DICOM file path가 아니라 DICOM Data Element path다.
- [ ] public API는 우선 `ElementPath + DataSet method` 중심으로 유지한다.
- [ ] SR 전용 path alias는 SR 구현/문서에서 필요할 때 별도로 둔다.
- [ ] 성능이 더 필요한 반복 frame metadata 접근은 geometry/SR 등 각 도메인 reader가 별도 캐시를 갖는다.
