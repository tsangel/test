# ファイルI/O

このページでは、ディスク入力、メモリ入力、部分読み込み、そしてファイル・バイト列・ストリームへの主要な出力経路について説明します。

## ファイル I/O の仕組み

- `read_file(...)` および `read_bytes(...)` は `DicomFile` を作成し、`load_until` までの入力をすぐに解析します。
- `write_file(...)` および `write_bytes(...)` は、`DicomFile` オブジェクトをファイルまたはバイトにシリアル化します。
- `write_with_transfer_syntax(...)` は、異なる転送構文でファイルまたはストリームに直接書き込むための、出力指向のトランスコード経路です。これは、ピクセル圧縮を `HTJ2KLossless` などに変更したいときによく使います。元のオブジェクトは先に変更されません。C++ では、同じ API ファミリにストリーム向けのオーバーロードもあります。

## ディスクから読み取る

**C++**

```cpp
#include <dicom.h>

auto file = dicom::read_file("in.dcm");
```

**パイソン**

```python
import dicomsdl as dicom

df = dicom.read_file("in.dcm")
```

注:
- 必要なタグがファイルの先頭近くにあり、未読の末尾をすぐに必要としない場合は、`load_until` を使用します。これにより、事前に完全なデータセットを解析する必要がなくなり、読み取りコストが削減されます。
- 後ろにあるタグへアクセスしても、解析は暗黙には続行されません。C++ と Python のどちらでも、後で必要になった時点で `ensure_loaded(tag)` を呼び出します。C++ では `ensure_loaded(...)` に `"Rows"_tag`、`"(0028,FFFF)"_tag`、`dicom::Tag(0x0028, 0x0010)` などの `Tag` を渡します。Python では `Tag`、パック済みの `int`、または単一タグのキーワード文字列を受け付けます。ドット付きタグパス文字列はサポートされていません。
- 即時例外ではなく、部分的に読み取られたデータを保持したい場合は、`keep_on_error=True` を使用します。次に、`has_error` と `error_message` を検査します。
- Python では、`path` は `str` および `os.PathLike` を受け入れます。 C++ では、`read_file(...)`、`write_file(...)`、`write_with_transfer_syntax(...)` などのディスク パス API は `std::filesystem::path` を受け取ります。

## メモリから読み取る

**C++**

```cpp
#include <dicom.h>
#include <vector>

std::vector<std::uint8_t> payload = /* full DICOM byte stream */;
auto file = dicom::read_bytes("in-memory in.dcm", std::move(payload));
```

**パイソン**

```python
from pathlib import Path
import dicomsdl as dicom

payload = Path("in.dcm").read_bytes()
df = dicom.read_bytes(payload, name="in-memory in.dcm")
```

注:
- `name` は、`path()` / `path` および診断によって報告される識別子になります。
- `load_until` は、メモリ内入力に対しても同じように動作します。データセットの初期部分のみが必要であるが、読み取られていない末尾データは後で暗黙的にロードされない場合に便利です。
- Python では、`read_bytes(..., copy=False)` はコピーの代わりに呼び出し元バッファへの参照を保持します。`DicomFile` がそのバッファを参照している間は、バッファを生かしたままにし、内容を変更しないでください。
- C++ では、`read_bytes(...)` は生のポインターからコピーすることも、移動された `std::vector<std::uint8_t>` の所有権を取得することもできます。

## ステージングされた読み取り

**C++**

```cpp
#include <dicom.h>
using namespace dicom::literals;

dicom::ReadOptions opts;
opts.load_until = "0028,ffff"_tag;

auto file = dicom::read_file("in.dcm", opts);  // initial partial parse

auto& ds = file->dataset();
ds.ensure_loaded("PixelData"_tag);  // later, advance farther
```

**パイソン**

```python
import dicomsdl as dicom

df = dicom.read_file("in.dcm", load_until=dicom.Tag("0028,ffff"))
df.ensure_loaded("PixelData")
```

注:
- 初期解析を早期に停止したい場合は、`options.load_until` を設定します。
- 部分的な読み取りの後、`ensure_loaded(tag)` を使用してさらにデータ要素を解析します。 C++ では、`"Rows"_tag`、`"(0028,FFFF)"_tag`、または `dicom::Tag(...)` などの `Tag` を渡します。
- 部分的にロードされたデータセットでは、まだ解析されていないデータ要素は、後の検索または書き込みのために暗黙的にロードされません。
- Python では、`ensure_loaded(...)` は、`Tag`、パックされた `int`、または単一タグのキーワード文字列を受け入れます。ネストされたドット区切りのタグパス文字列はサポートされていません。
- 同じ段階的読み取りパターンが `read_bytes(...)` でも機能します。ゼロコピーメモリ入力が必要な場合は、`copy=false` を使用します。
- `read_bytes(..., copy=false)` では、呼び出し元が所有するバッファーは `DicomFile` よりも長く存続する必要があります。

## 部分的なロードと許容的な読み取り

- `load_until` は、要求されたタグが読み取られた後、解析を停止します。
- `keep_on_error` は部分的に読み取られたデータを保持し、読み取り失敗を `DicomFile` に記録します。
- ファイルまたはメモリからロードされた部分的にロードされたデータセットでは、ルックアップ API とミューテーション API は、まだ解析されていないデータ要素のロードを暗黙的に継続しません。
- 実際には、これは、後のタグのアクセスが欠落または発生として動作する可能性があり、後のタグの書き込みが未読データをサイレントに変更する代わりに発生する可能性があることを意味します。

**C++**

```cpp
#include <dicom.h>
using namespace dicom::literals;

dicom::ReadOptions opts;
opts.load_until = "0002,ffff"_tag;  // stop after file meta

auto file = dicom::read_file("in.dcm", opts);
auto& ds = file->dataset();

ds.ensure_loaded("0028,0011"_tag);  // advance through Columns

long rows = ds.get_value<long>("0028,0010"_tag, -1L);  // Rows
long cols = ds.get_value<long>("0028,0011"_tag, -1L);  // Columns
long bits = ds.get_value<long>("0028,0100"_tag, -1L);  // BitsAllocated

// 行と列が利用可能になりました
// (0028,0100) がまだ解析されていないため、ビットはまだ -1 です

ds.ensure_loaded("0028,ffff"_tag);
bits = ds.get_value<long>("0028,0100"_tag, -1L);  // now available
```

注:
- これは、必要なタグがデータセットの先頭近くに集中している場合に便利です。
- データセット全体やピクセル ペイロードに触れることなくメタデータ インデックスやデータベースを構築するなど、多くの DICOM ファイルにわたる高速スキャンにも適しています。
- Python は、プレーンなタグとキーワードに対して同じ `ensure_loaded(...)` 継続パターンをサポートします。

## `DicomFile` オブジェクトをファイルまたはバイトにシリアル化します。

**C++**

```cpp
#include <dicom.h>

auto file = dicom::read_file("in.dcm");

dicom::WriteOptions opts;
opts.include_preamble = true;
opts.write_file_meta = true;
opts.keep_existing_meta = false;

file->write_file("out.dcm", opts);
auto payload = file->write_bytes(opts);
```

**パイソン**

```python
import dicomsdl as dicom

df = dicom.read_file("in.dcm")
payload = df.write_bytes(keep_existing_meta=False)
df.write_file("out.dcm", keep_existing_meta=False)
```

注:
- デフォルトのオプションでは、`write_file()` および `write_bytes()` は、プリアンブルとファイル メタ情報を含む通常の Part 10 スタイルの出力を生成します。
- `write_file_meta=False` はファイル メタ グループを省略します。
- `include_preamble=False` は 128 バイトのプリアンブルを省略します。
- `keep_existing_meta=False` は、書き込み前にファイル メタを再構築します。シリアル化の前にそのステップを明示的に実行する場合は、`rebuild_file_meta()` を使用します。
- これらの API は、`DicomFile` オブジェクトをファイルまたはバイトにシリアル化します。個別の出力専用トランスコード パスは提供されません。

## ファイルまたはストリームに直接トランスコードする

**C++**

```cpp
#include <dicom.h>

auto file = dicom::read_file("in.dcm");
file->write_with_transfer_syntax(
    "out_htj2k_lossless.dcm",
    dicom::uid::WellKnown::HTJ2KLossless
);
```

**パイソン**

```python
from pathlib import Path
import dicomsdl as dicom

df = dicom.read_file("in.dcm")
df.write_with_transfer_syntax(Path("out_htj2k_lossless.dcm"), "HTJ2KLossless")
```

注:
- `write_with_transfer_syntax(...)` は、ターゲット転送構文を使用して出力に直接トランスコードします。これは、ソース `DicomFile` を変更せずに、ピクセル圧縮を `HTJ2KLossless` などに変更するためによく使用されます。
- 実際の目標が出力ファイルまたはストリームである場合、特に大きなピクセル ペイロードの場合に推奨します。デコード作業バッファと再エンコードされたターゲット `PixelData` の両方を必要以上に長く存続させるメモリ内トランスコード パスを回避することで、ピーク時のメモリ使用量を削減できます。
- Python では、`write_with_transfer_syntax(...)` はパスベースの出力専用トランスコード API です。 C++ では、同じ API ファミリが直接ストリーム出力もサポートしています。
- シーク可能な出力は、必要に応じて `ExtendedOffsetTable` データをバックパッチできます。シーク不可能な出力は有効な DICOM のままですが、それらのテーブルが省略され、空の基本オフセット テーブルが使用される場合があります。
- 一般的なシーク可能な出力は、ローカル ディスク上の通常のファイルです。一般的なシーク不可能な出力は、パイプ、ソケット、標準出力、HTTP 応答ストリーム、または zip エントリ スタイルのストリームです。

## どの API を使用すればよいですか?

- ローカル ファイル、要求された境界まで直ちに解析します: `read_file(...)`
- すでにメモリ内にあるバイト: `read_bytes(...)`
- Python でのゼロコピー メモリ入力: `read_bytes(..., copy=False)`
- C++ でのファイルバック ステージ読み取り: `read_file(...)` と `load_until`、その後 `ensure_loaded(...)`
- C++ でのメモリからのゼロコピー ステージング読み取り: `read_bytes(...)` と `copy=false` およびオプションの `load_until`、その後 `ensure_loaded(...)`
- `DicomFile` オブジェクトをファイルまたはバイトにシリアル化します: `write_file(...)` または `write_bytes(...)`
- 新しい転送構文をパスに直接書き込みます: `write_with_transfer_syntax(...)`
- C++ では、新しい転送構文を出力ストリームに直接書き込みます: `write_with_transfer_syntax(...)`

## 関連ドキュメント

- [コアオブジェクト](core_objects.md)
- [C++ データセット ガイド](cpp_dataset_guide.md)
- [Python データセット ガイド](python_dataset_guide.md)
- [ピクセルデコード](pixel_decode.md)
- [ピクセルエンコード](pixel_encode.md)
- [C++ API の概要](../reference/cpp_api.md)
- [データセットリファレンス](../reference/dataset_reference.md)
- [Dicomファイルリファレンス](../reference/dicomfile_reference.md)
- [エラー処理](error_handling.md)
