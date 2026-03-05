#include "runtime_registry_v2.hpp"

#include <algorithm>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "shared_plugin_loader_v2.hpp"

namespace pixel::runtime_v2 {

namespace {

struct ExternalPluginEntryV2 {
  std::string plugin_key{};
  bool owns_shared_library{false};
  LoadedSharedPluginV2 shared_plugin{};
  bool has_decoder_api{false};
  bool has_encoder_api{false};
  pixel_decoder_plugin_api_v2 decoder_api{};
  pixel_encoder_plugin_api_v2 encoder_api{};
};

struct RuntimeRegistryStateV2 {
  std::mutex mutex{};
  bool initialized{false};
  PluginRegistryV2 registry{};
  std::vector<ExternalPluginEntryV2> external_plugins{};
  uint32_t generated_plugin_index{0};
};

RuntimeRegistryStateV2& runtime_registry_state_v2() {
  static RuntimeRegistryStateV2 state{};
  return state;
}

void set_optional_error(std::string* out_error, std::string message) {
  if (out_error != nullptr) {
    *out_error = std::move(message);
  }
}

void ensure_initialized_locked(RuntimeRegistryStateV2& state) {
  if (state.initialized) {
    return;
  }
  init_builtin_registry_v2(&state.registry);
  state.initialized = true;
}

void rebuild_registry_locked(RuntimeRegistryStateV2& state) {
  state.registry.clear();
  init_builtin_registry_v2(&state.registry);
  for (const auto& plugin : state.external_plugins) {
    if (plugin.has_decoder_api) {
      (void)state.registry.register_decoder_api(&plugin.decoder_api);
    }
    if (plugin.has_encoder_api) {
      (void)state.registry.register_encoder_api(&plugin.encoder_api);
    }
  }
}

std::string load_status_message(SharedPluginLoadStatusV2 status) {
  switch (status) {
  case SharedPluginLoadStatusV2::kOk:
    return "ok";
  case SharedPluginLoadStatusV2::kInvalidArgument:
    return "invalid argument";
  case SharedPluginLoadStatusV2::kOpenFailed:
    return "failed to open shared library";
  case SharedPluginLoadStatusV2::kNoApiEntrypoint:
    return "v2 plugin entrypoint is missing";
  case SharedPluginLoadStatusV2::kDecoderHandshakeFailed:
    return "decoder ABI handshake failed";
  case SharedPluginLoadStatusV2::kEncoderHandshakeFailed:
    return "encoder ABI handshake failed";
  }
  return "unknown plugin load failure";
}

bool has_profile(uint64_t flags, uint32_t profile_code) {
  const uint64_t decode_bit = UINT64_C(1) << profile_code;
  const uint64_t encode_bit = UINT64_C(1) << (32u + profile_code);
  return (flags & decode_bit) != 0 || (flags & encode_bit) != 0;
}

std::string infer_plugin_key_from_flags(
    uint64_t flags, RuntimeRegistryStateV2& state) {
  bool has_jpeg = has_profile(flags, PIXEL_CODEC_PROFILE_JPEG_LOSSLESS_V2) ||
      has_profile(flags, PIXEL_CODEC_PROFILE_JPEG_LOSSY_V2);
  bool has_jpegls = has_profile(flags, PIXEL_CODEC_PROFILE_JPEGLS_LOSSLESS_V2) ||
      has_profile(flags, PIXEL_CODEC_PROFILE_JPEGLS_NEAR_LOSSLESS_V2);
  bool has_jpeg2k = has_profile(flags, PIXEL_CODEC_PROFILE_JPEG2000_LOSSLESS_V2) ||
      has_profile(flags, PIXEL_CODEC_PROFILE_JPEG2000_LOSSY_V2);
  bool has_htj2k = has_profile(flags, PIXEL_CODEC_PROFILE_HTJ2K_LOSSLESS_V2) ||
      has_profile(flags, PIXEL_CODEC_PROFILE_HTJ2K_LOSSLESS_RPCL_V2) ||
      has_profile(flags, PIXEL_CODEC_PROFILE_HTJ2K_LOSSY_V2);
  bool has_jpegxl = has_profile(flags, PIXEL_CODEC_PROFILE_JPEGXL_LOSSLESS_V2) ||
      has_profile(flags, PIXEL_CODEC_PROFILE_JPEGXL_LOSSY_V2) ||
      has_profile(flags, PIXEL_CODEC_PROFILE_JPEGXL_JPEG_RECOMPRESSION_V2);
  bool has_rle = has_profile(flags, PIXEL_CODEC_PROFILE_RLE_LOSSLESS_V2);
  bool has_native = has_profile(flags, PIXEL_CODEC_PROFILE_NATIVE_UNCOMPRESSED_V2) ||
      has_profile(flags, PIXEL_CODEC_PROFILE_ENCAPSULATED_UNCOMPRESSED_V2);

  uint32_t family_count = 0;
  family_count += has_jpeg ? 1u : 0u;
  family_count += has_jpegls ? 1u : 0u;
  family_count += has_jpeg2k ? 1u : 0u;
  family_count += has_htj2k ? 1u : 0u;
  family_count += has_jpegxl ? 1u : 0u;
  family_count += has_rle ? 1u : 0u;
  family_count += has_native ? 1u : 0u;

  if (family_count == 1u) {
    if (has_jpeg) {
      return "jpeg";
    }
    if (has_jpegls) {
      return "jpegls";
    }
    if (has_jpeg2k) {
      return "jpeg2k";
    }
    if (has_htj2k) {
      return "htj2k";
    }
    if (has_jpegxl) {
      return "jpegxl";
    }
    if (has_rle) {
      return "rle";
    }
    if (has_native) {
      return "native";
    }
  }

  return "external-v2-" + std::to_string(state.generated_plugin_index++);
}

std::vector<ExternalPluginEntryV2>::iterator find_plugin_entry(
    RuntimeRegistryStateV2& state, std::string_view plugin_key) {
  return std::find_if(state.external_plugins.begin(), state.external_plugins.end(),
      [plugin_key](const ExternalPluginEntryV2& entry) {
        return entry.plugin_key == plugin_key;
      });
}

bool validate_decoder_api(const pixel_decoder_plugin_api_v2* api) {
  if (api == nullptr) {
    return false;
  }
  if (api->struct_size < sizeof(pixel_decoder_plugin_api_v2) ||
      api->abi_version != PIXEL_DECODER_PLUGIN_ABI_V2) {
    return false;
  }
  if (api->info.struct_size < sizeof(pixel_decoder_plugin_info_v2) ||
      api->info.abi_version != PIXEL_DECODER_PLUGIN_ABI_V2) {
    return false;
  }
  if (api->create == nullptr || api->destroy == nullptr || api->configure == nullptr ||
      api->decode_frame == nullptr || api->copy_last_error_detail == nullptr) {
    return false;
  }
  return true;
}

bool validate_encoder_api(const pixel_encoder_plugin_api_v2* api) {
  if (api == nullptr) {
    return false;
  }
  if (api->struct_size < sizeof(pixel_encoder_plugin_api_v2) ||
      api->abi_version != PIXEL_ENCODER_PLUGIN_ABI_V2) {
    return false;
  }
  if (api->info.struct_size < sizeof(pixel_encoder_plugin_info_v2) ||
      api->info.abi_version != PIXEL_ENCODER_PLUGIN_ABI_V2) {
    return false;
  }
  if (api->create == nullptr || api->destroy == nullptr || api->configure == nullptr ||
      api->encode_frame == nullptr || api->copy_last_error_detail == nullptr) {
    return false;
  }
  return true;
}

}  // namespace

const PluginRegistryV2* current_registry() noexcept {
  auto& state = runtime_registry_state_v2();
  std::lock_guard<std::mutex> lock(state.mutex);
  ensure_initialized_locked(state);
  return &state.registry;
}

bool register_external_codec_plugin_from_library(
    std::string_view library_path, std::string* out_plugin_key, std::string* out_error) {
  if (library_path.empty()) {
    set_optional_error(out_error, "library path is empty");
    return false;
  }

  auto& state = runtime_registry_state_v2();
  std::lock_guard<std::mutex> lock(state.mutex);
  ensure_initialized_locked(state);

  ExternalPluginEntryV2 entry{};
  entry.owns_shared_library = true;
  const std::string library_path_text(library_path);
  const auto load_status =
      load_shared_plugin_v2(library_path_text.c_str(), &entry.shared_plugin);
  if (load_status != SharedPluginLoadStatusV2::kOk) {
    set_optional_error(out_error, load_status_message(load_status));
    return false;
  }

  uint64_t supported_flags = 0;
  if (entry.shared_plugin.has_decoder_api) {
    entry.has_decoder_api = true;
    entry.decoder_api = entry.shared_plugin.decoder_api;
    supported_flags |= entry.decoder_api.info.supported_profile_flags;
  }
  if (entry.shared_plugin.has_encoder_api) {
    entry.has_encoder_api = true;
    entry.encoder_api = entry.shared_plugin.encoder_api;
    supported_flags |= entry.encoder_api.info.supported_profile_flags;
  }
  if (!entry.has_decoder_api && !entry.has_encoder_api) {
    unload_shared_plugin_v2(&entry.shared_plugin);
    set_optional_error(out_error, "shared library has no v2 decoder/encoder API");
    return false;
  }

  entry.plugin_key = infer_plugin_key_from_flags(supported_flags, state);

  auto it = find_plugin_entry(state, entry.plugin_key);
  if (it != state.external_plugins.end()) {
    if (it->owns_shared_library) {
      unload_shared_plugin_v2(&it->shared_plugin);
    }
    state.external_plugins.erase(it);
  }
  state.external_plugins.push_back(std::move(entry));
  rebuild_registry_locked(state);

  if (out_plugin_key != nullptr) {
    *out_plugin_key = state.external_plugins.back().plugin_key;
  }
  set_optional_error(out_error, {});
  return true;
}

bool register_external_decoder_plugin_static(
    const pixel_decoder_plugin_api_v2* api, std::string* out_error) {
  if (!validate_decoder_api(api)) {
    set_optional_error(out_error, "decoder api contract is invalid");
    return false;
  }

  auto& state = runtime_registry_state_v2();
  std::lock_guard<std::mutex> lock(state.mutex);
  ensure_initialized_locked(state);

  const std::string plugin_key =
      infer_plugin_key_from_flags(api->info.supported_profile_flags, state);

  auto it = find_plugin_entry(state, plugin_key);
  if (it == state.external_plugins.end()) {
    ExternalPluginEntryV2 entry{};
    entry.plugin_key = plugin_key;
    entry.has_decoder_api = true;
    entry.decoder_api = *api;
    state.external_plugins.push_back(std::move(entry));
  } else {
    it->has_decoder_api = true;
    it->decoder_api = *api;
  }

  rebuild_registry_locked(state);
  set_optional_error(out_error, {});
  return true;
}

bool register_external_encoder_plugin_static(
    const pixel_encoder_plugin_api_v2* api, std::string* out_error) {
  if (!validate_encoder_api(api)) {
    set_optional_error(out_error, "encoder api contract is invalid");
    return false;
  }

  auto& state = runtime_registry_state_v2();
  std::lock_guard<std::mutex> lock(state.mutex);
  ensure_initialized_locked(state);

  const std::string plugin_key =
      infer_plugin_key_from_flags(api->info.supported_profile_flags, state);

  auto it = find_plugin_entry(state, plugin_key);
  if (it == state.external_plugins.end()) {
    ExternalPluginEntryV2 entry{};
    entry.plugin_key = plugin_key;
    entry.has_encoder_api = true;
    entry.encoder_api = *api;
    state.external_plugins.push_back(std::move(entry));
  } else {
    it->has_encoder_api = true;
    it->encoder_api = *api;
  }

  rebuild_registry_locked(state);
  set_optional_error(out_error, {});
  return true;
}

bool unregister_external_codec_plugin(
    std::string_view plugin_key, std::string* out_error) {
  if (plugin_key.empty()) {
    set_optional_error(out_error, "plugin key is empty");
    return false;
  }

  auto& state = runtime_registry_state_v2();
  std::lock_guard<std::mutex> lock(state.mutex);
  ensure_initialized_locked(state);

  auto it = find_plugin_entry(state, plugin_key);
  if (it == state.external_plugins.end()) {
    set_optional_error(out_error, "plugin is not registered");
    return false;
  }
  if (it->owns_shared_library) {
    unload_shared_plugin_v2(&it->shared_plugin);
  }
  state.external_plugins.erase(it);
  rebuild_registry_locked(state);
  set_optional_error(out_error, {});
  return true;
}

}  // namespace pixel::runtime_v2
