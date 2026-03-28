#include "shared_plugin_loader.hpp"

#include <cstddef>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace pixel::runtime {

namespace {

using get_decoder_api_fn = int (*)(pixel_decoder_plugin_api* out_api);
using get_encoder_api_fn = int (*)(pixel_encoder_plugin_api* out_api);
constexpr std::size_t kEncoderApiRequiredPrefixSize =
    offsetof(pixel_encoder_plugin_api, encode_frame_to_context_buffer);

void* open_shared_library(const std::filesystem::path& library_path) noexcept {
#if defined(_WIN32)
  return reinterpret_cast<void*>(LoadLibraryW(library_path.c_str()));
#else
  return dlopen(library_path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

void close_shared_library(void* native_handle) noexcept {
  if (native_handle == nullptr) {
    return;
  }
#if defined(_WIN32)
  auto* handle = reinterpret_cast<HMODULE>(native_handle);
  FreeLibrary(handle);
#else
  dlclose(native_handle);
#endif
}

void* find_symbol(void* native_handle, const char* symbol_name) noexcept {
  if (native_handle == nullptr || symbol_name == nullptr) {
    return nullptr;
  }
#if defined(_WIN32)
  auto* handle = reinterpret_cast<HMODULE>(native_handle);
  return reinterpret_cast<void*>(GetProcAddress(handle, symbol_name));
#else
  return dlsym(native_handle, symbol_name);
#endif
}

bool validate_decoder_api_contract(const pixel_decoder_plugin_api& api) noexcept {
  if (api.struct_size < sizeof(pixel_decoder_plugin_api) ||
      api.abi_version != PIXEL_DECODER_PLUGIN_ABI) {
    return false;
  }
  if (api.info.struct_size < sizeof(pixel_decoder_plugin_info) ||
      api.info.abi_version != PIXEL_DECODER_PLUGIN_ABI) {
    return false;
  }
  if (api.create == nullptr || api.destroy == nullptr || api.configure == nullptr ||
      api.decode_frame == nullptr || api.copy_last_error_detail == nullptr) {
    return false;
  }
  return true;
}

bool validate_encoder_api_contract(const pixel_encoder_plugin_api& api) noexcept {
  if (api.struct_size < kEncoderApiRequiredPrefixSize ||
      api.abi_version != PIXEL_ENCODER_PLUGIN_ABI) {
    return false;
  }
  if (api.info.struct_size < sizeof(pixel_encoder_plugin_info) ||
      api.info.abi_version != PIXEL_ENCODER_PLUGIN_ABI) {
    return false;
  }
  if (api.create == nullptr || api.destroy == nullptr || api.configure == nullptr ||
      api.encode_frame == nullptr || api.copy_last_error_detail == nullptr) {
    return false;
  }
  return true;
}

SharedPluginLoadStatus load_decoder_api(void* native_handle,
    get_decoder_api_fn get_decoder_api, LoadedSharedPlugin* out_plugin) noexcept {
  if (get_decoder_api == nullptr || out_plugin == nullptr) {
    return SharedPluginLoadStatus::kOk;
  }

  pixel_decoder_plugin_api decoder_api{};
  decoder_api.struct_size = sizeof(pixel_decoder_plugin_api);
  decoder_api.abi_version = PIXEL_DECODER_PLUGIN_ABI;

  int handshake_ok = 0;
  try {
    handshake_ok = get_decoder_api(&decoder_api);
  } catch (...) {
    close_shared_library(native_handle);
    return SharedPluginLoadStatus::kDecoderHandshakeFailed;
  }
  if (handshake_ok == 0 || !validate_decoder_api_contract(decoder_api)) {
    close_shared_library(native_handle);
    return SharedPluginLoadStatus::kDecoderHandshakeFailed;
  }

  out_plugin->decoder_api = decoder_api;
  out_plugin->has_decoder_api = true;
  return SharedPluginLoadStatus::kOk;
}

SharedPluginLoadStatus load_encoder_api(void* native_handle,
    get_encoder_api_fn get_encoder_api, LoadedSharedPlugin* out_plugin) noexcept {
  if (get_encoder_api == nullptr || out_plugin == nullptr) {
    return SharedPluginLoadStatus::kOk;
  }

  pixel_encoder_plugin_api encoder_api{};
  encoder_api.struct_size = sizeof(pixel_encoder_plugin_api);
  encoder_api.abi_version = PIXEL_ENCODER_PLUGIN_ABI;

  int handshake_ok = 0;
  try {
    handshake_ok = get_encoder_api(&encoder_api);
  } catch (...) {
    close_shared_library(native_handle);
    return SharedPluginLoadStatus::kEncoderHandshakeFailed;
  }
  if (handshake_ok == 0 || !validate_encoder_api_contract(encoder_api)) {
    close_shared_library(native_handle);
    return SharedPluginLoadStatus::kEncoderHandshakeFailed;
  }

  out_plugin->encoder_api = encoder_api;
  out_plugin->has_encoder_api = true;
  return SharedPluginLoadStatus::kOk;
}

}  // namespace

SharedPluginLoadStatus load_shared_plugin(
    const std::filesystem::path& library_path, LoadedSharedPlugin* out_plugin) {
  if (library_path.empty() || out_plugin == nullptr) {
    return SharedPluginLoadStatus::kInvalidArgument;
  }

  *out_plugin = {};

  void* native_handle = open_shared_library(library_path);
  if (native_handle == nullptr) {
    return SharedPluginLoadStatus::kOpenFailed;
  }

  auto* get_decoder_api = reinterpret_cast<get_decoder_api_fn>(
      find_symbol(native_handle, "pixel_get_decoder_plugin_api"));
  auto* get_encoder_api = reinterpret_cast<get_encoder_api_fn>(
      find_symbol(native_handle, "pixel_get_encoder_plugin_api"));

  if (get_decoder_api == nullptr && get_encoder_api == nullptr) {
    close_shared_library(native_handle);
    return SharedPluginLoadStatus::kNoApiEntrypoint;
  }

  out_plugin->native_handle = native_handle;

  const SharedPluginLoadStatus decoder_status =
      load_decoder_api(native_handle, get_decoder_api, out_plugin);
  if (decoder_status != SharedPluginLoadStatus::kOk) {
    *out_plugin = {};
    return decoder_status;
  }

  const SharedPluginLoadStatus encoder_status =
      load_encoder_api(native_handle, get_encoder_api, out_plugin);
  if (encoder_status != SharedPluginLoadStatus::kOk) {
    *out_plugin = {};
    return encoder_status;
  }

  return SharedPluginLoadStatus::kOk;
}

void unload_shared_plugin(LoadedSharedPlugin* plugin) noexcept {
  if (plugin == nullptr) {
    return;
  }
  close_shared_library(plugin->native_handle);
  *plugin = {};
}

uint32_t register_loaded_shared_plugins(BindingRegistry* registry,
    const LoadedSharedPlugin* loaded_plugins, uint32_t loaded_plugin_count) {
  if (registry == nullptr || loaded_plugins == nullptr || loaded_plugin_count == 0) {
    return 0;
  }

  uint32_t registered_plugin_count = 0;
  for (uint32_t i = 0; i < loaded_plugin_count; ++i) {
    const LoadedSharedPlugin& loaded = loaded_plugins[i];
    LoadedPluginApis apis{};
    if (loaded.has_decoder_api) {
      apis.decoder_api = &loaded.decoder_api;
    }
    if (loaded.has_encoder_api) {
      apis.encoder_api = &loaded.encoder_api;
    }
    if (register_loaded_plugins(registry, &apis, 1u) != 0u) {
      ++registered_plugin_count;
    }
  }
  return registered_plugin_count;
}

}  // namespace pixel::runtime

