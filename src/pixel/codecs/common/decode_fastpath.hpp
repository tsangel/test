#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "pixel_decoder_plugin_abi.h"

namespace pixel::codec_common {

enum class loaded_integral_write_status {
  ok,
  unsupported_dst_dtype,
};

template <typename DtypeInfoT>
bool decoded_integral_component_fits_dst_dtype(
    uint32_t precision_bits, bool is_signed, const DtypeInfoT& dst_dtype) {
  if (dst_dtype.is_float || precision_bits == 0 || precision_bits > 32) {
    return false;
  }

  const uint32_t dst_bits = static_cast<uint32_t>(dst_dtype.bytes * 8u);
  if (is_signed) {
    return dst_dtype.is_signed && precision_bits <= dst_bits;
  }
  if (!dst_dtype.is_signed) {
    return precision_bits <= dst_bits &&
        !(dst_bits == 32u && precision_bits == 32u);
  }
  return precision_bits < dst_bits;
}

template <typename DtypeInfoT>
bool integral_storage_matches_dst_dtype(
    uint32_t source_sample_bytes, bool source_is_signed, const DtypeInfoT& dst_dtype) {
  return !dst_dtype.is_float &&
      dst_dtype.bytes == source_sample_bytes &&
      dst_dtype.is_signed == source_is_signed;
}

template <typename DstT>
inline void store_scalar(uint8_t* dst, DstT value) {
  std::memcpy(dst, &value, sizeof(DstT));
}

template <typename DstT>
inline void write_typed_integral_mono_row(
    uint8_t* dst_row_bytes, std::size_t cols, const int32_t* src_row) {
  if constexpr (std::is_same_v<DstT, int32_t>) {
    std::memcpy(dst_row_bytes, src_row, cols * sizeof(DstT));
    return;
  }

  const bool typed_aligned =
      (reinterpret_cast<std::uintptr_t>(dst_row_bytes) % alignof(DstT)) == 0;
  if (typed_aligned) {
    auto* dst_row = reinterpret_cast<DstT*>(dst_row_bytes);
    for (std::size_t c = 0; c < cols; ++c) {
      dst_row[c] = static_cast<DstT>(src_row[c]);
    }
    return;
  }

  for (std::size_t c = 0; c < cols; ++c) {
    store_scalar(dst_row_bytes + c * sizeof(DstT), static_cast<DstT>(src_row[c]));
  }
}

template <typename DstT, typename LoadSampleFn>
inline void write_typed_integral_loaded_row(
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
    store_scalar(dst_row_bytes + c * sizeof(DstT),
        static_cast<DstT>(load_sample(c)));
  }
}

template <typename DstT, typename LoadSampleFn>
inline void write_typed_integral_interleaved_row(
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
      store_scalar(
          dst_row_bytes + pixel_base + comp * sizeof(DstT),
          static_cast<DstT>(load_sample(c, comp)));
    }
  }
}

template <typename DstT, typename LoadSampleFn>
inline void write_loaded_integral_rows_as(
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
        write_typed_integral_loaded_row<DstT>(
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
    write_typed_integral_interleaved_row<DstT>(
        dst_row, cols, samples,
        [&](std::size_t col, std::size_t comp) -> int32_t {
          return load_sample(row, col, comp);
        });
  }
}

template <typename LoadSampleFn>
loaded_integral_write_status write_loaded_integral_rows(
    const pixel_decoder_request* request,
    uint64_t row_stride,
    bool output_planar,
    LoadSampleFn&& load_sample) {
  if (request == nullptr || request->output.dst == nullptr) {
    return loaded_integral_write_status::unsupported_dst_dtype;
  }

  switch (request->output.dst_dtype) {
  case PIXEL_DTYPE_U8:
    write_loaded_integral_rows_as<uint8_t>(
        request, row_stride, output_planar, load_sample);
    return loaded_integral_write_status::ok;
  case PIXEL_DTYPE_S8:
    write_loaded_integral_rows_as<int8_t>(
        request, row_stride, output_planar, load_sample);
    return loaded_integral_write_status::ok;
  case PIXEL_DTYPE_U16:
    write_loaded_integral_rows_as<uint16_t>(
        request, row_stride, output_planar, load_sample);
    return loaded_integral_write_status::ok;
  case PIXEL_DTYPE_S16:
    write_loaded_integral_rows_as<int16_t>(
        request, row_stride, output_planar, load_sample);
    return loaded_integral_write_status::ok;
  case PIXEL_DTYPE_U32:
    write_loaded_integral_rows_as<uint32_t>(
        request, row_stride, output_planar, load_sample);
    return loaded_integral_write_status::ok;
  case PIXEL_DTYPE_S32:
    write_loaded_integral_rows_as<int32_t>(
        request, row_stride, output_planar, load_sample);
    return loaded_integral_write_status::ok;
  default:
    return loaded_integral_write_status::unsupported_dst_dtype;
  }
}

}  // namespace pixel::codec_common
