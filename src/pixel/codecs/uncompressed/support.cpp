#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

#include "support.hpp"

namespace pixel::core {

bool is_uncompressed_profile(uint32_t codec_profile_code) {
  return codec_profile_code == PIXEL_CODEC_PROFILE_NATIVE_UNCOMPRESSED ||
      codec_profile_code == PIXEL_CODEC_PROFILE_ENCAPSULATED_UNCOMPRESSED;
}

bool is_native_uncompressed_profile(uint32_t codec_profile_code) {
  return codec_profile_code == PIXEL_CODEC_PROFILE_NATIVE_UNCOMPRESSED;
}

bool is_encapsulated_uncompressed_profile(uint32_t codec_profile_code) {
  return codec_profile_code == PIXEL_CODEC_PROFILE_ENCAPSULATED_UNCOMPRESSED;
}

pixel_supported_profile_flags supported_profile_flags() {
  return PIXEL_CODEC_PROFILE_NATIVE_UNCOMPRESSED_DEC |
      PIXEL_CODEC_PROFILE_NATIVE_UNCOMPRESSED_ENC |
      PIXEL_CODEC_PROFILE_ENCAPSULATED_UNCOMPRESSED_DEC |
      PIXEL_CODEC_PROFILE_ENCAPSULATED_UNCOMPRESSED_ENC;
}

void copy_text(char* dst, std::size_t dst_capacity, const char* src) {
  if (dst == nullptr || dst_capacity == 0) {
    return;
  }
  if (src == nullptr) {
    dst[0] = '\0';
    return;
  }

  const std::size_t n = std::strlen(src);
  const std::size_t count = (n < (dst_capacity - 1)) ? n : (dst_capacity - 1);
  std::memcpy(dst, src, count);
  dst[count] = '\0';
}

void clear_error(ErrorState* state) {
  if (state == nullptr) {
    return;
  }
  state->last_error_detail[0] = '\0';
}

void set_error_detail(ErrorState* state, const char* detail) {
  if (state == nullptr) {
    return;
  }
  copy_text(state->last_error_detail, kLastErrorDetailCapacity, detail);
}

uint32_t copy_last_error_detail(
    const ErrorState* state, char* out_detail, uint32_t out_detail_capacity) {
  if (out_detail == nullptr || out_detail_capacity == 0) {
    return 0;
  }
  if (state == nullptr) {
    out_detail[0] = '\0';
    return 0;
  }
  copy_text(out_detail, out_detail_capacity, state->last_error_detail);
  return static_cast<uint32_t>(std::strlen(out_detail));
}

pixel_error_code fail_detail(ErrorState* state, pixel_error_code code,
    const char* stage, const char* reason) {
  char detail[kLastErrorDetailCapacity];
  std::snprintf(detail, sizeof(detail), "stage=%s;reason=%s",
      (stage != nullptr ? stage : "unknown"),
      (reason != nullptr ? reason : "unknown"));
  set_error_detail(state, detail);
  return code;
}

pixel_error_code fail_detail_u32(ErrorState* state, pixel_error_code code,
    const char* stage, const char* reason_fmt, uint32_t value) {
  char reason[320];
  std::snprintf(reason, sizeof(reason), reason_fmt, static_cast<unsigned>(value));
  return fail_detail(state, code, stage, reason);
}

bool mul_u64(uint64_t a, uint64_t b, uint64_t* out) {
  if (out == nullptr) {
    return false;
  }
  if (a != 0 && b > (std::numeric_limits<uint64_t>::max() / a)) {
    return false;
  }
  *out = a * b;
  return true;
}

bool u64_to_size(uint64_t value, std::size_t* out) {
  if (out == nullptr) {
    return false;
  }
  if (value > static_cast<uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return false;
  }
  *out = static_cast<std::size_t>(value);
  return true;
}

bool dtype_info_from_code(uint8_t code, DtypeInfo* out) {
  if (out == nullptr) {
    return false;
  }
  switch (code) {
  case PIXEL_DTYPE_U8:
    *out = DtypeInfo{1, false, false};
    return true;
  case PIXEL_DTYPE_S8:
    *out = DtypeInfo{1, true, false};
    return true;
  case PIXEL_DTYPE_U16:
    *out = DtypeInfo{2, false, false};
    return true;
  case PIXEL_DTYPE_S16:
    *out = DtypeInfo{2, true, false};
    return true;
  case PIXEL_DTYPE_U32:
    *out = DtypeInfo{4, false, false};
    return true;
  case PIXEL_DTYPE_S32:
    *out = DtypeInfo{4, true, false};
    return true;
  case PIXEL_DTYPE_F32:
    *out = DtypeInfo{4, true, true};
    return true;
  case PIXEL_DTYPE_F64:
    *out = DtypeInfo{8, true, true};
    return true;
  default:
    return false;
  }
}

bool is_valid_planar_code(uint8_t code) {
  return code == PIXEL_PLANAR_INTERLEAVED || code == PIXEL_PLANAR_PLANAR;
}

bool is_planar_code(uint8_t code) {
  return code == PIXEL_PLANAR_PLANAR;
}

}  // namespace pixel::core
