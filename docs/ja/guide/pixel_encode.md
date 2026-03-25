# ピクセルエンコード

エンコード対象のネイティブ ピクセル データがすでにある場合は `set_pixel_data()` を使います。現在の `DicomFile` にピクセル データがすでにあり、それをメモリ内でトランスコードしたい場合は `set_transfer_syntax()` を使います。元のオブジェクトを先に変更せず、別の転送構文でそのまま出力したい場合は `write_with_transfer_syntax()` を使います。同じ転送構文とオプション セットを複数回使い回す場合や、長めのエンコード ループに入る前に設定を検証したい場合は `EncoderContext` を作成します。

## 主要なエンコード API

**C++**

- `set_pixel_data(...)`
- `pixel::ConstPixelSpan` でレイアウトを明示したネイティブ ソース バッファから Pixel Data を置き換えます。
- `create_encoder_context(...)` + `set_pixel_data(...)` / `set_transfer_syntax(...)`
- 繰り返し行うエンコードやトランスコードのループの外で、設定済みの転送構文とオプション セットを保持して再利用します。
- `write_with_transfer_syntax(...)`
- メモリ内の `DicomFile` を変更せずに、別の転送構文をファイルまたはストリームに直接書き込みます。

**パイソン**

- `set_pixel_data(...)`
- C 連続 NumPy 配列または他の連続数値バッファーからのピクセル データを置き換えます。
- `create_encoder_context(...)` + `set_pixel_data(...)` / `set_transfer_syntax(...)`
- 1 つの Python `options` オブジェクトを先に解析して検証し、生成されたコンテキストを繰り返しの呼び出しで再利用します。
- `write_with_transfer_syntax(...)`
- 元のオブジェクトを先に変更せず、別の転送構文でファイルに直接書き込みます。
- `set_transfer_syntax(...)`
- 後で同じオブジェクトを読み書きするときに新しい転送構文を使いたい場合は、メモリ内の現在の `DicomFile` をトランスコードします。

## 関連する DICOM 標準セクション

- エンコードされたデータとの一貫性を保つ必要があるピクセル メタデータは、[DICOM PS3.3 セクション C.7.6.3、画像ピクセル モジュール](https://dicom.nema.org/medical/dicom/current/output/chtml/part03/sect_C.7.6.3.html) で定義されています。
- ピクセル データのネイティブ エンコーディングとカプセル化されたピクセル データのエンコーディングおよびコーデック固有の 8.2.x ルールは、[DICOM PS3.5 第 8 章、ピクセル、オーバーレイ、および波形データのエンコーディング](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/chapter_8.html) および [セクション 8.2、ネイティブ フォーマットまたはカプセル化フォーマット] で定義されています。エンコーディング](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/sect_8.2.html)。
- カプセル化された転送構文とフラグメント ルールは、[DICOM PS3.5 セクション A.4、エンコードされたピクセル データのカプセル化のための転送構文](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/sect_A.4.html) で定義されています。
- ファイルベースのエンコードおよびトランスコードのワークフローでは、結果の転送構文 UID は、[DICOM PS3.10 第 7 章、DICOM ファイル形式](https://dicom.nema.org/medical/dicom/current/output/chtml/part10/chapter_7.html) で定義されたファイルのメタ情報に含まれます。

## C++

### `set_pixel_data()` を呼び出す前にソース ピクセルを明示的に記述してください

```cpp
#include <cstdint>
#include <dicom.h>
#include <random>
#include <span>
#include <vector>

using namespace dicom::literals;

auto file = dicom::read_file("sample.dcm");

const std::uint32_t rows = 256;
const std::uint32_t cols = 256;
const std::uint32_t frames = 1;

std::vector<std::uint16_t> pixels(rows * cols * frames);
std::mt19937 rng(0);
std::uniform_int_distribution<int> dist(0, 4095);
for (auto& px : pixels) {
    px = static_cast<std::uint16_t>(dist(rng));
}

const dicom::pixel::ConstPixelSpan source{
    .layout = dicom::pixel::PixelLayout{
        .data_type = dicom::pixel::DataType::u16,
        .photometric = dicom::pixel::Photometric::monochrome2,
        .planar = dicom::pixel::Planar::interleaved,
        .reserved = 0,
        .rows = rows,
        .cols = cols,
        .frames = frames,
        .samples_per_pixel = 1,
        .bits_stored = 12,
        .row_stride = cols * sizeof(std::uint16_t),
        .frame_stride = rows * cols * sizeof(std::uint16_t),
    },
    .bytes = std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(pixels.data()),
        pixels.size() * sizeof(std::uint16_t)),
};

// set_pixel_data() は上記のレイアウトを使用してネイティブ ソース バッファーを読み取り、
// DicomFile 上の一致する画像ピクセルのメタデータを書き換えます。
file->set_pixel_data("RLELossless"_uid, source);
```

### 事前構成された 1 つのコンテキストを繰り返し書き込みループの外側に保持します。

```cpp
#include <array>
#include <dicom.h>
#include <diagnostics.h>
#include <iostream>
#include <span>

using namespace dicom::literals;

auto file = dicom::read_file("sample.dcm");

const std::array<dicom::pixel::CodecOptionTextKv, 3> j2k_options{{
    {"target_psnr", "45"},
    {"threads", "4"},
    {"color_transform", "true"},
}};

// 繰り返される書き込みループの外側に、再利用可能な JPEG 2000 コンテキストを 1 つ構築します。
// これにより、転送構文とオプション セットが 1 か所に保持されます。
// 各呼び出しサイトで同じオプション リストを再構築します。
auto j2k_ctx = dicom::pixel::create_encoder_context(
    "JPEG2000"_uid,
    std::span<const dicom::pixel::CodecOptionTextKv>(j2k_options));

try {
    for (const char* path : {"out_j2k_1.dcm", "out_j2k_2.dcm"}) {
        file->write_with_transfer_syntax(path, "JPEG2000"_uid, j2k_ctx);
    }
} catch (const dicom::diag::DicomException& ex) {
    // エンコードまたは configure が失敗すると、例外メッセージには
    // 失敗した呼び出しの段階と理由が含まれます。通常は ex.what() を
    // ログに出すだけでも最初のデバッグには十分です。
    std::cerr << ex.what() << '\n';
}
```

### 別の転送構文を出力に直接記述します。

```cpp
#include <dicom.h>

using namespace dicom::literals;

auto file = dicom::read_file("sample.dcm");

// write_with_transfer_syntax() は、次の場合の出力指向のトランスコード パスです。
// ターゲット構文はシリアル化された結果に対してのみ重要です。
file->write_with_transfer_syntax("out_rle.dcm", "RLELossless"_uid);

// 同じ API ファミリには、C++ の std::ostream オーバーロードもあります。
```

### 明示的なコーデック オプションを渡す

```cpp
#include <array>
#include <dicom.h>

using namespace dicom::literals;

auto file = dicom::read_file("sample.dcm");

const std::array<dicom::pixel::CodecOptionTextKv, 1> lossy_options{{
    {"target_psnr", "45"},
}};

// 非可逆ターゲットの場合は、代わりに必要なコーデック オプションを明示的に渡します。
// デフォルトに依存すると、意図した出力と一致しない可能性があります。この直接的な
// style は 1 回限りの書き込みには適しています。同じ場合は、代わりに EncoderContext を使用してください
// オプション セットは多くの呼び出しで再利用されます。
file->write_with_transfer_syntax(
    "out_j2k_lossy.dcm", "JPEG2000"_uid,
    std::span<const dicom::pixel::CodecOptionTextKv>(lossy_options));
```

## パイソン

### NumPy 配列からピクセル データを置換する

```python
import dicomsdl as dicom
import numpy as np

df = dicom.read_file("sample.dcm")

# set_pixel_data() は C 連続数値配列を想定しています。
# 配列の形状と dtype により、エンコードされた Rows、Columns、
# SamplesPerPixel、NumberOfFrames、およびビット深度メタデータ。
rng = np.random.default_rng(0)
arr = rng.integers(0, 4096, size=(256, 256), dtype=np.uint16)
df.set_pixel_data("ExplicitVRLittleEndian", arr)

df.write_file("native_replaced.dcm")
```

### 明示的なコーデック オプションを `set_pixel_data()` に渡します

```python
import dicomsdl as dicom
import numpy as np

df = dicom.read_file("sample.dcm")
rng = np.random.default_rng(0)
arr = rng.integers(0, 4096, size=(256, 256), dtype=np.uint16)

# 非可逆ターゲットでは、コーデックオプションを明示的に渡して、
# エンコード設定が呼び出し箇所から分かるようにします。
df.set_pixel_data(
    "JPEG2000",
    arr,
    options={"type": "j2k", "target_psnr": 45.0},
)
```

### Python オプション辞書を一度解析して検証し、コンテキストを再利用します。

```python
import dicomsdl as dicom

df = dicom.read_file("sample.dcm")

# create_encoder_context() は、ここで Python オプション オブジェクトを解析して検証します。
# 繰り返し書き込みループが開始される前に 1 回。
j2k_ctx = dicom.create_encoder_context(
    "JPEG2000",
    options={
        "type": "j2k",
        "target_psnr": 45.0,
        "threads": 4,
        "color_transform": True,
    },
)

# 繰り返しの出力には、同じ検証済みの転送構文とオプション セットを再利用します。
for path in ("out_j2k_1.dcm", "out_j2k_2.dcm"):
    df.write_with_transfer_syntax(path, "JPEG2000", encoder_context=j2k_ctx)
```

### エンコード ループが開始する前に構成エラーを検査する

```python
import dicomsdl as dicom

try:
    dicom.create_encoder_context(
        "JPEG2000",
        options={
            "type": "j2k",
            "target_psnr": -1.0,
        },
    )
except ValueError as exc:
    # 無効なオプションは、長時間実行されるエンコード ループが開始される前に、ここで失敗します。
    print(exc)
```

### ソースオブジェクトを変更せずに別の転送構文を作成します。

```python
import dicomsdl as dicom

df = dicom.read_file("sample.dcm")

# write_with_transfer_syntax() は、シリアル化された出力パスのみを変更します。
# インメモリ DicomFile は、現在の転送構文とピクセル状態を保持します。
df.write_with_transfer_syntax("out_rle.dcm", "RLELossless", options="rle")
```

## 例外

**C++**

| API |スロー |典型的な理由 |
| --- | --- | --- |
| `create_encoder_context(...)` / `EncoderContext::configure(...)` | `dicom::diag::DicomException` |転送構文が無効であるか、エンコードに対してサポートされていません。 C++ では、ほとんどのコーデック オプション セマンティクスは、後でエンコードまたはトランスコード呼び出しがランタイム エンコーダーを構成するときに検証されます。 |
| `set_pixel_data(...)` | `dicom::diag::DicomException` |ソース レイアウトとソース バイトが一致しない、エンコーダー コンテキストが欠落または不一致である、エンコーダー バインディングが利用できない、バックエンドが現在のコーデック オプションまたはピクセル レイアウトを拒否する、またはエンコード後に転送構文メタデータの更新が失敗する。 |
| `set_transfer_syntax(...)` | `dicom::diag::DicomException` |転送構文の選択が無効であるか、エンコーダ コンテキストが要求された構文と一致しないか、トランスコード パスがサポートされていないか、バックエンド エンコードが失敗しています。 |
| `write_with_transfer_syntax(...)` | `dicom::diag::DicomException` |転送構文の選択が無効です。エンコーダ コンテキストが要求された構文と一致しません。トランスコード パスがサポートされていません。バックエンド エンコードが失敗するか、ファイル/ストリーム出力が失敗します。 |

C++ エンコード メッセージには通常、`status=...`、`stage=...`、および `reason=...` が含まれ、ステータスは `invalid_argument`、`unsupported`、`backend_error`、`internal_error` などになります。

**パイソン**

| API |レイズ |典型的な理由 |
| --- | --- | --- |
| `create_encoder_context(...)` | `TypeError`、`ValueError`、`RuntimeError` | `options` のコンテナーまたは値のタイプが間違っているか、オプションのキーまたは値が無効であるか、転送構文テキストが不明であるか、基になる C++ 構成ステップが依然として失敗します。 |
| `set_pixel_data(...)` | `TypeError`、`ValueError`、`RuntimeError` | `source` はサポートされているバッファ オブジェクトではないか、C 連続ではありません。推論されたソース形状または dtype が無効です。エンコード オプションが無効です。または、ランタイム エンコーダ/データセットの更新が失敗します。 |
| `set_transfer_syntax(...)` | `TypeError`、`ValueError`、`RuntimeError` |転送構文テキストが無効、`options` オブジェクト タイプが間違っている、オプション値が無効、エンコーダー コンテキストが要求された構文と一致しない、またはトランスコード パス/バックエンドが失敗します。 |
| `write_with_transfer_syntax(...)` | `TypeError`、`ValueError`、`RuntimeError` |パスまたは `options` タイプが無効、転送構文テキストまたはオプション値が無効、エンコーダー コンテキストが要求された構文と一致しない、または書き込み/トランスコードが失敗します。 |

## 注意事項

- C++ では、`set_pixel_data()` は、指定した `pixel::ConstPixelSpan` レイアウトからネイティブ ピクセルを読み取ります。ソースバイトに行間隔またはフレーム間隔がある場合、レイアウトはその間隔を正確に記述する必要があります。
- Python では、`set_pixel_data()` は C 連続数値バッファーを期待します。配列が現在ストライドされているか非連続である場合は、最初に `np.ascontiguousarray(...)` を使用します。
- `set_pixel_data()` は、`Rows`、`Columns`、`SamplesPerPixel`、`BitsAllocated`、`BitsStored`、`PhotometricInterpretation`、`NumberOfFrames` などの関連する画像ピクセル メタデータと転送構文状態を書き換えます。
- `set_transfer_syntax()` は、メモリ内の `DicomFile` を変更します。目的が単に異なるエンコードされた出力ファイルまたはストリームである場合は、`write_with_transfer_syntax()` の方が良いパスです。
- 同じ転送構文とコーデック オプションが繰り返し適用される場合は、`EncoderContext` を再利用します。 Python では、`create_encoder_context(..., options=...)` も `options` オブジェクトを前もって解析して検証します。 C++ では、`EncoderContext` は 1 つの転送構文とオプション セットをまとめて保持しますが、詳細な障害は依然として `dicom::diag::DicomException` として表面化します。
- 正確なコーデック ルール、オプション名、および転送構文ごとの制約については、短い例から推測するのではなく、リファレンス ページを使用してください。

## 関連ドキュメント

- [ピクセルデコード](pixel_decode.md)
- [ファイルI/O](file_io.md)
- [ピクセル エンコード制約](../reference/pixel_encode_constraints.md)
