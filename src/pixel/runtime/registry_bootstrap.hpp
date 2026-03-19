#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "plugin_registry.hpp"
#include "shared_plugin_loader.hpp"

namespace pixel::runtime {

struct RegistryBootstrapResult {
  uint32_t requested_plugin_count{0};
  uint32_t loaded_plugin_count{0};
  uint32_t registered_plugin_count{0};
};

struct BindingRegistryRuntime {
  BindingRegistry registry{};
  std::vector<LoadedSharedPlugin> loaded_shared_plugins{};

  RegistryBootstrapResult last_result{};
  bool initialized{false};
  bool shutdown_called{false};
  std::mutex lifecycle_mutex{};
  std::once_flag initialize_once{};
};

bool initialize_registry(const std::vector<std::string>& plugin_paths,
    BindingRegistryRuntime* state,
    RegistryBootstrapResult* out_result = nullptr);

void shutdown_registry(BindingRegistryRuntime* state) noexcept;

}  // namespace pixel::runtime

