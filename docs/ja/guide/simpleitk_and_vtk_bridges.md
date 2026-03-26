# SimpleITK と VTK のブリッジ

DicomSDL は、Python ワークフロー全体を大きく変えずに、DICOM ピクセルデータをデコードして SimpleITK や VTK のオブジェクトへ渡せます。

ブリッジモジュールは次の 2 つです。

- `dicomsdl.simpleitk_bridge`
- `dicomsdl.vtk_bridge`

DicomSDL に DICOM の解析、スライス並び替え、ピクセルデコード、rescale 処理を任せて、その後の処理を SimpleITK や VTK で続けたい場合に使います。

## このブリッジが行うこと

2 つのブリッジモジュールは内部で同じ volume 構築処理を共有し、最後の変換段階だけが異なります。

このブリッジでできること:

- 単一ファイルの 2D 画像、またはファイルベースの DICOM シリーズを読む
- stack として扱える単一ファイルの multiframe grayscale volume を読む
- スライスを並べ替えて volume を組み立てる
- DICOM geometry から `spacing`、`origin`、`direction` を計算する
- `to_modality_value=True` のとき frame ごとの modality rescale を適用する
- デコードした volume を `SimpleITK.Image` または `vtkImageData` に変換する

これらは `sitk.ReadImage(...)` や `vtkDICOMImageReader` の完全な drop-in replacement ではありません。
DicomSDL の上に載った軽量なブリッジ API と考えるのが適切です。

## インストール

必要な toolkit だけをインストールしてください。

```bash
pip install "dicomsdl[numpy]" SimpleITK vtk
```

どちらか一方だけ使う場合:

```bash
pip install "dicomsdl[numpy]" SimpleITK
pip install "dicomsdl[numpy]" vtk
```

## 共通の動作

### 入力

次のどちらかを渡します。

- ファイルベースの DICOM シリーズを含むディレクトリ
- 2D 画像を含む単一の DICOM ファイル
- stack として扱える multiframe grayscale volume を含む単一の DICOM ファイル

このガイドと例は、単一ファイルの 2D 画像、stack として扱える multiframe grayscale volume、file-per-slice シリーズを中心にしています。
non-stack の multiframe localizer や、より複雑な enhanced multi-frame レイアウトが重要なワークフローでは、別途検証してください。

ディレクトリに sidecar ファイルや DICOM ではないファイルが混在して
いる場合、ブリッジは DICOM ではない項目を無視します。複数の
`SeriesInstanceUID` が含まれている場合は、黙って結合せずにエラーを
返します。

### Geometry

ブリッジは次の geometry 情報を保持します。

- `spacing`
- `origin`
- `direction`

可能な場合、これらは DICOM の orientation と position metadata から求められます。

stack として扱える 3D 入力では、volume geometry を物理順に正規化します。

- slice と frame は base slice normal への projection で並べ替えます
- 出力の先頭 slice は projected position が最も小さい slice です
- `spacing[2]` は常に正の値を保ちます
- stack の向きは `direction` で表します

ファイルベースの series に orientation の異なる slice が混在する場合は、
dominant orientation group のみを使い、localizer のような outlier は除外します。

### 出力 dtype の方針

ブリッジは、すべてを float に持ち上げるのではなく、実用的な scalar type を保つようにしています。

- rescale なし: stored dtype を維持
- `slope ~= 1` かつ intercept が整数: 整数 dtype を維持
- fractional rescale: `float32` に昇格

そのため、各 toolkit のネイティブ reader とは出力 dtype が異なることがあります。
reader ごとに別の scalar type 方針を持つことがあるためです。

### Modality value

2 つの公開 API はどちらも `to_modality_value=True` がデフォルトです。

特に PET では frame ごとの rescale が重要になることが多いです。
保存済みの値が必要なら `to_modality_value=False` を指定してください。

### 2D 画像

単一ファイルの 2D 画像は 1 スライス画像として扱われます。
RGB などの vector pixel data は component 軸を保持します。

### Multiframe volume

stack として扱える単一ファイルの multiframe grayscale volume は `(z, y, x)` として扱われます。
この場合も file-per-slice series と同じ物理順の canonicalization を適用します。
frame position が 1 本の stack 方向にそろわない場合、ブリッジは geometry を推測せず、現在は `NotImplementedError` を返します。

## SimpleITK

公開 API:

- `read_series_image(path, *, to_modality_value=True) -> SimpleITK.Image`
- `to_simpleitk_image(volume) -> SimpleITK.Image`

例:

```python
from dicomsdl.simpleitk_bridge import read_series_image

image = read_series_image(
    r"..\sample\PT\00013 Torso PET AC OSEM",
    to_modality_value=True,
)

print(image.GetSize())
print(image.GetSpacing())
```

まず中間のデコード済み volume を扱いたい場合:

```python
from dicomsdl.simpleitk_bridge import read_series_volume, to_simpleitk_image

volume = read_series_volume(r"..\sample\PT\00013 Torso PET AC OSEM")
image = to_simpleitk_image(volume)
```

返り値は通常の `SimpleITK.Image` なので、そのまま SimpleITK の filter や pipeline に渡せます。

## VTK

公開 API:

- `read_series_image_data(path, *, to_modality_value=True, copy=False) -> vtkImageData`
- `to_vtk_image_data(volume, *, copy=False) -> vtkImageData`

例:

```python
from dicomsdl.vtk_bridge import read_series_image_data

image = read_series_image_data(
    r"..\sample\PT\00013 Torso PET AC OSEM",
    to_modality_value=True,
    copy=False,
)

print(image.GetDimensions())
print(image.GetSpacing())
```

まず中間のデコード済み volume を扱いたい場合:

```python
from dicomsdl.vtk_bridge import read_series_volume, to_vtk_image_data

volume = read_series_volume(r"..\sample\PT\00013 Torso PET AC OSEM")
image = to_vtk_image_data(volume, copy=False)
```

`copy=False` は可能な場合に zero-copy を使う高速経路です。
元の NumPy ベースの bridge オブジェクトから独立した VTK 所有のコピーが必要なら `copy=True` を使ってください。

## ネイティブ reader とブリッジの使い分け

reader クラス自体の API 互換性が重要なら、各 toolkit のネイティブ reader を使うほうが適しています。

次の点が重要なら DicomSDL bridge のほうが向いています。

- decode と rescale の挙動を DicomSDL に任せたい
- 2 つの toolkit で同じ geometry と dtype 方針を使いたい
- 正の slice spacing を持つ canonical physical-order 3D volume が欲しい
- 例やベンチマークで DicomSDL とネイティブ reader を簡単に比較したい

実用上の注意:

- 一般的な SimpleITK の DICOM series 経路はすでに GDCM を使います
- VTK は環境によって `vtkDICOMImageReader` または `vtkgdcm` と比較することになります
- bridge は volume を公開する前に stack order を canonicalize するため、
  geometry が native reader の保存順表現と意図的に異なることがあります

## 例とノートブック

このリポジトリには、すぐ実行できる例とノートブックが含まれています。

- `examples/python/itk_vtk/dicomsdl_to_simpleitk_pet_volume.py`
- `examples/python/itk_vtk/dicomsdl_to_vtk_pet_volume.py`
- `tutorials/basic_vtk3d.ipynb`
- `tutorials/timeit_itk.ipynb`
- `tutorials/timeit_vtk.ipynb`
- `benchmarks/python/benchmark_pet_volume_readers.py`
- `benchmarks/python/benchmark_wg04_readers.py`
