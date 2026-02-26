#include "pixel_codec_plugin_jpegls_builtin.hpp"

#include "pixel_codec_plugin_abi_adapter.hpp"
#include "pixel_codec_option_bridge.hpp"
#include "pixel_decoder_detail.hpp"
#include "pixel_encoder_detail.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
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
constexpr int kDefaultNearLosslessError = 0;

struct JpegLsDecoderPluginContext {
  std::uint16_t transfer_syntax_code{DICOMSDL_TRANSFER_SYNTAX_CODE_INVALID};
  bool configured{false};
};

struct JpegLsEncoderPluginContext {
  std::uint16_t transfer_syntax_code{DICOMSDL_TRANSFER_SYNTAX_CODE_INVALID};
  int near_lossless_error{kDefaultNearLosslessError};
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

[[nodiscard]] constexpr std::uint32_t to_abi_status_code(
    CodecStatusCode code) noexcept {
  switch (code) {
  case CodecStatusCode::ok:
    return DICOMSDL_CODEC_OK;
  case CodecStatusCode::invalid_argument:
    return DICOMSDL_CODEC_INVALID_ARGUMENT;
  case CodecStatusCode::unsupported:
    return DICOMSDL_CODEC_UNSUPPORTED;
  case CodecStatusCode::internal_error:
    return DICOMSDL_CODEC_INTERNAL_ERROR;
  case CodecStatusCode::backend_error:
  default:
    return DICOMSDL_CODEC_BACKEND_ERROR;
  }
}

[[nodiscard]] constexpr std::uint32_t to_abi_stage_code(
    std::string_view stage) noexcept {
  if (stage == "plugin_lookup") {
    return DICOMSDL_CODEC_STAGE_PLUGIN_LOOKUP;
  }
  if (stage == "parse_options") {
    return DICOMSDL_CODEC_STAGE_PARSE_OPTIONS;
  }
  if (stage == "validate") {
    return DICOMSDL_CODEC_STAGE_VALIDATE;
  }
  if (stage == "load_frame_source") {
    return DICOMSDL_CODEC_STAGE_LOAD_FRAME_SOURCE;
  }
  if (stage == "encode_frame" || stage == "encode") {
    return DICOMSDL_CODEC_STAGE_ENCODE_FRAME;
  }
  if (stage == "decode_frame" || stage == "decode") {
    return DICOMSDL_CODEC_STAGE_DECODE_FRAME;
  }
  if (stage == "postprocess") {
    return DICOMSDL_CODEC_STAGE_POSTPROCESS;
  }
  if (stage == "allocate") {
    return DICOMSDL_CODEC_STAGE_ALLOCATE;
  }
  return DICOMSDL_CODEC_STAGE_UNKNOWN;
}

void set_abi_error(dicomsdl_codec_error_v1* error, std::uint32_t status_code,
    std::uint32_t stage_code, std::string_view detail) noexcept {
  if (!error) {
    return;
  }
  error->status_code = status_code;
  error->stage_code = stage_code;
  error->detail_length = 0;
  if (!error->detail || error->detail_capacity == 0) {
    return;
  }

  const std::size_t copy_size = std::min<std::size_t>(
      detail.size(), static_cast<std::size_t>(error->detail_capacity - 1));
  if (copy_size > 0) {
    std::memcpy(error->detail, detail.data(), copy_size);
  }
  error->detail[copy_size] = '\0';
  error->detail_length = static_cast<std::uint32_t>(copy_size);
}

void set_abi_ok(dicomsdl_codec_error_v1* error) noexcept {
  set_abi_error(error, DICOMSDL_CODEC_OK, DICOMSDL_CODEC_STAGE_UNKNOWN, "");
}

void set_abi_error_from_codec_error(
    dicomsdl_codec_error_v1* error, const CodecError& in_error,
    std::string_view fallback_stage,
    std::string_view fallback_detail) noexcept {
  CodecError effective = in_error;
  if (effective.code == CodecStatusCode::ok) {
    effective.code = CodecStatusCode::backend_error;
  }
  if (effective.stage.empty()) {
    effective.stage = std::string(fallback_stage);
  }
  if (effective.detail.empty()) {
    effective.detail = std::string(fallback_detail);
  }
  set_abi_error(error, to_abi_status_code(effective.code),
      to_abi_stage_code(effective.stage), effective.detail);
}

[[nodiscard]] bool checked_u64_to_size_t(
    std::uint64_t value, std::size_t& out) noexcept {
  if (value > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return false;
  }
  out = static_cast<std::size_t>(value);
  return true;
}

[[nodiscard]] bool checked_positive_i32_to_size_t(
    std::int32_t value, std::size_t& out) noexcept {
  if (value <= 0) {
    return false;
  }
  out = static_cast<std::size_t>(value);
  return true;
}

[[nodiscard]] bool is_jpegls_transfer_syntax_code(
    std::uint16_t transfer_syntax_code) noexcept {
  const auto transfer_syntax =
      abi::from_transfer_syntax_code(transfer_syntax_code);
  return transfer_syntax.has_value() && transfer_syntax->is_jpegls();
}

[[nodiscard]] bool is_option_separator(char c) noexcept {
  return c == '_' || c == '-' || c == ' ' || c == '\t';
}

[[nodiscard]] char normalized_key_char(char c) noexcept {
  return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

[[nodiscard]] bool option_key_matches(
    std::string_view input, std::string_view expected) noexcept {
  std::size_t input_index = 0;
  std::size_t expected_index = 0;
  while (true) {
    while (input_index < input.size() &&
           is_option_separator(input[input_index])) {
      ++input_index;
    }
    while (expected_index < expected.size() &&
           is_option_separator(expected[expected_index])) {
      ++expected_index;
    }
    if (input_index == input.size() || expected_index == expected.size()) {
      break;
    }
    if (normalized_key_char(input[input_index]) !=
        normalized_key_char(expected[expected_index])) {
      return false;
    }
    ++input_index;
    ++expected_index;
  }
  while (input_index < input.size() && is_option_separator(input[input_index])) {
    ++input_index;
  }
  while (expected_index < expected.size() &&
         is_option_separator(expected[expected_index])) {
    ++expected_index;
  }
  return input_index == input.size() && expected_index == expected.size();
}

[[nodiscard]] bool parse_strict_int_option(
    std::string_view text, int& out_value) noexcept {
  long long parsed = 0;
  const auto* begin = text.data();
  const auto* end = text.data() + text.size();
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc{} || result.ptr != end) {
    return false;
  }
  if (parsed < static_cast<long long>(std::numeric_limits<int>::min()) ||
      parsed > static_cast<long long>(std::numeric_limits<int>::max())) {
    return false;
  }
  out_value = static_cast<int>(parsed);
  return true;
}

[[nodiscard]] bool decoder_request_has_value_transform(
    const dicomsdl_decoder_request_v1& request) noexcept {
  const std::size_t required_size =
      offsetof(dicomsdl_decoder_request_v1, value_transform) +
      sizeof(dicomsdl_decoder_value_transform_v1);
  return static_cast<std::size_t>(request.struct_size) >= required_size;
}

[[nodiscard]] bool decode_value_transform_from_request(
    const dicomsdl_decoder_request_v1& request,
    DecodeValueTransform& out_value_transform,
    CodecError& out_error) noexcept {
  out_value_transform = {};
  if (!decoder_request_has_value_transform(request)) {
    return true;
  }

  const auto& in_transform = request.value_transform;
  switch (in_transform.transform_kind) {
  case DICOMSDL_DECODER_VALUE_TRANSFORM_NONE:
    return true;
  case DICOMSDL_DECODER_VALUE_TRANSFORM_RESCALE:
    out_value_transform.enabled = true;
    out_value_transform.rescale_slope = in_transform.rescale_slope;
    out_value_transform.rescale_intercept = in_transform.rescale_intercept;
    return true;
  case DICOMSDL_DECODER_VALUE_TRANSFORM_MODALITY_LUT: {
    std::size_t lut_byte_size = 0;
    std::size_t lut_value_count = 0;
    if (!checked_u64_to_size_t(in_transform.lut_values_f32.size, lut_byte_size) ||
        !checked_u64_to_size_t(in_transform.lut_value_count, lut_value_count)) {
      set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
          "Modality LUT size exceeds host size_t range");
      return false;
    }
    if (lut_byte_size % sizeof(float) != 0) {
      set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
          "Modality LUT byte size is not a multiple of float size");
      return false;
    }
    if (lut_byte_size > 0 && in_transform.lut_values_f32.data == nullptr) {
      set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
          "Modality LUT values pointer is null");
      return false;
    }
    if (lut_value_count != (lut_byte_size / sizeof(float))) {
      set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
          "Modality LUT value_count does not match value byte size");
      return false;
    }

    pixel::ModalityLut lut{};
    lut.first_mapped = in_transform.lut_first_mapped;
    try {
      lut.values.resize(lut_value_count);
    } catch (const std::bad_alloc&) {
      set_codec_error(out_error, CodecStatusCode::internal_error, "allocate",
          "memory allocation failed");
      return false;
    }
    if (lut_byte_size > 0) {
      std::memcpy(lut.values.data(), in_transform.lut_values_f32.data, lut_byte_size);
    }
    out_value_transform.enabled = true;
    out_value_transform.modality_lut = std::move(lut);
    return true;
  }
  default:
    set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
        "decoder value transform kind is invalid");
    return false;
  }
}

void* jpegls_decoder_create() noexcept {
  return new (std::nothrow) JpegLsDecoderPluginContext{};
}

void jpegls_decoder_destroy(void* ctx) noexcept {
  delete static_cast<JpegLsDecoderPluginContext*>(ctx);
}

int jpegls_decoder_configure(void* ctx, std::uint16_t transfer_syntax_code,
    const dicomsdl_codec_option_list_v1* options,
    dicomsdl_codec_error_v1* error) noexcept {
  auto* context = static_cast<JpegLsDecoderPluginContext*>(ctx);
  if (!context) {
    set_abi_error(error, DICOMSDL_CODEC_INTERNAL_ERROR,
        DICOMSDL_CODEC_STAGE_PLUGIN_LOOKUP, "decoder context is null");
    return 0;
  }
  if (options && options->count > 0) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_PARSE_OPTIONS,
        "JPEG-LS decoder does not accept configure options");
    return 0;
  }
  if (!is_jpegls_transfer_syntax_code(transfer_syntax_code)) {
    set_abi_error(error, DICOMSDL_CODEC_UNSUPPORTED,
        DICOMSDL_CODEC_STAGE_VALIDATE,
        "transfer syntax is not a JPEG-LS syntax");
    return 0;
  }

  context->transfer_syntax_code = transfer_syntax_code;
  context->configured = true;
  set_abi_ok(error);
  return 1;
}

int jpegls_decoder_decode_frame(void* ctx,
    const dicomsdl_decoder_request_v1* request,
    dicomsdl_codec_error_v1* error) noexcept {
  auto* context = static_cast<JpegLsDecoderPluginContext*>(ctx);
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
  if (!is_jpegls_transfer_syntax_code(request->frame.transfer_syntax_code)) {
    set_abi_error(error, DICOMSDL_CODEC_UNSUPPORTED, DICOMSDL_CODEC_STAGE_VALIDATE,
        "request transfer syntax is not a JPEG-LS syntax");
    return 0;
  }
  if (request->frame.transfer_syntax_code != context->transfer_syntax_code) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_VALIDATE,
        "request transfer syntax differs from configured transfer syntax");
    return 0;
  }

  const auto source_dtype = abi::from_dtype_code(request->frame.source_dtype);
  if (!source_dtype || *source_dtype == DataType::unknown) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_VALIDATE, "source_dtype is invalid");
    return 0;
  }
  const auto source_planar = abi::from_planar_code(request->frame.source_planar);
  if (!source_planar) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_VALIDATE, "source_planar is invalid");
    return 0;
  }
  const auto dst_planar = abi::from_planar_code(request->output.dst_planar);
  if (!dst_planar) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_VALIDATE, "dst_planar is invalid");
    return 0;
  }
  const auto dst_dtype = abi::from_dtype_code(request->output.dst_dtype);
  if (!dst_dtype || *dst_dtype == DataType::unknown) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_VALIDATE, "dst_dtype is invalid");
    return 0;
  }
  const bool scaled_output = (*dst_dtype == DataType::f32);
  if (*dst_dtype != *source_dtype && !scaled_output) {
    set_abi_error(error, DICOMSDL_CODEC_UNSUPPORTED, DICOMSDL_CODEC_STAGE_VALIDATE,
        "JPEG-LS decoder plugin supports destination dtype == source dtype or f32");
    return 0;
  }

  std::size_t source_size = 0;
  std::size_t dst_size = 0;
  std::size_t row_stride = 0;
  std::size_t frame_stride = 0;
  if (!checked_u64_to_size_t(request->source.source_buffer.size, source_size) ||
      !checked_u64_to_size_t(request->output.dst_size, dst_size) ||
      !checked_u64_to_size_t(request->output.row_stride, row_stride) ||
      !checked_u64_to_size_t(request->output.frame_stride, frame_stride)) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_VALIDATE,
        "request size/stride exceeds host size_t range");
    return 0;
  }
  if (source_size > 0 && request->source.source_buffer.data == nullptr) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_VALIDATE, "source_buffer.data is null");
    return 0;
  }
  if (dst_size > 0 && request->output.dst == nullptr) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_VALIDATE, "output.dst is null");
    return 0;
  }

  pixel::PixelDataInfo info{};
  const auto transfer_syntax =
      abi::from_transfer_syntax_code(request->frame.transfer_syntax_code);
  if (!transfer_syntax) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_VALIDATE, "transfer syntax code is invalid");
    return 0;
  }
  info.ts = *transfer_syntax;
  info.sv_dtype = *source_dtype;
  info.rows = request->frame.rows;
  info.cols = request->frame.cols;
  info.frames = 1;
  info.samples_per_pixel = request->frame.samples_per_pixel;
  info.planar_configuration = *source_planar;
  info.bits_stored = request->frame.bits_stored;
  info.has_pixel_data = true;

  DecodeValueTransform value_transform{};
  CodecError decode_error{};
  if (!decode_value_transform_from_request(*request, value_transform, decode_error)) {
    set_abi_error_from_codec_error(error, decode_error, "validate",
        "failed to parse decoder value transform");
    return 0;
  }
  if (scaled_output && !value_transform.enabled) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_VALIDATE,
        "scaled output requires a decoder value transform");
    return 0;
  }

  DecodeOptions options{};
  options.planar_out = *dst_planar;
  options.scaled = scaled_output;
  options.decode_mct = request->frame.decode_mct != 0;

  const auto source =
      std::span<const std::uint8_t>(request->source.source_buffer.data, source_size);
  const auto destination = std::span<std::uint8_t>(request->output.dst, dst_size);
  const DecodeStrides strides{.row = row_stride, .frame = frame_stride};
  if (!decode_jpegls_into(info, value_transform, destination, strides, options,
          decode_error, source)) {
    set_abi_error_from_codec_error(error, decode_error, "decode_frame",
        "JPEG-LS decoder backend failed");
    return 0;
  }

  set_abi_ok(error);
  return 1;
}

void* jpegls_encoder_create() noexcept {
  return new (std::nothrow) JpegLsEncoderPluginContext{};
}

void jpegls_encoder_destroy(void* ctx) noexcept {
  delete static_cast<JpegLsEncoderPluginContext*>(ctx);
}

int jpegls_encoder_configure(void* ctx, std::uint16_t transfer_syntax_code,
    const dicomsdl_codec_option_list_v1* options,
    dicomsdl_codec_error_v1* error) noexcept {
  auto* context = static_cast<JpegLsEncoderPluginContext*>(ctx);
  if (!context) {
    set_abi_error(error, DICOMSDL_CODEC_INTERNAL_ERROR,
        DICOMSDL_CODEC_STAGE_PLUGIN_LOOKUP, "encoder context is null");
    return 0;
  }
  if (!is_jpegls_transfer_syntax_code(transfer_syntax_code)) {
    set_abi_error(error, DICOMSDL_CODEC_UNSUPPORTED,
        DICOMSDL_CODEC_STAGE_VALIDATE,
        "transfer syntax is not a JPEG-LS syntax");
    return 0;
  }

  int near_lossless_error = kDefaultNearLosslessError;
  bool near_lossless_seen = false;
  if (options) {
    for (std::uint32_t i = 0; i < options->count; ++i) {
      const auto& item = options->items[i];
      if (!item.key || !item.value) {
        set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
            DICOMSDL_CODEC_STAGE_PARSE_OPTIONS,
            "JPEG-LS option key/value is null");
        return 0;
      }
      const std::string_view key(item.key);
      if (!option_key_matches(key, "near_lossless_error")) {
        set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
            DICOMSDL_CODEC_STAGE_PARSE_OPTIONS,
            std::string("unknown JPEG-LS option key: ") + std::string(key));
        return 0;
      }
      if (near_lossless_seen) {
        set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
            DICOMSDL_CODEC_STAGE_PARSE_OPTIONS,
            "duplicate JPEG-LS option key: near_lossless_error");
        return 0;
      }
      if (!parse_strict_int_option(
              std::string_view(item.value), near_lossless_error)) {
        set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
            DICOMSDL_CODEC_STAGE_PARSE_OPTIONS,
            "near_lossless_error must be an integer in [0,255]");
        return 0;
      }
      near_lossless_seen = true;
    }
  }

  if (near_lossless_error < 0 || near_lossless_error > 255) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_PARSE_OPTIONS,
        "JpegLsOptions.near_lossless_error must be in [0,255]");
    return 0;
  }
  const auto transfer_syntax =
      abi::from_transfer_syntax_code(transfer_syntax_code);
  if (!transfer_syntax) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_VALIDATE, "transfer syntax code is invalid");
    return 0;
  }
  if (transfer_syntax->is_lossless() && near_lossless_error != 0) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_PARSE_OPTIONS,
        "JPEG-LS lossless transfer syntax requires near_lossless_error=0");
    return 0;
  }
  if (transfer_syntax->is_lossy() && near_lossless_error <= 0) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_PARSE_OPTIONS,
        "JPEG-LS lossy transfer syntax requires near_lossless_error>0");
    return 0;
  }

  context->transfer_syntax_code = transfer_syntax_code;
  context->near_lossless_error = near_lossless_error;
  context->configured = true;
  set_abi_ok(error);
  return 1;
}

int jpegls_encoder_encode_frame(void* ctx,
    const dicomsdl_encoder_request_v1* request,
    dicomsdl_codec_error_v1* error) noexcept {
  auto* context = static_cast<JpegLsEncoderPluginContext*>(ctx);
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
  if (!is_jpegls_transfer_syntax_code(request->frame.transfer_syntax_code)) {
    set_abi_error(error, DICOMSDL_CODEC_UNSUPPORTED, DICOMSDL_CODEC_STAGE_VALIDATE,
        "request transfer syntax is not a JPEG-LS syntax");
    return 0;
  }
  if (request->frame.transfer_syntax_code != context->transfer_syntax_code) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_VALIDATE,
        "request transfer syntax differs from configured transfer syntax");
    return 0;
  }

  const auto source_dtype = abi::from_dtype_code(request->frame.source_dtype);
  if (!source_dtype || *source_dtype == DataType::unknown) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_VALIDATE, "source_dtype is invalid");
    return 0;
  }
  const auto source_planar = abi::from_planar_code(request->frame.source_planar);
  if (!source_planar) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_VALIDATE, "source_planar is invalid");
    return 0;
  }

  std::size_t rows = 0;
  std::size_t cols = 0;
  std::size_t samples_per_pixel = 0;
  std::size_t source_size = 0;
  std::size_t row_stride = 0;
  std::size_t encoded_capacity = 0;
  if (!checked_positive_i32_to_size_t(request->frame.rows, rows) ||
      !checked_positive_i32_to_size_t(request->frame.cols, cols) ||
      !checked_positive_i32_to_size_t(
          request->frame.samples_per_pixel, samples_per_pixel) ||
      !checked_u64_to_size_t(request->source.source_buffer.size, source_size) ||
      !checked_u64_to_size_t(request->frame.source_row_stride, row_stride) ||
      !checked_u64_to_size_t(request->output.encoded_buffer.size, encoded_capacity)) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_VALIDATE,
        "request numeric field is invalid or exceeds size_t range");
    return 0;
  }
  if (source_size > 0 && request->source.source_buffer.data == nullptr) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_VALIDATE, "source_buffer.data is null");
    return 0;
  }

  const auto bytes_per_sample = sv_dtype_bytes(*source_dtype);
  if (bytes_per_sample == 0) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_VALIDATE, "source_dtype is not byte-addressable");
    return 0;
  }

  std::vector<std::uint8_t> encoded{};
  CodecError encode_error{};
  if (!try_encode_jpegls_frame(
          std::span<const std::uint8_t>(request->source.source_buffer.data, source_size),
          rows, cols, samples_per_pixel, bytes_per_sample,
          request->frame.bits_allocated, request->frame.bits_stored,
          *source_planar, row_stride, context->near_lossless_error,
          encoded, encode_error)) {
    set_abi_error_from_codec_error(error, encode_error, "encode_frame",
        "JPEG-LS encoder backend failed");
    return 0;
  }

  auto* mutable_request = const_cast<dicomsdl_encoder_request_v1*>(request);
  mutable_request->output.encoded_size =
      static_cast<std::uint64_t>(encoded.size());
  if (encoded.size() > encoded_capacity ||
      mutable_request->output.encoded_buffer.data == nullptr) {
    set_abi_error(error, DICOMSDL_CODEC_OUTPUT_TOO_SMALL,
        DICOMSDL_CODEC_STAGE_ENCODE_FRAME, "output buffer too small");
    return 0;
  }

  std::memcpy(mutable_request->output.encoded_buffer.data, encoded.data(),
      encoded.size());
  set_abi_ok(error);
  return 1;
}

[[nodiscard]] const dicomsdl_decoder_plugin_api_v1&
jpegls_decoder_plugin_api_v1() noexcept {
  static const auto api = [] {
    dicomsdl_decoder_plugin_api_v1 value{};
    value.struct_size = sizeof(dicomsdl_decoder_plugin_api_v1);
    value.abi_version = DICOMSDL_DECODER_PLUGIN_ABI_V1;
    value.info.struct_size = sizeof(dicomsdl_decoder_plugin_info_v1);
    value.info.abi_version = DICOMSDL_DECODER_PLUGIN_ABI_V1;
    value.info.plugin_key = "jpegls";
    value.info.display_name = "JPEG-LS Builtin ABI Decoder";
    value.create = &jpegls_decoder_create;
    value.destroy = &jpegls_decoder_destroy;
    value.configure = &jpegls_decoder_configure;
    value.decode_frame = &jpegls_decoder_decode_frame;
    return value;
  }();
  return api;
}

[[nodiscard]] const dicomsdl_encoder_plugin_api_v1&
jpegls_encoder_plugin_api_v1() noexcept {
  static const auto api = [] {
    dicomsdl_encoder_plugin_api_v1 value{};
    value.struct_size = sizeof(dicomsdl_encoder_plugin_api_v1);
    value.abi_version = DICOMSDL_ENCODER_PLUGIN_ABI_V1;
    value.info.struct_size = sizeof(dicomsdl_encoder_plugin_info_v1);
    value.info.abi_version = DICOMSDL_ENCODER_PLUGIN_ABI_V1;
    value.info.plugin_key = "jpegls";
    value.info.display_name = "JPEG-LS Builtin ABI Encoder";
    value.create = &jpegls_encoder_create;
    value.destroy = &jpegls_encoder_destroy;
    value.configure = &jpegls_encoder_configure;
    value.encode_frame = &jpegls_encoder_encode_frame;
    return value;
  }();
  return api;
}

bool invoke_jpegls_decoder_via_plugin_api(
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

  const auto& api = jpegls_decoder_plugin_api_v1();
  DecoderContextGuard guard{};
  guard.api = &api;
  guard.context = api.create ? api.create() : nullptr;
  if (guard.context == nullptr) {
    set_codec_error(out_error, CodecStatusCode::internal_error,
        "plugin_lookup", "JPEG-LS decoder plugin create() returned null context");
    return false;
  }

  char detail_buffer[1024];
  dicomsdl_codec_error_v1 plugin_error{};
  abi::initialize_codec_error_buffer(plugin_error, detail_buffer,
      static_cast<std::uint32_t>(sizeof(detail_buffer)));
  if (!api.configure(guard.context, request.frame.transfer_syntax_code,
          nullptr, &plugin_error)) {
    out_error = abi::decode_plugin_error_v1(
        plugin_error, "parse_options", "JPEG-LS decoder configure failed");
    if (out_error.code == CodecStatusCode::ok) {
      out_error.code = CodecStatusCode::backend_error;
    }
    return false;
  }

  abi::initialize_codec_error_buffer(plugin_error, detail_buffer,
      static_cast<std::uint32_t>(sizeof(detail_buffer)));
  if (!api.decode_frame(guard.context, &request, &plugin_error)) {
    out_error = abi::decode_plugin_error_v1(
        plugin_error, "decode_frame", "JPEG-LS decoder failed");
    if (out_error.code == CodecStatusCode::ok) {
      out_error.code = CodecStatusCode::backend_error;
    }
    return false;
  }
  return true;
}

bool invoke_jpegls_encoder_via_plugin_api(const CodecEncodeFrameInput& input,
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

  const auto& api = jpegls_encoder_plugin_api_v1();
  EncoderContextGuard guard{};
  guard.api = &api;
  guard.context = api.create ? api.create() : nullptr;
  if (guard.context == nullptr) {
    set_codec_error(out_error, CodecStatusCode::internal_error, "plugin_lookup",
        "JPEG-LS encoder plugin create() returned null context");
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
        plugin_error, "parse_options", "JPEG-LS encoder configure failed");
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
            "JPEG-LS encoder returned encoded_size larger than output buffer");
        out_encoded_frame.clear();
        return false;
      }
      out_encoded_frame.resize(
          static_cast<std::size_t>(request.output.encoded_size));
      return true;
    }

    if (plugin_error.status_code != DICOMSDL_CODEC_OUTPUT_TOO_SMALL) {
      out_error = abi::decode_plugin_error_v1(
          plugin_error, "encode_frame", "JPEG-LS encoder failed");
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
      "JPEG-LS encoder repeatedly returned OUTPUT_TOO_SMALL");
  out_encoded_frame.clear();
  return false;
}

}  // namespace

bool decode_frame_plugin_jpegls_via_abi(
    const CodecDecodeFrameInput& input, CodecError& out_error) noexcept {
  try {
    if (!input.info.has_pixel_data) {
      set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
          "sv_dtype is unknown");
      return false;
    }
    return invoke_jpegls_decoder_via_plugin_api(input, out_error);
  } catch (const std::exception& e) {
    set_codec_error(out_error, CodecStatusCode::backend_error, "decode_frame",
        e.what());
  } catch (...) {
    set_codec_error(out_error, CodecStatusCode::backend_error, "decode_frame",
        "non-standard exception");
  }
  return false;
}

bool encode_frame_plugin_jpegls_via_abi(const CodecEncodeFrameInput& input,
    std::span<const CodecOptionKv> encode_options,
    std::vector<std::uint8_t>& out_encoded_frame,
    CodecError& out_error) noexcept {
  try {
    return invoke_jpegls_encoder_via_plugin_api(
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
