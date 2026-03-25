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

## 注意

- パスの形式は `SequenceName.item_index.LeafName` です。
- ネストされた値を一度に検索または代入したい場合は、このパス形式を使います。
- C++ では、`operator[]`、`get_dataelement(...)`、`get_value(...)` はいずれもドット区切りのタグ パスを受け付けます。
- `ensure_dataelement(...)` が既存の非シーケンス中間要素の下にネストされたパスを作成すると、その中間要素を `SQ` にリセットすることがあります。
- 低レベルの走査の詳細が必要な場合は、返された `Sequence` / item データセットへの参照を保持し、要素を直接調べます。

## 関連ドキュメント

- [コアオブジェクト](core_objects.md)
- [C++ データセット ガイド](cpp_dataset_guide.md)
- [Python データセット ガイド](python_dataset_guide.md)
- [タグパス検索セマンティクス](../reference/tag_path_lookup.md)
