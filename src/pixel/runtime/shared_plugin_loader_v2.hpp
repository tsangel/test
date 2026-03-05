#pragma once

#include <cstdint>

#include "plugin_registry_v2.hpp"

namespace pixel::runtime_v2 {

enum class SharedPluginLoadStatusV2 : uint32_t {
  kOk = 0u,
  kInvalidArgument = 1u,
  kOpenFailed = 2u,
  kNoApiEntrypoint = 3u,
  kDecoderHandshakeFailed = 4u,
  kEncoderHandshakeFailed = 5u,
};

struct LoadedSharedPluginV2 {
  void* native_handle{nullptr};
  bool has_decoder_api{false};
  bool has_encoder_api{false};
  pixel_decoder_plugin_api_v2 decoder_api{};
  pixel_encoder_plugin_api_v2 encoder_api{};
};

SharedPluginLoadStatusV2 load_shared_plugin_v2(
    const char* library_path, LoadedSharedPluginV2* out_plugin);

void unload_shared_plugin_v2(LoadedSharedPluginV2* plugin) noexcept;

uint32_t register_loaded_shared_plugins_v2(PluginRegistryV2* registry,
    const LoadedSharedPluginV2* loaded_plugins, uint32_t loaded_plugin_count);

}  // namespace pixel::runtime_v2

