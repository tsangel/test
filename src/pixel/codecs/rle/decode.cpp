#include <array>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <type_traits>
#include <utility>
#include <vector>

#include "../common/decode_fastpath.hpp"
#include "../common/decode_info.hpp"
#include "../common/integral_hotpath.hpp"
#include "internal.hpp"

namespace pixel::rle_codec {

namespace {

template <std::size_t Bytes>
inline void copy_sample_bytes(const std::uint8_t* src, std::uint8_t* dst) noexcept {
  std::memcpy(dst, src, Bytes);
}

template <std::size_t Bytes, std::size_t SamplesPerPixel>
void copy_planar_to_interleaved_fast(const std::uint8_t* src_frame,
    std::uint8_t* dst_base, std::size_t rows, std::size_t cols,
    std::size_t src_row_bytes, std::size_t dst_row_bytes) noexcept {
  static_assert((Bytes == 1 || Bytes == 2), "fast path supports Bytes=1/2");
  static_assert((SamplesPerPixel == 3 || SamplesPerPixel == 4),
      "fast path supports SPP=3/4");

  const std::size_t src_plane_bytes = src_row_bytes * rows;
  for (std::size_t r = 0; r < rows; ++r) {
    std::uint8_t* dst = dst_base + r * dst_row_bytes;
    const std::uint8_t* s0 = src_frame + r * src_row_bytes;
    const std::uint8_t* s1 = s0 + src_plane_bytes;
    const std::uint8_t* s2 = s1 + src_plane_bytes;
    if constexpr (SamplesPerPixel == 3) {
      for (std::size_t c = 0; c < cols; ++c) {
        if constexpr (Bytes == 1) {
          *dst++ = *s0++;
          *dst++ = *s1++;
          *dst++ = *s2++;
        } else {
          copy_sample_bytes<2>(s0, dst);
          s0 += 2;
          dst += 2;

          copy_sample_bytes<2>(s1, dst);
          s1 += 2;
          dst += 2;

          copy_sample_bytes<2>(s2, dst);
          s2 += 2;
          dst += 2;
        }
      }
    } else {
      const std::uint8_t* s3 = s2 + src_plane_bytes;
      for (std::size_t c = 0; c < cols; ++c) {
        if constexpr (Bytes == 1) {
          *dst++ = *s0++;
          *dst++ = *s1++;
          *dst++ = *s2++;
          *dst++ = *s3++;
        } else {
          copy_sample_bytes<2>(s0, dst);
          s0 += 2;
          dst += 2;

          copy_sample_bytes<2>(s1, dst);
          s1 += 2;
          dst += 2;

          copy_sample_bytes<2>(s2, dst);
          s2 += 2;
          dst += 2;

          copy_sample_bytes<2>(s3, dst);
          s3 += 2;
          dst += 2;
        }
      }
    }
  }
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

bool load_raw_sample(const uint8_t* src, uint32_t sample_bytes, uint32_t* out_raw) {
  if (src == nullptr || out_raw == nullptr) {
    return false;
  }

  switch (sample_bytes) {
  case 1:
    *out_raw = src[0];
    return true;
  case 2: {
    uint16_t value = 0;
    std::memcpy(&value, src, sizeof(value));
    *out_raw = value;
    return true;
  }
  case 4: {
    uint32_t value = 0;
    std::memcpy(&value, src, sizeof(value));
    *out_raw = value;
    return true;
  }
  default:
    return false;
  }
}

bool load_integral_sample(const uint8_t* src, uint32_t sample_bytes, bool is_signed,
    int bits_stored, int32_t* out_value) {
  if (src == nullptr || out_value == nullptr) {
    return false;
  }

  uint32_t raw = 0;
  if (!load_raw_sample(src, sample_bytes, &raw)) {
    return false;
  }

  if (is_signed) {
    *out_value = sign_extend_u32(raw, bits_stored);
    return true;
  }

  raw &= bit_mask_u32(bits_stored);
  *out_value = static_cast<int32_t>(raw);
  return true;
}

bool write_float_sample(uint8_t dst_dtype, double sample, uint8_t* dst) {
  if (dst == nullptr) {
    return false;
  }
  switch (dst_dtype) {
  case PIXEL_DTYPE_F32: {
    const float value = static_cast<float>(sample);
    std::memcpy(dst, &value, sizeof(value));
    return true;
  }
  case PIXEL_DTYPE_F64:
    std::memcpy(dst, &sample, sizeof(sample));
    return true;
  default:
    return false;
  }
}

bool write_integer_sample(uint8_t dst_dtype, int32_t sample, uint8_t* dst) {
  if (dst == nullptr) {
    return false;
  }
  switch (dst_dtype) {
  case PIXEL_DTYPE_U8: {
    const auto value = static_cast<std::uint8_t>(sample);
    std::memcpy(dst, &value, sizeof(value));
    return true;
  }
  case PIXEL_DTYPE_S8: {
    const auto value = static_cast<std::int8_t>(sample);
    std::memcpy(dst, &value, sizeof(value));
    return true;
  }
  case PIXEL_DTYPE_U16: {
    const auto value = static_cast<std::uint16_t>(sample);
    std::memcpy(dst, &value, sizeof(value));
    return true;
  }
  case PIXEL_DTYPE_S16: {
    const auto value = static_cast<std::int16_t>(sample);
    std::memcpy(dst, &value, sizeof(value));
    return true;
  }
  case PIXEL_DTYPE_U32: {
    const auto value = static_cast<std::uint32_t>(sample);
    std::memcpy(dst, &value, sizeof(value));
    return true;
  }
  case PIXEL_DTYPE_S32: {
    const auto value = static_cast<std::int32_t>(sample);
    std::memcpy(dst, &value, sizeof(value));
    return true;
  }
  default:
    return false;
  }
}

void set_rle_decode_info(const pixel_decoder_request* request) noexcept {
  ::pixel::codec_common::set_decoder_info(
      request,
      ::pixel::codec_common::default_color_space_for_sample_count(
          request->frame.samples_per_pixel),
      PIXEL_ENCODED_LOSSY_STATE_LOSSLESS,
      request->output.dst_dtype,
      ::pixel::codec_common::decoded_planar_code_from_request(
          request->output.dst_planar),
      request->frame.bits_stored > 0
          ? static_cast<uint16_t>(request->frame.bits_stored)
          : uint16_t{0});
}

[[nodiscard]] bool integral_storage_is_already_normalized(
    std::size_t sample_bytes, int bits_stored) noexcept {
  return bits_stored == static_cast<int>(sample_bytes * 8u);
}

template <uint32_t SampleBytes, bool SourceSigned>
using MatchingStorageValueT = std::conditional_t<
    SourceSigned,
    std::conditional_t<SampleBytes == 1, std::int8_t,
        std::conditional_t<SampleBytes == 2, std::int16_t, std::int32_t>>,
    std::conditional_t<SampleBytes == 1, std::uint8_t,
        std::conditional_t<SampleBytes == 2, std::uint16_t, std::uint32_t>>>;

template <uint32_t SampleBytes, bool SourceSigned>
inline MatchingStorageValueT<SampleBytes, SourceSigned>
load_normalized_matching_storage_value(const uint8_t* src, int bits_stored) noexcept {
  return static_cast<MatchingStorageValueT<SampleBytes, SourceSigned>>(
      ::pixel::codec_common::normalize_loaded_integral<SourceSigned>(
          ::pixel::codec_common::load_raw_native_integral<SampleBytes>(src),
          bits_stored));
}

template <uint32_t SampleBytes, bool SourceSigned>
void write_normalized_single_channel_rows(
    const uint8_t* decoded_planar,
    uint8_t* dst,
    std::size_t rows,
    std::size_t cols,
    std::size_t row_payload,
    std::size_t row_stride,
    int bits_stored) {
  using DstT = MatchingStorageValueT<SampleBytes, SourceSigned>;

  for (std::size_t r = 0; r < rows; ++r) {
    const uint8_t* src_row = decoded_planar + r * row_payload;
    uint8_t* dst_row_bytes = dst + r * row_stride;
    const bool typed_aligned =
        (reinterpret_cast<std::uintptr_t>(dst_row_bytes) % alignof(DstT)) == 0;
    if (typed_aligned) {
      auto* dst_row = reinterpret_cast<DstT*>(dst_row_bytes);
      for (std::size_t c = 0; c < cols; ++c) {
        dst_row[c] = load_normalized_matching_storage_value<SampleBytes, SourceSigned>(
            src_row + c * SampleBytes, bits_stored);
      }
      continue;
    }

    for (std::size_t c = 0; c < cols; ++c) {
      ::pixel::codec_common::store_scalar_unaligned(
          dst_row_bytes + c * sizeof(DstT),
          load_normalized_matching_storage_value<SampleBytes, SourceSigned>(
              src_row + c * SampleBytes, bits_stored));
    }
  }
}

template <uint32_t SampleBytes, bool SourceSigned>
void write_normalized_planar_rows(
    const uint8_t* decoded_planar,
    uint8_t* dst,
    std::size_t rows,
    std::size_t cols,
    std::size_t samples,
    std::size_t row_payload,
    std::size_t plane_bytes,
    std::size_t row_stride,
    int bits_stored) {
  using DstT = MatchingStorageValueT<SampleBytes, SourceSigned>;
  const std::size_t dst_plane_bytes = row_stride * rows;

  for (std::size_t comp = 0; comp < samples; ++comp) {
    const uint8_t* src_plane = decoded_planar + comp * plane_bytes;
    uint8_t* dst_plane = dst + comp * dst_plane_bytes;
    for (std::size_t r = 0; r < rows; ++r) {
      const uint8_t* src_row = src_plane + r * row_payload;
      uint8_t* dst_row_bytes = dst_plane + r * row_stride;
      const bool typed_aligned =
          (reinterpret_cast<std::uintptr_t>(dst_row_bytes) % alignof(DstT)) == 0;
      if (typed_aligned) {
        auto* dst_row = reinterpret_cast<DstT*>(dst_row_bytes);
        for (std::size_t c = 0; c < cols; ++c) {
          dst_row[c] = load_normalized_matching_storage_value<SampleBytes, SourceSigned>(
              src_row + c * SampleBytes, bits_stored);
        }
        continue;
      }

      for (std::size_t c = 0; c < cols; ++c) {
        ::pixel::codec_common::store_scalar_unaligned(
            dst_row_bytes + c * sizeof(DstT),
            load_normalized_matching_storage_value<SampleBytes, SourceSigned>(
                src_row + c * SampleBytes, bits_stored));
      }
    }
  }
}

template <uint32_t SampleBytes, std::size_t SamplesPerPixel, bool SourceSigned>
void copy_planar_to_interleaved_normalized_fast(const uint8_t* src_frame,
    uint8_t* dst_base, std::size_t rows, std::size_t cols,
    std::size_t src_row_bytes, std::size_t dst_row_bytes,
    int bits_stored) noexcept {
  using DstT = MatchingStorageValueT<SampleBytes, SourceSigned>;
  static_assert((SampleBytes == 1 || SampleBytes == 2),
      "normalized fast path supports SampleBytes=1/2");
  static_assert((SamplesPerPixel == 3 || SamplesPerPixel == 4),
      "normalized fast path supports SPP=3/4");

  const std::size_t src_plane_bytes = src_row_bytes * rows;
  for (std::size_t r = 0; r < rows; ++r) {
    uint8_t* dst_row_bytes_ptr = dst_base + r * dst_row_bytes;
    const std::uint8_t* s0 = src_frame + r * src_row_bytes;
    const std::uint8_t* s1 = s0 + src_plane_bytes;
    const std::uint8_t* s2 = s1 + src_plane_bytes;
    const bool typed_aligned =
        (reinterpret_cast<std::uintptr_t>(dst_row_bytes_ptr) % alignof(DstT)) == 0;

    if (typed_aligned) {
      auto* dst_row = reinterpret_cast<DstT*>(dst_row_bytes_ptr);
      if constexpr (SamplesPerPixel == 3) {
        for (std::size_t c = 0; c < cols; ++c) {
          const std::size_t pixel_base = c * 3u;
          dst_row[pixel_base] =
              load_normalized_matching_storage_value<SampleBytes, SourceSigned>(s0, bits_stored);
          dst_row[pixel_base + 1u] =
              load_normalized_matching_storage_value<SampleBytes, SourceSigned>(s1, bits_stored);
          dst_row[pixel_base + 2u] =
              load_normalized_matching_storage_value<SampleBytes, SourceSigned>(s2, bits_stored);
          s0 += SampleBytes;
          s1 += SampleBytes;
          s2 += SampleBytes;
        }
      } else {
        const std::uint8_t* s3 = s2 + src_plane_bytes;
        for (std::size_t c = 0; c < cols; ++c) {
          const std::size_t pixel_base = c * 4u;
          dst_row[pixel_base] =
              load_normalized_matching_storage_value<SampleBytes, SourceSigned>(s0, bits_stored);
          dst_row[pixel_base + 1u] =
              load_normalized_matching_storage_value<SampleBytes, SourceSigned>(s1, bits_stored);
          dst_row[pixel_base + 2u] =
              load_normalized_matching_storage_value<SampleBytes, SourceSigned>(s2, bits_stored);
          dst_row[pixel_base + 3u] =
              load_normalized_matching_storage_value<SampleBytes, SourceSigned>(s3, bits_stored);
          s0 += SampleBytes;
          s1 += SampleBytes;
          s2 += SampleBytes;
          s3 += SampleBytes;
        }
      }
      continue;
    }

    if constexpr (SamplesPerPixel == 3) {
      for (std::size_t c = 0; c < cols; ++c) {
        const std::size_t pixel_base = (c * 3u) * sizeof(DstT);
        ::pixel::codec_common::store_scalar_unaligned(
            dst_row_bytes_ptr + pixel_base,
            load_normalized_matching_storage_value<SampleBytes, SourceSigned>(s0, bits_stored));
        ::pixel::codec_common::store_scalar_unaligned(
            dst_row_bytes_ptr + pixel_base + sizeof(DstT),
            load_normalized_matching_storage_value<SampleBytes, SourceSigned>(s1, bits_stored));
        ::pixel::codec_common::store_scalar_unaligned(
            dst_row_bytes_ptr + pixel_base + sizeof(DstT) * 2u,
            load_normalized_matching_storage_value<SampleBytes, SourceSigned>(s2, bits_stored));
        s0 += SampleBytes;
        s1 += SampleBytes;
        s2 += SampleBytes;
      }
    } else {
      const std::uint8_t* s3 = s2 + src_plane_bytes;
      for (std::size_t c = 0; c < cols; ++c) {
        const std::size_t pixel_base = (c * 4u) * sizeof(DstT);
        ::pixel::codec_common::store_scalar_unaligned(
            dst_row_bytes_ptr + pixel_base,
            load_normalized_matching_storage_value<SampleBytes, SourceSigned>(s0, bits_stored));
        ::pixel::codec_common::store_scalar_unaligned(
            dst_row_bytes_ptr + pixel_base + sizeof(DstT),
            load_normalized_matching_storage_value<SampleBytes, SourceSigned>(s1, bits_stored));
        ::pixel::codec_common::store_scalar_unaligned(
            dst_row_bytes_ptr + pixel_base + sizeof(DstT) * 2u,
            load_normalized_matching_storage_value<SampleBytes, SourceSigned>(s2, bits_stored));
        ::pixel::codec_common::store_scalar_unaligned(
            dst_row_bytes_ptr + pixel_base + sizeof(DstT) * 3u,
            load_normalized_matching_storage_value<SampleBytes, SourceSigned>(s3, bits_stored));
        s0 += SampleBytes;
        s1 += SampleBytes;
        s2 += SampleBytes;
        s3 += SampleBytes;
      }
    }
  }
}

template <uint32_t SampleBytes, bool SourceSigned>
void write_normalized_interleaved_rows(
    const uint8_t* decoded_planar,
    uint8_t* dst,
    std::size_t rows,
    std::size_t cols,
    std::size_t samples,
    std::size_t row_payload,
    std::size_t plane_bytes,
    std::size_t row_stride,
    int bits_stored) {
  using DstT = MatchingStorageValueT<SampleBytes, SourceSigned>;

  for (std::size_t r = 0; r < rows; ++r) {
    uint8_t* dst_row_bytes = dst + r * row_stride;
    const bool typed_aligned =
        (reinterpret_cast<std::uintptr_t>(dst_row_bytes) % alignof(DstT)) == 0;
    if (typed_aligned) {
      auto* dst_row = reinterpret_cast<DstT*>(dst_row_bytes);
      for (std::size_t c = 0; c < cols; ++c) {
        const std::size_t pixel_base = c * samples;
        for (std::size_t comp = 0; comp < samples; ++comp) {
          const uint8_t* src_row =
              decoded_planar + comp * plane_bytes + r * row_payload;
          dst_row[pixel_base + comp] =
              load_normalized_matching_storage_value<SampleBytes, SourceSigned>(
                  src_row + c * SampleBytes, bits_stored);
        }
      }
      continue;
    }

    for (std::size_t c = 0; c < cols; ++c) {
      const std::size_t pixel_base = (c * samples) * sizeof(DstT);
      for (std::size_t comp = 0; comp < samples; ++comp) {
        const uint8_t* src_row =
            decoded_planar + comp * plane_bytes + r * row_payload;
        ::pixel::codec_common::store_scalar_unaligned(
            dst_row_bytes + pixel_base + comp * sizeof(DstT),
            load_normalized_matching_storage_value<SampleBytes, SourceSigned>(
                src_row + c * SampleBytes, bits_stored));
      }
    }
  }
}

[[nodiscard]] bool write_normalized_single_channel_matching_storage_to_dst(
    const uint8_t* decoded_planar,
    uint8_t* dst,
    std::size_t rows,
    std::size_t cols,
    std::size_t row_payload,
    std::size_t row_stride,
    std::size_t sample_bytes,
    bool source_signed,
    int bits_stored) {
  switch (sample_bytes) {
  case 1:
    if (source_signed) {
      write_normalized_single_channel_rows<1, true>(
          decoded_planar, dst, rows, cols, row_payload, row_stride, bits_stored);
    } else {
      write_normalized_single_channel_rows<1, false>(
          decoded_planar, dst, rows, cols, row_payload, row_stride, bits_stored);
    }
    return true;
  case 2:
    if (source_signed) {
      write_normalized_single_channel_rows<2, true>(
          decoded_planar, dst, rows, cols, row_payload, row_stride, bits_stored);
    } else {
      write_normalized_single_channel_rows<2, false>(
          decoded_planar, dst, rows, cols, row_payload, row_stride, bits_stored);
    }
    return true;
  case 4:
    if (source_signed) {
      write_normalized_single_channel_rows<4, true>(
          decoded_planar, dst, rows, cols, row_payload, row_stride, bits_stored);
    } else {
      write_normalized_single_channel_rows<4, false>(
          decoded_planar, dst, rows, cols, row_payload, row_stride, bits_stored);
    }
    return true;
  default:
    return false;
  }
}

[[nodiscard]] bool write_normalized_matching_storage_to_dst(
    const uint8_t* decoded_planar,
    uint8_t* dst,
    std::size_t rows,
    std::size_t cols,
    std::size_t samples,
    std::size_t row_payload,
    std::size_t plane_bytes,
    std::size_t row_stride,
    bool output_planar,
    std::size_t sample_bytes,
    bool source_signed,
    int bits_stored) {
  switch (sample_bytes) {
  case 1:
    if (output_planar) {
      if (source_signed) {
        write_normalized_planar_rows<1, true>(
            decoded_planar, dst, rows, cols, samples, row_payload, plane_bytes,
            row_stride, bits_stored);
      } else {
        write_normalized_planar_rows<1, false>(
            decoded_planar, dst, rows, cols, samples, row_payload, plane_bytes,
            row_stride, bits_stored);
      }
      return true;
    }
    if (samples == 3u) {
      if (source_signed) {
        copy_planar_to_interleaved_normalized_fast<1, 3, true>(
            decoded_planar, dst, rows, cols, row_payload, row_stride, bits_stored);
      } else {
        copy_planar_to_interleaved_normalized_fast<1, 3, false>(
            decoded_planar, dst, rows, cols, row_payload, row_stride, bits_stored);
      }
      return true;
    }
    if (samples == 4u) {
      if (source_signed) {
        copy_planar_to_interleaved_normalized_fast<1, 4, true>(
            decoded_planar, dst, rows, cols, row_payload, row_stride, bits_stored);
      } else {
        copy_planar_to_interleaved_normalized_fast<1, 4, false>(
            decoded_planar, dst, rows, cols, row_payload, row_stride, bits_stored);
      }
      return true;
    }
    if (source_signed) {
      write_normalized_interleaved_rows<1, true>(
          decoded_planar, dst, rows, cols, samples, row_payload, plane_bytes,
          row_stride, bits_stored);
    } else {
      write_normalized_interleaved_rows<1, false>(
          decoded_planar, dst, rows, cols, samples, row_payload, plane_bytes,
          row_stride, bits_stored);
    }
    return true;
  case 2:
    if (output_planar) {
      if (source_signed) {
        write_normalized_planar_rows<2, true>(
            decoded_planar, dst, rows, cols, samples, row_payload, plane_bytes,
            row_stride, bits_stored);
      } else {
        write_normalized_planar_rows<2, false>(
            decoded_planar, dst, rows, cols, samples, row_payload, plane_bytes,
            row_stride, bits_stored);
      }
      return true;
    }
    if (samples == 3u) {
      if (source_signed) {
        copy_planar_to_interleaved_normalized_fast<2, 3, true>(
            decoded_planar, dst, rows, cols, row_payload, row_stride, bits_stored);
      } else {
        copy_planar_to_interleaved_normalized_fast<2, 3, false>(
            decoded_planar, dst, rows, cols, row_payload, row_stride, bits_stored);
      }
      return true;
    }
    if (samples == 4u) {
      if (source_signed) {
        copy_planar_to_interleaved_normalized_fast<2, 4, true>(
            decoded_planar, dst, rows, cols, row_payload, row_stride, bits_stored);
      } else {
        copy_planar_to_interleaved_normalized_fast<2, 4, false>(
            decoded_planar, dst, rows, cols, row_payload, row_stride, bits_stored);
      }
      return true;
    }
    if (source_signed) {
      write_normalized_interleaved_rows<2, true>(
          decoded_planar, dst, rows, cols, samples, row_payload, plane_bytes,
          row_stride, bits_stored);
    } else {
      write_normalized_interleaved_rows<2, false>(
          decoded_planar, dst, rows, cols, samples, row_payload, plane_bytes,
          row_stride, bits_stored);
    }
    return true;
  case 4:
    if (output_planar) {
      if (source_signed) {
        write_normalized_planar_rows<4, true>(
            decoded_planar, dst, rows, cols, samples, row_payload, plane_bytes,
            row_stride, bits_stored);
      } else {
        write_normalized_planar_rows<4, false>(
            decoded_planar, dst, rows, cols, samples, row_payload, plane_bytes,
            row_stride, bits_stored);
      }
      return true;
    }
    if (source_signed) {
      write_normalized_interleaved_rows<4, true>(
          decoded_planar, dst, rows, cols, samples, row_payload, plane_bytes,
          row_stride, bits_stored);
    } else {
      write_normalized_interleaved_rows<4, false>(
          decoded_planar, dst, rows, cols, samples, row_payload, plane_bytes,
          row_stride, bits_stored);
    }
    return true;
  default:
    return false;
  }
}

uint32_t load_le32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) |
      (static_cast<uint32_t>(data[1]) << 8u) |
      (static_cast<uint32_t>(data[2]) << 16u) |
      (static_cast<uint32_t>(data[3]) << 24u);
}

pixel_error_code decode_packbits_segment(DecoderCtx* ctx, std::size_t segment_index,
    const uint8_t* encoded, std::size_t encoded_size, uint8_t* decoded,
    std::size_t decoded_size) {
  std::size_t in = 0;
  std::size_t out = 0;
  while (out < decoded_size) {
    if (in >= encoded_size) {
      return fail_detail_u32(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "decode_frame",
          "RLE segment ended early at index=%u",
          static_cast<uint32_t>(segment_index));
    }

    const int8_t control = static_cast<int8_t>(encoded[in++]);
    if (control >= 0) {
      const std::size_t literal_count = static_cast<std::size_t>(control) + 1u;
      if (in + literal_count > encoded_size || out + literal_count > decoded_size) {
        return fail_detail_u32(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "decode_frame",
            "RLE literal run out of bounds at segment=%u",
            static_cast<uint32_t>(segment_index));
      }
      std::memcpy(decoded + out, encoded + in, literal_count);
      in += literal_count;
      out += literal_count;
      continue;
    }

    if (control >= -127) {
      const std::size_t repeat_count = static_cast<std::size_t>(1 - control);
      if (in >= encoded_size || out + repeat_count > decoded_size) {
        return fail_detail_u32(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "decode_frame",
            "RLE repeat run out of bounds at segment=%u",
            static_cast<uint32_t>(segment_index));
      }
      std::memset(decoded + out, encoded[in], repeat_count);
      ++in;
      out += repeat_count;
      continue;
    }

    // control == -128 : no-op
  }
  return PIXEL_CODEC_ERR_OK;
}

pixel_error_code decode_rle_frame_to_planar(DecoderCtx* ctx,
    const uint8_t* encoded_data, std::size_t encoded_size, std::size_t rows,
    std::size_t cols, std::size_t samples_per_pixel, std::size_t bytes_per_sample,
    std::vector<uint8_t>* out_decoded_planar) {
  if (out_decoded_planar == nullptr) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "decode_frame",
        "decoded_planar output pointer is null");
  }
  if (encoded_data == nullptr || encoded_size < 64u) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "decode_frame",
        "RLE codestream is shorter than 64-byte header");
  }

  std::size_t expected_segment_count = 0;
  uint64_t expected_segment_count_u64 = 0;
  if (!mul_u64(static_cast<uint64_t>(samples_per_pixel),
          static_cast<uint64_t>(bytes_per_sample),
          &expected_segment_count_u64) ||
      !u64_to_size(expected_segment_count_u64, &expected_segment_count)) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "decode_frame",
        "RLE segment count overflow");
  }
  if (expected_segment_count == 0 || expected_segment_count > 15u) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_UNSUPPORTED, "decode_frame",
        "unsupported RLE segment layout");
  }

  const uint32_t segment_count_u32 = load_le32(encoded_data);
  const std::size_t segment_count = static_cast<std::size_t>(segment_count_u32);
  if (segment_count == 0 || segment_count > 15u) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "decode_frame",
        "invalid RLE segment count");
  }
  if (segment_count < expected_segment_count) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "decode_frame",
        "RLE segment count is smaller than expected");
  }

  std::array<std::size_t, 15> offsets{};
  for (std::size_t i = 0; i < segment_count; ++i) {
    offsets[i] = static_cast<std::size_t>(load_le32(
        encoded_data + 4u + i * sizeof(uint32_t)));
  }
  for (std::size_t i = 0; i < segment_count; ++i) {
    const std::size_t start = offsets[i];
    if (start < 64u || start >= encoded_size) {
      return fail_detail_u32(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "decode_frame",
          "invalid RLE segment offset at index=%u",
          static_cast<uint32_t>(i));
    }
    const std::size_t end = (i + 1 < segment_count) ? offsets[i + 1] : encoded_size;
    if (end < start || end > encoded_size) {
      return fail_detail_u32(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "decode_frame",
          "invalid RLE segment range at index=%u",
          static_cast<uint32_t>(i));
    }
  }

  uint64_t row_payload_u64 = 0;
  if (!mul_u64(static_cast<uint64_t>(cols), static_cast<uint64_t>(bytes_per_sample),
          &row_payload_u64)) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "decode_frame",
        "RLE row payload overflow");
  }
  uint64_t plane_bytes_u64 = 0;
  if (!mul_u64(row_payload_u64, static_cast<uint64_t>(rows), &plane_bytes_u64)) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "decode_frame",
        "RLE plane bytes overflow");
  }
  uint64_t frame_bytes_u64 = 0;
  if (!mul_u64(plane_bytes_u64, static_cast<uint64_t>(samples_per_pixel), &frame_bytes_u64)) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "decode_frame",
        "RLE decoded frame bytes overflow");
  }

  std::size_t row_payload = 0;
  std::size_t plane_bytes = 0;
  std::size_t frame_bytes = 0;
  if (!u64_to_size(row_payload_u64, &row_payload) ||
      !u64_to_size(plane_bytes_u64, &plane_bytes) ||
      !u64_to_size(frame_bytes_u64, &frame_bytes)) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "decode_frame",
        "RLE size conversion overflow");
  }

  uint64_t pixels_u64 = 0;
  if (!mul_u64(static_cast<uint64_t>(rows), static_cast<uint64_t>(cols), &pixels_u64)) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "decode_frame",
        "RLE pixel count overflow");
  }
  std::size_t pixels = 0;
  if (!u64_to_size(pixels_u64, &pixels)) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "decode_frame",
        "RLE pixel count conversion overflow");
  }

  std::vector<uint8_t> decoded_planar(frame_bytes, uint8_t{0});
  std::vector<uint8_t> byte_plane(pixels, uint8_t{0});
  for (std::size_t sample = 0; sample < samples_per_pixel; ++sample) {
    uint8_t* plane = decoded_planar.data() + sample * plane_bytes;
    for (std::size_t byte_plane_index = 0; byte_plane_index < bytes_per_sample;
         ++byte_plane_index) {
      const std::size_t segment_index = sample * bytes_per_sample + byte_plane_index;
      const std::size_t segment_start = offsets[segment_index];
      const std::size_t segment_end =
          (segment_index + 1u < segment_count) ? offsets[segment_index + 1u] : encoded_size;
      const pixel_error_code decode_ec = decode_packbits_segment(
          ctx, segment_index, encoded_data + segment_start,
          segment_end - segment_start, byte_plane.data(), byte_plane.size());
      if (decode_ec != PIXEL_CODEC_ERR_OK) {
        return decode_ec;
      }

      const std::size_t component_byte_index = bytes_per_sample - 1u - byte_plane_index;
      for (std::size_t r = 0; r < rows; ++r) {
        const uint8_t* src_row = byte_plane.data() + r * cols;
        uint8_t* dst_row = plane + r * row_payload;
        for (std::size_t c = 0; c < cols; ++c) {
          dst_row[c * bytes_per_sample + component_byte_index] = src_row[c];
        }
      }
    }
  }

  *out_decoded_planar = std::move(decoded_planar);
  return PIXEL_CODEC_ERR_OK;
}

pixel_error_code validate_decoder_request(DecoderCtx* ctx,
    const pixel_decoder_request* request, DtypeInfo* out_source_dtype,
    DtypeInfo* out_dst_dtype, uint64_t* out_row_stride, bool* out_output_planar) {
  if (request->struct_size < sizeof(pixel_decoder_request) ||
      request->source.struct_size < sizeof(pixel_decoder_source) ||
      request->frame.struct_size < sizeof(pixel_decoder_frame_info) ||
      request->output.struct_size < sizeof(pixel_decoder_output)) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "decoder request struct_size is too small");
  }
  if (request->frame.codec_profile_code != ctx->codec_profile_code) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "decoder request codec_profile_code does not match configured profile");
  }
  if (!is_supported_decoder_profile(request->frame.codec_profile_code)) {
    return fail_detail_u32(ctx, PIXEL_CODEC_ERR_UNSUPPORTED, "validate",
        "unsupported decoder codec_profile_code=%u",
        request->frame.codec_profile_code);
  }
  if (request->source.source_buffer.data == nullptr ||
      request->source.source_buffer.size == 0) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "source_buffer is null or empty");
  }
  if (request->frame.rows <= 0 || request->frame.cols <= 0 ||
      request->frame.samples_per_pixel <= 0) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "rows/cols/samples_per_pixel must be positive");
  }
  if (!is_valid_planar_code(request->output.dst_planar)) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "unsupported dst_planar code");
  }
  if (request->frame.decode_mct != 0) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_UNSUPPORTED, "validate",
        "RLE decoder does not support decode_mct");
  }

  DtypeInfo source_dtype{};
  DtypeInfo dst_dtype{};
  if (!dtype_info_from_code(request->frame.source_dtype, &source_dtype) ||
      source_dtype.is_float || source_dtype.bytes == 0 || source_dtype.bytes > 4u) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "source_dtype must be integral 8/16/32-bit type");
  }
  if (!dtype_info_from_code(request->output.dst_dtype, &dst_dtype)) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "unsupported dst_dtype code");
  }
  if (request->frame.bits_stored <= 0 ||
      request->frame.bits_stored > static_cast<int32_t>(source_dtype.bytes * 8u)) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "bits_stored must be in [1, source dtype width]");
  }
  if (request->output.dst == nullptr) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "output dst is null");
  }

  const uint64_t rows = static_cast<uint64_t>(request->frame.rows);
  const uint64_t cols = static_cast<uint64_t>(request->frame.cols);
  const uint64_t samples = static_cast<uint64_t>(request->frame.samples_per_pixel);
  const bool output_planar = is_planar_code(request->output.dst_planar);
  const uint64_t output_row_components = output_planar ? 1u : samples;

  uint64_t min_row_bytes = 0;
  if (!mul_u64(cols, output_row_components, &min_row_bytes) ||
      !mul_u64(min_row_bytes, dst_dtype.bytes, &min_row_bytes)) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "destination row byte size overflow");
  }

  uint64_t row_stride = request->output.row_stride;
  if (row_stride == 0) {
    row_stride = min_row_bytes;
  }
  if (row_stride < min_row_bytes) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "row_stride is too small");
  }

  uint64_t plane_bytes = 0;
  if (!mul_u64(row_stride, rows, &plane_bytes)) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "destination plane byte size overflow");
  }

  uint64_t min_frame_bytes = plane_bytes;
  if (output_planar && samples > 1u) {
    if (!mul_u64(plane_bytes, samples, &min_frame_bytes)) {
      return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
          "destination frame byte size overflow");
    }
  }

  uint64_t frame_stride = request->output.frame_stride;
  if (frame_stride == 0) {
    frame_stride = min_frame_bytes;
  }
  if (frame_stride < min_frame_bytes) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "frame_stride is too small");
  }
  if (request->output.dst_size < frame_stride) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "destination buffer is too small");
  }

  *out_source_dtype = source_dtype;
  *out_dst_dtype = dst_dtype;
  *out_row_stride = row_stride;
  *out_output_planar = output_planar;
  return PIXEL_CODEC_ERR_OK;
}

pixel_error_code decode_single_channel_frame_fast(DecoderCtx* ctx,
    const pixel_decoder_request* request, const DtypeInfo& source_dtype,
    const DtypeInfo& dst_dtype, uint64_t row_stride_u64) {
  std::size_t rows = 0;
  std::size_t cols = 0;
  std::size_t sample_bytes = 0;
  std::size_t row_stride = 0;
  if (!u64_to_size(static_cast<uint64_t>(request->frame.rows), &rows) ||
      !u64_to_size(static_cast<uint64_t>(request->frame.cols), &cols) ||
      !u64_to_size(source_dtype.bytes, &sample_bytes) ||
      !u64_to_size(row_stride_u64, &row_stride)) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "size conversion overflow");
  }

  uint64_t row_payload_u64 = 0;
  if (!mul_u64(static_cast<uint64_t>(cols), source_dtype.bytes, &row_payload_u64)) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "decoded row payload overflow");
  }
  std::size_t row_payload = 0;
  if (!u64_to_size(row_payload_u64, &row_payload)) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "decoded row payload conversion overflow");
  }

  std::size_t source_size = 0;
  if (!u64_to_size(request->source.source_buffer.size, &source_size)) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "source buffer size conversion overflow");
  }

  std::vector<uint8_t> decoded_planar{};
  const pixel_error_code decode_ec = decode_rle_frame_to_planar(ctx,
      request->source.source_buffer.data, source_size, rows, cols, 1u, sample_bytes,
      &decoded_planar);
  if (decode_ec != PIXEL_CODEC_ERR_OK) {
    return decode_ec;
  }

  uint64_t plane_bytes_u64 = 0;
  if (!mul_u64(row_payload_u64, static_cast<uint64_t>(rows), &plane_bytes_u64)) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "decode_frame",
        "decoded plane byte size overflow");
  }
  std::size_t plane_bytes = 0;
  if (!u64_to_size(plane_bytes_u64, &plane_bytes)) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "decode_frame",
        "decoded plane byte size conversion overflow");
  }

  uint8_t* dst = request->output.dst;
  const bool matching_integral_storage =
      ::pixel::codec_common::integral_storage_matches_dst_dtype(
          static_cast<uint32_t>(sample_bytes), source_dtype.is_signed, dst_dtype);
  if (matching_integral_storage &&
      integral_storage_is_already_normalized(sample_bytes, request->frame.bits_stored)) {
    if (row_stride == row_payload) {
      std::memcpy(dst, decoded_planar.data(), plane_bytes);
    } else {
      for (std::size_t r = 0; r < rows; ++r) {
        const uint8_t* src_row = decoded_planar.data() + r * row_payload;
        uint8_t* dst_row = dst + r * row_stride;
        std::memcpy(dst_row, src_row, row_payload);
      }
    }
    clear_detail(ctx);
    return PIXEL_CODEC_ERR_OK;
  }

  if (matching_integral_storage) {
    if (!write_normalized_single_channel_matching_storage_to_dst(
            decoded_planar.data(), dst, rows, cols, row_payload, row_stride,
            sample_bytes, source_dtype.is_signed, request->frame.bits_stored)) {
      return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "decode_frame",
          "unsupported destination dtype");
    }
    clear_detail(ctx);
    return PIXEL_CODEC_ERR_OK;
  }

  for (std::size_t r = 0; r < rows; ++r) {
    const uint8_t* src_row = decoded_planar.data() + r * row_payload;
    uint8_t* dst_row = dst + r * row_stride;
    for (std::size_t col = 0; col < cols; ++col) {
      const uint8_t* src_sample = src_row + col * sample_bytes;
      int32_t sample_value = 0;
      if (!load_integral_sample(src_sample, static_cast<uint32_t>(sample_bytes),
              source_dtype.is_signed, request->frame.bits_stored, &sample_value)) {
        return fail_detail(ctx, PIXEL_CODEC_ERR_FAILED, "decode_frame",
            "failed to parse decoded RLE sample");
      }

      uint8_t* dst_sample = dst_row + col * dst_dtype.bytes;
      if (dst_dtype.is_float) {
        if (!write_float_sample(request->output.dst_dtype,
                static_cast<double>(sample_value), dst_sample)) {
          return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "decode_frame",
              "unsupported float destination dtype");
        }
      } else {
        if (!write_integer_sample(request->output.dst_dtype, sample_value, dst_sample)) {
          return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "decode_frame",
              "unsupported integer destination dtype");
        }
      }
    }
  }

  clear_detail(ctx);
  return PIXEL_CODEC_ERR_OK;
}

}  // namespace

pixel_error_code decoder_decode_frame(
    void* ctx, const pixel_decoder_request* request) {
  auto* c = static_cast<DecoderCtx*>(ctx);
  if (c == nullptr) {
    return PIXEL_CODEC_ERR_INVALID_ARGUMENT;
  }
  if (!c->configured) {
    return fail_detail(c, PIXEL_CODEC_ERR_FAILED, "decode_frame",
        "configure must be called before decode_frame");
  }
  if (request == nullptr) {
    return fail_detail(c, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "decoder request is null");
  }

  DtypeInfo source_dtype{};
  DtypeInfo dst_dtype{};
  uint64_t row_stride_u64 = 0;
  bool output_planar = false;
  const pixel_error_code validate_ec = validate_decoder_request(
      c, request, &source_dtype, &dst_dtype, &row_stride_u64, &output_planar);
  if (validate_ec != PIXEL_CODEC_ERR_OK) {
    return validate_ec;
  }

  const auto finish_success = [&]() noexcept -> pixel_error_code {
    set_rle_decode_info(request);
    clear_detail(c);
    return PIXEL_CODEC_ERR_OK;
  };

  // Keep the dominant mono RLE case on a compact path inside the generic
  // decoder so host dispatch does not need a separate codec-specific bypass.
  if (request->frame.samples_per_pixel == 1) {
    const pixel_error_code fast_ec =
        decode_single_channel_frame_fast(c, request, source_dtype, dst_dtype, row_stride_u64);
    if (fast_ec != PIXEL_CODEC_ERR_OK) {
      return fast_ec;
    }
    return finish_success();
  }

  std::size_t rows = 0;
  std::size_t cols = 0;
  std::size_t samples = 0;
  std::size_t sample_bytes = 0;
  std::size_t dst_sample_bytes = 0;
  std::size_t row_stride = 0;
  if (!u64_to_size(static_cast<uint64_t>(request->frame.rows), &rows) ||
      !u64_to_size(static_cast<uint64_t>(request->frame.cols), &cols) ||
      !u64_to_size(static_cast<uint64_t>(request->frame.samples_per_pixel), &samples) ||
      !u64_to_size(source_dtype.bytes, &sample_bytes) ||
      !u64_to_size(dst_dtype.bytes, &dst_sample_bytes) ||
      !u64_to_size(row_stride_u64, &row_stride)) {
    return fail_detail(c, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "size conversion overflow");
  }

  std::vector<uint8_t> decoded_planar{};
  std::size_t source_size = 0;
  if (!u64_to_size(request->source.source_buffer.size, &source_size)) {
    return fail_detail(c, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "source buffer size conversion overflow");
  }
  const pixel_error_code decode_ec = decode_rle_frame_to_planar(c,
      request->source.source_buffer.data,
      source_size,
      rows, cols, samples, sample_bytes, &decoded_planar);
  if (decode_ec != PIXEL_CODEC_ERR_OK) {
    return decode_ec;
  }

  uint64_t row_payload_u64 = 0;
  if (!mul_u64(static_cast<uint64_t>(cols), static_cast<uint64_t>(sample_bytes), &row_payload_u64)) {
    return fail_detail(c, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "decode_frame",
        "decoded row payload overflow");
  }
  std::size_t row_payload = 0;
  if (!u64_to_size(row_payload_u64, &row_payload)) {
    return fail_detail(c, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "decode_frame",
        "decoded row payload conversion overflow");
  }
  uint64_t plane_bytes_u64 = 0;
  if (!mul_u64(static_cast<uint64_t>(row_payload), static_cast<uint64_t>(rows), &plane_bytes_u64)) {
    return fail_detail(c, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "decode_frame",
        "decoded plane byte size overflow");
  }
  std::size_t plane_bytes = 0;
  if (!u64_to_size(plane_bytes_u64, &plane_bytes)) {
    return fail_detail(c, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "decode_frame",
        "decoded plane byte size conversion overflow");
  }

  uint8_t* dst = request->output.dst;
  auto sample_ptr_at = [&](std::size_t r, std::size_t c_idx, std::size_t comp) -> const uint8_t* {
    const uint8_t* src_plane = decoded_planar.data() + comp * plane_bytes;
    const uint8_t* src_row = src_plane + r * row_payload;
    return src_row + c_idx * sample_bytes;
  };

  const bool matching_integral_storage =
      ::pixel::codec_common::integral_storage_matches_dst_dtype(
          static_cast<uint32_t>(sample_bytes), source_dtype.is_signed, dst_dtype);
  if (matching_integral_storage &&
      integral_storage_is_already_normalized(sample_bytes, request->frame.bits_stored)) {
    if (output_planar) {
      uint64_t dst_plane_bytes_u64 = 0;
      if (!mul_u64(static_cast<uint64_t>(row_stride), static_cast<uint64_t>(rows),
              &dst_plane_bytes_u64)) {
        return fail_detail(c, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "decode_frame",
            "destination plane byte size overflow");
      }
      std::size_t dst_plane_bytes = 0;
      if (!u64_to_size(dst_plane_bytes_u64, &dst_plane_bytes)) {
        return fail_detail(c, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "decode_frame",
            "destination plane byte size conversion overflow");
      }
      for (std::size_t s = 0; s < samples; ++s) {
        const uint8_t* src_plane = decoded_planar.data() + s * plane_bytes;
        uint8_t* dst_plane = dst + s * dst_plane_bytes;
        for (std::size_t r = 0; r < rows; ++r) {
          const uint8_t* src_row = src_plane + r * row_payload;
          uint8_t* dst_row = dst_plane + r * row_stride;
          std::memcpy(dst_row, src_row, row_payload);
        }
      }
      return finish_success();
    }

    if ((sample_bytes == 1u || sample_bytes == 2u) &&
        (samples == 3u || samples == 4u)) {
      if (sample_bytes == 1u) {
        if (samples == 3u) {
          copy_planar_to_interleaved_fast<1, 3>(
              decoded_planar.data(), dst, rows, cols, row_payload, row_stride);
        } else {
          copy_planar_to_interleaved_fast<1, 4>(
              decoded_planar.data(), dst, rows, cols, row_payload, row_stride);
        }
      } else {
        if (samples == 3u) {
          copy_planar_to_interleaved_fast<2, 3>(
              decoded_planar.data(), dst, rows, cols, row_payload, row_stride);
        } else {
          copy_planar_to_interleaved_fast<2, 4>(
              decoded_planar.data(), dst, rows, cols, row_payload, row_stride);
        }
      }
      return finish_success();
    }

    for (std::size_t r = 0; r < rows; ++r) {
      uint8_t* dst_row = dst + r * row_stride;
      for (std::size_t col = 0; col < cols; ++col) {
        uint8_t* dst_pixel = dst_row + (col * samples) * sample_bytes;
        for (std::size_t s = 0; s < samples; ++s) {
          const uint8_t* src_sample = sample_ptr_at(r, col, s);
          std::memcpy(dst_pixel + s * sample_bytes, src_sample, sample_bytes);
        }
      }
    }

    return finish_success();
  }

  if (matching_integral_storage) {
    if (!write_normalized_matching_storage_to_dst(
            decoded_planar.data(), dst, rows, cols, samples, row_payload,
            plane_bytes, row_stride, output_planar, sample_bytes, source_dtype.is_signed,
            request->frame.bits_stored)) {
      return fail_detail(c, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "decode_frame",
          "unsupported destination dtype");
    }
    return finish_success();
  }

  bool typed_load_failed = false;

  // Keep the generic converted path shared so uncommon storage conversions do
  // not need a separate RLE-only writeback implementation.
  const auto typed_status =
      ::pixel::codec_common::write_loaded_integral_rows(
          request, row_stride_u64, output_planar,
          [&](std::size_t row, std::size_t col, std::size_t comp) -> int32_t {
            int32_t sample = 0;
            const uint8_t* src_ptr = sample_ptr_at(row, col, comp);
            if (!load_integral_sample(src_ptr, static_cast<uint32_t>(sample_bytes),
                    source_dtype.is_signed, request->frame.bits_stored, &sample)) {
              typed_load_failed = true;
              return 0;
            }
            return sample;
          });
  if (typed_load_failed) {
    return fail_detail(c, PIXEL_CODEC_ERR_FAILED, "decode_frame",
        "failed to parse decoded RLE sample");
  }
  if (typed_status == ::pixel::codec_common::loaded_integral_write_status::ok) {
      return finish_success();
  }

  uint64_t dst_plane_bytes_u64 = 0;
  if (output_planar &&
      !mul_u64(static_cast<uint64_t>(row_stride), static_cast<uint64_t>(rows), &dst_plane_bytes_u64)) {
    return fail_detail(c, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "decode_frame",
        "destination plane byte size overflow");
  }
  std::size_t dst_plane_bytes = 0;
  if (output_planar && !u64_to_size(dst_plane_bytes_u64, &dst_plane_bytes)) {
    return fail_detail(c, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "decode_frame",
        "destination plane byte size conversion overflow");
  }

  const std::size_t dst_pixel_stride = samples * dst_sample_bytes;
  for (std::size_t r = 0; r < rows; ++r) {
    uint8_t* dst_row = dst + r * row_stride;
    for (std::size_t col = 0; col < cols; ++col) {
      uint8_t* dst_pixel = dst_row + col * dst_pixel_stride;
      for (std::size_t s = 0; s < samples; ++s) {
        const uint8_t* src_sample = sample_ptr_at(r, col, s);
        int32_t sample_value = 0;
        if (!load_integral_sample(src_sample, static_cast<uint32_t>(sample_bytes),
                source_dtype.is_signed, request->frame.bits_stored, &sample_value)) {
          return fail_detail(c, PIXEL_CODEC_ERR_FAILED, "decode_frame",
              "failed to parse decoded RLE sample");
        }

        uint8_t* dst_sample = output_planar
            ? (dst + s * dst_plane_bytes + r * row_stride + col * dst_sample_bytes)
            : (dst_pixel + s * dst_sample_bytes);
        if (dst_dtype.is_float) {
          if (!write_float_sample(request->output.dst_dtype,
                  static_cast<double>(sample_value), dst_sample)) {
            return fail_detail(c, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "decode_frame",
                "unsupported float destination dtype");
          }
        } else {
          if (!write_integer_sample(request->output.dst_dtype, sample_value, dst_sample)) {
            return fail_detail(c, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "decode_frame",
                "unsupported integer destination dtype");
          }
        }
      }
    }
  }

  return finish_success();
}

}  // namespace pixel::rle_codec
