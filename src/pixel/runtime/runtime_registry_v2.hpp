#pragma once

#include <string>
#include <string_view>

#include "plugin_registry_v2.hpp"

namespace pixel::runtime_v2 {

[[nodiscard]] const PluginRegistryV2* current_registry() noexcept;
[[nodiscard]] std::uint64_t current_registry_generation() noexcept;

[[nodiscard]] bool set_htj2k_decoder_backend_preference(
    Htj2kDecoderBackendPreference backend, std::string* out_error);

[[nodiscard]] Htj2kDecoderBackendPreference get_htj2k_decoder_backend_preference();

[[nodiscard]] bool register_external_codec_plugin_from_library(
    std::string_view library_path, std::string* out_error);

[[nodiscard]] bool clear_external_codec_plugins(std::string* out_error);

}  // namespace pixel::runtime_v2
