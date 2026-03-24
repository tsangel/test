# Error Handling

At the public API level, most failures fall into a small set of categories: invalid input, DICOM/domain failure, and lower-level platform/runtime failure.

## C++

```cpp
try {
  auto file = dicom::read_file("sample.dcm");
  // high-level work
} catch (const dicom::diag::DicomException& ex) {
  // user-facing DICOM failure
} catch (const std::exception& ex) {
  // lower-level or platform-specific failure
}
```

## Python

```python
import dicomsdl as dicom

try:
    df = dicom.read_file("sample.dcm")
    arr = df.to_array(frame=0)
except ValueError as ex:
    # invalid keyword, bad buffer/layout request, malformed argument
    ...
except RuntimeError as ex:
    # parse/decode/encode failure after validation
    ...
```

## Notes

- `ValueError` usually means invalid input or contract misuse.
- In Python, invalid keyword/tag strings on strict lookup paths also raise `ValueError`.
- `decode_into()` and `to_array()` raise `ValueError` for invalid frame, buffer, or layout requests.
- `RuntimeError` usually means dicomsdl reached the real DICOM/codec operation and that operation failed.
- In Python, parse, decode, and encode failures after validation usually surface as `RuntimeError`.
- `dicom::diag::DicomException` is not exposed as a dedicated Python exception class; Python code usually sees that failure as `RuntimeError`.
- In C++, high-level failures usually surface as `dicom::diag::DicomException`.

## Related docs

- [Troubleshooting](troubleshooting.md)
- [Error Model](../reference/error_model.md)
- [logging](../developer/logging.md)
