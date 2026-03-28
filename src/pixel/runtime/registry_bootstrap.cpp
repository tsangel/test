#include "registry_bootstrap.hpp"

#include <cstdio>
#include <unordered_set>

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

void write_registry_bootstrap_warning(
    const char* path_label, const std::string& path, const char* detail_label,
    const std::string& detail) noexcept {
  std::fprintf(stderr,
      "[WARN] pixel runtime bootstrap %s=%s %s=%s\n", path_label, path.c_str(),
      detail_label, detail.c_str());
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
    std::filesystem::path resolved_plugin_path{};
    std::string resolve_error{};
    if (!detail::resolve_shared_library_path(
            std::string_view(plugin_path), &resolved_plugin_path, &resolve_error)) {
      write_registry_bootstrap_warning(
          "path", plugin_path, "reason",
          resolve_error.empty() ? "failed to resolve shared library path"
                                : resolve_error);
      continue;
    }
    const std::string resolved_plugin_path_text =
        detail::filesystem_path_to_utf8(resolved_plugin_path);
    if (!loaded_library_paths.insert(resolved_plugin_path_text).second) {
      continue;
    }

    LoadedSharedPlugin loaded{};
    const SharedPluginLoadStatus load_status =
        load_shared_plugin(resolved_plugin_path, &loaded);
    if (load_status != SharedPluginLoadStatus::kOk) {
      write_registry_bootstrap_warning(
          "path", resolved_plugin_path_text, "status",
          describe_shared_plugin_load_status(load_status));
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

