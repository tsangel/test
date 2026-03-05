#pragma once

#include "codec_plugin_abi_codes.hpp"
#include "pixel_decoder_plugin_abi.h"
#include "pixel_encoder_plugin_abi.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>

namespace dicom::pixel::detail::abi {

[[nodiscard]] inline constexpr const std::uint8_t* nullable_data_ptr(
    std::span<const std::uint8_t> bytes) noexcept {
  return bytes.empty() ? nullptr : bytes.data();
}

[[nodiscard]] inline constexpr std::uint8_t* nullable_data_ptr(
    std::span<std::uint8_t> bytes) noexcept {
  return bytes.empty() ? nullptr : bytes.data();
}

[[nodiscard]] inline constexpr std::uint32_t bool_to_u32(bool value) noexcept {
  return value ? 1u : 0u;
}

[[nodiscard]] inline constexpr std::uint8_t infer_encoder_source_dtype_code(
    const CodecEncodeFrameInput& input) noexcept {
  const bool signed_samples = input.pixel_representation != 0;
  switch (input.bytes_per_sample) {
  case 1:
    return signed_samples ? DICOMSDL_DTYPE_S8 : DICOMSDL_DTYPE_U8;
  case 2:
    return signed_samples ? DICOMSDL_DTYPE_S16 : DICOMSDL_DTYPE_U16;
  case 4:
    return signed_samples ? DICOMSDL_DTYPE_S32 : DICOMSDL_DTYPE_U32;
  default:
    return DICOMSDL_DTYPE_UNKNOWN;
  }
}

[[nodiscard]] inline constexpr std::uint8_t decoder_output_dtype_code(
    const CodecDecodeFrameInput& input) noexcept {
  if (input.options.scaled) {
    return DICOMSDL_DTYPE_F32;
  }
  return to_dtype_code(input.info.sv_dtype);
}

[[nodiscard]] inline constexpr std::uint64_t lut_values_byte_size(
    std::size_t value_count) noexcept {
  if (value_count >
      (std::numeric_limits<std::uint64_t>::max)() / sizeof(float)) {
    return 0;
  }
  return static_cast<std::uint64_t>(value_count * sizeof(float));
}

inline void initialize_codec_error_buffer(dicomsdl_codec_error_v1& out_error,
    char* detail_buffer, std::uint32_t detail_capacity) noexcept {
  out_error.struct_size = sizeof(dicomsdl_codec_error_v1);
  out_error.abi_version = DICOMSDL_CODEC_PLUGIN_ABI_V1;
  out_error.status_code = DICOMSDL_CODEC_OK;
  out_error.stage_code = DICOMSDL_CODEC_STAGE_UNKNOWN;
  out_error.detail = detail_buffer;
  out_error.detail_capacity = detail_capacity;
  out_error.detail_length = 0;
  if (detail_buffer != nullptr && detail_capacity > 0) {
    detail_buffer[0] = '\0';
  }
}

inline void build_decoder_request_v1(const CodecDecodeFrameInput& input,
    dicomsdl_decoder_request_v1& out_request) noexcept {
  out_request = {};
  out_request.struct_size = sizeof(dicomsdl_decoder_request_v1);
  out_request.abi_version = DICOMSDL_DECODER_PLUGIN_ABI_V1;

  out_request.source.struct_size = sizeof(dicomsdl_decoder_source_v1);
  out_request.source.abi_version = DICOMSDL_DECODER_PLUGIN_ABI_V1;
  out_request.source.source_buffer.data = nullable_data_ptr(input.prepared_source);
  out_request.source.source_buffer.size =
      static_cast<std::uint64_t>(input.prepared_source.size());

  out_request.frame.struct_size = sizeof(dicomsdl_decoder_frame_info_v1);
  out_request.frame.abi_version = DICOMSDL_DECODER_PLUGIN_ABI_V1;
  out_request.frame.transfer_syntax_code = to_transfer_syntax_code(input.info.ts);
  out_request.frame.source_dtype = to_dtype_code(input.info.sv_dtype);
  out_request.frame.source_planar = to_planar_code(input.info.planar_configuration);
  out_request.frame.rows = input.info.rows;
  out_request.frame.cols = input.info.cols;
  out_request.frame.samples_per_pixel = input.info.samples_per_pixel;
  out_request.frame.bits_stored = input.info.bits_stored;
  out_request.frame.decode_mct = bool_to_u32(input.options.decode_mct);
  out_request.frame.reserved0 = 0;

  out_request.output.struct_size = sizeof(dicomsdl_decoder_output_v1);
  out_request.output.abi_version = DICOMSDL_DECODER_PLUGIN_ABI_V1;
  out_request.output.dst = nullable_data_ptr(input.destination);
  out_request.output.dst_size = static_cast<std::uint64_t>(input.destination.size());
  out_request.output.row_stride =
      static_cast<std::uint64_t>(input.destination_strides.row);
  out_request.output.frame_stride =
      static_cast<std::uint64_t>(input.destination_strides.frame);
  out_request.output.dst_dtype = decoder_output_dtype_code(input);
  out_request.output.dst_planar = to_planar_code(input.options.planar_out);
  out_request.output.reserved0 = 0;

  out_request.value_transform.struct_size =
      sizeof(dicomsdl_decoder_value_transform_v1);
  out_request.value_transform.abi_version = DICOMSDL_DECODER_PLUGIN_ABI_V1;
  out_request.value_transform.transform_kind =
      DICOMSDL_DECODER_VALUE_TRANSFORM_NONE;
  out_request.value_transform.reserved0 = 0;
  out_request.value_transform.rescale_slope = input.value_transform.rescale_slope;
  out_request.value_transform.rescale_intercept =
      input.value_transform.rescale_intercept;
  out_request.value_transform.lut_first_mapped = 0;
  out_request.value_transform.lut_value_count = 0;
  out_request.value_transform.lut_values_f32.data = nullptr;
  out_request.value_transform.lut_values_f32.size = 0;

  if (input.value_transform.enabled) {
    if (input.value_transform.modality_lut.has_value()) {
      const auto& lut = *input.value_transform.modality_lut;
      out_request.value_transform.transform_kind =
          DICOMSDL_DECODER_VALUE_TRANSFORM_MODALITY_LUT;
      out_request.value_transform.lut_first_mapped =
          static_cast<std::int64_t>(lut.first_mapped);
      out_request.value_transform.lut_value_count =
          static_cast<std::uint64_t>(lut.values.size());
      out_request.value_transform.lut_values_f32.data =
          reinterpret_cast<const std::uint8_t*>(
              lut.values.empty() ? nullptr : lut.values.data());
      out_request.value_transform.lut_values_f32.size =
          lut_values_byte_size(lut.values.size());
    } else {
      out_request.value_transform.transform_kind =
          DICOMSDL_DECODER_VALUE_TRANSFORM_RESCALE;
    }
  }
}

inline void build_encoder_request_v1(const CodecEncodeFrameInput& input,
    std::span<std::uint8_t> encoded_buffer, std::uint64_t encoded_size,
    dicomsdl_encoder_request_v1& out_request) noexcept {
  out_request = {};
  out_request.struct_size = sizeof(dicomsdl_encoder_request_v1);
  out_request.abi_version = DICOMSDL_ENCODER_PLUGIN_ABI_V1;

  out_request.source.struct_size = sizeof(dicomsdl_encoder_source_v1);
  out_request.source.abi_version = DICOMSDL_ENCODER_PLUGIN_ABI_V1;
  out_request.source.source_buffer.data = nullable_data_ptr(input.source_frame);
  out_request.source.source_buffer.size =
      static_cast<std::uint64_t>(input.source_frame.size());

  out_request.frame.struct_size = sizeof(dicomsdl_encoder_frame_info_v1);
  out_request.frame.abi_version = DICOMSDL_ENCODER_PLUGIN_ABI_V1;
  out_request.frame.transfer_syntax_code = to_transfer_syntax_code(input.transfer_syntax);
  out_request.frame.source_dtype = infer_encoder_source_dtype_code(input);
  out_request.frame.source_planar = to_planar_code(input.source_planar);
  out_request.frame.rows = static_cast<std::int32_t>(input.rows);
  out_request.frame.cols = static_cast<std::int32_t>(input.cols);
  out_request.frame.samples_per_pixel =
      static_cast<std::int32_t>(input.samples_per_pixel);
  out_request.frame.bits_allocated = input.bits_allocated;
  out_request.frame.bits_stored = input.bits_stored;
  out_request.frame.pixel_representation = input.pixel_representation;
  out_request.frame.source_row_stride =
      static_cast<std::uint64_t>(input.source_row_stride);
  out_request.frame.source_plane_stride =
      static_cast<std::uint64_t>(input.source_plane_stride);
  out_request.frame.source_frame_size_bytes =
      static_cast<std::uint64_t>(input.source_frame_size_bytes);
  out_request.frame.codec_profile_code = to_profile_code(input.profile);
  out_request.frame.use_multicomponent_transform =
      bool_to_u32(input.use_multicomponent_transform);

  out_request.output.struct_size = sizeof(dicomsdl_encoder_output_v1);
  out_request.output.abi_version = DICOMSDL_ENCODER_PLUGIN_ABI_V1;
  out_request.output.encoded_buffer.data = nullable_data_ptr(encoded_buffer);
  out_request.output.encoded_buffer.size =
      static_cast<std::uint64_t>(encoded_buffer.size());
  out_request.output.encoded_size = encoded_size;
}

[[nodiscard]] inline constexpr CodecStatusCode status_from_abi_code(
    std::uint32_t status_code) noexcept {
  switch (status_code) {
  case DICOMSDL_CODEC_OK:
    return CodecStatusCode::ok;
  case DICOMSDL_CODEC_INVALID_ARGUMENT:
    return CodecStatusCode::invalid_argument;
  case DICOMSDL_CODEC_UNSUPPORTED:
    return CodecStatusCode::unsupported;
  case DICOMSDL_CODEC_INTERNAL_ERROR:
    return CodecStatusCode::internal_error;
  case DICOMSDL_CODEC_OUTPUT_TOO_SMALL:
  case DICOMSDL_CODEC_BACKEND_ERROR:
  default:
    return CodecStatusCode::backend_error;
  }
}

[[nodiscard]] inline constexpr std::string_view stage_name_from_abi_code(
    std::uint32_t stage_code) noexcept {
  switch (stage_code) {
  case DICOMSDL_CODEC_STAGE_PLUGIN_LOOKUP:
    return "plugin_lookup";
  case DICOMSDL_CODEC_STAGE_PARSE_OPTIONS:
    return "parse_options";
  case DICOMSDL_CODEC_STAGE_VALIDATE:
    return "validate";
  case DICOMSDL_CODEC_STAGE_LOAD_FRAME_SOURCE:
    return "load_frame_source";
  case DICOMSDL_CODEC_STAGE_ENCODE_FRAME:
    return "encode_frame";
  case DICOMSDL_CODEC_STAGE_DECODE_FRAME:
    return "decode_frame";
  case DICOMSDL_CODEC_STAGE_POSTPROCESS:
    return "postprocess";
  case DICOMSDL_CODEC_STAGE_ALLOCATE:
    return "allocate";
  case DICOMSDL_CODEC_STAGE_UNKNOWN:
  default:
    return "unknown";
  }
}

[[nodiscard]] inline CodecError decode_plugin_error_v1(
    const dicomsdl_codec_error_v1& plugin_error, std::string_view fallback_stage,
    std::string_view fallback_detail) {
  CodecError out_error{};
  out_error.code = status_from_abi_code(plugin_error.status_code);

  std::string_view stage = stage_name_from_abi_code(plugin_error.stage_code);
  if (stage == "unknown" && !fallback_stage.empty()) {
    out_error.stage = std::string(fallback_stage);
  } else {
    out_error.stage = std::string(stage);
  }

  if (plugin_error.detail != nullptr && plugin_error.detail_length > 0) {
    const std::size_t max_readable =
        plugin_error.detail_capacity == 0
            ? static_cast<std::size_t>(plugin_error.detail_length)
            : static_cast<std::size_t>(
                  std::min(plugin_error.detail_length, plugin_error.detail_capacity));
    if (max_readable > 0) {
      out_error.detail.assign(plugin_error.detail, max_readable);
      while (!out_error.detail.empty() && out_error.detail.back() == '\0') {
        out_error.detail.pop_back();
      }
    }
  }
  if (out_error.detail.empty()) {
    out_error.detail = std::string(fallback_detail);
  }
  return out_error;
}

}  // namespace dicom::pixel::detail::abi
