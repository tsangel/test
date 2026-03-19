#include <charls/charls.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

#include "../common/decoded_bytes_writeback.hpp"
#include "internal.hpp"

namespace pixel::jpegls_codec {

namespace {

pixel_error_code validate_decoder_request(
    DecoderCtx* ctx, const pixel_decoder_request* request,
    DtypeInfo* out_source_dtype, DtypeInfo* out_dst_dtype,
    uint64_t* out_row_stride, uint64_t* out_frame_stride,
    bool* out_output_planar) {
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
  return PIXEL_CODEC_ERR_OK;
}

pixel_error_code write_decoded_pixels(DecoderCtx* ctx,
    const pixel_decoder_request* request,
    const uint8_t* decoded,
    uint32_t source_sample_bytes,
    int source_bits,
    bool source_is_signed,
    bool source_is_planar,
    uint64_t source_row_bytes,
    uint64_t row_stride,
    bool output_planar) {
  if (decoded == nullptr) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_FAILED, "decode_frame",
        "decoded source buffer is null");
  }

  switch (source_sample_bytes) {
  case 1:
    if (source_is_signed) {
      if (::pixel::codec_common::write_decoded_native_bytes_to_dst<1, true>(
              request, decoded, static_cast<std::size_t>(source_row_bytes),
              source_is_planar, row_stride, output_planar, source_bits)) {
        return PIXEL_CODEC_ERR_OK;
      }
    } else {
      if (::pixel::codec_common::write_decoded_native_bytes_to_dst<1, false>(
              request, decoded, static_cast<std::size_t>(source_row_bytes),
              source_is_planar, row_stride, output_planar, source_bits)) {
        return PIXEL_CODEC_ERR_OK;
      }
    }
    break;
  case 2:
    if (source_is_signed) {
      if (::pixel::codec_common::write_decoded_native_bytes_to_dst<2, true>(
              request, decoded, static_cast<std::size_t>(source_row_bytes),
              source_is_planar, row_stride, output_planar, source_bits)) {
        return PIXEL_CODEC_ERR_OK;
      }
    } else {
      if (::pixel::codec_common::write_decoded_native_bytes_to_dst<2, false>(
              request, decoded, static_cast<std::size_t>(source_row_bytes),
              source_is_planar, row_stride, output_planar, source_bits)) {
        return PIXEL_CODEC_ERR_OK;
      }
    }
    break;
  default:
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "decode_frame",
        "unsupported decoded sample width");
  }
  return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "decode_frame",
      "unsupported destination dtype");
}

}  // namespace

pixel_error_code decoder_decode_frame(
    void* ctx, const pixel_decoder_request* request) {
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

    const pixel_error_code valid_ec = validate_decoder_request(c, request,
        &source_dtype, &dst_dtype,
        &row_stride, &frame_stride,
        &output_planar);
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

    const bool matching_output_dtype = !dst_dtype.is_float &&
        dst_dtype.bytes == source_dtype.bytes &&
        dst_dtype.is_signed == source_dtype.is_signed;
    const bool can_decode_directly =
        matching_output_dtype &&
        (samples == 1 || source_planar == output_planar);
    if (can_decode_directly) {
      if (row_stride > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
        return fail_detail(c, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "decode_frame",
            "destination row stride exceeds uint32 range");
      }

      decoder.decode(request->output.dst,
          static_cast<std::size_t>(frame_stride),
          static_cast<uint32_t>(row_stride));
      clear_detail(c);
      return PIXEL_CODEC_ERR_OK;
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

    const pixel_error_code write_ec = write_decoded_pixels(c, request,
        decoded.data(), decoded_sample_bytes, frame_info.bits_per_sample,
        source_dtype.is_signed, source_planar, source_row_bytes,
        row_stride, output_planar);
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

}  // namespace pixel::jpegls_codec
