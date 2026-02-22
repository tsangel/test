# Tag-path lookup semantics

`DataSet::get_dataelement(std::string_view tag_path)` parses flexible tag-path strings to navigate nested sequences. Behavior corresponds to `src/dataset.cpp` lines 350–361.

## Accepted forms
- Hex tag, with or without parens/comma: `00100010`, `(0010,0010)`
- Keyword: `PatientName`
- Private creator: `gggg,xxee,CREATOR` where `gggg` is odd, `xx` is the reserved block placeholder, and `CREATOR` matches the value in `(gggg,00xx)`; e.g. `0009,xx1e,GEMS_GENIE_1`
- Nested sequences: `00082112.0.00081190` (sequence tag • index • child tag ...)
- Nested with keywords: `RadiopharmaceuticalInformationSequence.0.RadionuclideTotalDose`

## Resolution rules
1. Tokens are split on `.`; each token except the last must resolve to a sequence element.
2. Tag tokens accept parentheses/commas and are trimmed; keywords fall back to the CHD lookup.
3. Private-creator tokens are parsed via `parse_private_creator_tag`; on failure, lookup stops and returns a falsey `DataElement` (`VR::None`).
4. Sequence indices are parsed with `std::stoul`; type/VR mismatches raise `diag::error_and_throw`.
5. If any required element or nested dataset is missing, a falsey `DataElement` (`VR::None`) is returned.

## Notes
- No implicit loading: callers must ensure the needed elements are present via `ensure_loaded(tag)` or an earlier `read_attached_stream()`.
- The function returns a non-null `DataElement*`; callers should check `elem->is_present()` before dereferencing.
- Errors (malformed path, non-sequence traversal, bad index) throw and include the offending tag string.

## Examples
- Direct keyword: `get_dataelement("PatientName")`
- Private creator element inside a private block: `get_dataelement("0009,xx1e,GEMS_GENIE_1")`
- Sequence traversal: `get_dataelement("00082112.0.00081190")`
- Keyword-based traversal: `get_dataelement("RadiopharmaceuticalInformationSequence.0.RadionuclideTotalDose")`
