#include <charls/charls.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <string>
#include <vector>

#include "internal.hpp"

namespace pixel::jpegls_plugin_v2 {

namespace {

pixel_error_code_v2 validate_decoder_request(
    DecoderCtx* ctx, const pixel_decoder_request_v2* request,
    DtypeInfo* out_source_dtype, DtypeInfo* out_dst_dtype,
    uint64_t* out_row_stride, uint64_t* out_frame_stride,
    bool* out_output_planar, uint32_t* out_transform_kind) {
  if (request->struct_size < sizeof(pixel_decoder_request_v2) ||
      request->source.struct_size < sizeof(pixel_decoder_source_v2) ||
      request->frame.struct_size < sizeof(pixel_decoder_frame_info_v2) ||
      request->output.struct_size < sizeof(pixel_decoder_output_v2) ||
      request->value_transform.struct_size < sizeof(pixel_decoder_value_transform_v2)) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "decoder request struct_size is too small");
  }

  if (request->frame.codec_profile_code != ctx->codec_profile_code) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "decoder request codec_profile_code does not match configured profile");
  }

  if (!is_supported_decoder_profile(request->frame.codec_profile_code)) {
    return fail_detail_u32(ctx, PIXEL_CODEC_ERR_UNSUPPORTED, "validate",
        "unsupported decoder codec_profile_code=%u", request->frame.codec_profile_code);
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

  if (request->frame.samples_per_pixel != 1 &&
      request->frame.samples_per_pixel != 3 &&
      request->frame.samples_per_pixel != 4) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_UNSUPPORTED, "validate",
        "samples_per_pixel must be 1, 3, or 4");
  }

  DtypeInfo source_dtype{};
  if (!dtype_info_from_code(request->frame.source_dtype, &source_dtype) ||
      source_dtype.is_float || source_dtype.bytes == 0 || source_dtype.bytes > 2) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "source_dtype must be integral 8/16-bit type");
  }

  if (request->frame.bits_stored <= 0 ||
      request->frame.bits_stored > static_cast<int32_t>(source_dtype.bytes * 8u)) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "bits_stored must be in [1, source_dtype width]");
  }

  if (request->output.dst == nullptr) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "output dst is null");
  }

  if (!is_valid_planar_code(request->output.dst_planar)) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "unsupported dst_planar code");
  }

  DtypeInfo dst_dtype{};
  if (!dtype_info_from_code(request->output.dst_dtype, &dst_dtype)) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "unsupported dst_dtype code");
  }

  const uint32_t transform_kind = request->value_transform.transform_kind;
  if (transform_kind != PIXEL_DECODER_VALUE_TRANSFORM_NONE_V2) {
    if (request->frame.samples_per_pixel != 1) {
      return fail_detail(ctx, PIXEL_CODEC_ERR_UNSUPPORTED, "validate",
          "value transform requires samples_per_pixel=1");
    }
    if (!dst_dtype.is_float) {
      return fail_detail(ctx, PIXEL_CODEC_ERR_UNSUPPORTED, "validate",
          "value transform requires float destination dtype");
    }
    if (transform_kind == PIXEL_DECODER_VALUE_TRANSFORM_MODALITY_LUT_V2) {
      const uint64_t lut_values_bytes = request->value_transform.lut_values_f32.size;
      const uint64_t lut_count = request->value_transform.lut_value_count;
      uint64_t required_bytes = 0;
      if (!mul_u64(lut_count, sizeof(float), &required_bytes) ||
          lut_values_bytes < required_bytes ||
          request->value_transform.lut_values_f32.data == nullptr) {
        return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
            "invalid modality LUT buffer");
      }
    } else if (transform_kind == PIXEL_DECODER_VALUE_TRANSFORM_RESCALE_V2) {
      if (!std::isfinite(request->value_transform.rescale_slope) ||
          !std::isfinite(request->value_transform.rescale_intercept)) {
        return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
            "rescale slope/intercept must be finite");
      }
    } else {
      return fail_detail(ctx, PIXEL_CODEC_ERR_UNSUPPORTED, "validate",
          "unsupported value transform kind");
    }
  }

  const uint64_t rows = static_cast<uint64_t>(request->frame.rows);
  const uint64_t cols = static_cast<uint64_t>(request->frame.cols);
  const uint64_t samples = static_cast<uint64_t>(request->frame.samples_per_pixel);
  const bool output_planar = is_planar_code(request->output.dst_planar);
  const uint64_t row_components = output_planar ? 1 : samples;

  uint64_t min_row_bytes = 0;
  if (!mul_u64(cols, row_components, &min_row_bytes) ||
      !mul_u64(min_row_bytes, dst_dtype.bytes, &min_row_bytes)) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "output row byte size overflow");
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
        "output plane byte size overflow");
  }

  uint64_t min_frame_bytes = plane_bytes;
  if (output_planar && samples > 1) {
    if (!mul_u64(plane_bytes, samples, &min_frame_bytes)) {
      return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
          "output frame byte size overflow");
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
  *out_frame_stride = frame_stride;
  *out_output_planar = output_planar;
  *out_transform_kind = transform_kind;
  return PIXEL_CODEC_ERR_OK;
}

pixel_error_code_v2 write_decoded_pixels(DecoderCtx* ctx,
    const pixel_decoder_request_v2* request,
    const uint8_t* decoded,
    uint32_t source_sample_bytes,
    int source_bits,
    bool source_is_signed,
    bool source_is_planar,
    uint64_t source_row_bytes,
    DtypeInfo dst_dtype,
    uint64_t row_stride,
    bool output_planar,
    uint32_t transform_kind) {
  if (decoded == nullptr) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_FAILED, "decode_frame",
        "decoded source buffer is null");
  }

  const uint64_t rows = static_cast<uint64_t>(request->frame.rows);
  const uint64_t cols = static_cast<uint64_t>(request->frame.cols);
  const uint64_t samples = static_cast<uint64_t>(request->frame.samples_per_pixel);

  const std::size_t rows_sz = static_cast<std::size_t>(rows);
  const std::size_t cols_sz = static_cast<std::size_t>(cols);
  const std::size_t samples_sz = static_cast<std::size_t>(samples);
  const std::size_t source_sample_bytes_sz = static_cast<std::size_t>(source_sample_bytes);
  const std::size_t source_row_bytes_sz = static_cast<std::size_t>(source_row_bytes);
  const std::size_t dst_sample_bytes = static_cast<std::size_t>(dst_dtype.bytes);
  const std::size_t row_stride_sz = static_cast<std::size_t>(row_stride);
  const std::size_t source_plane_bytes_sz = source_row_bytes_sz * rows_sz;

  auto apply_transform = [&](int32_t sv, double* out_value) -> bool {
    if (out_value == nullptr) {
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
      std::memcpy(&lut_value, lut_data + static_cast<std::size_t>(idx) * sizeof(float),
          sizeof(float));
      *out_value = static_cast<double>(lut_value);
      return true;
    }
    return false;
  };

  auto sample_ptr_at = [&](std::size_t r, std::size_t c, std::size_t comp) -> const uint8_t* {
    if (source_is_planar) {
      return decoded + comp * source_plane_bytes_sz +
          r * source_row_bytes_sz + c * source_sample_bytes_sz;
    }
    const std::size_t source_pixel_stride = samples_sz * source_sample_bytes_sz;
    return decoded + r * source_row_bytes_sz +
        c * source_pixel_stride + comp * source_sample_bytes_sz;
  };

  const bool transformed = transform_kind != PIXEL_DECODER_VALUE_TRANSFORM_NONE_V2;

  if (!transformed) {
    if (output_planar && samples_sz > 1) {
      const std::size_t output_plane_bytes_sz = row_stride_sz * rows_sz;
      for (std::size_t comp = 0; comp < samples_sz; ++comp) {
        uint8_t* dst_plane = request->output.dst + comp * output_plane_bytes_sz;
        for (std::size_t r = 0; r < rows_sz; ++r) {
          uint8_t* dst_row = dst_plane + r * row_stride_sz;
          for (std::size_t c = 0; c < cols_sz; ++c) {
            const uint8_t* src_ptr = sample_ptr_at(r, c, comp);
            char reason[256];
            int32_t sample = 0;
            if (!load_integral_sample(src_ptr, source_sample_bytes,
                    source_is_signed, source_bits, &sample, reason, sizeof(reason))) {
              return fail_detail(ctx, PIXEL_CODEC_ERR_FAILED, "decode_frame", reason);
            }
            uint8_t* dst_ptr = dst_row + c * dst_sample_bytes;
            if (dst_dtype.is_float) {
              if (!write_float_sample(request->output.dst_dtype,
                      static_cast<double>(sample), dst_ptr)) {
                return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "decode_frame",
                    "unsupported float destination dtype");
              }
            } else {
              if (!write_integer_sample(request->output.dst_dtype, sample, dst_ptr)) {
                return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "decode_frame",
                    "unsupported integer destination dtype");
              }
            }
          }
        }
      }
      return PIXEL_CODEC_ERR_OK;
    }

    const std::size_t dst_pixel_stride = samples_sz * dst_sample_bytes;
    for (std::size_t r = 0; r < rows_sz; ++r) {
      uint8_t* dst_row = request->output.dst + r * row_stride_sz;
      for (std::size_t c = 0; c < cols_sz; ++c) {
        uint8_t* dst_pixel = dst_row + c * dst_pixel_stride;
        for (std::size_t comp = 0; comp < samples_sz; ++comp) {
          const uint8_t* src_ptr = sample_ptr_at(r, c, comp);
          char reason[256];
          int32_t sample = 0;
          if (!load_integral_sample(src_ptr, source_sample_bytes,
                  source_is_signed, source_bits, &sample, reason, sizeof(reason))) {
            return fail_detail(ctx, PIXEL_CODEC_ERR_FAILED, "decode_frame", reason);
          }
          uint8_t* dst_ptr = dst_pixel + comp * dst_sample_bytes;
          if (dst_dtype.is_float) {
            if (!write_float_sample(request->output.dst_dtype,
                    static_cast<double>(sample), dst_ptr)) {
              return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "decode_frame",
                  "unsupported float destination dtype");
            }
          } else {
            if (!write_integer_sample(request->output.dst_dtype, sample, dst_ptr)) {
              return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "decode_frame",
                  "unsupported integer destination dtype");
            }
          }
        }
      }
    }
    return PIXEL_CODEC_ERR_OK;
  }

  for (std::size_t r = 0; r < rows_sz; ++r) {
    uint8_t* dst_row = request->output.dst + r * row_stride_sz;
    for (std::size_t c = 0; c < cols_sz; ++c) {
      const uint8_t* src_ptr = sample_ptr_at(r, c, 0);
      char reason[256];
      int32_t sample = 0;
      if (!load_integral_sample(src_ptr, source_sample_bytes,
              source_is_signed, source_bits, &sample, reason, sizeof(reason))) {
        return fail_detail(ctx, PIXEL_CODEC_ERR_FAILED, "decode_frame", reason);
      }

      double value = 0.0;
      if (!apply_transform(sample, &value)) {
        return fail_detail(ctx, PIXEL_CODEC_ERR_UNSUPPORTED, "decode_frame",
            "unsupported value transform kind");
      }

      uint8_t* dst_ptr = dst_row + c * dst_sample_bytes;
      if (!write_float_sample(request->output.dst_dtype, value, dst_ptr)) {
        return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "decode_frame",
            "value transform requires float destination dtype");
      }
    }
  }

  return PIXEL_CODEC_ERR_OK;
}

}  // namespace

pixel_error_code_v2 decoder_decode_frame(
    void* ctx, const pixel_decoder_request_v2* request) {
  auto* c = static_cast<DecoderCtx*>(ctx);
  if (c == nullptr || request == nullptr) {
    return fail_detail(c, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "invalid decoder request");
  }
  if (!c->configured) {
    return fail_detail(c, PIXEL_CODEC_ERR_FAILED, "decode_frame",
        "configure must be called before decode_frame");
  }

  try {
    DtypeInfo source_dtype{};
    DtypeInfo dst_dtype{};
    uint64_t row_stride = 0;
    uint64_t frame_stride = 0;
    bool output_planar = false;
    uint32_t transform_kind = PIXEL_DECODER_VALUE_TRANSFORM_NONE_V2;

    const pixel_error_code_v2 valid_ec = validate_decoder_request(c, request,
        &source_dtype, &dst_dtype,
        &row_stride, &frame_stride,
        &output_planar, &transform_kind);
    if (valid_ec != PIXEL_CODEC_ERR_OK) {
      return valid_ec;
    }

    charls::jpegls_decoder decoder{};
    decoder.source(request->source.source_buffer.data,
        static_cast<std::size_t>(request->source.source_buffer.size));
    decoder.read_header();

    const charls::frame_info frame_info = decoder.frame_info();
    const charls::interleave_mode interleave_mode = decoder.interleave_mode();

    const uint64_t rows = static_cast<uint64_t>(request->frame.rows);
    const uint64_t cols = static_cast<uint64_t>(request->frame.cols);
    const uint64_t samples = static_cast<uint64_t>(request->frame.samples_per_pixel);

    if (frame_info.height != rows || frame_info.width != cols) {
      return fail_detail(c, PIXEL_CODEC_ERR_FAILED, "decode_frame",
          "decoded dimensions mismatch");
    }

    if (frame_info.component_count != static_cast<int32_t>(samples)) {
      return fail_detail(c, PIXEL_CODEC_ERR_FAILED, "decode_frame",
          "decoded component count mismatch");
    }

    if (frame_info.bits_per_sample <= 0 || frame_info.bits_per_sample > 16) {
      return fail_detail(c, PIXEL_CODEC_ERR_FAILED, "decode_frame",
          "decoded bits_per_sample is outside [1,16]");
    }

    const uint32_t decoded_sample_bytes =
        static_cast<uint32_t>((frame_info.bits_per_sample + 7) / 8);
    if (decoded_sample_bytes == 0 || decoded_sample_bytes > 2) {
      return fail_detail(c, PIXEL_CODEC_ERR_FAILED, "decode_frame",
          "decoded sample width is unsupported");
    }

    if (decoded_sample_bytes > source_dtype.bytes) {
      return fail_detail(c, PIXEL_CODEC_ERR_FAILED, "decode_frame",
          "decoded precision exceeds source_dtype width");
    }

    if (request->frame.bits_stored > 0 &&
        frame_info.bits_per_sample > request->frame.bits_stored) {
      const int decoded_bytes = (frame_info.bits_per_sample + 7) / 8;
      const int metadata_bytes = (request->frame.bits_stored + 7) / 8;
      if (decoded_bytes > metadata_bytes) {
        return fail_detail(c, PIXEL_CODEC_ERR_FAILED, "decode_frame",
            "decoded precision exceeds metadata bits_stored width");
      }
    }

    bool source_planar = false;
    switch (interleave_mode) {
    case charls::interleave_mode::none:
      source_planar = true;
      break;
    case charls::interleave_mode::line:
    case charls::interleave_mode::sample:
      source_planar = false;
      break;
    default:
      return fail_detail(c, PIXEL_CODEC_ERR_UNSUPPORTED, "decode_frame",
          "unsupported JPEG-LS interleave mode");
    }

    const uint64_t source_row_components = source_planar ? 1 : samples;
    uint64_t source_row_bytes = 0;
    if (!mul_u64(cols, source_row_components, &source_row_bytes) ||
        !mul_u64(source_row_bytes, decoded_sample_bytes, &source_row_bytes)) {
      return fail_detail(c, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "decode_frame",
          "decoded row byte size overflow");
    }
    if (source_row_bytes > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
      return fail_detail(c, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "decode_frame",
          "decoded row stride exceeds uint32 range");
    }

    uint64_t source_frame_bytes = 0;
    if (!mul_u64(source_row_bytes, rows, &source_frame_bytes)) {
      return fail_detail(c, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "decode_frame",
          "decoded frame byte size overflow");
    }
    if (source_planar && samples > 1) {
      if (!mul_u64(source_frame_bytes, samples, &source_frame_bytes)) {
        return fail_detail(c, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "decode_frame",
            "decoded planar frame byte size overflow");
      }
    }

    std::vector<uint8_t> decoded(source_frame_bytes);
    decoder.decode(decoded.data(), decoded.size(), static_cast<uint32_t>(source_row_bytes));

    const pixel_error_code_v2 write_ec = write_decoded_pixels(c, request,
        decoded.data(), decoded_sample_bytes, frame_info.bits_per_sample,
        source_dtype.is_signed, source_planar, source_row_bytes,
        dst_dtype, row_stride, output_planar, transform_kind);
    if (write_ec != PIXEL_CODEC_ERR_OK) {
      return write_ec;
    }

    clear_detail(c);
    return PIXEL_CODEC_ERR_OK;
  } catch (const std::bad_alloc&) {
    return fail_detail(c, PIXEL_CODEC_ERR_FAILED, "allocate",
        "memory allocation failed");
  } catch (const std::exception& e) {
    return fail_detail(c, PIXEL_CODEC_ERR_FAILED, "decode_frame", e.what());
  } catch (...) {
    return fail_detail(c, PIXEL_CODEC_ERR_FAILED, "decode_frame",
        "non-standard exception");
  }
}

}  // namespace pixel::jpegls_plugin_v2

