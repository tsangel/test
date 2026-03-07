#pragma once

#include <cstdint>

#include "dicom.h"
#include "pixel_codec_plugin_abi_v2.h"

namespace pixel::runtime_v2 {

struct DtypeMeta {
  uint8_t code{PIXEL_DTYPE_UNKNOWN_V2};
  uint32_t bytes{0};
  bool is_signed{false};
  bool is_float{false};
};

inline bool resolve_dtype_meta(dicom::pixel::DataType data_type, DtypeMeta* out_meta) {
  if (out_meta == nullptr) {
    return false;
  }
  switch (data_type) {
  case dicom::pixel::DataType::u8:
    *out_meta = DtypeMeta{PIXEL_DTYPE_U8_V2, 1u, false, false};
    return true;
  case dicom::pixel::DataType::s8:
    *out_meta = DtypeMeta{PIXEL_DTYPE_S8_V2, 1u, true, false};
    return true;
  case dicom::pixel::DataType::u16:
    *out_meta = DtypeMeta{PIXEL_DTYPE_U16_V2, 2u, false, false};
    return true;
  case dicom::pixel::DataType::s16:
    *out_meta = DtypeMeta{PIXEL_DTYPE_S16_V2, 2u, true, false};
    return true;
  case dicom::pixel::DataType::u32:
    *out_meta = DtypeMeta{PIXEL_DTYPE_U32_V2, 4u, false, false};
    return true;
  case dicom::pixel::DataType::s32:
    *out_meta = DtypeMeta{PIXEL_DTYPE_S32_V2, 4u, true, false};
    return true;
  case dicom::pixel::DataType::f32:
    *out_meta = DtypeMeta{PIXEL_DTYPE_F32_V2, 4u, true, true};
    return true;
  case dicom::pixel::DataType::f64:
    *out_meta = DtypeMeta{PIXEL_DTYPE_F64_V2, 8u, true, true};
    return true;
  case dicom::pixel::DataType::unknown:
  default:
    return false;
  }
}

inline uint8_t to_planar_code(dicom::pixel::Planar planar) noexcept {
  return planar == dicom::pixel::Planar::planar
      ? PIXEL_PLANAR_PLANAR_V2
      : PIXEL_PLANAR_INTERLEAVED_V2;
}

}  // namespace pixel::runtime_v2
