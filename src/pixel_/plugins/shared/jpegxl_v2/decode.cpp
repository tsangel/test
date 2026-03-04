#include <jxl/codestream_header.h>
#include <jxl/decode.h>
#include <jxl/thread_parallel_runner.h>
#include <jxl/types.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <vector>

#include "internal.hpp"

namespace pixel::jpegxl_plugin_v2 {

namespace {

class JxlDecoderGuard {
public:
  explicit JxlDecoderGuard(JxlDecoder* decoder) noexcept : decoder_(decoder) {}
  ~JxlDecoderGuard() {
    if (decoder_ != nullptr) {
      JxlDecoderDestroy(decoder_);
    }
  }

  JxlDecoderGuard(const JxlDecoderGuard&) = delete;
  JxlDecoderGuard& operator=(const JxlDecoderGuard&) = delete;

  [[nodiscard]] JxlDecoder* get() const noexcept {
    return decoder_;
  }

private:
  JxlDecoder* decoder_{nullptr};
};

class JxlRunnerGuard {
public:
  JxlRunnerGuard() = default;
  ~JxlRunnerGuard() {
    if (runner_ != nullptr) {
      JxlThreadParallelRunnerDestroy(runner_);
    }
  }

  JxlRunnerGuard(const JxlRunnerGuard&) = delete;
  JxlRunnerGuard& operator=(const JxlRunnerGuard&) = delete;

  void reset(void* runner) {
    if (runner_ != nullptr) {
      JxlThreadParallelRunnerDestroy(runner_);
    }
    runner_ = runner;
  }

  [[nodiscard]] void* get() const noexcept {
    return runner_;
  }

private:
  void* runner_{nullptr};
};

const char* jxl_status_name(JxlDecoderStatus status) noexcept {
  switch (status) {
  case JXL_DEC_SUCCESS:
    return "success";
  case JXL_DEC_ERROR:
    return "error";
  case JXL_DEC_NEED_MORE_INPUT:
    return "need_more_input";
  case JXL_DEC_NEED_IMAGE_OUT_BUFFER:
    return "need_image_out_buffer";
  case JXL_DEC_BASIC_INFO:
    return "basic_info";
  case JXL_DEC_FULL_IMAGE:
    return "full_image";
  default:
    return "other";
  }
}

bool resolve_worker_threads(int threads, std::size_t* out_threads) {
  if (out_threads == nullptr || threads < -1) {
    return false;
  }
  if (threads == -1) {
    *out_threads = JxlThreadParallelRunnerDefaultNumWorkerThreads();
    return true;
  }
  if (threads == 0) {
    *out_threads = 0;
    return true;
  }
  *out_threads = static_cast<std::size_t>(threads);
  return true;
}

struct DecodedImage {
  std::vector<uint8_t> pixels{};
  uint64_t row_bytes{0};
  int bits_per_sample{0};
};

pixel_error_code_v2 validate_decoder_request(
    DecoderCtx* ctx, const pixel_decoder_request_v2* request,
    DtypeInfo* out_source_dtype, DtypeInfo* out_dst_dtype,
    uint64_t* out_row_stride, bool* out_output_planar,
    uint32_t* out_transform_kind) {
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
  *out_output_planar = output_planar;
  *out_transform_kind = transform_kind;
  return PIXEL_CODEC_ERR_OK;
}

pixel_error_code_v2 validate_basic_info(DecoderCtx* ctx,
    const pixel_decoder_request_v2* request, const DtypeInfo& source_dtype,
    const JxlBasicInfo& basic_info) {
  if (basic_info.have_animation == JXL_TRUE) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_UNSUPPORTED, "decode_frame",
        "JPEG-XL animation codestream is not supported");
  }

  const uint64_t rows = static_cast<uint64_t>(request->frame.rows);
  const uint64_t cols = static_cast<uint64_t>(request->frame.cols);
  if (basic_info.xsize != cols || basic_info.ysize != rows) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_FAILED, "decode_frame",
        "decoded dimensions mismatch");
  }

  if (basic_info.exponent_bits_per_sample != 0) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_UNSUPPORTED, "decode_frame",
        "JPEG-XL floating-point codestream is not supported");
  }

  if (basic_info.bits_per_sample == 0) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_FAILED, "decode_frame",
        "JPEG-XL decoded precision is invalid");
  }

  const int32_t samples = request->frame.samples_per_pixel;
  if (samples == 1) {
    if (basic_info.num_color_channels != 1) {
      return fail_detail(ctx, PIXEL_CODEC_ERR_FAILED, "decode_frame",
          "decoded component count mismatch");
    }
  } else if (samples == 3) {
    if (basic_info.num_color_channels != 3 ||
        basic_info.num_extra_channels != 0 ||
        basic_info.alpha_bits != 0) {
      return fail_detail(ctx, PIXEL_CODEC_ERR_FAILED, "decode_frame",
          "decoded component layout mismatch for samples_per_pixel=3");
    }
  } else if (samples == 4) {
    if (basic_info.num_color_channels != 3 ||
        basic_info.num_extra_channels == 0 ||
        basic_info.alpha_bits == 0) {
      return fail_detail(ctx, PIXEL_CODEC_ERR_FAILED, "decode_frame",
          "decoded component layout mismatch for samples_per_pixel=4");
    }
  }

  const uint32_t max_output_bits = source_dtype.bytes * 8u;
  if (basic_info.bits_per_sample > max_output_bits) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_FAILED, "decode_frame",
        "decoded precision exceeds source dtype width");
  }

  const uint32_t request_bits_stored =
      static_cast<uint32_t>(request->frame.bits_stored);
  if (basic_info.bits_per_sample > request_bits_stored &&
      ((basic_info.bits_per_sample + 7u) / 8u) >
          ((request_bits_stored + 7u) / 8u)) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_FAILED, "decode_frame",
        "decoded precision exceeds bits_stored width");
  }

  return PIXEL_CODEC_ERR_OK;
}

pixel_error_code_v2 decode_with_jxl(DecoderCtx* ctx,
    const pixel_decoder_request_v2* request, const DtypeInfo& source_dtype,
    DecodedImage* out_image) {
  if (out_image == nullptr) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "decode_frame",
        "decoded image output pointer is null");
  }

  const auto sig = JxlSignatureCheck(
      request->source.source_buffer.data,
      static_cast<std::size_t>(request->source.source_buffer.size));
  if (sig == JXL_SIG_INVALID || sig == JXL_SIG_NOT_ENOUGH_BYTES) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_FAILED, "decode_frame",
        "invalid JPEG-XL signature");
  }

  JxlDecoderGuard decoder(JxlDecoderCreate(nullptr));
  if (decoder.get() == nullptr) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_FAILED, "decode_frame",
        "failed to create JPEG-XL decoder");
  }

  if (JxlDecoderSetKeepOrientation(decoder.get(), JXL_TRUE) != JXL_DEC_SUCCESS) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_FAILED, "decode_frame",
        "failed to configure orientation handling");
  }

  const auto subscribe_status = JxlDecoderSubscribeEvents(
      decoder.get(), JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE);
  if (subscribe_status != JXL_DEC_SUCCESS) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_FAILED, "decode_frame",
        "failed to subscribe decoder events");
  }

  std::size_t worker_threads = 0;
  if (!resolve_worker_threads(ctx->threads, &worker_threads)) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "decode_frame",
        "invalid decoder thread count");
  }

  JxlRunnerGuard runner{};
  if (worker_threads > 0) {
    runner.reset(JxlThreadParallelRunnerCreate(nullptr, worker_threads));
    if (runner.get() == nullptr) {
      return fail_detail(ctx, PIXEL_CODEC_ERR_FAILED, "decode_frame",
          "failed to initialize JPEG-XL thread runner");
    }
    if (JxlDecoderSetParallelRunner(
            decoder.get(), JxlThreadParallelRunner, runner.get()) != JXL_DEC_SUCCESS) {
      return fail_detail(ctx, PIXEL_CODEC_ERR_FAILED, "decode_frame",
          "failed to set JPEG-XL thread runner");
    }
  }

  const auto set_input_status = JxlDecoderSetInput(
      decoder.get(), request->source.source_buffer.data,
      static_cast<std::size_t>(request->source.source_buffer.size));
  if (set_input_status != JXL_DEC_SUCCESS) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_FAILED, "decode_frame",
        "failed to provide JPEG-XL input");
  }
  JxlDecoderCloseInput(decoder.get());

  const uint64_t rows = static_cast<uint64_t>(request->frame.rows);
  const uint64_t cols = static_cast<uint64_t>(request->frame.cols);
  const uint64_t samples = static_cast<uint64_t>(request->frame.samples_per_pixel);
  const uint64_t source_sample_bytes = source_dtype.bytes;

  uint64_t min_row_bytes = 0;
  if (!mul_u64(cols, samples, &min_row_bytes) ||
      !mul_u64(min_row_bytes, source_sample_bytes, &min_row_bytes)) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "decode_frame",
        "decoded row byte size overflow");
  }

  uint64_t min_total_bytes = 0;
  if (!mul_u64(min_row_bytes, rows, &min_total_bytes)) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "decode_frame",
        "decoded frame byte size overflow");
  }

  const JxlPixelFormat pixel_format{
      static_cast<uint32_t>(samples),
      source_dtype.bytes == 1 ? JXL_TYPE_UINT8 : JXL_TYPE_UINT16,
      JXL_LITTLE_ENDIAN,
      0,
  };

  bool saw_basic_info = false;
  bool saw_full_image = false;
  int decoded_bits = 0;
  uint64_t decoded_row_bytes = 0;
  std::vector<uint8_t> decoded{};

  while (true) {
    const auto status = JxlDecoderProcessInput(decoder.get());
    switch (status) {
    case JXL_DEC_BASIC_INFO: {
      JxlBasicInfo basic_info{};
      if (JxlDecoderGetBasicInfo(decoder.get(), &basic_info) != JXL_DEC_SUCCESS) {
        return fail_detail(ctx, PIXEL_CODEC_ERR_FAILED, "decode_frame",
            "failed to read JPEG-XL basic info");
      }
      const pixel_error_code_v2 ec =
          validate_basic_info(ctx, request, source_dtype, basic_info);
      if (ec != PIXEL_CODEC_ERR_OK) {
        return ec;
      }
      saw_basic_info = true;
      decoded_bits = static_cast<int>(basic_info.bits_per_sample);
      break;
    }
    case JXL_DEC_NEED_IMAGE_OUT_BUFFER: {
      if (!saw_basic_info) {
        return fail_detail(ctx, PIXEL_CODEC_ERR_FAILED, "decode_frame",
            "decoder requested output buffer before basic info");
      }

      std::size_t image_out_bytes = 0;
      if (JxlDecoderImageOutBufferSize(decoder.get(), &pixel_format, &image_out_bytes) !=
          JXL_DEC_SUCCESS) {
        return fail_detail(ctx, PIXEL_CODEC_ERR_FAILED, "decode_frame",
            "failed to query output buffer size");
      }
      if (image_out_bytes < min_total_bytes) {
        return fail_detail(ctx, PIXEL_CODEC_ERR_FAILED, "decode_frame",
            "JPEG-XL output buffer smaller than expected");
      }
      if (rows == 0 || image_out_bytes % rows != 0) {
        return fail_detail(ctx, PIXEL_CODEC_ERR_FAILED, "decode_frame",
            "invalid decoded output buffer layout");
      }

      decoded_row_bytes = image_out_bytes / rows;
      if (decoded_row_bytes < min_row_bytes) {
        return fail_detail(ctx, PIXEL_CODEC_ERR_FAILED, "decode_frame",
            "decoded output row stride is too small");
      }

      decoded.resize(image_out_bytes);
      if (JxlDecoderSetImageOutBuffer(
              decoder.get(), &pixel_format, decoded.data(), decoded.size()) != JXL_DEC_SUCCESS) {
        return fail_detail(ctx, PIXEL_CODEC_ERR_FAILED, "decode_frame",
            "failed to set JPEG-XL output buffer");
      }

      const JxlBitDepth output_bit_depth{JXL_BIT_DEPTH_FROM_CODESTREAM, 0, 0};
      if (JxlDecoderSetImageOutBitDepth(decoder.get(), &output_bit_depth) != JXL_DEC_SUCCESS) {
        return fail_detail(ctx, PIXEL_CODEC_ERR_FAILED, "decode_frame",
            "failed to set output bit depth policy");
      }
      break;
    }
    case JXL_DEC_FULL_IMAGE:
      saw_full_image = true;
      break;
    case JXL_DEC_SUCCESS:
      if (!saw_full_image) {
        return fail_detail(ctx, PIXEL_CODEC_ERR_FAILED, "decode_frame",
            "decode finished before full image event");
      }
      if (decoded.empty() || decoded_bits <= 0 || decoded_row_bytes == 0) {
        return fail_detail(ctx, PIXEL_CODEC_ERR_FAILED, "decode_frame",
            "decoded image is empty");
      }
      out_image->pixels = std::move(decoded);
      out_image->row_bytes = decoded_row_bytes;
      out_image->bits_per_sample = decoded_bits;
      return PIXEL_CODEC_ERR_OK;
    case JXL_DEC_NEED_MORE_INPUT:
      return fail_detail(ctx, PIXEL_CODEC_ERR_FAILED, "decode_frame",
          "truncated JPEG-XL codestream");
    case JXL_DEC_ERROR: {
      char reason[256];
      std::snprintf(reason, sizeof(reason),
          "JPEG-XL decode failed (status=%s)", jxl_status_name(status));
      return fail_detail(ctx, PIXEL_CODEC_ERR_FAILED, "decode_frame", reason);
    }
    default:
      break;
    }
  }
}

pixel_error_code_v2 write_decoded_pixels(DecoderCtx* ctx,
    const pixel_decoder_request_v2* request,
    const std::vector<uint8_t>& decoded,
    uint32_t source_sample_bytes,
    int source_bits,
    bool source_is_signed,
    uint64_t source_row_bytes,
    DtypeInfo dst_dtype,
    uint64_t row_stride,
    bool output_planar,
    uint32_t transform_kind) {
  if (decoded.empty()) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_FAILED, "decode_frame",
        "decoded source buffer is empty");
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
    const std::size_t pixel_stride = samples_sz * source_sample_bytes_sz;
    return decoded.data() + r * source_row_bytes_sz +
        c * pixel_stride + comp * source_sample_bytes_sz;
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
    bool output_planar = false;
    uint32_t transform_kind = PIXEL_DECODER_VALUE_TRANSFORM_NONE_V2;

    const pixel_error_code_v2 valid_ec = validate_decoder_request(c, request,
        &source_dtype, &dst_dtype, &row_stride, &output_planar, &transform_kind);
    if (valid_ec != PIXEL_CODEC_ERR_OK) {
      return valid_ec;
    }

    DecodedImage decoded{};
    const pixel_error_code_v2 decode_ec =
        decode_with_jxl(c, request, source_dtype, &decoded);
    if (decode_ec != PIXEL_CODEC_ERR_OK) {
      return decode_ec;
    }

    const pixel_error_code_v2 write_ec = write_decoded_pixels(c, request,
        decoded.pixels, source_dtype.bytes, decoded.bits_per_sample,
        source_dtype.is_signed, decoded.row_bytes, dst_dtype,
        row_stride, output_planar, transform_kind);
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

}  // namespace pixel::jpegxl_plugin_v2
