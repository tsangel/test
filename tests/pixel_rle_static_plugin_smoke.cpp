#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
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

std::string encoder_error_detail(const pixel_encoder_plugin_api& api, const void* ctx) {
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

std::string decoder_error_detail(const pixel_decoder_plugin_api& api, const void* ctx) {
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

void store_le32(uint8_t* dst, uint32_t value) {
  dst[0] = static_cast<uint8_t>(value & 0xFFu);
  dst[1] = static_cast<uint8_t>((value >> 8u) & 0xFFu);
  dst[2] = static_cast<uint8_t>((value >> 16u) & 0xFFu);
  dst[3] = static_cast<uint8_t>((value >> 24u) & 0xFFu);
}

struct EncoderContextGuard {
  const pixel_encoder_plugin_api* api{nullptr};
  void* context{nullptr};

  ~EncoderContextGuard() {
    if (api != nullptr && api->destroy != nullptr && context != nullptr) {
      api->destroy(context);
    }
  }
};

struct DecoderContextGuard {
  const pixel_decoder_plugin_api* api{nullptr};
  void* context{nullptr};

  ~DecoderContextGuard() {
    if (api != nullptr && api->destroy != nullptr && context != nullptr) {
      api->destroy(context);
    }
  }
};

pixel_encoder_request make_encoder_request(
    std::vector<uint8_t>& source, pixel_output_buffer encoded_buffer, uint64_t encoded_size) {
  pixel_encoder_request request{};
  request.struct_size = sizeof(pixel_encoder_request);
  request.abi_version = PIXEL_ENCODER_PLUGIN_ABI;

  request.source.struct_size = sizeof(pixel_encoder_source);
  request.source.abi_version = PIXEL_ENCODER_PLUGIN_ABI;
  request.source.source_buffer.data = source.data();
  request.source.source_buffer.size = source.size();

  request.frame.struct_size = sizeof(pixel_encoder_frame_info);
  request.frame.abi_version = PIXEL_ENCODER_PLUGIN_ABI;
  request.frame.codec_profile_code = PIXEL_CODEC_PROFILE_RLE_LOSSLESS;
  request.frame.source_dtype = PIXEL_DTYPE_U16;
  request.frame.source_planar = PIXEL_PLANAR_INTERLEAVED;
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

  request.output.struct_size = sizeof(pixel_encoder_output);
  request.output.abi_version = PIXEL_ENCODER_PLUGIN_ABI;
  request.output.encoded_buffer = encoded_buffer;
  request.output.encoded_size = encoded_size;

  return request;
}

pixel_decoder_request make_decoder_request(
    std::vector<uint8_t>& encoded, std::vector<uint8_t>& decoded) {
  pixel_decoder_request request{};
  request.struct_size = sizeof(pixel_decoder_request);
  request.abi_version = PIXEL_DECODER_PLUGIN_ABI;

  request.source.struct_size = sizeof(pixel_decoder_source);
  request.source.abi_version = PIXEL_DECODER_PLUGIN_ABI;
  request.source.source_buffer.data = encoded.data();
  request.source.source_buffer.size = encoded.size();

  request.frame.struct_size = sizeof(pixel_decoder_frame_info);
  request.frame.abi_version = PIXEL_DECODER_PLUGIN_ABI;
  request.frame.codec_profile_code = PIXEL_CODEC_PROFILE_RLE_LOSSLESS;
  request.frame.source_dtype = PIXEL_DTYPE_U16;
  request.frame.source_planar = PIXEL_PLANAR_INTERLEAVED;
  request.frame.rows = 16;
  request.frame.cols = 16;
  request.frame.samples_per_pixel = 1;
  request.frame.bits_stored = 16;
  request.frame.decode_mct = 0;

  request.output.struct_size = sizeof(pixel_decoder_output);
  request.output.abi_version = PIXEL_DECODER_PLUGIN_ABI;
  request.output.dst = decoded.data();
  request.output.dst_size = decoded.size();
  request.output.row_stride = 0;
  request.output.frame_stride = 0;
  request.output.dst_dtype = PIXEL_DTYPE_U16;
  request.output.dst_planar = PIXEL_PLANAR_INTERLEAVED;

  return request;
}

pixel_encoder_request make_rgb8_encoder_request(
    std::vector<uint8_t>& source, pixel_output_buffer encoded_buffer, uint64_t encoded_size) {
  pixel_encoder_request request{};
  request.struct_size = sizeof(pixel_encoder_request);
  request.abi_version = PIXEL_ENCODER_PLUGIN_ABI;

  request.source.struct_size = sizeof(pixel_encoder_source);
  request.source.abi_version = PIXEL_ENCODER_PLUGIN_ABI;
  request.source.source_buffer.data = source.data();
  request.source.source_buffer.size = source.size();

  request.frame.struct_size = sizeof(pixel_encoder_frame_info);
  request.frame.abi_version = PIXEL_ENCODER_PLUGIN_ABI;
  request.frame.codec_profile_code = PIXEL_CODEC_PROFILE_RLE_LOSSLESS;
  request.frame.source_dtype = PIXEL_DTYPE_U8;
  request.frame.source_planar = PIXEL_PLANAR_INTERLEAVED;
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

  request.output.struct_size = sizeof(pixel_encoder_output);
  request.output.abi_version = PIXEL_ENCODER_PLUGIN_ABI;
  request.output.encoded_buffer = encoded_buffer;
  request.output.encoded_size = encoded_size;

  return request;
}

pixel_decoder_request make_rgb8_decoder_request(
    std::vector<uint8_t>& encoded, std::vector<uint8_t>& decoded) {
  pixel_decoder_request request{};
  request.struct_size = sizeof(pixel_decoder_request);
  request.abi_version = PIXEL_DECODER_PLUGIN_ABI;

  request.source.struct_size = sizeof(pixel_decoder_source);
  request.source.abi_version = PIXEL_DECODER_PLUGIN_ABI;
  request.source.source_buffer.data = encoded.data();
  request.source.source_buffer.size = encoded.size();

  request.frame.struct_size = sizeof(pixel_decoder_frame_info);
  request.frame.abi_version = PIXEL_DECODER_PLUGIN_ABI;
  request.frame.codec_profile_code = PIXEL_CODEC_PROFILE_RLE_LOSSLESS;
  request.frame.source_dtype = PIXEL_DTYPE_U8;
  request.frame.source_planar = PIXEL_PLANAR_INTERLEAVED;
  request.frame.rows = 8;
  request.frame.cols = 8;
  request.frame.samples_per_pixel = 3;
  request.frame.bits_stored = 8;
  request.frame.decode_mct = 0;

  request.output.struct_size = sizeof(pixel_decoder_output);
  request.output.abi_version = PIXEL_DECODER_PLUGIN_ABI;
  request.output.dst = decoded.data();
  request.output.dst_size = decoded.size();
  request.output.row_stride = 0;
  request.output.frame_stride = 0;
  request.output.dst_dtype = PIXEL_DTYPE_U8;
  request.output.dst_planar = PIXEL_PLANAR_INTERLEAVED;

  return request;
}

std::vector<uint8_t> build_rle_literal_codestream(
    const std::vector<std::vector<uint8_t>>& segments) {
  if (segments.empty() || segments.size() > 15u) {
    fail("invalid RLE literal segment count");
  }

  std::vector<uint8_t> encoded(64u, uint8_t{0});
  store_le32(encoded.data(), static_cast<uint32_t>(segments.size()));
  for (std::size_t i = 0; i < segments.size(); ++i) {
    const auto& segment = segments[i];
    if (segment.empty() || segment.size() > 128u) {
      fail("invalid RLE literal segment size");
    }
    store_le32(encoded.data() + 4u + i * sizeof(uint32_t),
        static_cast<uint32_t>(encoded.size()));
    encoded.push_back(static_cast<uint8_t>(segment.size() - 1u));
    encoded.insert(encoded.end(), segment.begin(), segment.end());
  }
  return encoded;
}

pixel_decoder_request make_custom_decoder_request(
    std::vector<uint8_t>& encoded, std::vector<uint8_t>& decoded,
    uint32_t source_dtype, int32_t rows, int32_t cols,
    int32_t samples_per_pixel, int32_t bits_stored,
    uint32_t dst_dtype, uint32_t dst_planar) {
  pixel_decoder_request request{};
  request.struct_size = sizeof(pixel_decoder_request);
  request.abi_version = PIXEL_DECODER_PLUGIN_ABI;

  request.source.struct_size = sizeof(pixel_decoder_source);
  request.source.abi_version = PIXEL_DECODER_PLUGIN_ABI;
  request.source.source_buffer.data = encoded.data();
  request.source.source_buffer.size = encoded.size();

  request.frame.struct_size = sizeof(pixel_decoder_frame_info);
  request.frame.abi_version = PIXEL_DECODER_PLUGIN_ABI;
  request.frame.codec_profile_code = PIXEL_CODEC_PROFILE_RLE_LOSSLESS;
  request.frame.source_dtype = source_dtype;
  request.frame.source_planar = PIXEL_PLANAR_INTERLEAVED;
  request.frame.rows = rows;
  request.frame.cols = cols;
  request.frame.samples_per_pixel = samples_per_pixel;
  request.frame.bits_stored = bits_stored;
  request.frame.decode_mct = 0;

  request.output.struct_size = sizeof(pixel_decoder_output);
  request.output.abi_version = PIXEL_DECODER_PLUGIN_ABI;
  request.output.dst = decoded.data();
  request.output.dst_size = decoded.size();
  request.output.row_stride = 0;
  request.output.frame_stride = 0;
  request.output.dst_dtype = dst_dtype;
  request.output.dst_planar = dst_planar;

  return request;
}

}  // namespace

int main() {
  const auto& encoder_api = pixel::rle_codec::encoder_builtin_api();
  const auto& decoder_api = pixel::rle_codec::decoder_builtin_api();

  EncoderContextGuard encoder_ctx{};
  encoder_ctx.api = &encoder_api;
  encoder_ctx.context = encoder_api.create();
  expect_true(encoder_ctx.context != nullptr, "encoder create");

  const auto encoder_configure_ec =
      encoder_api.configure(encoder_ctx.context, PIXEL_CODEC_PROFILE_RLE_LOSSLESS, nullptr);
  if (encoder_configure_ec != PIXEL_CODEC_ERR_OK) {
    fail("encoder configure failed: " +
        encoder_error_detail(encoder_api, encoder_ctx.context));
  }

  std::vector<uint8_t> source(16u * 16u * sizeof(uint16_t), uint8_t{0});
  for (std::size_t i = 0; i < 16u * 16u; ++i) {
    const uint16_t value = static_cast<uint16_t>((i * 37u + 11u) & 0xFFFFu);
    std::memcpy(source.data() + i * sizeof(uint16_t), &value, sizeof(value));
  }

  pixel_output_buffer tiny_output{};
  auto first_encode_request = make_encoder_request(source, tiny_output, 0);
  const auto first_encode_ec =
      encoder_api.encode_frame(encoder_ctx.context, &first_encode_request);
  expect_eq(first_encode_ec, PIXEL_CODEC_ERR_OUTPUT_TOO_SMALL,
      "first encode should report output too small");
  expect_true(first_encode_request.output.encoded_size > 0,
      "first encode returns required encoded size");

  std::vector<uint8_t> encoded(first_encode_request.output.encoded_size);
  pixel_output_buffer encoded_output{};
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
      decoder_api.configure(decoder_ctx.context, PIXEL_CODEC_PROFILE_RLE_LOSSLESS, nullptr);
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

  std::vector<uint8_t> rgb_source(8u * 8u * 3u, uint8_t{0});
  for (std::size_t i = 0; i < rgb_source.size(); ++i) {
    rgb_source[i] = static_cast<uint8_t>((i * 29u + 7u) & 0xFFu);
  }

  pixel_output_buffer rgb_tiny_output{};
  auto first_rgb_encode_request =
      make_rgb8_encoder_request(rgb_source, rgb_tiny_output, 0);
  const auto first_rgb_encode_ec =
      encoder_api.encode_frame(encoder_ctx.context, &first_rgb_encode_request);
  expect_eq(first_rgb_encode_ec, PIXEL_CODEC_ERR_OUTPUT_TOO_SMALL,
      "RGB encode should report output too small");
  expect_true(first_rgb_encode_request.output.encoded_size > 0,
      "RGB encode returns required encoded size");

  std::vector<uint8_t> rgb_encoded(first_rgb_encode_request.output.encoded_size);
  pixel_output_buffer rgb_output{};
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

  auto masked_u16_rle = build_rle_literal_codestream({
      {0xF1u},
      {0x23u},
  });
  std::vector<uint8_t> masked_u16_decoded(sizeof(uint16_t), uint8_t{0});
  auto masked_u16_request = make_custom_decoder_request(
      masked_u16_rle, masked_u16_decoded,
      PIXEL_DTYPE_U16, 1, 1, 1, 12,
      PIXEL_DTYPE_U16, PIXEL_PLANAR_INTERLEAVED);
  const auto masked_u16_ec =
      decoder_api.decode_frame(decoder_ctx.context, &masked_u16_request);
  if (masked_u16_ec != PIXEL_CODEC_ERR_OK) {
    fail("unsigned 12-bit decode failed: " +
        decoder_error_detail(decoder_api, decoder_ctx.context));
  }
  uint16_t masked_u16_value = 0;
  std::memcpy(&masked_u16_value, masked_u16_decoded.data(), sizeof(masked_u16_value));
  expect_eq(masked_u16_value, static_cast<uint16_t>(0x0123u),
      "RLE unsigned 12-bit decode should mask unused high bits");

  auto signed_rgb12_rle = build_rle_literal_codestream({
      {0x0Fu},
      {0xFFu},
      {0x00u},
      {0x01u},
      {0x08u},
      {0x00u},
  });
  std::vector<uint8_t> signed_rgb12_decoded(3u * sizeof(int16_t), uint8_t{0});
  auto signed_rgb12_request = make_custom_decoder_request(
      signed_rgb12_rle, signed_rgb12_decoded,
      PIXEL_DTYPE_S16, 1, 1, 3, 12,
      PIXEL_DTYPE_S16, PIXEL_PLANAR_INTERLEAVED);
  const auto signed_rgb12_ec =
      decoder_api.decode_frame(decoder_ctx.context, &signed_rgb12_request);
  if (signed_rgb12_ec != PIXEL_CODEC_ERR_OK) {
    fail("signed RGB 12-bit decode failed: " +
        decoder_error_detail(decoder_api, decoder_ctx.context));
  }
  std::array<int16_t, 3> signed_rgb12_values{};
  std::memcpy(signed_rgb12_values.data(),
      signed_rgb12_decoded.data(), signed_rgb12_decoded.size());
  expect_eq(signed_rgb12_values[0], static_cast<int16_t>(-1),
      "RLE signed RGB 12-bit decode red sign extension");
  expect_eq(signed_rgb12_values[1], static_cast<int16_t>(1),
      "RLE signed RGB 12-bit decode green preserve positive");
  expect_eq(signed_rgb12_values[2], static_cast<int16_t>(-2048),
      "RLE signed RGB 12-bit decode blue sign extension");

  return 0;
}
