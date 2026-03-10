#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "plugin_registry_v2.hpp"
#include "shared_plugin_loader_v2.hpp"

namespace pixel::runtime_v2 {

struct RegistryBootstrapResultV2 {
  uint32_t requested_plugin_count{0};
  uint32_t loaded_plugin_count{0};
  uint32_t registered_plugin_count{0};
};

struct BindingRegistryRuntimeV2 {
  BindingRegistryV2 registry{};
  std::vector<LoadedSharedPluginV2> loaded_shared_plugins{};

  RegistryBootstrapResultV2 last_result{};
  bool initialized{false};
  bool shutdown_called{false};
  std::mutex lifecycle_mutex{};
  std::once_flag initialize_once{};
};

bool initialize_registry_v2(const std::vector<std::string>& plugin_paths,
    BindingRegistryRuntimeV2* state,
    RegistryBootstrapResultV2* out_result = nullptr);

void shutdown_registry_v2(BindingRegistryRuntimeV2* state) noexcept;

}  // namespace pixel::runtime_v2
