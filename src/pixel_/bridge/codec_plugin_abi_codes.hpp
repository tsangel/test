#pragma once

#include "../registry/codec_registry.hpp"
#include "pixel_codec_plugin_abi_common.h"

#include <optional>

namespace dicom::pixel::detail::abi {

[[nodiscard]] inline constexpr std::uint16_t to_transfer_syntax_code(
    uid::WellKnown transfer_syntax) noexcept {
  if (!transfer_syntax.valid() ||
      transfer_syntax.uid_type() != UidType::TransferSyntax) {
    return DICOMSDL_TRANSFER_SYNTAX_CODE_INVALID;
  }
  return transfer_syntax.raw_index();
}

[[nodiscard]] inline constexpr std::optional<uid::WellKnown>
from_transfer_syntax_code(std::uint16_t code) noexcept {
  if (code == DICOMSDL_TRANSFER_SYNTAX_CODE_INVALID) {
    return std::nullopt;
  }
  const auto transfer_syntax = uid::from_index(code);
  if (!transfer_syntax ||
      transfer_syntax->uid_type() != UidType::TransferSyntax) {
    return std::nullopt;
  }
  return transfer_syntax;
}

[[nodiscard]] inline constexpr std::uint8_t to_dtype_code(
    DataType data_type) noexcept {
  switch (data_type) {
  case DataType::u8:
    return DICOMSDL_DTYPE_U8;
  case DataType::s8:
    return DICOMSDL_DTYPE_S8;
  case DataType::u16:
    return DICOMSDL_DTYPE_U16;
  case DataType::s16:
    return DICOMSDL_DTYPE_S16;
  case DataType::u32:
    return DICOMSDL_DTYPE_U32;
  case DataType::s32:
    return DICOMSDL_DTYPE_S32;
  case DataType::f32:
    return DICOMSDL_DTYPE_F32;
  case DataType::f64:
    return DICOMSDL_DTYPE_F64;
  case DataType::unknown:
  default:
    return DICOMSDL_DTYPE_UNKNOWN;
  }
}

[[nodiscard]] inline constexpr std::optional<DataType> from_dtype_code(
    std::uint8_t code) noexcept {
  switch (code) {
  case DICOMSDL_DTYPE_UNKNOWN:
    return DataType::unknown;
  case DICOMSDL_DTYPE_U8:
    return DataType::u8;
  case DICOMSDL_DTYPE_S8:
    return DataType::s8;
  case DICOMSDL_DTYPE_U16:
    return DataType::u16;
  case DICOMSDL_DTYPE_S16:
    return DataType::s16;
  case DICOMSDL_DTYPE_U32:
    return DataType::u32;
  case DICOMSDL_DTYPE_S32:
    return DataType::s32;
  case DICOMSDL_DTYPE_F32:
    return DataType::f32;
  case DICOMSDL_DTYPE_F64:
    return DataType::f64;
  default:
    return std::nullopt;
  }
}

[[nodiscard]] inline constexpr std::uint8_t to_planar_code(
    Planar planar) noexcept {
  return planar == Planar::planar ? DICOMSDL_PLANAR_PLANAR
                                  : DICOMSDL_PLANAR_INTERLEAVED;
}

[[nodiscard]] inline constexpr std::optional<Planar> from_planar_code(
    std::uint8_t code) noexcept {
  switch (code) {
  case DICOMSDL_PLANAR_INTERLEAVED:
    return Planar::interleaved;
  case DICOMSDL_PLANAR_PLANAR:
    return Planar::planar;
  default:
    return std::nullopt;
  }
}

[[nodiscard]] inline constexpr std::uint32_t to_profile_code(
    CodecProfile profile) noexcept {
  switch (profile) {
  case CodecProfile::native_uncompressed:
    return DICOMSDL_CODEC_PROFILE_NATIVE_UNCOMPRESSED;
  case CodecProfile::encapsulated_uncompressed:
    return DICOMSDL_CODEC_PROFILE_ENCAPSULATED_UNCOMPRESSED;
  case CodecProfile::rle_lossless:
    return DICOMSDL_CODEC_PROFILE_RLE_LOSSLESS;
  case CodecProfile::jpeg_lossless:
    return DICOMSDL_CODEC_PROFILE_JPEG_LOSSLESS;
  case CodecProfile::jpeg_lossy:
    return DICOMSDL_CODEC_PROFILE_JPEG_LOSSY;
  case CodecProfile::jpegls_lossless:
    return DICOMSDL_CODEC_PROFILE_JPEGLS_LOSSLESS;
  case CodecProfile::jpegls_near_lossless:
    return DICOMSDL_CODEC_PROFILE_JPEGLS_NEAR_LOSSLESS;
  case CodecProfile::jpeg2000_lossless:
    return DICOMSDL_CODEC_PROFILE_JPEG2000_LOSSLESS;
  case CodecProfile::jpeg2000_lossy:
    return DICOMSDL_CODEC_PROFILE_JPEG2000_LOSSY;
  case CodecProfile::htj2k_lossless:
    return DICOMSDL_CODEC_PROFILE_HTJ2K_LOSSLESS;
  case CodecProfile::htj2k_lossless_rpcl:
    return DICOMSDL_CODEC_PROFILE_HTJ2K_LOSSLESS_RPCL;
  case CodecProfile::htj2k_lossy:
    return DICOMSDL_CODEC_PROFILE_HTJ2K_LOSSY;
  case CodecProfile::jpegxl_lossless:
    return DICOMSDL_CODEC_PROFILE_JPEGXL_LOSSLESS;
  case CodecProfile::jpegxl_lossy:
    return DICOMSDL_CODEC_PROFILE_JPEGXL_LOSSY;
  case CodecProfile::jpegxl_jpeg_recompression:
    return DICOMSDL_CODEC_PROFILE_JPEGXL_JPEG_RECOMPRESSION;
  case CodecProfile::unknown:
  default:
    return DICOMSDL_CODEC_PROFILE_UNKNOWN;
  }
}

[[nodiscard]] inline constexpr std::optional<CodecProfile> from_profile_code(
    std::uint32_t code) noexcept {
  switch (code) {
  case DICOMSDL_CODEC_PROFILE_UNKNOWN:
    return CodecProfile::unknown;
  case DICOMSDL_CODEC_PROFILE_NATIVE_UNCOMPRESSED:
    return CodecProfile::native_uncompressed;
  case DICOMSDL_CODEC_PROFILE_ENCAPSULATED_UNCOMPRESSED:
    return CodecProfile::encapsulated_uncompressed;
  case DICOMSDL_CODEC_PROFILE_RLE_LOSSLESS:
    return CodecProfile::rle_lossless;
  case DICOMSDL_CODEC_PROFILE_JPEG_LOSSLESS:
    return CodecProfile::jpeg_lossless;
  case DICOMSDL_CODEC_PROFILE_JPEG_LOSSY:
    return CodecProfile::jpeg_lossy;
  case DICOMSDL_CODEC_PROFILE_JPEGLS_LOSSLESS:
    return CodecProfile::jpegls_lossless;
  case DICOMSDL_CODEC_PROFILE_JPEGLS_NEAR_LOSSLESS:
    return CodecProfile::jpegls_near_lossless;
  case DICOMSDL_CODEC_PROFILE_JPEG2000_LOSSLESS:
    return CodecProfile::jpeg2000_lossless;
  case DICOMSDL_CODEC_PROFILE_JPEG2000_LOSSY:
    return CodecProfile::jpeg2000_lossy;
  case DICOMSDL_CODEC_PROFILE_HTJ2K_LOSSLESS:
    return CodecProfile::htj2k_lossless;
  case DICOMSDL_CODEC_PROFILE_HTJ2K_LOSSLESS_RPCL:
    return CodecProfile::htj2k_lossless_rpcl;
  case DICOMSDL_CODEC_PROFILE_HTJ2K_LOSSY:
    return CodecProfile::htj2k_lossy;
  case DICOMSDL_CODEC_PROFILE_JPEGXL_LOSSLESS:
    return CodecProfile::jpegxl_lossless;
  case DICOMSDL_CODEC_PROFILE_JPEGXL_LOSSY:
    return CodecProfile::jpegxl_lossy;
  case DICOMSDL_CODEC_PROFILE_JPEGXL_JPEG_RECOMPRESSION:
    return CodecProfile::jpegxl_jpeg_recompression;
  default:
    return std::nullopt;
  }
}

}  // namespace dicom::pixel::detail::abi
