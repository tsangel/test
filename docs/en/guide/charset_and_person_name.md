# Charset and Person Name

Text VRs and `PN` values depend on the declared Specific Character Set. Use the charset-aware helpers when correct decoding or re-encoding matters.

## Relevant DICOM standard sections

- The `Specific Character Set` attribute itself belongs to the SOP Common Module in [DICOM PS3.3 Section C.12, General Modules](https://dicom.nema.org/medical/dicom/current/output/chtml/part03/sect_C.12.html).
- Character repertoire selection, replacement, and ISO/IEC 2022 code extension behavior are defined in [DICOM PS3.5 Chapter 6, Value Encoding](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/chapter_6.html), especially Section 6.1 and Sections 6.1.2.4 through 6.1.2.5.
- `PN` rules are defined in [DICOM PS3.5 Section 6.2, Value Representation](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/sect_6.2.html), especially Section 6.2.1, Person Name (PN) Value Representation.
- Language-specific examples for Japanese, Korean, Unicode UTF-8, GB18030, and GBK live in the informative [DICOM PS3.5 Annex H](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/chapter_H.html), [Annex I](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/chapter_I.html), and [Annex J](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/chapter_J.html).

## C++

```cpp
#include <dicom.h>
#include <iostream>
using namespace dicom::literals;

auto file = dicom::read_file("sample.dcm");
auto& elem = file->dataset()["PatientName"_tag];
auto text = elem.to_utf8_string();
auto name = elem.to_person_name();
```

## Python

```python
import dicomsdl as dicom

df = dicom.read_file("sample.dcm")
elem = df.dataset["PatientName"]
text = elem.to_utf8_string()
name = elem.to_person_name()
df.set_declared_specific_charset("ISO_IR 192")
```

## Python helpers

### Charset-aware reads

- `to_utf8_string()`
- `to_utf8_strings()`
- `to_person_name()`
- `to_person_names()`

### Charset-aware writes

- `from_utf8_view()`
- `from_utf8_views()`
- `from_person_name()`
- `from_person_names()`

`set_declared_specific_charset()` and `set_specific_charset()` are preferred over directly mutating `(0008,0005) Specific Character Set` as a raw element.

### PersonNameGroup

Represents one `PN` component group with up to 5 components.

Properties:

- `components`
- `family_name`
- `given_name`
- `middle_name`
- `name_prefix`
- `name_suffix`

Methods:

- `component(index)`
- `empty()`
- `to_dicom_string()`

### PersonName

Represents a parsed `PN` value with up to 3 component groups:

- `alphabetic`
- `ideographic`
- `phonetic`

Methods:

- `empty()`
- `to_dicom_string()`

Example:

```python
pn = dicom.PersonName(
    alphabetic=("Yamada", "Tarou"),
    ideographic=("山田", "太郎"),
    phonetic=("やまだ", "たろう"),
)

assert pn.to_dicom_string() == "Yamada^Tarou=山田^太郎=やまだ^たろう"
```

## Notes

- Prefer `set_declared_specific_charset()` and `set_specific_charset()` over mutating `(0008,0005)` as a raw element.
- Use `to_person_name()` / `to_person_names()` when you need parsed `PN` components.
- Charset conversion can fail or replace characters depending on the selected encode error policy.

## Related docs

- [Python DataSet Guide](python_dataset_guide.md)
- [Error Handling](error_handling.md)
