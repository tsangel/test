#include <cstddef>
#include <cstdint>
#include <cstring>

#include "../common/decode_fastpath.hpp"
#include "support.hpp"

namespace pixel::core {

namespace {

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

pixel_error_code validate_decode_request(ErrorState* state,
    const pixel_decoder_request* request, DtypeInfo* out_source_dtype,
    DtypeInfo* out_dst_dtype, uint64_t* out_row_stride, bool* out_source_planar,
    bool* out_output_planar, uint64_t* out_source_row_bytes,
    uint64_t* out_source_plane_bytes) {
  if (request == nullptr) {
    return fail_detail(state, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "decoder request is null");
  }
  if (request->struct_size < sizeof(pixel_decoder_request) ||
      request->source.struct_size < sizeof(pixel_decoder_source) ||
      request->frame.struct_size < sizeof(pixel_decoder_frame_info) ||
      request->output.struct_size < sizeof(pixel_decoder_output)) {
    return fail_detail(state, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "decoder request struct_size is too small");
  }
  if (!is_uncompressed_profile(request->frame.codec_profile_code)) {
    return fail_detail_u32(state, PIXEL_CODEC_ERR_UNSUPPORTED, "validate",
        "unsupported uncompressed codec_profile_code=%u",
        request->frame.codec_profile_code);
  }
  if (request->source.source_buffer.data == nullptr ||
      request->source.source_buffer.size == 0) {
    return fail_detail(state, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "source_buffer is null or empty");
  }
  if (request->frame.rows <= 0 || request->frame.cols <= 0 ||
      request->frame.samples_per_pixel <= 0) {
    return fail_detail(state, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "rows/cols/samples_per_pixel must be positive");
  }
  if (!is_valid_planar_code(request->frame.source_planar)) {
    return fail_detail(state, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "unsupported source_planar code");
  }
  if (!is_valid_planar_code(request->output.dst_planar)) {
    return fail_detail(state, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "unsupported dst_planar code");
  }

  DtypeInfo source_dtype{};
  DtypeInfo dst_dtype{};
  if (!dtype_info_from_code(request->frame.source_dtype, &source_dtype)) {
    return fail_detail(state, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "unsupported source_dtype code");
  }
  if (!dtype_info_from_code(request->output.dst_dtype, &dst_dtype)) {
    return fail_detail(state, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "unsupported dst_dtype code");
  }
  if (request->frame.bits_stored <= 0 ||
      request->frame.bits_stored > static_cast<int32_t>(source_dtype.bytes * 8u)) {
    return fail_detail(state, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "bits_stored must be in [1, source dtype width]");
  }
  if (request->output.dst == nullptr) {
    return fail_detail(state, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "output dst is null");
  }

  const uint64_t rows = static_cast<uint64_t>(request->frame.rows);
  const uint64_t cols = static_cast<uint64_t>(request->frame.cols);
  const uint64_t samples = static_cast<uint64_t>(request->frame.samples_per_pixel);
  const bool source_planar = is_planar_code(request->frame.source_planar);
  const bool output_planar = is_planar_code(request->output.dst_planar);

  const uint64_t source_row_components = source_planar ? 1 : samples;
  const uint64_t output_row_components = output_planar ? 1 : samples;

  uint64_t source_row_bytes = 0;
  if (!mul_u64(cols, source_row_components, &source_row_bytes) ||
      !mul_u64(source_row_bytes, source_dtype.bytes, &source_row_bytes)) {
    return fail_detail(state, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "source row byte size overflow");
  }

  uint64_t source_plane_bytes = 0;
  if (!mul_u64(source_row_bytes, rows, &source_plane_bytes)) {
    return fail_detail(state, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "source plane byte size overflow");
  }

  uint64_t source_frame_bytes = source_plane_bytes;
  if (source_planar && samples > 1) {
    if (!mul_u64(source_plane_bytes, samples, &source_frame_bytes)) {
      return fail_detail(state, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
          "source frame byte size overflow");
    }
  }
  if (request->source.source_buffer.size < source_frame_bytes) {
    return fail_detail(state, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "source buffer is shorter than expected frame size");
  }

  uint64_t min_row_bytes = 0;
  if (!mul_u64(cols, output_row_components, &min_row_bytes) ||
      !mul_u64(min_row_bytes, dst_dtype.bytes, &min_row_bytes)) {
    return fail_detail(state, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "destination row byte size overflow");
  }

  uint64_t row_stride = request->output.row_stride;
  if (row_stride == 0) {
    row_stride = min_row_bytes;
  }
  if (row_stride < min_row_bytes) {
    return fail_detail(state, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "row_stride is too small");
  }

  uint64_t plane_bytes = 0;
  if (!mul_u64(row_stride, rows, &plane_bytes)) {
    return fail_detail(state, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "destination plane byte size overflow");
  }

  uint64_t min_frame_bytes = plane_bytes;
  if (output_planar && samples > 1) {
    if (!mul_u64(plane_bytes, samples, &min_frame_bytes)) {
      return fail_detail(state, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
          "destination frame byte size overflow");
    }
  }

  uint64_t frame_stride = request->output.frame_stride;
  if (frame_stride == 0) {
    frame_stride = min_frame_bytes;
  }
  if (frame_stride < min_frame_bytes) {
    return fail_detail(state, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "frame_stride is too small");
  }
  if (request->output.dst_size < frame_stride) {
    return fail_detail(state, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "destination buffer is too small");
  }

  *out_source_dtype = source_dtype;
  *out_dst_dtype = dst_dtype;
  *out_row_stride = row_stride;
  *out_source_planar = source_planar;
  *out_output_planar = output_planar;
  *out_source_row_bytes = source_row_bytes;
  *out_source_plane_bytes = source_plane_bytes;
  return PIXEL_CODEC_ERR_OK;
}

}  // namespace

pixel_error_code decode_uncompressed_frame(
    ErrorState* state, const pixel_decoder_request* request) {
  DtypeInfo source_dtype{};
  DtypeInfo dst_dtype{};
  uint64_t row_stride_u64 = 0;
  bool source_planar = false;
  bool output_planar = false;
  uint64_t source_row_bytes_u64 = 0;
  uint64_t source_plane_bytes_u64 = 0;
  const pixel_error_code validate_ec = validate_decode_request(state, request,
      &source_dtype, &dst_dtype, &row_stride_u64, &source_planar, &output_planar,
      &source_row_bytes_u64, &source_plane_bytes_u64);
  if (validate_ec != PIXEL_CODEC_ERR_OK) {
    return validate_ec;
  }

  std::size_t rows = 0;
  std::size_t cols = 0;
  std::size_t samples = 0;
  std::size_t sample_bytes = 0;
  std::size_t dst_sample_bytes = 0;
  std::size_t row_stride = 0;
  std::size_t source_row_bytes = 0;
  std::size_t source_plane_bytes = 0;
  if (!u64_to_size(static_cast<uint64_t>(request->frame.rows), &rows) ||
      !u64_to_size(static_cast<uint64_t>(request->frame.cols), &cols) ||
      !u64_to_size(static_cast<uint64_t>(request->frame.samples_per_pixel), &samples) ||
      !u64_to_size(source_dtype.bytes, &sample_bytes) ||
      !u64_to_size(dst_dtype.bytes, &dst_sample_bytes) ||
      !u64_to_size(row_stride_u64, &row_stride) ||
      !u64_to_size(source_row_bytes_u64, &source_row_bytes) ||
      !u64_to_size(source_plane_bytes_u64, &source_plane_bytes)) {
    return fail_detail(state, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "size conversion overflow");
  }

  const uint8_t* src = request->source.source_buffer.data;
  uint8_t* dst = request->output.dst;
  const bool matching_integral_storage =
      ::pixel::codec_common::integral_storage_matches_dst_dtype(
          static_cast<uint32_t>(sample_bytes), source_dtype.is_signed, dst_dtype);

  if (samples == 1 && matching_integral_storage) {
    // Single-channel planar and interleaved layouts are byte-identical, so a
    // row copy is still the cheapest path when storage already matches.
    if (row_stride == source_row_bytes) {
      uint64_t copy_bytes_u64 = 0;
      std::size_t copy_bytes = 0;
      if (!mul_u64(static_cast<uint64_t>(source_row_bytes), static_cast<uint64_t>(rows),
              &copy_bytes_u64) ||
          !u64_to_size(copy_bytes_u64, &copy_bytes)) {
        return fail_detail(state, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
            "single-channel copy byte size overflow");
      }
      std::memcpy(dst, src, copy_bytes);
      clear_error(state);
      return PIXEL_CODEC_ERR_OK;
    }

    for (std::size_t r = 0; r < rows; ++r) {
      const uint8_t* src_row = src + r * source_row_bytes;
      uint8_t* dst_row = dst + r * row_stride;
      std::memcpy(dst_row, src_row, source_row_bytes);
    }
    clear_error(state);
    return PIXEL_CODEC_ERR_OK;
  }

  if (source_planar == output_planar && matching_integral_storage) {
    if (!source_planar) {
      if (row_stride == source_row_bytes) {
        uint64_t copy_bytes_u64 = 0;
        std::size_t copy_bytes = 0;
        if (!mul_u64(static_cast<uint64_t>(source_row_bytes), static_cast<uint64_t>(rows),
                &copy_bytes_u64) ||
            !u64_to_size(copy_bytes_u64, &copy_bytes)) {
          return fail_detail(state, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
              "interleaved copy byte size overflow");
        }
        std::memcpy(dst, src, copy_bytes);
        clear_error(state);
        return PIXEL_CODEC_ERR_OK;
      }

      for (std::size_t r = 0; r < rows; ++r) {
        const uint8_t* src_row = src + r * source_row_bytes;
        uint8_t* dst_row = dst + r * row_stride;
        std::memcpy(dst_row, src_row, source_row_bytes);
      }
      clear_error(state);
      return PIXEL_CODEC_ERR_OK;
    }

    uint64_t row_payload_u64 = 0;
    uint64_t dst_plane_bytes_u64 = 0;
    std::size_t row_payload = 0;
    std::size_t dst_plane_bytes = 0;
    if (!mul_u64(static_cast<uint64_t>(cols), static_cast<uint64_t>(sample_bytes),
            &row_payload_u64) ||
        !mul_u64(static_cast<uint64_t>(row_stride), static_cast<uint64_t>(rows),
            &dst_plane_bytes_u64) ||
        !u64_to_size(row_payload_u64, &row_payload) ||
        !u64_to_size(dst_plane_bytes_u64, &dst_plane_bytes)) {
      return fail_detail(state, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
          "size conversion overflow");
    }
    for (std::size_t s = 0; s < samples; ++s) {
      const uint8_t* src_plane = src + s * source_plane_bytes;
      uint8_t* dst_plane = dst + s * dst_plane_bytes;
      for (std::size_t r = 0; r < rows; ++r) {
        const uint8_t* src_row = src_plane + r * row_payload;
        uint8_t* dst_row = dst_plane + r * row_stride;
        std::memcpy(dst_row, src_row, row_payload);
      }
    }
    clear_error(state);
    return PIXEL_CODEC_ERR_OK;
  }

  auto sample_ptr_at = [&](std::size_t r, std::size_t c, std::size_t comp) -> const uint8_t* {
    if (source_planar) {
      return src + comp * source_plane_bytes + r * source_row_bytes + c * sample_bytes;
    }
    const std::size_t pixel_stride = samples * sample_bytes;
    return src + r * source_row_bytes + c * pixel_stride + comp * sample_bytes;
  };

  if (matching_integral_storage) {
    bool typed_load_failed = false;

    // Keep layout conversion on a typed raw path so interleave/deinterleave
    // does not need separate memcpy-specialized branches for every dtype.
    const auto typed_status =
        ::pixel::codec_common::write_loaded_integral_rows(
            request, row_stride_u64, output_planar,
            [&](std::size_t row, std::size_t col, std::size_t comp) -> int32_t {
              int32_t sample = 0;
              if (!load_integral_sample(sample_ptr_at(row, col, comp),
                      static_cast<uint32_t>(sample_bytes),
                      source_dtype.is_signed, request->frame.bits_stored, &sample)) {
                typed_load_failed = true;
                return 0;
              }
              return sample;
            });
    if (typed_load_failed) {
      return fail_detail(state, PIXEL_CODEC_ERR_FAILED, "decode_frame",
          "failed to parse source sample");
    }
    if (typed_status == ::pixel::codec_common::loaded_integral_write_status::ok) {
      clear_error(state);
      return PIXEL_CODEC_ERR_OK;
    }
  }

  uint64_t dst_plane_bytes_u64 = 0;
  if (output_planar &&
      !mul_u64(static_cast<uint64_t>(row_stride), static_cast<uint64_t>(rows), &dst_plane_bytes_u64)) {
    return fail_detail(state, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "destination plane byte size overflow");
  }
  std::size_t dst_plane_bytes = 0;
  if (output_planar && !u64_to_size(dst_plane_bytes_u64, &dst_plane_bytes)) {
    return fail_detail(state, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "destination plane byte size conversion overflow");
  }

  if (output_planar && samples > 1) {
    for (std::size_t comp = 0; comp < samples; ++comp) {
      uint8_t* dst_plane = dst + comp * dst_plane_bytes;
      for (std::size_t r = 0; r < rows; ++r) {
        uint8_t* dst_row = dst_plane + r * row_stride;
        for (std::size_t c = 0; c < cols; ++c) {
          int32_t sample = 0;
          if (!load_integral_sample(sample_ptr_at(r, c, comp),
                  static_cast<uint32_t>(sample_bytes),
                  source_dtype.is_signed, request->frame.bits_stored, &sample)) {
            return fail_detail(state, PIXEL_CODEC_ERR_FAILED, "decode_frame",
                "failed to parse source sample");
          }

          uint8_t* dst_ptr = dst_row + c * dst_sample_bytes;
          if (dst_dtype.is_float) {
            if (!write_float_sample(request->output.dst_dtype,
                    static_cast<double>(sample), dst_ptr)) {
              return fail_detail(state, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "decode_frame",
                  "unsupported float destination dtype");
            }
          } else {
            if (!write_integer_sample(request->output.dst_dtype, sample, dst_ptr)) {
              return fail_detail(state, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "decode_frame",
                  "unsupported integer destination dtype");
            }
          }
        }
      }
    }
    clear_error(state);
    return PIXEL_CODEC_ERR_OK;
  }

  const std::size_t dst_pixel_stride = samples * dst_sample_bytes;
  for (std::size_t r = 0; r < rows; ++r) {
    uint8_t* dst_row = dst + r * row_stride;
    for (std::size_t c = 0; c < cols; ++c) {
      uint8_t* dst_pixel = dst_row + c * dst_pixel_stride;
      for (std::size_t comp = 0; comp < samples; ++comp) {
        int32_t sample = 0;
        if (!load_integral_sample(sample_ptr_at(r, c, comp),
                static_cast<uint32_t>(sample_bytes),
                source_dtype.is_signed, request->frame.bits_stored, &sample)) {
          return fail_detail(state, PIXEL_CODEC_ERR_FAILED, "decode_frame",
              "failed to parse source sample");
        }

        uint8_t* dst_ptr = dst_pixel + comp * dst_sample_bytes;
        if (dst_dtype.is_float) {
          if (!write_float_sample(request->output.dst_dtype,
                  static_cast<double>(sample), dst_ptr)) {
            return fail_detail(state, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "decode_frame",
                "unsupported float destination dtype");
          }
        } else {
          if (!write_integer_sample(request->output.dst_dtype, sample, dst_ptr)) {
            return fail_detail(state, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "decode_frame",
                "unsupported integer destination dtype");
          }
        }
      }
    }
  }

  clear_error(state);
  return PIXEL_CODEC_ERR_OK;
}

}  // namespace pixel::core
