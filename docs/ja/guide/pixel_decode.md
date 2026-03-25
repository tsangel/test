# ピクセルデコード

デコード前に検証済みの出力レイアウトが必要な場合、自分で出力バッファを確保または再利用したい場合、デコード後の行またはフレーム stride を明示したい場合、あるいは単一フレーム入力とマルチフレーム入力で同じコード パスを使いたい場合は、`create_decode_plan()` と `decode_into()` を組み合わせて使います。新しいデコード結果を最も簡単に得たい場合は、C++ では `pixel_buffer()`、Python では `to_array()` を使います。

## 主要なデコード API

**C++**

- `create_decode_plan(...)` + `decode_into(...)`
  - 呼び出し元が用意した出力バッファと組み合わせて、検証済みで再利用可能なデコードレイアウトが必要なときに、この 2 つを一緒に使います。単一フレーム入力でバッファを先に確保・再利用したい場合や、`DecodeOptions` で明示的な出力 stride を指定したい場合もここに含まれます。
- `pixel_buffer(...)`
- 新しいピクセル バッファをデコードして返します。

**パイソン**

- `create_decode_plan(...)` + `decode_into(...)`
  - 呼び出し元が用意した書き込み可能な配列またはバッファと組み合わせて、検証済みで再利用可能なデコードレイアウトが必要なときに、この 2 つを一緒に使います。単一フレーム入力で出力先を先に準備したい場合や、`DecodeOptions` で明示的な出力 stride を指定したい場合もここに含まれます。
- `to_array(...)`
  - デコードして新しい NumPy 配列を返します。最も手早く結果を得られる基本ルートです。
- `to_array_view(...)`
- ソースピクセルデータが非圧縮転送構文を使用している場合、ゼロコピーの NumPy ビューを返します。

## 関連する DICOM 標準セクション

- 行、列、ピクセルごとのサンプル数、測光解釈、Pixel Data を規定するピクセル属性は、[DICOM PS3.3 セクション C.7.6.3、画像ピクセル モジュール](https://dicom.nema.org/medical/dicom/current/output/chtml/part03/sect_C.7.6.3.html) に定義されています。
- ピクセル データのネイティブ エンコーディングとカプセル化されたピクセル データのエンコーディングは、[DICOM PS3.5 第 8 章、ピクセル、オーバーレイ、および波形データのエンコーディング](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/chapter_8.html) および [セクション 8.2、ネイティブ形式またはカプセル化形式] で定義されています。エンコーディング](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/sect_8.2.html)。
- カプセル化されたフラグメント/アイテムのレイアウトと転送構文の要件は、[DICOM PS3.5 セクション A.4、エンコードされたピクセル データのカプセル化のための転送構文](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/sect_A.4.html) で定義されています。
- ファイルベースのワークフローでは、転送構文 UID は、[DICOM PS3.10 第 7 章、DICOM ファイル形式](https://dicom.nema.org/medical/dicom/current/output/chtml/part10/chapter_7.html) で説明されているファイルのメタ情報から取得されます。

## C++

### C++: 1 フレームをデコードする前に出力レイアウトを検査する

```cpp
#include <cstdint>
#include <dicom.h>
#include <span>
#include <vector>

auto file = dicom::read_file("single_frame.dcm");

// プランはデコードされたピクセルを保持しません。
// 代わりに、現在のファイルのメタデータを検証し、その内容を教えてくれます。
// デコードされた出力は、宛先メモリを割り当てる前のように見える必要があります。
const auto plan = file->create_decode_plan();

// 単一フレームのデコードの場合、frame_stride は、
// decode_into() は、このプランで 1 つのデコードされたフレームを想定しています。
std::vector<std::uint8_t> out(plan.output_layout.frame_stride);

// ここではフレーム 0 が唯一のフレームですが、この呼び出し形状は次のフレームでも機能します。
// マルチフレーム入力。これにより、呼び出し元が所有する 1 つのバッファー パスを維持することが容易になります。
// シングルフレームコードとマルチフレームコードの両方に対応します。
file->decode_into(0, std::span<std::uint8_t>(out), plan);

// `out` には、計画に従って正確にレイアウトされた 1 つのデコードされたフレームが含まれています。
```

### 多くのフレームにわたって 1 つのプランと 1 つの宛先バッファーを再利用する

```cpp
#include <cstdint>
#include <dicom.h>
#include <span>
#include <vector>

auto file = dicom::read_file("multiframe.dcm");
const auto plan = file->create_decode_plan();

// 1 つの DecodePlan は 1 つのデコードされたフレーム レイアウトを意味するため、単一のフレーム レイアウトを割り当てることができます。
// 再利用可能なフレーム バッファーをフレームごとに補充します。
std::vector<std::uint8_t> frame_bytes(plan.output_layout.frame_stride);

for (std::size_t frame = 0; frame < plan.output_layout.frames; ++frame) {
	// 再計算する代わりに、すべてのフレームで同じ検証済みのレイアウトを再利用します。
	// メタデータを取得したり、毎回新しいバッファを割り当てたりすることができます。
	file->decode_into(frame, std::span<std::uint8_t>(frame_bytes), plan);

	// 次の反復の前に、ここで `frame_bytes` を処理、コピー、または転送します。
	// 次のデコードされたフレームで上書きします。
}
```

### C++: DecodeOptions から計画を構築する

```cpp
#include <cstdint>
#include <dicom.h>
#include <span>
#include <vector>

auto file = dicom::read_file("multiframe_j2k.dcm");

dicom::pixel::DecodeOptions options{};
options.alignment = 32;
// デコードされたイメージにピクセルごとに複数のサンプルがある場合は、平面出力を要求します。
options.planar_out = dicom::pixel::Planar::planar;
// バックエンドの場合、コードストリーム レベルの逆 MCT/カラー変換を適用します。
// それをサポートします。これがデフォルトであり、通常の開始点です。
options.decode_mct = true;
// 外部ワーカーのスケジューリングは、主にバッチまたは複数の作業項目のデコードに重要です。
options.worker_threads = 4;
// サポートされている場合は、コーデック バックエンドに最大 2 つの内部スレッドを使用するように依頼します。
options.codec_threads = 2;

// 計画では、これらのオプションと、それらが意味する正確な出力レイアウトをキャプチャします。
const auto plan = file->create_decode_plan(options);

// フルボリュームのデコードの場合は、デコードされたフレームごとに十分なストレージを割り当てます。
std::vector<std::uint8_t> volume(
    plan.output_layout.frames * plan.output_layout.frame_stride);

// decode_all_frames_into() は同じ検証済みプランを使用しますが、全体を満たします。
// 一度に 1 フレームずつではなく、出力ボリュームを調整します。
file->decode_all_frames_into(std::span<std::uint8_t>(volume), plan);
```

### C++: 明示的な出力ストライドをリクエストする

```cpp
#include <cstdint>
#include <dicom.h>
#include <span>
#include <vector>

auto file = dicom::read_file("multiframe.dcm");

const auto rows = static_cast<std::size_t>(file["Rows"_tag].to_long().value_or(0));
const auto cols = static_cast<std::size_t>(file["Columns"_tag].to_long().value_or(0));
const auto samples_per_pixel =
    static_cast<std::size_t>(file["SamplesPerPixel"_tag].to_long().value_or(1));
const auto frame_count =
    static_cast<std::size_t>(file["NumberOfFrames"_tag].to_long().value_or(1));
const auto bits_allocated =
    static_cast<std::size_t>(file["BitsAllocated"_tag].to_long().value_or(0));
const auto bytes_per_sample = (bits_allocated + 7) / 8;
const auto packed_row_bytes = cols * samples_per_pixel * bytes_per_sample;
const auto row_stride = ((packed_row_bytes + 32 + 31) / 32) * 32;
const auto frame_stride = row_stride * rows;

// まず、メタデータから派生したレイアウトから宛先バッファを割り当てます。
std::vector<std::uint8_t> frame_bytes(frame_stride);
std::vector<std::uint8_t> volume_bytes(frame_count * frame_stride);

dicom::pixel::DecodeOptions options{};
// インターリーブ出力がデフォルトですが、ストライドが異なるため、ここで詳しく説明します。
// 以下の計算では、各行内のサンプルがインターリーブされていることを前提としています。
options.planar_out = dicom::pixel::Planar::interleaved;
// パックされた行ペイロードを超えて少なくとも 32 バイトを追加し、次のように切り上げます。
// 次の 32 バイト境界。
options.row_stride = row_stride;
options.frame_stride = frame_stride;

const auto plan = file->create_decode_plan(options);

// このプランは、上で選択した明示的な行/フレーム ストライドを検証します。
file->decode_into(0, std::span<std::uint8_t>(frame_bytes), plan);
file->decode_all_frames_into(std::span<std::uint8_t>(volume_bytes), plan);
```

## パイソン

### Python: 1 フレームをデコードする前に出力レイアウトを検査する

```python
import dicomsdl as dicom
import numpy as np

df = dicom.read_file("single_frame.dcm")

# この plan からは、実際にデコードする前に、デコード結果の dtype と配列 shape が分かります。
# 呼び出し側が先に出力配列を確保したい場合に便利です。
plan = df.create_decode_plan()

# まず plan から、frame 0 の正確な NumPy 配列 shape を確認します。
# こうしておくと、後続の decode_into() 呼び出しと同じレイアウト契約を保てます。
out = np.empty(plan.shape(frame=0), dtype=plan.dtype)

# ここでレイアウト メタデータを再計算する代わりに、検証済みのプランを再利用します。
df.decode_into(out, frame=0, plan=plan)

# `out` には、plan が指定したレイアウトどおりのデコード済み frame が 1 つ入ります。
```

### 多くのフレームにわたって 1 つのプランと 1 つの宛先配列を再利用する

```python
import dicomsdl as dicom
import numpy as np

df = dicom.read_file("multiframe.dcm")
plan = df.create_decode_plan()

# 各フレームは 1 つのプランに対して同じデコードされた形状を持つため、1 つの再利用可能な配列は次のようになります。
# フレームごとの処理ループには十分です。
frame_out = np.empty(plan.shape(frame=0), dtype=plan.dtype)

for frame in range(plan.frames):
    # decode_into() は、同じ検証済みプランを再利用しながら
    # 毎回同じ出力先を上書きします。
    df.decode_into(frame_out, frame=frame, plan=plan)

    # 次の反復の前に、ここで `frame_out` を処理、コピー、または転送します。
    # 次のデコードされたフレームにそれを再利用します。
```

### Python: DecodeOptions から計画を構築する

```python
import dicomsdl as dicom
import numpy as np

df = dicom.read_file("multiframe_j2k.dcm")

options = dicom.DecodeOptions(
    alignment=32,
    planar_out=dicom.Planar.planar,
    # バックエンドの場合、コードストリーム レベルの逆 MCT/カラー変換を適用します。
    # それをサポートします。これがデフォルトであり、通常の開始点です。
    decode_mct=True,
    # 外部ワーカーのスケジューリングは、主にバッチまたはマルチフレームのデコードに重要です。
    worker_threads=4,
    # サポートされている場合は、コーデック バックエンドに最大 2 つの内部スレッドを使用するように依頼します。
    codec_threads=2,
)

# プランは要求されたデコード動作をキャプチャするため、後でデコード呼び出しを行うことができます。
# オプションを繰り返さずに、`plan` を再利用するだけです。
plan = df.create_decode_plan(options)

# Frame=-1 は「すべてのフレーム」を意味します。計画では、正確な全容積の形状を知ることができます。
# 宛先配列を割り当てる前に。
volume = np.empty(plan.shape(frame=-1), dtype=plan.dtype)

# plan=... が指定されている場合、プランのキャプチャされたオプションによってデコードが実行されます。
df.decode_into(volume, frame=-1, plan=plan)
```

### Python: 明示的な出力ストライドをリクエストする

```python
import dicomsdl as dicom
import numpy as np

df = dicom.read_file("test_le.dcm")

options = dicom.DecodeOptions(
    # インターリーブ出力がデフォルトですが、これをここで詳しく説明します。
    # 例は、インターリーブ レイアウトでの行ストライドを記述しています。
    planar_out=dicom.Planar.interleaved,
    # この小さなサンプル ファイルでは、より大きな行ストライドを使用してカスタム
    # レイアウトが一目瞭然。独自のファイルの場合は、パックされたファイルよりも大きい値を選択してください
    # デコードされた行サイズ。
    row_stride=1024,
)
plan = df.create_decode_plan(options)

# to_array(plan=...) は、NumPy ストライドがプランと一致する配列を返します。
# つまり、計画で使用する場合、結果が意図的に不連続になる可能性があります。
# 明示的な行またはフレームのストライド。
arr = df.to_array(frame=0, plan=plan)

# `arr.strides` は要求された出力ストライドを反映するようになりました。
# デコードされたピクセル値が正しい場合でも、意図的に不連続である可能性があります。
```

### カスタムストライド NumPy ビューの生ストレージにデコードします

```python
import dicomsdl as dicom
import numpy as np

df = dicom.read_file("test_le.dcm")
plan = df.create_decode_plan(
    # この小さなサンプルでは、​​意図的に大きめの行ストライドを使用しているため、カスタム
    # NumPyビューが見やすいです。独自のファイルの場合は、大きな値を選択してください
    # デコードされた 1 行には十分です。
    dicom.DecodeOptions(row_stride=1024)
)

# decode_into() には、引き続き書き込み可能な C 連続出力バッファ オブジェクトが必要です。
# カスタムストライドレイアウトの場合は、正確な番号を含む生の 1 次元バッファを割り当てます。
# プランに必要なデコードされたバイト数。
raw = np.empty(
    plan.required_bytes(frame=0) // plan.bytes_per_sample,
    dtype=plan.dtype,
)
df.decode_into(raw, frame=0, plan=plan)

# 明示的なストライドがプランと一致する NumPy ビューで RAW ストレージをラップします。
# この単一フレームのモノクロの例は、カスタム ストライドの 2 次元配列ビューになります。
# 余分なピクセルのコピー。
arr = np.ndarray(
    shape=plan.shape(frame=0),
    dtype=plan.dtype,
    buffer=raw,
    strides=(plan.row_stride, plan.bytes_per_sample),
)
```

### まず NumPy ストレージを準備してから、そこにマルチフレーム出力をデコードします

```python
import dicomsdl as dicom
import numpy as np

df = dicom.read_file("multiframe.dcm")

# この例では、モノクロの uint16 マルチフレーム出力レイアウトを想定しています。
# デコードされた dtype またはサンプル レイアウトが異なる場合は、最初にこれらの値を調整します。
dtype = np.uint16
itemsize = np.dtype(dtype).itemsize
rows = int(df.Rows)
cols = int(df.Columns)
frame_count = int(df.NumberOfFrames)
packed_row_bytes = cols * itemsize
# パックされた行ペイロードを超えて少なくとも 32 バイトを追加し、次のように切り上げます。
# 次の 32 バイト境界。
row_stride = ((packed_row_bytes + 32 + 31) // 32) * 32
frame_stride = row_stride * rows

# まず、通常の 1 次元 C 連続 NumPy 配列としてバッキング ストレージを準備します。
# これは、decode_into() が書き込むオブジェクトです。
backing = np.empty((frame_stride * frame_count) // itemsize, dtype=dtype)

# デコードする前に、同じストレージ上にアプリケーション側の配列ビューを構築します。
# この例では、フレーム メジャーのモノクロ レイアウトを使用します。
#   (フレーム、行、列) とストライド (frame_stride、row_stride、itemsize)。
frames = np.ndarray(
    shape=(frame_count, rows, cols),
    dtype=dtype,
    buffer=backing,
    strides=(frame_stride, row_stride, itemsize),
)

# 収納レイアウトが決まったら、それに合わせたプランを立てます。
plan = df.create_decode_plan(
    dicom.DecodeOptions(
        # インターリーブ出力がデフォルトですが、ここでは詳しく説明します。
        # 上記のストレージ レイアウトは、インターリーブされたサンプル用に準備されました。
        planar_out=dicom.Planar.interleaved,
        row_stride=row_stride,
        frame_stride=frame_stride,
    )
)

# 計画が手動で準備した NumPy レイアウトと一致していることを確認します。
assert plan.dtype == np.dtype(dtype)
assert plan.bytes_per_sample == itemsize
assert plan.shape(frame=-1) == frames.shape
assert plan.row_stride == row_stride
assert plan.frame_stride == frame_stride
assert plan.required_bytes(frame=-1) == backing.nbytes

# decode_into() では、宛先オブジェクト自体が書き込み可能である必要があり、
# C 連続。そのため、ここでは `backing` を渡します。
df.decode_into(backing, frame=-1, plan=plan)

# `frames` は、準備した NumPy レイアウトを通じてデコードされたピクセルを公開します。
# `backing` は引き続き基礎となるストレージを所有します。
```

### C++ デコード失敗を明示的に処理する

```cpp
#include <dicom.h>
#include <diagnostics.h>
#include <iostream>
#include <span>
#include <vector>

try {
    auto file = dicom::read_file("single_frame.dcm");
    const auto plan = file->create_decode_plan();

    std::vector<std::uint8_t> out(plan.output_layout.frame_stride);
    file->decode_into(0, std::span<std::uint8_t>(out), plan);
} catch (const dicom::diag::DicomException& ex) {
    // 通常、メッセージには status=...、stage=...、reason=... が含まれます。
    // そのため、多くの場合、エラーの原因が 1 行であるかどうかを確認するのに十分です。
    // メタデータの検証、宛先の検証、デコーダの選択、または
    // バックエンドのデコード ステップ自体。
    std::cerr << ex.what() << '\n';
}
```

### Python デコード失敗を明示的に処理する

```python
import dicomsdl as dicom
import numpy as np

df = dicom.read_file("single_frame.dcm")

try:
    plan = df.create_decode_plan()
    out = np.empty(plan.shape(frame=0), dtype=plan.dtype)
    df.decode_into(out, frame=0, plan=plan)
except (TypeError, ValueError, IndexError) as exc:
    # バインディング側の検証の失敗はここに発生します。
    # 間違ったバッファタイプ、間違った出力サイズ、無効なフレームインデックスなどです。
    print(exc)
except RuntimeError as exc:
    # RuntimeError は通常、基になる C++ デコード パスが後で失敗したことを意味します。
    # Python 引数が受け入れられました。
    print(exc)
```

## 例外

**C++**

| API |スロー |典型的な理由 |
| --- | --- | --- |
| `create_decode_plan(...)` | `dicom::diag::DicomException` |ピクセル メタデータが欠落しているか一貫性がありません。`alignment` が無効です。明示的な `row_stride` / `frame_stride` がデコードされたペイロードより小さいか、出力レイアウトがオーバーフローします。 |
| `decode_into(...)` | `dicom::diag::DicomException` |プランが現在のファイル状態と一致しなくなったか、フレーム インデックスが範囲外であるか、宛先バッファが小さすぎるか、デコーダ バインディングが利用できないか、バックエンド デコードが失敗します。 |
| `pixel_buffer(...)` | `dicom::diag::DicomException` | `decode_into(...)` と同じ障害モードですが、所有バッファーの利便性パス上にあります。 |
| `decode_all_frames_into(...)` | `dicom::diag::DicomException` |フルボリュームの宛先が小さすぎる、フレームのメタデータが無効、デコーダのバインディングが利用できない、バックエンドのデコードが失敗する、または `ExecutionObserver` がバッチをキャンセルします。 |

C++ デコード メッセージには通常、`status=...`、`stage=...`、`reason=...` と、`invalid_argument`、`unsupported`、`backend_error`、`cancelled`、`internal_error` などのステータスが含まれます。

**パイソン**

| API |レイズ |典型的な理由 |
| --- | --- | --- |
| `create_decode_plan(...)` | `RuntimeError` |ピクセル メタデータが欠落しているか、要求された出力レイアウトが無効であるか、デコードされたレイアウトがオーバーフローしているため、基礎となる C++ プランの作成は失敗します。 |
| `to_array(...)` | `ValueError`、`IndexError`、`RuntimeError` | `frame < -1`、無効なスレッド数、範囲外のフレーム インデックス、または引数の検証が成功した後の基礎的なデコード エラー。 |
| `decode_into(...)` | `TypeError`、`ValueError`、`IndexError`、`RuntimeError` |宛先が書き込み可能な C 連続バッファではない、項目サイズまたは合計バイト サイズがデコードされたレイアウトと一致しない、フレーム インデックスが範囲外である、または基になるデコード パスが失敗します。 |
| `to_array_view(...)` | `ValueError`、`IndexError` |ソース転送構文が圧縮されているか、マルチサンプル ネイティブ データがインターリーブされていないか、直接の生ピクセル ビューが利用できないか、またはフレーム インデックスが範囲外です。 |

## 注意事項

- 単一フレーム入力の場合でも、`DecodePlan` は、デコード前に出力レイアウトを検査したり、呼び出し間で宛先バッファを再利用したりする場合に便利です。
- `DecodePlan` を、デコードされたピクセルのキャッシュとしてではなく、検証された出力コントラクトとして扱います。
- `DecodeOptions.row_stride` および `DecodeOptions.frame_stride` を使用すると、デコードされた出力の明示的な行ストライドとフレーム ストライドをリクエストできます。どちらかがゼロ以外の場合、`alignment` は無視されます。
- 明示的にデコードされたストライドは、デコードされた行またはフレームのペイロードに対して十分な大きさであり、デコードされたサンプル サイズに合わせて配置されている必要があります。
- 転送構文、行、列、ピクセルあたりのサンプル、割り当てられたビット、ピクセル表現、平面構成、フレーム数、ピクセル データ要素など、ピクセルに影響を与えるメタデータを変更する場合は、古いデコード レイアウトの仮定を再利用しないでください。
- ピクセルに影響するメタデータが変更された場合は、次の `decode_into()` の前に、新しい `DecodePlan` と一致する出力バッファーを作成します。
- `decode_into()` は、ベンチマークまたはホット ループの再利用シナリオ、または単一フレーム入力とマルチフレーム入力の両方で同じバッファ管理フローが必要な場合に適切なパスです。
- Python では、プランが明示的な行ストライドまたはフレーム ストライドを要求する場合、`to_array(plan=...)` はパックされた C 連続配列の代わりにカスタム ストライドを含む NumPy 配列を返す場合があります。
- Python では、`decode_into()` には書き込み可能な C 連続宛先オブジェクトが必要です。カスタム ストライドの結果の場合は、連続したバッキング ストレージにデコードしてから、明示的なストライドを使用して NumPy ビューを通じて公開します。
- `to_array()` は、最も手早く成功しやすい基本ルートです。

## 関連ドキュメント

- [クイックスタート](quickstart.md)
- [ピクセルエンコード](pixel_encode.md)
- [ピクセル変換メタデータ解像度](../reference/pixel_transform_metadata.md)
