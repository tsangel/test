# 文字セットと人名

テキスト VR または `PN` 値が `SpecificCharacterSet` に依存し、デコードされた UTF-8 または構造化された名前コンポーネントが必要な場合は、`to_utf8_string()` / `to_person_name()` を使用します。 `to_string_view()` は、文字セットのデコードを行わずに通常の VR トリミング後に保存されたバイトを意図的に必要とする場合にのみ使用します。文字セットを認識した書き込みが必要な場合は、`from_utf8_view()` / `from_person_name()` を使用します。現在のデータセットのサブツリーを新しい文字セットに正規化またはトランスコードする場合は、`set_specific_charset()` を使用します。すでに保存されているバイトの欠落または間違った宣言を修復する必要がある場合は、[トラブルシューティング](troubleshooting.md) を参照してください。

スコープに関する注意: 以下の読み取り/書き込みヘルパーのほとんどは `DataElement` メソッドです。 `(0008,0005)` を書き換えまたは再宣言する文字セット変更 API は、`DataSet` / `DicomFile` 上でライブになります。

## 主要な文字セットと PN API

**C++**

`DataElement` メソッド

- `to_string_view()` / `to_string_views()`
- 文字セットをデコードせずに、トリミングされた生の保存バイトを読み取ります。
- `to_utf8_string()` / `to_utf8_strings()`
- 文字セットのデコード後にテキスト VR を所有する UTF-8 として読み取ります。
- `to_person_name()` / `to_person_names()`
- `PN` 値をアルファベット、表意文字、および発音のグループに解析します。
- `from_utf8_view()` / `from_utf8_views()`
- UTF-8 テキストを、所有するデータセットで現在宣言されている文字セットにエンコードします。
- `from_person_name()` / `from_person_names()`
- 構造化された `PersonName` 値を `PN` 要素にシリアル化します。

`DataSet` / `DicomFile` メソッド

- `set_specific_charset()`
- 既存のテキスト バイトを新しい文字セットにトランスコードし、`(0008,0005)` を一貫して更新します。

`Helper types`

- `PersonName` / `PersonNameGroup`
- `^` および `=` 文字列を手動で処理せずに、`PN` 値を構築または検査するためのヘルパー タイプ。

**パイソン**

`DataElement` メソッド

- `to_string_view()` / `to_string_views()`
- 文字セットをデコードせずに、トリミングされた生の保存テキストを読み取ります。
- `to_utf8_string()` / `to_utf8_strings()`
- テキスト VR をデコードされた UTF-8 文字列として読み取ります。 `return_replaced=True` を使用すると、デコード フォールバックでバイトが置き換えられたかどうかも確認できます。
- `to_person_name()` / `to_person_names()`
- `PN` 値を、アルファベット、表意文字、および発音グループを含む `PersonName` オブジェクトに解析します。
- `from_utf8_view()` / `from_utf8_views()`
- Python `str` データをデータセットの宣言された文字セットにエンコードします。 `return_replaced=True` を使用すると、置換動作を検査できます。
- `from_person_name()` / `from_person_names()`
- `PersonName` オブジェクトを `PN` 要素にシリアル化します。

`DataSet` / `DicomFile` メソッド

- `set_specific_charset()`
- 既存のテキスト値を新しい文字セットにトランスコードし、`(0008,0005)` を一貫して書き換えます。

`Helper types`

- `PersonName(...)` / `PersonNameGroup(...)`
- 構造化された `PN` 値を Python 文字列またはタプルから直接構築します。

## 関連する DICOM 標準セクション

- `Specific Character Set` 属性自体は、[DICOM PS3.3 セクション C.12、一般モジュール](https://dicom.nema.org/medical/dicom/current/output/chtml/part03/sect_C.12.html) の SOP 共通モジュールに属します。
- 文字レパートリーの選択、置換、および ISO/IEC 2022 コード拡張の動作は、[DICOM PS3.5 Chapter 6, Value Encoding](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/chapter_6.html)、特にセクション 6.1 およびセクション 6.1.2.4 ～ 6.1.2.5 で定義されています。
- `PN` ルールは、[DICOM PS3.5 セクション 6.2、値表現](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/sect_6.2.html)、特にセクション 6.2.1、人名 (PN) 値表現で定義されています。
- 日本語、韓国語、Unicode UTF-8、GB18030、GBK の言語固有の例は、有益な [DICOM PS3.5 Annex H](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/chapter_H.html)、[Annex] にあります。 I](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/chapter_I.html)、および[付録 J](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/chapter_J.html)。

## C++

### 保存された生のテキストとデコードされた UTF-8 を比較します。

```cpp
#include <dicom.h>
#include <iostream>

using namespace dicom::literals;

auto file = dicom::read_file("patient_names.dcm");
const auto& patient_name = file->dataset()["PatientName"_tag];

// to_string_view() は、通常の VR トリミング後にのみ保存されたテキスト バイトを提供します。
// ここでは、SpecificCharacterSet デコードは行われません。
if (auto raw = patient_name.to_string_view()) {
    std::cout << "raw: " << *raw << '\n';
}

// to_utf8_string() は、データセットで宣言された SpecificCharacterSet に従ってデコードします。
if (auto utf8 = patient_name.to_utf8_string()) {
    std::cout << "utf8: " << *utf8 << '\n';
}

// to_person_name() はさらに一歩進んで、PN グループとコンポーネントを解析します。
if (auto parsed = patient_name.to_person_name()) {
    if (parsed->alphabetic) {
        std::cout << parsed->alphabetic->family_name() << '\n';
        std::cout << parsed->alphabetic->given_name() << '\n';
    }
}
```

最初の `PatientName` 値が `Hong^Gildong=洪^吉洞=홍^길동` の場合の出力例:

```text
raw: Hong^Gildong=洪^吉洞=홍^길동
utf8: Hong^Gildong=洪^吉洞=홍^길동
Hong
Gildong
```

### 構造化された PersonName を構築して保存する

```cpp
#include <dicom.h>
#include <iostream>

using namespace dicom::literals;

dicom::DicomFile file;
file.set_specific_charset(dicom::SpecificCharacterSet::ISO_IR_192);

dicom::PersonName name;
name.alphabetic = dicom::PersonNameGroup{{"Hong", "Gildong", "", "", ""}};
name.ideographic = dicom::PersonNameGroup{{"洪", "吉洞", "", "", ""}};
name.phonetic = dicom::PersonNameGroup{{"홍", "길동", "", "", ""}};

auto& patient_name = file.add_dataelement("PatientName"_tag, dicom::VR::PN);
if (!patient_name.from_person_name(name)) {
    // from_person_name() も、通常の割り当て失敗を false で報告します。
}

if (auto parsed = patient_name.to_person_name()) {
    std::cout << parsed->alphabetic->family_name() << '\n';
    std::cout << parsed->ideographic->family_name() << '\n';
    std::cout << parsed->phonetic->family_name() << '\n';
}
```

期待される出力:

```text
Hong
洪
홍
```

### 既存のテキスト値を新しい文字セットにトランスコードします。

```cpp
#include <dicom.h>
#include <iostream>

using namespace dicom::literals;

auto file = dicom::read_file("utf8_names.dcm");
bool replaced = false;

// set_specific_charset() はデータセットのサブツリーを探索し、テキストの VR 値を書き換えます。
// 新しい宣言への更新 (0008,0005)。このポリシーにより、
// 文字の目に見える痕跡を残しながら移動するトランスコード
// ターゲットの文字セットを直接表すことはできません。
file->set_specific_charset(
    dicom::SpecificCharacterSet::ISO_IR_100,
    dicom::CharsetEncodeErrorPolicy::replace_unicode_escape,
    &replaced);

// 書き換えられた保存バイトはプレーン ASCII テキストになるため、to_string_view()
// ここでは、 と to_utf8_string() の両方が同じ目に見えるエスケープ マーカーを公開します。
if (auto raw_name = file->dataset()["PatientName"_tag].to_string_view()) {
    std::cout << *raw_name << '\n';
}
std::cout << std::boolalpha << replaced << '\n';
```

`utf8_names.dcm` に `홍길동` が含まれる場合の出力例:

```text
(U+D64D)(U+AE38)(U+B3D9)
true
```

### 宣言とトランスコードの失敗を明示的に処理する

```cpp
#include <dicom.h>
#include <diagnostics.h>
#include <iostream>

using namespace dicom::literals;

try {
    dicom::DicomFile file;
    file.set_specific_charset(dicom::SpecificCharacterSet::ISO_IR_192);

    auto& patient_name = file.add_dataelement("PatientName"_tag, dicom::VR::PN);
    if (!patient_name.from_utf8_view("홍길동")) {
        std::cerr << "initial UTF-8 assignment failed\n";
    }

    // set_specific_charset() は from_utf8_view() とは異なります。
    // データセット全体の宣言/トランスコードの問題が、false を返す代わりにスローされます。
    file.set_specific_charset(
        dicom::SpecificCharacterSet::ISO_IR_100,
        dicom::CharsetEncodeErrorPolicy::strict);
} catch (const dicom::diag::DicomException& ex) {
    std::cerr << ex.what() << '\n';
}
```

## パイソン

### 保存された生のテキストとデコードされた UTF-8 を比較します。

```python
import dicomsdl as dicom

df = dicom.read_file("patient_names.dcm")
elem = df.dataset["PatientName"]

# to_string_view() は、通常の VR トリミング後にのみ、保存されたテキストを返します。
# ここでは、SpecificCharacterSet デコードは行われません。
raw = elem.to_string_view()

# to_utf8_string() は、デコードされた Python str または None を返します。
text, replaced = elem.to_utf8_string(return_replaced=True)

# to_person_name() は、構造化された PersonName または None を返します。
name = elem.to_person_name()
if name is not None and name.alphabetic is not None:
    print(name.alphabetic.family_name)
    print(name.alphabetic.given_name)
```

最初の `PatientName` 値が `Hong^Gildong=洪^吉洞=홍^길동` の場合の出力例:

```text
Hong
Gildong
```

### 構造化された PersonName を構築して保存する

```python
import dicomsdl as dicom

df = dicom.DicomFile()
df.set_specific_charset("ISO_IR 192")

pn = dicom.PersonName(
    alphabetic=("Hong", "Gildong"),
    ideographic=("洪", "吉洞"),
    phonetic=("홍", "길동"),
)

patient_name = df.dataset.add_dataelement(dicom.Tag("PatientName"), dicom.VR.PN)
ok = patient_name.from_person_name(pn)

# 同じ PersonName オブジェクトは、データセットの属性代入用の簡便な糖衣構文でも使えます。
df.PatientName = pn

value = df.PatientName
print(value.alphabetic.family_name)
print(value.ideographic.family_name)
print(value.phonetic.family_name)
```

期待される出力:

```text
Hong
洪
홍
```

### 既存のテキスト値をトランスコードし、置換を検査する

```python
import dicomsdl as dicom

df = dicom.read_file("utf8_names.dcm")

# 多くの場合、目に見えるフォールバックは、厳密な失敗よりも扱いやすいです。
# トランスコードが完了し、置換が完了したため、本番環境のクリーンアップはパスしました。
# 結果のテキストでは依然として明らかです。
replaced = df.set_specific_charset(
    "ISO_IR 100",
    errors="replace_unicode_escape",
    return_replaced=True,
)
print(df.get_dataelement("PatientName").to_string_view())
print(replaced)
```

`utf8_names.dcm` に `홍길동` が含まれる場合に予期される出力:

```text
(U+D64D)(U+AE38)(U+B3D9)
True
```

### 宣言とトランスコードの失敗を明示的に処理する

```python
import dicomsdl as dicom

df = dicom.DicomFile()

try:
    df.set_specific_charset("ISO_IR 192")
    patient_name = df.dataset.add_dataelement(dicom.Tag("PatientName"), dicom.VR.PN)
    ok = patient_name.from_utf8_view("홍길동")
    print(ok)

    # set_specific_charset() は、要求されたトランスコードを実行できない場合に発生します。
    # 選択したエラー ポリシーの下で。
    df.set_specific_charset("ISO_IR 100", errors="strict")
except (TypeError, ValueError) as exc:
    # 無効な文字セット引数の形状または無効なポリシー テキスト。
    print(exc)
except RuntimeError as exc:
    # 基礎となる宣言またはトランスコードステップが失敗しました。
    print(exc)
```

## `set_specific_charset()` ポリシー オプション

最初の引数はターゲットの文字セットを選択します。 2 番目の引数は、ターゲットの文字セットで表現できない文字を処理する方法を選択します。オプションの 3 番目の出力は、実際に置換が行われたかどうかを報告します。これは主に非可逆モードで役立ちます。

すべてのテキスト値がターゲットの文字セットで表現できる場合、すべてのポリシーは同じトランスコードされたデータセットを生成し、`replaced == false` をレポートします。この違いは、一部の既存のテキストが要求されたターゲット文字セットで表現できない場合にのみ問題になります。

ポリシー名は 2 つの API に直接マッピングされます。

- C++: `dicom::CharsetEncodeErrorPolicy::strict`、`::replace_qmark`、`::replace_unicode_escape`
- Python: `errors="strict"`、`"replace_qmark"`、`"replace_unicode_escape"`

たとえば、ソース テキストが `홍길동` で、ターゲットの文字セットが `ISO_IR 100` の場合、そのターゲットの文字セットは韓国語の文字を直接表すことはできません。ポリシーは次のように分岐します。

|比較ポイント | `strict` | `replace_qmark` | `replace_unicode_escape` |
| --- | --- | --- | --- |
|一部のテキストが表現できない場合 | `set_specific_charset()` はスロー/レイズして停止します。 |トランスコードは成功し、`?` に置き換えられます。 |トランスコードは成功し、表示されている `(U+XXXX)` テキストに置き換えられます。 |
| `홍길동 -> ISO_IR 100` の結果の例 |呼び出しが失敗するため、トランスコードされたテキストは生成されません。 | `???` | `(U+D64D)(U+AE38)(U+B3D9)` |
|データセットのコミット |変化なし。 |文字セットが更新され、テキスト VR が `?` に書き換えられます。 |文字セットが更新され、テキスト VR が `(U+XXXX)` マーカーで書き換えられます。 |
| `replaced` 出力 | 呼び出し自体が失敗するため該当しません。 | 少なくとも 1 つ置換が起きた場合に `true`。 | 少なくとも 1 つ置換が起きた場合に `true`。 |

オプションの `replaced` 出力は、上記の非可逆モードで最も役立ちます。

- C++ では、`bool* out_replaced` を渡します。
- Python では、`return_replaced=True` を使用します。
- トランスコードが正確な場合は `false` のままで、置換ポリシーで実際に文字を置換する必要がある場合にのみ `true` に切り替わります。

トランスコードには、ターゲットのエンコードの前にソースのデコード段階もあります。現在のデータセットに、現在の宣言ではデコードできないバイトがすでに含まれている場合、同じポリシー名がそこにも適用されます。

たとえば、現在の宣言が `ISO_IR 192` であるが、保存された生のテキスト値に無効な UTF-8 バイト `b"\xFF"` が含まれている場合、ソース デコード ステージは次のように分岐します。

|比較ポイント | `strict` | `replace_qmark` | `replace_unicode_escape` |
| --- | --- | --- | --- |
|現在保存されているバイトがすでにデコードできない場合 | `set_specific_charset()` はスロー/レイズして停止します。 |トランスコードは続行され、デコードできないソース バイト スパンを `?` に置き換えます。 |トランスコードは続行され、目に見えるバイトエスケープを置き換えます。 |
|生のバイト `b"\xFF"` の置換例 |呼び出しが失敗するため、トランスコードされたテキストは生成されません。 | `?` | `(0xFF)` |
|これがターゲット エンコード フォールバックと異なる理由 | Unicode テキストが復元されなかったため、トランスコードを続行できません。 | Unicode コード ポイントは回復されなかったため、フォールバックは `?` だけです。 | Unicode コード ポイントは回復されなかったため、フォールバックは `(U+XXXX)` ではなく `(0xNN)` バイト エスケープです。 |

## `to_utf8_string()` デコード ポリシー オプション

これらのポリシーは、現在宣言されている文字セットで保存されたバイトを正しくデコードできない場合の動作を制御します。

ポリシー名は 2 つの API に直接マッピングされます。

- C++: `dicom::CharsetDecodeErrorPolicy::strict`、`::replace_fffd`、`::replace_hex_escape`
- Python: `errors="strict"`、`"replace_fffd"`、`"replace_hex_escape"`

たとえば、データセットが `ISO 2022 IR 100` を宣言しているが、格納されている生のバイトがそのデコード パスに対して無効である場合 (`b"\x1b%GA"` など)、`to_utf8_string()` は次のように分岐します。

|比較ポイント | `strict` | `replace_fffd` | `replace_hex_escape` |
| --- | --- | --- | --- |
|保存されたバイトをきれいにデコードできない場合 | `to_utf8_string()` は失敗します。 |デコードは文字を置き換えて成功します。 |可視バイトエスケープを使用してデコードは成功します。 |
| `b"\x1b%GA"` の結果の例 |デコードされたテキストは生成されません。 | `�` | `(0x1B)(0x25)(0x47)(0x41)` |
|戻り値 | C++ の `nullopt`、Python の `None` |デコードされた UTF-8 テキスト |デコードされた UTF-8 テキスト |
| `replaced` 出力 |値が返されないため、`false` |少なくとも 1 つの置換が発生したときの `true` |少なくとも 1 つの置換が発生したときの `true` |

## `from_utf8_view()` エンコード ポリシー オプション

これらのポリシーは、入力 UTF-8 テキストがデータセットで現在宣言されている文字セットで表現できない場合の動作を制御します。 `from_utf8_view()` は戻り値 API であるため、`set_specific_charset()` とは異なり、スロー/レイズではなく、`false` による通常のエンコード失敗を報告します。

ポリシー名は 2 つの API に直接マッピングされます。

- C++: `dicom::CharsetEncodeErrorPolicy::strict`、`::replace_qmark`、`::replace_unicode_escape`
- Python: `errors="strict"`、`"replace_qmark"`、`"replace_unicode_escape"`

たとえば、データセットが `ISO_IR 100` として宣言され、入力テキストが `홍길동` である場合、宣言された文字セットは韓国語の文字を直接表すことはできません。 `from_utf8_view()` は次のように分岐します。

|比較ポイント | `strict` | `replace_qmark` | `replace_unicode_escape` |
| --- | --- | --- | --- |
|入力テキストが宣言された文字セットで表現できない場合 |呼び出しは失敗し、新しいものは何も保存されません。 |呼び出しは成功し、`?` に置き換えられます。 |呼び出しは成功し、表示されている `(U+XXXX)` テキストに置き換えられます。 |
| `홍길동 -> ISO_IR 100` の保存されたテキストの例 |エンコードされたテキストは生成されません。 | `???` | `(U+D64D)(U+AE38)(U+B3D9)` |
|戻り値 | `false` | `true` | `true` |
| `replaced` 出力 |書き込みが成功しなかったため、`false` |少なくとも 1 つの置換が発生したときの `true` |少なくとも 1 つの置換が発生したときの `true` |

## 障害モデル

**C++**

| API |失敗フォーム |典型的な理由 |
| --- | --- | --- |
| `to_utf8_string()` / `to_person_name()` |空の`std::optional` |間違った VR、文字セットのデコードに失敗したか、デコード後に `PN` 構文を解析できませんでした。 |
| `from_utf8_view()` / `from_person_name()` | `false` | VR が間違っています。入力が有効な UTF-8 ではありません。宣言された文字セットは選択したポリシーに基づくテキストを表すことができません。または、DICOM の理由で割り当てが失敗しました。 |
| `set_specific_charset()` | `dicom::diag::DicomException` |無効なターゲット文字セット宣言、サポートされていない宣言の組み合わせ、またはデータセット全体のトランスコード エラー。 |

**パイソン**

| API |失敗フォーム |典型的な理由 |
| --- | --- | --- |
| `to_utf8_string()` / `to_person_name()` | `None` または `(None, replaced)` |間違った VR、文字セットのデコードに失敗したか、デコード後に `PN` 構文を解析できませんでした。無効な `errors=` 値では、`ValueError` が発生します。 |
| `from_utf8_view()` / `from_person_name()` | `False` または `(False, replaced)` |ターゲット VR に互換性がないか、宣言された文字セットが選択したポリシーのテキストを表現できないか、割り当てが失敗しました。 Python 引数の型が間違っていると、`TypeError` が発生します。 |
| `set_specific_charset()` | `TypeError`、`ValueError`、`RuntimeError` | charset 引数の形状が無効であるか、charset 用語が不明であるか、基礎となる C++ トランスコード ステップが失敗します。 |
| `PersonNameGroup.component(index)` | `IndexError` |コンポーネント インデックスは `[0, 4]` の外にあります。 |

## 注意事項

- `to_string_view()` および `to_string_views()` は、VR トリミング ルールの後に生のテキスト バイトを返します。文字セットのデコードは実行しません。アプリケーション向けテキストには `to_utf8_string()` および `to_utf8_strings()` を使用します。
- `to_string_views()` は、ISO 2022 JIS、GBK、GB18030 などの宣言されたマルチバイト文字セットに対して `nullopt` / `None` を返すことがあります。これは、`\` で生のバイトを分割するのはデコード前に安全ではないためです。
- `set_specific_charset()` は、データセット サブツリー内のテキスト VR 値を書き換え、`(0008,0005)` を新しい宣言に同期します。
- `set_specific_charset("ISO_IR 192")` は、後の `from_utf8_view()` または `from_person_name()` の書き込み前にデータセットを UTF-8 宣言状態のままにするため、新しい Unicode コンテンツの適切な通常フローの開始点です。
- `from_utf8_view()` および `from_person_name()` は通常の戻り値 API です。 `false` は、要素の書き込みが成功しなかったことを意味します。 `set_specific_charset()` は検証/トランスコード API であり、代わりに throw/raise によって失敗を報告します。
- `PersonName` には、アルファベット、表意文字、表音文字の 3 つのグループが含まれます。
- `PersonNameGroup` は、DICOM 順序で最大 5 つのコンポーネント (姓、名、ミドルネーム、接頭辞、および接尾辞) を保持します。
- ネストされたシーケンス項目データセットは、その項目が独自のローカル `(0008,0005)` を宣言しない限り、親から有効な文字セットを継承します。
- `PersonName` の解析とシリアル化では、明示的な空のグループと空のコンポーネントが保持されるため、これらの詳細を保持するために `=` および `^` セパレーターを手動で組み立てる必要はありません。
- 新しい Unicode コンテンツの場合、保存されるテキストは ISO 2022 エスケープ状態管理のないプレーン UTF-8 であるため、通常、`ISO_IR 192` が最も単純な宣言になります。
- 保存されたバイトはすでに正しいが、`(0008,0005)` が見つからないか間違っている場合は、宣言修復パスについて [トラブルシューティング](troubleshooting.md) を参照してください。
- 目標が通常のトランスコードまたは正規化フローである場合、生の要素として `(0008,0005)` を変更するよりも `set_specific_charset()` を優先します。

## 関連ドキュメント

- [コアオブジェクト](core_objects.md)
- [Python データセット ガイド](python_dataset_guide.md)
- [C++ データセット ガイド](cpp_dataset_guide.md)
- [エラー処理](error_handling.md)
- [トラブルシューティング](troubleshooting.md)
