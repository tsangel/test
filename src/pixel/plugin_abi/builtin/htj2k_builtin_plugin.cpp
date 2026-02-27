#include "pixel/plugin_abi/builtin/htj2k_builtin_plugin.hpp"

#include "pixel/bridge/codec_plugin_abi_adapter.hpp"
#include "pixel/plugin_abi/common/codec_plugin_common.hpp"
#include "pixel/bridge/codec_option_bridge.hpp"
#include "pixel/decode/core/decode_codec_impl_detail.hpp"
#include "pixel/encode/core/encode_codec_impl_detail.hpp"

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

struct Htj2kDecoderPluginContext {
  std::uint16_t transfer_syntax_code{DICOMSDL_TRANSFER_SYNTAX_CODE_INVALID};
  int decoder_threads{-1};
  Htj2kDecoder backend{Htj2kDecoder::auto_select};
  bool configured{false};
};

struct Htj2kEncoderPluginContext {
  std::uint16_t transfer_syntax_code{DICOMSDL_TRANSFER_SYNTAX_CODE_INVALID};
  Htj2kOptions options{};
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
using plugin_common::htj2k_decoder_backend_option_value;
using plugin_common::parse_htj2k_decoder_configure_options;
using plugin_common::parse_j2k_like_encoder_configure_options;
using plugin_common::prepare_plugin_decode_request;
using plugin_common::prepare_plugin_encode_request;
using plugin_common::PreparedDecodeRequest;
using plugin_common::PreparedEncodeRequest;
using plugin_common::set_abi_error;
using plugin_common::set_abi_error_from_codec_error;
using plugin_common::set_abi_ok;
using plugin_common::validate_htj2k_decoder_configure_options;
using plugin_common::validate_j2k_like_encoder_configure_options;
using plugin_common::write_plugin_encoded_output;

[[nodiscard]] bool is_htj2k_transfer_syntax_code(
    std::uint16_t transfer_syntax_code) noexcept {
  const auto transfer_syntax =
      abi::from_transfer_syntax_code(transfer_syntax_code);
  return transfer_syntax.has_value() && transfer_syntax->is_htj2k();
}

void* htj2k_decoder_create() noexcept {
  return new (std::nothrow) Htj2kDecoderPluginContext{};
}

void htj2k_decoder_destroy(void* ctx) noexcept {
  delete static_cast<Htj2kDecoderPluginContext*>(ctx);
}

int htj2k_decoder_configure(void* ctx, std::uint16_t transfer_syntax_code,
    const dicomsdl_codec_option_list_v1* options,
    dicomsdl_codec_error_v1* error) noexcept {
  auto* context = static_cast<Htj2kDecoderPluginContext*>(ctx);
  if (!context) {
    set_abi_error(error, DICOMSDL_CODEC_INTERNAL_ERROR,
        DICOMSDL_CODEC_STAGE_PLUGIN_LOOKUP, "decoder context is null");
    return 0;
  }
  if (!is_htj2k_transfer_syntax_code(transfer_syntax_code)) {
    set_abi_error(error, DICOMSDL_CODEC_UNSUPPORTED,
        DICOMSDL_CODEC_STAGE_VALIDATE,
        "transfer syntax is not a HTJ2K syntax");
    return 0;
  }

  plugin_common::Htj2kDecoderConfigureOptions parsed_options{};
  if (!parse_htj2k_decoder_configure_options(options, parsed_options, error)) {
    return 0;
  }
  if (!validate_htj2k_decoder_configure_options(parsed_options, error)) {
    return 0;
  }

  context->transfer_syntax_code = transfer_syntax_code;
  context->decoder_threads = parsed_options.decoder_threads;
  context->backend = parsed_options.backend;
  context->configured = true;
  set_abi_ok(error);
  return 1;
}

int htj2k_decoder_decode_frame(void* ctx,
    const dicomsdl_decoder_request_v1* request,
    dicomsdl_codec_error_v1* error) noexcept {
  auto* context = static_cast<Htj2kDecoderPluginContext*>(ctx);
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
  if (!prepare_plugin_decode_request(*request, context->transfer_syntax_code,
          context->decoder_threads, "request transfer syntax is not a HTJ2K syntax",
          "HTJ2K decoder plugin supports destination dtype == source dtype or f32",
          is_htj2k_transfer_syntax_code, prepared, error)) {
    return 0;
  }

  CodecError decode_error{};
  if (!decode_htj2k_into(prepared.info, prepared.value_transform,
          prepared.destination, prepared.strides, prepared.options, decode_error,
          prepared.source, context->backend)) {
    set_abi_error_from_codec_error(error, decode_error, "decode_frame",
        "HTJ2K decoder backend failed");
    return 0;
  }

  set_abi_ok(error);
  return 1;
}

void* htj2k_encoder_create() noexcept {
  return new (std::nothrow) Htj2kEncoderPluginContext{};
}

void htj2k_encoder_destroy(void* ctx) noexcept {
  delete static_cast<Htj2kEncoderPluginContext*>(ctx);
}

int htj2k_encoder_configure(void* ctx, std::uint16_t transfer_syntax_code,
    const dicomsdl_codec_option_list_v1* options,
    dicomsdl_codec_error_v1* error) noexcept {
  auto* context = static_cast<Htj2kEncoderPluginContext*>(ctx);
  if (!context) {
    set_abi_error(error, DICOMSDL_CODEC_INTERNAL_ERROR,
        DICOMSDL_CODEC_STAGE_PLUGIN_LOOKUP, "encoder context is null");
    return 0;
  }
  if (!is_htj2k_transfer_syntax_code(transfer_syntax_code)) {
    set_abi_error(error, DICOMSDL_CODEC_UNSUPPORTED,
        DICOMSDL_CODEC_STAGE_VALIDATE,
        "transfer syntax is not a HTJ2K syntax");
    return 0;
  }

  plugin_common::J2kLikeEncoderConfigureOptions parsed_options{};
  if (!parse_j2k_like_encoder_configure_options(options,
          "HTJ2K option key/value is null",
          "unknown HTJ2K option key: ", parsed_options, error)) {
    return 0;
  }
  if (!validate_j2k_like_encoder_configure_options(
          parsed_options, "Htj2kOptions", error)) {
    return 0;
  }

  Htj2kOptions configured_options{};
  configured_options.target_bpp = parsed_options.target_bpp;
  configured_options.target_psnr = parsed_options.target_psnr;
  configured_options.threads = parsed_options.threads;
  configured_options.use_color_transform = parsed_options.use_color_transform;

  context->transfer_syntax_code = transfer_syntax_code;
  context->options = configured_options;
  context->configured = true;
  set_abi_ok(error);
  return 1;
}

int htj2k_encoder_encode_frame(void* ctx,
    const dicomsdl_encoder_request_v1* request,
    dicomsdl_codec_error_v1* error) noexcept {
  auto* context = static_cast<Htj2kEncoderPluginContext*>(ctx);
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
          "request transfer syntax is not a HTJ2K syntax",
          is_htj2k_transfer_syntax_code, prepared, error)) {
    return 0;
  }
  bool rpcl_progression = false;
  const auto profile =
      abi::from_profile_code(request->frame.codec_profile_code);
  if (profile.has_value() &&
      *profile == CodecProfile::htj2k_lossless_rpcl) {
    rpcl_progression = true;
  }

  std::vector<std::uint8_t> encoded{};
  CodecError encode_error{};
  if (!try_encode_htj2k_frame(prepared.source, prepared.rows, prepared.cols,
          prepared.samples_per_pixel, prepared.bytes_per_sample,
          request->frame.bits_allocated, request->frame.bits_stored,
          request->frame.pixel_representation, prepared.source_planar,
          prepared.row_stride, prepared.use_multicomponent_transform,
          prepared.lossless, rpcl_progression,
          context->options,
          encoded, encode_error)) {
    set_abi_error_from_codec_error(error, encode_error, "encode_frame",
        "HTJ2K encoder backend failed");
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
htj2k_decoder_plugin_api_v1() noexcept {
  static const auto api = [] {
    dicomsdl_decoder_plugin_api_v1 value{};
    value.struct_size = sizeof(dicomsdl_decoder_plugin_api_v1);
    value.abi_version = DICOMSDL_DECODER_PLUGIN_ABI_V1;
    value.info.struct_size = sizeof(dicomsdl_decoder_plugin_info_v1);
    value.info.abi_version = DICOMSDL_DECODER_PLUGIN_ABI_V1;
    value.info.plugin_key = "htj2k";
    value.info.display_name = "HTJ2K Builtin ABI Decoder";
    value.create = &htj2k_decoder_create;
    value.destroy = &htj2k_decoder_destroy;
    value.configure = &htj2k_decoder_configure;
    value.decode_frame = &htj2k_decoder_decode_frame;
    return value;
  }();
  return api;
}

[[nodiscard]] const dicomsdl_encoder_plugin_api_v1&
htj2k_encoder_plugin_api_v1() noexcept {
  static const auto api = [] {
    dicomsdl_encoder_plugin_api_v1 value{};
    value.struct_size = sizeof(dicomsdl_encoder_plugin_api_v1);
    value.abi_version = DICOMSDL_ENCODER_PLUGIN_ABI_V1;
    value.info.struct_size = sizeof(dicomsdl_encoder_plugin_info_v1);
    value.info.abi_version = DICOMSDL_ENCODER_PLUGIN_ABI_V1;
    value.info.plugin_key = "htj2k";
    value.info.display_name = "HTJ2K Builtin ABI Encoder";
    value.create = &htj2k_encoder_create;
    value.destroy = &htj2k_encoder_destroy;
    value.configure = &htj2k_encoder_configure;
    value.encode_frame = &htj2k_encoder_encode_frame;
    return value;
  }();
  return api;
}

bool invoke_htj2k_decoder_via_plugin_api(const CodecDecodeFrameInput& input,
    Htj2kDecoder backend, CodecError& out_error) noexcept {
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

  const auto& api = htj2k_decoder_plugin_api_v1();
  DecoderContextGuard guard{};
  guard.api = &api;
  guard.context = api.create ? api.create() : nullptr;
  if (guard.context == nullptr) {
    set_codec_error(out_error, CodecStatusCode::internal_error,
        "plugin_lookup", "HTJ2K decoder plugin create() returned null context");
    return false;
  }

  std::string threads_value = std::to_string(input.options.decoder_threads);
  dicomsdl_codec_option_kv_v1 option_items[2] = {
      dicomsdl_codec_option_kv_v1{
          "backend",
          htj2k_decoder_backend_option_value(backend)},
      dicomsdl_codec_option_kv_v1{"threads", threads_value.c_str()},
  };
  dicomsdl_codec_option_list_v1 option_list{};
  option_list.items = option_items;
  option_list.count = 2u;

  char detail_buffer[1024];
  dicomsdl_codec_error_v1 plugin_error{};
  abi::initialize_codec_error_buffer(plugin_error, detail_buffer,
      static_cast<std::uint32_t>(sizeof(detail_buffer)));
  if (!api.configure(guard.context, request.frame.transfer_syntax_code,
          &option_list, &plugin_error)) {
    out_error = abi::decode_plugin_error_v1(
        plugin_error, "parse_options", "HTJ2K decoder configure failed");
    if (out_error.code == CodecStatusCode::ok) {
      out_error.code = CodecStatusCode::backend_error;
    }
    return false;
  }

  abi::initialize_codec_error_buffer(plugin_error, detail_buffer,
      static_cast<std::uint32_t>(sizeof(detail_buffer)));
  if (!api.decode_frame(guard.context, &request, &plugin_error)) {
    out_error = abi::decode_plugin_error_v1(
        plugin_error, "decode_frame", "HTJ2K decoder failed");
    if (out_error.code == CodecStatusCode::ok) {
      out_error.code = CodecStatusCode::backend_error;
    }
    return false;
  }
  return true;
}

bool invoke_htj2k_encoder_via_plugin_api(const CodecEncodeFrameInput& input,
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

  const auto& api = htj2k_encoder_plugin_api_v1();
  EncoderContextGuard guard{};
  guard.api = &api;
  guard.context = api.create ? api.create() : nullptr;
  if (guard.context == nullptr) {
    set_codec_error(out_error, CodecStatusCode::internal_error, "plugin_lookup",
        "HTJ2K encoder plugin create() returned null context");
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
        plugin_error, "parse_options", "HTJ2K encoder configure failed");
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
            "HTJ2K encoder returned encoded_size larger than output buffer");
        out_encoded_frame.clear();
        return false;
      }
      out_encoded_frame.resize(
          static_cast<std::size_t>(request.output.encoded_size));
      return true;
    }

    if (plugin_error.status_code != DICOMSDL_CODEC_OUTPUT_TOO_SMALL) {
      out_error = abi::decode_plugin_error_v1(
          plugin_error, "encode_frame", "HTJ2K encoder failed");
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
      "HTJ2K encoder repeatedly returned OUTPUT_TOO_SMALL");
  out_encoded_frame.clear();
  return false;
}

}  // namespace

namespace {

bool decode_frame_plugin_htj2k_via_abi_with_backend(
    const CodecDecodeFrameInput& input, Htj2kDecoder backend,
    CodecError& out_error) noexcept {
  try {
    if (!input.info.has_pixel_data) {
      set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
          "sv_dtype is unknown");
      return false;
    }
    return invoke_htj2k_decoder_via_plugin_api(input, backend, out_error);
  } catch (const std::exception& e) {
    set_codec_error(out_error, CodecStatusCode::backend_error, "decode_frame",
        e.what());
  } catch (...) {
    set_codec_error(out_error, CodecStatusCode::backend_error, "decode_frame",
        "non-standard exception");
  }
  return false;
}

} // namespace

const dicomsdl_decoder_plugin_api_v1&
htj2k_decoder_plugin_api_for_shared() noexcept {
  return htj2k_decoder_plugin_api_v1();
}

const dicomsdl_encoder_plugin_api_v1&
htj2k_encoder_plugin_api_for_shared() noexcept {
  return htj2k_encoder_plugin_api_v1();
}

bool decode_frame_plugin_htj2k_via_abi(
    const CodecDecodeFrameInput& input, CodecError& out_error) noexcept {
  return decode_frame_plugin_htj2k_via_abi_auto(input, out_error);
}

bool decode_frame_plugin_htj2k_via_abi_auto(
    const CodecDecodeFrameInput& input, CodecError& out_error) noexcept {
  return decode_frame_plugin_htj2k_via_abi_with_backend(
      input, Htj2kDecoder::auto_select, out_error);
}

bool decode_frame_plugin_htj2k_via_abi_openjph(
    const CodecDecodeFrameInput& input, CodecError& out_error) noexcept {
  return decode_frame_plugin_htj2k_via_abi_with_backend(
      input, Htj2kDecoder::openjph, out_error);
}

bool decode_frame_plugin_htj2k_via_abi_openjpeg(
    const CodecDecodeFrameInput& input, CodecError& out_error) noexcept {
  return decode_frame_plugin_htj2k_via_abi_with_backend(
      input, Htj2kDecoder::openjpeg, out_error);
}

codec_decode_frame_fn htj2k_decode_dispatch_for_backend(
    Htj2kDecoder backend) noexcept {
  switch (backend) {
  case Htj2kDecoder::openjph:
    return &decode_frame_plugin_htj2k_via_abi_openjph;
  case Htj2kDecoder::openjpeg:
    return &decode_frame_plugin_htj2k_via_abi_openjpeg;
  case Htj2kDecoder::auto_select:
  default:
    return &decode_frame_plugin_htj2k_via_abi_auto;
  }
}

Htj2kDecoder htj2k_decoder_backend_for_dispatch(
    codec_decode_frame_fn decode_dispatch) noexcept {
  if (decode_dispatch == &decode_frame_plugin_htj2k_via_abi_openjph) {
    return Htj2kDecoder::openjph;
  }
  if (decode_dispatch == &decode_frame_plugin_htj2k_via_abi_openjpeg) {
    return Htj2kDecoder::openjpeg;
  }
  return Htj2kDecoder::auto_select;
}

bool is_builtin_htj2k_decode_dispatch(
    codec_decode_frame_fn decode_dispatch) noexcept {
  return decode_dispatch == &decode_frame_plugin_htj2k_via_abi ||
      decode_dispatch == &decode_frame_plugin_htj2k_via_abi_auto ||
      decode_dispatch == &decode_frame_plugin_htj2k_via_abi_openjph ||
      decode_dispatch == &decode_frame_plugin_htj2k_via_abi_openjpeg;
}

bool encode_frame_plugin_htj2k_via_abi(const CodecEncodeFrameInput& input,
    std::span<const CodecOptionKv> encode_options,
    std::vector<std::uint8_t>& out_encoded_frame,
    CodecError& out_error) noexcept {
  try {
    return invoke_htj2k_encoder_via_plugin_api(
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
