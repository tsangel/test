#include "registry_bootstrap_v2.hpp"

namespace pixel::runtime_v2 {

namespace {

RegistryBootstrapResultV2 initialize_once_impl(
    const std::vector<std::string>& plugin_paths, BindingRegistryRuntimeV2* state) {
  RegistryBootstrapResultV2 result{};
  result.requested_plugin_count = static_cast<uint32_t>(plugin_paths.size());

  init_builtin_registry_v2(&state->registry);
  state->loaded_shared_plugins.reserve(plugin_paths.size());

  for (const std::string& plugin_path : plugin_paths) {
    if (plugin_path.empty()) {
      continue;
    }

    LoadedSharedPluginV2 loaded{};
    if (load_shared_plugin_v2(plugin_path.c_str(), &loaded) !=
        SharedPluginLoadStatusV2::kOk) {
      continue;
    }
    state->loaded_shared_plugins.push_back(loaded);
    ++result.loaded_plugin_count;
  }

  if (!state->loaded_shared_plugins.empty()) {
    result.registered_plugin_count = register_loaded_shared_plugins_v2(
        &state->registry, state->loaded_shared_plugins.data(),
        static_cast<uint32_t>(state->loaded_shared_plugins.size()));
  }
  return result;
}

}  // namespace

bool initialize_registry_v2(const std::vector<std::string>& plugin_paths,
    BindingRegistryRuntimeV2* state, RegistryBootstrapResultV2* out_result) {
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

void shutdown_registry_v2(BindingRegistryRuntimeV2* state) noexcept {
  if (state == nullptr) {
    return;
  }

  std::lock_guard<std::mutex> lock(state->lifecycle_mutex);
  for (LoadedSharedPluginV2& plugin : state->loaded_shared_plugins) {
    unload_shared_plugin_v2(&plugin);
  }
  state->loaded_shared_plugins.clear();
  state->registry.clear();
  state->initialized = false;
  state->shutdown_called = true;
  state->last_result = {};
}

}  // namespace pixel::runtime_v2
