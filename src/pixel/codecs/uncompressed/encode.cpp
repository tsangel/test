#include <cstddef>
#include <cstdint>
#include <cstring>

#include "support.hpp"

namespace pixel::core {

namespace {

pixel_error_code validate_encode_request(ErrorState* state,
    const pixel_encoder_request* request, DtypeInfo* out_source_dtype,
    bool* out_source_planar, uint64_t* out_source_row_stride,
    uint64_t* out_source_plane_stride, uint64_t* out_source_frame_size,
    uint64_t* out_encoded_frame_size) {
  if (request == nullptr) {
    return fail_detail(state, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "encoder request is null");
  }
  if (request->struct_size < sizeof(pixel_encoder_request) ||
      request->source.struct_size < sizeof(pixel_encoder_source) ||
      request->frame.struct_size < sizeof(pixel_encoder_frame_info) ||
      request->output.struct_size < sizeof(pixel_encoder_output)) {
    return fail_detail(state, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "encoder request struct_size is too small");
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

  DtypeInfo source_dtype{};
  if (!dtype_info_from_code(request->frame.source_dtype, &source_dtype)) {
    return fail_detail(state, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "unsupported source_dtype code");
  }
  if (request->frame.bits_allocated <= 0 ||
      request->frame.bits_allocated > static_cast<int32_t>(source_dtype.bytes * 8u)) {
    return fail_detail(state, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "bits_allocated must be in [1, source dtype width]");
  }
  if (request->frame.bits_stored <= 0 ||
      request->frame.bits_stored > request->frame.bits_allocated) {
    return fail_detail(state, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "bits_stored must be in [1, bits_allocated]");
  }
  if (request->frame.use_multicomponent_transform != 0) {
    return fail_detail(state, PIXEL_CODEC_ERR_UNSUPPORTED, "validate",
        "uncompressed core path does not use multicomponent transform");
  }

  const uint64_t rows = static_cast<uint64_t>(request->frame.rows);
  const uint64_t cols = static_cast<uint64_t>(request->frame.cols);
  const uint64_t samples = static_cast<uint64_t>(request->frame.samples_per_pixel);
  const bool source_planar = is_planar_code(request->frame.source_planar);

  const uint64_t source_row_components = source_planar ? 1 : samples;
  uint64_t row_payload = 0;
  if (!mul_u64(cols, source_row_components, &row_payload) ||
      !mul_u64(row_payload, source_dtype.bytes, &row_payload)) {
    return fail_detail(state, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "source row payload byte size overflow");
  }

  uint64_t source_row_stride = request->frame.source_row_stride;
  if (source_row_stride == 0) {
    source_row_stride = row_payload;
  }
  if (source_row_stride < row_payload) {
    return fail_detail(state, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "source_row_stride is too small");
  }

  uint64_t min_plane_stride = 0;
  if (!mul_u64(source_row_stride, rows, &min_plane_stride)) {
    return fail_detail(state, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "source plane stride overflow");
  }

  uint64_t source_plane_stride = request->frame.source_plane_stride;
  if (source_planar && samples > 1) {
    if (source_plane_stride == 0) {
      source_plane_stride = min_plane_stride;
    }
    if (source_plane_stride < min_plane_stride) {
      return fail_detail(state, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
          "source_plane_stride is too small");
    }
  } else {
    source_plane_stride = min_plane_stride;
  }

  uint64_t min_frame_size = min_plane_stride;
  if (source_planar && samples > 1) {
    if (!mul_u64(source_plane_stride, samples, &min_frame_size)) {
      return fail_detail(state, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
          "source frame byte size overflow");
    }
  }

  uint64_t source_frame_size = request->frame.source_frame_size_bytes;
  if (source_frame_size == 0) {
    source_frame_size = min_frame_size;
  }
  if (source_frame_size < min_frame_size) {
    return fail_detail(state, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "source_frame_size_bytes is too small");
  }
  if (request->source.source_buffer.size < source_frame_size) {
    return fail_detail(state, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "source buffer is shorter than source_frame_size_bytes");
  }

  uint64_t encoded_plane_bytes = 0;
  if (!mul_u64(row_payload, rows, &encoded_plane_bytes)) {
    return fail_detail(state, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "encoded plane byte size overflow");
  }

  uint64_t encoded_frame_size = encoded_plane_bytes;
  if (source_planar && samples > 1) {
    if (!mul_u64(encoded_plane_bytes, samples, &encoded_frame_size)) {
      return fail_detail(state, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
          "encoded frame byte size overflow");
    }
  }

  *out_source_dtype = source_dtype;
  *out_source_planar = source_planar;
  *out_source_row_stride = source_row_stride;
  *out_source_plane_stride = source_plane_stride;
  *out_source_frame_size = source_frame_size;
  *out_encoded_frame_size = encoded_frame_size;
  return PIXEL_CODEC_ERR_OK;
}

}  // namespace

pixel_error_code encode_uncompressed_frame(
    ErrorState* state, pixel_encoder_request* request) {
  DtypeInfo source_dtype{};
  bool source_planar = false;
  uint64_t source_row_stride_u64 = 0;
  uint64_t source_plane_stride_u64 = 0;
  uint64_t source_frame_size_u64 = 0;
  uint64_t encoded_frame_size_u64 = 0;
  const pixel_error_code validate_ec = validate_encode_request(state, request,
      &source_dtype, &source_planar, &source_row_stride_u64,
      &source_plane_stride_u64, &source_frame_size_u64, &encoded_frame_size_u64);
  if (validate_ec != PIXEL_CODEC_ERR_OK) {
    return validate_ec;
  }

  request->output.encoded_size = encoded_frame_size_u64;

  if (request->output.encoded_buffer.data == nullptr ||
      request->output.encoded_buffer.size < encoded_frame_size_u64) {
    return fail_detail(state, PIXEL_CODEC_ERR_OUTPUT_TOO_SMALL, "encode_frame",
        "output buffer too small");
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
    return fail_detail(state, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "size conversion overflow");
  }

  const uint8_t* src = request->source.source_buffer.data;
  uint8_t* dst = request->output.encoded_buffer.data;
  uint64_t row_payload_u64 = 0;
  if (!mul_u64(static_cast<uint64_t>(cols),
          static_cast<uint64_t>(source_planar ? std::size_t{1} : samples),
          &row_payload_u64) ||
      !mul_u64(row_payload_u64, static_cast<uint64_t>(sample_bytes), &row_payload_u64)) {
    return fail_detail(state, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "row payload byte size overflow");
  }
  std::size_t row_payload = 0;
  if (!u64_to_size(row_payload_u64, &row_payload)) {
    return fail_detail(state, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "row payload size conversion overflow");
  }

  if (!source_planar) {
    for (std::size_t r = 0; r < rows; ++r) {
      const uint8_t* src_row = src + r * source_row_stride;
      uint8_t* dst_row = dst + r * row_payload;
      std::memcpy(dst_row, src_row, row_payload);
    }
    clear_error(state);
    return PIXEL_CODEC_ERR_OK;
  }

  uint64_t plane_payload_u64 = 0;
  if (!mul_u64(static_cast<uint64_t>(row_payload), static_cast<uint64_t>(rows), &plane_payload_u64)) {
    return fail_detail(state, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "plane payload byte size overflow");
  }
  std::size_t plane_payload = 0;
  if (!u64_to_size(plane_payload_u64, &plane_payload)) {
    return fail_detail(state, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "plane payload size conversion overflow");
  }
  for (std::size_t s = 0; s < samples; ++s) {
    const uint8_t* src_plane = src + s * source_plane_stride;
    uint8_t* dst_plane = dst + s * plane_payload;
    for (std::size_t r = 0; r < rows; ++r) {
      const uint8_t* src_row = src_plane + r * source_row_stride;
      uint8_t* dst_row = dst_plane + r * row_payload;
      std::memcpy(dst_row, src_row, row_payload);
    }
  }

  clear_error(state);
  return PIXEL_CODEC_ERR_OK;
}

}  // namespace pixel::core
