# Python データセット ガイド

DicomSDL は薄い nanobind ラッパーです。実行時にネイティブ拡張機能をロードするため、ドキュメントは `mock import` を使ってビルドされます。サンプルを実行するときはホイールをインストールしてください。

これは、DicomSDL の Python 側のファイル、データセット、要素へのアクセスに関するユーザー向けの主要なガイドです。
モジュール レベルのエントリ ポイント、DicomSDL オブジェクトが DICOM にマップされる方法、および最も重要な読み取り/書き込みパターンについて説明します。

## インポート

```python
import dicomsdl as dicom
```

## モジュールレベルのエントリポイント

- `keyword_to_tag_vr(keyword: str) -> (Tag, VR)`: キーワードを `(Tag, VR)` に解決します。
- `tag_to_keyword(tag: Tag | str) -> str`: タグをキーワードに解決します。
- `read_file(path) -> DicomFile`: DICOM ファイル/セッションをディスクからロードします。
- `read_bytes(data, name="inline") -> DicomFile`: メモリ内バッファからロードします。
- `generate_uid() -> str`: DICOMSDL プレフィックスの下に新しい UID を作成します。
- `append_uid(base_uid: str, component: int) -> str`: フォールバック ポリシーを含む 1 つの UID コンポーネントを追加します。

## DicomSDL と DICOM のマッピング方法

DicomSDL は、関連する Python オブジェクトの小さなセットを公開します。

- `DicomFile`: ルート データセットを所有するファイル/セッション ラッパー
- `DataSet`: DICOM `DataElement` オブジェクトのコンテナー
- `DataElement`: 1 つの DICOM フィールド、タグ / VR / 長さメタデータおよび型付き値アクセス付き
- `Sequence`: `SQ` 値のネストされた項目コンテナー
- `PixelSequence`: カプセル化または圧縮されたピクセル データのフレーム/フラグメント コンテナー

オブジェクト モデルと DICOM マッピングについては、[Core Objects](core_objects.md) を参照してください。

バインディングは意図的に分割モデルを使用します。

- 属性アクセスは値指向です: `ds.Rows`
- インデックス アクセスは `DataElement` を返します: `ds["Rows"]`

これにより、一般的な読み取りが短くなり、同時に VR/長さ/タグのメタデータの検査が容易になります。

### DicomFile と DataSet

ほとんどのデータ要素アクセス API は `DataSet` に実装されています。
`DicomFile` はルート `DataSet` を所有し、ロード、保存、トランスコードなどのファイル指向の作業を処理します。
便宜上、`DicomFile` はルート データセット アクセスを転送するため、`df.Rows`、`df["Rows"]`、`df.get_value(...)`、および `df.Rows = 512` はすべて `df.dataset` に委任されます。
`ds = df.dataset` をバインドすると、同じデータセット API を転送せずに直接使用することになります。

これらのパターンは同等です。

```python
df = dicom.read_file("sample.dcm")

rows1 = df.Rows
rows2 = df.dataset.Rows

elem1 = df["Rows"]
elem2 = df.dataset["Rows"]

df.Rows = 512
df.dataset.Rows = 512
```

## 推奨 API

| API | 戻り値 | 欠落時の動作 | 用途 |
| --- | --- | --- | --- |
| `"Rows" in ds` | `bool` | `False` |存在プローブ |
| `ds.get_value("Rows", default=None)` |型付きの値または指定したデフォルト値 | 指定したデフォルト値を返します | `None` または別のデフォルト値で欠落を表すワンショットの型付き読み取り |
| `ds["Rows"]`、`ds.get_dataelement("Rows")` | `DataElement` | `False` と評価される `NullElement` を返し、例外は出ません | `DataElement` アクセス |
| `ds.ensure_loaded("Rows")` | `None` |無効なキーで発生します | `Rows` などの後のタグまで部分読み取りを明示的に続行します。
| `ds.ensure_dataelement("Rows", vr=None)` | `DataElement` |既存の要素を返すか、長さゼロの要素を挿入します。チェーンに適した API の確保/作成 |
| `ds.set_value("Rows", 512)` | `bool` |書き込みまたは長さゼロ |ワンショット型の代入 |
| `ds.set_value(0x00090030, dicom.VR.US, 16)` | `bool` |明示的な VR によって作成またはオーバーライドされます。プライベートまたは曖昧なタグ |
| `ds.Rows` | 型付き値 | `AttributeError` | 既知のタグ向けの開発・対話環境用の簡便なアクセス |
| `ds.Rows = 512` | `None` | 割り当て失敗時に例外送出 | 標準キーワード更新向けの開発・対話環境用の簡便な代入 |

## Python でデータ要素を識別する方法

| 形式 | 例 | まず使う場面 |
| --- | --- | --- |
| パック整数 | `0x00280010` | タグが数値テーブルや外部メタデータからすでに来ているとき |
| キーワードまたはタグ文字列 | `"Rows"`, `"(0028,0010)"` | ふつうの Python コードの大半 |
| ドット区切りのタグパス文字列 | `"RadiopharmaceuticalInformationSequence.0.RadionuclideTotalDose"` | 一度でネストされた検索や代入を済ませたいとき |
| `Tag` オブジェクト | `dicom.Tag("Rows")`, `dicom.Tag(0x0028, 0x0010)` | 明示的で再利用可能なタグオブジェクトがほしいとき |

### `0x00280010`

- タグが数値定数、生成テーブル、外部メタデータからすでに来ているときに使います。
- 利点: 単純なタグでは最速かつ最も直接的で、文字列解析がなく、`ensure_loaded(...)` のような単純タグ向け API でも使えます。
- トレードオフ: キーワードより読みにくく、ネストパスは表現できません。

### `"Rows"` または `"(0028,0010)"`

- ふつうの Python コードでは、まずこれを使います。
- 利点: 短く読みやすく、一般的な検索 / 書き込み API で広く使えます。
- トレードオフ: キーワード/タグ文字列の解析コストが少しあり、ネストアクセスにはドット区切りのパス文字列が必要です。

### ドット区切りのタグパス文字列

- 一度でネストされた検索や代入を済ませたいときに使います。
- 例: `"RadiopharmaceuticalInformationSequence.0.RadionuclideTotalDose"`, `"00540016.0.00181074"`。
- 利点: ネストされた dataset に対して読みやすいワンショットアクセスになります。
- トレードオフ: ネストパス対応 API でしか使えず、`ensure_loaded(...)` や `remove_dataelement(...)` のような単純タグ専用 API では使えません。

### `dicom.Tag(...)`

- 明示的なタグオブジェクトが必要なときや、同じタグを複数回再利用したいときに使います。
- 利点: 型が明示的で、再利用しやすく、API 境界でも扱いやすいです。
- トレードオフ: 一回きりの呼び出しでは Python レベルの `Tag` オブジェクト生成が増えます。再利用が不要なら packed int のほうが直接的です。

実際的な推奨事項:

- ほとんどの Python コードで通常のキーワード/タグ文字列にアクセスするには、`"Rows"` を使用します。これには依然として実行時のキーワード/タグ解析コストが若干かかりますが、DicomSDL は最適化された実行時のキーワード パスとプレーンなキーワード文字列の軽量の直接パスを使用するため、通常はオーバーヘッドが小さくなります。
- プレーンタグが既に数値定数または外部メタデータから取得されている場合、またはプレーンタグの最速パスが必要な場合は、パック整数を使用します。
- ネストされた値または代入を 1 ステップで行う場合は、ドット区切りのタグパス文字列を使用します。 Python では、走査が 1 つの C++ パス解析/検索呼び出し内に留まるため、ネストされた `Sequence` / `DataSet` API 呼び出しを繰り返すよりも高速になる可能性があります。
- 明示的に再利用可能なタグオブジェクトが必要な場合は、`dicom.Tag(...)` を使用します。
- `ds.Rows` は、開発中またはインタラクティブな探索中に便利です。また、`dir()` は現在のパブリック標準キーワードを公開しているため、多くのインタラクティブ シェルでのタブ補完ともうまく機能します。ただし、キーワードが不明な場合、または要素が欠落している場合は、`AttributeError` が発生します。製品コードの場合、通常、string/int/`Tag` キーを明示的に処理する方が簡単です。

## 値の読み取り

### 属性アクセスは型指定された値を返します

```python
rows = ds.Rows
patient_name = ds.PatientName
```

要素が存在することが予想され、メタデータではなく実際の値が必要な場合にこれを使用します。

### インデックスアクセスは DataElement を返します

```python
elem = ds["Rows"]
if elem:
    print(elem.tag, elem.vr, elem.length, elem.value)
```

ルックアップが見つからない場合は、発生するのではなく偽のセンチネルが返されます。

```python
missing = ds["NotARealKeyword"]
assert not missing
assert missing.value is None
```

### 存在チェック

要素が存在するかどうかだけを知りたい場合は、`in` を使用します。

```python
if "Rows" in ds:
    rows = ds["Rows"].value

if dicom.Tag("PatientName") in df:
    print(df["PatientName"].value)
```

受け入れられるキーのタイプは次のとおりです。

- `str` キーワードまたはタグパス文字列
- `Tag`
- `0x00280010` などのパックされた `int`

不正な形式のキーワード/タグ文字列は `False` を返します。

### メソッド形式での同じ検索: get_dataelement(...)

`get_dataelement(...)` は、`ds[...]` と同じ検索を実行します。一部のコードベースでは、より明確に読み取れる名前付きメソッド形式が好まれます。

```python
elem = ds.get_dataelement("PatientName")
if elem:
    print(elem.vr, elem.length, elem.value)
```

`ds[...]` と同じ欠落要素センチネル動作を使用します。

### 部分ロード継続

必要なタグの前で部分的な読み取りが停止した場合は、`ensure_loaded(...)` を使用します。

```python
df.ensure_loaded("Rows")
df.dataset.ensure_loaded(dicom.Tag("Columns"))
```

受け入れられるキーのタイプは次のとおりです。

- `Tag`
- `0x00280010` などのパックされた `int`
- `"Rows"` や `"(0028,0010)"` などのキーワードまたはタグ文字列

ネストされたドット区切りのタグパス文字列は、`ensure_loaded(...)` ではサポートされていません。

### 高速パス: get_value()

`None` などのデフォルトが欠落要素を表す場合、ワンショット値の読み取りには `get_value()` を使用します。

```python
rows = ds.get_value("Rows")
window_center = ds.get_value("WindowCenter", default=None)
```

これは、`DataElement` オブジェクトが不要な場合に、最も直接的に値を読む方法です。
要素が欠落している場合は、`default` が返されます。

`get_value()` は暗黙的に部分ロードを継続しません。ファイルにバックアップされたデータセットが
前のタグまでのみロードされ、後のタグをクエリすると、現在利用可能なタグが返されます。
状態。部分読み取りの後で `Rows` のような後続タグが必要になった場合は、先に `ensure_loaded(...)` を呼び出します。

指定したデフォルト値は、欠落している要素にのみ使われます。長さゼロの値を持つデータ要素は、依然として型付きの空値を返します。

```python
assert ds.get_value("PatientName", default="DEFAULT") == "DEFAULT"  # missing
assert ds.get_value("Rows", default="DEFAULT") == []                # present, zero-length US
```

### チェーンされたパス: ds["Rows"].value

メタデータと値を一緒に扱いたい場合は、この経路を使います。

```python
rows_elem = ds["Rows"]
if rows_elem:
    print(rows_elem.vr)
    print(rows_elem.value)
```

### `DataElement.value` / `get_value()` の戻り値の型

- `SQ` / `PX` -> `Sequence` / `PixelSequence`
- 数値のような VR (`IS`、`DS`、`AT`、`FL`、`FD`、`SS`、`US`、`SL`、`UL`、`SV`、 `UV`) -> `int`、`float`、`Tag`、または `list[...]`
- `PN` -> 解析成功時は `PersonName` または `list[PersonName]`
- 文字セット対応テキスト VR -> UTF-8 `str` または `list[str]`
- 文字セットのデコードまたは `PN` 解析の失敗 -> 生の `bytes`
- バイナリ VR -> 読み取り専用 `memoryview`

長さゼロの値を持つデータ要素の場合:

- 数値のような VR は `[]` を返します
- テキスト VR は `""` を返します
- `PN` は空の `PersonName` を返します
- バイナリ VR は空の読み取り専用 `memoryview` を返します。
- `SQ` / `PX` は空のコンテナ オブジェクトを返します

これは、基礎となる C++ ベクトル アクセサーと一致します。長さ 0 の数値のような値は、欠損値ではなく、解析可能な空のベクトルとして扱われます。

### 長さゼロの戻り行列

最も重要なルールは、長さゼロの値を持つデータ要素が依然として *存在* するということです。 `default` 引数は使用されず、欠落した検索のように動作しません。

特に、一部の文字列 VR は通常 `VM > 1` を持つことができますが、`vm()` は `> 1` ではなく `0` であるため、長さ 0 の値は空のスカラー スタイルの値として読み戻されます。

| VRファミリー |空ではない `VM == 1` |空ではない `VM > 1` |長さゼロの値 |
| --- | --- | --- | --- |
| `AE`、`AS`、`CS`、`DA`、`DT`、`TM`、`UI`、`UR` | `str` | `list[str]` | `""` |
| `LO`、`LT`、`SH`、`ST`、`UC`、`UT` | `str` |複数値対応 VR の場合は `list[str]`。それ以外の場合は `str` | `""` |
| `PN` | `PersonName` |解析が成功した場合は `list[PersonName]` |空の`PersonName` |
| `IS`、`DS` | `int` / `float` | `list[int]` / `list[float]` | `[]` |
| `AT` | `Tag` | `list[Tag]` | `[]` |
| `FL`、`FD`、`SS`、`US`、`SL`、`UL`、`SV`、`UV` | `int` / `float` | `list[int]` / `list[float]` | `[]` |
| `OB`、`OD`、`OF`、`OL`、`OW`、`OV`、`UN` | `memoryview` | Python リスト値としては使用されません。空の`memoryview` |
| `SQ`、`PX` |シーケンスオブジェクト |シーケンスのようなコンテナ |空のコンテナ オブジェクト |

例:

```python
assert ds.get_value("ImageType") == ["ORIGINAL", "PRIMARY"]
assert ds.get_value("ImageType", default="DEFAULT") == ""   # present, zero-length CS

assert ds.get_value("PatientName", default="DEFAULT") == "DEFAULT"  # missing
assert str(ds["PatientName"].value) == ""                           # present, zero-length PN

assert ds.get_value("Rows", default="DEFAULT") == []               # present, zero-length US
assert ds.get_value("WindowCenter", default="DEFAULT") == []       # present, zero-length DS
```

直接ベクトル アクセサーの場合、長さ 0 の値も `None` ではなく空のコンテナーを返します。

```python
assert ds["Rows"].to_longlong_vector() == []
assert ds["WindowCenter"].to_double_vector() == []
assert ds["FrameIncrementPointer"].to_tag_vector() == []
```

C++ 層では、同じ規約がベクトル アクセサーに適用されるようになりました。

```cpp
auto rows = dataset["Rows"_tag].to_longlong_vector();   // engaged optional, empty vector when zero-length
auto wc = dataset["WindowCenter"_tag].to_double_vector();
auto at = dataset["FrameIncrementPointer"_tag].to_tag_vector();
```

### 長さゼロと欠落の区別

DicomSDL では、`missing` と `zero-length` は異なる要素の状態であるため、`elem.value` だけを調べるのではなく、`DataElement` レベルでテストする必要があります。

このルールを使用します。

```python
elem = ds["PatientName"]

if not elem:
    # ルックアップがありません
elif elem.length == 0:
    # 長さゼロの値を持つ現在の要素
else:
    # 空ではない値を持つ現在の要素
```

実際的な違い:

- 欠落している要素
- `bool(elem) == False`
- `elem.is_missing() == True`
- `elem.vr == dicom.VR.None`
- `elem.value is None`
- 長さゼロの存在要素
- `bool(elem) == True`
- `elem.is_missing() == False`
- `elem.vr != dicom.VR.None`
- `elem.length == 0`

この区別は、「存在するが空」が「存在しない」とは意味的に異なる DICOM 属性にとって重要です。

## データ要素

`DataElement` は、メインのメタデータを持つオブジェクトです。

### コアプロパティ

- `elem.value`
- `elem.tag`
- `elem.vr`
- `elem.length`
- `elem.offset`
- `elem.vm`

これらはプロパティであり、メソッド呼び出しではありません。

```python
elem = ds["Rows"]
print(elem.tag)
print(elem.vr)
print(elem.length)
```

### 真偽値判定と欠落要素オブジェクト

```python
elem = ds["PatientName"]
if elem:
    ...

missing = ds["NotARealKeyword"]
assert not missing
assert missing.is_missing()
```

`bool(elem)` は `elem.is_present()` と一致します。

長さがゼロの存在要素の場合、`bool(elem)` は依然として `True` です。 `elem.length == 0` を使用してそれらを検出します。

### 型付き読み取り/書き込みヘルパー

- `elem.get_value()` は `elem.value` をミラーリングします
- `elem.set_value(value)` は、`value` セッターをミラーリングし、`True`/`False` を返します。
- `elem.set_value(value)` が失敗すると、所有するデータセットは有効のままになりますが、ターゲット要素の状態は指定されません
- 型変換ヘルパーには、`to_long()`、`to_double()`、`to_string_view()`、`to_utf8_string()`、`to_utf8_strings()`、`to_person_name()`、および関連するベクター系 API が含まれます。

### 生バイト

`value_span()` は、コピーせずに読み取り専用の `memoryview` を返します。

```python
raw = ds.get_dataelement("PixelData").value_span()
print(raw.nbytes)
```

## 値の書き込み

### 参照の確保または作成

`ensure_dataelement(...)` は、チェーンに適した「この要素が存在することを確認する」API です。

```python
rows = ds.ensure_dataelement("Rows")
private_value = ds.ensure_dataelement(0x00090030, dicom.VR.US)
```

ルール:

- 要素がすでに存在し、`vr` が省略されるか、`None` の場合、既存の要素が変更されずに返されます。
- 要素がすでに存在し、`vr` が明示的であるが異なる場合、既存の要素はリセットされます
要求された VR が保証されるように適切に配置されています
- 要素が欠落している場合は、長さゼロの新しい要素が挿入されます
- `add_dataelement(...)` とは異なり、この API は明示的な VR を強制する必要がある場合にのみリセットされます。
- 部分的にロードされたファイルベースのデータセットで、タグの `ensure_dataelement(...)` を呼び出します。
データ要素はまだ解析されていないため、暗黙的にロードを継続する代わりに発生します。
### 返された DataElement を通じて既存の要素を更新します

```python
ds["Rows"].value = 512
```

要素オブジェクトをすでに持っている場合、これは自然なパスです。

### set_value() によるワンショット代入

```python
assert ds.set_value("Rows", 512)
assert ds.set_value("StudyDescription", "Example")
assert ds.set_value("Rows", None)   # present, zero-length US
```

これは、1 回の呼び出しでキーによる作成/更新を行う場合に最適なパスです。

部分的にロードされたファイルバッキング データセットでは、`set_value(...)` がターゲットを介してロードされません
タグ。ターゲットのデータ要素がまだ解析されていない場合は、次のように発生します。
`add_dataelement(...)` / `ensure_dataelement(...)`。

失敗時の挙動:

- 成功すると、要求された値が書き込まれます
- 失敗した場合、`set_value()` は `False` を返します
- `DataSet` / `DicomFile` は引き続き使用可能です
- 宛先要素の状態は指定されていないため、依存すべきではありません

ロールバック動作が必要な場合は、以前の値を自分で保持し、明示的に復元してください。

### 長さゼロの値の作成と要素の削除

`None` は、要素は存在するが値の長さが 0 であることを意味します。

```python
assert ds.set_value("PatientName", None)   # present, zero-length PN
assert ds.set_value("Rows", None)          # present, zero-length US
```

`None` は、解決された VR に対応する長さ 0 の表現を簡潔に書くための省略形です。
明示的な空ペイロードを使って、同じ意図を表すこともできます。

```python
ds.add_dataelement(dicom.Tag("PatientName"), dicom.VR.PN)   # present, zero-length

assert ds.set_value("PatientName", "")      # zero-length text element
assert ds.set_value("Rows", [])             # zero-length numeric VM-based element
assert ds.set_value(0x00111001, dicom.VR.OB, b"")  # zero-length binary element
```

同じルールが既存の要素にも適用されます。

```python
ds["PatientName"].value = None
ds["PatientName"].value = ""
ds["Rows"].value = None
ds["Rows"].value = []
```

見方:

- `None` -> 長さ 0 の要素を保持するか、新しく作成します
- 空のペイロード (`""`、`[]`、`b""`) -> 要素は存在したまま `length == 0` になります。

削除には `remove_dataelement()` を使用します。

```python
ds.remove_dataelement("PatientName")
ds.remove_dataelement(0x00280010)
ds.remove_dataelement(dicom.Tag("Rows"))
```

### プライベートタグまたはあいまいなタグに対する明示的な VR 割り当て

```python
assert ds.set_value(0x00090030, dicom.VR.US, 16)
```

この形式は、タグがプライベートである場合、または値を割り当てる前に既存の非シーケンス要素 VR を上書きしたい場合に便利です。

ルール:

- 要素が欠落している場合は、提供された VR を使用して要素が作成されます
- 要素が存在し、その VR がすでに存在する場合、値はその場で更新されます
- 要素が別の非 `SQ`/非 `PX` VR で存在する場合、バインディングはその VR を置き換えてから値を割り当てる可能性があります。
- `VR.None`、`SQ`、および `PX` は、この形式で有効な上書き対象ではありません

この形式ではロールバック動作は提供されません。`False` が返ってもデータセット自体は有効なままですが、対象要素の状態は未定義です。

### 属性代入の簡便な書き方

```python
ds.Rows = 512
df.PatientName = pn
```

属性代入は、標準のキーワード ベース更新をより簡潔に書くための構文です。
`DataSet` でも、`DicomFile` 経由で公開される同等のアクセスでも使えます。
`set_value(...)` と違って、この経路は代入に失敗すると `False` を返すのではなく例外を送出します。
そのため、通常は開発、ノートブック、対話的な利用に向いており、明示的なエラー処理が必要な本番コードでは `set_value(...)` の方が適しています。

## ユーティリティの操作

### 反復とサイズ

`for elem in ds` は、そのデータセット内の現在の要素を反復処理します。
`ds.size()` は、そのデータセットの要素数を返します。
`len(df)` は、`DicomFile` のルート データセット サイズを返します。

```pycon
>>> for elem in ds:
...     print(elem.tag, elem.vr, elem.length)
(0002,0010) UI 20
(0010,0010) PN 8
(0028,0010) US 2
>>> ds.size()
42
>>> len(df)
42
```

すでにデータセット オブジェクトを持っているなら `ds.size()` を使います。
ファイル オブジェクトを基準に作業しているなら `len(df)` を使います。

### dump()

`dump()` は、`DicomFile` と `DataSet` のどちらでも、人が読みやすいタブ区切りのダンプ文字列を返します。

```python
full_text = df.dump(max_print_chars=80, include_offset=True)
compact_text = ds.dump(max_print_chars=40, include_offset=False)
```

- `max_print_chars` は、長い `VALUE` プレビューを切り捨てます。
- `include_offset=False` は、`OFFSET` 列を削除します。
- ファイル ベースのルート データセットでは、`dump()` はダンプを整形する前に、まだ読み込まれていない残りの要素も読み込みます。

代表的な出力は次のようになります。

```text
TAG	VR	LEN	VM	OFFSET	VALUE	KEYWORD
'00020010'	UI	20	1	132	'1.2.840.10008.1.2.1'	TransferSyntaxUID
'00100010'	PN	8	1	340	'Doe^Jane'	PatientName
'00280010'	US	2	1	702	512	Rows
```

`include_offset=False` を使用すると、ヘッダーと列は次のようになります。

```text
TAG	VR	LEN	VM	VALUE	KEYWORD
'00100010'	PN	8	1	'Doe^Jane'	PatientName
```

## 追加の注意事項

### パフォーマンスノート

- キーワードとタグの検索では、定数時間辞書パスが使用されます。
- 大きなファイルの場合は、Python ホット ループでの完全な反復よりもターゲットの要素へのアクセスを優先します。

### ピクセル変換メタデータ

以下のフレーム対応メタデータ解決:

- `DicomFile.rescale_transform_for_frame(frame_index)`
- `DicomFile.window_transform_for_frame(frame_index)`
- `DicomFile.voi_lut_for_frame(frame_index)`
- `DicomFile.modality_lut_for_frame(frame_index)`

[ピクセル変換メタデータ解像度](../reference/pixel_transform_metadata.md) に記載されています。

### 実行可能な例

- `examples/python/dataset_access_example.py`
- `examples/python/dump_dataset_example.py`

## 関連ドキュメント

- C++ 対応物: [C++ DataSet ガイド](cpp_dataset_guide.md)
- 入出力動作: [ファイル I/O](file_io.md)
- ファイルレベルの API サーフェス: [DicomFile Reference](../reference/dicomfile_reference.md)
- `DataElement` 詳細: [DataElement リファレンス](../reference/dataelement_reference.md)
- `Sequence` トラバーサル: [シーケンス参照](../reference/sequence_reference.md)
- 例外と失敗のカテゴリ: [エラー処理](error_handling.md)
- デコードされたピクセル出力: [ピクセルデコード](pixel_decode.md)
- テキスト VR および `PN`: [文字セットと人物名](charset_and_person_name.md)
- サポートする Python タイプ: [Python API リファレンス](../reference/python_reference.md)
- UID 生成の詳細: [UID の生成](generating_uid.md)
- ピクセル エンコードの制限: [ピクセル エンコードの制約](../reference/pixel_encode_constraints.md)
