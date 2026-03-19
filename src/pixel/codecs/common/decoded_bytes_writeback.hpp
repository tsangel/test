#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "integral_hotpath.hpp"

namespace pixel::codec_common {

template <typename DstT, uint32_t SourceBytes, bool SourceSigned>
inline void write_decoded_native_bytes_as(
    const pixel_decoder_request* request,
    const uint8_t* decoded,
    std::size_t source_row_bytes,
    bool source_planar,
    uint64_t row_stride,
    bool output_planar,
    int source_bits) {
  const std::size_t rows = static_cast<std::size_t>(request->frame.rows);
  const std::size_t cols = static_cast<std::size_t>(request->frame.cols);
  const std::size_t samples = static_cast<std::size_t>(request->frame.samples_per_pixel);
  const std::size_t row_stride_sz = static_cast<std::size_t>(row_stride);
  const std::size_t source_plane_bytes = source_row_bytes * rows;

  if (source_planar) {
    if (output_planar && samples > 1) {
      const std::size_t output_plane_bytes = row_stride_sz * rows;
      for (std::size_t comp = 0; comp < samples; ++comp) {
        const uint8_t* src_plane = decoded + comp * source_plane_bytes;
        uint8_t* dst_plane = request->output.dst + comp * output_plane_bytes;
        for (std::size_t row = 0; row < rows; ++row) {
          const uint8_t* src_row = src_plane + row * source_row_bytes;
          uint8_t* dst_row = dst_plane + row * row_stride_sz;
          write_loaded_converted_row<DstT>(
              dst_row, cols,
              [&](std::size_t col) -> int32_t {
                return load_native_integral_as_i32<SourceBytes, SourceSigned>(
                    src_row + col * SourceBytes, source_bits);
              });
        }
      }
      return;
    }

    for (std::size_t row = 0; row < rows; ++row) {
      std::array<const uint8_t*, 4> src_rows{};
      for (std::size_t comp = 0; comp < samples; ++comp) {
        src_rows[comp] = decoded + comp * source_plane_bytes + row * source_row_bytes;
      }
      uint8_t* dst_row = request->output.dst + row * row_stride_sz;
      write_loaded_converted_interleaved_row<DstT>(
          dst_row, cols, samples,
          [&](std::size_t col, std::size_t comp) -> int32_t {
            return load_native_integral_as_i32<SourceBytes, SourceSigned>(
                src_rows[comp] + col * SourceBytes, source_bits);
          });
    }
    return;
  }

  const std::size_t source_pixel_stride = samples * SourceBytes;
  if (output_planar && samples > 1) {
    const std::size_t output_plane_bytes = row_stride_sz * rows;
    for (std::size_t comp = 0; comp < samples; ++comp) {
      uint8_t* dst_plane = request->output.dst + comp * output_plane_bytes;
      for (std::size_t row = 0; row < rows; ++row) {
        const uint8_t* src_row = decoded + row * source_row_bytes + comp * SourceBytes;
        uint8_t* dst_row = dst_plane + row * row_stride_sz;
        write_loaded_converted_row<DstT>(
            dst_row, cols,
            [&](std::size_t col) -> int32_t {
              return load_native_integral_as_i32<SourceBytes, SourceSigned>(
                  src_row + col * source_pixel_stride, source_bits);
            });
      }
    }
    return;
  }

  for (std::size_t row = 0; row < rows; ++row) {
    const uint8_t* src_row = decoded + row * source_row_bytes;
    uint8_t* dst_row = request->output.dst + row * row_stride_sz;
    write_loaded_converted_interleaved_row<DstT>(
        dst_row, cols, samples,
        [&](std::size_t col, std::size_t comp) -> int32_t {
          return load_native_integral_as_i32<SourceBytes, SourceSigned>(
              src_row + col * source_pixel_stride + comp * SourceBytes,
              source_bits);
        });
  }
}

template <uint32_t SourceBytes, bool SourceSigned>
inline bool write_decoded_native_bytes_cast_to_dst(
    const pixel_decoder_request* request,
    const uint8_t* decoded,
    std::size_t source_row_bytes,
    bool source_planar,
    uint64_t row_stride,
    bool output_planar,
    int source_bits) {
  if (request == nullptr || decoded == nullptr || request->output.dst == nullptr) {
    return false;
  }

  const std::size_t rows = static_cast<std::size_t>(request->frame.rows);
  const std::size_t samples = static_cast<std::size_t>(request->frame.samples_per_pixel);
  const std::size_t source_plane_bytes = source_row_bytes * rows;

  return write_loaded_cast_rows_to_dst(
      request, row_stride, output_planar,
      [&](std::size_t row, std::size_t col, std::size_t comp) -> int32_t {
        if (source_planar) {
          const uint8_t* src_plane = decoded + comp * source_plane_bytes;
          const uint8_t* src_row = src_plane + row * source_row_bytes;
          return load_native_integral_as_i32<SourceBytes, SourceSigned>(
              src_row + col * SourceBytes, source_bits);
        }

        const std::size_t source_pixel_stride = samples * SourceBytes;
        const uint8_t* src_row = decoded + row * source_row_bytes;
        return load_native_integral_as_i32<SourceBytes, SourceSigned>(
            src_row + col * source_pixel_stride + comp * SourceBytes,
            source_bits);
      });
}

template <uint32_t SourceBytes, bool SourceSigned>
inline bool write_decoded_native_bytes_to_dst(
    const pixel_decoder_request* request,
    const uint8_t* decoded,
    std::size_t source_row_bytes,
    bool source_planar,
    uint64_t row_stride,
    bool output_planar,
    int source_bits) {
  if (request == nullptr || decoded == nullptr || request->output.dst == nullptr) {
    return false;
  }

  switch (request->output.dst_dtype) {
  case PIXEL_DTYPE_U8:
    write_decoded_native_bytes_as<uint8_t, SourceBytes, SourceSigned>(
        request, decoded, source_row_bytes, source_planar,
        row_stride, output_planar, source_bits);
    return true;
  case PIXEL_DTYPE_S8:
    write_decoded_native_bytes_as<int8_t, SourceBytes, SourceSigned>(
        request, decoded, source_row_bytes, source_planar,
        row_stride, output_planar, source_bits);
    return true;
  case PIXEL_DTYPE_U16:
    write_decoded_native_bytes_as<uint16_t, SourceBytes, SourceSigned>(
        request, decoded, source_row_bytes, source_planar,
        row_stride, output_planar, source_bits);
    return true;
  case PIXEL_DTYPE_S16:
    write_decoded_native_bytes_as<int16_t, SourceBytes, SourceSigned>(
        request, decoded, source_row_bytes, source_planar,
        row_stride, output_planar, source_bits);
    return true;
  case PIXEL_DTYPE_U32:
    write_decoded_native_bytes_as<uint32_t, SourceBytes, SourceSigned>(
        request, decoded, source_row_bytes, source_planar,
        row_stride, output_planar, source_bits);
    return true;
  case PIXEL_DTYPE_S32:
    write_decoded_native_bytes_as<int32_t, SourceBytes, SourceSigned>(
        request, decoded, source_row_bytes, source_planar,
        row_stride, output_planar, source_bits);
    return true;
  case PIXEL_DTYPE_F32:
    write_decoded_native_bytes_as<float, SourceBytes, SourceSigned>(
        request, decoded, source_row_bytes, source_planar,
        row_stride, output_planar, source_bits);
    return true;
  case PIXEL_DTYPE_F64:
    write_decoded_native_bytes_as<double, SourceBytes, SourceSigned>(
        request, decoded, source_row_bytes, source_planar,
        row_stride, output_planar, source_bits);
    return true;
  default:
    return false;
  }
}

}  // namespace pixel::codec_common
