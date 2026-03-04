#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "pixel_decoder_plugin_abi_v2.h"
#include "pixel_encoder_plugin_abi_v2.h"

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

std::string encoder_error_detail(const pixel_encoder_plugin_api_v2& api, const void* ctx) {
  if (api.copy_last_error_detail == nullptr) {
    return {};
  }
  char buffer[1024] = {};
  const uint32_t copied = api.copy_last_error_detail(ctx, buffer,
      static_cast<uint32_t>(sizeof(buffer)));
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
  const uint32_t copied = api.copy_last_error_detail(ctx, buffer,
      static_cast<uint32_t>(sizeof(buffer)));
  if (copied == 0) {
    return {};
  }
  return std::string(buffer);
}

#if defined(_WIN32)
using library_handle = HMODULE;

library_handle open_library(const char* path) {
  return LoadLibraryA(path);
}

void* load_symbol(library_handle handle, const char* symbol_name) {
  return reinterpret_cast<void*>(GetProcAddress(handle, symbol_name));
}

void close_library(library_handle handle) {
  if (handle != nullptr) {
    FreeLibrary(handle);
  }
}

#else
using library_handle = void*;

library_handle open_library(const char* path) {
  return dlopen(path, RTLD_NOW | RTLD_LOCAL);
}

void* load_symbol(library_handle handle, const char* symbol_name) {
  return dlsym(handle, symbol_name);
}

void close_library(library_handle handle) {
  if (handle != nullptr) {
    dlclose(handle);
  }
}

#endif

struct LibraryGuard {
  library_handle handle{nullptr};

  ~LibraryGuard() {
    close_library(handle);
  }
};

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
    std::vector<uint8_t>& source, uint32_t codec_profile_code,
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
  request.frame.codec_profile_code = codec_profile_code;
  request.frame.source_dtype = PIXEL_DTYPE_U8_V2;
  request.frame.source_planar = PIXEL_PLANAR_INTERLEAVED_V2;
  request.frame.rows = 32;
  request.frame.cols = 32;
  request.frame.samples_per_pixel = 1;
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

pixel_decoder_request_v2 make_decoder_request(
    std::vector<uint8_t>& encoded, std::vector<uint8_t>& decoded,
    uint32_t codec_profile_code) {
  pixel_decoder_request_v2 request{};
  request.struct_size = sizeof(pixel_decoder_request_v2);
  request.abi_version = PIXEL_DECODER_PLUGIN_ABI_V2;

  request.source.struct_size = sizeof(pixel_decoder_source_v2);
  request.source.abi_version = PIXEL_DECODER_PLUGIN_ABI_V2;
  request.source.source_buffer.data = encoded.data();
  request.source.source_buffer.size = encoded.size();

  request.frame.struct_size = sizeof(pixel_decoder_frame_info_v2);
  request.frame.abi_version = PIXEL_DECODER_PLUGIN_ABI_V2;
  request.frame.codec_profile_code = codec_profile_code;
  request.frame.source_dtype = PIXEL_DTYPE_U8_V2;
  request.frame.source_planar = PIXEL_PLANAR_INTERLEAVED_V2;
  request.frame.rows = 32;
  request.frame.cols = 32;
  request.frame.samples_per_pixel = 1;
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

bool has_non_zero_sample(const std::vector<uint8_t>& data) {
  for (uint8_t value : data) {
    if (value != 0u) {
      return true;
    }
  }
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2 || argv[1] == nullptr) {
    fail("plugin path argument is required");
  }

  LibraryGuard library{};
  library.handle = open_library(argv[1]);
  expect_true(library.handle != nullptr, "load JPEG plugin library");

  using get_decoder_api_fn = int (*)(pixel_decoder_plugin_api_v2* out_api);
  using get_encoder_api_fn = int (*)(pixel_encoder_plugin_api_v2* out_api);

  auto* get_decoder_api = reinterpret_cast<get_decoder_api_fn>(
      load_symbol(library.handle, "pixel_get_decoder_plugin_api_v2"));
  auto* get_encoder_api = reinterpret_cast<get_encoder_api_fn>(
      load_symbol(library.handle, "pixel_get_encoder_plugin_api_v2"));
  expect_true(get_decoder_api != nullptr, "resolve decoder entrypoint");
  expect_true(get_encoder_api != nullptr, "resolve encoder entrypoint");

  pixel_decoder_plugin_api_v2 decoder_api{};
  decoder_api.struct_size = sizeof(pixel_decoder_plugin_api_v2);
  decoder_api.abi_version = PIXEL_DECODER_PLUGIN_ABI_V2;
  expect_true(get_decoder_api(&decoder_api) != 0, "get decoder api");

  pixel_encoder_plugin_api_v2 encoder_api{};
  encoder_api.struct_size = sizeof(pixel_encoder_plugin_api_v2);
  encoder_api.abi_version = PIXEL_ENCODER_PLUGIN_ABI_V2;
  expect_true(get_encoder_api(&encoder_api) != 0, "get encoder api");

  EncoderContextGuard encoder_ctx{};
  encoder_ctx.api = &encoder_api;
  encoder_ctx.context = encoder_api.create();
  expect_true(encoder_ctx.context != nullptr, "encoder create");

  const auto encoder_configure_ec = encoder_api.configure(
      encoder_ctx.context, PIXEL_CODEC_PROFILE_JPEG_LOSSLESS_V2, nullptr);
  if (encoder_configure_ec != PIXEL_CODEC_ERR_OK) {
    fail("encoder configure failed: " + encoder_error_detail(encoder_api, encoder_ctx.context));
  }

  std::vector<uint8_t> source(32u * 32u);
  for (std::size_t i = 0; i < source.size(); ++i) {
    source[i] = static_cast<uint8_t>((i * 19u + 11u) & 0xFFu);
  }

  pixel_output_buffer_v2 tiny_output{};
  auto first_encode_request = make_encoder_request(
      source, PIXEL_CODEC_PROFILE_JPEG_LOSSLESS_V2, tiny_output, 0);
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

  auto second_encode_request = make_encoder_request(
      source, PIXEL_CODEC_PROFILE_JPEG_LOSSLESS_V2, encoded_output, 0);
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

  const auto decoder_configure_ec = decoder_api.configure(
      decoder_ctx.context, PIXEL_CODEC_PROFILE_JPEG_LOSSLESS_V2, nullptr);
  if (decoder_configure_ec != PIXEL_CODEC_ERR_OK) {
    fail("decoder configure failed: " + decoder_error_detail(decoder_api, decoder_ctx.context));
  }

  std::vector<uint8_t> decoded(source.size(), uint8_t{0});
  auto decode_request = make_decoder_request(
      encoded, decoded, PIXEL_CODEC_PROFILE_JPEG_LOSSLESS_V2);

  const auto decode_ec = decoder_api.decode_frame(decoder_ctx.context, &decode_request);
  if (decode_ec != PIXEL_CODEC_ERR_OK) {
    fail("decode failed: " + decoder_error_detail(decoder_api, decoder_ctx.context));
  }

  expect_eq(decoded.size(), source.size(), "decoded size");
  expect_true(std::memcmp(decoded.data(), source.data(), source.size()) == 0,
      "JPEG lossless round-trip data equality");

  const pixel_option_kv_v2 lossy_options[] = {
      {"quality", "20"},
  };
  const uint32_t lossy_option_count = static_cast<uint32_t>(
      sizeof(lossy_options) / sizeof(lossy_options[0]));
  const pixel_option_list_v2 lossy_option_list{
      lossy_options, lossy_option_count};

  const auto encoder_configure_lossy_ec = encoder_api.configure(
      encoder_ctx.context, PIXEL_CODEC_PROFILE_JPEG_LOSSY_V2, &lossy_option_list);
  if (encoder_configure_lossy_ec != PIXEL_CODEC_ERR_OK) {
    fail("encoder lossy configure failed: " +
        encoder_error_detail(encoder_api, encoder_ctx.context));
  }

  const auto decoder_configure_lossy_ec = decoder_api.configure(
      decoder_ctx.context, PIXEL_CODEC_PROFILE_JPEG_LOSSY_V2, nullptr);
  if (decoder_configure_lossy_ec != PIXEL_CODEC_ERR_OK) {
    fail("decoder lossy configure failed: " +
        decoder_error_detail(decoder_api, decoder_ctx.context));
  }

  pixel_output_buffer_v2 tiny_lossy_output{};
  auto first_lossy_encode_request = make_encoder_request(
      source, PIXEL_CODEC_PROFILE_JPEG_LOSSY_V2, tiny_lossy_output, 0);
  const auto first_lossy_encode_ec =
      encoder_api.encode_frame(encoder_ctx.context, &first_lossy_encode_request);
  expect_eq(first_lossy_encode_ec, PIXEL_CODEC_ERR_OUTPUT_TOO_SMALL,
      "first lossy encode should report output too small");
  expect_true(first_lossy_encode_request.output.encoded_size > 0,
      "first lossy encode returns required encoded size");

  std::vector<uint8_t> lossy_encoded(first_lossy_encode_request.output.encoded_size);
  pixel_output_buffer_v2 lossy_output{};
  lossy_output.data = lossy_encoded.data();
  lossy_output.size = lossy_encoded.size();

  auto second_lossy_encode_request = make_encoder_request(
      source, PIXEL_CODEC_PROFILE_JPEG_LOSSY_V2, lossy_output, 0);
  const auto second_lossy_encode_ec =
      encoder_api.encode_frame(encoder_ctx.context, &second_lossy_encode_request);
  if (second_lossy_encode_ec != PIXEL_CODEC_ERR_OK) {
    fail("second lossy encode failed: " + encoder_error_detail(encoder_api, encoder_ctx.context));
  }
  expect_true(second_lossy_encode_request.output.encoded_size > 0,
      "second lossy encode wrote non-empty codestream");
  expect_true(second_lossy_encode_request.output.encoded_size <= lossy_encoded.size(),
      "second lossy encode size fits output buffer");
  lossy_encoded.resize(static_cast<std::size_t>(second_lossy_encode_request.output.encoded_size));

  std::vector<uint8_t> lossy_decoded(source.size(), uint8_t{0});
  auto lossy_decode_request = make_decoder_request(
      lossy_encoded, lossy_decoded, PIXEL_CODEC_PROFILE_JPEG_LOSSY_V2);
  const auto lossy_decode_ec = decoder_api.decode_frame(decoder_ctx.context, &lossy_decode_request);
  if (lossy_decode_ec != PIXEL_CODEC_ERR_OK) {
    fail("lossy decode failed: " + decoder_error_detail(decoder_api, decoder_ctx.context));
  }
  expect_eq(lossy_decoded.size(), source.size(), "lossy decoded size");
  expect_true(has_non_zero_sample(lossy_decoded),
      "JPEG lossy decode should produce non-zero output");

  bool lossy_differs = false;
  for (std::size_t i = 0; i < source.size(); ++i) {
    if (source[i] != lossy_decoded[i]) {
      lossy_differs = true;
      break;
    }
  }
  expect_true(lossy_differs, "JPEG lossy decode should differ from source");

  return 0;
}
