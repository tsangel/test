# DataSet Visit and Walk

`DataSet.visit()` と `DicomFile.visit()` は、C++ 向けの callback-style
fast path として、ルート dataset とその下にあるすべての nested
sequence item dataset を depth-first preorder で走査します。

`DataSet.walk()` と `DicomFile.walk()` は、同じ木を iterator-style で
たどる API です。

visit/walk はどちらも `SQ` data element 自体を先に訪問してから、その
item に降ります。各 step では次の 2 つを返します。

- ancestors-only の path view
- 現在の `DataElement`

nested metadata inspection、必要な部分だけを選んで飛ばすこと、UID rewrite のような
transform-style pass の基盤として使いやすい API です。

`visit()` と `walk()` は、どちらもすでに読み込まれている dataset 状態
だけを走査します。暗黙に `ensure_loaded()` や `ensure_dataelement()` を
呼ぶことはありません。

partial-load された attached dataset では、後ろ側の tag は traversal
から静かに抜け落ちます。dataset 全体を検査する pass に使うなら、先に
fully load するか、visit/walk の前に必要な frontier まで
`ensure_loaded(tag)` を呼んでください。

## 何が訪問されるか

訪問順序は次の通りです。

1. current `DataElement`
2. その element が `SQ` なら nested item dataset を順に訪問
3. 残りの sibling elements を訪問

つまり `SQ` element は子より先に訪問されるので、そこで下を飛ばすか
どうかを決める自然な skip point になります。

## C++ visit

C++ では、iterator object が不要なら `visit()` から見るのが自然です。

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

戻り値が `void` の callback は、自動的に
`DataSetVisitControl::continue_` として扱われます。

これらの control return 値は、現在の element がすでに callback に
渡された後に適用されます。現在の step 自体を取り消すわけではありません。

- `DataSetVisitControl::skip_sequence`
  - 現在の `SQ` element への callback の後に適用されます
  - その下の nested item dataset は飛ばします
  - その sequence の次の sibling element から再開します
- `DataSetVisitControl::skip_current_dataset`
  - 現在の element への callback の後に適用されます
  - 現在の dataset に残っている element 全体を飛ばします
  - root level では visit 全体を終了します
  - nested item dataset では次の sibling item または parent sibling
    element から再開します
- `DataSetVisitControl::stop`
  - visit 全体を即座に停止します

### `DicomFile::visit(...)`

`DicomFile::visit(...)` は root dataset に forward されます。

```cpp
auto file = dicom::read_file("sample.dcm");

file->visit([](auto path, auto& element) {
  std::cout << path.to_string() << " -> " << element.tag().to_string() << "\n";
});
```

## C++ walk

`DataSetWalker`、iterator-style のコード、または yielded entry の
`skip_sequence()` のような live method が必要なら `walk()` を使います。

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

iterator を明示的に扱いたい場合は、iterator 側にも同じ walk-control
API があります。

```cpp
auto walker = ds.walk();
for (auto it = walker.begin(); it != walker.end(); ++it) {
  if (it->element.tag() == "PerFrameFunctionalGroupsSequence"_tag) {
    it->skip_sequence();
  }
}
```

これらの live control も意味は同じです。

- `entry.skip_sequence()` / `it->skip_sequence()`
  - 現在の `SQ` element 自体は traversal に残ります
  - その sequence 配下の nested item dataset を飛ばします
  - その sequence の次の sibling element から続きます
- `entry.skip_current_dataset()` / `it->skip_current_dataset()`
  - 現在の element 自体は traversal に残ります
  - 現在の dataset に残っている element を飛ばします
  - root level では walk を終了します
  - nested item dataset では次の sibling item または parent sibling
    element に進みます

## Python walk

Python では現在 `visit()` ではなく `walk()` が公開されています。

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

ただし途中の subtree を飛ばしたいなら、`for entry in df.walk():` の方が分かりやすい
ことが多いです。`skip_sequence()` と `skip_current_dataset()` は entry と
walker の両方にあるためです。

## Path の意味

`path` は ancestors-only view です。現在の leaf tag は含みません。

例:

- current location: `ReferencedSeriesSequence[0].SeriesInstanceUID`
- `path.to_string()`: `00081115.0`
- current `element.tag()`: `SeriesInstanceUID`

文字列形式は packed uppercase hex tag と dotted item index を使うので、
dump/path 出力と比較しやすくなっています。

## Borrowed path の寿命

`path` は現在の callback/step に結び付いた borrowed view です。

- current callback または iteration step の中で使います。
- 後で保持したい場合は `path.to_string()` を保存します。
- callback が終わった後や walker が advance した後に path object 自体を
  保持し続けるのは避けてください。

これは DicomSDL の他の borrowed view と同じ感覚です。

## Walk control

walk-control operation は 2 つあります。

これらの制御は、現在の element がすでに caller に渡された後に適用されます。
現在の step 自体を消すのではなく、その後の traversal を飛ばします。

- `skip_sequence()`
  - 現在の entry が `SQ` element のときに有効
  - 現在の `SQ` element を yield した後に適用されます
  - 現在の walk でその sequence subtree を飛ばします
- `skip_current_dataset()`
  - 現在の element を yield した後に適用されます
  - 現在の walk で現在の dataset の残り全体を飛ばします
  - root level では walk を終了します
  - nested item dataset では次の sibling item または parent sibling
    element に進みます

これらは次の API で使えます。

- `DataSetWalkEntry`
- `DataSetWalkIterator`
- Python `DataSetWalkEntry`
- Python `DataSetWalkIterator`

C++ `visit()` では、次の return 値が同じ役割を持ちます。

- `return DataSetVisitControl::skip_sequence;`
- `return DataSetVisitControl::skip_current_dataset;`
- `return DataSetVisitControl::stop;`

## 関連ページ

- [Python DataSet Guide](python_dataset_guide.md)
- [C++ DataSet Guide](cpp_dataset_guide.md)
- [Sequence and Paths](sequence_and_paths.md)
