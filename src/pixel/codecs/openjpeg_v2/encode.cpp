#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <new>
#include <string>
#include <vector>

#include "internal.hpp"

namespace pixel::openjpeg_codec_v2 {

namespace {

pixel_error_code_v2 configure_j2k_lossy_parameters(EncoderCtx* ctx,
    opj_cparameters_t* parameters, const pixel_encoder_request_v2* request) {
  if (ctx == nullptr || parameters == nullptr || request == nullptr) {
    return PIXEL_CODEC_ERR_INVALID_ARGUMENT;
  }

  const double target_psnr = ctx->target_psnr;
  const double target_bpp = ctx->target_bpp;
  const double samples =
      static_cast<double>(request->frame.samples_per_pixel);
  const double bits_stored = static_cast<double>(request->frame.bits_stored);

  if (target_psnr > 0.0) {
    parameters->cp_disto_alloc = 0;
    parameters->cp_fixed_quality = 1;
    parameters->tcp_distoratio[0] = static_cast<float>(target_psnr);
    parameters->irreversible = 1;
    return PIXEL_CODEC_ERR_OK;
  }

  if (target_bpp > 0.0) {
    const double uncompressed_bpp = bits_stored * samples;
    float compression_ratio =
        static_cast<float>(uncompressed_bpp / target_bpp);
    if (compression_ratio < 1.0f) {
      compression_ratio = 1.0f;
    }
    parameters->cp_disto_alloc = 1;
    parameters->cp_fixed_quality = 0;
    parameters->tcp_rates[0] = compression_ratio;
    parameters->irreversible = 1;
    return PIXEL_CODEC_ERR_OK;
  }

  if (ctx->has_quality) {
    parameters->tcp_rates[0] = quality_to_openjpeg_rate(ctx->quality);
    return PIXEL_CODEC_ERR_OK;
  }

  return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "encode_frame",
      "lossy JPEG2000 requires target_psnr>0 or target_bpp>0");
}

}  // namespace

pixel_error_code_v2 validate_encoder_request(
    EncoderCtx* ctx, const pixel_encoder_request_v2* request, DtypeInfo* out_source_dtype,
    uint64_t* out_row_stride, uint64_t* out_plane_stride,
    bool* out_source_planar) {
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

  const uint64_t samples = static_cast<uint64_t>(request->frame.samples_per_pixel);
  if (samples != 1 && samples != 3 && samples != 4) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_UNSUPPORTED, "validate",
        "samples_per_pixel must be 1, 3, or 4");
  }

  DtypeInfo src_dtype{};
  if (!dtype_info_from_code(request->frame.source_dtype, &src_dtype) || src_dtype.is_float) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "source_dtype must be integral type");
  }

  if (!is_valid_planar_code(request->frame.source_planar)) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "unsupported source_planar code");
  }

  if (request->frame.bits_allocated <= 0 || request->frame.bits_allocated > 16) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "bits_allocated must be in [1,16]");
  }
  if (request->frame.bits_stored <= 0 ||
      request->frame.bits_stored > request->frame.bits_allocated) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "bits_stored must be in [1,bits_allocated]");
  }
  if (request->frame.pixel_representation != 0 &&
      request->frame.pixel_representation != 1) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "pixel_representation must be 0 or 1");
  }

  const bool pixel_is_signed = request->frame.pixel_representation == 1;
  if (pixel_is_signed != src_dtype.is_signed) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "pixel_representation does not match source_dtype signedness");
  }

  if (request->frame.bits_allocated > static_cast<int32_t>(src_dtype.bytes * 8u)) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "bits_allocated exceeds source dtype width");
  }

  const bool source_planar = is_planar_code(request->frame.source_planar);
  if (request->frame.use_multicomponent_transform != 0 && samples != 3) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "use_multicomponent_transform requires samples_per_pixel=3");
  }

  const uint64_t rows = static_cast<uint64_t>(request->frame.rows);
  const uint64_t cols = static_cast<uint64_t>(request->frame.cols);

  const uint64_t row_components = source_planar ? 1 : samples;
  uint64_t min_row_bytes = 0;
  if (!mul_u64(cols, row_components, &min_row_bytes) ||
      !mul_u64(min_row_bytes, src_dtype.bytes, &min_row_bytes)) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "source row byte size overflow");
  }

  uint64_t row_stride = request->frame.source_row_stride;
  if (row_stride == 0) {
    row_stride = min_row_bytes;
  }
  if (row_stride < min_row_bytes) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "source_row_stride is too small");
  }

  uint64_t plane_stride = 0;
  if (source_planar) {
    uint64_t min_plane_stride = 0;
    if (!mul_u64(row_stride, rows, &min_plane_stride)) {
      return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
          "source plane stride overflow");
    }
    plane_stride = request->frame.source_plane_stride;
    if (plane_stride == 0) {
      plane_stride = min_plane_stride;
    }
    if (plane_stride < min_plane_stride) {
      return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
          "source_plane_stride is too small");
    }
  }

  uint64_t min_frame_bytes = 0;
  if (source_planar) {
    if (!mul_u64(plane_stride, samples, &min_frame_bytes)) {
      return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
          "source frame byte size overflow");
    }
  } else {
    if (!mul_u64(row_stride, rows, &min_frame_bytes)) {
      return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
          "source frame byte size overflow");
    }
  }

  uint64_t source_frame_bytes = request->frame.source_frame_size_bytes;
  if (source_frame_bytes == 0) {
    source_frame_bytes = min_frame_bytes;
  }
  if (source_frame_bytes < min_frame_bytes ||
      request->source.source_buffer.size < source_frame_bytes) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "source buffer is too small for frame layout");
  }

  *out_source_dtype = src_dtype;
  *out_row_stride = row_stride;
  *out_plane_stride = plane_stride;
  *out_source_planar = source_planar;
  return PIXEL_CODEC_ERR_OK;
}


pixel_error_code_v2 encoder_encode_frame_to_context_buffer(
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
    uint64_t row_stride = 0;
    uint64_t plane_stride = 0;
    bool source_planar = false;
    const pixel_error_code_v2 valid_ec = validate_encoder_request(c, request,
        &source_dtype, &row_stride, &plane_stride,
        &source_planar);
    if (valid_ec != PIXEL_CODEC_ERR_OK) {
      return valid_ec;
    }

    OPJ_UINT32 thread_count = 0;
    if (!resolve_thread_count(c->threads, &thread_count)) {
      return fail_detail(c, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "encode_frame",
          "invalid encoder thread count");
    }

    const uint64_t rows = static_cast<uint64_t>(request->frame.rows);
    const uint64_t cols = static_cast<uint64_t>(request->frame.cols);
    const uint64_t samples = static_cast<uint64_t>(request->frame.samples_per_pixel);

    std::vector<opj_image_cmptparm_t> component_params(static_cast<std::size_t>(samples));
    opj_cparameters_t parameters{};
    opj_set_default_encoder_parameters(&parameters);

    for (auto& p : component_params) {
      std::memset(&p, 0, sizeof(p));
      p.dx = static_cast<OPJ_UINT32>(parameters.subsampling_dx);
      p.dy = static_cast<OPJ_UINT32>(parameters.subsampling_dy);
      p.w = static_cast<OPJ_UINT32>(cols);
      p.h = static_cast<OPJ_UINT32>(rows);
      p.x0 = 0;
      p.y0 = 0;
      p.prec = static_cast<OPJ_UINT32>(request->frame.bits_stored);
      p.sgnd = static_cast<OPJ_UINT32>(request->frame.pixel_representation);
    }

    opj_image_ptr image(opj_image_create(static_cast<OPJ_UINT32>(samples),
        component_params.data(), resolve_color_space(static_cast<std::size_t>(samples))));
    if (!image) {
      return fail_detail(c, PIXEL_CODEC_ERR_FAILED, "encode_frame",
          "opj_image_create failed");
    }

    image->x0 = 0;
    image->y0 = 0;
    image->x1 = static_cast<OPJ_UINT32>(cols);
    image->y1 = static_cast<OPJ_UINT32>(rows);

    const uint8_t* source = request->source.source_buffer.data;
    const std::size_t row_stride_sz = static_cast<std::size_t>(row_stride);
    const std::size_t plane_stride_sz = static_cast<std::size_t>(plane_stride);
    const std::size_t cols_sz = static_cast<std::size_t>(cols);
    const std::size_t rows_sz = static_cast<std::size_t>(rows);
    const std::size_t samples_sz = static_cast<std::size_t>(samples);
    const std::size_t sample_bytes = static_cast<std::size_t>(source_dtype.bytes);
    const std::size_t pixel_stride = samples_sz * sample_bytes;
    const bool source_is_signed = request->frame.pixel_representation == 1;

    for (std::size_t comp = 0; comp < samples_sz; ++comp) {
      auto& opj_comp = image->comps[comp];
      if (opj_comp.data == nullptr) {
        return fail_detail(c, PIXEL_CODEC_ERR_FAILED, "encode_frame",
            "OpenJPEG component buffer is null");
      }

      for (std::size_t r = 0; r < rows_sz; ++r) {
        const uint8_t* row_base = nullptr;
        if (source_planar) {
          row_base = source + comp * plane_stride_sz + r * row_stride_sz;
        } else {
          row_base = source + r * row_stride_sz;
        }

        for (std::size_t col = 0; col < cols_sz; ++col) {
          const uint8_t* sample_ptr = nullptr;
          if (source_planar) {
            sample_ptr = row_base + col * sample_bytes;
          } else {
            sample_ptr = row_base + col * pixel_stride + comp * sample_bytes;
          }

          char reason[256];
          int32_t sample = 0;
          if (!load_integral_sample_from_le(sample_ptr,
                  static_cast<uint32_t>(sample_bytes), source_is_signed,
                  request->frame.bits_stored, &sample, reason, sizeof(reason))) {
            return fail_detail(c, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "encode_frame", reason);
          }
          opj_comp.data[r * cols_sz + col] = sample;
        }
      }
    }

    const bool lossless =
        request->frame.codec_profile_code == PIXEL_CODEC_PROFILE_JPEG2000_LOSSLESS_V2;

    parameters.cod_format = 0;  // J2K codestream
    parameters.tcp_numlayers = 1;
    parameters.cp_disto_alloc = 1;
    parameters.tcp_rates[0] = 0.0f;
    parameters.irreversible = lossless ? 0 : 1;
    parameters.tcp_mct = request->frame.use_multicomponent_transform ? 1 : 0;

    if (!lossless) {
      const pixel_error_code_v2 lossy_ec =
          configure_j2k_lossy_parameters(c, &parameters, request);
      if (lossy_ec != PIXEL_CODEC_ERR_OK) {
        return lossy_ec;
      }
    }

    parameters.numresolution = std::min(
        parameters.numresolution,
        max_num_resolutions_for_image(static_cast<std::size_t>(rows), static_cast<std::size_t>(cols)));
    if (parameters.numresolution < 1) {
      parameters.numresolution = 1;
    }

    opj_codec_ptr codec(opj_create_compress(OPJ_CODEC_J2K));
    if (!codec) {
      return fail_detail(c, PIXEL_CODEC_ERR_FAILED, "encode_frame",
          "opj_create_compress failed");
    }

    OpenJpegLogSink sink{};
    opj_set_warning_handler(codec.get(), opj_warning_handler, &sink);
    opj_set_error_handler(codec.get(), opj_error_handler, &sink);

    if (!opj_setup_encoder(codec.get(), &parameters, image.get())) {
      return fail_detail(c, PIXEL_CODEC_ERR_FAILED, "encode_frame",
          openjpeg_failure_message(sink, "opj_setup_encoder failed").c_str());
    }

    if (thread_count > 0 &&
        !opj_codec_set_threads(codec.get(), static_cast<int>(thread_count))) {
      return fail_detail(c, PIXEL_CODEC_ERR_FAILED, "encode_frame",
          "opj_codec_set_threads failed");
    }

    EncodeStreamContext output_context{};
    opj_stream_ptr stream = create_encode_stream(&output_context);
    if (!stream) {
      return fail_detail(c, PIXEL_CODEC_ERR_FAILED, "encode_frame",
          "OpenJPEG output stream creation failed");
    }

    if (!opj_start_compress(codec.get(), image.get(), stream.get())) {
      return fail_detail(c, PIXEL_CODEC_ERR_FAILED, "encode_frame",
          openjpeg_failure_message(sink, "opj_start_compress failed").c_str());
    }
    if (!opj_encode(codec.get(), stream.get())) {
      return fail_detail(c, PIXEL_CODEC_ERR_FAILED, "encode_frame",
          openjpeg_failure_message(sink, "opj_encode failed").c_str());
    }
    if (!opj_end_compress(codec.get(), stream.get())) {
      return fail_detail(c, PIXEL_CODEC_ERR_FAILED, "encode_frame",
          openjpeg_failure_message(sink, "opj_end_compress failed").c_str());
    }

    if (output_context.position < output_context.bytes.size()) {
      output_context.bytes.resize(output_context.position);
    }
    if (output_context.bytes.empty()) {
      return fail_detail(c, PIXEL_CODEC_ERR_FAILED, "encode_frame",
          "OpenJPEG produced empty codestream");
    }

    c->encoded_buffer = std::move(output_context.bytes);
    auto* mutable_request = const_cast<pixel_encoder_request_v2*>(request);
    mutable_request->output.encoded_size = c->encoded_buffer.size();
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

pixel_error_code_v2 encoder_encode_frame(
    void* ctx, const pixel_encoder_request_v2* request) {
  auto* c = static_cast<EncoderCtx*>(ctx);
  const pixel_error_code_v2 encode_ec =
      encoder_encode_frame_to_context_buffer(ctx, request);
  if (encode_ec != PIXEL_CODEC_ERR_OK) {
    return encode_ec;
  }
  if (c == nullptr || request == nullptr) {
    return PIXEL_CODEC_ERR_INVALID_ARGUMENT;
  }
  if (request->output.encoded_buffer.data == nullptr ||
      request->output.encoded_buffer.size < c->encoded_buffer.size()) {
    return fail_detail(c, PIXEL_CODEC_ERR_OUTPUT_TOO_SMALL, "encode_frame",
        "output buffer too small");
  }
  std::memcpy(request->output.encoded_buffer.data,
      c->encoded_buffer.data(), c->encoded_buffer.size());
  clear_detail(c);
  return PIXEL_CODEC_ERR_OK;
}

pixel_error_code_v2 encoder_get_encoded_buffer(
    const void* ctx, pixel_const_buffer_v2* out_encoded_buffer) {
  auto* c = static_cast<const EncoderCtx*>(ctx);
  if (c == nullptr || out_encoded_buffer == nullptr) {
    return PIXEL_CODEC_ERR_INVALID_ARGUMENT;
  }
  if (!c->configured || c->encoded_buffer.empty()) {
    return fail_detail(const_cast<EncoderCtx*>(c), PIXEL_CODEC_ERR_FAILED,
        "encode_frame", "encoded buffer is not available");
  }
  out_encoded_buffer->data = c->encoded_buffer.data();
  out_encoded_buffer->size = static_cast<uint64_t>(c->encoded_buffer.size());
  return PIXEL_CODEC_ERR_OK;
}

}  // namespace pixel::openjpeg_codec_v2
