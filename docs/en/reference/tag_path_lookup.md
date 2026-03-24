# Tag-path lookup semantics

`DataSet::get_dataelement(std::string_view tag_path)` and the string overload of `operator[]` are the low-level path parsers for nested sequence traversal. For regular tag reads, prefer `dataset[tag].to_xxx().value_or(default)` or `dataset["Keyword"].to_xxx().value_or(default)` style access.

## Accepted forms
- Hex tag, with or without parens/comma: `00100010`, `(0010,0010)`
- Keyword: `PatientName`
- Private creator: `gggg,xxee,CREATOR` where `gggg` is odd, `xx` is the reserved block placeholder, and `CREATOR` matches the value in `(gggg,00xx)`; e.g. `0009,xx1e,GEMS_GENIE_1`
- Nested sequences: `00082112.0.00081190` (sequence tag • index • child tag ...)
- Nested with keywords: `RadiopharmaceuticalInformationSequence.0.RadionuclideTotalDose`

## Resolution rules
1. Tokens are split on `.`; each token except the last must resolve to a sequence element.
2. Tag tokens accept parentheses/commas and are trimmed; runtime keyword text is resolved via the
   normal runtime dictionary lookup path (`keyword_to_entry_runtime(...)`), which currently uses the
   runtime keyword cache. Compile-time keyword literals still use the constexpr CHD tables.
3. Private-creator tokens are parsed via `parse_private_creator_tag`; on failure, lookup stops and returns a falsey `DataElement` (`VR::None`).
4. Sequence indices are parsed with `std::from_chars`; type/VR mismatches raise `diag::error_and_throw`.
5. If any required element or nested dataset is missing, a falsey `DataElement` (`VR::None`) is returned.

## Notes
- Preferred user-facing access pattern for plain tags:
  `long rows = dataset["Rows"_tag].to_long().value_or(0);`
- Use `if (auto& e = dataset[tag]; e)` only when element presence itself matters.
- No implicit loading: after a partial read, callers must make the needed plain tag such as `Rows` available first via `ensure_loaded(tag)`.
- The function returns `DataElement&`. If you need to distinguish missing from present, use `if (elem)` or `elem.is_missing()`. If you just need a fallback value, `get_dataelement(...).to_xxx().value_or(default)` is fine.
- For keyword paths without `.`, the parser now avoids the heavier dotted-path loop and falls
  through the lighter direct token resolution path before lookup.
- Errors (malformed path, non-sequence traversal, bad index) throw and include the offending tag string.

## Examples
- Direct keyword: `dataset["PatientName"]` or `get_dataelement("PatientName")`
- Private creator element inside a private block: `get_dataelement("0009,xx1e,GEMS_GENIE_1")`
- Sequence traversal: `dataset["00082112.0.00081190"]` or `get_dataelement("00082112.0.00081190")`
- Keyword-based traversal: `dataset["RadiopharmaceuticalInformationSequence.0.RadionuclideTotalDose"]`
