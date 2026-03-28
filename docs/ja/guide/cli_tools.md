# CLI ツール

DicomSDL には、ユーザー向けのコマンドラインツールが 4 つあります。

- `dicomdump`: 読みやすい DICOM dump を出力する
- `dicomshow`: Pillow を使って 1 フレームをすばやく表示する
- `dicomconv`: transfer syntax を変更して新しいファイルを書き出す
- `dicomview`: フォルダ、画像、dump を Qt UI で横断的に見るブラウザ

## コマンドの導入方法

### Python wheel から導入する

```bash
pip install dicomsdl
```

これで `dicomdump`、`dicomconv`、`dicomshow`、`dicomview` のコンソールスクリプトが入ります。

`dicomshow` は Pillow ベースのプレビュー経路を使うため、実際には次のインストールが便利です。

```bash
pip install "dicomsdl[numpy,pil]"
```

Qt ブラウザも使うなら viewer extra が必要です。

```bash
pip install "dicomsdl[viewer]"
```

Pillow プレビュー経路と Qt viewer の両方を使うなら、たとえば次のように導入できます。

```bash
pip install "dicomsdl[numpy,pil,viewer]"
```

### ソースビルドから使う

`-DDICOM_BUILD_EXAMPLES=ON` を付けて C++ 例をビルドすると、build ツリーに
`dicomdump` と `dicomconv` のバイナリが作られます。

```bash
cmake -S . -B build -DDICOM_BUILD_EXAMPLES=ON
cmake --build build

./build/dicomdump sample.dcm
./build/dicomconv in.dcm out.dcm ExplicitVRLittleEndian
```

独立した C++ 用 `dicomshow` / `dicomview` 実行ファイルはありません。どちらも Python の
コンソールスクリプト entry point です。

## `dicomdump`

`dicomdump` は、1 つ以上の DICOM ファイルを読みやすいテキストとして確認したいときに使います。
各ファイルを読み込み、`DicomFile.dump(...)` の結果を表示します。

### `dicomdump` の使い方

```bash
dicomdump [--max-print-chars N] [--no-offset] [--with-filename] <file> [file...]
```

### `dicomdump` リファレンス

位置引数:

| 引数 | 意味 |
| --- | --- |
| `paths` | 1 つ以上の入力パスです。`*.dcm` のような wildcard パターンも CLI 側で展開します。 |

オプション:

| オプション | 意味 |
| --- | --- |
| `--max-print-chars N` | 長い printable value を `N` 文字で打ち切ります。既定値は `80` です。 |
| `--no-offset` | `OFFSET` 列を隠します。 |
| `--with-filename` | 各出力行の先頭に `filename:` を付けます。複数入力のときは既定で有効になります。 |

### `dicomdump` の例

```bash
dicomdump sample.dcm
dicomdump a.dcm b.dcm
dicomdump --no-offset --max-print-chars 120 sample.dcm
dicomdump "*.dcm"
```

入力が複数ある場合は、出力が混ざっても分かるように各行へファイル名が付きます。

## `dicomshow`

`dicomshow` はシェルからの簡単な目視確認用ツールです。DICOM ファイルを 1 つ読み、
`to_pil_image(frame=...)` で 1 フレームを作って `Pillow.Image.show()` を呼びます。

### `dicomshow` の使い方

```bash
dicomshow [--frame N] <input.dcm>
```

### `dicomshow` リファレンス

位置引数:

| 引数 | 意味 |
| --- | --- |
| `input` | 入力 DICOM ファイルパスです。 |

オプション:

| オプション | 意味 |
| --- | --- |
| `--frame N` | 表示する 0-based フレーム番号です。既定値は `0` です。 |

### `dicomshow` の注意

- `dicomshow` は簡易プレビュー用であり、診断用ビューアではありません。
- ローカル GUI / viewer の関連付けに依存するため、headless 環境では動かないことがあります。
- Pillow や NumPy がない場合は `DicomSDL[numpy,pil]` を入れてください。

### `dicomshow` の例

```bash
dicomshow sample.dcm
dicomshow --frame 5 multiframe.dcm
```

## `dicomconv`

`dicomconv` は、シェルスクリプトやターミナルからファイル単位で transfer syntax を
変換したいときに使います。

内部では入力ファイルを読み、`set_transfer_syntax(...)` を適用して、新しいパスへ書き出します。

### `dicomconv` の使い方

```bash
dicomconv <input.dcm> <output.dcm> <transfer-syntax> [options]
```

`<transfer-syntax>` には次を指定できます。

- `ExplicitVRLittleEndian` のような transfer syntax keyword
- `1.2.840.10008.1.2` のような dotted UID 文字列
- `jpeg`, `jpeg2k`, `htj2k-lossless`, `jpegxl` のような shortcut alias

### `dicomconv` リファレンス

位置引数:

| 引数 | 意味 |
| --- | --- |
| `input` | 入力 DICOM ファイルパスです。 |
| `output` | 出力 DICOM ファイルパスです。 |
| `transfer_syntax` | target transfer syntax keyword、dotted UID 文字列、または shortcut alias です。 |

オプション:

| オプション | 適用先 | 意味 |
| --- | --- | --- |
| `--codec {auto,none,rle,jpeg,jpegls,j2k,htj2k,jpegxl}` | 全体 | target transfer syntax から推定せず、codec オプション系統を明示します。 |
| `--quality N` | `jpeg` | JPEG quality。範囲は `[1, 100]` です。 |
| `--near-lossless-error N` | `jpegls` | JPEG-LS `NEAR`。範囲は `[0, 255]` です。 |
| `--target-psnr V` | `j2k`, `htj2k` | target PSNR です。 |
| `--target-bpp V` | `j2k`, `htj2k` | target bits-per-pixel です。 |
| `--threads N` | `j2k`, `htj2k`, `jpegxl` | encoder thread 設定です。`-1` は auto、`0` はライブラリ既定値です。 |
| `--color-transform` | `j2k`, `htj2k` | MCT color transform を有効にします。 |
| `--no-color-transform` | `j2k`, `htj2k` | MCT color transform を無効にします。 |
| `--distance V` | `jpegxl` | JPEG-XL distance。範囲は `[0, 25]` です。`0` は lossless を意味します。 |
| `--effort N` | `jpegxl` | JPEG-XL effort。範囲は `[1, 10]` です。 |

完全な help、例、対応している target transfer syntax 一覧は `dicomconv -h` で確認できます。

### `dicomconv` の例

```bash
dicomconv in.dcm out.dcm ExplicitVRLittleEndian
dicomconv in.dcm out.dcm 1.2.840.10008.1.2
dicomconv in.dcm out.dcm jpeg --quality 92
dicomconv in.dcm out.dcm jpegls-near-lossless --near-lossless-error 3
dicomconv in.dcm out.dcm jpeg2k --target-psnr 45 --threads -1
dicomconv in.dcm out.dcm htj2k-lossless --no-color-transform
dicomconv in.dcm out.dcm jpegxl --distance 1.5 --effort 7 --threads -1
```

## `dicomview`

`dicomview` は軽量な開発者向け DICOM ブラウザです。1 フレームだけを素早く確認する
`dicomshow` と違い、フォルダを開いてファイル一覧、基本メタデータ、画像プレビュー、
dump を 1 つのウィンドウで確認できます。

### `dicomview` の使い方

```bash
dicomview [<input>]
```

`<input>` には DICOM ファイル 1 つまたはディレクトリ 1 つを指定できます。省略した場合は、
最後に開いたパスを優先して復元し、それがなければ現在の作業ディレクトリから始めます。

### `dicomview` の注意

- `dicomview` は軽量な開発者向けブラウザであり、診断用ビューアではありません。
- `PySide6` が必要です。`pip install "dicomsdl[viewer]"` を推奨します。
- カラム表示、カラム幅、最後に開いたパス、ウィンドウサイズなどの状態は `QSettings` に保存されます。
- `DICOMVIEW_SETTINGS_PATH=/path/to/dicomview.ini` を指定すると設定保存先を上書きできます。
- GUI backend のない headless CI やサーバー環境では実行できない場合があります。

### `dicomview` の例

```bash
dicomview
dicomview sample.dcm
dicomview sample_folder/
```

## 終了コード

4 つのコマンドは共通して次の規則です。

- 成功時は `0`
- 入力、parse、decode、encode、write のどこかで失敗すると `1`

エラーは `dicomdump:`, `dicomshow:`, `dicomconv:`, `dicomview:` のような接頭辞付きで標準エラーへ出力されます。

## 関連ドキュメント

- [Installation](installation.md)
- [File I/O](file_io.md)
- [Pixel Decode](pixel_decode.md)
- [Pixel Encode](pixel_encode.md)
- [Pixel Encode Constraints](../reference/pixel_encode_constraints.md)
