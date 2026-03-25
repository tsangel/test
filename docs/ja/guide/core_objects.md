# コアオブジェクト

このページでは、DicomSDL でよく使う主要なオブジェクトと補助型を紹介します: `DicomFile`、`DataSet`、`DataElement`、`Sequence`、`PixelSequence`、`Uid`、`PersonName`。

## DicomFile

`DicomFile` は DicomSDL におけるファイル/セッション単位のラッパーです。ルート `DataSet` を保持し、高水準の read / write / decode / transcode 操作もまとめて提供します。ファイル/セッションの状態管理、デコード、シリアライズが重要な場合はここから始めます。

関連する DICOM 標準セクション:

- `DicomFile` 自体は、DICOM 標準で名前の付いたオブジェクトというより DicomSDL の実装オブジェクトです。
- 最も近い標準上の対応は [DICOM PS3.10 Chapter 7, DICOM File Format](https://dicom.nema.org/medical/dicom/current/output/chtml/part10/chapter_7.html)、特に File Meta Information を扱う Section 7.1 です。
- 内包する root dataset は [DICOM PS3.5 Chapter 7, The Data Set](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/chapter_7.html) に従います。

## DataSet

`DataSet` は、1 つの DICOM オブジェクトを構成する構造化されたフィールドの集合です。DicomSDL の `DataSet` クラスは、この DICOM Data Element コンテナを実装します。実際にはメタデータのフィールドを読んだり更新したりするときに最もよく使うオブジェクトなので、多くのメタデータ参照や更新はここから始まります。

関連する DICOM 標準セクション:

- 主に [DICOM PS3.5 Chapter 7, The Data Set](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/chapter_7.html) に対応します。

## DataElement

`DataElement` は `DataSet` の中の 1 つのフィールドです。たとえば `PatientName` や `Rows` のような項目です。DicomSDL の `DataElement` クラスは、この単一の DICOM フィールドと、その tag、VR、value、length、offset、関連メタデータを表現します。値だけでなく、その要素の構造情報も必要なときはここから始めます。

各 `DataElement` には 2 つの補助概念が付いています。

- `Tag`: フィールドの数値識別子です。たとえば Patient Name は `(0010,0010)` です。
- `VR`: `PN`、`SQ`、`US`、`OB` などの Value Representation です。この値はフィールドがどのようにエンコードされ、解釈されるかを示します。

関連する DICOM 標準セクション:

- 主に [DICOM PS3.5 Section 7.1, Data Elements](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/chapter_7.html) に対応します。
- フィールドとレイアウトの規則は Section 7.1.1 から 7.1.3 に説明されています。
- `VR` の規則は主に [DICOM PS3.5 Section 6.2, Value Representation](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/sect_6.2.html) に対応します。

## Sequence

`Sequence` は、その値がネストされた item のリストになっている DICOM フィールドで、各 item はそれ自体が別の `DataSet` です。DicomSDL の `Sequence` クラスは、この `SQ` の概念を実装します。`Seq.0.Tag` のようなネストした DICOM 構造をたどったり変更したりするときはここから始めます。

関連する DICOM 標準セクション:

- 主に [DICOM PS3.5 Section 7.5, Nesting of Data Sets](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/sect_7.5.html) に対応します。
- item のエンコーディング規則は Section 7.5.1 に記載されています。

## PixelSequence

`PixelSequence` は、`PixelData` が encapsulated または compressed 形式で保存されているときに使われるコンテナです。`Sequence` と違って、一般的なネスト DICOM フィールドではありません。compressed pixel payload の背後にある frame / fragment コンテナです。DicomSDL の `PixelSequence` クラスは、この特殊な pixel-storage オブジェクトを実装します。通常は Python では `elem.pixel_sequence`、C++ では `as_pixel_sequence()` から到達し、compressed `PixelData` で encoded frame bytes や fragment レベルのアクセスが必要なときに使います。

関連する DICOM 標準セクション:

- encapsulated pixel storage は [DICOM PS3.5 Section 8.2, Native or Encapsulated Format Encoding](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/sect_8.2.html) と [Section A.4, Transfer Syntaxes for Encapsulation of Encoded Pixel Data](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/sect_A.4.html) に定義されています。
- 周辺の pixel attribute は引き続き [DICOM PS3.3 Section C.7.6.3, Image Pixel Module](https://dicom.nema.org/medical/dicom/current/output/chtml/part03/sect_C.7.6.3.html) に従います。

## Supporting Types

`Uid` と `PersonName` は最上位コンテナではなく補助的な値型ですが、実際のコードでは頻繁に登場します。

- `Uid`: SOP Class UID、SOP Instance UID、Transfer Syntax UID などの DICOM unique identifier 文字列を包むラッパー
- `PersonName`: `PN` 値の構造化表現。Python では `PN` field の `.value` が単なる文字列ではなく `PersonName(...)` オブジェクトとして見えることがあります。

関連する DICOM 標準セクション:

- `PersonName` の規則は主に [DICOM PS3.5 Section 6.2, Value Representation](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/sect_6.2.html)、特に `PN` を扱う Section 6.2.1 に対応します。
- `Uid` の規則は主に [DICOM PS3.5 Chapter 9, Unique Identifiers (UIDs)](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/chapter_9.html) に対応します。

## 関連ドキュメント

- [Sequence and Paths](sequence_and_paths.md)
- [DataElement Reference](../reference/dataelement_reference.md)
- [Sequence Reference](../reference/sequence_reference.md)
- [Pixel Decode](pixel_decode.md)
- [Pixel Reference](../reference/pixel_reference.md)
- [Charset and Person Name](charset_and_person_name.md)
- [Charset Reference](../reference/charset_reference.md)
- [C++ DataSet Guide](cpp_dataset_guide.md)
- [Python DataSet Guide](python_dataset_guide.md)
