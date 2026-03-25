# クイックスタート

## Python
ほとんどの利用者は PyPI から始めれば十分です。
1. 必要条件: Python 3.9+, `pip`
2. PyPI からインストール

```bash
python -m pip install --upgrade pip
pip install "dicomsdl[numpy,pil]"
```

利用中の環境で `pip` が source build にフォールバックする場合は、先に `cmake` を入れてください。

```{note}
サーバー上で metadata access、file I/O、transcode workflow だけが必要なら、
`pip install dicomsdl` だけで十分です。
```

source build、custom wheel、テスト workflow が必要なら [Build Python From Source](../developer/build_python_from_source.md) を参照してください。
プラットフォーム別のインストール詳細は [Installation](installation.md) を参照してください。

3. metadata を読む

```pycon
>>> import dicomsdl as dicom
>>> df = dicom.read_file("sample.dcm")
>>> df["PatientName"].value
PersonName(Doe^Jane)
>>> df["Rows"].value, df["Columns"].value
(512, 512)
```

`DicomFile` は root `DataSet` の access helper を forwarding するので、Python では `df["Rows"]`、`df.get_value(...)`、`df.get_dataelement(...)` をそのまま使えます。
dataset の境界を明示したいときは `df.dataset` を使ってください。
`PatientName` は `PN` なので、`.value` は通常の Python 文字列ではなく `PersonName(...)` オブジェクトとして表示されます。
オブジェクトモデル、metadata lookup の規則、decode 全体の流れは [Core Objects](core_objects.md)、[Python DataSet Guide](python_dataset_guide.md)、[Pixel Decode](pixel_decode.md) を参照してください。

4. ピクセルを NumPy 配列に decode する

```pycon
>>> import dicomsdl as dicom
>>> df = dicom.read_file("sample.dcm")
>>> arr = df.to_array()
>>> arr.shape
(512, 512)
>>> arr.dtype
dtype('uint16')
```

decode option、frame 選択、出力 layout 制御が必要なら [Pixel Decode](pixel_decode.md) を参照してください。

5. Pillow で手早く画像をプレビューする

```bash
pip install "dicomsdl[numpy,pil]"
```

```python
import dicomsdl as dicom

df = dicom.read_file("sample.dcm")
image = df.to_pil_image(frame=0)
image.show()
```

`to_pil_image()` は素早い目視確認のための限定的な convenience helper です。
解析パイプラインや再現可能な処理では `to_array()` を優先してください。`show()` はローカル GUI / viewer に依存するため、headless 環境では動かないことがあります。
decode option や配列中心の workflow が必要なら [Pixel Decode](pixel_decode.md) を参照してください。

6. `HTJ2KLossless` に transcode して新しいファイルを書く

```python
from pathlib import Path

import dicomsdl as dicom

in_path = Path("in.dcm")
out_path = Path("out_htj2k_lossless.dcm")

df = dicom.read_file(in_path)
df.set_transfer_syntax("HTJ2KLossless")
df.write_file(out_path)

print("Input bytes:", in_path.stat().st_size)
print("Output bytes:", out_path.stat().st_size)
```

代表的なファイルでは、出力はおおよそ次のようになります。

```text
Input bytes: 525312
Output bytes: 287104
```

この file-to-file transcode 経路は、基本の `pip install dicomsdl` だけでも使えます。実際のサイズ変化は source transfer syntax、pixel content、metadata に依存します。
lossy encode option、codec の制約、streaming write の説明は [Pixel Encode](pixel_encode.md)、[Pixel Encode Constraints](../reference/pixel_encode_constraints.md)、[Encode-capable Transfer Syntax Families](../reference/codec_support_matrix.md) を参照してください。

7. `memoryview` で `DataElement` の value bytes にアクセスする

```python
import dicomsdl as dicom

df = dicom.read_file("sample.dcm")
elem = df["PixelData"]
if elem:
    raw = elem.value_span()  # memoryview
    print("Raw bytes:", raw.nbytes)
    print("Head:", list(raw[:8]))
```

非圧縮の `512 x 512` `uint16` 画像なら:

```text
Raw bytes: 524288
Head: [34, 12, 40, 12, 36, 12, 39, 12]
```

先頭の bytes はファイルによって変わります。この `value_span()` view は native / uncompressed `PixelData` 向けです。圧縮された encapsulated transfer syntax では `PixelData` は `PixelSequence` として保存されるため、`elem.value_span()` は空になり、代わりに `elem.pixel_sequence.frame_encoded_memoryview(0)` または `elem.pixel_sequence.frame_encoded_bytes(0)` を使う必要があります。
`raw` を使っている間は `df` を生かしておいてください。この memoryview は読み込まれた DICOM オブジェクトが所有する bytes を参照しており、その bytes が置き換えられると無効になります。
raw byte の意味や encapsulated `PixelData` の詳細は [DataElement Reference](../reference/dataelement_reference.md) と [Pixel Reference](../reference/pixel_reference.md) を参照してください。

decode safety 全体のモデルが必要なら [Pixel Decode](pixel_decode.md) と [Error Handling](error_handling.md) を参照してください。

## C++
リポジトリ checkout からビルドします。
必要条件: `git`、`CMake`、`C++20` compiler
1. リポジトリを clone

```bash
git clone https://github.com/tsangel/dicomsdl.git
cd dicomsdl
```

2. configure と build
```bash
cmake -S . -B build -DDICOM_BUILD_EXAMPLES=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

3. 使用例
```cpp
#include <dicom.h>
#include <diagnostics.h>
#include <iostream>
#include <memory>
using namespace dicom::literals;

int main() {
  auto file = dicom::read_file("sample.dcm");
  auto& ds = file->dataset();

  long rows = ds["Rows"_tag].to_long().value_or(0);
  // 存在感が重要であるなら、値を取り出す前に要素の存在で分岐する。
  long cols = 0;
  if (auto& e = ds["Columns"_tag]; e) {
    cols = e.to_long().value_or(0);
  }
  std::cout << "Image size: " << rows << " x " << cols << '\n';
}
```

典型的な出力は次のようになります。

```text
Image size: 512 x 512
```

より詳しい C++ API の説明は [C++ API Overview](../reference/cpp_api.md) と [DataSet Reference](../reference/dataset_reference.md) を参照してください。

4. `ok &= ...` と error check でまとめて設定する
```cpp
#include <dicom.h>
#include <diagnostics.h>
#include <memory>
#include <iostream>
using namespace dicom::literals;

int main() {
  dicom::DataSet ds;
  auto reporter = std::make_shared<dicom::diag::BufferingReporter>(256);
  dicom::diag::set_thread_reporter(reporter);

  bool ok = true;
  ok &= ds.add_dataelement("Rows"_tag, dicom::VR::US).from_long(512);
  ok &= ds.add_dataelement("Columns"_tag, dicom::VR::US).from_long(-1); // 失敗例

  if (!ok) {
    for (const auto& msg : reporter->take_messages()) {
      std::cerr << msg << '\n';
    }
  }
  dicom::diag::set_thread_reporter(nullptr);
}
```

上の例では意図的に `Columns = -1` を失敗させているので、出力はおおよそ次のようになります。
`VR::US` は unsigned 値しか受け付けないため、`Columns = -1` で range error になります。

```text
[ERROR] from_long tag=(0028,0011) vr=US reason=value out of range for VR
```

- 実行可能な完全例: `examples/batch_assign_with_error_check.cpp`
- `add_dataelement(...)` は `DataElement&` を返すので、write helper は `.` で連結します。
より広い書き込みパターンや失敗処理の説明は [C++ DataSet Guide](cpp_dataset_guide.md) と [Error Handling](error_handling.md) を参照してください。
