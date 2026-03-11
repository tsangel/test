#pragma once

#include <optional>
#include <string>
#include <vector>

#include "dicom.h"

namespace dicom::charset {

[[nodiscard]] std::optional<std::vector<std::string>> raw_element_as_owned_utf8_values(
    const DataElement& element, std::string* out_error = nullptr);
[[nodiscard]] std::optional<std::vector<std::string>> raw_element_as_owned_utf8_values(
    const DataElement& element, CharsetDecodeErrorPolicy errors,
    std::string* out_error = nullptr);
[[nodiscard]] std::optional<std::vector<std::string>> raw_element_as_owned_utf8_values(
    const DataElement& element, CharsetDecodeErrorPolicy errors, std::string* out_error,
    bool* out_replaced);

}  // namespace dicom::charset
