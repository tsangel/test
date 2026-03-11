#pragma once

#include <span>
#include <string>

#include "charset/charset_detail.hpp"
#include "dicom.h"

namespace dicom::charset::detail {

[[nodiscard]] bool validate_declared_charset(
    const ParsedSpecificCharacterSet& parsed, std::string* out_error);
[[nodiscard]] bool omit_charset_tag(const ParsedSpecificCharacterSet& parsed) noexcept;
[[nodiscard]] DecodeReplacementMode decode_mode(CharsetDecodeErrorPolicy errors) noexcept;
[[nodiscard]] DecodeReplacementMode decode_mode_for_encode(
    CharsetEncodeErrorPolicy errors) noexcept;
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_charset_tag(
    SpecificCharacterSet charset, std::string* out_error);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_charset_tag(
    std::span<const SpecificCharacterSet> charsets, std::string* out_error);
[[nodiscard]] std::optional<std::string> sanitize_utf8_for_charset(std::string_view value,
    const ParsedSpecificCharacterSet& target_charset, CharsetEncodeErrorPolicy errors,
    std::string* out_error, bool* out_replaced);

[[nodiscard]] std::optional<std::vector<std::string>> decode_text_values(
    const DataElement& element, const ParsedSpecificCharacterSet& source_charset_plan,
    DecodeReplacementMode decode_mode, std::string* out_error, bool* out_replaced);
[[nodiscard]] std::optional<std::vector<std::string>> decode_text_values(
    const DataElement& element, DecodeReplacementMode decode_mode, std::string* out_error,
    bool* out_replaced);

[[nodiscard]] bool should_reuse_raw_values(const ParsedSpecificCharacterSet& source_charset,
    const ParsedSpecificCharacterSet& target_charset) noexcept;
[[nodiscard]] std::optional<std::vector<std::uint8_t>> transcode_element_text(
    const DataElement& element, const ParsedSpecificCharacterSet& source_charset,
    const ParsedSpecificCharacterSet& target_charset, CharsetEncodeErrorPolicy errors,
    std::string* out_error, bool* out_replaced);

[[nodiscard]] bool validate_charset_values(DataSet& dataset,
    const ParsedSpecificCharacterSet& target_charset, std::string* out_error,
    bool* out_replaced);
[[nodiscard]] bool rewrite_charset_values(DataSet& dataset,
    const ParsedSpecificCharacterSet& target_charset, CharsetEncodeErrorPolicy errors,
    std::string* out_error, bool* out_replaced);

[[nodiscard]] bool apply_declared_charset(
    DataSet& dataset, const ParsedSpecificCharacterSet& parsed, std::string* out_error);
DataSet& ensure_dataset_loaded_for_edit(DataSet& dataset);

}  // namespace dicom::charset::detail
