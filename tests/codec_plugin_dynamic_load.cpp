#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include <dicom.h>

#include "../src/pixel/registry/codec_registry.hpp"

namespace {

using namespace dicom::literals;

[[noreturn]] void fail(const std::string& message) {
  std::cerr << message << std::endl;
  std::exit(1);
}

void expect_true(bool value, std::string_view label) {
  if (!value) {
    fail(std::string(label) + " expected true");
  }
}

void expect_eq(
    std::string_view actual, std::string_view expected, std::string_view label) {
  if (actual != expected) {
    fail(std::string(label) + " mismatch");
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
  using dicom::pixel::detail::CodecDecodeFrameInput;
  using dicom::pixel::detail::CodecEncodeFrameInput;
  using dicom::pixel::detail::CodecError;
  using dicom::pixel::detail::global_codec_registry;

  if (argc < 2 || argv[1] == nullptr) {
    fail("plugin path argument is required");
  }
  const std::string plugin_library_path = argv[1];

  auto& registry = global_codec_registry();
  const auto* jpeg_plugin = registry.find_plugin("jpeg");
  if (!jpeg_plugin) {
    fail("jpeg plugin is not registered");
  }
  const auto original_decode = jpeg_plugin->decode_frame;
  const auto original_encode = jpeg_plugin->encode_frame;

  std::string plugin_key{};
  std::string error{};
  expect_true(dicom::pixel::register_external_codec_plugin_from_library(
                  plugin_library_path, &plugin_key, &error),
      "register external codec plugin from library");
  expect_eq(plugin_key, std::string_view("jpeg"),
      "loaded plugin key");
  expect_true(error.empty(), "register external codec plugin error is empty");

  jpeg_plugin = registry.find_plugin("jpeg");
  expect_true(jpeg_plugin != nullptr, "jpeg plugin exists after dynamic load");
  expect_true(jpeg_plugin->decode_frame != original_decode,
      "dynamic load decode dispatch override");
  expect_true(jpeg_plugin->encode_frame != original_encode,
      "dynamic load encode dispatch override");

  std::vector<std::uint8_t> decode_source{0x00, 0x01};
  std::vector<std::uint8_t> decode_destination(8, 0);
  CodecDecodeFrameInput decode_input{
      .info = dicom::pixel::PixelDataInfo{
          .ts = "JPEGBaseline8Bit"_uid,
          .sv_dtype = dicom::pixel::DataType::u8,
          .rows = 1,
          .cols = 1,
          .frames = 1,
          .samples_per_pixel = 1,
          .planar_configuration = dicom::pixel::Planar::interleaved,
          .bits_stored = 8,
          .has_pixel_data = true,
      },
      .prepared_source = std::span<const std::uint8_t>(decode_source),
      .destination = std::span<std::uint8_t>(decode_destination),
      .destination_strides = dicom::pixel::DecodeStrides{.row = 1, .frame = 1},
      .options = dicom::pixel::DecodeOptions{},
  };
  CodecError decode_error{};
  expect_true(jpeg_plugin->decode_frame(decode_input, decode_error),
      "dynamic load decode frame");
  expect_eq(decode_destination[0], static_cast<std::uint8_t>(0x5c),
      "dynamic load decode marker");

  std::vector<std::uint8_t> encode_source(64, 0x33);
  CodecEncodeFrameInput encode_input{
      .source_frame = std::span<const std::uint8_t>(encode_source),
      .transfer_syntax = "JPEGBaseline8Bit"_uid,
      .rows = 8,
      .cols = 8,
      .samples_per_pixel = 1,
      .bytes_per_sample = 1,
      .bits_allocated = 8,
      .bits_stored = 8,
      .pixel_representation = 0,
      .use_multicomponent_transform = false,
      .source_planar = dicom::pixel::Planar::interleaved,
      .planar_source = false,
      .row_payload_bytes = 8,
      .source_row_stride = 8,
      .source_plane_stride = 64,
      .source_frame_size_bytes = 64,
      .destination_frame_payload = 1,
      .profile = dicom::pixel::detail::CodecProfile::jpeg_lossy,
  };
  dicom::pixel::detail::codec_option_pairs encode_options{
      dicom::pixel::detail::CodecOptionKv{
          .key = "quality",
          .value = dicom::pixel::detail::codec_option_value{
              static_cast<std::int64_t>(90)},
      },
  };
  CodecError encode_error{};
  std::vector<std::uint8_t> encoded_frame{};
  expect_true(jpeg_plugin->encode_frame(
                  encode_input, std::span<const dicom::pixel::detail::CodecOptionKv>(encode_options),
                  encoded_frame, encode_error),
      "dynamic load encode frame");
  expect_eq(encoded_frame.size(), std::size_t{4},
      "dynamic load encoded payload size");
  expect_eq(encoded_frame[0], static_cast<std::uint8_t>(0xde),
      "dynamic load encoded payload byte 0");
  expect_eq(encoded_frame[1], static_cast<std::uint8_t>(0xad),
      "dynamic load encoded payload byte 1");
  expect_eq(encoded_frame[2], static_cast<std::uint8_t>(0xbe),
      "dynamic load encoded payload byte 2");
  expect_eq(encoded_frame[3], static_cast<std::uint8_t>(0xef),
      "dynamic load encoded payload byte 3");

  error.clear();
  expect_true(dicom::pixel::unregister_external_codec_plugin("jpeg", &error),
      "unregister external dynamic plugin");
  expect_true(error.empty(), "unregister external dynamic plugin error is empty");

  jpeg_plugin = registry.find_plugin("jpeg");
  expect_true(jpeg_plugin != nullptr, "jpeg plugin exists after dynamic unload");
  expect_true(jpeg_plugin->decode_frame == original_decode,
      "dynamic load decode dispatch restore");
  expect_true(jpeg_plugin->encode_frame == original_encode,
      "dynamic load encode dispatch restore");

  return 0;
}
