#include <jxl/codestream_header.h>
#include <jxl/color_encoding.h>
#include <jxl/encode.h>
#include <jxl/thread_parallel_runner.h>
#include <jxl/types.h>

#include <algorithm>
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

class JxlEncoderGuard {
public:
  explicit JxlEncoderGuard(JxlEncoder* encoder) noexcept : encoder_(encoder) {}
  ~JxlEncoderGuard() {
    if (encoder_ != nullptr) {
      JxlEncoderDestroy(encoder_);
    }
  }

  JxlEncoderGuard(const JxlEncoderGuard&) = delete;
  JxlEncoderGuard& operator=(const JxlEncoderGuard&) = delete;

  [[nodiscard]] JxlEncoder* get() const noexcept {
    return encoder_;
  }

private:
  JxlEncoder* encoder_{nullptr};
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

struct SourceFrameLayout {
  bool source_is_planar{false};
  uint64_t row_payload{0};
  uint64_t row_stride{0};
  uint64_t plane_stride{0};
  uint64_t minimum_frame_bytes{0};
};

const char* jxl_encode_status_name(JxlEncoderStatus status) noexcept {
  switch (status) {
  case JXL_ENC_SUCCESS:
    return "success";
  case JXL_ENC_ERROR:
    return "error";
  case JXL_ENC_NEED_MORE_OUTPUT:
    return "need_more_output";
  default:
    return "other";
  }
}

const char* jxl_encode_error_name(JxlEncoderError error) noexcept {
  switch (error) {
  case JXL_ENC_ERR_OK:
    return "ok";
  case JXL_ENC_ERR_GENERIC:
    return "generic";
  case JXL_ENC_ERR_OOM:
    return "oom";
  case JXL_ENC_ERR_JBRD:
    return "jpeg_reconstruction";
  case JXL_ENC_ERR_BAD_INPUT:
    return "bad_input";
  case JXL_ENC_ERR_NOT_SUPPORTED:
    return "not_supported";
  case JXL_ENC_ERR_API_USAGE:
    return "api_usage";
  default:
    return "unknown";
  }
}

pixel_error_code_v2 check_jxl_success(EncoderCtx* ctx, JxlEncoder* encoder,
    JxlEncoderStatus status, const char* action) {
  if (status == JXL_ENC_SUCCESS) {
    return PIXEL_CODEC_ERR_OK;
  }
  const auto error = JxlEncoderGetError(encoder);
  char reason[320];
  std::snprintf(reason, sizeof(reason),
      "%s failed (status=%s,error=%s)", action,
      jxl_encode_status_name(status), jxl_encode_error_name(error));
  return fail_detail(ctx, PIXEL_CODEC_ERR_FAILED, "encode_frame", reason);
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

pixel_error_code_v2 validate_encoder_request(
    EncoderCtx* ctx, const pixel_encoder_request_v2* request,
    DtypeInfo* out_source_dtype, SourceFrameLayout* out_source_layout,
    bool* out_source_signed, bool* out_lossless) {
  if (request->struct_size < sizeof(pixel_encoder_request_v2) ||
      request->source.struct_size < sizeof(pixel_encoder_source_v2) ||
      request->frame.struct_size < sizeof(pixel_encoder_frame_info_v2) ||
      request->output.struct_size < sizeof(pixel_encoder_output_v2)) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "encoder request struct_size is too small");
  }

  if (request->frame.codec_profile_code != ctx->codec_profile_code) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "encoder request codec_profile_code does not match configured profile");
  }

  if (!is_supported_encoder_profile(request->frame.codec_profile_code)) {
    return fail_detail_u32(ctx, PIXEL_CODEC_ERR_UNSUPPORTED, "validate",
        "unsupported encoder codec_profile_code=%u", request->frame.codec_profile_code);
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

  const uint64_t rows = static_cast<uint64_t>(request->frame.rows);
  const uint64_t cols = static_cast<uint64_t>(request->frame.cols);
  const uint64_t samples = static_cast<uint64_t>(request->frame.samples_per_pixel);

  if (samples != 1 && samples != 3 && samples != 4) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_UNSUPPORTED, "validate",
        "samples_per_pixel must be 1, 3, or 4");
  }

  DtypeInfo source_dtype{};
  if (!dtype_info_from_code(request->frame.source_dtype, &source_dtype) ||
      source_dtype.is_float || source_dtype.bytes == 0 || source_dtype.bytes > 2) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "source_dtype must be integral 8/16-bit type");
  }

  if (!is_valid_planar_code(request->frame.source_planar)) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "unsupported source_planar code");
  }

  if (request->frame.bits_allocated != 8 && request->frame.bits_allocated != 16) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "bits_allocated must be 8 or 16");
  }

  if (request->frame.bits_stored <= 0 ||
      request->frame.bits_stored > request->frame.bits_allocated) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "bits_stored must be in [1,bits_allocated]");
  }

  if (request->frame.bits_allocated >
      static_cast<int32_t>(source_dtype.bytes * 8u)) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "bits_allocated exceeds source_dtype width");
  }

  if ((request->frame.bits_stored <= 8 && source_dtype.bytes != 1) ||
      (request->frame.bits_stored > 8 && source_dtype.bytes != 2)) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "source_dtype width is incompatible with bits_stored");
  }

  if (request->frame.pixel_representation != 0 &&
      request->frame.pixel_representation != 1) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "pixel_representation must be 0 or 1");
  }

  const bool source_signed = request->frame.pixel_representation == 1;
  if (source_signed != source_dtype.is_signed) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "pixel_representation does not match source_dtype signedness");
  }

  if (request->frame.use_multicomponent_transform != 0 && samples != 3) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "use_multicomponent_transform requires samples_per_pixel=3");
  }

  const bool source_planar = is_planar_code(request->frame.source_planar) && samples > 1;
  const uint64_t row_components = source_planar ? 1 : samples;

  uint64_t row_payload = 0;
  if (!mul_u64(cols, row_components, &row_payload) ||
      !mul_u64(row_payload, source_dtype.bytes, &row_payload)) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "source row payload overflow");
  }

  uint64_t row_stride = request->frame.source_row_stride;
  if (row_stride == 0) {
    row_stride = row_payload;
  }
  if (row_stride < row_payload) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "source_row_stride is too small");
  }

  uint64_t plane_stride = 0;
  if (!mul_u64(row_stride, rows, &plane_stride)) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "source plane stride overflow");
  }

  uint64_t minimum_frame_bytes = plane_stride;
  if (source_planar) {
    uint64_t source_plane_stride = request->frame.source_plane_stride;
    if (source_plane_stride == 0) {
      source_plane_stride = plane_stride;
    }
    if (source_plane_stride < plane_stride) {
      return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
          "source_plane_stride is too small");
    }
    plane_stride = source_plane_stride;
    if (!mul_u64(plane_stride, samples, &minimum_frame_bytes)) {
      return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
          "source frame byte size overflow");
    }
  }

  uint64_t source_frame_bytes = request->frame.source_frame_size_bytes;
  if (source_frame_bytes == 0) {
    source_frame_bytes = minimum_frame_bytes;
  }
  if (source_frame_bytes < minimum_frame_bytes ||
      request->source.source_buffer.size < source_frame_bytes) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "source buffer is too small for frame layout");
  }

  const bool lossless =
      request->frame.codec_profile_code == PIXEL_CODEC_PROFILE_JPEGXL_LOSSLESS_V2;

  if (!lossless && (!std::isfinite(ctx->distance) ||
        ctx->distance <= 0.0 || ctx->distance > 25.0)) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "lossy JPEG-XL requires distance in (0,25]");
  }

  if (lossless && ctx->distance != 0.0) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "lossless JPEG-XL requires distance=0");
  }

  if (ctx->effort < 1 || ctx->effort > 10) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "effort must be in [1,10]");
  }

  if (ctx->threads < -1) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "threads must be -1, 0, or positive");
  }

  out_source_layout->source_is_planar = source_planar;
  out_source_layout->row_payload = row_payload;
  out_source_layout->row_stride = row_stride;
  out_source_layout->plane_stride = plane_stride;
  out_source_layout->minimum_frame_bytes = minimum_frame_bytes;
  *out_source_dtype = source_dtype;
  *out_source_signed = source_signed;
  *out_lossless = lossless;
  return PIXEL_CODEC_ERR_OK;
}

pixel_error_code_v2 build_normalized_interleaved(
    EncoderCtx* ctx, const pixel_encoder_request_v2* request,
    const DtypeInfo& source_dtype, const SourceFrameLayout& source_layout,
    bool source_signed, std::vector<uint8_t>* out_interleaved) {
  if (out_interleaved == nullptr) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "encode_frame",
        "interleaved output pointer is null");
  }

  const uint64_t rows = static_cast<uint64_t>(request->frame.rows);
  const uint64_t cols = static_cast<uint64_t>(request->frame.cols);
  const uint64_t samples = static_cast<uint64_t>(request->frame.samples_per_pixel);

  uint64_t row_bytes = 0;
  if (!mul_u64(cols, samples, &row_bytes) ||
      !mul_u64(row_bytes, source_dtype.bytes, &row_bytes)) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "encode_frame",
        "interleaved row byte size overflow");
  }

  uint64_t total_bytes = 0;
  if (!mul_u64(row_bytes, rows, &total_bytes)) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "encode_frame",
        "interleaved frame byte size overflow");
  }

  out_interleaved->assign(static_cast<std::size_t>(total_bytes), uint8_t{0});

  const std::size_t rows_sz = static_cast<std::size_t>(rows);
  const std::size_t cols_sz = static_cast<std::size_t>(cols);
  const std::size_t samples_sz = static_cast<std::size_t>(samples);
  const std::size_t row_bytes_sz = static_cast<std::size_t>(row_bytes);
  const std::size_t source_row_stride_sz = static_cast<std::size_t>(source_layout.row_stride);
  const std::size_t source_plane_stride_sz = static_cast<std::size_t>(source_layout.plane_stride);
  const std::size_t source_sample_bytes_sz = static_cast<std::size_t>(source_dtype.bytes);
  const std::size_t interleaved_pixel_stride = samples_sz * source_sample_bytes_sz;

  const uint8_t* source = request->source.source_buffer.data;
  for (std::size_t r = 0; r < rows_sz; ++r) {
    uint8_t* dst_row = out_interleaved->data() + r * row_bytes_sz;
    for (std::size_t c = 0; c < cols_sz; ++c) {
      uint8_t* dst_pixel = dst_row + c * interleaved_pixel_stride;
      for (std::size_t comp = 0; comp < samples_sz; ++comp) {
        const uint8_t* src_ptr = nullptr;
        if (source_layout.source_is_planar) {
          src_ptr = source + comp * source_plane_stride_sz +
              r * source_row_stride_sz + c * source_sample_bytes_sz;
        } else {
          src_ptr = source + r * source_row_stride_sz +
              (c * samples_sz + comp) * source_sample_bytes_sz;
        }

        char reason[256];
        int32_t sample = 0;
        if (!load_integral_sample(src_ptr,
                static_cast<uint32_t>(source_sample_bytes_sz),
                source_signed, request->frame.bits_stored,
                &sample, reason, sizeof(reason))) {
          return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "encode_frame", reason);
        }

        const uint32_t raw =
            static_cast<uint32_t>(sample) & bit_mask_u32(request->frame.bits_stored);
        uint8_t* dst_ptr = dst_pixel + comp * source_sample_bytes_sz;
        if (!store_raw_sample(dst_ptr, source_dtype.bytes, raw)) {
          return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "encode_frame",
              "failed to write normalized sample");
        }
      }
    }
  }

  return PIXEL_CODEC_ERR_OK;
}

}  // namespace

pixel_error_code_v2 encoder_encode_frame(
    void* ctx, const pixel_encoder_request_v2* request) {
  auto* c = static_cast<EncoderCtx*>(ctx);
  if (c == nullptr || request == nullptr) {
    return fail_detail(c, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "invalid encoder request");
  }
  if (!c->configured) {
    return fail_detail(c, PIXEL_CODEC_ERR_FAILED, "encode_frame",
        "configure must be called before encode_frame");
  }

  try {
    DtypeInfo source_dtype{};
    SourceFrameLayout source_layout{};
    bool source_signed = false;
    bool lossless = false;

    const pixel_error_code_v2 valid_ec = validate_encoder_request(c, request,
        &source_dtype, &source_layout, &source_signed, &lossless);
    if (valid_ec != PIXEL_CODEC_ERR_OK) {
      return valid_ec;
    }

    std::vector<uint8_t> interleaved{};
    const pixel_error_code_v2 normalize_ec = build_normalized_interleaved(c, request,
        source_dtype, source_layout, source_signed, &interleaved);
    if (normalize_ec != PIXEL_CODEC_ERR_OK) {
      return normalize_ec;
    }

    JxlEncoderGuard encoder(JxlEncoderCreate(nullptr));
    if (encoder.get() == nullptr) {
      return fail_detail(c, PIXEL_CODEC_ERR_FAILED, "encode_frame",
          "failed to initialize JPEG-XL encoder");
    }

    std::size_t worker_threads = 0;
    if (!resolve_worker_threads(c->threads, &worker_threads)) {
      return fail_detail(c, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "encode_frame",
          "invalid encoder thread count");
    }

    JxlRunnerGuard runner{};
    if (worker_threads > 0) {
      runner.reset(JxlThreadParallelRunnerCreate(nullptr, worker_threads));
      if (runner.get() == nullptr) {
        return fail_detail(c, PIXEL_CODEC_ERR_FAILED, "encode_frame",
            "failed to initialize JPEG-XL thread runner");
      }
      const pixel_error_code_v2 ec = check_jxl_success(c, encoder.get(),
          JxlEncoderSetParallelRunner(encoder.get(), JxlThreadParallelRunner, runner.get()),
          "set parallel runner");
      if (ec != PIXEL_CODEC_ERR_OK) {
        return ec;
      }
    }

    const uint32_t cols = static_cast<uint32_t>(request->frame.cols);
    const uint32_t rows = static_cast<uint32_t>(request->frame.rows);
    const uint32_t samples = static_cast<uint32_t>(request->frame.samples_per_pixel);

    JxlBasicInfo basic_info{};
    JxlEncoderInitBasicInfo(&basic_info);
    basic_info.xsize = cols;
    basic_info.ysize = rows;
    basic_info.bits_per_sample = static_cast<uint32_t>(request->frame.bits_stored);
    basic_info.exponent_bits_per_sample = 0;
    basic_info.num_color_channels = (samples == 1) ? 1u : 3u;
    basic_info.num_extra_channels = (samples == 4) ? 1u : 0u;
    basic_info.alpha_bits =
        (samples == 4) ? static_cast<uint32_t>(request->frame.bits_stored) : 0u;
    basic_info.alpha_exponent_bits = 0;
    basic_info.alpha_premultiplied = JXL_FALSE;
    basic_info.uses_original_profile = lossless ? JXL_TRUE : JXL_FALSE;

    {
      const pixel_error_code_v2 ec = check_jxl_success(c, encoder.get(),
          JxlEncoderSetBasicInfo(encoder.get(), &basic_info), "set basic info");
      if (ec != PIXEL_CODEC_ERR_OK) {
        return ec;
      }
    }

    if (samples == 4) {
      JxlExtraChannelInfo alpha_info{};
      JxlEncoderInitExtraChannelInfo(JXL_CHANNEL_ALPHA, &alpha_info);
      alpha_info.bits_per_sample = static_cast<uint32_t>(request->frame.bits_stored);
      alpha_info.exponent_bits_per_sample = 0;
      alpha_info.alpha_premultiplied = JXL_FALSE;
      const pixel_error_code_v2 ec = check_jxl_success(c, encoder.get(),
          JxlEncoderSetExtraChannelInfo(encoder.get(), 0, &alpha_info),
          "set alpha channel info");
      if (ec != PIXEL_CODEC_ERR_OK) {
        return ec;
      }
    }

    JxlColorEncoding color_encoding{};
    JxlColorEncodingSetToSRGB(&color_encoding, samples == 1 ? JXL_TRUE : JXL_FALSE);
    {
      const pixel_error_code_v2 ec = check_jxl_success(c, encoder.get(),
          JxlEncoderSetColorEncoding(encoder.get(), &color_encoding),
          "set color encoding");
      if (ec != PIXEL_CODEC_ERR_OK) {
        return ec;
      }
    }

    auto* frame_settings = JxlEncoderFrameSettingsCreate(encoder.get(), nullptr);
    if (frame_settings == nullptr) {
      return fail_detail(c, PIXEL_CODEC_ERR_FAILED, "encode_frame",
          "failed to allocate JPEG-XL frame settings");
    }

    {
      const pixel_error_code_v2 ec = check_jxl_success(c, encoder.get(),
          JxlEncoderFrameSettingsSetOption(
              frame_settings, JXL_ENC_FRAME_SETTING_EFFORT, c->effort),
          "set effort");
      if (ec != PIXEL_CODEC_ERR_OK) {
        return ec;
      }
    }

    if (lossless) {
      const pixel_error_code_v2 ec = check_jxl_success(c, encoder.get(),
          JxlEncoderSetFrameLossless(frame_settings, JXL_TRUE),
          "set lossless mode");
      if (ec != PIXEL_CODEC_ERR_OK) {
        return ec;
      }
    } else {
      const pixel_error_code_v2 ec = check_jxl_success(c, encoder.get(),
          JxlEncoderSetFrameDistance(frame_settings, static_cast<float>(c->distance)),
          "set distance");
      if (ec != PIXEL_CODEC_ERR_OK) {
        return ec;
      }
    }

    const JxlBitDepth bit_depth{
        JXL_BIT_DEPTH_FROM_CODESTREAM,
        static_cast<uint32_t>(request->frame.bits_stored),
        0,
    };
    {
      const pixel_error_code_v2 ec = check_jxl_success(c, encoder.get(),
          JxlEncoderSetFrameBitDepth(frame_settings, &bit_depth),
          "set frame bit depth");
      if (ec != PIXEL_CODEC_ERR_OK) {
        return ec;
      }
    }

    const JxlPixelFormat pixel_format{
        samples,
        source_dtype.bytes == 1 ? JXL_TYPE_UINT8 : JXL_TYPE_UINT16,
        JXL_LITTLE_ENDIAN,
        0,
    };
    {
      const pixel_error_code_v2 ec = check_jxl_success(c, encoder.get(),
          JxlEncoderAddImageFrame(
              frame_settings, &pixel_format, interleaved.data(), interleaved.size()),
          "add image frame");
      if (ec != PIXEL_CODEC_ERR_OK) {
        return ec;
      }
    }

    JxlEncoderCloseInput(encoder.get());

    std::vector<uint8_t> encoded(64 * 1024);
    std::size_t produced_bytes = 0;
    while (true) {
      if (encoded.size() - produced_bytes < 32) {
        const auto next_size =
            std::max<std::size_t>(encoded.size() * 2, produced_bytes + 64);
        encoded.resize(next_size);
      }

      auto* next_out = encoded.data() + produced_bytes;
      std::size_t avail_out = encoded.size() - produced_bytes;
      const auto status = JxlEncoderProcessOutput(encoder.get(), &next_out, &avail_out);
      produced_bytes = static_cast<std::size_t>(next_out - encoded.data());

      if (status == JXL_ENC_SUCCESS) {
        break;
      }
      if (status == JXL_ENC_NEED_MORE_OUTPUT) {
        if (produced_bytes == encoded.size()) {
          encoded.resize(encoded.size() * 2);
        }
        continue;
      }

      const auto error = JxlEncoderGetError(encoder.get());
      char reason[320];
      std::snprintf(reason, sizeof(reason),
          "JPEG-XL encode failed (status=%s,error=%s)",
          jxl_encode_status_name(status), jxl_encode_error_name(error));
      return fail_detail(c, PIXEL_CODEC_ERR_FAILED, "encode_frame", reason);
    }

    encoded.resize(produced_bytes);
    if (encoded.empty()) {
      return fail_detail(c, PIXEL_CODEC_ERR_FAILED, "encode_frame",
          "JPEG-XL encoder produced empty codestream");
    }

    auto* mutable_request = const_cast<pixel_encoder_request_v2*>(request);
    mutable_request->output.encoded_size = encoded.size();

    if (request->output.encoded_buffer.data == nullptr ||
        request->output.encoded_buffer.size < encoded.size()) {
      return fail_detail(c, PIXEL_CODEC_ERR_OUTPUT_TOO_SMALL, "encode_frame",
          "output buffer too small");
    }

    std::memcpy(request->output.encoded_buffer.data, encoded.data(), encoded.size());
    clear_detail(c);
    return PIXEL_CODEC_ERR_OK;
  } catch (const std::bad_alloc&) {
    return fail_detail(c, PIXEL_CODEC_ERR_FAILED, "allocate",
        "memory allocation failed");
  } catch (const std::exception& e) {
    return fail_detail(c, PIXEL_CODEC_ERR_FAILED, "encode_frame", e.what());
  } catch (...) {
    return fail_detail(c, PIXEL_CODEC_ERR_FAILED, "encode_frame",
        "non-standard exception");
  }
}

}  // namespace pixel::jpegxl_plugin_v2
