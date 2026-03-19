#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "pixel_codec_plugin_abi.h"
#include "registry_bootstrap.hpp"

namespace {

[[noreturn]] void fail(const std::string& message) {
  std::cerr << message << std::endl;
  std::exit(1);
}

void expect_true(bool value, std::string_view label) {
  if (!value) {
    fail(std::string(label) + " expected true");
  }
}

template <typename T>
void expect_eq(const T& actual, const T& expected, std::string_view label) {
  if (!(actual == expected)) {
    fail(std::string(label) + " mismatch");
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2 || argv[1] == nullptr) {
    fail("plugin path argument is required");
  }

  const std::filesystem::path plugin_path =
      std::filesystem::absolute(argv[1]);
  const std::filesystem::path plugin_alias_path =
      plugin_path.parent_path() / "." / plugin_path.filename();

  std::vector<std::string> plugin_paths;
  plugin_paths.emplace_back(plugin_path.string());
  plugin_paths.emplace_back(plugin_alias_path.string());

  pixel::runtime::BindingRegistryRuntime state{};
  pixel::runtime::RegistryBootstrapResult result{};
  expect_true(
      pixel::runtime::initialize_registry(plugin_paths, &state, &result),
      "initialize registry");

  expect_eq(result.requested_plugin_count, 2u, "requested plugin count");
  expect_eq(result.loaded_plugin_count, 1u, "loaded plugin count");
  expect_eq(result.registered_plugin_count, 1u, "registered plugin count");

  pixel::runtime::RegistryBootstrapResult second_result{};
  expect_true(
      pixel::runtime::initialize_registry(plugin_paths, &state, &second_result),
      "second initialize registry");
  expect_eq(second_result.requested_plugin_count, 2u, "second requested plugin count");
  expect_eq(second_result.loaded_plugin_count, 1u, "second loaded plugin count");
  expect_eq(second_result.registered_plugin_count, 1u, "second registered plugin count");

  const auto* decoder_binding =
      state.registry.find_decoder_binding(PIXEL_CODEC_PROFILE_JPEG_LOSSLESS);
  const auto* encoder_binding =
      state.registry.find_encoder_binding(PIXEL_CODEC_PROFILE_JPEG_LOSSLESS);
  expect_true(decoder_binding != nullptr, "decoder binding exists");
  expect_true(encoder_binding != nullptr, "encoder binding exists");
  expect_eq(decoder_binding->binding_kind,
      pixel::runtime::DecoderBindingKind::kPluginApi,
      "decoder binding kind");
  expect_eq(encoder_binding->binding_kind,
      pixel::runtime::EncoderBindingKind::kPluginApi,
      "encoder binding kind");

  pixel::runtime::shutdown_registry(&state);
  expect_true(state.registry.find_decoder_binding(PIXEL_CODEC_PROFILE_JPEG_LOSSLESS) == nullptr,
      "decoder binding removed after shutdown");
  expect_true(state.registry.find_encoder_binding(PIXEL_CODEC_PROFILE_JPEG_LOSSLESS) == nullptr,
      "encoder binding removed after shutdown");

  pixel::runtime::RegistryBootstrapResult after_shutdown_result{};
  expect_true(
      !pixel::runtime::initialize_registry(plugin_paths, &state, &after_shutdown_result),
      "initialize should fail after shutdown");
  expect_eq(after_shutdown_result.requested_plugin_count, 0u,
      "after-shutdown requested plugin count");
  expect_eq(after_shutdown_result.loaded_plugin_count, 0u,
      "after-shutdown loaded plugin count");
  expect_eq(after_shutdown_result.registered_plugin_count, 0u,
      "after-shutdown registered plugin count");

  return 0;
}

