#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "direct_api_v2.hpp"

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

std::string error_detail(const pixel::core_v2::ErrorState& state) {
  char buffer[1024] = {};
  const uint32_t copied = pixel::core_v2::copy_last_error_detail(
      &state, buffer, static_cast<uint32_t>(sizeof(buffer)));
  if (copied == 0) {
    return {};
  }
  return std::string(buffer);
}

pixel_encoder_request_v2 make_encode_request(
    std::vector<uint8_t>& source, uint32_t profile_code, uint64_t source_row_stride,
    pixel_output_buffer_v2 encoded_buffer, uint64_t encoded_size) {
  pixel_encoder_request_v2 request{};
  request.struct_size = sizeof(pixel_encoder_request_v2);
  request.abi_version = PIXEL_ENCODER_PLUGIN_ABI_V2;

  request.source.struct_size = sizeof(pixel_encoder_source_v2);
  request.source.abi_version = PIXEL_ENCODER_PLUGIN_ABI_V2;
  request.source.source_buffer.data = source.data();
  request.source.source_buffer.size = source.size();

  request.frame.struct_size = sizeof(pixel_encoder_frame_info_v2);
  request.frame.abi_version = PIXEL_ENCODER_PLUGIN_ABI_V2;
  request.frame.codec_profile_code = profile_code;
  request.frame.source_dtype = PIXEL_DTYPE_U8_V2;
  request.frame.source_planar = PIXEL_PLANAR_INTERLEAVED_V2;
  request.frame.rows = 8;
  request.frame.cols = 8;
  request.frame.samples_per_pixel = 1;
  request.frame.bits_allocated = 8;
  request.frame.bits_stored = 8;
  request.frame.pixel_representation = 0;
  request.frame.source_row_stride = source_row_stride;
  request.frame.source_plane_stride = 0;
  request.frame.source_frame_size_bytes = source.size();
  request.frame.use_multicomponent_transform = 0;

  request.output.struct_size = sizeof(pixel_encoder_output_v2);
  request.output.abi_version = PIXEL_ENCODER_PLUGIN_ABI_V2;
  request.output.encoded_buffer = encoded_buffer;
  request.output.encoded_size = encoded_size;

  return request;
}

pixel_decoder_request_v2 make_decode_request(
    std::vector<uint8_t>& encoded, std::vector<uint8_t>& decoded, uint32_t profile_code,
    uint8_t dst_dtype = PIXEL_DTYPE_U8_V2) {
  pixel_decoder_request_v2 request{};
  request.struct_size = sizeof(pixel_decoder_request_v2);
  request.abi_version = PIXEL_DECODER_PLUGIN_ABI_V2;

  request.source.struct_size = sizeof(pixel_decoder_source_v2);
  request.source.abi_version = PIXEL_DECODER_PLUGIN_ABI_V2;
  request.source.source_buffer.data = encoded.data();
  request.source.source_buffer.size = encoded.size();

  request.frame.struct_size = sizeof(pixel_decoder_frame_info_v2);
  request.frame.abi_version = PIXEL_DECODER_PLUGIN_ABI_V2;
  request.frame.codec_profile_code = profile_code;
  request.frame.source_dtype = PIXEL_DTYPE_U8_V2;
  request.frame.source_planar = PIXEL_PLANAR_INTERLEAVED_V2;
  request.frame.rows = 8;
  request.frame.cols = 8;
  request.frame.samples_per_pixel = 1;
  request.frame.bits_stored = 8;
  request.frame.decode_mct = 0;

  request.output.struct_size = sizeof(pixel_decoder_output_v2);
  request.output.abi_version = PIXEL_DECODER_PLUGIN_ABI_V2;
  request.output.dst = decoded.data();
  request.output.dst_size = decoded.size();
  request.output.row_stride = 0;
  request.output.frame_stride = 0;
  request.output.dst_dtype = dst_dtype;
  request.output.dst_planar = PIXEL_PLANAR_INTERLEAVED_V2;

  request.value_transform.struct_size = sizeof(pixel_decoder_value_transform_v2);
  request.value_transform.abi_version = PIXEL_DECODER_PLUGIN_ABI_V2;
  request.value_transform.transform_kind = PIXEL_DECODER_VALUE_TRANSFORM_NONE_V2;

  return request;
}

}  // namespace

int main() {
  pixel::core_v2::ErrorState core_error{};

  const std::size_t rows = 8;
  const std::size_t cols = 8;
  const std::size_t source_row_stride = 12;
  std::vector<uint8_t> source(rows * source_row_stride, uint8_t{0});
  std::vector<uint8_t> expected(rows * cols, uint8_t{0});
  for (std::size_t r = 0; r < rows; ++r) {
    for (std::size_t c = 0; c < cols; ++c) {
      const uint8_t value = static_cast<uint8_t>((r * cols + c) & 0xFFu);
      source[r * source_row_stride + c] = value;
      expected[r * cols + c] = value;
    }
  }

  pixel_output_buffer_v2 tiny_output{};
  auto first_native_encode_request = make_encode_request(
      source, PIXEL_CODEC_PROFILE_NATIVE_UNCOMPRESSED_V2, source_row_stride,
      tiny_output, 0);
  auto ec = pixel::core_v2::encode_uncompressed_frame(
      &core_error, &first_native_encode_request);
  expect_eq(ec, PIXEL_CODEC_ERR_OUTPUT_TOO_SMALL,
      "native first encode should report output too small");
  expect_true(first_native_encode_request.output.encoded_size == expected.size(),
      "native first encode returns expected payload size");

  std::vector<uint8_t> encoded_native(first_native_encode_request.output.encoded_size);
  pixel_output_buffer_v2 native_output{};
  native_output.data = encoded_native.data();
  native_output.size = encoded_native.size();

  auto second_native_encode_request = make_encode_request(
      source, PIXEL_CODEC_PROFILE_NATIVE_UNCOMPRESSED_V2, source_row_stride,
      native_output, 0);
  ec = pixel::core_v2::encode_uncompressed_frame(&core_error, &second_native_encode_request);
  if (ec != PIXEL_CODEC_ERR_OK) {
    fail("native second encode failed: " + error_detail(core_error));
  }
  expect_eq(second_native_encode_request.output.encoded_size,
      static_cast<uint64_t>(expected.size()), "native encoded size");
  expect_true(std::memcmp(encoded_native.data(), expected.data(), expected.size()) == 0,
      "native encoded payload equals expected packed bytes");

  std::vector<uint8_t> decoded_native(expected.size(), uint8_t{0});
  auto native_decode_request = make_decode_request(
      encoded_native, decoded_native, PIXEL_CODEC_PROFILE_NATIVE_UNCOMPRESSED_V2);
  ec = pixel::core_v2::decode_uncompressed_frame(&core_error, &native_decode_request);
  if (ec != PIXEL_CODEC_ERR_OK) {
    fail("native decode failed: " + error_detail(core_error));
  }
  expect_true(std::memcmp(decoded_native.data(), expected.data(), expected.size()) == 0,
      "native decode payload equality");

  std::vector<uint8_t> decoded_rescale(rows * cols * sizeof(float), uint8_t{0});
  auto native_rescale_decode_request = make_decode_request(
      encoded_native, decoded_rescale, PIXEL_CODEC_PROFILE_NATIVE_UNCOMPRESSED_V2,
      PIXEL_DTYPE_F32_V2);
  native_rescale_decode_request.value_transform.transform_kind =
      PIXEL_DECODER_VALUE_TRANSFORM_RESCALE_V2;
  native_rescale_decode_request.value_transform.rescale_slope = 1.5;
  native_rescale_decode_request.value_transform.rescale_intercept = -2.0;
  ec = pixel::core_v2::decode_uncompressed_frame(&core_error, &native_rescale_decode_request);
  if (ec != PIXEL_CODEC_ERR_OK) {
    fail("native rescale decode failed: " + error_detail(core_error));
  }
  const float* native_rescaled_values =
      reinterpret_cast<const float*>(decoded_rescale.data());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    const double expected_value = static_cast<double>(expected[i]) * 1.5 - 2.0;
    expect_near(native_rescaled_values[i], expected_value, 1e-5,
        "native rescale decoded sample");
  }

  std::vector<float> modality_lut(256, 0.0f);
  for (std::size_t i = 0; i < modality_lut.size(); ++i) {
    modality_lut[i] = static_cast<float>(i * 10.0 + 0.5);
  }
  std::vector<uint8_t> decoded_lut(rows * cols * sizeof(float), uint8_t{0});
  auto native_lut_decode_request = make_decode_request(
      encoded_native, decoded_lut, PIXEL_CODEC_PROFILE_NATIVE_UNCOMPRESSED_V2,
      PIXEL_DTYPE_F32_V2);
  native_lut_decode_request.value_transform.transform_kind =
      PIXEL_DECODER_VALUE_TRANSFORM_MODALITY_LUT_V2;
  native_lut_decode_request.value_transform.lut_first_mapped = 0;
  native_lut_decode_request.value_transform.lut_value_count =
      static_cast<uint64_t>(modality_lut.size());
  native_lut_decode_request.value_transform.lut_values_f32.data =
      reinterpret_cast<const uint8_t*>(modality_lut.data());
  native_lut_decode_request.value_transform.lut_values_f32.size =
      static_cast<uint64_t>(modality_lut.size() * sizeof(float));
  ec = pixel::core_v2::decode_uncompressed_frame(&core_error, &native_lut_decode_request);
  if (ec != PIXEL_CODEC_ERR_OK) {
    fail("native modality LUT decode failed: " + error_detail(core_error));
  }
  const float* native_lut_values = reinterpret_cast<const float*>(decoded_lut.data());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    expect_near(native_lut_values[i], modality_lut[expected[i]], 1e-5,
        "native modality LUT decoded sample");
  }

  auto first_encap_encode_request = make_encode_request(
      source, PIXEL_CODEC_PROFILE_ENCAPSULATED_UNCOMPRESSED_V2, source_row_stride,
      tiny_output, 0);
  ec = pixel::core_v2::encode_uncompressed_frame(
      &core_error, &first_encap_encode_request);
  expect_eq(ec, PIXEL_CODEC_ERR_OUTPUT_TOO_SMALL,
      "encapsulated first encode should report output too small");
  expect_true(first_encap_encode_request.output.encoded_size == expected.size(),
      "encapsulated first encode returns expected payload size");

  std::vector<uint8_t> encoded_encap(first_encap_encode_request.output.encoded_size);
  pixel_output_buffer_v2 encap_output{};
  encap_output.data = encoded_encap.data();
  encap_output.size = encoded_encap.size();

  auto second_encap_encode_request = make_encode_request(
      source, PIXEL_CODEC_PROFILE_ENCAPSULATED_UNCOMPRESSED_V2, source_row_stride,
      encap_output, 0);
  ec = pixel::core_v2::encode_uncompressed_frame(&core_error, &second_encap_encode_request);
  if (ec != PIXEL_CODEC_ERR_OK) {
    fail("encapsulated second encode failed: " + error_detail(core_error));
  }
  expect_eq(second_encap_encode_request.output.encoded_size,
      static_cast<uint64_t>(expected.size()), "encapsulated encoded size");

  std::vector<uint8_t> decoded_encap(expected.size(), uint8_t{0});
  auto encap_decode_request = make_decode_request(
      encoded_encap, decoded_encap, PIXEL_CODEC_PROFILE_ENCAPSULATED_UNCOMPRESSED_V2);
  ec = pixel::core_v2::decode_uncompressed_frame(&core_error, &encap_decode_request);
  if (ec != PIXEL_CODEC_ERR_OK) {
    fail("encapsulated decode failed: " + error_detail(core_error));
  }
  expect_true(std::memcmp(decoded_encap.data(), expected.data(), expected.size()) == 0,
      "encapsulated decode payload equality");

  return 0;
}
