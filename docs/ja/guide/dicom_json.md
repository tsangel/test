# DICOM JSON

DicomSDL は `DataSet` または `DicomFile` を DICOM JSON Model に
シリアライズでき、DICOM JSON を 1 つ以上の `DicomFile` オブジェクトへ
読み戻すこともできます。

このページでは、Python と C++ の両方で使う公開 API
`write_json(...)`、`read_json(...)`、`set_bulk_data(...)` の流れに
絞って説明します。

## サポート範囲

- `DicomFile.write_json(...)`
- `DataSet.write_json(...)`
- メモリ上にある UTF-8 テキストまたはバイト列からの `read_json(...)`
- DICOM JSON の top-level object と top-level array payload
- `BulkDataURI`、`InlineBinary`、ネストした sequence、PN object
- `JsonBulkRef` + `set_bulk_data(...)` による呼び出し側管理の bulk ダウンロード

現在のスコープに関する注意:

- `read_json(...)` はメモリ入力 API です。ディスクや HTTP ストリームを
  直接読むものではありません。
- JSON reader / writer は DICOM JSON Model を実装しますが、完全な
  DICOMweb HTTP クライアント / サーバースタックではありません。

## JSON の書き出し

### Python write example

```python
import dicomsdl as dicom

df = dicom.read_file("sample.dcm")

json_text, bulk_parts = df.write_json()
```

戻り値は次のとおりです。

- `json_text: str`
- `bulk_parts: list[tuple[str, memoryview, str, str]]`

各 bulk tuple は次の要素を持ちます。

- `uri`
- `payload`
- `media_type`
- `transfer_syntax_uid`

### C++ write example

```cpp
#include <dicom.h>

auto file = dicom::read_file("sample.dcm");
dicom::JsonWriteResult out = file->write_json();

std::string json_text = std::move(out.json);
for (const auto& part : out.bulk_parts) {
    auto bytes = part.bytes();
    // part.uri
    // part.media_type
    // part.transfer_syntax_uid
}
```

## JSON write options

`JsonWriteOptions` は公開ヘッダーで定義されており、Python では keyword
argument として公開されています。

### `include_group_0002`

- デフォルト: `false`
- 意味: JSON 出力に file meta group `0002` を含めるかどうか

デフォルトの DICOM JSON / DICOMweb 形式の出力では group `0002` を
含めません。Group length element `(gggg,0000)` は常に除外されます。

### `bulk_data`

Python の値:

- `"inline"`
- `"uri"`
- `"omit"`

C++ の値:

- `JsonBulkDataMode::inline_`
- `JsonBulkDataMode::uri`
- `JsonBulkDataMode::omit`

動作:

- `inline`: bulk 可能な値も `InlineBinary` のまま出力
- `uri`: threshold 以上の値は `BulkDataURI` に切り出す
- `omit`: attribute 自体は `vr` とともに残し、bulk 値だけを出力しない

### `bulk_data_threshold`

- デフォルト: `1024`
- `bulk_data="uri"` のときだけ使用

`bulk_data="uri"` の場合、threshold より小さい値は inline のままで、
threshold 以上の値は `BulkDataURI` になります。

### `bulk_data_uri_template`

`bulk_data="uri"` のときに `PixelData` 以外の bulk element に使う
URI template です。

使える placeholder:

- `{study}`
- `{series}`
- `{instance}`
- `{tag}`

`{tag}` は次のように展開されます。

- top-level element: `7FE00010`
- ネストした sequence element: `22002200.0.12340012` のような dotted tag path

例:

```python
json_text, bulk_parts = df.write_json(
    bulk_data="uri",
    bulk_data_uri_template="/dicomweb/studies/{study}/series/{series}/instances/{instance}/bulk/{tag}",
)
```

### `pixel_data_uri_template`

`PixelData (7FE0,0010)` 専用の override です。

典型的な使い方:

```python
json_text, bulk_parts = df.write_json(
    bulk_data="uri",
    bulk_data_uri_template="/dicomweb/studies/{study}/series/{series}/instances/{instance}/bulk/{tag}",
    pixel_data_uri_template="/dicomweb/studies/{study}/series/{series}/instances/{instance}/frames",
)
```

サーバーが frame 指向の pixel route を他の bulk data route と分けて
公開しているときに使います。

### Write `charset_errors`

Python の値:

- `"strict"`
- `"replace_fffd"`
- `"replace_hex_escape"`

C++ の値:

- `CharsetDecodeErrorPolicy::strict`
- `CharsetDecodeErrorPolicy::replace_fffd`
- `CharsetDecodeErrorPolicy::replace_hex_escape`

JSON テキストを生成するときの text decode 処理を制御します。

## PixelData bulk behavior

### Native PixelData

- JSON には `BulkDataURI` が 1 つだけ残ります。
- native multi-frame bulk も aggregate bulk part 1 つのままです。

### Encapsulated PixelData

- JSON には base `BulkDataURI` が 1 つだけ残ります。
- `bulk_parts` は frame 単位で返されます。
- frame URI は選んだ base に応じて次のようになります。
  - `/.../frames` -> `/.../frames/1`, `/.../frames/2`, ...
  - generic base URI -> `/.../bulk/7FE00010/frames/1`, ...

これにより JSON 自体は簡潔なまま保ちつつ、multipart 応答や frame 応答を
組み立てるための per-frame payload 一覧を取得できます。

## JSON の読み込み

### Python read example

```python
import dicomsdl as dicom

items = dicom.read_json(json_text)

for df, refs in items:
    ...
```

### C++ read example

```cpp
#include <dicom.h>

dicom::JsonReadResult result = dicom::read_json(
    reinterpret_cast<const std::uint8_t*>(json_bytes.data()),
    json_bytes.size());

for (auto& item : result.items) {
    auto& file = *item.file;
    auto& refs = item.pending_bulk_data;
    (void)file;
    (void)refs;
}
```

reader は DICOM JSON が次のどちらかになり得るため、常に collection を
返します。

- dataset object 1 つ
- dataset object の配列

JSON が top-level object 1 つなら、結果リストの長さは `1` です。

## JSON read options

### Read `charset_errors`

Python の値:

- `"strict"`
- `"replace_qmark"`
- `"replace_unicode_escape"`

C++ の値:

- `CharsetEncodeErrorPolicy::strict`
- `CharsetEncodeErrorPolicy::replace_qmark`
- `CharsetEncodeErrorPolicy::replace_unicode_escape`

このポリシーは、UTF-8 JSON から読んだテキストを、後で
`value_span()`、`write_file(...)`、`set_bulk_data(...)` などの API 用に
raw DICOM bytes へ戻すときに使われます。

## bulk ダウンロードの流れ

典型的な Python の流れ:

```python
items = dicom.read_json(json_text)

for df, refs in items:
    for ref in refs:
        payload = download(ref.uri)
        df.set_bulk_data(ref, payload)
```

典型的な C++ の流れ:

```cpp
for (auto& item : result.items) {
    for (const auto& ref : item.pending_bulk_data) {
        std::vector<std::uint8_t> payload = download(ref.uri);
        item.file->set_bulk_data(ref, payload);
    }
}
```

`JsonBulkRef` には次の情報が入っています。

- `kind`
- `path`
- `frame_index`
- `uri`
- `media_type`
- `transfer_syntax_uid`
- `vr`

## 読み込み時の URI 保持ルール

JSON reader は意図的に保守的に動作します。

次のように、すでに dereference 可能な URI はそのまま保持します。

- `.../frames/1`
- `.../frames/1,2,3`
- `https://example.test/instances/1?sig=...` のような presigned URL、
  トークン付きダウンロード URL、または opaque absolute URL
- `https://example.test/studies/s/series/r/instances/i/bulk/7FE00010?sig=...`
  のような presigned / トークン付き generic pixel URL

次のように URI の形そのものが frame route を明示している場合にだけ、
frame URL を合成します。

- `.../frames`
- `.../bulk/7FE00010` のような署名やトークン suffix を持たない plain generic base URI

これは presigned URL やトークン付きダウンロード URL で特に重要です。
すでに署名済みの opaque URL に `/frames/{n}` を追加すると path が変わり、
通常は dereference に失敗するため、そのような URL は変更せずそのまま
保持します。

## `set_bulk_data(...)` behavior

`set_bulk_data(...)` は次の 2 つの重要なケースをサポートします。

- frame ref: encoded frame 1 つを encapsulated `PixelData` slot にコピー
- opaque encapsulated element ref: encapsulated `PixelData` value field 全体を
  受け取り、書き込み可能な内部 pixel sequence に復元

つまり opaque presigned `BulkDataURI` やトークン付き `BulkDataURI`
でも通常の流れに参加できます。

1. `read_json(...)` が presigned またはトークン付きダウンロード URL を
   `element` ref 1 つとして保持
2. 呼び出し側がその URL から payload bytes をダウンロード
3. `set_bulk_data(ref, payload)` がダウンロードした value field から
   encapsulated `PixelData` を実際の内部 pixel sequence に復元

## transfer syntax に関する注意

`JsonBulkPart.transfer_syntax_uid` と `JsonBulkRef.transfer_syntax_uid` は、
file meta `TransferSyntaxUID (0002,0010)` が存在するときにその値で
埋められます。その情報がない場合、reader は保守的に振る舞い、
metadata だけから encapsulated frame layout を推測しません。

## 入力ルール

- JSON 入力は UTF-8 テキストである必要があります。
- Python は `str` または bytes-like 入力を受け取れます。
- 空入力はエラーです。
- top-level 入力は JSON object または array である必要があります。

## 関連ドキュメント

- [ファイルI/O](file_io.md)
- [Python データセット ガイド](python_dataset_guide.md)
- [Python API Reference](../reference/python_reference.md)
- [DicomFile Reference](../reference/dicomfile_reference.md)
- [DataSet Reference](../reference/dataset_reference.md)
