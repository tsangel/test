# DataSet Walk

`DataSet.walk()` と `DicomFile.walk()` は、ルート dataset とその下にある
すべての nested sequence item dataset を depth-first preorder で走査します。

walk は `SQ` data element 自体も先に訪問してから、その item に
降ります。各 step では次の 2 つを返します。

- ancestors-only の path view
- 現在の `DataElement`

nested metadata inspection、selective pruning、UID rewrite のような
transform-style pass の基盤として使いやすい API です。

`walk()` は、すでに読み込まれている dataset 状態だけを走査します。
暗黙に `ensure_loaded()` や `ensure_dataelement()` を呼ぶことはありません。

partial-load された attached dataset では、後ろ側の tag は walk から
静かに抜け落ちます。dataset 全体を検査する pass に使うなら、先に fully
load するか、walk の前に必要な frontier まで `ensure_loaded(tag)` を
呼んでください。

## 何が訪問されるか

訪問順序は次の通りです。

1. current `DataElement`
2. その element が `SQ` なら nested item dataset を順に訪問
3. 残りの sibling elements を訪問

つまり `SQ` element 自体も呼び出し側から見えるので、その位置で
walk control をかけられます。

## C++ example

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

iterator を明示的に扱いたい場合は、iterator 側にも同じ
walk-control API があります。

```cpp
auto walker = ds.walk();
for (auto it = walker.begin(); it != walker.end(); ++it) {
  if (it->element.tag() == "PerFrameFunctionalGroupsSequence"_tag) {
    it->skip_sequence();
  }
}
```

## Python example

```python
import dicomsdl as dicom

df = dicom.read_file("sample.dcm")

for entry in df.walk():
    print(entry.path.to_string(), entry.element.tag)
    if entry.element.tag == dicom.Tag("PerFrameFunctionalGroupsSequence"):
        entry.skip_sequence()
```

path と element だけを unpack して使うこともできます。

```python
for path, elem in df.walk():
    print(path.to_string(), elem.tag)
```

ただし walk control が必要なら、`for entry in df.walk():` の方が
分かりやすいことが多いです。`skip_sequence()` と
`skip_current_dataset()` は entry と walker の両方にあるためです。

## Path の意味

`entry.path` は ancestors-only view です。現在の leaf tag は含みません。

例:

- current location: `ReferencedSeriesSequence[0].SeriesInstanceUID`
- `entry.path.to_string()`: `00081115.0`
- `entry.element.tag()`: `SeriesInstanceUID`

文字列形式は packed uppercase hex tag と dotted item index を使うので、
dump/path 出力と比較しやすくなっています。

## Borrowed path の寿命

`entry.path` は現在の walk step に結び付いた borrowed view です。

- current iteration step の中で使います。
- 後で保持したい場合は `entry.path.to_string()` を保存します。
- walker が advance した後に path object 自体を保持し続けるのは避けてください。

これは DicomSDL の他の borrowed view と同じ感覚です。

## Walk control

walk-control operation は 2 つあります。

- `skip_sequence()`
  - 現在の entry が `SQ` element のときに有効
  - 現在の walk でその sequence subtree を飛ばします
- `skip_current_dataset()`
  - 現在の walk で現在の dataset の残り全体を飛ばします
  - root level では walk を終了します
  - nested item dataset では次の sibling item または parent sibling
    element に進みます

これらは次の API で使えます。

- `DataSetWalkEntry`
- `DataSetWalkIterator`
- Python `DataSetWalkEntry`
- Python `DataSetWalkIterator`

## 関連ページ

- [Python DataSet Guide](python_dataset_guide.md)
- [C++ DataSet Guide](cpp_dataset_guide.md)
- [Sequence and Paths](sequence_and_paths.md)
