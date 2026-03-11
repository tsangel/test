#pragma once

#include <string_view>

namespace dicom::charset {

[[nodiscard]] bool validate_ascii(std::string_view value) noexcept;
[[nodiscard]] bool validate_utf8(std::string_view value) noexcept;

}  // namespace dicom::charset
