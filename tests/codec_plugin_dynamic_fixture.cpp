#include <cstdint>
#include <cstring>

#include "pixel_decoder_plugin_abi.h"
#include "pixel_encoder_plugin_abi.h"

#if defined(_WIN32)
#define DICOMSDL_TEST_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#define DICOMSDL_TEST_PLUGIN_EXPORT extern "C"
#endif

namespace {

void set_error(dicomsdl_codec_error_v1* error, std::uint32_t status_code,
    std::uint32_t stage_code, const char* detail) {
  if (!error) {
    return;
  }
  error->status_code = status_code;
  error->stage_code = stage_code;
  if (!error->detail || error->detail_capacity == 0 || !detail) {
    error->detail_length = 0;
    return;
  }
  const std::size_t detail_length = std::strlen(detail);
  const std::size_t copy_length =
      (detail_length < (error->detail_capacity - 1))
          ? detail_length
          : (error->detail_capacity - 1);
  std::memcpy(error->detail, detail, copy_length);
  error->detail[copy_length] = '\0';
  error->detail_length = static_cast<std::uint32_t>(copy_length);
}

struct DecoderCtx {
  std::uint16_t transfer_syntax_code{0};
};

void* decoder_create() {
  return new DecoderCtx{};
}

void decoder_destroy(void* ctx) {
  delete static_cast<DecoderCtx*>(ctx);
}

int decoder_configure(void* ctx, std::uint16_t transfer_syntax_code,
    const dicomsdl_codec_option_list_v1* options, dicomsdl_codec_error_v1* error) {
  auto* state = static_cast<DecoderCtx*>(ctx);
  if (!state) {
    set_error(error, DICOMSDL_CODEC_INTERNAL_ERROR,
        DICOMSDL_CODEC_STAGE_PLUGIN_LOOKUP, "decoder ctx is null");
    return 0;
  }
  if (options && options->count > 0) {
    set_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_PARSE_OPTIONS, "decoder options are not supported");
    return 0;
  }
  state->transfer_syntax_code = transfer_syntax_code;
  set_error(error, DICOMSDL_CODEC_OK, DICOMSDL_CODEC_STAGE_UNKNOWN, "");
  return 1;
}

int decoder_decode_frame(void* ctx, const dicomsdl_decoder_request_v1* request,
    dicomsdl_codec_error_v1* error) {
  auto* state = static_cast<DecoderCtx*>(ctx);
  if (!state || !request) {
    set_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_DECODE_FRAME, "decoder request is null");
    return 0;
  }
  if (!request->output.dst || request->output.dst_size == 0) {
    set_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_VALIDATE, "decoder destination is empty");
    return 0;
  }
  request->output.dst[0] = static_cast<std::uint8_t>(0x5c);
  set_error(error, DICOMSDL_CODEC_OK, DICOMSDL_CODEC_STAGE_UNKNOWN, "");
  return 1;
}

struct EncoderCtx {
  std::uint16_t transfer_syntax_code{0};
};

void* encoder_create() {
  return new EncoderCtx{};
}

void encoder_destroy(void* ctx) {
  delete static_cast<EncoderCtx*>(ctx);
}

int encoder_configure(void* ctx, std::uint16_t transfer_syntax_code,
    const dicomsdl_codec_option_list_v1* options, dicomsdl_codec_error_v1* error) {
  auto* state = static_cast<EncoderCtx*>(ctx);
  if (!state) {
    set_error(error, DICOMSDL_CODEC_INTERNAL_ERROR,
        DICOMSDL_CODEC_STAGE_PLUGIN_LOOKUP, "encoder ctx is null");
    return 0;
  }
  if (!options || options->count == 0) {
    state->transfer_syntax_code = transfer_syntax_code;
    set_error(error, DICOMSDL_CODEC_OK, DICOMSDL_CODEC_STAGE_UNKNOWN, "");
    return 1;
  }
  for (std::uint32_t i = 0; i < options->count; ++i) {
    const auto& item = options->items[i];
    if (!item.key || !item.value) {
      set_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
          DICOMSDL_CODEC_STAGE_PARSE_OPTIONS, "encoder option key/value is null");
      return 0;
    }
  }
  state->transfer_syntax_code = transfer_syntax_code;
  set_error(error, DICOMSDL_CODEC_OK, DICOMSDL_CODEC_STAGE_UNKNOWN, "");
  return 1;
}

int encoder_encode_frame(void* ctx, const dicomsdl_encoder_request_v1* request,
    dicomsdl_codec_error_v1* error) {
  auto* state = static_cast<EncoderCtx*>(ctx);
  if (!state || !request) {
    set_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_ENCODE_FRAME, "encoder request is null");
    return 0;
  }
  constexpr std::uint8_t kPayload[] = {0xde, 0xad, 0xbe, 0xef};
  constexpr std::uint64_t kPayloadSize = sizeof(kPayload);
  if (!request->output.encoded_buffer.data ||
      request->output.encoded_buffer.size < kPayloadSize) {
    const_cast<dicomsdl_encoder_request_v1*>(request)->output.encoded_size =
        kPayloadSize;
    set_error(error, DICOMSDL_CODEC_OUTPUT_TOO_SMALL,
        DICOMSDL_CODEC_STAGE_ENCODE_FRAME, "output buffer too small");
    return 0;
  }
  std::memcpy(
      request->output.encoded_buffer.data, kPayload, static_cast<std::size_t>(kPayloadSize));
  const_cast<dicomsdl_encoder_request_v1*>(request)->output.encoded_size =
      kPayloadSize;
  set_error(error, DICOMSDL_CODEC_OK, DICOMSDL_CODEC_STAGE_UNKNOWN, "");
  return 1;
}

}  // namespace

DICOMSDL_TEST_PLUGIN_EXPORT int dicomsdl_get_decoder_plugin_api_v1(
    dicomsdl_decoder_plugin_api_v1* out_api) {
  if (!out_api) {
    return 0;
  }
  if (out_api->abi_version != DICOMSDL_DECODER_PLUGIN_ABI_V1 ||
      out_api->struct_size < sizeof(dicomsdl_decoder_plugin_api_v1)) {
    return 0;
  }
  dicomsdl_decoder_plugin_api_v1 api{};
  api.struct_size = sizeof(dicomsdl_decoder_plugin_api_v1);
  api.abi_version = DICOMSDL_DECODER_PLUGIN_ABI_V1;
  api.info.struct_size = sizeof(dicomsdl_decoder_plugin_info_v1);
  api.info.abi_version = DICOMSDL_DECODER_PLUGIN_ABI_V1;
  api.info.plugin_key = "jpeg";
  api.info.display_name = "Dynamic JPEG Decoder Fixture";
  api.create = &decoder_create;
  api.destroy = &decoder_destroy;
  api.configure = &decoder_configure;
  api.decode_frame = &decoder_decode_frame;
  *out_api = api;
  return 1;
}

DICOMSDL_TEST_PLUGIN_EXPORT int dicomsdl_get_encoder_plugin_api_v1(
    dicomsdl_encoder_plugin_api_v1* out_api) {
  if (!out_api) {
    return 0;
  }
  if (out_api->abi_version != DICOMSDL_ENCODER_PLUGIN_ABI_V1 ||
      out_api->struct_size < sizeof(dicomsdl_encoder_plugin_api_v1)) {
    return 0;
  }
  dicomsdl_encoder_plugin_api_v1 api{};
  api.struct_size = sizeof(dicomsdl_encoder_plugin_api_v1);
  api.abi_version = DICOMSDL_ENCODER_PLUGIN_ABI_V1;
  api.info.struct_size = sizeof(dicomsdl_encoder_plugin_info_v1);
  api.info.abi_version = DICOMSDL_ENCODER_PLUGIN_ABI_V1;
  api.info.plugin_key = "jpeg";
  api.info.display_name = "Dynamic JPEG Encoder Fixture";
  api.create = &encoder_create;
  api.destroy = &encoder_destroy;
  api.configure = &encoder_configure;
  api.encode_frame = &encoder_encode_frame;
  *out_api = api;
  return 1;
}
