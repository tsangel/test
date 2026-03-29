# DataSet Walk

`DataSet.walk()`와 `DicomFile.walk()`는 루트 dataset과 모든 nested sequence
item dataset을 depth-first preorder로 순회합니다.

이 순회는 `SQ` data element 자체도 먼저 방문한 뒤 그 아래 item으로
내려갑니다. 각 step은 다음 두 값을 제공합니다.

- ancestors-only path view
- 현재 `DataElement`

즉 nested metadata inspection, selective pruning, 그리고 UID rewrite 같은
transform-style pass의 기반으로 쓰기 좋습니다.

`walk()`는 이미 로드된 dataset 상태만 순회합니다. 암묵적으로
`ensure_loaded()`나 `ensure_dataelement()`를 호출하지 않습니다.

partial-load된 attached dataset에서는 뒤쪽 태그들이 walk에서 조용히 빠질 수
있습니다. 전체 dataset을 검사해야 하는 pass라면 먼저 fully load하거나,
walk 전에 필요한 frontier까지 `ensure_loaded(tag)`를 호출해야 합니다.

## 어떤 순서로 방문하나

순회 순서는 다음과 같습니다.

1. 현재 `DataElement`
2. 그 element가 `SQ`이면 nested item dataset들을 순서대로 방문
3. 나머지 sibling element 방문

즉 `SQ` element도 caller가 직접 보게 되므로, 그 지점에서 pruning을 걸 수
있습니다.

## C++ 예시

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

iterator를 직접 다루고 싶다면 iterator 쪽에도 같은 walk-control API가 있습니다.

```cpp
auto walker = ds.walk();
for (auto it = walker.begin(); it != walker.end(); ++it) {
  if (it->element.tag() == "PerFrameFunctionalGroupsSequence"_tag) {
    it->skip_sequence();
  }
}
```

## Python 예시

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

다만 pruning이 필요하면 `for entry in df.walk():` 스타일이 더 자연스럽습니다.
`skip_sequence()`와 `skip_current_dataset()`는 entry와 walker 양쪽에 있기
때문입니다.

## Path 의미

`entry.path`는 ancestors-only view입니다. 현재 leaf tag는 포함하지 않습니다.

예:

- 현재 위치: `ReferencedSeriesSequence[0].SeriesInstanceUID`
- `entry.path.to_string()`: `00081115.0`
- `entry.element.tag()`: `SeriesInstanceUID`

문자열 형식은 packed uppercase hex tag와 dotted item index를 사용하므로
dump/path 출력과 맞춰 보기 쉽습니다.

## Borrowed path lifetime

`entry.path`는 현재 walk step에 묶인 borrowed view입니다.

- 현재 iteration step 안에서 바로 사용합니다.
- 나중에도 보관해야 하면 `entry.path.to_string()` 결과를 저장합니다.
- walker가 advance된 뒤에는 path object 자체를 계속 들고 있지 않는 것이 좋습니다.

이건 DicomSDL의 다른 borrowed view 스타일과 같은 감각입니다.

## Walk control

walk-control 연산은 두 가지입니다.

- `skip_sequence()`
  - 현재 entry가 `SQ`일 때 의미가 있습니다
  - 현재 walk에서 그 sequence subtree를 건너뜁니다
- `skip_current_dataset()`
  - 현재 walk에서 현재 dataset의 남은 부분 전체를 건너뜁니다
  - root level에서는 walk를 종료합니다
  - nested item dataset 안에서는 다음 sibling item 또는 parent sibling
    element로 이어집니다

이 연산들은 다음 위치에 모두 있습니다.

- `DataSetWalkEntry`
- `DataSetWalkIterator`
- Python `DataSetWalkEntry`
- Python `DataSetWalkIterator`

## 함께 보면 좋은 문서

- [Python DataSet Guide](python_dataset_guide.md)
- [C++ DataSet Guide](cpp_dataset_guide.md)
- [Sequence and Paths](sequence_and_paths.md)
