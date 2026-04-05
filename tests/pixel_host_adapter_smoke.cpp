#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dicom.h"
#include "pixel/host/adapter/host_adapter.hpp"

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

std::string decoder_detail(const pixel::runtime::HostDecoderContext& ctx) {
  char buffer[1024] = {};
  const uint32_t copied = pixel::runtime::copy_host_decoder_last_error_detail(
      &ctx, buffer, static_cast<uint32_t>(sizeof(buffer)));
  if (copied == 0) {
    return {};
  }
  return std::string(buffer, buffer + copied);
}

std::string encoder_detail(const pixel::runtime::HostEncoderContext& ctx) {
  char buffer[1024] = {};
  const uint32_t copied = pixel::runtime::copy_host_encoder_last_error_detail(
      &ctx, buffer, static_cast<uint32_t>(sizeof(buffer)));
  if (copied == 0) {
    return {};
  }
  return std::string(buffer, buffer + copied);
}

}  // namespace

int main() {
  using namespace dicom::literals;

  pixel::runtime::BindingRegistry registry{};
  pixel::runtime::init_builtin_registry(&registry);

  pixel::runtime::HostEncoderContext encoder_ctx{};
  pixel::runtime::HostDecoderContext decoder_ctx{};
  pixel_option_kv ignored_option{};
  ignored_option.key = "ignored_option";
  ignored_option.value = "1";
  pixel_option_list ignored_options{};
  ignored_options.items = &ignored_option;
  ignored_options.count = 1u;

  auto ec = pixel::runtime::configure_host_encoder_context(
      &encoder_ctx, &registry, "ExplicitVRLittleEndian"_uid, &ignored_options);
  if (ec != PIXEL_CODEC_ERR_OK) {
    fail("configure_host_encoder_context failed: " + encoder_detail(encoder_ctx));
  }

  ec = pixel::runtime::configure_host_decoder_context(
      &decoder_ctx, &registry, "ExplicitVRLittleEndian"_uid, &ignored_options);
  if (ec != PIXEL_CODEC_ERR_OK) {
    fail("configure_host_decoder_context failed: " + decoder_detail(decoder_ctx));
  }

  std::vector<uint16_t> source_samples{10u, 11u, 12u, 13u, 14u, 15u};
  std::vector<uint8_t> source_bytes(source_samples.size() * sizeof(uint16_t), uint8_t{0});
  std::memcpy(
      source_bytes.data(), source_samples.data(), source_samples.size() * sizeof(uint16_t));

  const dicom::pixel::PixelLayout source_layout{
      .data_type = dicom::pixel::DataType::u16,
      .photometric = dicom::pixel::Photometric::monochrome2,
      .planar = dicom::pixel::Planar::interleaved,
      .reserved = 0,
      .rows = 2,
      .cols = 3,
      .frames = 1,
      .samples_per_pixel = 1,
      .bits_stored = 16,
      .row_stride = 3u * sizeof(uint16_t),
      .frame_stride = 2u * 3u * sizeof(uint16_t),
  };

  std::vector<uint8_t> tiny_encoded(4u, uint8_t{0});
  uint64_t encoded_size = 0;
  ec = pixel::runtime::encode_frame_with_host_context(&encoder_ctx, &source_layout,
      std::span<const uint8_t>(source_bytes), false,
      pixel_output_buffer{tiny_encoded.data(), static_cast<uint64_t>(tiny_encoded.size())},
      &encoded_size);
  expect_eq(ec, PIXEL_CODEC_ERR_OUTPUT_TOO_SMALL,
      "encode_frame_with_host_context first pass status");
  expect_eq(encoded_size, static_cast<uint64_t>(source_bytes.size()),
      "encode_frame_with_host_context first pass required size");

  std::vector<uint8_t> encoded(static_cast<std::size_t>(encoded_size), uint8_t{0});
  uint64_t encoded_size_second = 0;
  ec = pixel::runtime::encode_frame_with_host_context(&encoder_ctx, &source_layout,
      std::span<const uint8_t>(source_bytes), false,
      pixel_output_buffer{encoded.data(), static_cast<uint64_t>(encoded.size())},
      &encoded_size_second);
  if (ec != PIXEL_CODEC_ERR_OK) {
    fail("encode_frame_with_host_context second pass failed: " +
        encoder_detail(encoder_ctx));
  }
  expect_eq(encoded_size_second, static_cast<uint64_t>(source_bytes.size()),
      "encode_frame_with_host_context second pass encoded size");
  expect_true(std::memcmp(encoded.data(), source_bytes.data(), source_bytes.size()) == 0,
      "native encoded payload equality");

  const dicom::pixel::PixelLayout source_decode_layout{
      .data_type = dicom::pixel::DataType::u16,
      .photometric = dicom::pixel::Photometric::monochrome2,
      .planar = dicom::pixel::Planar::interleaved,
      .reserved = 0,
      .rows = 2,
      .cols = 3,
      .frames = 1,
      .samples_per_pixel = 1,
      .bits_stored = 16,
      .row_stride = 0,
      .frame_stride = 0,
  };

  const dicom::pixel::PixelLayout int_layout{
      .data_type = dicom::pixel::DataType::u16,
      .photometric = dicom::pixel::Photometric::monochrome2,
      .planar = dicom::pixel::Planar::interleaved,
      .reserved = 0,
      .rows = 2,
      .cols = 3,
      .frames = 1,
      .samples_per_pixel = 1,
      .bits_stored = 16,
      .row_stride = 3u * sizeof(uint16_t),
      .frame_stride = 2u * 3u * sizeof(uint16_t),
  };

  dicom::pixel::DecodeOptions decode_options{};
  decode_options.decode_mct = false;

  std::vector<uint8_t> decoded_int(source_bytes.size(), uint8_t{0});
  pixel_decoder_info decode_info{};
  decode_info.struct_size = sizeof(pixel_decoder_info);
  decode_info.abi_version = PIXEL_DECODER_PLUGIN_ABI;
  ec = pixel::runtime::decode_frame_with_host_context(&decoder_ctx, &source_decode_layout,
      std::span<const uint8_t>(encoded), std::span<uint8_t>(decoded_int), &int_layout,
      &decode_options, &decode_info);
  if (ec != PIXEL_CODEC_ERR_OK) {
    fail("decode_frame_with_host_context integer decode failed: " +
        decoder_detail(decoder_ctx));
  }
  expect_true(std::memcmp(decoded_int.data(), source_bytes.data(), source_bytes.size()) == 0,
      "native integer decode equality");
  expect_eq(decode_info.actual_color_space,
      static_cast<uint8_t>(PIXEL_DECODED_COLOR_SPACE_MONOCHROME),
      "host decode actual color space");
  expect_eq(decode_info.encoded_lossy_state,
      static_cast<uint8_t>(PIXEL_ENCODED_LOSSY_STATE_LOSSLESS),
      "host decode encoded lossy state");
  expect_eq(decode_info.actual_dtype, static_cast<uint8_t>(PIXEL_DTYPE_U16),
      "host decode actual dtype");
  expect_eq(decode_info.actual_planar,
      static_cast<uint8_t>(PIXEL_DECODED_PLANAR_INTERLEAVED),
      "host decode actual planar");
  expect_eq(decode_info.bits_per_sample, static_cast<uint16_t>(16),
      "host decode bits per sample");

  pixel::runtime::destroy_host_decoder_context(&decoder_ctx);
  pixel::runtime::destroy_host_encoder_context(&encoder_ctx);
  return 0;
}

