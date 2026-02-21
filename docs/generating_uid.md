# Generating UID

This document explains the current UID generation and append flow in `dicomsdl`.

## 1. Scope

- C++:
  - `dicom::uid::try_generate_uid()`
  - `dicom::uid::generate_uid()`
  - `dicom::uid::Generated::try_append(component)`
  - `dicom::uid::Generated::append(component)`
- Python:
  - `dicom.try_generate_uid()`
  - `dicom.generate_uid()`
  - `dicom.try_append_uid(base_uid, component)`
  - `dicom.append_uid(base_uid, component)`

## 2. C++ API

### 2.1 Generate base UID

```cpp
auto uid_opt = dicom::uid::try_generate_uid(); // std::optional<Generated>
auto uid = dicom::uid::generate_uid();         // Generated (throws on failure)
```

- Prefix: `dicom::uid::uid_prefix()`
- `generate_uid()` builds UID as `<uid_prefix>.<random_numeric_suffix>`.
  The suffix is generated from process-level random nonce + monotonic sequence.
- Output: strict-valid UID text (max 64 chars)

### 2.2 Append one component

```cpp
auto study = dicom::uid::generate_uid();
auto series = study.append(23);
auto inst = series.append(34);
```

- `try_append(component)` returns `std::optional<Generated>`
- `append(component)` throws `std::runtime_error` on failure

### 2.3 Existing UID text to `Generated`

```cpp
auto base = dicom::uid::make_generated("1.2.840.10008");
if (base) {
    auto extended = base->append(7);
}
```

## 3. Python API

```python
import dicomsdl as dicom

study = dicom.generate_uid()
series = dicom.append_uid(study, 23)
inst = dicom.append_uid(series, 34)

safe = dicom.try_append_uid("1.2.840.10008", 7)  # Optional[str]
```

- `append_uid(...)` raises on invalid input/failure
- `try_append_uid(...)` returns `None` on failure

## 4. Append behavior

For `append` / `try_append`:

1. Direct path:
   - Try `<base_uid>.<component>` first.
   - If valid and <= 64 chars, return it.

2. Fallback path (when direct append does not fit):
   - Keep first 30 chars of `base_uid`.
   - If last char is not `.`, add `.`.
   - Append one U96 decimal block.
   - Result is revalidated by strict UID validator.

### Important note

Fallback output is intentionally non-deterministic:

- It is still based on `component` and `base_uid`,
- but also mixes process-level random nonce + atomic sequence.

So for the same `(base_uid, component)`, fallback suffix can differ across calls.

## 5. Failure model summary

- `generate_uid()`:
  - throws on failure
- `try_generate_uid()`:
  - returns `None` / `std::nullopt` on failure
- `append_uid()` / `Generated::append()`:
  - throws on failure
- `try_append_uid()` / `Generated::try_append()`:
  - returns `None` / `std::nullopt` on failure

## 6. Practical recommendation

- Application flow:
  - build base with `generate_uid()`
  - derive child UIDs via `append(...)`
- If your input base UID may be untrusted, use:
  - `try_append_uid(...)` in Python
  - `try_append(...)` in C++
