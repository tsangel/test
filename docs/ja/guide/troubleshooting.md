# トラブルシューティング

最初のビルド、読み取り、デコード、または書き込みの試行が失敗し、考えられる原因に最も早くたどり着きたい場合は、このページを使用します。

## よくある失敗パターン

- ホイールのビルドがコンパイル前に失敗する:
Python、`pip`、`cmake`、コンパイラ ツールチェーン、アクティブな仮想環境を確認します。
- 部分的にロードされたファイルで、later-tag の突然変異が発生します。
最初にさらに多くのファイルをロードするか、まだ解析されていないデータ要素の変更を避けてください。
- `decode_into()` が配列 shape、dtype、またはバッファー サイズの不一致を報告します。
行、列、ピクセルごとのサンプル、フレーム数、出力アイテムサイズを再確認します。
- 文字セットの書き換えが失敗するか、置換が発生します。
宣言されたターゲット文字セットとエンコードエラーポリシーを確認します。
- タグ/パスの検索が解決されない:
キーワードのスペルまたは点線のパス形式を確認してください

## Charset宣言の修復

このパスは、保存されているテキスト バイトはすでに正しいが、`(0008,0005) Specific Character Set` が見つからないか間違っている場合にのみ使用してください。その場合、基になるバイトが正常であっても、`to_utf8_string()` または `to_person_name()` が失敗する可能性があります。

このパスを通常のトランスコード ワークフローとして使用しないでください。テキストを別の文字セットに書き換える必要がある場合は、代わりに `set_specific_charset()` を使用してください。

**C++**

```cpp
#include <dicom.h>
#include <iostream>

using namespace dicom::literals;

dicom::DicomFile file;
auto& study = file.add_dataelement("StudyDescription"_tag, dicom::VR::LO);

// これらのバイトはすでに UTF-8 ですが、データセットはその事実を宣言するのを忘れていました。
study.from_string_view("심장 MRI");

if (!study.to_utf8_string()) {
    std::cout << "decode failed before declaration repair\n";
}

// 宣言のみを修正します。既存のバイトはそのまま残ります。
file.set_declared_specific_charset(dicom::SpecificCharacterSet::ISO_IR_192);

if (auto utf8 = study.to_utf8_string()) {
    std::cout << *utf8 << '\n';
}
```

**パイソン**

```python
import dicomsdl as dicom

df = dicom.DicomFile()
study = df.dataset.add_dataelement(dicom.Tag("StudyDescription"), dicom.VR.LO)

# これらのバイトはすでに UTF-8 ですが、データセットはその事実を宣言するのを忘れていました。
study.from_string_view("심장 MRI")

print(study.to_utf8_string())

# 宣言のみを修正します。既存のバイトはそのまま残ります。
df.set_declared_specific_charset("ISO_IR 192")

print(study.to_utf8_string())
```

## 次にどこを見るべきか

- 読み取り/デコード失敗: [エラー処理](error_handling.md)
- 文字セットのテキストと PN の概要: [文字セットと人物名](charset_and_person_name.md)
- ネストされたパスの問題: [シーケンスとパス](sequence_and_paths.md)
- ピクセル エンコードの問題: [ピクセル エンコードの制約](../reference/pixel_encode_constraints.md)
- 正確な障害カテゴリ: [エラー モデル](../reference/error_model.md)
