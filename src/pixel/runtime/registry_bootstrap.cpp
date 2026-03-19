#include "registry_bootstrap.hpp"

#include <unordered_set>

#include "diagnostics.h"
#include "shared_library_path.hpp"

namespace pixel::runtime {

namespace {

const char* describe_shared_plugin_load_status(
    SharedPluginLoadStatus status) noexcept {
  switch (status) {
    case SharedPluginLoadStatus::kOk:
      return "ok";
    case SharedPluginLoadStatus::kInvalidArgument:
      return "invalid_argument";
    case SharedPluginLoadStatus::kOpenFailed:
      return "open_failed";
    case SharedPluginLoadStatus::kNoApiEntrypoint:
      return "no_api_entrypoint";
    case SharedPluginLoadStatus::kDecoderHandshakeFailed:
      return "decoder_handshake_failed";
    case SharedPluginLoadStatus::kEncoderHandshakeFailed:
      return "encoder_handshake_failed";
  }
  return "unknown";
}

RegistryBootstrapResult initialize_once_impl(
    const std::vector<std::string>& plugin_paths, BindingRegistryRuntime* state) {
  RegistryBootstrapResult result{};
  result.requested_plugin_count = static_cast<uint32_t>(plugin_paths.size());

  init_builtin_registry(&state->registry);
  state->loaded_shared_plugins.reserve(plugin_paths.size());
  std::unordered_set<std::string> loaded_library_paths{};
  loaded_library_paths.reserve(plugin_paths.size());

  for (const std::string& plugin_path : plugin_paths) {
    std::string resolved_plugin_path{};
    std::string resolve_error{};
    if (!detail::resolve_shared_library_path(
            plugin_path, &resolved_plugin_path, &resolve_error)) {
      dicom::diag::warn(
          "pixel runtime bootstrap skipped plugin path={} reason={}",
          plugin_path,
          resolve_error.empty() ? "failed to resolve shared library path"
                                : resolve_error);
      continue;
    }
    if (!loaded_library_paths.insert(resolved_plugin_path).second) {
      continue;
    }

    LoadedSharedPlugin loaded{};
    const SharedPluginLoadStatus load_status =
        load_shared_plugin(resolved_plugin_path.c_str(), &loaded);
    if (load_status != SharedPluginLoadStatus::kOk) {
      dicom::diag::warn(
          "pixel runtime bootstrap failed plugin path={} status={}",
          resolved_plugin_path, describe_shared_plugin_load_status(load_status));
      continue;
    }
    state->loaded_shared_plugins.push_back(loaded);
    ++result.loaded_plugin_count;
  }

  if (!state->loaded_shared_plugins.empty()) {
    result.registered_plugin_count = register_loaded_shared_plugins(
        &state->registry, state->loaded_shared_plugins.data(),
        static_cast<uint32_t>(state->loaded_shared_plugins.size()));
  }
  return result;
}

}  // namespace

bool initialize_registry(const std::vector<std::string>& plugin_paths,
    BindingRegistryRuntime* state, RegistryBootstrapResult* out_result) {
  if (state == nullptr) {
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(state->lifecycle_mutex);
    if (state->shutdown_called) {
      if (out_result != nullptr) {
        *out_result = {};
      }
      return false;
    }
  }

  try {
    std::call_once(state->initialize_once, [&plugin_paths, state]() {
      std::lock_guard<std::mutex> lock(state->lifecycle_mutex);
      if (state->shutdown_called) {
        state->initialized = false;
        state->last_result = {};
        return;
      }
      state->last_result = initialize_once_impl(plugin_paths, state);
      state->initialized = true;
    });
  } catch (...) {
    if (out_result != nullptr) {
      *out_result = {};
    }
    return false;
  }

  std::lock_guard<std::mutex> lock(state->lifecycle_mutex);
  if (out_result != nullptr) {
    *out_result = state->last_result;
  }
  return state->initialized && !state->shutdown_called;
}

void shutdown_registry(BindingRegistryRuntime* state) noexcept {
  if (state == nullptr) {
    return;
  }

  std::lock_guard<std::mutex> lock(state->lifecycle_mutex);
  for (LoadedSharedPlugin& plugin : state->loaded_shared_plugins) {
    unload_shared_plugin(&plugin);
  }
  state->loaded_shared_plugins.clear();
  state->registry.clear();
  state->initialized = false;
  state->shutdown_called = true;
  state->last_result = {};
}

}  // namespace pixel::runtime

