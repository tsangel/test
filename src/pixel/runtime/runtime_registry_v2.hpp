#pragma once

#include <string>
#include <string_view>

#include "pixel_decoder_plugin_abi_v2.h"
#include "pixel_encoder_plugin_abi_v2.h"
#include "plugin_registry_v2.hpp"

namespace pixel::runtime_v2 {

[[nodiscard]] const PluginRegistryV2* current_registry() noexcept;

[[nodiscard]] bool register_external_codec_plugin_from_library(
    std::string_view library_path, std::string* out_plugin_key, std::string* out_error);

[[nodiscard]] bool register_external_decoder_plugin_static(
    const pixel_decoder_plugin_api_v2* api, std::string* out_error);

[[nodiscard]] bool register_external_encoder_plugin_static(
    const pixel_encoder_plugin_api_v2* api, std::string* out_error);

[[nodiscard]] bool unregister_external_codec_plugin(
    std::string_view plugin_key, std::string* out_error);

}  // namespace pixel::runtime_v2
