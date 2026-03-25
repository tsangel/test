# Core Objects

This page introduces the objects and supporting types you meet most often in DicomSDL: `DicomFile`, `DataSet`, `DataElement`, `Sequence`, `PixelSequence`, `Uid`, and `PersonName`.

## DicomFile

`DicomFile` is DicomSDL's file/session wrapper. It owns the root dataset together with high-level read, write, decode, and transcode operations. Start here when you care about file/session state, decode, or serialization.

Relevant DICOM sections:

- `DicomFile` itself is a DicomSDL implementation object rather than a named DICOM standard object.
- Its closest standard mapping is the file-level encapsulation defined in [DICOM PS3.10 Chapter 7, DICOM File Format](https://dicom.nema.org/medical/dicom/current/output/chtml/part10/chapter_7.html), especially Section 7.1 on File Meta Information.
- The enclosed root dataset follows [DICOM PS3.5 Chapter 7, The Data Set](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/chapter_7.html).

## DataSet

`DataSet` is the whole structured collection of fields that make up one DICOM object. In DicomSDL, the `DataSet` class implements that container of DICOM Data Elements. In practice this is the object you use most often when reading or mutating metadata fields, so start here for most metadata access and update flows.

In Python, the main top-level metadata read path is usually `df.Rows` or
`ds.PatientName`. Reach for `get_value("Seq.0.Tag")` when the key is dynamic
or nested, and use `ds["Rows"]` when you need `DataElement` metadata instead
of just the typed value.

Relevant DICOM sections:

- Mainly maps to [DICOM PS3.5 Chapter 7, The Data Set](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/chapter_7.html).

## DataElement

`DataElement` is one field inside a `DataSet`, such as `PatientName` or `Rows`. In DicomSDL, the `DataElement` class implements that single DICOM field together with its tag, VR, value, length, offset, and related metadata. Start here when you need both the value and the structural details of that element.

Two supporting concepts are attached to every `DataElement`:

- `Tag`: the numeric identity of a field, such as `(0010,0010)` for Patient Name.
- `VR`: the Value Representation, such as `PN`, `SQ`, `US`, or `OB`, which tells you how the field is encoded and interpreted.

Relevant DICOM sections:

- Mainly maps to [DICOM PS3.5 Section 7.1, Data Elements](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/chapter_7.html).
- Field and layout rules are described in Section 7.1.1 through 7.1.3.
- `VR` rules mainly map to [DICOM PS3.5 Section 6.2, Value Representation](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/sect_6.2.html).

## Sequence

`Sequence` is a DICOM field whose value is a list of nested items, where each item is itself another `DataSet`. In DicomSDL, the `Sequence` class implements that `SQ` concept. Start here when you are traversing or modifying nested DICOM structures such as `Seq.0.Tag`.

Relevant DICOM sections:

- Mainly maps to [DICOM PS3.5 Section 7.5, Nesting of Data Sets](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/sect_7.5.html).
- Item encoding rules are described in Section 7.5.1.

## PixelSequence

`PixelSequence` is the container used when `PixelData` is stored in encapsulated or compressed form. Unlike `Sequence`, it is not a general nested DICOM field. It is the frame/fragment container behind compressed pixel payloads. In DicomSDL, the `PixelSequence` class implements that specialized pixel-storage object. You usually reach it from `PixelData` as `elem.pixel_sequence` in Python or `as_pixel_sequence()` in C++, and you start here when compressed `PixelData` requires encoded frame bytes or fragment-level access.

Relevant DICOM sections:

- Encapsulated pixel storage is defined in [DICOM PS3.5 Section 8.2, Native or Encapsulated Format Encoding](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/sect_8.2.html) and [Section A.4, Transfer Syntaxes for Encapsulation of Encoded Pixel Data](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/sect_A.4.html).
- The surrounding pixel attributes still follow [DICOM PS3.3 Section C.7.6.3, Image Pixel Module](https://dicom.nema.org/medical/dicom/current/output/chtml/part03/sect_C.7.6.3.html).

## Supporting Types

`Uid` and `PersonName` are supporting value types rather than top-level containers, but they show up constantly in real code.

- `Uid`: a wrapper for DICOM unique identifier strings such as SOP Class UID, SOP Instance UID, and Transfer Syntax UID.
- `PersonName`: a structured representation of a `PN` value. In Python, `.value` for a `PN` field may appear as a `PersonName(...)` object rather than a plain string.

Relevant DICOM sections:

- `PersonName` rules mainly map to [DICOM PS3.5 Section 6.2, Value Representation](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/sect_6.2.html), especially Section 6.2.1 for `PN`.
- `Uid` rules mainly map to [DICOM PS3.5 Chapter 9, Unique Identifiers (UIDs)](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/chapter_9.html).

## Related docs

- [Sequence and Paths](sequence_and_paths.md)
- [DataElement Reference](../reference/dataelement_reference.md)
- [Sequence Reference](../reference/sequence_reference.md)
- [Pixel Decode](pixel_decode.md)
- [Pixel Reference](../reference/pixel_reference.md)
- [Charset and Person Name](charset_and_person_name.md)
- [Charset Reference](../reference/charset_reference.md)
- [C++ DataSet Guide](cpp_dataset_guide.md)
- [Python DataSet Guide](python_dataset_guide.md)
