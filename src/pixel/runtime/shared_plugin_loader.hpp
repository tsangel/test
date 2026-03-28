#pragma once

#include <cstdint>
#include <filesystem>

#include "plugin_registry.hpp"

namespace pixel::runtime {

enum class SharedPluginLoadStatus : uint32_t {
  kOk = 0u,
  kInvalidArgument = 1u,
  kOpenFailed = 2u,
  kNoApiEntrypoint = 3u,
  kDecoderHandshakeFailed = 4u,
  kEncoderHandshakeFailed = 5u,
};

struct LoadedSharedPlugin {
  void* native_handle{nullptr};
  bool has_decoder_api{false};
  bool has_encoder_api{false};
  pixel_decoder_plugin_api decoder_api{};
  pixel_encoder_plugin_api encoder_api{};
};

SharedPluginLoadStatus load_shared_plugin(
    const std::filesystem::path& library_path, LoadedSharedPlugin* out_plugin);

void unload_shared_plugin(LoadedSharedPlugin* plugin) noexcept;

uint32_t register_loaded_shared_plugins(BindingRegistry* registry,
    const LoadedSharedPlugin* loaded_plugins, uint32_t loaded_plugin_count);

}  // namespace pixel::runtime

