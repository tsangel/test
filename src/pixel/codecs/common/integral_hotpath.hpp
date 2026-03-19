#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

#include "pixel_decoder_plugin_abi.h"

namespace pixel::codec_common {

template <typename T>
inline T clamp_i32_to_dst(int32_t value) {
  if constexpr (std::is_floating_point_v<T>) {
    return static_cast<T>(value);
  } else if constexpr (std::is_same_v<T, int32_t>) {
    return value;
  } else if constexpr (std::is_signed_v<T>) {
    if (value < static_cast<int32_t>(std::numeric_limits<T>::min())) {
      return std::numeric_limits<T>::min();
    }
    if (value > static_cast<int32_t>(std::numeric_limits<T>::max())) {
      return std::numeric_limits<T>::max();
    }
    return static_cast<T>(value);
  } else {
    if (value < 0) {
      return 0;
    }
    const uint64_t unsigned_value = static_cast<uint64_t>(value);
    if (unsigned_value > static_cast<uint64_t>(std::numeric_limits<T>::max())) {
      return std::numeric_limits<T>::max();
    }
    return static_cast<T>(unsigned_value);
  }
}

template <typename T>
inline void store_scalar_unaligned(uint8_t* dst, T value) {
  std::memcpy(dst, &value, sizeof(T));
}

template <typename T>
inline void store_scalar_aligned_or_unaligned(uint8_t* dst, T value) {
  const bool typed_aligned =
      (reinterpret_cast<std::uintptr_t>(dst) % alignof(T)) == 0;
  if (typed_aligned) {
    *reinterpret_cast<T*>(dst) = value;
    return;
  }
  store_scalar_unaligned(dst, value);
}

inline uint32_t bit_mask_u32_fast(int bits) {
  if (bits <= 0) {
    return 0;
  }
  if (bits >= 32) {
    return 0xFFFFFFFFu;
  }
  return (uint32_t{1} << static_cast<unsigned>(bits)) - 1u;
}

inline int32_t sign_extend_u32_fast(uint32_t raw, int bits) {
  if (bits <= 0) {
    return 0;
  }
  if (bits >= 32) {
    return static_cast<int32_t>(raw);
  }
  const int shift = 32 - bits;
  return static_cast<int32_t>(raw << static_cast<unsigned>(shift)) >> shift;
}

template <uint32_t SampleBytes>
inline uint32_t load_raw_native_integral(const uint8_t* src) {
  static_assert(SampleBytes == 1 || SampleBytes == 2 || SampleBytes == 4);
  if constexpr (SampleBytes == 1) {
    return src[0];
  } else if constexpr (SampleBytes == 2) {
    uint16_t value = 0;
    std::memcpy(&value, src, sizeof(value));
    return value;
  } else {
    uint32_t value = 0;
    std::memcpy(&value, src, sizeof(value));
    return value;
  }
}

template <uint32_t SampleBytes>
inline uint32_t load_raw_le_integral(const uint8_t* src) {
  static_assert(SampleBytes == 1 || SampleBytes == 2 || SampleBytes == 4);
  if constexpr (SampleBytes == 1) {
    return src[0];
  } else if constexpr (SampleBytes == 2) {
    return static_cast<uint32_t>(src[0]) |
        (static_cast<uint32_t>(src[1]) << 8);
  } else {
    return static_cast<uint32_t>(src[0]) |
        (static_cast<uint32_t>(src[1]) << 8) |
        (static_cast<uint32_t>(src[2]) << 16) |
        (static_cast<uint32_t>(src[3]) << 24);
  }
}

template <bool SourceSigned>
inline int32_t normalize_loaded_integral(uint32_t raw, int bits_stored) {
  if constexpr (SourceSigned) {
    return sign_extend_u32_fast(raw, bits_stored);
  } else {
    return static_cast<int32_t>(raw & bit_mask_u32_fast(bits_stored));
  }
}

template <uint32_t SampleBytes, bool SourceSigned>
inline int32_t load_native_integral_as_i32(const uint8_t* src, int bits_stored) {
  return normalize_loaded_integral<SourceSigned>(
      load_raw_native_integral<SampleBytes>(src), bits_stored);
}

template <uint32_t SampleBytes, bool SourceSigned>
inline int32_t load_le_integral_as_i32(const uint8_t* src, int bits_stored) {
  return normalize_loaded_integral<SourceSigned>(
      load_raw_le_integral<SampleBytes>(src), bits_stored);
}

template <uint32_t SampleBytes>
inline void store_raw_native_integral(uint8_t* dst, uint32_t raw) {
  static_assert(SampleBytes == 1 || SampleBytes == 2 || SampleBytes == 4);
  if constexpr (SampleBytes == 1) {
    dst[0] = static_cast<uint8_t>(raw & 0xFFu);
  } else if constexpr (SampleBytes == 2) {
    const uint16_t value = static_cast<uint16_t>(raw & 0xFFFFu);
    std::memcpy(dst, &value, sizeof(value));
  } else {
    std::memcpy(dst, &raw, sizeof(raw));
  }
}

template <uint32_t SampleBytes>
inline void store_raw_le_integral(uint8_t* dst, uint32_t raw) {
  static_assert(SampleBytes == 1 || SampleBytes == 2 || SampleBytes == 4);
  if constexpr (SampleBytes == 1) {
    dst[0] = static_cast<uint8_t>(raw & 0xFFu);
  } else if constexpr (SampleBytes == 2) {
    dst[0] = static_cast<uint8_t>(raw & 0xFFu);
    dst[1] = static_cast<uint8_t>((raw >> 8) & 0xFFu);
  } else {
    dst[0] = static_cast<uint8_t>(raw & 0xFFu);
    dst[1] = static_cast<uint8_t>((raw >> 8) & 0xFFu);
    dst[2] = static_cast<uint8_t>((raw >> 16) & 0xFFu);
    dst[3] = static_cast<uint8_t>((raw >> 24) & 0xFFu);
  }
}

template <typename DstT, typename LoadSampleFn>
inline void write_loaded_converted_row(
    uint8_t* dst_row_bytes, std::size_t cols, LoadSampleFn&& load_sample) {
  const bool typed_aligned =
      (reinterpret_cast<std::uintptr_t>(dst_row_bytes) % alignof(DstT)) == 0;
  if (typed_aligned) {
    auto* dst_row = reinterpret_cast<DstT*>(dst_row_bytes);
    for (std::size_t c = 0; c < cols; ++c) {
      dst_row[c] = clamp_i32_to_dst<DstT>(load_sample(c));
    }
    return;
  }

  for (std::size_t c = 0; c < cols; ++c) {
    store_scalar_unaligned(
        dst_row_bytes + c * sizeof(DstT),
        clamp_i32_to_dst<DstT>(load_sample(c)));
  }
}

template <typename DstT, typename LoadSampleFn>
inline void write_loaded_cast_row(
    uint8_t* dst_row_bytes, std::size_t cols, LoadSampleFn&& load_sample) {
  const bool typed_aligned =
      (reinterpret_cast<std::uintptr_t>(dst_row_bytes) % alignof(DstT)) == 0;
  if (typed_aligned) {
    auto* dst_row = reinterpret_cast<DstT*>(dst_row_bytes);
    for (std::size_t c = 0; c < cols; ++c) {
      dst_row[c] = static_cast<DstT>(load_sample(c));
    }
    return;
  }

  for (std::size_t c = 0; c < cols; ++c) {
    store_scalar_unaligned(
        dst_row_bytes + c * sizeof(DstT),
        static_cast<DstT>(load_sample(c)));
  }
}

template <typename DstT, typename LoadSampleFn>
inline void write_loaded_converted_interleaved_row(
    uint8_t* dst_row_bytes,
    std::size_t cols,
    std::size_t samples,
    LoadSampleFn&& load_sample) {
  const bool typed_aligned =
      (reinterpret_cast<std::uintptr_t>(dst_row_bytes) % alignof(DstT)) == 0;
  if (typed_aligned) {
    auto* dst_row = reinterpret_cast<DstT*>(dst_row_bytes);
    for (std::size_t c = 0; c < cols; ++c) {
      const std::size_t pixel_base = c * samples;
      for (std::size_t comp = 0; comp < samples; ++comp) {
        dst_row[pixel_base + comp] =
            clamp_i32_to_dst<DstT>(load_sample(c, comp));
      }
    }
    return;
  }

  for (std::size_t c = 0; c < cols; ++c) {
    const std::size_t pixel_base = (c * samples) * sizeof(DstT);
    for (std::size_t comp = 0; comp < samples; ++comp) {
      store_scalar_unaligned(
          dst_row_bytes + pixel_base + comp * sizeof(DstT),
          clamp_i32_to_dst<DstT>(load_sample(c, comp)));
    }
  }
}

template <typename DstT, typename LoadSampleFn>
inline void write_loaded_cast_interleaved_row(
    uint8_t* dst_row_bytes,
    std::size_t cols,
    std::size_t samples,
    LoadSampleFn&& load_sample) {
  const bool typed_aligned =
      (reinterpret_cast<std::uintptr_t>(dst_row_bytes) % alignof(DstT)) == 0;
  if (typed_aligned) {
    auto* dst_row = reinterpret_cast<DstT*>(dst_row_bytes);
    for (std::size_t c = 0; c < cols; ++c) {
      const std::size_t pixel_base = c * samples;
      for (std::size_t comp = 0; comp < samples; ++comp) {
        dst_row[pixel_base + comp] = static_cast<DstT>(load_sample(c, comp));
      }
    }
    return;
  }

  for (std::size_t c = 0; c < cols; ++c) {
    const std::size_t pixel_base = (c * samples) * sizeof(DstT);
    for (std::size_t comp = 0; comp < samples; ++comp) {
      store_scalar_unaligned(
          dst_row_bytes + pixel_base + comp * sizeof(DstT),
          static_cast<DstT>(load_sample(c, comp)));
    }
  }
}

template <typename DstT, typename LoadSampleFn>
inline void write_loaded_converted_rows_as(
    const pixel_decoder_request* request,
    uint64_t row_stride,
    bool output_planar,
    LoadSampleFn&& load_sample) {
  const std::size_t rows = static_cast<std::size_t>(request->frame.rows);
  const std::size_t cols = static_cast<std::size_t>(request->frame.cols);
  const std::size_t samples = static_cast<std::size_t>(request->frame.samples_per_pixel);
  const std::size_t row_stride_sz = static_cast<std::size_t>(row_stride);

  if (output_planar && samples > 1) {
    const std::size_t output_plane_bytes = row_stride_sz * rows;
    for (std::size_t comp = 0; comp < samples; ++comp) {
      uint8_t* dst_plane = request->output.dst + comp * output_plane_bytes;
      for (std::size_t row = 0; row < rows; ++row) {
        uint8_t* dst_row = dst_plane + row * row_stride_sz;
        write_loaded_converted_row<DstT>(
            dst_row, cols,
            [&](std::size_t col) -> int32_t {
              return load_sample(row, col, comp);
            });
      }
    }
    return;
  }

  for (std::size_t row = 0; row < rows; ++row) {
    uint8_t* dst_row = request->output.dst + row * row_stride_sz;
    write_loaded_converted_interleaved_row<DstT>(
        dst_row, cols, samples,
        [&](std::size_t col, std::size_t comp) -> int32_t {
          return load_sample(row, col, comp);
        });
  }
}

template <typename DstT, typename LoadSampleFn>
inline void write_loaded_cast_rows_as(
    const pixel_decoder_request* request,
    uint64_t row_stride,
    bool output_planar,
    LoadSampleFn&& load_sample) {
  const std::size_t rows = static_cast<std::size_t>(request->frame.rows);
  const std::size_t cols = static_cast<std::size_t>(request->frame.cols);
  const std::size_t samples = static_cast<std::size_t>(request->frame.samples_per_pixel);
  const std::size_t row_stride_sz = static_cast<std::size_t>(row_stride);

  if (output_planar && samples > 1) {
    const std::size_t output_plane_bytes = row_stride_sz * rows;
    for (std::size_t comp = 0; comp < samples; ++comp) {
      uint8_t* dst_plane = request->output.dst + comp * output_plane_bytes;
      for (std::size_t row = 0; row < rows; ++row) {
        uint8_t* dst_row = dst_plane + row * row_stride_sz;
        write_loaded_cast_row<DstT>(
            dst_row, cols,
            [&](std::size_t col) -> int32_t {
              return load_sample(row, col, comp);
            });
      }
    }
    return;
  }

  for (std::size_t row = 0; row < rows; ++row) {
    uint8_t* dst_row = request->output.dst + row * row_stride_sz;
    write_loaded_cast_interleaved_row<DstT>(
        dst_row, cols, samples,
        [&](std::size_t col, std::size_t comp) -> int32_t {
          return load_sample(row, col, comp);
        });
  }
}

template <typename LoadSampleFn>
inline bool write_loaded_converted_rows_to_dst(
    const pixel_decoder_request* request,
    uint64_t row_stride,
    bool output_planar,
    LoadSampleFn&& load_sample) {
  if (request == nullptr || request->output.dst == nullptr) {
    return false;
  }

  switch (request->output.dst_dtype) {
  case PIXEL_DTYPE_U8:
    write_loaded_converted_rows_as<uint8_t>(
        request, row_stride, output_planar, load_sample);
    return true;
  case PIXEL_DTYPE_S8:
    write_loaded_converted_rows_as<int8_t>(
        request, row_stride, output_planar, load_sample);
    return true;
  case PIXEL_DTYPE_U16:
    write_loaded_converted_rows_as<uint16_t>(
        request, row_stride, output_planar, load_sample);
    return true;
  case PIXEL_DTYPE_S16:
    write_loaded_converted_rows_as<int16_t>(
        request, row_stride, output_planar, load_sample);
    return true;
  case PIXEL_DTYPE_U32:
    write_loaded_converted_rows_as<uint32_t>(
        request, row_stride, output_planar, load_sample);
    return true;
  case PIXEL_DTYPE_S32:
    write_loaded_converted_rows_as<int32_t>(
        request, row_stride, output_planar, load_sample);
    return true;
  case PIXEL_DTYPE_F32:
    write_loaded_converted_rows_as<float>(
        request, row_stride, output_planar, load_sample);
    return true;
  case PIXEL_DTYPE_F64:
    write_loaded_converted_rows_as<double>(
        request, row_stride, output_planar, load_sample);
    return true;
  default:
    return false;
  }
}

template <typename LoadSampleFn>
inline bool write_loaded_cast_rows_to_dst(
    const pixel_decoder_request* request,
    uint64_t row_stride,
    bool output_planar,
    LoadSampleFn&& load_sample) {
  if (request == nullptr || request->output.dst == nullptr) {
    return false;
  }

  switch (request->output.dst_dtype) {
  case PIXEL_DTYPE_U8:
    write_loaded_cast_rows_as<uint8_t>(
        request, row_stride, output_planar, load_sample);
    return true;
  case PIXEL_DTYPE_S8:
    write_loaded_cast_rows_as<int8_t>(
        request, row_stride, output_planar, load_sample);
    return true;
  case PIXEL_DTYPE_U16:
    write_loaded_cast_rows_as<uint16_t>(
        request, row_stride, output_planar, load_sample);
    return true;
  case PIXEL_DTYPE_S16:
    write_loaded_cast_rows_as<int16_t>(
        request, row_stride, output_planar, load_sample);
    return true;
  case PIXEL_DTYPE_U32:
    write_loaded_cast_rows_as<uint32_t>(
        request, row_stride, output_planar, load_sample);
    return true;
  case PIXEL_DTYPE_S32:
    write_loaded_cast_rows_as<int32_t>(
        request, row_stride, output_planar, load_sample);
    return true;
  case PIXEL_DTYPE_F32:
    write_loaded_cast_rows_as<float>(
        request, row_stride, output_planar, load_sample);
    return true;
  case PIXEL_DTYPE_F64:
    write_loaded_cast_rows_as<double>(
        request, row_stride, output_planar, load_sample);
    return true;
  default:
    return false;
  }
}

}  // namespace pixel::codec_common
