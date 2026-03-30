# DataSet Visit and Walk

`DataSet.visit()`와 `DicomFile.visit()`는 C++에서 쓰는 callback-style fast
path로, 루트 dataset과 모든 nested sequence item dataset을 depth-first
preorder로 순회합니다.

`DataSet.walk()`와 `DicomFile.walk()`는 같은 트리를 iterator-style로
순회하는 API입니다.

visit/walk 모두 `SQ` data element 자체를 먼저 방문한 뒤 그 아래 item으로
내려갑니다. 각 step은 다음 두 값을 제공합니다.

- ancestors-only path view
- 현재 `DataElement`

즉 nested metadata inspection, 필요한 부분만 골라 건너뛰기, 그리고 UID rewrite 같은
transform-style pass의 기반으로 쓰기 좋습니다.

`visit()`와 `walk()`는 모두 이미 로드된 dataset 상태까지만 순회합니다.
암묵적으로 `ensure_loaded()`나 `ensure_dataelement()`를 호출하지 않습니다.

partial-load된 attached dataset에서는 뒤쪽 태그들이 순회에서 조용히 빠질 수
있습니다. 전체 dataset을 검사해야 하는 pass라면 먼저 fully load하거나,
visit/walk 전에 필요한 frontier까지 `ensure_loaded(tag)`를 호출해야
합니다.

## 어떤 순서로 방문하나

순회 순서는 다음과 같습니다.

1. 현재 `DataElement`
2. 그 element가 `SQ`이면 nested item dataset들을 순서대로 방문
3. 나머지 sibling elements 방문

즉 `SQ` element는 자식보다 먼저 방문되므로, 그 지점이 자연스러운 skip
지점이 됩니다.

## C++ visit

C++ 코드에서는 iterator object가 필요 없으면 `visit()`부터 보는 편이
자연스럽습니다.

### `DataSet::visit(...)`

```cpp
#include <dicom.h>
#include <iostream>
using namespace dicom::literals;

auto file = dicom::read_file("sample.dcm");
auto& ds = file->dataset();

ds.visit([](auto path, auto& element) {
  if (element.tag() == "PerFrameFunctionalGroupsSequence"_tag) {
    return dicom::DataSetVisitControl::skip_sequence;
  }
  std::cout << path.to_string() << " -> " << element.tag().to_string() << "\n";
  return dicom::DataSetVisitControl::continue_;
});
```

반환형이 `void`인 callback은 자동으로
`DataSetVisitControl::continue_`로 처리됩니다.

이 control return 값들은 현재 element가 이미 callback에 전달된 뒤에
적용됩니다. 즉 현재 step 자체를 되돌리지는 않습니다.

- `DataSetVisitControl::skip_sequence`
  - 현재 `SQ` element에 대한 callback이 끝난 뒤에 적용됩니다
  - 그 아래 nested item dataset들은 건너뜁니다
  - 그 sequence 다음 sibling element부터 다시 이어집니다
- `DataSetVisitControl::skip_current_dataset`
  - 현재 element에 대한 callback이 끝난 뒤에 적용됩니다
  - 현재 dataset에 남아 있는 나머지 element들을 건너뜁니다
  - root level에서는 visit 전체를 끝냅니다
  - nested item dataset 안에서는 다음 sibling item 또는 parent sibling
    element부터 다시 이어집니다
- `DataSetVisitControl::stop`
  - visit 전체를 즉시 중단합니다

### `DicomFile::visit(...)`

`DicomFile::visit(...)`는 root dataset으로 forward됩니다.

```cpp
auto file = dicom::read_file("sample.dcm");

file->visit([](auto path, auto& element) {
  std::cout << path.to_string() << " -> " << element.tag().to_string() << "\n";
});
```

## C++ walk

`DataSetWalker`, iterator-style 코드, 또는 yielded entry의
`skip_sequence()` 같은 live method가 필요하면 `walk()`를 쓰면 됩니다.

### `DataSet::walk(...)`

```cpp
#include <dicom.h>
#include <iostream>
using namespace dicom::literals;

auto file = dicom::read_file("sample.dcm");
auto& ds = file->dataset();

for (auto entry : ds.walk()) {
  const auto path = entry.path.to_string();
  const auto tag = entry.element.tag();

  std::cout << path << " -> " << tag.to_string() << "\n";

  if (tag == "PerFrameFunctionalGroupsSequence"_tag) {
    entry.skip_sequence();
  }
}
```

iterator를 직접 다루고 싶다면 iterator 쪽에도 같은 walk-control API가
있습니다.

```cpp
auto walker = ds.walk();
for (auto it = walker.begin(); it != walker.end(); ++it) {
  if (it->element.tag() == "PerFrameFunctionalGroupsSequence"_tag) {
    it->skip_sequence();
  }
}
```

이 live control들의 의미도 같습니다.

- `entry.skip_sequence()` / `it->skip_sequence()`
  - 현재 `SQ` element 자체는 순회에 남습니다
  - 그 sequence 아래의 nested item dataset들을 건너뜁니다
  - 그 sequence 다음 sibling element부터 이어집니다
- `entry.skip_current_dataset()` / `it->skip_current_dataset()`
  - 현재 element 자체는 순회에 남습니다
  - 현재 dataset에 남은 element들을 건너뜁니다
  - root level에서는 walk를 끝냅니다
  - nested item dataset 안에서는 다음 sibling item 또는 parent sibling
    element로 이어집니다

## Python walk

Python은 현재 `visit()`이 아니라 `walk()`를 제공합니다.

```python
import dicomsdl as dicom

df = dicom.read_file("sample.dcm")

for entry in df.walk():
    print(entry.path.to_string(), entry.element.tag)
    if entry.element.tag == dicom.Tag("PerFrameFunctionalGroupsSequence"):
        entry.skip_sequence()
```

path와 element만 바로 unpack해서 쓸 수도 있습니다.

```python
for path, elem in df.walk():
    print(path.to_string(), elem.tag)
```

다만 중간 subtree를 건너뛰어야 한다면 `for entry in df.walk():` 스타일이 더
자연스럽습니다.
`skip_sequence()`와 `skip_current_dataset()`는 entry와 walker 양쪽에 있기
때문입니다.

## Path 의미

`path`는 ancestors-only view입니다. 현재 leaf tag는 포함하지 않습니다.

예:

- 현재 위치: `ReferencedSeriesSequence[0].SeriesInstanceUID`
- `path.to_string()`: `00081115.0`
- 현재 `element.tag()`: `SeriesInstanceUID`

문자열 형식은 packed uppercase hex tag와 dotted item index를 사용하므로
dump/path 출력과 맞춰 보기 쉽습니다.

## Borrowed path lifetime

`path`는 현재 callback/step에 묶인 borrowed view입니다.

- 현재 callback 또는 iteration step 안에서 바로 사용합니다.
- 나중에도 보관해야 하면 `path.to_string()` 결과를 저장합니다.
- callback이 끝나거나 walker가 advance된 뒤에는 path object 자체를 계속
  들고 있지 않는 것이 좋습니다.

이건 DicomSDL의 다른 borrowed view 스타일과 같은 감각입니다.

## Walk control

walk-control 연산은 두 가지입니다.

이 제어들은 현재 element가 이미 caller에게 전달된 뒤에 적용됩니다.
현재 step 자체를 없애는 것이 아니라, 그 다음에 어디까지 내려갈지를
바꿉니다.

- `skip_sequence()`
  - 현재 entry가 `SQ`일 때 의미가 있습니다
  - 현재 `SQ` element를 yield한 뒤에 적용됩니다
  - 현재 walk에서 그 sequence subtree를 건너뜁니다
- `skip_current_dataset()`
  - 현재 element를 yield한 뒤에 적용됩니다
  - 현재 walk에서 현재 dataset의 남은 부분 전체를 건너뜁니다
  - root level에서는 walk를 종료합니다
  - nested item dataset 안에서는 다음 sibling item 또는 parent sibling
    element로 이어집니다

이 연산들은 다음 위치에 모두 있습니다.

- `DataSetWalkEntry`
- `DataSetWalkIterator`
- Python `DataSetWalkEntry`
- Python `DataSetWalkIterator`

C++ `visit()`에서는 아래 return 값들이 같은 역할을 합니다.

- `return DataSetVisitControl::skip_sequence;`
- `return DataSetVisitControl::skip_current_dataset;`
- `return DataSetVisitControl::stop;`

## 함께 보면 좋은 문서

- [Python DataSet Guide](python_dataset_guide.md)
- [C++ DataSet Guide](cpp_dataset_guide.md)
- [Sequence and Paths](sequence_and_paths.md)
