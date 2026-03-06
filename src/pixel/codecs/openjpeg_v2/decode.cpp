#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <new>
#include <string>

#include "internal.hpp"

namespace pixel::openjpeg_plugin_v2 {

pixel_error_code_v2 validate_decoder_request(
    DecoderCtx* ctx, const pixel_decoder_request_v2* request) {
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

  return PIXEL_CODEC_ERR_OK;
}

pixel_error_code_v2 write_decoded_image(
    DecoderCtx* ctx, const pixel_decoder_request_v2* request, const opj_image_t* image) {
  if (image == nullptr) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_FAILED, "decode_frame",
        "decoded image is null");
  }

  const uint64_t rows = static_cast<uint64_t>(request->frame.rows);
  const uint64_t cols = static_cast<uint64_t>(request->frame.cols);
  const uint64_t samples = static_cast<uint64_t>(request->frame.samples_per_pixel);

  if (image->numcomps != static_cast<OPJ_UINT32>(samples)) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_FAILED, "decode_frame",
        "decoded component count mismatch");
  }

  const uint64_t decoded_rows =
      (image->y1 >= image->y0) ? static_cast<uint64_t>(image->y1 - image->y0) : 0;
  const uint64_t decoded_cols =
      (image->x1 >= image->x0) ? static_cast<uint64_t>(image->x1 - image->x0) : 0;
  if (decoded_rows != rows || decoded_cols != cols) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_FAILED, "decode_frame",
        "decoded dimensions mismatch");
  }

  for (uint64_t c = 0; c < samples; ++c) {
    const auto& comp = image->comps[c];
    if (comp.data == nullptr) {
      return fail_detail(ctx, PIXEL_CODEC_ERR_FAILED, "decode_frame",
          "decoded component has null data");
    }
    if (comp.w != cols || comp.h != rows) {
      return fail_detail(ctx, PIXEL_CODEC_ERR_FAILED, "decode_frame",
          "decoded component dimensions mismatch");
    }
  }

  DtypeInfo dst_dtype{};
  if (!dtype_info_from_code(request->output.dst_dtype, &dst_dtype)) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "decode_frame",
        "unsupported dst_dtype code");
  }

  const bool planar = is_planar_code(request->output.dst_planar);
  const uint64_t row_components = planar ? 1 : samples;

  uint64_t min_row_bytes = 0;
  if (!mul_u64(cols, row_components, &min_row_bytes) ||
      !mul_u64(min_row_bytes, dst_dtype.bytes, &min_row_bytes)) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "decode_frame",
        "row byte size overflow");
  }

  uint64_t row_stride = request->output.row_stride;
  if (row_stride == 0) {
    row_stride = min_row_bytes;
  }
  if (row_stride < min_row_bytes) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "decode_frame",
        "row_stride is too small");
  }

  uint64_t plane_bytes = 0;
  if (!mul_u64(row_stride, rows, &plane_bytes)) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "decode_frame",
        "plane byte size overflow");
  }

  uint64_t min_frame_bytes = plane_bytes;
  if (planar && samples > 1) {
    if (!mul_u64(plane_bytes, samples, &min_frame_bytes)) {
      return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "decode_frame",
          "frame byte size overflow");
    }
  }

  uint64_t frame_stride = request->output.frame_stride;
  if (frame_stride == 0) {
    frame_stride = min_frame_bytes;
  }
  if (frame_stride < min_frame_bytes) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "decode_frame",
        "frame_stride is too small");
  }
  if (request->output.dst_size < frame_stride) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "decode_frame",
        "destination buffer is too small");
  }

  const uint32_t transform_kind = request->value_transform.transform_kind;
  const bool transformed = transform_kind != PIXEL_DECODER_VALUE_TRANSFORM_NONE_V2;

  const std::size_t row_stride_sz = static_cast<std::size_t>(row_stride);
  const std::size_t plane_bytes_sz = static_cast<std::size_t>(plane_bytes);
  const std::size_t samples_sz = static_cast<std::size_t>(samples);
  const std::size_t cols_sz = static_cast<std::size_t>(cols);
  const std::size_t rows_sz = static_cast<std::size_t>(rows);
  const std::size_t dst_sample_bytes = static_cast<std::size_t>(dst_dtype.bytes);

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

  if (!transformed) {
    if (planar && samples_sz > 1) {
      for (std::size_t comp = 0; comp < samples_sz; ++comp) {
        const int* src_comp = image->comps[comp].data;
        uint8_t* dst_plane = request->output.dst + comp * plane_bytes_sz;
        for (std::size_t r = 0; r < rows_sz; ++r) {
          const std::size_t src_row_idx = r * cols_sz;
          uint8_t* dst_row = dst_plane + r * row_stride_sz;
          for (std::size_t c = 0; c < cols_sz; ++c) {
            const int32_t sample = src_comp[src_row_idx + c];
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

    const std::size_t pixel_stride = samples_sz * dst_sample_bytes;
    for (std::size_t r = 0; r < rows_sz; ++r) {
      uint8_t* dst_row = request->output.dst + r * row_stride_sz;
      const std::size_t src_row_idx = r * cols_sz;
      for (std::size_t c = 0; c < cols_sz; ++c) {
        uint8_t* dst_pixel = dst_row + c * pixel_stride;
        for (std::size_t comp = 0; comp < samples_sz; ++comp) {
          const int32_t sample = image->comps[comp].data[src_row_idx + c];
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

  const int* src = image->comps[0].data;
  for (std::size_t r = 0; r < rows_sz; ++r) {
    const std::size_t src_row_idx = r * cols_sz;
    uint8_t* dst_row = request->output.dst + r * row_stride_sz;
    for (std::size_t c = 0; c < cols_sz; ++c) {
      const int32_t sample = src[src_row_idx + c];
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
    const pixel_error_code_v2 valid_ec = validate_decoder_request(c, request);
    if (valid_ec != PIXEL_CODEC_ERR_OK) {
      return valid_ec;
    }

    OPJ_UINT32 thread_count = 0;
    if (!resolve_thread_count(c->threads, &thread_count)) {
      return fail_detail(c, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "decode_frame",
          "invalid decoder thread count");
    }

    opj_image_ptr decoded_image;
    std::string decode_error;
    if (!decode_with_openjpeg_auto(request->source.source_buffer.data,
            static_cast<std::size_t>(request->source.source_buffer.size),
            thread_count, &decoded_image, &decode_error)) {
      return fail_detail(c, PIXEL_CODEC_ERR_FAILED, "decode_frame",
          decode_error.c_str());
    }

    const pixel_error_code_v2 write_ec = write_decoded_image(c, request, decoded_image.get());
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

}  // namespace pixel::openjpeg_plugin_v2
