# エラー処理

このページは、各ガイドに分散しているエラー処理のパターンをまとめたものです。次の 2 つを手早く確認したいときに参照してください。

- どの API が例外を送出しますか?
- 失敗した場合、次に何をすればよいでしょうか?

## 失敗の種類

dicomsdl パブリック API は、次の 3 つの異なる失敗スタイルを使用します。

- 例外の送出
  - 高レベルの C++ 読み取り、書き込み、デコード、エンコード、およびデータセット全体の文字セット変更 API は、通常 `dicom::diag::DicomException` で失敗を報告します。
  - Python では、同じ実行時失敗が主に `RuntimeError` として見え、バインディング側の引数検証エラーは `TypeError`、`ValueError`、`IndexError` として報告されます。
- 戻り値ベースの失敗
  - 一部の要素レベル文字セット API は例外 API ではなく、`false`、`None`、空の `optional` といった戻り値で失敗を報告します。
- エラー状態を記録した部分成功
  - `read_file(..., keep_on_error=True)` と `read_bytes(..., keep_on_error=True)` は `DicomFile` を返すことがありますが、その場合は `has_error` と `error_message` が設定されます。

## 例外処理パターン

**C++**

```cpp
try {
    // 高レベルの dicomsdl 操作
} catch (const dicom::diag::DicomException& ex) {
    // ユーザー側の DICOM、コーデック、またはファイル I/O エラー
} catch (const std::exception& ex) {
    // 下位レベルの前提条件違反またはプラットフォーム依存の障害
}
```

**パイソン**

```python
import dicomsdl as dicom

try:
    # 高レベルの dicomsdl 操作
    ...
except TypeError as exc:
    # 間違った引数の型または非バッファ/パスのような誤用
    ...
except ValueError as exc:
    # 無効なテキスト オプション、無効なバッファ/レイアウト指定、または設定の不整合
    ...
except IndexError as exc:
    # フレームまたはコンポーネントのインデックスが範囲外です
    ...
except RuntimeError as exc:
    # 基礎となる C++ の解析、デコード、エンコード、トランスコード、または書き込みの失敗
    ...
```

## ファイル I/O

解析上の問題が発生してファイルを直ちに拒否する必要がある場合は、`keep_on_error=False` を使用します。ファイルの形式が後で不正であることが判明した場合でも、初期のメタデータがまだ役立つ場合は、`keep_on_error=True` を使用します。

### `keep_on_error=False`: フェイルファスト

- これは、インポート パイプライン、検証ジョブ、または不正なファイルを直ちに停止する必要があるワークフローに使用します。
- あらゆる例外を「このファイルを処理し続けるのは安全ではない」ものとして扱います。
- パスと例外テキストをログに記録し、ファイルを隔離、スキップ、または報告します。

### `keep_on_error=True`: すでに解析されたものを保持します

- これは、初期のタグから引き続きメリットを享受できるクローラー、メタデータのインデックス作成、トリアージ ツール、または修復ツールに使用します。
- 許可読み取りを行うたびに、結果を信頼する前に `has_error` と `error_message` をチェックしてください。
- `has_error` が true の場合、オブジェクトは部分的に読み取られたか汚染されているものとして処理されます。
- 意図的に復旧したいメタデータのみを使用してください
- 盲目的にピクセル デコード、ピクセル エンコード、またはライトバック フローを続行しないでください。
- 完全に信頼できるオブジェクトが必要な場合は、修復後に厳密にリロードしてください
- `keep_on_error` は、一般的な「すべてのエラーを無視する」スイッチではありません。パス/オープンの失敗、無効な Python バッファー コントラクト、および同様の境界エラーは、引き続き即座に発生します。

### 例

**C++**

```cpp
#include <dicom.h>
#include <diagnostics.h>
#include <iostream>

try {
    dicom::ReadOptions opts;
    opts.keep_on_error = true;

    auto file = dicom::read_file("in.dcm", opts);
    if (file->has_error()) {
        std::cerr << "partial read: " << file->error_message() << '\n';
        // 明示的に復旧したいメタデータだけを保持します。
        // これがクリーンなファイルであるかのようにデコード/トランスコードを続行しないでください。
    }
} catch (const dicom::diag::DicomException& ex) {
    // ファイルオープンの失敗、または keep_on_error の別の境界エラー
    // 部分復帰状態にはなりません。
    std::cerr << ex.what() << '\n';
}
```

**パイソン**

```python
import dicomsdl as dicom

df = dicom.read_file("in.dcm", keep_on_error=True)
if df.has_error:
    print("partial read:", df.error_message)
    # 検査しようとしていた、すでに解析されたメタデータのみを使用してください。
    # デコード/トランスコード/書き込みワークフローの前に厳密にリロードしてください。
```

### スロー/レイズ可能なファイル I/O API

| APIファミリー | C++ 失敗フォーム | Python はレイズします |典型的な理由 |
| --- | --- | --- | --- |
| `read_file(...)` |厳密な読み取りが失敗した場合は `dicom::diag::DicomException`。 `keep_on_error=true` を使用すると、解析失敗は代わりに返された `DicomFile` でキャプチャされます。 `TypeError`、`RuntimeError` |パスを開けない、厳密な解析が失敗する、または Python パス引数が `str` / `bytes` / `os.PathLike` ではありません。
| `read_bytes(...)` |厳密な読み取りが失敗した場合は `dicom::diag::DicomException`。 `keep_on_error=true` を使用すると、解析失敗は代わりに返された `DicomFile` でキャプチャされます。 `TypeError`、`ValueError`、`RuntimeError` |バッファーは 1 次元の連続したバイトのようなデータではありません。`copy=False` が非バイト要素で使用されているか、解析が失敗します。
| `write_file(...)` | `dicom::diag::DicomException` | `TypeError`、`RuntimeError` |出力パスが無効です。ファイルのオープン/フラッシュが失敗します。ファイル メタの再構築が失敗します。または、データセットを現在の状態でシリアル化できません。
| `write_bytes(...)` | `dicom::diag::DicomException` | `RuntimeError` |ファイル メタの再構築が失敗するか、現在のデータセットをきれいにシリアル化できません。
| `write_with_transfer_syntax(...)` | `dicom::diag::DicomException` | `TypeError`、`ValueError`、`RuntimeError` |出力パスが無効です、転送構文の選択が無効です、エンコーダのコンテキスト/オプションがリクエストと一致しません、トランスコードが失敗する、または出力の書き込みが失敗します。

## ピクセルデコード

最も安全なデコード パターンは次のとおりです。

1. 事前に `DecodePlan` を作成します
2. その計画から目的地を割り当てます
3. 同じ検証済みのプランと宛先コントラクトをデコード呼び出し全体で再利用します。

デコードが失敗した場合は、まず 3 つのバケットのいずれかを想定します。つまり、不正な呼び出し元コントラクト、古いレイアウト仮定、または実際のバックエンド/ランタイム デコード失敗です。

### デコードが失敗した場合の対処方法

- デコードが開始される前に検証が失敗した場合:
- フレーム インデックス、宛先サイズ、連続性、および `DecodeOptions` をチェックします。
- 以前は良好だった計画が失敗し始めた場合:
- ピクセルに影響するメタデータの変更後に計画と目的地を再作成します。
- 実行時のデコードが失敗した場合:
- メッセージをログに記録し、単なる形状の問題ではなく、ファイル/コーデックの問題として扱います。
- Python の場合:
- `TypeError`、`ValueError`、`IndexError` は通常、引数または要求したレイアウトが間違っていることを意味します
- `RuntimeError` は通常、基礎となるデコード パス自体が失敗したことを意味します

### スロー/レイズ可能なピクセル デコード API

| APIファミリー | C++ 失敗フォーム | Python はレイズします |典型的な理由 |
| --- | --- | --- | --- |
| `create_decode_plan(...)` | `dicom::diag::DicomException` | `RuntimeError` |ピクセル メタデータが欠落しているか矛盾しているか、明示的なストライドが無効であるか、要求されたデコードされたレイアウトがオーバーフローしています。
| `decode_into(...)` | `dicom::diag::DicomException` | `TypeError`、`ValueError`、`IndexError`、`RuntimeError` |フレーム インデックスが無効です。宛先のサイズまたはレイアウトが間違っています。プランがファイルの状態と一致しなくなっているか、デコーダー/バックエンドが失敗しています。
| `pixel_buffer(...)` | `dicom::diag::DicomException` |直接暴露されない |所有バッファーコンビニエンスパス上の `decode_into(...)` と同じ基本的なデコードエラー |
| `decode_all_frames_into(...)` | `dicom::diag::DicomException` | `decode_into(..., frame=-1)` および `to_array(frame=-1)` によってカバーされています |宛先が小さすぎるか、フレームのメタデータが無効であるか、バッチ デコード/バックエンドの実行が失敗します。
| `to_array(...)` |該当なし | `ValueError`、`IndexError`、`RuntimeError` |無効なフレーム要求、無効なデコード オプション要求、または基礎となるデコード エラー |
| `to_array_view(...)` |該当なし | `ValueError`、`IndexError` |無効なフレーム要求、圧縮されたソース データ、または互換性のある直接生ピクセル ビューがありません。

## ピクセルエンコード

最も安全なエンコード パターンは次のとおりです。

1. 長いループの前にターゲット転送の構文とオプションを検証する
2. 同じ転送構文とオプション セットが繰り返される場合は、`EncoderContext` を優先します。
3. 目的が単に異なるエンコードされた出力ファイルである場合は、`write_with_transfer_syntax(...)` を推奨します。

### エンコードが失敗した場合の対処方法

- `EncoderContext` の構築中に障害が発生した場合:
- 実際のエンコード ループを開始する前に、転送構文またはオプション セットを修正します。
- `set_pixel_data(...)` 中に障害が発生した場合:
- 最初にソース バッファの形状、dtype、連続性、およびピクセル メタデータの仮定を検証します。
- `set_transfer_syntax(...)` 中に障害が発生した場合:
- 現在のオブジェクトの状態でのメモリ内トランスコードの失敗として扱います。
- 目標が出力のみの場合:
- 失敗したトランスコードが通常のメモリ内ワークフローにならないように、`write_with_transfer_syntax(...)` を優先します

### スロー/レイズ可能なピクセル エンコード API

| APIファミリー | C++ 失敗フォーム | Python はレイズします |典型的な理由 |
| --- | --- | --- | --- |
| `create_encoder_context(...)` / `EncoderContext::configure(...)` | `dicom::diag::DicomException` | `TypeError`、`ValueError`、`RuntimeError` |転送構文が無効か、オプションのキー/値が無効か、ランタイム エンコーダの構成が失敗します。
| `set_pixel_data(...)` | `dicom::diag::DicomException` | `TypeError`、`ValueError`、`RuntimeError` |ソース バッファのタイプ/形状/レイアウトが無効です。ソース バイトが宣言されたレイアウトと一致しません。エンコーダの選択に失敗するか、エンコード/バックエンドの更新に失敗します。
| `set_transfer_syntax(...)` | `dicom::diag::DicomException` | `TypeError`、`ValueError`、`RuntimeError` |転送構文の選択が無効です。オプション/コンテキストがリクエストと一致しません。または、トランスコード/バックエンド パスが失敗します。
| `write_with_transfer_syntax(...)` | `dicom::diag::DicomException` | `TypeError`、`ValueError`、`RuntimeError` |無効なパスまたは転送構文テキスト、無効なオプション/コンテキスト、サポートされていないトランスコード パス、バックエンド エンコードの失敗、または出力の書き込み失敗 |

## 文字セットと人名

文字セットの処理では、次の 2 つのスタイルを意図的に混合します。

- 要素レベルの読み取り/書き込みヘルパーは、ほとんどの場合、`None`、空の `optional`、または `false` による通常のエラーを報告します。
- データセット全体の文字セットの変更は検証/トランスコード操作であり、失敗するとスローまたは発生します。

この違いは、失敗が「この 1 つのテキスト割り当てが失敗した」だけなのか、「このデータセット全体のトランスコードを停止する必要がある」のかを判断するときに重要です。

### 文字セットの作業が失敗した場合の対処方法

- `to_utf8_string()` / `to_person_name()` の場合:
- 空の `optional` または `None` を「デコード/解析で使用可能な値が生成されませんでした」として扱います。
- 厳密な失敗ではなくベストエフォート型のテキストが必要な場合は、置換ポリシーを選択します
- `from_utf8_view()` / `from_person_name()` の場合:
- `false` を「現在の文字セット/ポリシーではこの書き込みが成功しませんでした」として扱います。
- 不可逆置換が許容され、それが起こったかどうかを知りたい場合は、Python の `return_replaced=True` または C++ の `bool* out_replaced` を使用します。
- Python 要素レベルのヘルパーの場合:
- 無効な `errors=` テキストでは、通常の戻り値パスに到達する前に `ValueError` が発生することに注意してください。
- `set_specific_charset()`の場合:
- 検証またはフェイルファストクリーンアップには `strict` を使用します
- `(U+XXXX)` マーカーを表示したままトランスコードを終了したい場合は、`replace_unicode_escape` を使用します。
- 現在のデータセットに誤って宣言された raw バイトがすでに含まれている可能性がある場合は、通常のトランスコードを宣言修復として扱う代わりに、トラブルシューティング フローを使用します。

### スロー/レイズ可能な文字セット API

| APIファミリー | C++ 失敗フォーム | Python はレイズします |典型的な理由 |
| --- | --- | --- | --- |
| `set_specific_charset(...)` | `dicom::diag::DicomException` | `TypeError`、`ValueError`、`RuntimeError` |文字セット宣言テキストが無効です。ポリシー テキストが無効です。選択したポリシーでソース テキストをトランスコードできないか、データセット全体の文字セット変更が失敗します。
| `set_declared_specific_charset(...)` | `dicom::diag::DicomException` | `TypeError`、`ValueError`、`RuntimeError` |宣言引数が無効であるか、`(0008,0005)` を一貫して更新できません。主に修理/トラブルシューティングのフローに使用します。

### 通常のコンテンツ障害に対してスローされない Charset API

| APIファミリー | C++ 失敗フォーム | Python の失敗フォーム |代表的な意味 |
| --- | --- | --- | --- |
| `to_utf8_string()` / `to_utf8_strings()` |空の`std::optional` | `None` または `(None, replaced)` |間違った VR、文字セットのデコードに失敗したか、使用可能なデコードされたテキストが生成されませんでした。
| `to_person_name()` / `to_person_names()` |空の`std::optional` | `None` または `(None, replaced)` |間違った VR、文字セットのデコードに失敗、またはデコード後の PN 解析に失敗しました。
| `from_utf8_view()` / `from_utf8_views()` | `false` | `False` または `(False, replaced)` |現在の文字セットとエラー ポリシーでは要素の書き込みが成功しませんでした。
| `from_person_name()` / `from_person_names()` | `false` | `False` または `(False, replaced)` |現在の文字セットとエラー ポリシーでは PN 書き込みが成功しませんでした。

## どの戦略から始めるべきですか?

- 不正な形式のファイルはワークフローを直ちに停止する必要があります
- 厳密な `read_file(...)` / `read_bytes(...)` を使用します
- 不正な形式のファイルからメタデータを復旧したい
- `keep_on_error=True` を使用し、常に `has_error` と `error_message` を検査します
- 呼び出し元管理のデコード バッファーまたは明示的な出力ストライドが必要です
- `create_decode_plan(...)` と `decode_into(...)` を使用します
- 最初に最も単純なデコードパスが必要です
- Python では `to_array()` を使用するか、C++ では `pixel_buffer()` を使用します
- 同じエンコード構成で多くの出力を書き込んでいます
- `EncoderContext` を 1 つ構築する
- 別の出力転送構文が必要なだけです
- `write_with_transfer_syntax(...)` を優先します
- データセット全体でテキスト値を変更またはトランスコードしています
- `set_specific_charset(...)` を使用します
- 1 つのテキスト要素の読み取りまたは書き込みを行っており、通常の「はい/いいえ」の失敗が必要です
- `to_utf8_string()` / `from_utf8_view()` とその PN バリアントを使用します

## 関連ドキュメント

- [ファイルI/O](file_io.md)
- [ピクセルデコード](pixel_decode.md)
- [ピクセルエンコード](pixel_encode.md)
- [文字セットと人物名](charset_and_person_name.md)
- [トラブルシューティング](troubleshooting.md)
- [エラーモデル](../reference/error_model.md)
