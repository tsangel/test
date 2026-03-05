#pragma once

#include <string_view>

namespace dicom::pixel::detail {

[[nodiscard]] bool has_external_decoder_bridge(std::string_view plugin_key);

}  // namespace dicom::pixel::detail

