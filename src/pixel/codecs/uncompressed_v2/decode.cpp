#include <cstddef>
#include <cstdint>
#include <cmath>
#include <cstring>

#include "support.hpp"

namespace pixel::core_v2 {

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

bool write_float_sample(uint8_t dst_dtype, double sample, uint8_t* dst) {
  if (dst == nullptr) {
    return false;
  }
  switch (dst_dtype) {
  case PIXEL_DTYPE_F32_V2: {
    const float value = static_cast<float>(sample);
    std::memcpy(dst, &value, sizeof(value));
    return true;
  }
  case PIXEL_DTYPE_F64_V2:
    std::memcpy(dst, &sample, sizeof(sample));
    return true;
  default:
    return false;
  }
}

bool apply_value_transform(const pixel_decoder_request_v2* request, int32_t sample_value,
    double* out_value) {
  if (request == nullptr || out_value == nullptr) {
    return false;
  }

  const uint32_t transform_kind = request->value_transform.transform_kind;
  if (transform_kind == PIXEL_DECODER_VALUE_TRANSFORM_NONE_V2) {
    *out_value = static_cast<double>(sample_value);
    return true;
  }
  if (transform_kind == PIXEL_DECODER_VALUE_TRANSFORM_RESCALE_V2) {
    const double slope = request->value_transform.rescale_slope;
    const double intercept = request->value_transform.rescale_intercept;
    *out_value = static_cast<double>(sample_value) * slope + intercept;
    return true;
  }
  if (transform_kind == PIXEL_DECODER_VALUE_TRANSFORM_MODALITY_LUT_V2) {
    const int64_t first_mapped = request->value_transform.lut_first_mapped;
    const uint64_t count = request->value_transform.lut_value_count;
    if (count == 0) {
      *out_value = 0.0;
      return true;
    }
    int64_t idx = static_cast<int64_t>(sample_value) - first_mapped;
    if (idx < 0) {
      idx = 0;
    }
    const int64_t max_idx = static_cast<int64_t>(count - 1);
    if (idx > max_idx) {
      idx = max_idx;
    }
    float lut_value = 0.0f;
    const uint8_t* lut_data = request->value_transform.lut_values_f32.data;
    std::memcpy(&lut_value, lut_data + static_cast<std::size_t>(idx) * sizeof(float),
        sizeof(float));
    *out_value = static_cast<double>(lut_value);
    return true;
  }
  return false;
}

pixel_error_code_v2 validate_decode_request(ErrorState* state,
    const pixel_decoder_request_v2* request, DtypeInfo* out_source_dtype,
    DtypeInfo* out_dst_dtype, uint64_t* out_row_stride, bool* out_source_planar,
    bool* out_output_planar, uint32_t* out_transform_kind,
    uint64_t* out_source_row_bytes, uint64_t* out_source_plane_bytes) {
  if (request == nullptr) {
    return fail_detail(state, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "decoder request is null");
  }
  if (request->struct_size < sizeof(pixel_decoder_request_v2) ||
      request->source.struct_size < sizeof(pixel_decoder_source_v2) ||
      request->frame.struct_size < sizeof(pixel_decoder_frame_info_v2) ||
      request->output.struct_size < sizeof(pixel_decoder_output_v2) ||
      request->value_transform.struct_size < sizeof(pixel_decoder_value_transform_v2)) {
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

  const uint32_t transform_kind = request->value_transform.transform_kind;
  if (transform_kind != PIXEL_DECODER_VALUE_TRANSFORM_NONE_V2) {
    if (request->frame.samples_per_pixel != 1) {
      return fail_detail(state, PIXEL_CODEC_ERR_UNSUPPORTED, "validate",
          "value transform requires samples_per_pixel=1");
    }
    if (source_dtype.is_float || source_dtype.bytes == 0 || source_dtype.bytes > 4u) {
      return fail_detail(state, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
          "value transform requires integral source dtype");
    }
    if (!dst_dtype.is_float) {
      return fail_detail(state, PIXEL_CODEC_ERR_UNSUPPORTED, "validate",
          "value transform requires float destination dtype");
    }
    if (transform_kind == PIXEL_DECODER_VALUE_TRANSFORM_MODALITY_LUT_V2) {
      const uint64_t lut_values_bytes = request->value_transform.lut_values_f32.size;
      const uint64_t lut_count = request->value_transform.lut_value_count;
      uint64_t required_bytes = 0;
      if (!mul_u64(lut_count, sizeof(float), &required_bytes) ||
          lut_values_bytes < required_bytes ||
          request->value_transform.lut_values_f32.data == nullptr) {
        return fail_detail(state, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
            "invalid modality LUT buffer");
      }
    } else if (transform_kind == PIXEL_DECODER_VALUE_TRANSFORM_RESCALE_V2) {
      if (!std::isfinite(request->value_transform.rescale_slope) ||
          !std::isfinite(request->value_transform.rescale_intercept)) {
        return fail_detail(state, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
            "rescale slope/intercept must be finite");
      }
    } else {
      return fail_detail(state, PIXEL_CODEC_ERR_UNSUPPORTED, "validate",
          "unsupported value transform kind");
    }
  } else if (request->frame.source_dtype != request->output.dst_dtype) {
    return fail_detail(state, PIXEL_CODEC_ERR_UNSUPPORTED, "validate",
        "source_dtype must match dst_dtype without value transform");
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
  *out_transform_kind = transform_kind;
  *out_source_row_bytes = source_row_bytes;
  *out_source_plane_bytes = source_plane_bytes;
  return PIXEL_CODEC_ERR_OK;
}

}  // namespace

pixel_error_code_v2 decode_uncompressed_frame(
    ErrorState* state, const pixel_decoder_request_v2* request) {
  DtypeInfo source_dtype{};
  DtypeInfo dst_dtype{};
  uint64_t row_stride_u64 = 0;
  bool source_planar = false;
  bool output_planar = false;
  uint32_t transform_kind = PIXEL_DECODER_VALUE_TRANSFORM_NONE_V2;
  uint64_t source_row_bytes_u64 = 0;
  uint64_t source_plane_bytes_u64 = 0;
  const pixel_error_code_v2 validate_ec = validate_decode_request(state, request,
      &source_dtype, &dst_dtype, &row_stride_u64, &source_planar, &output_planar,
      &transform_kind,
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

  if (transform_kind != PIXEL_DECODER_VALUE_TRANSFORM_NONE_V2) {
    for (std::size_t r = 0; r < rows; ++r) {
      const uint8_t* src_row = src + r * source_row_bytes;
      uint8_t* dst_row = dst + r * row_stride;
      for (std::size_t c = 0; c < cols; ++c) {
        const uint8_t* src_sample = src_row + c * sample_bytes;
        int32_t sample_value = 0;
        if (!load_integral_sample(src_sample, static_cast<uint32_t>(sample_bytes),
                source_dtype.is_signed, request->frame.bits_stored, &sample_value)) {
          return fail_detail(state, PIXEL_CODEC_ERR_FAILED, "decode_frame",
              "failed to parse source sample");
        }

        double transformed = 0.0;
        if (!apply_value_transform(request, sample_value, &transformed)) {
          return fail_detail(state, PIXEL_CODEC_ERR_UNSUPPORTED, "decode_frame",
              "unsupported value transform kind");
        }

        uint8_t* dst_sample = dst_row + c * dst_sample_bytes;
        if (!write_float_sample(request->output.dst_dtype, transformed, dst_sample)) {
          return fail_detail(state, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "decode_frame",
              "value transform requires float destination dtype");
        }
      }
    }
    clear_error(state);
    return PIXEL_CODEC_ERR_OK;
  }

  if (source_planar == output_planar) {
    if (!source_planar) {
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
    if (!mul_u64(static_cast<uint64_t>(cols), static_cast<uint64_t>(sample_bytes), &row_payload_u64) ||
        !mul_u64(static_cast<uint64_t>(row_stride), static_cast<uint64_t>(rows), &dst_plane_bytes_u64) ||
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

  if (!source_planar && output_planar) {
    uint64_t dst_plane_bytes_u64 = 0;
    if (!mul_u64(static_cast<uint64_t>(row_stride), static_cast<uint64_t>(rows), &dst_plane_bytes_u64)) {
      return fail_detail(state, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
          "destination plane byte size overflow");
    }
    std::size_t dst_plane_bytes = 0;
    if (!u64_to_size(dst_plane_bytes_u64, &dst_plane_bytes)) {
      return fail_detail(state, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
          "destination plane byte size conversion overflow");
    }
    for (std::size_t s = 0; s < samples; ++s) {
      uint8_t* dst_plane = dst + s * dst_plane_bytes;
      for (std::size_t r = 0; r < rows; ++r) {
        const uint8_t* src_row = src + r * source_row_bytes;
        uint8_t* dst_row = dst_plane + r * row_stride;
        for (std::size_t c = 0; c < cols; ++c) {
          const uint8_t* src_sample = src_row + (c * samples + s) * sample_bytes;
          uint8_t* dst_sample = dst_row + c * sample_bytes;
          std::memcpy(dst_sample, src_sample, sample_bytes);
        }
      }
    }
    clear_error(state);
    return PIXEL_CODEC_ERR_OK;
  }

  for (std::size_t r = 0; r < rows; ++r) {
    uint8_t* dst_row = dst + r * row_stride;
    for (std::size_t c = 0; c < cols; ++c) {
      uint8_t* dst_pixel = dst_row + (c * samples) * sample_bytes;
      for (std::size_t s = 0; s < samples; ++s) {
        const uint8_t* src_plane = src + s * source_plane_bytes;
        const uint8_t* src_row = src_plane + r * source_row_bytes;
        const uint8_t* src_sample = src_row + c * sample_bytes;
        std::memcpy(dst_pixel + s * sample_bytes, src_sample, sample_bytes);
      }
    }
  }

  clear_error(state);
  return PIXEL_CODEC_ERR_OK;
}

}  // namespace pixel::core_v2
