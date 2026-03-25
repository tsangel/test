# 核心对象

本页介绍你在 DicomSDL 中最常遇到的对象和辅助类型：`DicomFile`、`DataSet`、`DataElement`、`Sequence`、`PixelSequence`、`Uid`、`PersonName`。

## DicomFile

`DicomFile` 是 DicomSDL 的 file/session wrapper。它拥有 root dataset，并同时提供高层的 read、write、decode 和 transcode 操作。当你关心 file/session 状态、decode 或 serialization 时，应从这里开始。

相关 DICOM 标准章节：

- `DicomFile` 本身不是 DICOM 标准中一个有明确名称的对象，而是 DicomSDL 的实现对象。
- 它最接近的标准对应是 [DICOM PS3.10 Chapter 7, DICOM File Format](https://dicom.nema.org/medical/dicom/current/output/chtml/part10/chapter_7.html)，尤其是处理 File Meta Information 的 Section 7.1。
- 其中包含的 root dataset 遵循 [DICOM PS3.5 Chapter 7, The Data Set](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/chapter_7.html)。

## DataSet

`DataSet` 是构成一个 DICOM 对象的完整结构化 field 集合。DicomSDL 的 `DataSet` 类实现了这个 DICOM Data Element 容器。在实践中，它是你读取或修改 metadata field 时最常使用的对象，因此大多数 metadata access 和 update 流程都从这里开始。

在 Python 中，普通顶层 metadata 读取通常优先使用 `df.Rows` 或 `ds.PatientName`。当键是动态的或带有嵌套路径时，请使用 `get_value("Seq.0.Tag")`；当你需要的不只是类型化值，而是 `DataElement` 元数据时，请使用 `ds["Rows"]`。

相关 DICOM 标准章节：

- 主要对应 [DICOM PS3.5 Chapter 7, The Data Set](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/chapter_7.html)。

## DataElement

`DataElement` 是 `DataSet` 中的一个 field，例如 `PatientName` 或 `Rows`。DicomSDL 的 `DataElement` 类实现了这个单独的 DICOM field，以及它的 tag、VR、value、length、offset 和相关 metadata。当你不仅需要值本身，还需要该 element 的结构细节时，应从这里开始。

每个 `DataElement` 都带有两个辅助概念：

- `Tag`: field 的数值标识，例如 Patient Name 的 `(0010,0010)`。
- `VR`: Value Representation，例如 `PN`、`SQ`、`US`、`OB`，它告诉你这个 field 如何编码和解释。

相关 DICOM 标准章节：

- 主要对应 [DICOM PS3.5 Section 7.1, Data Elements](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/chapter_7.html)。
- field 与 layout 规则在 Section 7.1.1 到 7.1.3 中说明。
- `VR` 规则主要对应 [DICOM PS3.5 Section 6.2, Value Representation](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/sect_6.2.html)。

## Sequence

`Sequence` 是一种 DICOM field，它的值是一个 nested item 列表，而每个 item 本身又是另一个 `DataSet`。DicomSDL 的 `Sequence` 类实现了这个 `SQ` 概念。当你要遍历或修改 `Seq.0.Tag` 这样的 nested DICOM 结构时，应从这里开始。

相关 DICOM 标准章节：

- 主要对应 [DICOM PS3.5 Section 7.5, Nesting of Data Sets](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/sect_7.5.html)。
- item 编码规则在 Section 7.5.1 中说明。

## PixelSequence

`PixelSequence` 是在 `PixelData` 以 encapsulated 或 compressed 形式存储时使用的容器。它与 `Sequence` 不同，不是通用的 nested DICOM field，而是 compressed pixel payload 背后的 frame / fragment 容器。DicomSDL 的 `PixelSequence` 类实现了这个专门的 pixel-storage 对象。通常你会在 Python 中通过 `elem.pixel_sequence`、在 C++ 中通过 `as_pixel_sequence()` 到达它；当 compressed `PixelData` 需要 encoded frame bytes 或 fragment 级访问时，应从这里开始。

相关 DICOM 标准章节：

- encapsulated pixel storage 定义在 [DICOM PS3.5 Section 8.2, Native or Encapsulated Format Encoding](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/sect_8.2.html) 和 [Section A.4, Transfer Syntaxes for Encapsulation of Encoded Pixel Data](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/sect_A.4.html)。
- 周边 pixel attribute 仍然遵循 [DICOM PS3.3 Section C.7.6.3, Image Pixel Module](https://dicom.nema.org/medical/dicom/current/output/chtml/part03/sect_C.7.6.3.html)。

## Supporting Types

`Uid` 和 `PersonName` 是辅助 value type，而不是顶层容器，但在实际代码中会经常出现。

- `Uid`: 对 SOP Class UID、SOP Instance UID、Transfer Syntax UID 等 DICOM unique identifier 字符串的 wrapper
- `PersonName`: `PN` 值的结构化表示。在 Python 中，`PN` field 的 `.value` 可能显示为 `PersonName(...)` 对象，而不是普通字符串。

相关 DICOM 标准章节：

- `PersonName` 规则主要对应 [DICOM PS3.5 Section 6.2, Value Representation](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/sect_6.2.html)，尤其是处理 `PN` 的 Section 6.2.1。
- `Uid` 规则主要对应 [DICOM PS3.5 Chapter 9, Unique Identifiers (UIDs)](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/chapter_9.html)。

## 相关文档

- [Sequence and Paths](sequence_and_paths.md)
- [DataElement Reference](../reference/dataelement_reference.md)
- [Sequence Reference](../reference/sequence_reference.md)
- [Pixel Decode](pixel_decode.md)
- [Pixel Reference](../reference/pixel_reference.md)
- [Charset and Person Name](charset_and_person_name.md)
- [Charset Reference](../reference/charset_reference.md)
- [C++ DataSet Guide](cpp_dataset_guide.md)
- [Python DataSet Guide](python_dataset_guide.md)
