#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

#include "internal.hpp"

namespace pixel::rle_codec_v2 {

namespace {

void store_le32(uint8_t* dst, uint32_t value) {
  dst[0] = static_cast<uint8_t>(value & 0xFFu);
  dst[1] = static_cast<uint8_t>((value >> 8u) & 0xFFu);
  dst[2] = static_cast<uint8_t>((value >> 16u) & 0xFFu);
  dst[3] = static_cast<uint8_t>((value >> 24u) & 0xFFu);
}

void encode_packbits_segment_append(
    const uint8_t* source, std::size_t size, std::vector<uint8_t>* out_encoded) {
  if (out_encoded == nullptr) {
    return;
  }
  std::size_t i = 0;
  while (i < size) {
    std::size_t repeat_count = 1;
    while (i + repeat_count < size && repeat_count < 128u &&
           source[i + repeat_count] == source[i]) {
      ++repeat_count;
    }

    if (repeat_count >= 2) {
      const int8_t control =
          static_cast<int8_t>(1 - static_cast<int>(repeat_count));
      out_encoded->push_back(static_cast<uint8_t>(control));
      out_encoded->push_back(source[i]);
      i += repeat_count;
      continue;
    }

    const std::size_t literal_begin = i;
    std::size_t literal_count = 1;
    ++i;
    while (i < size && literal_count < 128u) {
      repeat_count = 1;
      while (i + repeat_count < size && repeat_count < 128u &&
             source[i + repeat_count] == source[i]) {
        ++repeat_count;
      }
      if (repeat_count >= 2) {
        break;
      }
      ++i;
      ++literal_count;
    }

    out_encoded->push_back(static_cast<uint8_t>(literal_count - 1u));
    out_encoded->insert(out_encoded->end(), source + literal_begin,
        source + literal_begin + literal_count);
  }
}

pixel_error_code_v2 build_rle_codestream(EncoderCtx* ctx,
    const uint8_t* source, std::size_t rows, std::size_t cols,
    std::size_t samples_per_pixel, std::size_t bytes_per_sample,
    bool source_planar, std::size_t source_row_stride, std::size_t source_plane_stride,
    std::vector<uint8_t>* out_encoded) {
  if (out_encoded == nullptr) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "encode_frame",
        "encoded output pointer is null");
  }
  if (source == nullptr) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "encode_frame",
        "source pointer is null");
  }

  uint64_t segment_count_u64 = 0;
  if (!mul_u64(static_cast<uint64_t>(samples_per_pixel),
          static_cast<uint64_t>(bytes_per_sample), &segment_count_u64)) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "encode_frame",
        "RLE segment count overflow");
  }
  std::size_t segment_count = 0;
  if (!u64_to_size(segment_count_u64, &segment_count) ||
      segment_count == 0 || segment_count > 15u) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_UNSUPPORTED, "encode_frame",
        "unsupported RLE segment layout");
  }

  uint64_t pixels_u64 = 0;
  if (!mul_u64(static_cast<uint64_t>(rows), static_cast<uint64_t>(cols), &pixels_u64)) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "encode_frame",
        "RLE pixel count overflow");
  }
  std::size_t pixels = 0;
  if (!u64_to_size(pixels_u64, &pixels)) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "encode_frame",
        "RLE pixel count conversion overflow");
  }

  std::vector<uint8_t> byte_plane(pixels, uint8_t{0});
  std::vector<uint8_t> encoded(64u, uint8_t{0});
  store_le32(encoded.data(), static_cast<uint32_t>(segment_count));

  const uint64_t segment_slack_u64 = pixels_u64 / 128u + 16u;
  uint64_t segment_upper_bound_u64 = 0;
  if (pixels_u64 > (std::numeric_limits<uint64_t>::max)() - segment_slack_u64) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "encode_frame",
        "RLE codestream reserve bound overflow");
  }
  segment_upper_bound_u64 = pixels_u64 + segment_slack_u64;
  uint64_t total_reserve_u64 = 0;
  std::size_t total_reserve = 0;
  if (mul_u64(segment_count_u64, segment_upper_bound_u64, &total_reserve_u64) &&
      total_reserve_u64 <= (std::numeric_limits<uint64_t>::max)() - 64u &&
      u64_to_size(total_reserve_u64 + 64u, &total_reserve)) {
    encoded.reserve(total_reserve);
  }

  std::size_t interleaved_pixel_stride = 0;
  if (!source_planar) {
    uint64_t interleaved_stride_u64 = 0;
    if (!mul_u64(static_cast<uint64_t>(samples_per_pixel),
            static_cast<uint64_t>(bytes_per_sample), &interleaved_stride_u64) ||
        !u64_to_size(interleaved_stride_u64, &interleaved_pixel_stride)) {
      return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "encode_frame",
          "interleaved pixel stride overflow");
    }
  }

  const std::size_t max_u32 = static_cast<std::size_t>(std::numeric_limits<uint32_t>::max());
  for (std::size_t sample = 0; sample < samples_per_pixel; ++sample) {
    for (std::size_t byte_plane_index = 0; byte_plane_index < bytes_per_sample;
         ++byte_plane_index) {
      const std::size_t component_byte_index = bytes_per_sample - 1u - byte_plane_index;
      for (std::size_t row = 0; row < rows; ++row) {
        for (std::size_t col = 0; col < cols; ++col) {
          const uint8_t* src_sample = nullptr;
          if (source_planar) {
            const uint8_t* src_plane = source + sample * source_plane_stride;
            const uint8_t* src_row = src_plane + row * source_row_stride;
            src_sample = src_row + col * bytes_per_sample;
          } else {
            const uint8_t* src_row = source + row * source_row_stride;
            src_sample = src_row + col * interleaved_pixel_stride +
                sample * bytes_per_sample;
          }
          byte_plane[row * cols + col] = src_sample[component_byte_index];
        }
      }

      const std::size_t segment_index = sample * bytes_per_sample + byte_plane_index;
      if (encoded.size() > max_u32) {
        return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "encode_frame",
            "RLE codestream exceeds 32-bit offset range");
      }
      store_le32(encoded.data() + 4u + segment_index * sizeof(uint32_t),
          static_cast<uint32_t>(encoded.size()));
      encode_packbits_segment_append(byte_plane.data(), byte_plane.size(), &encoded);
      if (encoded.size() > max_u32) {
        return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "encode_frame",
            "RLE codestream exceeds 32-bit offset range");
      }
    }
  }

  *out_encoded = std::move(encoded);
  return PIXEL_CODEC_ERR_OK;
}

pixel_error_code_v2 validate_encoder_request(EncoderCtx* ctx,
    const pixel_encoder_request_v2* request, DtypeInfo* out_source_dtype,
    bool* out_source_planar, uint64_t* out_source_row_stride,
    uint64_t* out_source_plane_stride, uint64_t* out_source_frame_size) {
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
        "unsupported encoder codec_profile_code=%u",
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
  if (!is_valid_planar_code(request->frame.source_planar)) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "unsupported source_planar code");
  }
  if (request->frame.use_multicomponent_transform != 0) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_UNSUPPORTED, "validate",
        "RLE encoder does not use multicomponent transform");
  }

  DtypeInfo source_dtype{};
  if (!dtype_info_from_code(request->frame.source_dtype, &source_dtype) ||
      source_dtype.is_float || source_dtype.bytes == 0 || source_dtype.bytes > 4u) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "source_dtype must be integral 8/16/32-bit type");
  }
  if (request->frame.bits_allocated <= 0 ||
      request->frame.bits_allocated > static_cast<int32_t>(source_dtype.bytes * 8u)) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "bits_allocated must be in [1, source dtype width]");
  }
  if (request->frame.bits_stored <= 0 ||
      request->frame.bits_stored > request->frame.bits_allocated) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "bits_stored must be in [1, bits_allocated]");
  }

  const uint64_t rows = static_cast<uint64_t>(request->frame.rows);
  const uint64_t cols = static_cast<uint64_t>(request->frame.cols);
  const uint64_t samples = static_cast<uint64_t>(request->frame.samples_per_pixel);
  const bool source_planar = is_planar_code(request->frame.source_planar);

  uint64_t segment_count_u64 = 0;
  if (!mul_u64(samples, source_dtype.bytes, &segment_count_u64) ||
      segment_count_u64 == 0 || segment_count_u64 > 15u) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_UNSUPPORTED, "validate",
        "unsupported RLE segment layout");
  }

  const uint64_t source_row_components = source_planar ? 1u : samples;
  uint64_t row_payload = 0;
  if (!mul_u64(cols, source_row_components, &row_payload) ||
      !mul_u64(row_payload, source_dtype.bytes, &row_payload)) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "source row payload byte size overflow");
  }

  uint64_t source_row_stride = request->frame.source_row_stride;
  if (source_row_stride == 0) {
    source_row_stride = row_payload;
  }
  if (source_row_stride < row_payload) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "source_row_stride is too small");
  }

  uint64_t min_plane_stride = 0;
  if (!mul_u64(source_row_stride, rows, &min_plane_stride)) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "source plane stride overflow");
  }

  uint64_t source_plane_stride = request->frame.source_plane_stride;
  if (source_planar && samples > 1u) {
    if (source_plane_stride == 0) {
      source_plane_stride = min_plane_stride;
    }
    if (source_plane_stride < min_plane_stride) {
      return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
          "source_plane_stride is too small");
    }
  } else {
    source_plane_stride = min_plane_stride;
  }

  uint64_t min_frame_size = min_plane_stride;
  if (source_planar && samples > 1u) {
    if (!mul_u64(source_plane_stride, samples, &min_frame_size)) {
      return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
          "source frame byte size overflow");
    }
  }

  uint64_t source_frame_size = request->frame.source_frame_size_bytes;
  if (source_frame_size == 0) {
    source_frame_size = min_frame_size;
  }
  if (source_frame_size < min_frame_size) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "source_frame_size_bytes is too small");
  }
  if (request->source.source_buffer.size < source_frame_size) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "source buffer is shorter than source_frame_size_bytes");
  }

  *out_source_dtype = source_dtype;
  *out_source_planar = source_planar;
  *out_source_row_stride = source_row_stride;
  *out_source_plane_stride = source_plane_stride;
  *out_source_frame_size = source_frame_size;
  return PIXEL_CODEC_ERR_OK;
}

}  // namespace

pixel_error_code_v2 encoder_encode_frame_to_context_buffer(
    void* ctx, const pixel_encoder_request_v2* request) {
  auto* c = static_cast<EncoderCtx*>(ctx);
  if (c == nullptr) {
    return PIXEL_CODEC_ERR_INVALID_ARGUMENT;
  }
  if (!c->configured) {
    return fail_detail(c, PIXEL_CODEC_ERR_FAILED, "encode_frame",
        "configure must be called before encode_frame");
  }
  if (request == nullptr) {
    return fail_detail(c, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "encoder request is null");
  }

  DtypeInfo source_dtype{};
  bool source_planar = false;
  uint64_t source_row_stride_u64 = 0;
  uint64_t source_plane_stride_u64 = 0;
  uint64_t source_frame_size_u64 = 0;
  const pixel_error_code_v2 validate_ec = validate_encoder_request(c, request,
      &source_dtype, &source_planar, &source_row_stride_u64,
      &source_plane_stride_u64, &source_frame_size_u64);
  if (validate_ec != PIXEL_CODEC_ERR_OK) {
    return validate_ec;
  }

  std::size_t rows = 0;
  std::size_t cols = 0;
  std::size_t samples = 0;
  std::size_t sample_bytes = 0;
  std::size_t source_row_stride = 0;
  std::size_t source_plane_stride = 0;
  if (!u64_to_size(static_cast<uint64_t>(request->frame.rows), &rows) ||
      !u64_to_size(static_cast<uint64_t>(request->frame.cols), &cols) ||
      !u64_to_size(static_cast<uint64_t>(request->frame.samples_per_pixel), &samples) ||
      !u64_to_size(source_dtype.bytes, &sample_bytes) ||
      !u64_to_size(source_row_stride_u64, &source_row_stride) ||
      !u64_to_size(source_plane_stride_u64, &source_plane_stride)) {
    return fail_detail(c, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "size conversion overflow");
  }

  c->encoded_buffer.clear();
  const pixel_error_code_v2 encode_ec = build_rle_codestream(c,
      request->source.source_buffer.data, rows, cols, samples, sample_bytes,
      source_planar, source_row_stride, source_plane_stride, &c->encoded_buffer);
  if (encode_ec != PIXEL_CODEC_ERR_OK) {
    return encode_ec;
  }

  auto* mutable_request = const_cast<pixel_encoder_request_v2*>(request);
  mutable_request->output.encoded_size = static_cast<uint64_t>(c->encoded_buffer.size());
  clear_detail(c);
  return PIXEL_CODEC_ERR_OK;
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

}  // namespace pixel::rle_codec_v2
