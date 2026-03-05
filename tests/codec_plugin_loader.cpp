#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

#include "../src/pixel_/plugin_abi/external/plugin_loader.hpp"

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

void expect_false(bool value, std::string_view label) {
  if (value) {
    fail(std::string(label) + " expected false");
  }
}

void expect_not_empty(std::string_view value, std::string_view label) {
  if (value.empty()) {
    fail(std::string(label) + " should not be empty");
  }
}

void test_invalid_library_path_load_failure() {
  using dicom::pixel::detail::abi::ExternalDecoderPlugin;
  using dicom::pixel::detail::abi::ExternalEncoderPlugin;
  using dicom::pixel::detail::abi::load_external_decoder_plugin;
  using dicom::pixel::detail::abi::load_external_encoder_plugin;

  ExternalDecoderPlugin decoder{};
  ExternalEncoderPlugin encoder{};
  std::string error{};

  expect_false(load_external_decoder_plugin(
                   "/definitely/not/existing/dicomsdl_decoder_plugin.so",
                   decoder, error),
      "decoder load invalid path");
  expect_not_empty(error, "decoder load error");

  error.clear();
  expect_false(load_external_encoder_plugin(
                   "/definitely/not/existing/dicomsdl_encoder_plugin.so",
                   encoder, error),
      "encoder load invalid path");
  expect_not_empty(error, "encoder load error");
}

void test_release_without_load_failure() {
  using dicom::pixel::detail::abi::ExternalDecoderPlugin;
  using dicom::pixel::detail::abi::ExternalEncoderPlugin;
  using dicom::pixel::detail::abi::release_external_decoder_plugin;
  using dicom::pixel::detail::abi::release_external_encoder_plugin;

  ExternalDecoderPlugin decoder{};
  ExternalEncoderPlugin encoder{};
  std::string error{};

  expect_false(release_external_decoder_plugin(decoder, error),
      "decoder release before load");
  expect_not_empty(error, "decoder release error");

  error.clear();
  expect_false(release_external_encoder_plugin(encoder, error),
      "encoder release before load");
  expect_not_empty(error, "encoder release error");
}

}  // namespace

int main() {
  test_invalid_library_path_load_failure();
  test_release_without_load_failure();
  return 0;
}
