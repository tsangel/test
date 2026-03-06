#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dicom.h"
#include "pixel/host/adapter/host_adapter_v2.hpp"

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

void expect_near(double actual, double expected, double tolerance, std::string_view label) {
  if (std::fabs(actual - expected) > tolerance) {
    fail(std::string(label) + " mismatch");
  }
}

std::string decoder_detail(const pixel::runtime_v2::HostDecoderContextV2& ctx) {
  char buffer[1024] = {};
  const uint32_t copied = pixel::runtime_v2::copy_host_decoder_last_error_detail_v2(
      &ctx, buffer, static_cast<uint32_t>(sizeof(buffer)));
  if (copied == 0) {
    return {};
  }
  return std::string(buffer, buffer + copied);
}

std::string encoder_detail(const pixel::runtime_v2::HostEncoderContextV2& ctx) {
  char buffer[1024] = {};
  const uint32_t copied = pixel::runtime_v2::copy_host_encoder_last_error_detail_v2(
      &ctx, buffer, static_cast<uint32_t>(sizeof(buffer)));
  if (copied == 0) {
    return {};
  }
  return std::string(buffer, buffer + copied);
}

}  // namespace

int main() {
  using namespace dicom::literals;

  pixel::runtime_v2::PluginRegistryV2 registry{};
  pixel::runtime_v2::init_builtin_registry_v2(&registry);

  pixel::runtime_v2::HostEncoderContextV2 encoder_ctx{};
  pixel::runtime_v2::HostDecoderContextV2 decoder_ctx{};
  pixel_option_kv_v2 ignored_option{};
  ignored_option.key = "ignored_option";
  ignored_option.value = "1";
  pixel_option_list_v2 ignored_options{};
  ignored_options.items = &ignored_option;
  ignored_options.count = 1u;

  auto ec = pixel::runtime_v2::configure_host_encoder_context_v2(
      &encoder_ctx, &registry, "ExplicitVRLittleEndian"_uid, &ignored_options);
  if (ec != PIXEL_CODEC_ERR_OK) {
    fail("configure_host_encoder_context_v2 failed: " + encoder_detail(encoder_ctx));
  }

  ec = pixel::runtime_v2::configure_host_decoder_context_v2(
      &decoder_ctx, &registry, "ExplicitVRLittleEndian"_uid, &ignored_options);
  if (ec != PIXEL_CODEC_ERR_OK) {
    fail("configure_host_decoder_context_v2 failed: " + decoder_detail(decoder_ctx));
  }

  std::vector<uint16_t> source_samples{10u, 11u, 12u, 13u, 14u, 15u};
  std::vector<uint8_t> source_bytes(source_samples.size() * sizeof(uint16_t), uint8_t{0});
  std::memcpy(
      source_bytes.data(), source_samples.data(), source_samples.size() * sizeof(uint16_t));

  dicom::pixel::PixelSource source{};
  source.bytes = source_bytes;
  source.data_type = dicom::pixel::DataType::u16;
  source.rows = 2;
  source.cols = 3;
  source.row_stride = 0;
  source.frames = 1;
  source.frame_stride = 0;
  source.samples_per_pixel = 1;
  source.planar = dicom::pixel::Planar::interleaved;
  source.photometric = dicom::pixel::Photometric::monochrome2;
  source.bits_stored = 16;

  std::vector<uint8_t> tiny_encoded(4u, uint8_t{0});
  uint64_t encoded_size = 0;
  ec = pixel::runtime_v2::encode_frame_with_host_context_v2(&encoder_ctx, &source,
      std::span<const uint8_t>(source_bytes), false,
      pixel_output_buffer_v2{tiny_encoded.data(), static_cast<uint64_t>(tiny_encoded.size())},
      &encoded_size);
  expect_eq(ec, PIXEL_CODEC_ERR_OUTPUT_TOO_SMALL,
      "encode_frame_with_host_context_v2 first pass status");
  expect_eq(encoded_size, static_cast<uint64_t>(source_bytes.size()),
      "encode_frame_with_host_context_v2 first pass required size");

  std::vector<uint8_t> encoded(static_cast<std::size_t>(encoded_size), uint8_t{0});
  uint64_t encoded_size_second = 0;
  ec = pixel::runtime_v2::encode_frame_with_host_context_v2(&encoder_ctx, &source,
      std::span<const uint8_t>(source_bytes), false,
      pixel_output_buffer_v2{encoded.data(), static_cast<uint64_t>(encoded.size())},
      &encoded_size_second);
  if (ec != PIXEL_CODEC_ERR_OK) {
    fail("encode_frame_with_host_context_v2 second pass failed: " +
        encoder_detail(encoder_ctx));
  }
  expect_eq(encoded_size_second, static_cast<uint64_t>(source_bytes.size()),
      "encode_frame_with_host_context_v2 second pass encoded size");
  expect_true(std::memcmp(encoded.data(), source_bytes.data(), source_bytes.size()) == 0,
      "native encoded payload equality");

  dicom::pixel::PixelDataInfo info{};
  info.ts = "ExplicitVRLittleEndian"_uid;
  info.sv_dtype = dicom::pixel::DataType::u16;
  info.rows = 2;
  info.cols = 3;
  info.frames = 1;
  info.samples_per_pixel = 1;
  info.planar_configuration = dicom::pixel::Planar::interleaved;
  info.bits_stored = 16;
  info.has_pixel_data = true;

  dicom::pixel::DecodeStrides int_strides{};
  int_strides.row = 3u * sizeof(uint16_t);
  int_strides.frame = 2u * int_strides.row;

  dicom::pixel::DecodeOptions decode_options{};
  decode_options.planar_out = dicom::pixel::Planar::interleaved;
  decode_options.to_modality_value = false;
  decode_options.decode_mct = false;

  std::vector<uint8_t> decoded_int(source_bytes.size(), uint8_t{0});
  ec = pixel::runtime_v2::decode_frame_with_host_context_v2(&decoder_ctx, &info,
      std::span<const uint8_t>(encoded), std::span<uint8_t>(decoded_int), &int_strides,
      &decode_options, nullptr);
  if (ec != PIXEL_CODEC_ERR_OK) {
    fail("decode_frame_with_host_context_v2 integer decode failed: " +
        decoder_detail(decoder_ctx));
  }
  expect_true(std::memcmp(decoded_int.data(), source_bytes.data(), source_bytes.size()) == 0,
      "native integer decode equality");

  dicom::pixel::DecodeStrides float_strides{};
  float_strides.row = 3u * sizeof(float);
  float_strides.frame = 2u * float_strides.row;

  std::vector<uint8_t> decoded_rescale(float_strides.frame, uint8_t{0});
  pixel::runtime_v2::HostModalityValueTransformV2 rescale{};
  rescale.kind = pixel::runtime_v2::HostModalityValueTransformKindV2::kRescale;
  rescale.rescale_slope = 2.0;
  rescale.rescale_intercept = -1.0;

  ec = pixel::runtime_v2::decode_frame_with_host_context_v2(&decoder_ctx, &info,
      std::span<const uint8_t>(encoded), std::span<uint8_t>(decoded_rescale),
      &float_strides, &decode_options, &rescale);
  if (ec != PIXEL_CODEC_ERR_OK) {
    fail("decode_frame_with_host_context_v2 rescale decode failed: " +
        decoder_detail(decoder_ctx));
  }
  const float* rescale_values = reinterpret_cast<const float*>(decoded_rescale.data());
  for (std::size_t i = 0; i < source_samples.size(); ++i) {
    const double expected = static_cast<double>(source_samples[i]) * 2.0 - 1.0;
    expect_near(rescale_values[i], expected, 1e-5, "rescale sample");
  }

  dicom::pixel::ModalityLut modality_lut{};
  modality_lut.first_mapped = 10;
  modality_lut.values = {100.0f, 101.0f, 102.0f, 103.0f, 104.0f, 105.0f};

  std::vector<uint8_t> decoded_lut(float_strides.frame, uint8_t{0});
  pixel::runtime_v2::HostModalityValueTransformV2 lut_transform{};
  lut_transform.kind = pixel::runtime_v2::HostModalityValueTransformKindV2::kModalityLut;
  lut_transform.modality_lut = &modality_lut;

  ec = pixel::runtime_v2::decode_frame_with_host_context_v2(&decoder_ctx, &info,
      std::span<const uint8_t>(encoded), std::span<uint8_t>(decoded_lut), &float_strides,
      &decode_options, &lut_transform);
  if (ec != PIXEL_CODEC_ERR_OK) {
    fail("decode_frame_with_host_context_v2 LUT decode failed: " +
        decoder_detail(decoder_ctx));
  }
  const float* lut_values = reinterpret_cast<const float*>(decoded_lut.data());
  for (std::size_t i = 0; i < source_samples.size(); ++i) {
    const std::size_t lut_index = static_cast<std::size_t>(source_samples[i] - 10u);
    expect_near(lut_values[i], modality_lut.values[lut_index], 1e-5, "modality LUT sample");
  }

  pixel::runtime_v2::destroy_host_decoder_context_v2(&decoder_ctx);
  pixel::runtime_v2::destroy_host_encoder_context_v2(&encoder_ctx);
  return 0;
}
