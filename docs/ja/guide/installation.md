# インストール

再現可能なインストール / ビルド手順が必要なときや、dicomsdl が現在どの環境を対象にしているかを確認したいときに、このページを参照してください。

## 環境サマリー

### Python / PyPI

- CPython `3.9` to `3.14`
- 現在の wheel CI 対象:
  - Linux `x86_64`
  - macOS `x86_64`
  - macOS `arm64`
  - Windows `AMD64`
- 現在 `musllinux` wheel はビルドしていません
- 基本インストールだけで、メタデータ参照、ファイル I/O、トランスコードのワークフローを利用できます
- NumPy ベースの pixel helper と Pillow プレビュー用の optional extra を用意しています

### C++ / source build

- `git`
- `CMake 3.16+`
- `C++20` compiler
- `Ninja` を推奨しますが、必須なのは一部の Windows toolchain だけです
- Python が必要なのは Python binding や wheel をビルドするときだけです

### Windows toolchain

`build.bat` は、次の source build 手順を文書化し、サポートしています。

- `MSVC` (`cl.exe`)
- `clang-cl` with `Ninja`
- `MSYS2 clang64` with `Ninja`

`DICOMSDL_WINDOWS_TOOLCHAIN` を設定しない場合、`build.bat` は `PATH` 上で利用可能なものを見て `msvc`、`clangcl`、`clang64` の順に自動選択します。

### macOS deployment target

- `x86_64` builds default to macOS `10.15`
- `arm64` builds default to macOS `11.0`
- 明示的な制御が必要な場合だけ `MACOSX_DEPLOYMENT_TARGET` を設定してください

## 経路を選ぶ

- PyPI install: Python binding を最速で試せる方法
- checkout からの C++ build: dicomsdl を自分の C++ アプリケーションに組み込むときに最適
- Unix 系 `build.sh`: macOS / Linux build 用の便利なラッパー
- checkout からの Python source build: [Build Python From Source](../developer/build_python_from_source.md) を参照
- Windows `build.bat`: 明示的に toolchain を選びたいときの推奨エントリポイント

## PyPI インストール

```bash
python -m pip install --upgrade pip
pip install dicomsdl
```

利用中の環境で `pip` が source build にフォールバックする場合は、先に `cmake` を入れてください。

任意の Python extra:

- `pip install "dicomsdl[numpy]"`: `to_array(...)`、`decode_into(...)`、NumPy ベースの pixel helper 用
- `pip install "dicomsdl[numpy,pil]"`: `to_pil_image(...)` convenience helper も使いたい場合 (`numpy` + `Pillow`)

サーバー側で metadata access、file I/O、transcode workflow だけが必要なら、基本の `pip install dicomsdl` で十分です。

## checkout から C++ をビルドする

```bash
git clone https://github.com/tsangel/dicomsdl.git
cd dicomsdl
cmake -S . -B build -DDICOM_BUILD_EXAMPLES=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Linux と macOS ではこれが標準的な CMake 経路です。Windows では、すでに特定の generator / toolchain に固定していない限り `build.bat` を優先してください。

## Unix 系 `build.sh`

macOS と Linux で使う wrapper script は次のとおりです。

```bash
./build.sh
```

特定の target だけをビルドするには:

```bash
./build.sh dicomsdl
./build.sh dicomsdl _dicomsdl
```

末尾の引数は `cmake --build --target ...` の target として渡されます。

## Windows `build.bat`

最短の Windows エントリポイントは次のとおりです。

```bat
call build.bat dicomsdl
```

特定の toolchain を強制したい場合:

```bat
set DICOMSDL_WINDOWS_TOOLCHAIN=msvc
call build.bat dicomsdl
```

必要に応じて `msvc` を `clangcl` または `clang64` に置き換えてください。

`MSVC` と `clang-cl` は初期化済みの Visual Studio developer environment で使ってください。`clang64` は `clang`、`clang++`、`cmake`、`ninja` が `PATH` にある MSYS2 `clang64` 環境で実行してください。

`build.sh` と同様に、末尾の引数は `cmake --build --target ...` の target として渡されます。

## 共通 build script オプション

`build.sh` と `build.bat` は同じコア環境変数を使います。

- `BUILD_DIR`: build directory のパス
- `BUILD_TYPE`: `Release` や `Debug` などの build configuration
- `BUILD_TESTING=ON|OFF`: CTest target を有効化または無効化
- `DICOM_BUILD_EXAMPLES=ON|OFF`: example binary をビルドするかどうか
- `RUN_TESTS=1|0`: build 後に CTest を実行するかどうか
- `BUILD_WHEEL=1|0`: Python wheel ステップをビルドするかどうか
- `WHEEL_ONLY=1`: top-level CMake configure/build を飛ばして wheel だけをビルド
- `WHEEL_DIR=...`: 生成した wheel の出力ディレクトリ
- `PYTHON_BIN=...`: wheel build に使う Python interpreter
- `CMAKE_EXTRA_ARGS="..."`: raw CMake configure argument を追加
- `BUILD_PARALLELISM=N`: 自動検出した並列 build 数を上書き
- `RESET_CMAKE_CACHE=1`: generator/compiler 設定が変わったときに古い CMake cache をクリア
- `DICOMSDL_PIXEL_DEFAULT_MODE=builtin|shared|none`: デフォルト codec plugin mode
- `DICOMSDL_PIXEL_<CODEC>_MODE=builtin|shared|none`: `JPEG`、`JPEGLS`、`JPEG2K`、`HTJ2K`、`JPEGXL` ごとの override

例:

```bash
BUILD_TESTING=OFF BUILD_WHEEL=0 ./build.sh dicomsdl
BUILD_PARALLELISM=8 CMAKE_EXTRA_ARGS="-DDICOM_BUILD_PYTHON=OFF" ./build.sh
```

```bat
set BUILD_WHEEL=0
set RUN_TESTS=0
set CMAKE_EXTRA_ARGS=-DDICOM_BUILD_PYTHON=OFF
call build.bat dicomsdl
```

## OS 別オプション

### macOS / `build.sh`

- `MACOSX_DEPLOYMENT_TARGET=...`: デフォルト deployment target を上書き
- `CMAKE_OSX_ARCHITECTURES=...`: target architecture を明示的に選ぶ
- `ARCHFLAGS=...`: `CMAKE_OSX_ARCHITECTURES` が unset のとき wheel build の architecture 検出に影響

### Linux / `build.sh`

- `CMAKE_GENERATOR=...`: script の自動選択ではなく generator を強制
- `BUILD_PARALLELISM=N`: CPU 使用量を明示的に制限したいときに便利

### Windows / `build.bat`

- `DICOMSDL_WINDOWS_TOOLCHAIN=auto|msvc|clangcl|clang64`: Windows toolchain を選ぶ
- `CMAKE_GENERATOR=...`: script が選んだ generator を上書き
- `DICOMSDL_MSVC_ENABLE_LTCG=ON|OFF`: MSVC link-time code generation を切り替え
- `DICOMSDL_MSVC_PGO=OFF|GEN|USE`: MSVC PGO mode を制御
- `DICOMSDL_MSVC_PGO_DIR=...`: `.pgd/.pgc` profile data のディレクトリ
- `FORCE_WHEEL_RELEASE=1|0`: wheel build を `Release` に固定するか、non-Release build を許可するか
- `VALIDATE_WHEEL_RUNTIME=1|0`: Windows wheel runtime validation を有効化またはスキップ

## Python source build

ローカル wheel、editable に近い開発フロー、custom Python build 環境が必要なら [Build Python From Source](../developer/build_python_from_source.md) を使ってください。

## 関連ドキュメント

- [Quickstart](quickstart.md)
- [Python DataSet Guide](python_dataset_guide.md)
- [Build Python From Source](../developer/build_python_from_source.md)
