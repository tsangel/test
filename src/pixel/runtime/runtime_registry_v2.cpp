#include "runtime_registry_v2.hpp"

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "shared_library_path_v2.hpp"
#include "shared_plugin_loader_v2.hpp"

namespace pixel::runtime_v2 {

namespace {

struct ExternalPluginEntryV2 {
  std::string library_path{};
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
  Htj2kDecoderBackendPreference htj2k_decoder_backend_preference{
      Htj2kDecoderBackendPreference::kAuto};
  std::uint64_t generation{0};
  BindingRegistryV2 registry{};
  std::vector<ExternalPluginEntryV2> external_plugins{};
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
  init_builtin_registry_v2(
      &state.registry, state.htj2k_decoder_backend_preference);
  ++state.generation;
  state.initialized = true;
}

void rebuild_registry_locked(RuntimeRegistryStateV2& state) {
  state.registry.clear();
  init_builtin_registry_v2(
      &state.registry, state.htj2k_decoder_backend_preference);
  for (const auto& plugin : state.external_plugins) {
    if (plugin.has_decoder_api) {
      (void)state.registry.register_decoder_api(&plugin.decoder_api);
    }
    if (plugin.has_encoder_api) {
      (void)state.registry.register_encoder_api(&plugin.encoder_api);
    }
  }
  ++state.generation;
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

std::vector<ExternalPluginEntryV2>::iterator find_library_plugin_entry(
    RuntimeRegistryStateV2& state, std::string_view library_path) {
  return std::find_if(state.external_plugins.begin(), state.external_plugins.end(),
      [library_path](const ExternalPluginEntryV2& entry) {
        return entry.owns_shared_library && entry.library_path == library_path;
      });
}

}  // namespace

const BindingRegistryV2* current_registry() noexcept {
  auto& state = runtime_registry_state_v2();
  std::lock_guard<std::mutex> lock(state.mutex);
  ensure_initialized_locked(state);
  return &state.registry;
}

std::uint64_t current_registry_generation() noexcept {
  auto& state = runtime_registry_state_v2();
  std::lock_guard<std::mutex> lock(state.mutex);
  ensure_initialized_locked(state);
  return state.generation;
}

bool set_htj2k_decoder_backend_preference(
    Htj2kDecoderBackendPreference backend, std::string* out_error) {
  auto& state = runtime_registry_state_v2();
  std::lock_guard<std::mutex> lock(state.mutex);
  switch (backend) {
  case Htj2kDecoderBackendPreference::kOpenJph:
#if !defined(DICOMSDL_PIXEL_RUNTIME_WITH_HTJ2K_STATIC)
    set_optional_error(
        out_error,
        "OpenJPH HTJ2K decoder backend is not available in this build");
    return false;
#endif
    break;
  case Htj2kDecoderBackendPreference::kOpenJpeg:
#if !defined(DICOMSDL_PIXEL_RUNTIME_WITH_OPENJPEG_STATIC)
    set_optional_error(
        out_error,
        "OpenJPEG HTJ2K decoder backend is not available in this build");
    return false;
#endif
    break;
  case Htj2kDecoderBackendPreference::kAuto:
  default:
    break;
  }
  if (state.initialized) {
    set_optional_error(
        out_error,
        "HTJ2K decoder backend can only be configured before pixel runtime "
        "initialization (before first pixel decode/encode or external plugin registration)");
    return false;
  }
  state.htj2k_decoder_backend_preference = backend;
  set_optional_error(out_error, {});
  return true;
}

Htj2kDecoderBackendPreference get_htj2k_decoder_backend_preference() {
  auto& state = runtime_registry_state_v2();
  std::lock_guard<std::mutex> lock(state.mutex);
  return state.htj2k_decoder_backend_preference;
}

bool register_external_codec_plugin_from_library(
    std::string_view library_path, std::string* out_error) {
  if (library_path.empty()) {
    set_optional_error(out_error, "library path is empty");
    return false;
  }

  std::string library_path_text{};
  if (!detail::resolve_shared_library_path(
          library_path, &library_path_text, out_error)) {
    return false;
  }

  auto& state = runtime_registry_state_v2();
  std::lock_guard<std::mutex> lock(state.mutex);
  ensure_initialized_locked(state);

  ExternalPluginEntryV2 entry{};
  entry.owns_shared_library = true;
  const auto load_status =
      load_shared_plugin_v2(library_path_text.c_str(), &entry.shared_plugin);
  if (load_status != SharedPluginLoadStatusV2::kOk) {
    set_optional_error(out_error,
        load_status_message(load_status) + ": " + library_path_text);
    return false;
  }

  if (entry.shared_plugin.has_decoder_api) {
    entry.has_decoder_api = true;
    entry.decoder_api = entry.shared_plugin.decoder_api;
  }
  if (entry.shared_plugin.has_encoder_api) {
    entry.has_encoder_api = true;
    entry.encoder_api = entry.shared_plugin.encoder_api;
  }
  if (!entry.has_decoder_api && !entry.has_encoder_api) {
    unload_shared_plugin_v2(&entry.shared_plugin);
    set_optional_error(out_error, "shared library has no v2 decoder/encoder API");
    return false;
  }

  entry.library_path = library_path_text;

  auto it = find_library_plugin_entry(state, library_path_text);
  if (it != state.external_plugins.end()) {
    if (it->owns_shared_library) {
      unload_shared_plugin_v2(&it->shared_plugin);
    }
    state.external_plugins.erase(it);
  }
  state.external_plugins.push_back(std::move(entry));
  rebuild_registry_locked(state);

  set_optional_error(out_error, {});
  return true;
}

bool clear_external_codec_plugins(std::string* out_error) {
  auto& state = runtime_registry_state_v2();
  std::lock_guard<std::mutex> lock(state.mutex);
  for (auto& entry : state.external_plugins) {
    if (entry.owns_shared_library) {
      unload_shared_plugin_v2(&entry.shared_plugin);
    }
  }
  state.external_plugins.clear();
  if (state.initialized) {
    rebuild_registry_locked(state);
  }
  set_optional_error(out_error, {});
  return true;
}

}  // namespace pixel::runtime_v2
