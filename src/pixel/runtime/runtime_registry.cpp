#include "runtime_registry.hpp"

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "shared_library_path.hpp"
#include "shared_plugin_loader.hpp"

namespace pixel::runtime {

namespace {

struct ExternalPluginEntry {
  std::string library_path{};
  bool owns_shared_library{false};
  LoadedSharedPlugin shared_plugin{};
  bool has_decoder_api{false};
  bool has_encoder_api{false};
  pixel_decoder_plugin_api decoder_api{};
  pixel_encoder_plugin_api encoder_api{};
};

struct RuntimeRegistryState {
  std::mutex mutex{};
  bool initialized{false};
  Htj2kDecoderBackendPreference htj2k_decoder_backend_preference{
      Htj2kDecoderBackendPreference::kAuto};
  std::uint64_t generation{0};
  BindingRegistry registry{};
  std::vector<ExternalPluginEntry> external_plugins{};
};

RuntimeRegistryState& runtime_registry_state() {
  static RuntimeRegistryState state{};
  return state;
}

void set_optional_error(std::string* out_error, std::string message) {
  if (out_error != nullptr) {
    *out_error = std::move(message);
  }
}

void ensure_initialized_locked(RuntimeRegistryState& state) {
  if (state.initialized) {
    return;
  }
  init_builtin_registry(
      &state.registry, state.htj2k_decoder_backend_preference);
  ++state.generation;
  state.initialized = true;
}

void rebuild_registry_locked(RuntimeRegistryState& state) {
  state.registry.clear();
  init_builtin_registry(
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

}  // namespace

const BindingRegistry* current_registry() noexcept {
  auto& state = runtime_registry_state();
  std::lock_guard<std::mutex> lock(state.mutex);
  ensure_initialized_locked(state);
  return &state.registry;
}

std::uint64_t current_registry_generation() noexcept {
  auto& state = runtime_registry_state();
  std::lock_guard<std::mutex> lock(state.mutex);
  ensure_initialized_locked(state);
  return state.generation;
}

bool set_htj2k_decoder_backend_preference(
    Htj2kDecoderBackendPreference backend, std::string* out_error) {
  auto& state = runtime_registry_state();
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
  auto& state = runtime_registry_state();
  std::lock_guard<std::mutex> lock(state.mutex);
  return state.htj2k_decoder_backend_preference;
}

bool register_external_codec_plugin_from_library(
    const std::filesystem::path& library_path, std::string* out_error) {
  if (library_path.empty()) {
    set_optional_error(out_error, "library path is empty");
    return false;
  }

  auto& state = runtime_registry_state();
  std::lock_guard<std::mutex> lock(state.mutex);
  ensure_initialized_locked(state);

  std::filesystem::path resolved_library_path{};
  if (!detail::resolve_shared_library_path(
          library_path, &resolved_library_path, out_error)) {
    return false;
  }
  const std::string library_path_text =
      detail::filesystem_path_to_utf8(resolved_library_path);

  ExternalPluginEntry entry{};
  entry.owns_shared_library = true;
  const auto load_status =
      load_shared_plugin(resolved_library_path, &entry.shared_plugin);
  if (load_status != SharedPluginLoadStatus::kOk) {
    std::string status_message;
    switch (load_status) {
    case SharedPluginLoadStatus::kOk:
      status_message = "ok";
      break;
    case SharedPluginLoadStatus::kInvalidArgument:
      status_message = "invalid argument";
      break;
    case SharedPluginLoadStatus::kOpenFailed:
      status_message = "failed to open shared library";
      break;
    case SharedPluginLoadStatus::kNoApiEntrypoint:
      status_message = "compatible decoder/encoder plugin entrypoint is missing";
      break;
    case SharedPluginLoadStatus::kDecoderHandshakeFailed:
      status_message = "decoder ABI handshake failed";
      break;
    case SharedPluginLoadStatus::kEncoderHandshakeFailed:
      status_message = "encoder ABI handshake failed";
      break;
    default:
      status_message = "unknown plugin load failure";
      break;
    }
    set_optional_error(out_error,
        std::move(status_message) + ": " + library_path_text);
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
    unload_shared_plugin(&entry.shared_plugin);
    set_optional_error(out_error, "shared library has no compatible decoder or encoder API");
    return false;
  }

  entry.library_path = library_path_text;

  // Replace an existing plugin loaded from the same resolved library path.
  auto it = std::find_if(state.external_plugins.begin(), state.external_plugins.end(),
      [&library_path_text](const ExternalPluginEntry& existing_entry) {
        return existing_entry.owns_shared_library &&
            existing_entry.library_path == library_path_text;
      });
  if (it != state.external_plugins.end()) {
    if (it->owns_shared_library) {
      unload_shared_plugin(&it->shared_plugin);
    }
    state.external_plugins.erase(it);
  }
  state.external_plugins.push_back(std::move(entry));
  rebuild_registry_locked(state);

  set_optional_error(out_error, {});
  return true;
}

bool clear_external_codec_plugins(std::string* out_error) {
  auto& state = runtime_registry_state();
  std::lock_guard<std::mutex> lock(state.mutex);
  for (auto& entry : state.external_plugins) {
    if (entry.owns_shared_library) {
      unload_shared_plugin(&entry.shared_plugin);
    }
  }
  state.external_plugins.clear();
  if (state.initialized) {
    rebuild_registry_locked(state);
  }
  set_optional_error(out_error, {});
  return true;
}

}  // namespace pixel::runtime

