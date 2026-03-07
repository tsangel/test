#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "pixel_decoder_plugin_abi_v2.h"

namespace pixel::codec_common_v2 {

enum class mono_transform_write_status_v2 {
  ok,
  unsupported_transform_kind,
  unsupported_dst_dtype,
};

enum class loaded_integral_write_status_v2 {
  ok,
  unsupported_dst_dtype,
};

template <typename DtypeInfoT>
bool decoded_integral_component_fits_dst_dtype_v2(
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
bool integral_storage_matches_dst_dtype_v2(
    uint32_t source_sample_bytes, bool source_is_signed, const DtypeInfoT& dst_dtype) {
  return !dst_dtype.is_float &&
      dst_dtype.bytes == source_sample_bytes &&
      dst_dtype.is_signed == source_is_signed;
}

template <typename DstT>
inline void store_scalar_v2(uint8_t* dst, DstT value) {
  std::memcpy(dst, &value, sizeof(DstT));
}

template <typename DstT>
inline void write_typed_integral_mono_row_v2(
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
    store_scalar_v2(dst_row_bytes + c * sizeof(DstT), static_cast<DstT>(src_row[c]));
  }
}

template <typename DstT, typename LoadSampleFn>
inline void write_typed_integral_loaded_row_v2(
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
    store_scalar_v2(dst_row_bytes + c * sizeof(DstT),
        static_cast<DstT>(load_sample(c)));
  }
}

template <typename DstT, typename LoadSampleFn>
inline void write_typed_integral_interleaved_row_v2(
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
      store_scalar_v2(
          dst_row_bytes + pixel_base + comp * sizeof(DstT),
          static_cast<DstT>(load_sample(c, comp)));
    }
  }
}

template <typename DstT, typename LoadSampleFn>
inline void write_loaded_integral_rows_as_v2(
    const pixel_decoder_request_v2* request,
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
        write_typed_integral_loaded_row_v2<DstT>(
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
    write_typed_integral_interleaved_row_v2<DstT>(
        dst_row, cols, samples,
        [&](std::size_t col, std::size_t comp) -> int32_t {
          return load_sample(row, col, comp);
        });
  }
}

template <typename LoadSampleFn>
loaded_integral_write_status_v2 write_loaded_integral_rows_v2(
    const pixel_decoder_request_v2* request,
    uint64_t row_stride,
    bool output_planar,
    LoadSampleFn&& load_sample) {
  if (request == nullptr || request->output.dst == nullptr) {
    return loaded_integral_write_status_v2::unsupported_dst_dtype;
  }

  switch (request->output.dst_dtype) {
  case PIXEL_DTYPE_U8_V2:
    write_loaded_integral_rows_as_v2<uint8_t>(
        request, row_stride, output_planar, load_sample);
    return loaded_integral_write_status_v2::ok;
  case PIXEL_DTYPE_S8_V2:
    write_loaded_integral_rows_as_v2<int8_t>(
        request, row_stride, output_planar, load_sample);
    return loaded_integral_write_status_v2::ok;
  case PIXEL_DTYPE_U16_V2:
    write_loaded_integral_rows_as_v2<uint16_t>(
        request, row_stride, output_planar, load_sample);
    return loaded_integral_write_status_v2::ok;
  case PIXEL_DTYPE_S16_V2:
    write_loaded_integral_rows_as_v2<int16_t>(
        request, row_stride, output_planar, load_sample);
    return loaded_integral_write_status_v2::ok;
  case PIXEL_DTYPE_U32_V2:
    write_loaded_integral_rows_as_v2<uint32_t>(
        request, row_stride, output_planar, load_sample);
    return loaded_integral_write_status_v2::ok;
  case PIXEL_DTYPE_S32_V2:
    write_loaded_integral_rows_as_v2<int32_t>(
        request, row_stride, output_planar, load_sample);
    return loaded_integral_write_status_v2::ok;
  default:
    return loaded_integral_write_status_v2::unsupported_dst_dtype;
  }
}

inline bool apply_decoder_value_transform_v2(
    const pixel_decoder_request_v2* request,
    uint32_t transform_kind,
    int32_t sv,
    double* out_value) {
  if (request == nullptr || out_value == nullptr) {
    return false;
  }

  if (transform_kind == PIXEL_DECODER_VALUE_TRANSFORM_NONE_V2) {
    *out_value = static_cast<double>(sv);
    return true;
  }

  if (transform_kind == PIXEL_DECODER_VALUE_TRANSFORM_RESCALE_V2) {
    const double slope = request->value_transform.rescale_slope;
    const double intercept = request->value_transform.rescale_intercept;
    *out_value = static_cast<double>(sv) * slope + intercept;
    return true;
  }

  if (transform_kind == PIXEL_DECODER_VALUE_TRANSFORM_MODALITY_LUT_V2) {
    const int64_t first_mapped = request->value_transform.lut_first_mapped;
    const uint64_t count = request->value_transform.lut_value_count;
    if (count == 0) {
      *out_value = 0.0;
      return true;
    }

    int64_t idx = static_cast<int64_t>(sv) - first_mapped;
    if (idx < 0) {
      idx = 0;
    }
    const int64_t max_idx = static_cast<int64_t>(count - 1);
    if (idx > max_idx) {
      idx = max_idx;
    }

    float lut_value = 0.0f;
    const uint8_t* lut_data = request->value_transform.lut_values_f32.data;
    std::memcpy(&lut_value,
        lut_data + static_cast<std::size_t>(idx) * sizeof(float),
        sizeof(float));
    *out_value = static_cast<double>(lut_value);
    return true;
  }

  return false;
}

template <typename LoadSampleFn>
mono_transform_write_status_v2 write_mono_value_transform_rows_v2(
    const pixel_decoder_request_v2* request,
    uint64_t row_stride,
    uint32_t transform_kind,
    LoadSampleFn&& load_sample) {
  if (request == nullptr || request->output.dst == nullptr) {
    return mono_transform_write_status_v2::unsupported_dst_dtype;
  }

  const std::size_t rows = static_cast<std::size_t>(request->frame.rows);
  const std::size_t cols = static_cast<std::size_t>(request->frame.cols);
  const std::size_t row_stride_sz = static_cast<std::size_t>(row_stride);

  if (request->output.dst_dtype == PIXEL_DTYPE_F32_V2) {
    const bool typed_aligned =
        (reinterpret_cast<std::uintptr_t>(request->output.dst) % alignof(float)) == 0 &&
        (row_stride_sz % alignof(float)) == 0;
    for (std::size_t r = 0; r < rows; ++r) {
      uint8_t* dst_row_bytes = request->output.dst + r * row_stride_sz;
      float* dst_row = typed_aligned ? reinterpret_cast<float*>(dst_row_bytes) : nullptr;
      for (std::size_t c = 0; c < cols; ++c) {
        double value = 0.0;
        if (!apply_decoder_value_transform_v2(
                request, transform_kind, load_sample(r, c), &value)) {
          return mono_transform_write_status_v2::unsupported_transform_kind;
        }
        const float out = static_cast<float>(value);
        if (dst_row != nullptr) {
          dst_row[c] = out;
        } else {
          store_scalar_v2(dst_row_bytes + c * sizeof(float), out);
        }
      }
    }
    return mono_transform_write_status_v2::ok;
  }

  if (request->output.dst_dtype == PIXEL_DTYPE_F64_V2) {
    const bool typed_aligned =
        (reinterpret_cast<std::uintptr_t>(request->output.dst) % alignof(double)) == 0 &&
        (row_stride_sz % alignof(double)) == 0;
    for (std::size_t r = 0; r < rows; ++r) {
      uint8_t* dst_row_bytes = request->output.dst + r * row_stride_sz;
      double* dst_row = typed_aligned ? reinterpret_cast<double*>(dst_row_bytes) : nullptr;
      for (std::size_t c = 0; c < cols; ++c) {
        double value = 0.0;
        if (!apply_decoder_value_transform_v2(
                request, transform_kind, load_sample(r, c), &value)) {
          return mono_transform_write_status_v2::unsupported_transform_kind;
        }
        if (dst_row != nullptr) {
          dst_row[c] = value;
        } else {
          store_scalar_v2(dst_row_bytes + c * sizeof(double), value);
        }
      }
    }
    return mono_transform_write_status_v2::ok;
  }

  return mono_transform_write_status_v2::unsupported_dst_dtype;
}

}  // namespace pixel::codec_common_v2
