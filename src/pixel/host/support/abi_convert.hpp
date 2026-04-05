#pragma once

#include <cstdint>
#include <span>

#include "dicom.h"
#include "pixel_codec_plugin_abi.h"
#include "pixel_decoder_plugin_abi.h"
#include "pixel_encoder_plugin_abi.h"

namespace pixel::runtime {

struct DtypeMeta {
  uint8_t code{PIXEL_DTYPE_UNKNOWN};
  uint32_t bytes{0};
  bool is_signed{false};
  bool is_float{false};
};

struct EncoderStrideMeta {
  uint64_t min_row_bytes{0};
  uint64_t row_stride{0};
  uint64_t plane_stride{0};
  uint64_t frame_size_bytes{0};
};

inline bool resolve_dtype_meta(dicom::pixel::DataType data_type, DtypeMeta* out_meta) {
  if (out_meta == nullptr) {
    return false;
  }
  switch (data_type) {
  case dicom::pixel::DataType::u8:
    *out_meta = DtypeMeta{PIXEL_DTYPE_U8, 1u, false, false};
    return true;
  case dicom::pixel::DataType::s8:
    *out_meta = DtypeMeta{PIXEL_DTYPE_S8, 1u, true, false};
    return true;
  case dicom::pixel::DataType::u16:
    *out_meta = DtypeMeta{PIXEL_DTYPE_U16, 2u, false, false};
    return true;
  case dicom::pixel::DataType::s16:
    *out_meta = DtypeMeta{PIXEL_DTYPE_S16, 2u, true, false};
    return true;
  case dicom::pixel::DataType::u32:
    *out_meta = DtypeMeta{PIXEL_DTYPE_U32, 4u, false, false};
    return true;
  case dicom::pixel::DataType::s32:
    *out_meta = DtypeMeta{PIXEL_DTYPE_S32, 4u, true, false};
    return true;
  case dicom::pixel::DataType::f32:
    *out_meta = DtypeMeta{PIXEL_DTYPE_F32, 4u, true, true};
    return true;
  case dicom::pixel::DataType::f64:
    *out_meta = DtypeMeta{PIXEL_DTYPE_F64, 8u, true, true};
    return true;
  case dicom::pixel::DataType::unknown:
  default:
    return false;
  }
}

inline uint8_t to_planar_code(dicom::pixel::Planar planar) noexcept {
  return planar == dicom::pixel::Planar::planar
      ? PIXEL_PLANAR_PLANAR
      : PIXEL_PLANAR_INTERLEAVED;
}

inline bool is_mct_capable_profile(uint32_t codec_profile_code) noexcept {
  return codec_profile_code == PIXEL_CODEC_PROFILE_JPEG2000_LOSSLESS ||
      codec_profile_code == PIXEL_CODEC_PROFILE_JPEG2000_LOSSY ||
      codec_profile_code == PIXEL_CODEC_PROFILE_HTJ2K_LOSSLESS ||
      codec_profile_code == PIXEL_CODEC_PROFILE_HTJ2K_LOSSLESS_RPCL ||
      codec_profile_code == PIXEL_CODEC_PROFILE_HTJ2K_LOSSY;
}

inline uint8_t decoded_color_space_code_from_photometric(
    dicom::pixel::Photometric photometric) noexcept {
  switch (photometric) {
  case dicom::pixel::Photometric::monochrome1:
  case dicom::pixel::Photometric::monochrome2:
    return PIXEL_DECODED_COLOR_SPACE_MONOCHROME;
  case dicom::pixel::Photometric::palette_color:
    return PIXEL_DECODED_COLOR_SPACE_PALETTE_COLOR;
  case dicom::pixel::Photometric::rgb:
    return PIXEL_DECODED_COLOR_SPACE_RGB;
  case dicom::pixel::Photometric::ybr_full:
    return PIXEL_DECODED_COLOR_SPACE_YBR_FULL;
  case dicom::pixel::Photometric::ybr_full_422:
    return PIXEL_DECODED_COLOR_SPACE_YBR_FULL_422;
  case dicom::pixel::Photometric::ybr_rct:
    return PIXEL_DECODED_COLOR_SPACE_YBR_RCT;
  case dicom::pixel::Photometric::ybr_ict:
    return PIXEL_DECODED_COLOR_SPACE_YBR_ICT;
  case dicom::pixel::Photometric::ybr_partial_420:
    return PIXEL_DECODED_COLOR_SPACE_YBR_PARTIAL_420;
  case dicom::pixel::Photometric::xyb:
    return PIXEL_DECODED_COLOR_SPACE_XYB;
  case dicom::pixel::Photometric::hsv:
    return PIXEL_DECODED_COLOR_SPACE_HSV;
  case dicom::pixel::Photometric::argb:
    return PIXEL_DECODED_COLOR_SPACE_ARGB;
  case dicom::pixel::Photometric::cmyk:
    return PIXEL_DECODED_COLOR_SPACE_CMYK;
  case dicom::pixel::Photometric::ybr_partial_422:
    return PIXEL_DECODED_COLOR_SPACE_YBR_PARTIAL_422;
  default:
    return PIXEL_DECODED_COLOR_SPACE_UNKNOWN;
  }
}

inline bool build_decoder_request(uint32_t codec_profile_code,
    uint8_t source_dtype_code, const dicom::pixel::PixelLayout& source_layout,
    dicom::pixel::Photometric output_photometric,
    std::span<const uint8_t> prepared_source, std::span<uint8_t> destination,
    uint8_t destination_dtype_code, dicom::pixel::Planar destination_planar,
    uint64_t output_row_stride, uint64_t output_frame_stride, bool decode_mct,
    pixel_decoder_info* decode_info, pixel_decoder_request* out_request) {
  if (out_request == nullptr) {
    return false;
  }

  // Build one normalized decoder request so plugin and core-direct backends
  // observe the same source and destination layout contract.
  pixel_decoder_request request{};
  request.struct_size = sizeof(pixel_decoder_request);
  request.abi_version = PIXEL_DECODER_PLUGIN_ABI;

  // Source bytes already point at one prepared codestream payload.
  request.source.struct_size = sizeof(pixel_decoder_source);
  request.source.abi_version = PIXEL_DECODER_PLUGIN_ABI;
  request.source.source_buffer.data = prepared_source.data();
  request.source.source_buffer.size = static_cast<uint64_t>(prepared_source.size());

  // Frame metadata comes from the normalized stored-value source layout.
  request.frame.struct_size = sizeof(pixel_decoder_frame_info);
  request.frame.abi_version = PIXEL_DECODER_PLUGIN_ABI;
  request.frame.codec_profile_code = codec_profile_code;
  request.frame.source_dtype = source_dtype_code;
  request.frame.source_planar = to_planar_code(source_layout.planar);
  request.frame.reserved0 = static_cast<uint16_t>(
      decoded_color_space_code_from_photometric(output_photometric));
  request.frame.rows = source_layout.rows;
  request.frame.cols = source_layout.cols;
  request.frame.samples_per_pixel = source_layout.samples_per_pixel;
  request.frame.bits_stored = source_layout.bits_stored;
  request.frame.decode_mct = decode_mct ? 1u : 0u;

  // Destination layout is passed as already normalized ABI-facing values.
  request.output.struct_size = sizeof(pixel_decoder_output);
  request.output.abi_version = PIXEL_DECODER_PLUGIN_ABI;
  request.output.dst = destination.data();
  request.output.dst_size = static_cast<uint64_t>(destination.size());
  request.output.row_stride = output_row_stride;
  request.output.frame_stride = output_frame_stride;
  request.output.dst_dtype = destination_dtype_code;
  request.output.dst_planar = to_planar_code(destination_planar);
  request.decode_info = decode_info;

  *out_request = request;
  return true;
}

inline bool build_encoder_request(uint32_t codec_profile_code,
    const DtypeMeta& source_dtype, const EncoderStrideMeta& stride_meta,
    const dicom::pixel::PixelLayout& source_layout, std::span<const uint8_t> source_frame,
    bool use_multicomponent_transform, pixel_output_buffer encoded_buffer,
    uint64_t encoded_size, pixel_encoder_request* out_request) {
  if (out_request == nullptr) {
    return false;
  }

  const int32_t bits_allocated = static_cast<int32_t>(source_dtype.bytes * 8u);
  const int32_t bits_stored =
      source_layout.bits_stored > 0 ? static_cast<int32_t>(source_layout.bits_stored)
                                    : bits_allocated;
  const int32_t pixel_representation =
      (source_dtype.is_signed && !source_dtype.is_float) ? 1 : 0;

  // Build one normalized encoder request so all encode entrypoints feed the
  // same ABI payload into the selected backend.
  pixel_encoder_request request{};
  request.struct_size = sizeof(pixel_encoder_request);
  request.abi_version = PIXEL_ENCODER_PLUGIN_ABI;

  // Runtime encode always operates on a single prepared source frame.
  request.source.struct_size = sizeof(pixel_encoder_source);
  request.source.abi_version = PIXEL_ENCODER_PLUGIN_ABI;
  request.source.source_buffer.data = source_frame.data();
  request.source.source_buffer.size = static_cast<uint64_t>(source_frame.size());

  // Frame metadata comes from the normalized source layout plus derived strides.
  request.frame.struct_size = sizeof(pixel_encoder_frame_info);
  request.frame.abi_version = PIXEL_ENCODER_PLUGIN_ABI;
  request.frame.codec_profile_code = codec_profile_code;
  request.frame.source_dtype = source_dtype.code;
  request.frame.source_planar = to_planar_code(source_layout.planar);
  request.frame.rows = static_cast<int32_t>(source_layout.rows);
  request.frame.cols = static_cast<int32_t>(source_layout.cols);
  request.frame.samples_per_pixel =
      static_cast<int32_t>(source_layout.samples_per_pixel);
  request.frame.bits_allocated = bits_allocated;
  request.frame.bits_stored = bits_stored;
  request.frame.pixel_representation = pixel_representation;
  request.frame.source_row_stride = stride_meta.row_stride;
  request.frame.source_plane_stride = stride_meta.plane_stride;
  request.frame.source_frame_size_bytes = stride_meta.frame_size_bytes;
  request.frame.use_multicomponent_transform = use_multicomponent_transform ? 1u : 0u;

  // Some callers provide an output buffer while others let the plugin own it.
  request.output.struct_size = sizeof(pixel_encoder_output);
  request.output.abi_version = PIXEL_ENCODER_PLUGIN_ABI;
  request.output.encoded_buffer = encoded_buffer;
  request.output.encoded_size = encoded_size;

  *out_request = request;
  return true;
}

}  // namespace pixel::runtime
