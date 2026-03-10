#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "builtin_api.hpp"

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

std::string encoder_error_detail(const pixel_encoder_plugin_api_v2& api, const void* ctx) {
  if (api.copy_last_error_detail == nullptr) {
    return {};
  }
  char buffer[1024] = {};
  const uint32_t copied = api.copy_last_error_detail(
      ctx, buffer, static_cast<uint32_t>(sizeof(buffer)));
  if (copied == 0) {
    return {};
  }
  return std::string(buffer);
}

std::string decoder_error_detail(const pixel_decoder_plugin_api_v2& api, const void* ctx) {
  if (api.copy_last_error_detail == nullptr) {
    return {};
  }
  char buffer[1024] = {};
  const uint32_t copied = api.copy_last_error_detail(
      ctx, buffer, static_cast<uint32_t>(sizeof(buffer)));
  if (copied == 0) {
    return {};
  }
  return std::string(buffer);
}

struct EncoderContextGuard {
  const pixel_encoder_plugin_api_v2* api{nullptr};
  void* context{nullptr};

  ~EncoderContextGuard() {
    if (api != nullptr && api->destroy != nullptr && context != nullptr) {
      api->destroy(context);
    }
  }
};

struct DecoderContextGuard {
  const pixel_decoder_plugin_api_v2* api{nullptr};
  void* context{nullptr};

  ~DecoderContextGuard() {
    if (api != nullptr && api->destroy != nullptr && context != nullptr) {
      api->destroy(context);
    }
  }
};

pixel_encoder_request_v2 make_encoder_request(
    std::vector<uint8_t>& source, pixel_output_buffer_v2 encoded_buffer, uint64_t encoded_size) {
  pixel_encoder_request_v2 request{};
  request.struct_size = sizeof(pixel_encoder_request_v2);
  request.abi_version = PIXEL_ENCODER_PLUGIN_ABI_V2;

  request.source.struct_size = sizeof(pixel_encoder_source_v2);
  request.source.abi_version = PIXEL_ENCODER_PLUGIN_ABI_V2;
  request.source.source_buffer.data = source.data();
  request.source.source_buffer.size = source.size();

  request.frame.struct_size = sizeof(pixel_encoder_frame_info_v2);
  request.frame.abi_version = PIXEL_ENCODER_PLUGIN_ABI_V2;
  request.frame.codec_profile_code = PIXEL_CODEC_PROFILE_RLE_LOSSLESS_V2;
  request.frame.source_dtype = PIXEL_DTYPE_U16_V2;
  request.frame.source_planar = PIXEL_PLANAR_INTERLEAVED_V2;
  request.frame.rows = 16;
  request.frame.cols = 16;
  request.frame.samples_per_pixel = 1;
  request.frame.bits_allocated = 16;
  request.frame.bits_stored = 16;
  request.frame.pixel_representation = 0;
  request.frame.source_row_stride = 0;
  request.frame.source_plane_stride = 0;
  request.frame.source_frame_size_bytes = 0;
  request.frame.use_multicomponent_transform = 0;

  request.output.struct_size = sizeof(pixel_encoder_output_v2);
  request.output.abi_version = PIXEL_ENCODER_PLUGIN_ABI_V2;
  request.output.encoded_buffer = encoded_buffer;
  request.output.encoded_size = encoded_size;

  return request;
}

pixel_decoder_request_v2 make_decoder_request(
    std::vector<uint8_t>& encoded, std::vector<uint8_t>& decoded) {
  pixel_decoder_request_v2 request{};
  request.struct_size = sizeof(pixel_decoder_request_v2);
  request.abi_version = PIXEL_DECODER_PLUGIN_ABI_V2;

  request.source.struct_size = sizeof(pixel_decoder_source_v2);
  request.source.abi_version = PIXEL_DECODER_PLUGIN_ABI_V2;
  request.source.source_buffer.data = encoded.data();
  request.source.source_buffer.size = encoded.size();

  request.frame.struct_size = sizeof(pixel_decoder_frame_info_v2);
  request.frame.abi_version = PIXEL_DECODER_PLUGIN_ABI_V2;
  request.frame.codec_profile_code = PIXEL_CODEC_PROFILE_RLE_LOSSLESS_V2;
  request.frame.source_dtype = PIXEL_DTYPE_U16_V2;
  request.frame.source_planar = PIXEL_PLANAR_INTERLEAVED_V2;
  request.frame.rows = 16;
  request.frame.cols = 16;
  request.frame.samples_per_pixel = 1;
  request.frame.bits_stored = 16;
  request.frame.decode_mct = 0;

  request.output.struct_size = sizeof(pixel_decoder_output_v2);
  request.output.abi_version = PIXEL_DECODER_PLUGIN_ABI_V2;
  request.output.dst = decoded.data();
  request.output.dst_size = decoded.size();
  request.output.row_stride = 0;
  request.output.frame_stride = 0;
  request.output.dst_dtype = PIXEL_DTYPE_U16_V2;
  request.output.dst_planar = PIXEL_PLANAR_INTERLEAVED_V2;

  request.value_transform.struct_size = sizeof(pixel_decoder_value_transform_v2);
  request.value_transform.abi_version = PIXEL_DECODER_PLUGIN_ABI_V2;
  request.value_transform.transform_kind = PIXEL_DECODER_VALUE_TRANSFORM_NONE_V2;

  return request;
}

pixel_encoder_request_v2 make_rgb8_encoder_request(
    std::vector<uint8_t>& source, pixel_output_buffer_v2 encoded_buffer, uint64_t encoded_size) {
  pixel_encoder_request_v2 request{};
  request.struct_size = sizeof(pixel_encoder_request_v2);
  request.abi_version = PIXEL_ENCODER_PLUGIN_ABI_V2;

  request.source.struct_size = sizeof(pixel_encoder_source_v2);
  request.source.abi_version = PIXEL_ENCODER_PLUGIN_ABI_V2;
  request.source.source_buffer.data = source.data();
  request.source.source_buffer.size = source.size();

  request.frame.struct_size = sizeof(pixel_encoder_frame_info_v2);
  request.frame.abi_version = PIXEL_ENCODER_PLUGIN_ABI_V2;
  request.frame.codec_profile_code = PIXEL_CODEC_PROFILE_RLE_LOSSLESS_V2;
  request.frame.source_dtype = PIXEL_DTYPE_U8_V2;
  request.frame.source_planar = PIXEL_PLANAR_INTERLEAVED_V2;
  request.frame.rows = 8;
  request.frame.cols = 8;
  request.frame.samples_per_pixel = 3;
  request.frame.bits_allocated = 8;
  request.frame.bits_stored = 8;
  request.frame.pixel_representation = 0;
  request.frame.source_row_stride = 0;
  request.frame.source_plane_stride = 0;
  request.frame.source_frame_size_bytes = 0;
  request.frame.use_multicomponent_transform = 0;

  request.output.struct_size = sizeof(pixel_encoder_output_v2);
  request.output.abi_version = PIXEL_ENCODER_PLUGIN_ABI_V2;
  request.output.encoded_buffer = encoded_buffer;
  request.output.encoded_size = encoded_size;

  return request;
}

pixel_decoder_request_v2 make_rgb8_decoder_request(
    std::vector<uint8_t>& encoded, std::vector<uint8_t>& decoded) {
  pixel_decoder_request_v2 request{};
  request.struct_size = sizeof(pixel_decoder_request_v2);
  request.abi_version = PIXEL_DECODER_PLUGIN_ABI_V2;

  request.source.struct_size = sizeof(pixel_decoder_source_v2);
  request.source.abi_version = PIXEL_DECODER_PLUGIN_ABI_V2;
  request.source.source_buffer.data = encoded.data();
  request.source.source_buffer.size = encoded.size();

  request.frame.struct_size = sizeof(pixel_decoder_frame_info_v2);
  request.frame.abi_version = PIXEL_DECODER_PLUGIN_ABI_V2;
  request.frame.codec_profile_code = PIXEL_CODEC_PROFILE_RLE_LOSSLESS_V2;
  request.frame.source_dtype = PIXEL_DTYPE_U8_V2;
  request.frame.source_planar = PIXEL_PLANAR_INTERLEAVED_V2;
  request.frame.rows = 8;
  request.frame.cols = 8;
  request.frame.samples_per_pixel = 3;
  request.frame.bits_stored = 8;
  request.frame.decode_mct = 0;

  request.output.struct_size = sizeof(pixel_decoder_output_v2);
  request.output.abi_version = PIXEL_DECODER_PLUGIN_ABI_V2;
  request.output.dst = decoded.data();
  request.output.dst_size = decoded.size();
  request.output.row_stride = 0;
  request.output.frame_stride = 0;
  request.output.dst_dtype = PIXEL_DTYPE_U8_V2;
  request.output.dst_planar = PIXEL_PLANAR_INTERLEAVED_V2;

  request.value_transform.struct_size = sizeof(pixel_decoder_value_transform_v2);
  request.value_transform.abi_version = PIXEL_DECODER_PLUGIN_ABI_V2;
  request.value_transform.transform_kind = PIXEL_DECODER_VALUE_TRANSFORM_NONE_V2;

  return request;
}

}  // namespace

int main() {
  const auto& encoder_api = pixel::rle_codec_v2::encoder_builtin_api();
  const auto& decoder_api = pixel::rle_codec_v2::decoder_builtin_api();

  EncoderContextGuard encoder_ctx{};
  encoder_ctx.api = &encoder_api;
  encoder_ctx.context = encoder_api.create();
  expect_true(encoder_ctx.context != nullptr, "encoder create");

  const auto encoder_configure_ec =
      encoder_api.configure(encoder_ctx.context, PIXEL_CODEC_PROFILE_RLE_LOSSLESS_V2, nullptr);
  if (encoder_configure_ec != PIXEL_CODEC_ERR_OK) {
    fail("encoder configure failed: " +
        encoder_error_detail(encoder_api, encoder_ctx.context));
  }

  std::vector<uint8_t> source(16u * 16u * sizeof(uint16_t), uint8_t{0});
  for (std::size_t i = 0; i < 16u * 16u; ++i) {
    const uint16_t value = static_cast<uint16_t>((i * 37u + 11u) & 0xFFFFu);
    std::memcpy(source.data() + i * sizeof(uint16_t), &value, sizeof(value));
  }

  pixel_output_buffer_v2 tiny_output{};
  auto first_encode_request = make_encoder_request(source, tiny_output, 0);
  const auto first_encode_ec =
      encoder_api.encode_frame(encoder_ctx.context, &first_encode_request);
  expect_eq(first_encode_ec, PIXEL_CODEC_ERR_OUTPUT_TOO_SMALL,
      "first encode should report output too small");
  expect_true(first_encode_request.output.encoded_size > 0,
      "first encode returns required encoded size");

  std::vector<uint8_t> encoded(first_encode_request.output.encoded_size);
  pixel_output_buffer_v2 encoded_output{};
  encoded_output.data = encoded.data();
  encoded_output.size = encoded.size();

  auto second_encode_request = make_encoder_request(source, encoded_output, 0);
  const auto second_encode_ec =
      encoder_api.encode_frame(encoder_ctx.context, &second_encode_request);
  if (second_encode_ec != PIXEL_CODEC_ERR_OK) {
    fail("second encode failed: " + encoder_error_detail(encoder_api, encoder_ctx.context));
  }
  expect_true(second_encode_request.output.encoded_size > 0,
      "second encode wrote non-empty codestream");
  expect_true(second_encode_request.output.encoded_size <= encoded.size(),
      "second encode size fits output buffer");
  encoded.resize(static_cast<std::size_t>(second_encode_request.output.encoded_size));

  DecoderContextGuard decoder_ctx{};
  decoder_ctx.api = &decoder_api;
  decoder_ctx.context = decoder_api.create();
  expect_true(decoder_ctx.context != nullptr, "decoder create");

  const auto decoder_configure_ec =
      decoder_api.configure(decoder_ctx.context, PIXEL_CODEC_PROFILE_RLE_LOSSLESS_V2, nullptr);
  if (decoder_configure_ec != PIXEL_CODEC_ERR_OK) {
    fail("decoder configure failed: " + decoder_error_detail(decoder_api, decoder_ctx.context));
  }

  std::vector<uint8_t> decoded(source.size(), uint8_t{0});
  auto decode_request = make_decoder_request(encoded, decoded);
  const auto decode_ec = decoder_api.decode_frame(decoder_ctx.context, &decode_request);
  if (decode_ec != PIXEL_CODEC_ERR_OK) {
    fail("decode failed: " + decoder_error_detail(decoder_api, decoder_ctx.context));
  }

  expect_eq(decoded.size(), source.size(), "decoded size");
  expect_true(std::memcmp(decoded.data(), source.data(), source.size()) == 0,
      "RLE lossless round-trip data equality");

  std::vector<uint8_t> decoded_rescale(16u * 16u * sizeof(float), uint8_t{0});
  auto decode_rescale_request = make_decoder_request(encoded, decoded_rescale);
  decode_rescale_request.output.dst_dtype = PIXEL_DTYPE_F32_V2;
  decode_rescale_request.value_transform.transform_kind =
      PIXEL_DECODER_VALUE_TRANSFORM_RESCALE_V2;
  decode_rescale_request.value_transform.rescale_slope = 0.5;
  decode_rescale_request.value_transform.rescale_intercept = -1.0;
  const auto decode_rescale_ec =
      decoder_api.decode_frame(decoder_ctx.context, &decode_rescale_request);
  if (decode_rescale_ec != PIXEL_CODEC_ERR_OK) {
    fail("decode rescale failed: " + decoder_error_detail(decoder_api, decoder_ctx.context));
  }
  const float* decoded_rescale_values =
      reinterpret_cast<const float*>(decoded_rescale.data());
  for (std::size_t i = 0; i < 16u * 16u; ++i) {
    uint16_t source_value = 0;
    std::memcpy(&source_value, source.data() + i * sizeof(uint16_t), sizeof(source_value));
    const double expected_value = static_cast<double>(source_value) * 0.5 - 1.0;
    expect_near(decoded_rescale_values[i], expected_value, 1e-5,
        "RLE rescale decoded sample");
  }

  std::vector<uint8_t> rgb_source(8u * 8u * 3u, uint8_t{0});
  for (std::size_t i = 0; i < rgb_source.size(); ++i) {
    rgb_source[i] = static_cast<uint8_t>((i * 29u + 7u) & 0xFFu);
  }

  pixel_output_buffer_v2 rgb_tiny_output{};
  auto first_rgb_encode_request =
      make_rgb8_encoder_request(rgb_source, rgb_tiny_output, 0);
  const auto first_rgb_encode_ec =
      encoder_api.encode_frame(encoder_ctx.context, &first_rgb_encode_request);
  expect_eq(first_rgb_encode_ec, PIXEL_CODEC_ERR_OUTPUT_TOO_SMALL,
      "RGB encode should report output too small");
  expect_true(first_rgb_encode_request.output.encoded_size > 0,
      "RGB encode returns required encoded size");

  std::vector<uint8_t> rgb_encoded(first_rgb_encode_request.output.encoded_size);
  pixel_output_buffer_v2 rgb_output{};
  rgb_output.data = rgb_encoded.data();
  rgb_output.size = rgb_encoded.size();

  auto second_rgb_encode_request =
      make_rgb8_encoder_request(rgb_source, rgb_output, 0);
  const auto second_rgb_encode_ec =
      encoder_api.encode_frame(encoder_ctx.context, &second_rgb_encode_request);
  if (second_rgb_encode_ec != PIXEL_CODEC_ERR_OK) {
    fail("RGB encode failed: " + encoder_error_detail(encoder_api, encoder_ctx.context));
  }
  rgb_encoded.resize(
      static_cast<std::size_t>(second_rgb_encode_request.output.encoded_size));

  std::vector<uint8_t> rgb_decoded(rgb_source.size(), uint8_t{0});
  auto rgb_decode_request = make_rgb8_decoder_request(rgb_encoded, rgb_decoded);
  const auto rgb_decode_ec =
      decoder_api.decode_frame(decoder_ctx.context, &rgb_decode_request);
  if (rgb_decode_ec != PIXEL_CODEC_ERR_OK) {
    fail("RGB decode failed: " + decoder_error_detail(decoder_api, decoder_ctx.context));
  }
  expect_true(std::memcmp(rgb_decoded.data(), rgb_source.data(), rgb_source.size()) == 0,
      "RLE RGB round-trip data equality");

  return 0;
}
