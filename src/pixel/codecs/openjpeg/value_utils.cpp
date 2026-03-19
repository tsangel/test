#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <type_traits>

#include "internal.hpp"

namespace pixel::openjpeg_codec {

namespace {

uint32_t load_u16_le(const uint8_t* src) {
  return static_cast<uint32_t>(src[0]) |
      (static_cast<uint32_t>(src[1]) << 8);
}

uint32_t load_u32_le(const uint8_t* src) {
  return static_cast<uint32_t>(src[0]) |
      (static_cast<uint32_t>(src[1]) << 8) |
      (static_cast<uint32_t>(src[2]) << 16) |
      (static_cast<uint32_t>(src[3]) << 24);
}

uint32_t bit_mask_u32(int bits) {
  if (bits <= 0) {
    return 0;
  }
  if (bits >= 32) {
    return 0xFFFFFFFFu;
  }
  return (uint32_t{1} << static_cast<unsigned>(bits)) - 1u;
}

int32_t sign_extend_u32(uint32_t raw, int bits) {
  if (bits <= 0) {
    return 0;
  }
  if (bits >= 32) {
    return static_cast<int32_t>(raw);
  }
  const int shift = 32 - bits;
  return static_cast<int32_t>(raw << static_cast<unsigned>(shift)) >> shift;
}

template <typename T>
T clamp_from_i32(int32_t v) {
  if constexpr (std::is_signed_v<T>) {
    if (v < static_cast<int32_t>(std::numeric_limits<T>::min())) {
      return std::numeric_limits<T>::min();
    }
    if (v > static_cast<int32_t>(std::numeric_limits<T>::max())) {
      return std::numeric_limits<T>::max();
    }
    return static_cast<T>(v);
  } else {
    if (v < 0) {
      return 0;
    }
    const uint64_t uv = static_cast<uint64_t>(v);
    if (uv > static_cast<uint64_t>(std::numeric_limits<T>::max())) {
      return std::numeric_limits<T>::max();
    }
    return static_cast<T>(uv);
  }
}

template <typename T>
void write_scalar(uint8_t* dst, T value) {
  std::memcpy(dst, &value, sizeof(T));
}

}  // namespace

namespace {

constexpr pixel_supported_profile_flags kSupportedProfileFlags =
    PIXEL_CODEC_PROFILE_JPEG2000_LOSSLESS_DEC |
    PIXEL_CODEC_PROFILE_JPEG2000_LOSSY_DEC |
    PIXEL_CODEC_PROFILE_HTJ2K_LOSSLESS_DEC |
    PIXEL_CODEC_PROFILE_HTJ2K_LOSSLESS_RPCL_DEC |
    PIXEL_CODEC_PROFILE_HTJ2K_LOSSY_DEC |
    PIXEL_CODEC_PROFILE_JPEG2000_LOSSLESS_ENC |
    PIXEL_CODEC_PROFILE_JPEG2000_LOSSY_ENC;

}  // namespace

pixel_supported_profile_flags supported_profile_flags() {
  return kSupportedProfileFlags;
}

bool is_supported_decoder_profile(uint32_t codec_profile_code) {
  if (codec_profile_code > 31u) {
    return false;
  }
  const pixel_supported_profile_flags bit =
      PIXEL_CODEC_PROFILE_DEC_FLAG(codec_profile_code);
  return (supported_profile_flags() & bit) != 0;
}

bool is_supported_encoder_profile(uint32_t codec_profile_code) {
  if (codec_profile_code > 31u) {
    return false;
  }
  const pixel_supported_profile_flags bit =
      PIXEL_CODEC_PROFILE_ENC_FLAG(codec_profile_code);
  return (supported_profile_flags() & bit) != 0;
}

bool parse_int_option(const char* value, int* out_value) {
  if (value == nullptr || out_value == nullptr) {
    return false;
  }
  char* end = nullptr;
  errno = 0;
  const long parsed = std::strtol(value, &end, 10);
  if (errno != 0 || end == value || *end != '\0') {
    return false;
  }
  if (parsed < std::numeric_limits<int>::min() ||
      parsed > std::numeric_limits<int>::max()) {
    return false;
  }
  *out_value = static_cast<int>(parsed);
  return true;
}

bool parse_double_option(const char* value, double* out_value) {
  if (value == nullptr || out_value == nullptr) {
    return false;
  }
  char* end = nullptr;
  errno = 0;
  const double parsed = std::strtod(value, &end);
  if (errno != 0 || end == value || *end != '\0' || !std::isfinite(parsed)) {
    return false;
  }
  *out_value = parsed;
  return true;
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

bool load_integral_sample_from_le(const uint8_t* src, uint32_t bytes,
    bool is_signed, int bits_stored, int32_t* out_value, char* reason,
    std::size_t reason_capacity) {
  if (src == nullptr || out_value == nullptr) {
    copy_text(reason, reason_capacity, "null sample input/output pointer");
    return false;
  }

  uint32_t raw = 0;
  switch (bytes) {
  case 1:
    raw = src[0];
    break;
  case 2:
    raw = load_u16_le(src);
    break;
  case 4:
    raw = load_u32_le(src);
    break;
  default:
    copy_text(reason, reason_capacity, "unsupported bytes_per_sample");
    return false;
  }

  if (is_signed) {
    const int32_t sample = sign_extend_u32(raw, bits_stored);
    *out_value = sample;
    return true;
  }

  raw &= bit_mask_u32(bits_stored);
  if (raw > static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) {
    copy_text(reason, reason_capacity,
        "unsigned sample exceeds OpenJPEG int32 range");
    return false;
  }
  *out_value = static_cast<int32_t>(raw);
  return true;
}

bool write_integer_sample(uint8_t dst_dtype, int32_t sample, uint8_t* dst) {
  if (dst == nullptr) {
    return false;
  }
  switch (dst_dtype) {
  case PIXEL_DTYPE_U8:
    write_scalar<uint8_t>(dst, clamp_from_i32<uint8_t>(sample));
    return true;
  case PIXEL_DTYPE_S8:
    write_scalar<int8_t>(dst, clamp_from_i32<int8_t>(sample));
    return true;
  case PIXEL_DTYPE_U16:
    write_scalar<uint16_t>(dst, clamp_from_i32<uint16_t>(sample));
    return true;
  case PIXEL_DTYPE_S16:
    write_scalar<int16_t>(dst, clamp_from_i32<int16_t>(sample));
    return true;
  case PIXEL_DTYPE_U32:
    write_scalar<uint32_t>(dst, clamp_from_i32<uint32_t>(sample));
    return true;
  case PIXEL_DTYPE_S32:
    write_scalar<int32_t>(dst, sample);
    return true;
  default:
    return false;
  }
}

bool write_float_sample(uint8_t dst_dtype, double sample, uint8_t* dst) {
  if (dst == nullptr) {
    return false;
  }
  switch (dst_dtype) {
  case PIXEL_DTYPE_F32: {
    const float value = static_cast<float>(sample);
    write_scalar<float>(dst, value);
    return true;
  }
  case PIXEL_DTYPE_F64:
    write_scalar<double>(dst, sample);
    return true;
  default:
    return false;
  }
}

bool resolve_thread_count(int configured_threads, OPJ_UINT32* out_thread_count) {
  if (out_thread_count == nullptr) {
    return false;
  }
  if (configured_threads < -1) {
    return false;
  }
  if (configured_threads == 0) {
    *out_thread_count = 0;
    return true;
  }
  if (configured_threads == -1) {
    configured_threads = opj_get_num_cpus();
  }
  if (configured_threads <= 0) {
    *out_thread_count = 0;
    return true;
  }
  if (static_cast<unsigned long long>(configured_threads) >
      static_cast<unsigned long long>(std::numeric_limits<OPJ_UINT32>::max())) {
    return false;
  }
  *out_thread_count = static_cast<OPJ_UINT32>(configured_threads);
  return true;
}

int max_num_resolutions_for_image(std::size_t rows, std::size_t cols) {
  const std::size_t min_dim = std::min(rows, cols);
  int max_num_resolutions = 1;
  std::size_t scale = 1;
  while (scale <= min_dim / 2 && max_num_resolutions < 32) {
    scale *= 2;
    ++max_num_resolutions;
  }
  return max_num_resolutions;
}

float quality_to_openjpeg_rate(int quality) {
  const int q = std::clamp(quality, 1, 100);
  return 1.0f + static_cast<float>(100 - q) * 0.5f;
}

OPJ_COLOR_SPACE resolve_color_space(std::size_t samples_per_pixel) {
  switch (samples_per_pixel) {
  case 1:
    return OPJ_CLRSPC_GRAY;
  case 3:
    return OPJ_CLRSPC_SRGB;
  default:
    return OPJ_CLRSPC_UNSPECIFIED;
  }
}

}  // namespace pixel::openjpeg_codec
