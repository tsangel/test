# Selected Read

`read_file_selected(...)` と `read_bytes_selected(...)` は、DICOM ストリームから
選択した tag と nested sequence child だけを読み込みます。

トップレベル tag の一部と、ネストした sequence 内の特定 child だけが必要で、
それ以外の dataset 全体を読み込みたくないときに使います。

## Selection tree

`DataSetSelection` はネストしたツリーです。

- leaf node はその tag 自体だけを選択します。
- nested node はその tag 自体を選択し、その sequence 配下のすべての item
  dataset に child selection を適用します。
- private tag と unknown tag も許可され、`"70531000"` のような explicit tag string も使えます。

構築時には次の正規化が自動で行われます。

- root level の `TransferSyntaxUID (0002,0010)` と
  `SpecificCharacterSet (0008,0005)` を、未指定なら自動で追加
- sibling tag を昇順に整列
- 重複 node をマージ

## C++ 例

```cpp
#include <dicom.h>
using namespace dicom::literals;

dicom::DataSetSelection selection{
    "StudyInstanceUID"_tag,
    "SeriesInstanceUID"_tag,
    {"ReferencedSeriesSequence"_tag, {
        "SeriesInstanceUID"_tag,
        "ReferencedSOPInstanceUID"_tag,
    }},
    "SOPInstanceUID"_tag,
};

auto file = dicom::read_file_selected("sample.dcm", selection);
auto& ds = file->dataset();
```

## Python の再利用例

```python
import dicomsdl as dicom

selection = dicom.DataSetSelection(
    [
        "StudyInstanceUID",
        "SeriesInstanceUID",
        ("ReferencedSeriesSequence", [
            "SeriesInstanceUID",
            "ReferencedSOPInstanceUID",
        ]),
        "SOPInstanceUID",
    ]
)

df = dicom.read_file_selected("sample.dcm", selection)
```

## Python の one-shot 例

Python では one-shot 呼び出し用に raw nested selection tree を
そのまま渡すこともできます。

```python
import dicomsdl as dicom

df = dicom.read_bytes_selected(
    data,
    [
        "SOPInstanceUID",
        ("ReferencedStudySequence", [
            "ReferencedSOPInstanceUID",
        ]),
    ],
    name="sample",
)
```

同じ selection を複数ファイルで再利用するなら、
`DataSetSelection(...)` を一度作って使い回すほうが向いています。

## Semantics

- 返される `DicomFile` には、選択した tag と nested sequence child だけが
  入ります。未選択 tag は存在しないかのように振る舞います。
- root level の `TransferSyntaxUID` と `SpecificCharacterSet` は、
  明示しなくても常に考慮されます。
- selected-read API では `ReadOptions.load_until` は無視されます。
  必要な read frontier は selection から内部的に導出されます。
- `SQ` だけを選択した場合でも、source にその sequence が存在すれば
  present な `SQ` を保持します。item dataset も保持されますが、
  child selection が空なら item dataset 自体は空になり得ます。
- private tag と unknown tag も selection 対象として使え、`"70531000"` のような explicit tag string も使えます。
- `keep_on_error` は通常の read と同様の考え方ですが、selected read が
  実際に訪問した領域に対してだけ適用されます。
- 選択領域の外にある malformed data は見えないままのことがあり、
  その場合 `has_error` や `error_message` に反映されないことがあります。

## 関連ドキュメント

- [File IO](file_io.md)
- [DataSet Visit and Walk](dataset_visit_and_walk.md)
- [C++ API Overview](../reference/cpp_api.md)
- [DicomFile Reference](../reference/dicomfile_reference.md)
- [Python API Reference](../reference/python_reference.md)
