#include "pixel_/plugin_abi/builtin/jpeg_builtin_plugin.hpp"

#include "pixel_/bridge/codec_plugin_abi_adapter.hpp"
#include "pixel_/plugin_abi/common/codec_plugin_common.hpp"
#include "pixel_/bridge/codec_option_bridge.hpp"
#include "pixel_/decode/core/decode_codec_impl_detail.hpp"
#include "pixel_/encode/core/encode_codec_impl_detail.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dicom::pixel::detail {

namespace {

constexpr std::size_t kMaxPluginFrameBytes =
    std::size_t{2} * 1024u * 1024u * 1024u;
constexpr int kDefaultJpegQuality = 90;

struct JpegDecoderPluginContext {
  std::uint16_t transfer_syntax_code{DICOMSDL_TRANSFER_SYNTAX_CODE_INVALID};
  bool configured{false};
};

struct JpegEncoderPluginContext {
  std::uint16_t transfer_syntax_code{DICOMSDL_TRANSFER_SYNTAX_CODE_INVALID};
  int quality{kDefaultJpegQuality};
  bool configured{false};
};

struct DecoderContextGuard {
  const dicomsdl_decoder_plugin_api_v1* api{nullptr};
  void* context{nullptr};

  ~DecoderContextGuard() {
    if (api && api->destroy && context) {
      api->destroy(context);
    }
  }
};

struct EncoderContextGuard {
  const dicomsdl_encoder_plugin_api_v1* api{nullptr};
  void* context{nullptr};

  ~EncoderContextGuard() {
    if (api && api->destroy && context) {
      api->destroy(context);
    }
  }
};

using plugin_common::checked_u64_to_size_t;
using plugin_common::parse_jpeg_quality_configure_option;
using plugin_common::prepare_plugin_decode_request;
using plugin_common::prepare_plugin_encode_request;
using plugin_common::PreparedDecodeRequest;
using plugin_common::PreparedEncodeRequest;
using plugin_common::set_abi_error;
using plugin_common::set_abi_error_from_codec_error;
using plugin_common::set_abi_ok;
using plugin_common::validate_configure_no_options;
using plugin_common::validate_jpeg_quality_configure_option;
using plugin_common::write_plugin_encoded_output;

[[nodiscard]] bool is_jpeg_transfer_syntax_code(
    std::uint16_t transfer_syntax_code) noexcept {
  const auto transfer_syntax =
      abi::from_transfer_syntax_code(transfer_syntax_code);
  if (!transfer_syntax.has_value()) {
    return false;
  }
  return transfer_syntax->is_jpeg_family() && !transfer_syntax->is_jpegls() &&
         !transfer_syntax->is_jpeg2000() && !transfer_syntax->is_jpegxl();
}

void* jpeg_decoder_create() noexcept {
  return new (std::nothrow) JpegDecoderPluginContext{};
}

void jpeg_decoder_destroy(void* ctx) noexcept {
  delete static_cast<JpegDecoderPluginContext*>(ctx);
}

int jpeg_decoder_configure(void* ctx, std::uint16_t transfer_syntax_code,
    const dicomsdl_codec_option_list_v1* options,
    dicomsdl_codec_error_v1* error) noexcept {
  auto* context = static_cast<JpegDecoderPluginContext*>(ctx);
  if (!context) {
    set_abi_error(error, DICOMSDL_CODEC_INTERNAL_ERROR,
        DICOMSDL_CODEC_STAGE_PLUGIN_LOOKUP, "decoder context is null");
    return 0;
  }
  if (!validate_configure_no_options(options, transfer_syntax_code,
          "JPEG decoder does not accept configure options",
          "transfer syntax is not a JPEG family syntax",
          is_jpeg_transfer_syntax_code, error)) {
    return 0;
  }

  context->transfer_syntax_code = transfer_syntax_code;
  context->configured = true;
  set_abi_ok(error);
  return 1;
}

int jpeg_decoder_decode_frame(void* ctx, const dicomsdl_decoder_request_v1* request,
    dicomsdl_codec_error_v1* error) noexcept {
  auto* context = static_cast<JpegDecoderPluginContext*>(ctx);
  if (!context) {
    set_abi_error(error, DICOMSDL_CODEC_INTERNAL_ERROR,
        DICOMSDL_CODEC_STAGE_PLUGIN_LOOKUP, "decoder context is null");
    return 0;
  }
  if (!context->configured) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_PARSE_OPTIONS,
        "decoder configure() must be called before decode_frame()");
    return 0;
  }
  if (!request) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_DECODE_FRAME, "decoder request is null");
    return 0;
  }
  PreparedDecodeRequest prepared{};
  if (!prepare_plugin_decode_request(*request, context->transfer_syntax_code, -1,
          "request transfer syntax is not a JPEG family syntax",
          "JPEG decoder plugin supports destination dtype == source dtype or f32",
          is_jpeg_transfer_syntax_code, prepared, error)) {
    return 0;
  }

  CodecError decode_error{};
  prepared.options.decode_mct = request->frame.decode_mct != 0;
  if (!decode_jpeg_into(prepared.info, prepared.value_transform,
          prepared.destination, prepared.strides, prepared.options, decode_error,
          prepared.source)) {
    set_abi_error_from_codec_error(error, decode_error, "decode_frame",
        "JPEG decoder backend failed");
    return 0;
  }

  set_abi_ok(error);
  return 1;
}

void* jpeg_encoder_create() noexcept {
  return new (std::nothrow) JpegEncoderPluginContext{};
}

void jpeg_encoder_destroy(void* ctx) noexcept {
  delete static_cast<JpegEncoderPluginContext*>(ctx);
}

int jpeg_encoder_configure(void* ctx, std::uint16_t transfer_syntax_code,
    const dicomsdl_codec_option_list_v1* options,
    dicomsdl_codec_error_v1* error) noexcept {
  auto* context = static_cast<JpegEncoderPluginContext*>(ctx);
  if (!context) {
    set_abi_error(error, DICOMSDL_CODEC_INTERNAL_ERROR,
        DICOMSDL_CODEC_STAGE_PLUGIN_LOOKUP, "encoder context is null");
    return 0;
  }
  if (!is_jpeg_transfer_syntax_code(transfer_syntax_code)) {
    set_abi_error(error, DICOMSDL_CODEC_UNSUPPORTED,
        DICOMSDL_CODEC_STAGE_VALIDATE,
        "transfer syntax is not a JPEG family syntax");
    return 0;
  }

  int quality = kDefaultJpegQuality;
  if (!parse_jpeg_quality_configure_option(
          options, kDefaultJpegQuality, quality, error)) {
    return 0;
  }
  if (!validate_jpeg_quality_configure_option(quality, error)) {
    return 0;
  }

  context->transfer_syntax_code = transfer_syntax_code;
  context->quality = quality;
  context->configured = true;
  set_abi_ok(error);
  return 1;
}

int jpeg_encoder_encode_frame(void* ctx, const dicomsdl_encoder_request_v1* request,
    dicomsdl_codec_error_v1* error) noexcept {
  auto* context = static_cast<JpegEncoderPluginContext*>(ctx);
  if (!context) {
    set_abi_error(error, DICOMSDL_CODEC_INTERNAL_ERROR,
        DICOMSDL_CODEC_STAGE_PLUGIN_LOOKUP, "encoder context is null");
    return 0;
  }
  if (!context->configured) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_PARSE_OPTIONS,
        "encoder configure() must be called before encode_frame()");
    return 0;
  }
  if (!request) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_ENCODE_FRAME, "encoder request is null");
    return 0;
  }
  PreparedEncodeRequest prepared{};
  if (!prepare_plugin_encode_request(*request, context->transfer_syntax_code,
          "request transfer syntax is not a JPEG family syntax",
          is_jpeg_transfer_syntax_code, prepared, error)) {
    return 0;
  }
  const bool lossless = prepared.transfer_syntax.is_jpeg_lossless();

  std::vector<std::uint8_t> encoded{};
  CodecError encode_error{};
  const JpegOptions options{
      .quality = context->quality,
  };
  if (!try_encode_jpeg_frame(prepared.source, prepared.rows, prepared.cols,
          prepared.samples_per_pixel, prepared.bytes_per_sample,
          request->frame.bits_allocated, request->frame.bits_stored,
          prepared.source_planar, prepared.row_stride, lossless, options, encoded,
          encode_error)) {
    set_abi_error_from_codec_error(error, encode_error, "encode_frame",
        "JPEG encoder backend failed");
    return 0;
  }

  if (!write_plugin_encoded_output(*request, prepared.encoded_capacity, encoded,
          error)) {
    return 0;
  }
  set_abi_ok(error);
  return 1;
}

[[nodiscard]] const dicomsdl_decoder_plugin_api_v1&
jpeg_decoder_plugin_api_v1() noexcept {
  static const auto api = [] {
    dicomsdl_decoder_plugin_api_v1 value{};
    value.struct_size = sizeof(dicomsdl_decoder_plugin_api_v1);
    value.abi_version = DICOMSDL_DECODER_PLUGIN_ABI_V1;
    value.info.struct_size = sizeof(dicomsdl_decoder_plugin_info_v1);
    value.info.abi_version = DICOMSDL_DECODER_PLUGIN_ABI_V1;
    value.info.plugin_key = "jpeg";
    value.info.display_name = "JPEG Builtin ABI Decoder";
    value.create = &jpeg_decoder_create;
    value.destroy = &jpeg_decoder_destroy;
    value.configure = &jpeg_decoder_configure;
    value.decode_frame = &jpeg_decoder_decode_frame;
    return value;
  }();
  return api;
}

[[nodiscard]] const dicomsdl_encoder_plugin_api_v1&
jpeg_encoder_plugin_api_v1() noexcept {
  static const auto api = [] {
    dicomsdl_encoder_plugin_api_v1 value{};
    value.struct_size = sizeof(dicomsdl_encoder_plugin_api_v1);
    value.abi_version = DICOMSDL_ENCODER_PLUGIN_ABI_V1;
    value.info.struct_size = sizeof(dicomsdl_encoder_plugin_info_v1);
    value.info.abi_version = DICOMSDL_ENCODER_PLUGIN_ABI_V1;
    value.info.plugin_key = "jpeg";
    value.info.display_name = "JPEG Builtin ABI Encoder";
    value.create = &jpeg_encoder_create;
    value.destroy = &jpeg_encoder_destroy;
    value.configure = &jpeg_encoder_configure;
    value.encode_frame = &jpeg_encoder_encode_frame;
    return value;
  }();
  return api;
}

bool invoke_jpeg_decoder_via_plugin_api(
    const CodecDecodeFrameInput& input, CodecError& out_error) noexcept {
  out_error = {};

  dicomsdl_decoder_request_v1 request{};
  abi::build_decoder_request_v1(input, request);

  if (request.frame.transfer_syntax_code == DICOMSDL_TRANSFER_SYNTAX_CODE_INVALID) {
    set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
        "invalid transfer syntax code for decoder ABI request");
    return false;
  }
  if (request.frame.source_dtype == DICOMSDL_DTYPE_UNKNOWN &&
      input.info.sv_dtype != DataType::unknown) {
    set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
        "source dtype is not representable in decoder ABI request");
    return false;
  }

  const auto& api = jpeg_decoder_plugin_api_v1();
  DecoderContextGuard guard{};
  guard.api = &api;
  guard.context = api.create ? api.create() : nullptr;
  if (guard.context == nullptr) {
    set_codec_error(out_error, CodecStatusCode::internal_error,
        "plugin_lookup", "JPEG decoder plugin create() returned null context");
    return false;
  }

  char detail_buffer[1024];
  dicomsdl_codec_error_v1 plugin_error{};
  abi::initialize_codec_error_buffer(plugin_error, detail_buffer,
      static_cast<std::uint32_t>(sizeof(detail_buffer)));
  if (!api.configure(guard.context, request.frame.transfer_syntax_code,
          nullptr, &plugin_error)) {
    out_error = abi::decode_plugin_error_v1(
        plugin_error, "parse_options", "JPEG decoder configure failed");
    if (out_error.code == CodecStatusCode::ok) {
      out_error.code = CodecStatusCode::backend_error;
    }
    return false;
  }

  abi::initialize_codec_error_buffer(plugin_error, detail_buffer,
      static_cast<std::uint32_t>(sizeof(detail_buffer)));
  if (!api.decode_frame(guard.context, &request, &plugin_error)) {
    out_error = abi::decode_plugin_error_v1(
        plugin_error, "decode_frame", "JPEG decoder failed");
    if (out_error.code == CodecStatusCode::ok) {
      out_error.code = CodecStatusCode::backend_error;
    }
    return false;
  }
  return true;
}

bool invoke_jpeg_encoder_via_plugin_api(const CodecEncodeFrameInput& input,
    std::span<const CodecOptionKv> encode_options,
    std::vector<std::uint8_t>& out_encoded_frame,
    CodecError& out_error) noexcept {
  out_encoded_frame.clear();
  out_error = {};

  if (input.transfer_syntax.uid_type() != UidType::TransferSyntax) {
    set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
        "invalid transfer syntax");
    return false;
  }

  const auto transfer_syntax_code = abi::to_transfer_syntax_code(input.transfer_syntax);
  if (transfer_syntax_code == DICOMSDL_TRANSFER_SYNTAX_CODE_INVALID) {
    set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
        "invalid transfer syntax code for encoder ABI request");
    return false;
  }

  const auto& api = jpeg_encoder_plugin_api_v1();
  EncoderContextGuard guard{};
  guard.api = &api;
  guard.context = api.create ? api.create() : nullptr;
  if (guard.context == nullptr) {
    set_codec_error(out_error, CodecStatusCode::internal_error, "plugin_lookup",
        "JPEG encoder plugin create() returned null context");
    return false;
  }

  AbiOptionStorage option_storage{};
  if (!build_abi_option_storage_from_pairs(
          encode_options, option_storage, out_error)) {
    return false;
  }
  const dicomsdl_codec_option_list_v1* option_list_ptr =
      abi_option_list_ptr(option_storage);

  char detail_buffer[1024];
  dicomsdl_codec_error_v1 plugin_error{};
  abi::initialize_codec_error_buffer(plugin_error, detail_buffer,
      static_cast<std::uint32_t>(sizeof(detail_buffer)));
  if (!api.configure(
          guard.context, transfer_syntax_code, option_list_ptr, &plugin_error)) {
    out_error = abi::decode_plugin_error_v1(
        plugin_error, "parse_options", "JPEG encoder configure failed");
    if (out_error.code == CodecStatusCode::ok) {
      out_error.code = CodecStatusCode::backend_error;
    }
    return false;
  }

  const std::size_t minimum_capacity = std::max<std::size_t>(
      input.destination_frame_payload, std::size_t{4096});
  if (minimum_capacity > kMaxPluginFrameBytes) {
    set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
        "initial output capacity exceeds max frame bytes");
    return false;
  }

  out_encoded_frame.resize(minimum_capacity);
  for (int attempt = 0; attempt < 4; ++attempt) {
    dicomsdl_encoder_request_v1 request{};
    abi::build_encoder_request_v1(input, std::span<std::uint8_t>(out_encoded_frame),
        static_cast<std::uint64_t>(out_encoded_frame.size()), request);

    abi::initialize_codec_error_buffer(plugin_error, detail_buffer,
        static_cast<std::uint32_t>(sizeof(detail_buffer)));
    if (api.encode_frame(guard.context, &request, &plugin_error)) {
      if (request.output.encoded_size >
          static_cast<std::uint64_t>(out_encoded_frame.size())) {
        set_codec_error(out_error, CodecStatusCode::internal_error,
            "encode_frame",
            "JPEG encoder returned encoded_size larger than output buffer");
        out_encoded_frame.clear();
        return false;
      }
      out_encoded_frame.resize(
          static_cast<std::size_t>(request.output.encoded_size));
      return true;
    }

    if (plugin_error.status_code != DICOMSDL_CODEC_OUTPUT_TOO_SMALL) {
      out_error = abi::decode_plugin_error_v1(
          plugin_error, "encode_frame", "JPEG encoder failed");
      if (out_error.code == CodecStatusCode::ok) {
        out_error.code = CodecStatusCode::backend_error;
      }
      out_encoded_frame.clear();
      return false;
    }

    std::size_t required_size = 0;
    if (!checked_u64_to_size_t(request.output.encoded_size, required_size)) {
      set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
          "requested output size exceeds host size_t range");
      out_encoded_frame.clear();
      return false;
    }
    if (required_size <= out_encoded_frame.size()) {
      if (out_encoded_frame.size() >
          std::numeric_limits<std::size_t>::max() / 2) {
        required_size = std::numeric_limits<std::size_t>::max();
      } else {
        required_size = out_encoded_frame.size() * 2;
      }
    }
    if (required_size == 0) {
      required_size = minimum_capacity;
    }
    if (required_size > kMaxPluginFrameBytes) {
      set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
          "requested output size exceeds max frame bytes");
      out_encoded_frame.clear();
      return false;
    }
    out_encoded_frame.resize(required_size);
  }

  set_codec_error(out_error, CodecStatusCode::backend_error, "encode_frame",
      "JPEG encoder repeatedly returned OUTPUT_TOO_SMALL");
  out_encoded_frame.clear();
  return false;
}

}  // namespace

const dicomsdl_decoder_plugin_api_v1&
jpeg_decoder_plugin_api_for_shared() noexcept {
  return jpeg_decoder_plugin_api_v1();
}

const dicomsdl_encoder_plugin_api_v1&
jpeg_encoder_plugin_api_for_shared() noexcept {
  return jpeg_encoder_plugin_api_v1();
}

bool decode_frame_plugin_jpeg_via_abi(
    const CodecDecodeFrameInput& input, CodecError& out_error) noexcept {
  try {
    if (!input.info.has_pixel_data) {
      set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
          "sv_dtype is unknown");
      return false;
    }
    return invoke_jpeg_decoder_via_plugin_api(input, out_error);
  } catch (const std::exception& e) {
    set_codec_error(out_error, CodecStatusCode::backend_error, "decode_frame",
        e.what());
  } catch (...) {
    set_codec_error(out_error, CodecStatusCode::backend_error, "decode_frame",
        "non-standard exception");
  }
  return false;
}

bool encode_frame_plugin_jpeg_via_abi(const CodecEncodeFrameInput& input,
    std::span<const CodecOptionKv> encode_options,
    std::vector<std::uint8_t>& out_encoded_frame,
    CodecError& out_error) noexcept {
  try {
    return invoke_jpeg_encoder_via_plugin_api(
        input, encode_options, out_encoded_frame, out_error);
  } catch (const std::exception& e) {
    set_codec_error(out_error, CodecStatusCode::backend_error, "encode_frame",
        e.what());
  } catch (...) {
    set_codec_error(out_error, CodecStatusCode::backend_error, "encode_frame",
        "non-standard exception");
  }
  out_encoded_frame.clear();
  return false;
}

}  // namespace dicom::pixel::detail
