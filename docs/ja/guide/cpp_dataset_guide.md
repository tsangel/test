# C++ データセット ガイド

これは、`dicomsdl` で C++ からデータセットと要素を扱うための主要なガイドです。
主要なオブジェクトの関係、タグの表記方法、重要な読み書きパターンを説明します。

## dicomsdl と DICOM のマッピング方法

`dicomsdl` は、関連する C++ オブジェクトの小さなセットを公開します。

- `DicomFile`: ルート データセットを所有するファイル/セッション ラッパー
- `DataSet`: DICOM `DataElement` オブジェクトのコンテナー
- `DataElement`: タグ / VR / 長さのメタデータと型付き / 生の値アクセスを持つ 1 つの DICOM フィールド
- `Sequence`: `SQ` 値のネストされた項目コンテナー
- `PixelSequence`: カプセル化または圧縮されたピクセル データのフレーム/フラグメント コンテナー

オブジェクト モデルと DICOM マッピングについては、[Core Objects](core_objects.md) を参照してください。

C++ には Python スタイルの属性アクセスの簡便記法はありません。
タグ、キーワード、ドット区切りのタグパスは明示的に書きます。

### DicomFile と DataSet

ほとんどのデータ要素アクセス API は `DataSet` に実装されています。
`DicomFile` はルート `DataSet` を所有し、ロード、保存、トランスコードなどのファイル指向の作業を処理します。
便宜上、`DicomFile` は `get_value(...)` などの多くのルート データセット ヘルパーを転送します。
`get_dataelement(...)`、`set_value(...)`、`ensure_dataelement(...)`、および `ensure_loaded(...)`。

ファイルレベルの操作と混合したいくつかのルートレベルの読み取りの場合、多くの場合、`DicomFile` 転送で十分です。

```cpp
#include <dicom.h>
using namespace dicom::literals;

auto file = dicom::read_file("sample.dcm");

long rows1 = file->get_value<long>("Rows"_tag, 0L);
const auto& patient_name1 = file->get_dataelement("PatientName"_tag);
```

データセット作業を繰り返す場合は、通常、`DataSet` を明示的に取得する方が明確です。

```cpp
#include <dicom.h>
using namespace dicom::literals;

auto file = dicom::read_file("sample.dcm");
auto& ds = file->dataset();

long rows2 = ds.get_value<long>("Rows"_tag, 0L);
const auto& patient_name2 = ds["PatientName"_tag];
```

## 推奨 API

| API | 戻り値 | 欠落時の動作 | 用途 |
| --- | --- | --- | --- |
| `ds.get_value<long>("Rows"_tag)` | `std::optional<long>` | `std::nullopt` | `std::nullopt` が欠落を表す型付き読み取り |
| `ds.get_value<long>("Rows"_tag, 0L)` | `long` | 指定したデフォルト値を返す | 一度で完結する型付き読み取り |
| `ds["Rows"_tag]`、`ds["Rows"]`、`ds.get_dataelement("Rows")` | `DataElement&` | `VR::None` で `false` と評価される要素を返します |型付き読み取り + メタデータアクセス |
| `if (const auto& e = ds["Rows"_tag]; e)` | 存在に応じて分岐 | 見つからなければ `false` | 存在有無を区別したいコード |
| `ds.ensure_loaded("(0028,FFFF)"_tag)` | `void` | 無効な使用では例外 | 後続のタグ境界まで部分読み取りを明示的に続行 |
| `ds.ensure_dataelement("Rows"_tag, dicom::VR::US)` | `DataElement&` | 既存要素を返すか新規挿入 | チェーンしやすい ensure/create |
| `ds.set_value("Rows"_tag, 512L)` | `bool` | エンコード/代入失敗で `false` | 一度で行う ensure + 型付き代入 |
| `ds.add_dataelement("Rows"_tag, dicom::VR::US)` | `DataElement&` |作成/置換 |明示的なリーフの挿入 |

## C++ でタグを表記する方法

ユーザー定義のリテラル接尾辞は、`"_Tag"` ではなく、小文字の `"_tag"` です。

| 形式 | 例 | まず使う場面 |
| --- | --- | --- |
| キーワードリテラル | `"Rows"_tag` | 日常的な C++ コードで大半の標準タグを扱うとき |
| 数値タグリテラル | `"(0028,0010)"_tag` | 数値タグ表記のほうが分かりやすいとき |
| グループ/要素コンストラクター | `dicom::Tag(0x0028, 0x0010)` | group と element が別の値としてすでにあるとき |
| パック済みタグ値コンストラクター | `dicom::Tag(0x00280010)` | タグが packed `0xGGGGEEEE` 値としてすでにあるとき |
| 実行時テキスト解析 | `dicom::Tag("Rows")`, `dicom::Tag("(0028,0010)")` | キーワードや数値タグが実行時文字列で渡されるとき |
| 文字列/パス形式 | `ds["Rows"]`, `ds.get_value<double>("00540016.0.00181074")` | キーワード検索や、一度で済むネスト読み書きをしたいとき |

### `"Rows"_tag`

- 日常的な C++ コードで大半の標準タグを扱うときは、まずこれを使います。
- 利点: 短く読みやすく、コンパイル時にチェックされ、実行時の解析がありません。
- トレードオフ: コンパイル時に分かっている文字列リテラルでしか使えません。

### `"(0028,0010)"_tag`

- キーワードより数値タグ表記のほうが分かりやすいときに使います。
- 利点: 曖昧さがなく、コンパイル時にチェックされ、Tag 専用 API でも使えます。
- トレードオフ: キーワードより見た目が重く、タイプミスもしやすくなります。

### `dicom::Tag(0x0028, 0x0010)`

- group と element が別々の実行時値としてすでにあるときに使います。
- 利点: 明示的で、実行時値でも使え、テキスト解析が不要です。
- トレードオフ: キーワードリテラルより冗長です。

### `dicom::Tag(0x00280010)`

- タグが packed `0xGGGGEEEE` 値としてすでにあるときに使います。
- 利点: 生成されたテーブルや packed tag 値と連携するときにコンパクトです。
- トレードオフ: bare `0x00280010` は `DataSet` API にそのまま渡せません。`Tag(...)` または `Tag::from_value(...)` で包む必要があります。

### `dicom::Tag("Rows")` または `dicom::Tag("(0028,0010)")`

- キーワードや数値タグが実行時文字列で渡されるときに使います。
- 利点: キーワード文字列と数値タグ文字列のどちらも受け付けます。
- トレードオフ: 実行時に解析され、不正な文字列では例外が発生する可能性があります。

### 文字列 / パス形式

- キーワード検索、ドット区切りのタグパス走査、またはワンショットのネストされた読み書きをしたいときに使います。
- 例: `ds["Rows"]`, `ds.get_value<double>("00540016.0.00181074")`, `ds.set_value("PatientName", "Doe^Jane")`, `ds.ensure_dataelement("ReferencedStudySequence.0.ReferencedSOPInstanceUID", dicom::VR::UI)`.
- 利点: `Tag` を明示的に組み立てなくてもよく、`operator[]`, `get_dataelement(...)`, `get_value(...)`, `set_value(...)`, `ensure_dataelement(...)`, `add_dataelement(...)` でネストパスを扱えます。
- トレードオフ: 文字列の解析は実行時に行われます。

実用的な推奨:

- ほとんどの C++ コードでの通常のタグ アクセスには `"Rows"_tag` を使用します
- 数値タグ表記が最も分かりやすい場合は、`"(0028,0010)"_tag` を使用します。
- タグが実行時の整数またはパックされた値に由来する場合は、`dicom::Tag(...)` を使用します。
- ネストされた検索または 1 ステップでの書き込みが必要な場合は、文字列/パス形式を使用します (例: `ds.get_value<double>("RadiopharmaceuticalInformationSequence.0.RadionuclideTotalDose")`、`ds.get_value<double>("00540016.0.00181074")`、`ds.set_value("ReferencedStudySequence.0.ReferencedSOPInstanceUID", "1.2.3")`、または `ds.ensure_dataelement("ReferencedStudySequence.0.ReferencedSOPInstanceUID", dicom::VR::UI)`)。

## 値の読み取り

### 高速パス: get_value<T>()

型付きの値だけが必要な場合は、`get_value<T>()` を使います。

```cpp
#include <dicom.h>
using namespace dicom::literals;

auto file = dicom::read_file("sample.dcm");
auto& ds = file->dataset();

if (auto rows = ds.get_value<long>("Rows"_tag)) {
  // *rowsを使用してください
}

double slope = ds.get_value<double>("RescaleSlope"_tag, 1.0);
auto desc = ds.get_value<std::string_view>("StudyDescription"_tag);
```

- `get_value<T>(tag)` は `std::optional<T>` を返します。呼び出し側で「欠落」と「実際の値を持つ存在」を区別したい場合にこれを使用します。
- `get_value<T>(tag, default_value)` は `T` を返します。インラインの代替値が欲しく、欠落と代替値の経路を区別しなくてよい場合に使います。
- デフォルト値つきの形式は、実質的に `get_value<T>(...).value_or(default_value)` です。
- `get_value<std::string_view>(...)` はゼロコピー ビューです。使用している間、所有するデータセット/ファイルを維持します。

### データ要素アクセス: 演算子[]

値だけでなく `DataElement` が必要な場合は、`operator[]` を使用します。

```cpp
#include <dicom.h>
using namespace dicom::literals;

auto file = dicom::read_file("sample.dcm");
auto& ds = file->dataset();

const auto& rows_elem = ds["Rows"];
if (rows_elem) {
  long rows = rows_elem.to_long(0L);
}

const auto& dose_elem =
    ds["RadiopharmaceuticalInformationSequence.0.RadionuclideTotalDose"];
```

`operator[]` は、`Tag`、キーワード文字列、またはドット区切りのタグ パスを受け入れます。
実行時の解析を行わずにコンパイル時のタグ表記を使いたい場合は、`"_tag"` を使用します。

### 存在チェック

存在自体が重要な場合は、返された `DataElement` の真偽値判定を使います。

```cpp
if (const auto& rows_elem = ds["Rows"_tag]; rows_elem) {
  long rows = rows_elem.to_long(0L);
}

if (const auto& patient_name = ds.get_dataelement("PatientName"); patient_name) {
  // 存在する要素
}
```

見つからないルックアップでは例外ではなく、`VR::None` を持ち `false` と評価される要素が返ります。

### メソッド形式での同じ検索: get_dataelement(...)

`get_dataelement(...)` は、`operator[]` と同じ検索を実行します。一部のコードベースでは、名前付き関数のほうが `ds[...]` より分かりやすく読める場合、メソッド形式を優先します。

```cpp
const auto& dose = ds.get_dataelement(
    "RadiopharmaceuticalInformationSequence.0.RadionuclideTotalDose");
if (dose) {
  double value = dose.to_double(0.0);
}
```

### 部分ロード継続

必要なタグの前で部分的な読み取りが停止した場合は、`ensure_loaded(tag)` を使用します。

```cpp
ds.ensure_loaded("(0028,FFFF)"_tag);

long rows = ds.get_value<long>("Rows"_tag, 0L);
long cols = ds.get_value<long>("Columns"_tag, 0L);
```

`ensure_loaded(...)` は、`"Rows"_tag`、`"(0028,FFFF)"_tag`、`dicom::Tag(0x0028, 0x0010)` などの `Tag` を受け取ります。
キーワード文字列やドット区切りのタグ パスは使用しません。

### 値の型と長さゼロの動作

主な型付き読み取りファミリーは次のとおりです。

- スカラー数値: `to_int()`、`to_long()`、`to_longlong()`、`to_double()`
- ベクトル数値/タグ: `to_longlong_vector()`、`to_double_vector()`、`to_tag_vector()`
- テキスト: `to_string_view()`、`to_string_views()`、`to_utf8_string()`、`to_utf8_strings()`
- タグとUID: `to_tag()`、`to_uid_string()`、`to_transfer_syntax_uid()`
- 人物名: `to_person_name()`、`to_person_names()`

ベクトル アクセサーの場合、長さ 0 の値は、`std::nullopt` ではなく、エンゲージされた空のコンテナーを返します。

```cpp
auto rows = ds["Rows"_tag].to_longlong_vector();         // empty vector when zero-length
auto wc = ds["WindowCenter"_tag].to_double_vector();     // empty vector when zero-length
auto at = ds["FrameIncrementPointer"_tag].to_tag_vector();
```

スカラー アクセサーの場合、`std::nullopt` を「このアクセサーからは利用できない」ものとして扱い、
欠落と長さ 0 の存在要素を区別したい場合は、`DataElement` 自体を見ます。

### 長さゼロと欠落の区別

`dicomsdl` では、`missing` と `zero-length` は異なる要素の状態であり、`DataElement` レベルでテストする必要があります。

```cpp
const auto& elem = ds["PatientName"_tag];

if (!elem) {
  // ルックアップがありません
} else if (elem.length() == 0) {
  // 長さ 0 の値を持つ存在要素
} else {
  // 空でない値を持つ存在要素
}
```

実際的な違い:

- 欠落している要素
- `bool(elem) == false`
- `elem.is_missing() == true`
- `elem.vr() == dicom::VR::None`
- 長さゼロの存在要素
- `bool(elem) == true`
- `elem.is_missing() == false`
- `elem.vr() != dicom::VR::None`
- `elem.length() == 0`

## データ要素

`DataElement` は、メインのメタデータを持つオブジェクトです。

### コアプロパティ

- `elem.tag()`
- `elem.vr()`
- `elem.length()`
- `elem.offset()`
- `elem.vm()`
- `elem.parent()`

```cpp
const auto& elem = ds["Rows"_tag];
auto tag = elem.tag();
auto vr = elem.vr();
auto length = elem.length();
```

### 真偽値判定と欠落要素オブジェクト

```cpp
const auto& elem = ds["PatientName"_tag];
if (elem) {
  // 存在する要素
}

const auto& missing = ds["NotARealKeyword"];
if (!missing && missing.is_missing()) {
  // ルックアップがありません
}
```

長さがゼロの存在要素の場合、`bool(elem)` は依然として `true` です。 `elem.length() == 0` を使用してそれらを検出します。

### 型付き読み取り/書き込みヘルパー

- `to_long()`、`to_double()`、`to_tag()`、`to_uid_string()`
- `to_string_view()`、`to_utf8_string()`、`to_utf8_strings()`
- `to_person_name()`、`to_person_names()`
- `from_long(...)`、`from_double(...)`、`from_tag(...)`
- `from_string_view(...)`、`from_utf8_view(...)`、`from_uid_string(...)`
- `from_person_name(...)`、`from_person_names(...)`

すでに `DataElement&` がある場合、`from_xxx(...)` ヘルパーが直接書き込みパスになります。

### コンテナヘルパー

`SQ` およびカプセル化されたピクセル データの場合:

- `elem.sequence()` / `elem.as_sequence()`
- `elem.pixel_sequence()` / `elem.as_pixel_sequence()`

これらをスカラー文字列や数値としてではなく、コンテナ値として扱います。

### Raw バイトとビューの有効期間

`value_span()` はコピーせずに `std::span<const std::uint8_t>` を返します。

```cpp
const auto& pixel_data = ds["PixelData"_tag];
auto bytes = pixel_data.value_span();
// bytes.data()、bytes.size()
```

`to_string_view()` スタイル アクセサーもビューベースです。
要素が置換または変更されるとビューは無効になるため、所有するデータセット/ファイルを生きたままにして、書き込み後にビューを更新します。

## 値の書き込み

### ensure_dataelement(...)

チェーンしやすい ensure/create 動作が必要な場合は、`ensure_dataelement(...)` を使います。

```cpp
auto& existing_rows = ds.ensure_dataelement("Rows"_tag);  // default vr == VR::None
ds.ensure_dataelement("Rows"_tag, dicom::VR::US).from_long(512);
ds.ensure_dataelement(
    "ReferencedStudySequence.0.ReferencedSOPInstanceUID",
    dicom::VR::UI).from_uid_string("1.2.3");
```

ルール:

- 既存の要素 + 省略された `vr` (デフォルト `VR::None`) または明示的な `VR::None` -> そのまま保持
- 既存の要素 + 明示的な異なる VR -> 所定の位置にリセット
- 欠落要素 + 明示的な VR -> その VR を含む長さゼロの要素を挿入
- 標準タグの欠落 + `vr` の省略 -> 辞書 VR を使用して長さ 0 の要素を挿入
- 不明/プライベートタグが欠落 + `vr` が省略 -> 解決する辞書 VR がないためスロー

### 返された DataElement を通じて既存の要素を更新します

すでに要素がある場合は、`DataElement` で `from_xxx(...)` を使用します。

```cpp
if (auto& rows = ds["Rows"_tag]; rows) {
  rows.from_long(512);
}
```

作成または更新の動作が必要な場合は、`operator[]` ではなく `ensure_dataelement(...)` から開始してください。

### set_value(...) によるワンショット代入

1 回の呼び出しで同じ ensure + 型付き書き込みを行いたい場合は、`set_value(...)` を使います。

```cpp
bool ok = true;
ok &= ds.set_value("Rows"_tag, 512L);
ok &= ds.set_value("Columns"_tag, 512L);
ok &= ds.set_value("BitsAllocated"_tag, 16L);
ok &= ds.set_value(dicom::Tag(0x0009, 0x0030), dicom::VR::US, 16L);  // private tag
```

この関数は、既存要素 / 欠落要素の扱いと、明示した `vr` / 省略した `vr` の規則について、
上の `ensure_dataelement(...)` と同じルールに従います。そのうえで、エンコードまたは代入に失敗すると `false` を返します。

失敗時の挙動:

- 成功すると、要求された値が書き込まれます
- 失敗した場合、`set_value()` は `false` を返します
- `DataSet` / `DicomFile` は引き続き使用可能です
- 宛先要素の状態は指定されていないため、依存すべきではありません

ロールバック セマンティクスが必要な場合は、以前の値を自分で保持し、明示的に復元します。

### 長さゼロの値の作成と要素の削除

長さゼロと削除は別の操作です。

`add_dataelement(...)`、`ensure_dataelement(...)`、を使用して長さゼロの存在要素を作成または保存します。
または明示的な空のペイロード:

```cpp
ds.add_dataelement("PatientName"_tag, dicom::VR::PN);  // present element with zero-length value
ds.set_value("PatientName"_tag, std::string_view{});

std::vector<long long> empty_numbers;
ds.set_value("Rows"_tag, std::span<const long long>(empty_numbers));
```

削除には `remove_dataelement(...)` を使用します。

```cpp
ds.remove_dataelement("PatientName"_tag);
ds.remove_dataelement(dicom::Tag(0x0028, 0x0010));
```

### プライベートタグまたはあいまいなタグに対する明示的な VR 割り当て

```cpp
bool ok = ds.set_value(dicom::Tag(0x0009, 0x0030), dicom::VR::US, 16L);
```

この形式は、タグが private である場合や、値を代入する前に既存の非シーケンス要素の VR を変更したい場合に便利です。

ルール:

- 要素が欠落している場合は、提供された VR を使用して要素が作成されます
- 要素が存在し、その VR がすでに存在する場合、値はその場で更新されます
- 要素が別の非 `SQ`/非 `PX` VR で存在する場合、バインディングはその VR を置き換えてから値を割り当てる可能性があります。
- `VR::None`、`SQ`、および `PX` は、このオーバーロードの有効なオーバーライド ターゲットではありません

### add_dataelement(...)

明示的な作成/置換セマンティクスが必要な場合は、`add_dataelement(...)` を使用します。

```cpp
ds.add_dataelement("Rows"_tag, dicom::VR::US).from_long(512);
```

`ensure_dataelement(...)` と比較して、`add_dataelement(...)` は既存の要素に対してより破壊的です。
ターゲット要素がすでに存在する場合、`add_dataelement(...)` は、再度入力する前に、それを新しい長さ 0 の要素にリセットします。

明示的な置換動作が必要な場合は、`add_dataelement(...)` を使用します。
明示的な VR 変更が必要でない限り、既存の要素を保持したい場合は、代わりに `ensure_dataelement(...)` を使用してください。
`set_value(...)` は、`add_dataelement(...)` パスではなく、`ensure_dataelement(...)` パスに従います。

## ユーティリティの操作

### 反復とサイズ

`for (const auto& elem : ds)` は、そのデータセットに含まれる要素を順にたどります。
`ds.size()` は、そのデータセットの要素数を返します。
`file->size()` は、`DicomFile` からルートデータセットのサイズをそのまま返します。

```cpp
for (const auto& elem : ds) {
  std::cout << elem.tag().to_string()
            << ' ' << elem.vr().str()
            << ' ' << elem.length() << '\n';
}

std::cout << "element count: " << ds.size() << '\n';
std::cout << "file count: " << file->size() << '\n';
```

代表的な出力は次のようになります。

```text
(0002,0010) UI 20
(0010,0010) PN 8
(0028,0010) US 2
element count: 42
file count: 42
```

### dump()

`dump()` は、`DataSet` と `DicomFile` の両方について、人間が判読できるタブ区切りのダンプ文字列を返します。

```cpp
auto full = file->dump(80, true);
auto compact = ds.dump(40, false);
```

- `max_print_chars` は、長い `VALUE` プレビューを切り捨てます。
- `include_offset = false` は、`OFFSET` 列を削除します。
- ファイルベースのルートデータセットでは、`dump()` は出力前に未読の残り要素も読み込みます。

代表的な出力は次のようになります。

```text
TAG	VR	LEN	VM	OFFSET	VALUE	KEYWORD
'00020010'	UI	20	1	132	'1.2.840.10008.1.2.1'	TransferSyntaxUID
'00100010'	PN	8	1	340	'Doe^Jane'	PatientName
'00280010'	US	2	1	702	512	Rows
```

`include_offset = false` を使用すると、ヘッダーと列は次のようになります。

```text
TAG	VR	LEN	VM	VALUE	KEYWORD
'00100010'	PN	8	1	'Doe^Jane'	PatientName
```

## 部分ロードのルール

- `get_value(...)`、`get_dataelement(...)`、および `operator[]` は暗黙的に部分ロードを継続しません。
- まだ解析されていないデータ要素は、`ensure_loaded(tag)` を呼び出すまで欠落しているものとして動作します。
- `add_dataelement(...)`、`ensure_dataelement(...)`、`set_value(...)` は、対象の要素がまだ解析されていない場合に例外を出します。
- staged read のあとで後続タグが必要になったら、まずロード境界を明示的に進めてから読み書きします。

## 追加の注意事項

### パフォーマンスノート

- ホットパスで通常の単一タグアクセスを行う場合は、ランタイム テキスト解析よりも `"_tag"` リテラルまたは再利用した `dicom::Tag` オブジェクトを優先します。
- 型付きの結果だけが必要で、`DataElement` メタデータが不要な場合は、`get_value<T>(tag, default)` を優先します。
- ネストしたアクセスを分かりやすく書きたい場合は、文字列 / パス形式を使います。ホットループで同じ検索を繰り返す場合は、タグをキャッシュするか、走査を明示的に分けます。

### 実行可能な例

- `examples/dataset_access_example.cpp`
- `examples/batch_assign_with_error_check.cpp`
- `examples/dump_dataset_example.cpp`
- `examples/tag_lookup_example.cpp`
- `examples/keyword_lookup_example.cpp`

## 関連ドキュメント

- [コアオブジェクト](core_objects.md)
- [ファイルI/O](file_io.md)
- [シーケンスとパス](sequence_and_paths.md)
- [Python データセット ガイド](python_dataset_guide.md)
- [C++ API の概要](../reference/cpp_api.md)
- [データセットリファレンス](../reference/dataset_reference.md)
- [データ要素リファレンス](../reference/dataelement_reference.md)
- [エラー処理](error_handling.md)
