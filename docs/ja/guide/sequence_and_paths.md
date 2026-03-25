# シーケンスとパス

ネストされた DICOM データは、`SequenceKeyword.0.LeafKeyword` などの点線のタグ パスを介して読み書きするのが最も簡単です。

## C++

```cpp
#include <dicom.h>

dicom::DataSet ds;
ds.ensure_dataelement("ReferencedStudySequence.0.ReferencedSOPInstanceUID", dicom::VR::UI)
  .from_uid_string("1.2.3");

const auto& uid =
    ds["ReferencedStudySequence.0.ReferencedSOPInstanceUID"];
```

## パイソン

```python
import dicomsdl as dicom

df = dicom.read_file("sample.dcm")
ds = df.dataset
half_life = ds.get_value("RadiopharmaceuticalInformationSequence.0.RadionuclideHalfLife")
ds.set_value("ReferencedStudySequence.0.ReferencedSOPInstanceUID", "1.2.3")
```

## 注意事項

- パスの形式は `SequenceName.item_index.LeafName` です。
- ワンショットのネストされた検索または割り当てが必要な場合は、このパス形式を使用します。
- C++ では、`operator[]`、`get_dataelement(...)`、および `get_value(...)` はすべてドット付きタグ パスを受け入れます。
- `ensure_dataelement(...)` が既存の非シーケンス中間要素の下にネストされたパスを具体化すると、その中間要素を `SQ` にリセットできます。
- 低レベルのトラバーサルの詳細が必要な場合は、返された `Sequence` / item データセットへの参照を保持し、要素を直接検査します。

## 関連ドキュメント

- [コアオブジェクト](core_objects.md)
- [C++ データセット ガイド](cpp_dataset_guide.md)
- [Python データセット ガイド](python_dataset_guide.md)
- [タグパス検索セマンティクス](../reference/tag_path_lookup.md)
