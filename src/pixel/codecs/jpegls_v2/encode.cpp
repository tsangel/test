#include <charls/charls.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <string>
#include <vector>

#include "internal.hpp"

namespace pixel::jpegls_codec_v2 {

namespace {

pixel_error_code_v2 validate_encoder_request(
    EncoderCtx* ctx, const pixel_encoder_request_v2* request,
    DtypeInfo* out_source_dtype,
    uint64_t* out_source_row_stride,
    uint64_t* out_source_plane_stride,
    uint64_t* out_source_frame_bytes,
    bool* out_source_planar,
    uint32_t* out_packed_sample_bytes,
    uint64_t* out_packed_row_bytes,
    uint64_t* out_packed_plane_bytes,
    uint64_t* out_packed_frame_bytes) {
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

  if (request->frame.bits_allocated > static_cast<int32_t>(source_dtype.bytes * 8u)) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "bits_allocated exceeds source_dtype width");
  }

  if (request->frame.pixel_representation != 0 &&
      request->frame.pixel_representation != 1) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "pixel_representation must be 0 or 1");
  }

  const bool source_is_signed = request->frame.pixel_representation == 1;
  if (source_is_signed != source_dtype.is_signed) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "pixel_representation does not match source_dtype signedness");
  }

  if (request->frame.use_multicomponent_transform != 0) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_UNSUPPORTED, "validate",
        "use_multicomponent_transform is not supported for JPEG-LS");
  }

  const uint64_t rows = static_cast<uint64_t>(request->frame.rows);
  const uint64_t cols = static_cast<uint64_t>(request->frame.cols);
  const bool source_planar = is_planar_code(request->frame.source_planar);

  const uint64_t source_row_components = source_planar ? 1 : samples;
  uint64_t source_min_row_bytes = 0;
  if (!mul_u64(cols, source_row_components, &source_min_row_bytes) ||
      !mul_u64(source_min_row_bytes, source_dtype.bytes, &source_min_row_bytes)) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "source row byte size overflow");
  }

  uint64_t source_row_stride = request->frame.source_row_stride;
  if (source_row_stride == 0) {
    source_row_stride = source_min_row_bytes;
  }
  if (source_row_stride < source_min_row_bytes) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "source_row_stride is too small");
  }

  uint64_t source_plane_stride = 0;
  if (source_planar) {
    uint64_t source_min_plane_stride = 0;
    if (!mul_u64(source_row_stride, rows, &source_min_plane_stride)) {
      return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
          "source plane byte size overflow");
    }
    source_plane_stride = request->frame.source_plane_stride;
    if (source_plane_stride == 0) {
      source_plane_stride = source_min_plane_stride;
    }
    if (source_plane_stride < source_min_plane_stride) {
      return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
          "source_plane_stride is too small");
    }
  }

  uint64_t source_min_frame_bytes = 0;
  if (source_planar) {
    if (!mul_u64(source_plane_stride, samples, &source_min_frame_bytes)) {
      return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
          "source frame byte size overflow");
    }
  } else {
    if (!mul_u64(source_row_stride, rows, &source_min_frame_bytes)) {
      return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
          "source frame byte size overflow");
    }
  }

  uint64_t source_frame_bytes = request->frame.source_frame_size_bytes;
  if (source_frame_bytes == 0) {
    source_frame_bytes = source_min_frame_bytes;
  }
  if (source_frame_bytes < source_min_frame_bytes ||
      request->source.source_buffer.size < source_frame_bytes) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "source buffer is too small for frame layout");
  }

  const uint32_t packed_sample_bytes =
      static_cast<uint32_t>((request->frame.bits_stored + 7) / 8);
  if (packed_sample_bytes == 0 || packed_sample_bytes > 2) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "packed sample bytes must be 1 or 2");
  }

  const uint64_t packed_row_components = source_planar ? 1 : samples;
  uint64_t packed_row_bytes = 0;
  if (!mul_u64(cols, packed_row_components, &packed_row_bytes) ||
      !mul_u64(packed_row_bytes, packed_sample_bytes, &packed_row_bytes)) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "packed row byte size overflow");
  }

  uint64_t packed_plane_bytes = 0;
  if (!mul_u64(packed_row_bytes, rows, &packed_plane_bytes)) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "packed plane byte size overflow");
  }

  uint64_t packed_frame_bytes = packed_plane_bytes;
  if (source_planar && samples > 1) {
    if (!mul_u64(packed_plane_bytes, samples, &packed_frame_bytes)) {
      return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
          "packed frame byte size overflow");
    }
  }

  if (packed_row_bytes > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "packed row stride exceeds uint32 range");
  }

  *out_source_dtype = source_dtype;
  *out_source_row_stride = source_row_stride;
  *out_source_plane_stride = source_plane_stride;
  *out_source_frame_bytes = source_frame_bytes;
  *out_source_planar = source_planar;
  *out_packed_sample_bytes = packed_sample_bytes;
  *out_packed_row_bytes = packed_row_bytes;
  *out_packed_plane_bytes = packed_plane_bytes;
  *out_packed_frame_bytes = packed_frame_bytes;
  return PIXEL_CODEC_ERR_OK;
}

}  // namespace

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
    uint64_t source_row_stride = 0;
    uint64_t source_plane_stride = 0;
    uint64_t source_frame_bytes = 0;
    bool source_planar = false;
    uint32_t packed_sample_bytes = 0;
    uint64_t packed_row_bytes = 0;
    uint64_t packed_plane_bytes = 0;
    uint64_t packed_frame_bytes = 0;

    const pixel_error_code_v2 valid_ec = validate_encoder_request(c, request,
        &source_dtype,
        &source_row_stride,
        &source_plane_stride,
        &source_frame_bytes,
        &source_planar,
        &packed_sample_bytes,
        &packed_row_bytes,
        &packed_plane_bytes,
        &packed_frame_bytes);
    if (valid_ec != PIXEL_CODEC_ERR_OK) {
      return valid_ec;
    }

    const uint64_t rows = static_cast<uint64_t>(request->frame.rows);
    const uint64_t cols = static_cast<uint64_t>(request->frame.cols);
    const uint64_t samples = static_cast<uint64_t>(request->frame.samples_per_pixel);

    const std::size_t rows_sz = static_cast<std::size_t>(rows);
    const std::size_t cols_sz = static_cast<std::size_t>(cols);
    const std::size_t samples_sz = static_cast<std::size_t>(samples);
    const std::size_t source_row_stride_sz = static_cast<std::size_t>(source_row_stride);
    const std::size_t source_plane_stride_sz = static_cast<std::size_t>(source_plane_stride);
    const std::size_t source_bytes_per_sample_sz = static_cast<std::size_t>(source_dtype.bytes);
    const std::size_t packed_sample_bytes_sz = static_cast<std::size_t>(packed_sample_bytes);
    const std::size_t packed_row_bytes_sz = static_cast<std::size_t>(packed_row_bytes);
    const std::size_t packed_plane_bytes_sz = static_cast<std::size_t>(packed_plane_bytes);

    const uint8_t* source = request->source.source_buffer.data;
    const bool source_is_signed = request->frame.pixel_representation == 1;

    std::vector<uint8_t> packed(static_cast<std::size_t>(packed_frame_bytes), uint8_t{0});

    if (source_planar) {
      for (std::size_t comp = 0; comp < samples_sz; ++comp) {
        uint8_t* packed_plane = packed.data() + comp * packed_plane_bytes_sz;
        for (std::size_t r = 0; r < rows_sz; ++r) {
          const uint8_t* src_row = source + comp * source_plane_stride_sz +
              r * source_row_stride_sz;
          uint8_t* dst_row = packed_plane + r * packed_row_bytes_sz;
          for (std::size_t cidx = 0; cidx < cols_sz; ++cidx) {
            const uint8_t* src_ptr = src_row + cidx * source_bytes_per_sample_sz;
            char reason[256];
            int32_t sample = 0;
            if (!load_integral_sample(src_ptr,
                    static_cast<uint32_t>(source_bytes_per_sample_sz),
                    source_is_signed, request->frame.bits_stored,
                    &sample, reason, sizeof(reason))) {
              return fail_detail(c, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "encode_frame", reason);
            }

            const uint32_t raw = static_cast<uint32_t>(sample) &
                bit_mask_u32(request->frame.bits_stored);
            uint8_t* dst_ptr = dst_row + cidx * packed_sample_bytes_sz;
            if (!store_raw_sample(dst_ptr, packed_sample_bytes, raw)) {
              return fail_detail(c, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "encode_frame",
                  "failed to write packed sample");
            }
          }
        }
      }
    } else {
      const std::size_t source_pixel_stride = samples_sz * source_bytes_per_sample_sz;
      const std::size_t packed_pixel_stride = samples_sz * packed_sample_bytes_sz;
      for (std::size_t r = 0; r < rows_sz; ++r) {
        const uint8_t* src_row = source + r * source_row_stride_sz;
        uint8_t* dst_row = packed.data() + r * packed_row_bytes_sz;
        for (std::size_t cidx = 0; cidx < cols_sz; ++cidx) {
          const uint8_t* src_pixel = src_row + cidx * source_pixel_stride;
          uint8_t* dst_pixel = dst_row + cidx * packed_pixel_stride;
          for (std::size_t comp = 0; comp < samples_sz; ++comp) {
            const uint8_t* src_ptr = src_pixel + comp * source_bytes_per_sample_sz;
            char reason[256];
            int32_t sample = 0;
            if (!load_integral_sample(src_ptr,
                    static_cast<uint32_t>(source_bytes_per_sample_sz),
                    source_is_signed, request->frame.bits_stored,
                    &sample, reason, sizeof(reason))) {
              return fail_detail(c, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "encode_frame", reason);
            }

            const uint32_t raw = static_cast<uint32_t>(sample) &
                bit_mask_u32(request->frame.bits_stored);
            uint8_t* dst_ptr = dst_pixel + comp * packed_sample_bytes_sz;
            if (!store_raw_sample(dst_ptr, packed_sample_bytes, raw)) {
              return fail_detail(c, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "encode_frame",
                  "failed to write packed sample");
            }
          }
        }
      }
    }

    charls::jpegls_encoder encoder{};
    encoder.frame_info(charls::frame_info{
        static_cast<uint32_t>(cols),
        static_cast<uint32_t>(rows),
        request->frame.bits_stored,
        static_cast<int32_t>(samples)});
    encoder.near_lossless(c->near_lossless_error);

    if (samples > 1) {
      encoder.interleave_mode(
          source_planar ? charls::interleave_mode::none : charls::interleave_mode::sample);
    }

    c->encoded_buffer.assign(encoder.estimated_destination_size(), uint8_t{0});
    encoder.destination(c->encoded_buffer);

    const std::size_t bytes_written = encoder.encode(
        packed.data(), packed.size(), static_cast<uint32_t>(packed_row_bytes));
    c->encoded_buffer.resize(bytes_written);

    if (c->encoded_buffer.empty()) {
      return fail_detail(c, PIXEL_CODEC_ERR_FAILED, "encode_frame",
          "CharLS produced empty codestream");
    }

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

}  // namespace pixel::jpegls_codec_v2
