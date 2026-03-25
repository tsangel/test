# Charset and Person Name

Use `to_utf8_string()` / `to_person_name()` when a text VR or `PN` value depends on `SpecificCharacterSet` and you want decoded UTF-8 or structured name components. Use `to_string_view()` only when you deliberately want the stored bytes after normal VR trimming without charset decode. Use `from_utf8_view()` / `from_person_name()` when you want charset-aware writes. Use `set_specific_charset()` when you want to normalize or transcode the current dataset subtree to a new charset. If you need to repair a missing or wrong declaration for already-stored bytes, see [Troubleshooting](troubleshooting.md).

Scope note: most of the read/write helpers below are `DataElement` methods. The charset mutation APIs that rewrite or re-declare `(0008,0005)` live on `DataSet` / `DicomFile`.

## Key Charset and PN APIs

**C++**

`DataElement` methods

- `to_string_view()` / `to_string_views()`
  - Read trimmed raw stored bytes without charset decode.
- `to_utf8_string()` / `to_utf8_strings()`
  - Read text VRs as owned UTF-8 after charset decode.
- `to_person_name()` / `to_person_names()`
  - Parse `PN` values into alphabetic, ideographic, and phonetic groups.
- `from_utf8_view()` / `from_utf8_views()`
  - Encode UTF-8 text into the charset currently declared on the owning dataset.
- `from_person_name()` / `from_person_names()`
  - Serialize structured `PersonName` values into a `PN` element.

`DataSet` / `DicomFile` methods

- `set_specific_charset()`
  - Transcode existing text bytes to a new charset and update `(0008,0005)` consistently.

`Helper types`

- `PersonName` / `PersonNameGroup`
  - Helper types for building or inspecting `PN` values without manual `^` and `=` string handling.

**Python**

`DataElement` methods

- `to_string_view()` / `to_string_views()`
  - Read trimmed raw stored text without charset decode.
- `to_utf8_string()` / `to_utf8_strings()`
  - Read text VRs as decoded UTF-8 strings. With `return_replaced=True`, you can also see whether decode fallback replaced bytes.
- `to_person_name()` / `to_person_names()`
  - Parse `PN` values into `PersonName` objects with alphabetic, ideographic, and phonetic groups.
- `from_utf8_view()` / `from_utf8_views()`
  - Encode Python `str` data into the dataset's declared charset. With `return_replaced=True`, you can inspect replacement behavior.
- `from_person_name()` / `from_person_names()`
  - Serialize `PersonName` objects into a `PN` element.

`DataSet` / `DicomFile` methods

- `set_specific_charset()`
  - Transcode existing text values to a new charset and rewrite `(0008,0005)` consistently.

`Helper types`

- `PersonName(...)` / `PersonNameGroup(...)`
  - Construct structured `PN` values directly from Python strings or tuples.

## Relevant DICOM standard sections

- The `Specific Character Set` attribute itself belongs to the SOP Common Module in [DICOM PS3.3 Section C.12, General Modules](https://dicom.nema.org/medical/dicom/current/output/chtml/part03/sect_C.12.html).
- Character repertoire selection, replacement, and ISO/IEC 2022 code extension behavior are defined in [DICOM PS3.5 Chapter 6, Value Encoding](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/chapter_6.html), especially Section 6.1 and Sections 6.1.2.4 through 6.1.2.5.
- `PN` rules are defined in [DICOM PS3.5 Section 6.2, Value Representation](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/sect_6.2.html), especially Section 6.2.1, Person Name (PN) Value Representation.
- Language-specific examples for Japanese, Korean, Unicode UTF-8, GB18030, and GBK live in the informative [DICOM PS3.5 Annex H](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/chapter_H.html), [Annex I](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/chapter_I.html), and [Annex J](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/chapter_J.html).

## C++

### C++: Compare raw stored text with decoded UTF-8

```cpp
#include <dicom.h>
#include <iostream>

using namespace dicom::literals;

auto file = dicom::read_file("patient_names.dcm");
const auto& patient_name = file->dataset()["PatientName"_tag];

// to_string_view() gives the stored text bytes after normal VR trimming only.
// No SpecificCharacterSet decode happens here.
if (auto raw = patient_name.to_string_view()) {
    std::cout << "raw: " << *raw << '\n';
}

// to_utf8_string() decodes according to the dataset's declared SpecificCharacterSet.
if (auto utf8 = patient_name.to_utf8_string()) {
    std::cout << "utf8: " << *utf8 << '\n';
}

// to_person_name() goes one step further and parses the PN groups and components.
if (auto parsed = patient_name.to_person_name()) {
    if (parsed->alphabetic) {
        std::cout << parsed->alphabetic->family_name() << '\n';
        std::cout << parsed->alphabetic->given_name() << '\n';
    }
}
```

Example output when the first `PatientName` value is `Hong^Gildong=洪^吉洞=홍^길동`:

```text
raw: Hong^Gildong=洪^吉洞=홍^길동
utf8: Hong^Gildong=洪^吉洞=홍^길동
Hong
Gildong
```

### C++: Build and store a structured PersonName

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
    // from_person_name() also reports normal assignment failure with false.
}

if (auto parsed = patient_name.to_person_name()) {
    std::cout << parsed->alphabetic->family_name() << '\n';
    std::cout << parsed->ideographic->family_name() << '\n';
    std::cout << parsed->phonetic->family_name() << '\n';
}
```

Expected output:

```text
Hong
洪
홍
```

### Transcode existing text values to a new charset

```cpp
#include <dicom.h>
#include <iostream>

using namespace dicom::literals;

auto file = dicom::read_file("utf8_names.dcm");
bool replaced = false;

// set_specific_charset() walks the dataset subtree, rewrites text VR values,
// and updates (0008,0005) to the new declaration. This policy keeps the
// transcode moving while leaving a visible trace for characters that the
// target charset cannot represent directly.
file->set_specific_charset(
    dicom::SpecificCharacterSet::ISO_IR_100,
    dicom::CharsetEncodeErrorPolicy::replace_unicode_escape,
    &replaced);

// The rewritten stored bytes are now plain ASCII text, so to_string_view()
// and to_utf8_string() both expose the same visible `(U+XXXX)` replacement text here.
if (auto raw_name = file->dataset()["PatientName"_tag].to_string_view()) {
    std::cout << *raw_name << '\n';
}
std::cout << std::boolalpha << replaced << '\n';
```

Example output when `utf8_names.dcm` contains `홍길동`:

```text
(U+D64D)(U+AE38)(U+B3D9)
true
```

### C++: Handle declaration and transcode failures explicitly

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

    // set_specific_charset() is different from from_utf8_view():
    // dataset-wide declaration/transcode problems throw instead of returning false.
    file.set_specific_charset(
        dicom::SpecificCharacterSet::ISO_IR_100,
        dicom::CharsetEncodeErrorPolicy::strict);
} catch (const dicom::diag::DicomException& ex) {
    std::cerr << ex.what() << '\n';
}
```

## Python

### Python: Compare raw stored text with decoded UTF-8

```python
import dicomsdl as dicom

df = dicom.read_file("patient_names.dcm")
elem = df.dataset["PatientName"]

# to_string_view() returns the stored text after normal VR trimming only.
# No SpecificCharacterSet decode happens here.
raw = elem.to_string_view()

# to_utf8_string() returns a decoded Python str or None.
text, replaced = elem.to_utf8_string(return_replaced=True)

# to_person_name() returns a structured PersonName or None.
name = elem.to_person_name()
if name is not None and name.alphabetic is not None:
    print(name.alphabetic.family_name)
    print(name.alphabetic.given_name)
```

Example output when the first `PatientName` value is `Hong^Gildong=洪^吉洞=홍^길동`:

```text
Hong
Gildong
```

### Python: Build and store a structured PersonName

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

# The same PersonName object can also be used with dataset attribute assignment convenience access.
df.PatientName = pn

value = df.PatientName
print(value.alphabetic.family_name)
print(value.ideographic.family_name)
print(value.phonetic.family_name)
```

Expected output:

```text
Hong
洪
홍
```

### Transcode existing text values and inspect replacement

```python
import dicomsdl as dicom

df = dicom.read_file("utf8_names.dcm")

# A visible fallback is often easier to work with than strict failure in a
# production cleanup pass because the transcode finishes and the replacement is
# still obvious in the resulting text.
replaced = df.set_specific_charset(
    "ISO_IR 100",
    errors="replace_unicode_escape",
    return_replaced=True,
)
print(df.get_dataelement("PatientName").to_string_view())
print(replaced)
```

Expected output when `utf8_names.dcm` contains `홍길동`:

```text
(U+D64D)(U+AE38)(U+B3D9)
True
```

### Python: Handle declaration and transcode failures explicitly

```python
import dicomsdl as dicom

df = dicom.DicomFile()

try:
    df.set_specific_charset("ISO_IR 192")
    patient_name = df.dataset.add_dataelement(dicom.Tag("PatientName"), dicom.VR.PN)
    ok = patient_name.from_utf8_view("홍길동")
    print(ok)

    # set_specific_charset() raises if the requested transcode cannot be done
    # under the selected error policy.
    df.set_specific_charset("ISO_IR 100", errors="strict")
except (TypeError, ValueError) as exc:
    # Invalid charset argument shape or invalid policy text.
    print(exc)
except RuntimeError as exc:
    # The underlying declaration or transcode step failed.
    print(exc)
```

## `set_specific_charset()` policy options

The first argument chooses the target charset. The second argument chooses how to handle characters that the target charset cannot represent. The optional third output reports whether any replacement actually happened, which is mainly useful for the lossy modes.

When every text value is representable in the target charset, all policies produce the same transcoded dataset and report `replaced == false`. The difference matters only when some existing text cannot be represented in the requested target charset.

Policy names map directly across the two APIs:

- C++: `dicom::CharsetEncodeErrorPolicy::strict`, `::replace_qmark`, `::replace_unicode_escape`
- Python: `errors="strict"`, `"replace_qmark"`, `"replace_unicode_escape"`

For example, if the source text is `홍길동` and the target charset is `ISO_IR 100`, that target charset cannot represent the Korean characters directly. The policies then diverge like this:

| Comparison point | `strict` | `replace_qmark` | `replace_unicode_escape` |
| --- | --- | --- | --- |
| If some text is not representable | `set_specific_charset()` throws / raises and stops. | The transcode succeeds and substitutes `?`. | The transcode succeeds and substitutes visible `(U+XXXX)` text. |
| Example result for `홍길동 -> ISO_IR 100` | No transcoded text is produced because the call fails. | `???` | `(U+D64D)(U+AE38)(U+B3D9)` |
| Dataset commit | No change. | Charset is updated and text VRs are rewritten with `?`. | Charset is updated and text VRs are rewritten with `(U+XXXX)` replacement text. |
| `replaced` output | Not applicable because the call fails. | `true` when at least one substitution happens. | `true` when at least one substitution happens. |

The optional `replaced` output is most useful with the lossy modes above:

- In C++, pass `bool* out_replaced`.
- In Python, use `return_replaced=True`.
- It stays `false` when the transcode was exact and flips to `true` only when a replacement policy actually had to substitute characters.

Transcode also has a source-decode stage before target encode. If the current dataset already contains bytes that cannot be decoded under the current declaration, the same policy names apply there too.

For example, if the current declaration is `ISO_IR 192` but a stored raw text value contains the invalid UTF-8 byte `b"\xFF"`, the source-decode stage diverges like this:

| Comparison point | `strict` | `replace_qmark` | `replace_unicode_escape` |
| --- | --- | --- | --- |
| If current stored bytes are already undecodable | `set_specific_charset()` throws / raises and stops. | The transcode continues and substitutes `?` for the undecodable source byte span. | The transcode continues and substitutes visible byte escapes. |
| Example replacement for raw byte `b"\xFF"` | No transcoded text is produced because the call fails. | `?` | `(0xFF)` |
| Why this differs from target encode fallback | No Unicode text was recovered, so the transcode cannot continue. | No Unicode code point was recovered, so the fallback is just `?`. | No Unicode code point was recovered, so the fallback is `(0xNN)` byte escape rather than `(U+XXXX)`. |

## `to_utf8_string()` decode policy options

These policies control what happens when the stored bytes cannot be decoded cleanly under the currently declared charset.

Policy names map directly across the two APIs:

- C++: `dicom::CharsetDecodeErrorPolicy::strict`, `::replace_fffd`, `::replace_hex_escape`
- Python: `errors="strict"`, `"replace_fffd"`, `"replace_hex_escape"`

For example, if the dataset declares `ISO 2022 IR 100` but the stored raw bytes are invalid for that decode path, such as `b"\x1b%GA"`, `to_utf8_string()` diverges like this:

| Comparison point | `strict` | `replace_fffd` | `replace_hex_escape` |
| --- | --- | --- | --- |
| If stored bytes cannot be decoded cleanly | `to_utf8_string()` fails. | Decode succeeds with replacement characters. | Decode succeeds with visible byte escapes. |
| Example result for `b"\x1b%GA"` | No decoded text is produced. | `�` | `(0x1B)(0x25)(0x47)(0x41)` |
| Return value | `nullopt` in C++, `None` in Python | Decoded UTF-8 text | Decoded UTF-8 text |
| `replaced` output | `false` because no value is returned | `true` when at least one replacement happened | `true` when at least one replacement happened |

## `from_utf8_view()` encode policy options

These policies control what happens when the input UTF-8 text cannot be represented in the dataset's currently declared charset. `from_utf8_view()` is a return-value API, so unlike `set_specific_charset()` it reports ordinary encode failure with `false` rather than throw/raise.

Policy names map directly across the two APIs:

- C++: `dicom::CharsetEncodeErrorPolicy::strict`, `::replace_qmark`, `::replace_unicode_escape`
- Python: `errors="strict"`, `"replace_qmark"`, `"replace_unicode_escape"`

For example, if the dataset is declared as `ISO_IR 100` and the input text is `홍길동`, the declared charset cannot represent the Korean characters directly. `from_utf8_view()` then diverges like this:

| Comparison point | `strict` | `replace_qmark` | `replace_unicode_escape` |
| --- | --- | --- | --- |
| If input text is not representable in the declared charset | The call fails and stores nothing new. | The call succeeds and substitutes `?`. | The call succeeds and substitutes visible `(U+XXXX)` text. |
| Example stored text for `홍길동 -> ISO_IR 100` | No encoded text is produced. | `???` | `(U+D64D)(U+AE38)(U+B3D9)` |
| Return value | `false` | `true` | `true` |
| `replaced` output | `false` because the write did not succeed | `true` when at least one substitution happened | `true` when at least one substitution happened |

## Failure Model

**C++**

| API | Failure form | Typical reasons |
| --- | --- | --- |
| `to_utf8_string()` / `to_person_name()` | empty `std::optional` | Wrong VR, charset decode failed, or `PN` syntax could not be parsed after decode. |
| `from_utf8_view()` / `from_person_name()` | `false` | Wrong VR, input is not valid UTF-8, the declared charset cannot represent the text under the selected policy, or assignment failed for DICOM reasons. |
| `set_specific_charset()` | `dicom::diag::DicomException` | Invalid target charset declaration, unsupported declaration combination, or dataset-wide transcode failure. |

**Python**

| API | Failure form | Typical reasons |
| --- | --- | --- |
| `to_utf8_string()` / `to_person_name()` | `None` or `(None, replaced)` | Wrong VR, charset decode failed, or `PN` syntax could not be parsed after decode. Invalid `errors=` values raise `ValueError`. |
| `from_utf8_view()` / `from_person_name()` | `False` or `(False, replaced)` | The target VR is incompatible, the declared charset cannot represent the text under the selected policy, or assignment failed. Wrong Python argument types raise `TypeError`. |
| `set_specific_charset()` | `TypeError`, `ValueError`, `RuntimeError` | The charset argument shape is invalid, a charset term is unknown, or the underlying C++ transcode step fails. |
| `PersonNameGroup.component(index)` | `IndexError` | The component index is outside `[0, 4]`. |

## Notes

- `to_string_view()` and `to_string_views()` return raw textual bytes after VR trimming rules. They do not perform charset decode. Use `to_utf8_string()` and `to_utf8_strings()` for application-facing text.
- `to_string_views()` can return `nullopt` / `None` for declared multibyte charsets such as ISO 2022 JIS, GBK, or GB18030 because splitting raw bytes on `\` would be unsafe before decode.
- `set_specific_charset()` rewrites text VR values in the dataset subtree and synchronizes `(0008,0005)` to the new declaration.
- `set_specific_charset("ISO_IR 192")` is a reasonable normal-flow starting point for new Unicode content because it leaves the dataset in a UTF-8 declaration state before later `from_utf8_view()` or `from_person_name()` writes.
- `from_utf8_view()` and `from_person_name()` are normal return-value APIs. `false` means the element write did not succeed. `set_specific_charset()` is a validation/transcode API and reports failure by throw/raise instead.
- `PersonName` carries up to three groups: alphabetic, ideographic, and phonetic.
- `PersonNameGroup` carries up to five components in DICOM order: family name, given name, middle name, prefix, and suffix.
- Nested sequence item datasets inherit the effective charset from their parent unless that item declares its own local `(0008,0005)`.
- `PersonName` parsing and serialization preserve explicit empty groups and empty components, so you do not need to hand-assemble `=` and `^` separators to keep those details.
- For new Unicode content, `ISO_IR 192` is usually the simplest declaration because the stored text is plain UTF-8 without ISO 2022 escape-state management.
- If the stored bytes are already correct but `(0008,0005)` is missing or wrong, see [Troubleshooting](troubleshooting.md) for the declaration-repair path.
- Prefer `set_specific_charset()` over mutating `(0008,0005)` as a raw element when the goal is a normal transcode or normalization flow.

## Related docs

- [Core Objects](core_objects.md)
- [Python DataSet Guide](python_dataset_guide.md)
- [C++ DataSet Guide](cpp_dataset_guide.md)
- [Error Handling](error_handling.md)
- [Troubleshooting](troubleshooting.md)
