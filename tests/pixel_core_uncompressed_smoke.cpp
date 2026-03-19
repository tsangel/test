#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "direct_api.hpp"

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

std::string error_detail(const pixel::core::ErrorState& state) {
  char buffer[1024] = {};
  const uint32_t copied = pixel::core::copy_last_error_detail(
      &state, buffer, static_cast<uint32_t>(sizeof(buffer)));
  if (copied == 0) {
    return {};
  }
  return std::string(buffer);
}

pixel_encoder_request make_encode_request(
    std::vector<uint8_t>& source, uint32_t profile_code, uint64_t source_row_stride,
    pixel_output_buffer encoded_buffer, uint64_t encoded_size) {
  pixel_encoder_request request{};
  request.struct_size = sizeof(pixel_encoder_request);
  request.abi_version = PIXEL_ENCODER_PLUGIN_ABI;

  request.source.struct_size = sizeof(pixel_encoder_source);
  request.source.abi_version = PIXEL_ENCODER_PLUGIN_ABI;
  request.source.source_buffer.data = source.data();
  request.source.source_buffer.size = source.size();

  request.frame.struct_size = sizeof(pixel_encoder_frame_info);
  request.frame.abi_version = PIXEL_ENCODER_PLUGIN_ABI;
  request.frame.codec_profile_code = profile_code;
  request.frame.source_dtype = PIXEL_DTYPE_U8;
  request.frame.source_planar = PIXEL_PLANAR_INTERLEAVED;
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

  request.output.struct_size = sizeof(pixel_encoder_output);
  request.output.abi_version = PIXEL_ENCODER_PLUGIN_ABI;
  request.output.encoded_buffer = encoded_buffer;
  request.output.encoded_size = encoded_size;

  return request;
}

pixel_decoder_request make_decode_request(
    std::vector<uint8_t>& encoded, std::vector<uint8_t>& decoded, uint32_t profile_code,
    uint8_t dst_dtype = PIXEL_DTYPE_U8) {
  pixel_decoder_request request{};
  request.struct_size = sizeof(pixel_decoder_request);
  request.abi_version = PIXEL_DECODER_PLUGIN_ABI;

  request.source.struct_size = sizeof(pixel_decoder_source);
  request.source.abi_version = PIXEL_DECODER_PLUGIN_ABI;
  request.source.source_buffer.data = encoded.data();
  request.source.source_buffer.size = encoded.size();

  request.frame.struct_size = sizeof(pixel_decoder_frame_info);
  request.frame.abi_version = PIXEL_DECODER_PLUGIN_ABI;
  request.frame.codec_profile_code = profile_code;
  request.frame.source_dtype = PIXEL_DTYPE_U8;
  request.frame.source_planar = PIXEL_PLANAR_INTERLEAVED;
  request.frame.rows = 8;
  request.frame.cols = 8;
  request.frame.samples_per_pixel = 1;
  request.frame.bits_stored = 8;
  request.frame.decode_mct = 0;

  request.output.struct_size = sizeof(pixel_decoder_output);
  request.output.abi_version = PIXEL_DECODER_PLUGIN_ABI;
  request.output.dst = decoded.data();
  request.output.dst_size = decoded.size();
  request.output.row_stride = 0;
  request.output.frame_stride = 0;
  request.output.dst_dtype = dst_dtype;
  request.output.dst_planar = PIXEL_PLANAR_INTERLEAVED;

  return request;
}

}  // namespace

int main() {
  pixel::core::ErrorState core_error{};

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

  pixel_output_buffer tiny_output{};
  auto first_native_encode_request = make_encode_request(
      source, PIXEL_CODEC_PROFILE_NATIVE_UNCOMPRESSED, source_row_stride,
      tiny_output, 0);
  auto ec = pixel::core::encode_uncompressed_frame(
      &core_error, &first_native_encode_request);
  expect_eq(ec, PIXEL_CODEC_ERR_OUTPUT_TOO_SMALL,
      "native first encode should report output too small");
  expect_true(first_native_encode_request.output.encoded_size == expected.size(),
      "native first encode returns expected payload size");

  std::vector<uint8_t> encoded_native(first_native_encode_request.output.encoded_size);
  pixel_output_buffer native_output{};
  native_output.data = encoded_native.data();
  native_output.size = encoded_native.size();

  auto second_native_encode_request = make_encode_request(
      source, PIXEL_CODEC_PROFILE_NATIVE_UNCOMPRESSED, source_row_stride,
      native_output, 0);
  ec = pixel::core::encode_uncompressed_frame(&core_error, &second_native_encode_request);
  if (ec != PIXEL_CODEC_ERR_OK) {
    fail("native second encode failed: " + error_detail(core_error));
  }
  expect_eq(second_native_encode_request.output.encoded_size,
      static_cast<uint64_t>(expected.size()), "native encoded size");
  expect_true(std::memcmp(encoded_native.data(), expected.data(), expected.size()) == 0,
      "native encoded payload equals expected packed bytes");

  std::vector<uint8_t> decoded_native(expected.size(), uint8_t{0});
  auto native_decode_request = make_decode_request(
      encoded_native, decoded_native, PIXEL_CODEC_PROFILE_NATIVE_UNCOMPRESSED);
  ec = pixel::core::decode_uncompressed_frame(&core_error, &native_decode_request);
  if (ec != PIXEL_CODEC_ERR_OK) {
    fail("native decode failed: " + error_detail(core_error));
  }
  expect_true(std::memcmp(decoded_native.data(), expected.data(), expected.size()) == 0,
      "native decode payload equality");

  std::vector<uint8_t> decoded_native_single_channel(expected.size(), uint8_t{0});
  auto native_single_channel_decode_request = make_decode_request(
      encoded_native, decoded_native_single_channel, PIXEL_CODEC_PROFILE_NATIVE_UNCOMPRESSED);
  native_single_channel_decode_request.frame.source_planar = PIXEL_PLANAR_PLANAR;
  native_single_channel_decode_request.output.dst_planar = PIXEL_PLANAR_INTERLEAVED;
  ec = pixel::core::decode_uncompressed_frame(
      &core_error, &native_single_channel_decode_request);
  if (ec != PIXEL_CODEC_ERR_OK) {
    fail("native single-channel layout-equivalent decode failed: " + error_detail(core_error));
  }
  expect_true(
      std::memcmp(decoded_native_single_channel.data(), expected.data(), expected.size()) == 0,
      "native single-channel layout-equivalent payload equality");

  auto first_encap_encode_request = make_encode_request(
      source, PIXEL_CODEC_PROFILE_ENCAPSULATED_UNCOMPRESSED, source_row_stride,
      tiny_output, 0);
  ec = pixel::core::encode_uncompressed_frame(
      &core_error, &first_encap_encode_request);
  expect_eq(ec, PIXEL_CODEC_ERR_OUTPUT_TOO_SMALL,
      "encapsulated first encode should report output too small");
  expect_true(first_encap_encode_request.output.encoded_size == expected.size(),
      "encapsulated first encode returns expected payload size");

  std::vector<uint8_t> encoded_encap(first_encap_encode_request.output.encoded_size);
  pixel_output_buffer encap_output{};
  encap_output.data = encoded_encap.data();
  encap_output.size = encoded_encap.size();

  auto second_encap_encode_request = make_encode_request(
      source, PIXEL_CODEC_PROFILE_ENCAPSULATED_UNCOMPRESSED, source_row_stride,
      encap_output, 0);
  ec = pixel::core::encode_uncompressed_frame(&core_error, &second_encap_encode_request);
  if (ec != PIXEL_CODEC_ERR_OK) {
    fail("encapsulated second encode failed: " + error_detail(core_error));
  }
  expect_eq(second_encap_encode_request.output.encoded_size,
      static_cast<uint64_t>(expected.size()), "encapsulated encoded size");

  std::vector<uint8_t> decoded_encap(expected.size(), uint8_t{0});
  auto encap_decode_request = make_decode_request(
      encoded_encap, decoded_encap, PIXEL_CODEC_PROFILE_ENCAPSULATED_UNCOMPRESSED);
  ec = pixel::core::decode_uncompressed_frame(&core_error, &encap_decode_request);
  if (ec != PIXEL_CODEC_ERR_OK) {
    fail("encapsulated decode failed: " + error_detail(core_error));
  }
  expect_true(std::memcmp(decoded_encap.data(), expected.data(), expected.size()) == 0,
      "encapsulated decode payload equality");

  return 0;
}
