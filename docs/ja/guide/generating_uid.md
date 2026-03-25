# UIDの生成

このドキュメントでは、`dicomsdl` における UID の生成と追加の流れを説明します。

## 1. 範囲

- C++:
- `dicom::uid::try_generate_uid()`
- `dicom::uid::generate_uid()`
- `dicom::uid::Generated::try_append(component)`
- `dicom::uid::Generated::append(component)`
- Python:
- `dicom.try_generate_uid()`
- `dicom.generate_uid()`
- `dicom.try_append_uid(base_uid, component)`
- `dicom.append_uid(base_uid, component)`

## 2. C++ API

### 2.1 ベース UID の生成

```cpp
auto uid_opt = dicom::uid::try_generate_uid(); // std::optional<Generated>
auto uid = dicom::uid::generate_uid();         // Generated (throws on failure)
```

- プレフィックス: `dicom::uid::uid_prefix()`
- `generate_uid()` は、UID を `<uid_prefix>.<random_numeric_suffix>` という形で構築します。
サフィックスは、プロセス単位のランダムな nonce と単調増加シーケンスから生成されます。
- 出力: 厳密に有効な UID テキスト (最大 64 文字)

### 2.2 コンポーネントを 1 つ追加する

```cpp
auto study = dicom::uid::generate_uid();
auto series = study.append(23);
auto inst = series.append(34);
```

- `try_append(component)` は `std::optional<Generated>` を返します
- `append(component)` は失敗時に `std::runtime_error` をスローします

### 2.3 `Generated` への既存の UID テキスト

```cpp
auto base = dicom::uid::make_generated("1.2.840.10008");
if (base) {
    auto extended = base->append(7);
}
```

## 3. Python API

```python
import dicomsdl as dicom

study = dicom.generate_uid()
series = dicom.append_uid(study, 23)
inst = dicom.append_uid(series, 34)

safe = dicom.try_append_uid("1.2.840.10008", 7)  # Optional[str]
```

- 無効な入力/失敗時に `append_uid(...)` が発生する
- `try_append_uid(...)` は失敗時に `None` を返します

## 4. 追加動作

`append` / `try_append` の場合:

1. 直接パス:
- 最初に `<base_uid>.<component>` を試してください。
- 有効で 64 文字以下の場合は、それを返します。

2. フォールバック パス (直接追加が適合しない場合):
- `base_uid` の最初の 30 文字を保持します。
- 最後の文字が `.` でない場合は、`.` を追加します。
- U96 10 進数ブロックを 1 つ追加します。
- 結果は厳密な UID バリデータによって再検証されます。

### 重要な注意事項

フォールバック出力は意図的に非決定的です。

- 依然として `component` および `base_uid` に基づいています。
- ただし、プロセスレベルのランダムなナンスとアトミックシーケンスも混合します。

したがって、同じ `(base_uid, component)` の場合、フォールバック サフィックスは呼び出しごとに異なる場合があります。

## 5. 障害モデルの概要

- `generate_uid()`:
- 失敗時にスローします
- `try_generate_uid()`:
- 失敗した場合は `None` / `std::nullopt` を返します
- `append_uid()` / `Generated::append()`:
- 失敗時にスローします
- `try_append_uid()` / `Generated::try_append()`:
- 失敗した場合は `None` / `std::nullopt` を返します

## 6. 実際的な推奨事項

- お申込みの流れ：
- `generate_uid()` でベースを構築する
- `append(...)` を介して子 UID を導出する
- 入力ベース UID が信頼できない可能性がある場合は、次を使用します。
- Python の `try_append_uid(...)`
- C++ の `try_append(...)`
