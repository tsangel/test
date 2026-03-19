#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <new>
#include <string>
#include <type_traits>

#include "../common/decode_fastpath.hpp"
#include "../common/integral_hotpath.hpp"
#include "internal.hpp"

namespace pixel::openjpeg_codec {

namespace {

template <typename DstT>
inline void store_value(uint8_t* dst, DstT value) {
  std::memcpy(dst, &value, sizeof(DstT));
}

bool can_write_openjpeg_direct_to_dst_dtype(
    const opj_image_t* image, const DtypeInfo& dst_dtype) {
  if (image == nullptr || dst_dtype.is_float) {
    return false;
  }
  for (OPJ_UINT32 comp_idx = 0; comp_idx < image->numcomps; ++comp_idx) {
    const auto& comp = image->comps[comp_idx];
    if (!::pixel::codec_common::decoded_integral_component_fits_dst_dtype(
            comp.prec, comp.sgnd != 0u, dst_dtype)) {
      return false;
    }
  }
  return true;
}

template <typename DstT>
pixel_error_code write_openjpeg_mono_image_typed(
    const pixel_decoder_request* request, const opj_image_t* image,
    uint64_t row_stride) {
  const std::size_t rows = static_cast<std::size_t>(request->frame.rows);
  const std::size_t cols = static_cast<std::size_t>(request->frame.cols);
  const std::size_t row_stride_sz = static_cast<std::size_t>(row_stride);
  const std::size_t row_bytes = cols * sizeof(DstT);
  const int* src_comp = image->comps[0].data;

  if constexpr (std::is_same_v<DstT, int32_t>) {
    if (row_stride_sz == row_bytes) {
      std::memcpy(request->output.dst, src_comp, rows * row_bytes);
      return PIXEL_CODEC_ERR_OK;
    }
  }
  for (std::size_t r = 0; r < rows; ++r) {
    const int* src_row = src_comp + r * cols;
    uint8_t* dst_row_bytes = request->output.dst + r * row_stride_sz;
    ::pixel::codec_common::write_typed_integral_mono_row<DstT>(
        dst_row_bytes, cols, src_row);
  }

  return PIXEL_CODEC_ERR_OK;
}

template <typename DstT>
pixel_error_code write_openjpeg_image_typed(const pixel_decoder_request* request,
    const opj_image_t* image, uint64_t row_stride, uint64_t plane_bytes,
    bool planar) {
  const std::size_t rows = static_cast<std::size_t>(request->frame.rows);
  const std::size_t cols = static_cast<std::size_t>(request->frame.cols);
  const std::size_t samples = static_cast<std::size_t>(request->frame.samples_per_pixel);
  const std::size_t row_stride_sz = static_cast<std::size_t>(row_stride);
  const std::size_t plane_bytes_sz = static_cast<std::size_t>(plane_bytes);

  if (samples == 1) {
    return write_openjpeg_mono_image_typed<DstT>(request, image, row_stride);
  }

  if (planar && samples > 1) {
    for (std::size_t comp = 0; comp < samples; ++comp) {
      const int* src_comp = image->comps[comp].data;
      uint8_t* dst_plane = request->output.dst + comp * plane_bytes_sz;
      for (std::size_t r = 0; r < rows; ++r) {
        const std::size_t src_row_idx = r * cols;
        uint8_t* dst_row = dst_plane + r * row_stride_sz;
        ::pixel::codec_common::write_typed_integral_mono_row<DstT>(
            dst_row, cols, src_comp + src_row_idx);
      }
    }
    return PIXEL_CODEC_ERR_OK;
  }

  for (std::size_t r = 0; r < rows; ++r) {
    uint8_t* dst_row = request->output.dst + r * row_stride_sz;
    const std::size_t src_row_idx = r * cols;
    ::pixel::codec_common::write_typed_integral_interleaved_row<DstT>(
        dst_row, cols, samples,
        [&](std::size_t c, std::size_t comp) -> int32_t {
          return image->comps[comp].data[src_row_idx + c];
        });
  }

  return PIXEL_CODEC_ERR_OK;
}

pixel_error_code validate_decoder_request(
    DecoderCtx* ctx, const pixel_decoder_request* request) {
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

  return PIXEL_CODEC_ERR_OK;
}

pixel_error_code write_openjpeg_image_to_output(
    DecoderCtx* ctx, const pixel_decoder_request* request, const opj_image_t* image) {
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
    if (comp.prec == 0 || comp.prec > 32 || comp.sgnd > 1u) {
      return fail_detail(ctx, PIXEL_CODEC_ERR_FAILED, "decode_frame",
          "decoded component precision/sign metadata is invalid");
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

  // OpenJPEG always exposes decoded samples via int32 component buffers. When the
  // decoded sample range fits the requested destination dtype, keep the writeback
  // on a typed path instead of dropping into the generic per-sample dispatcher.
  if (can_write_openjpeg_direct_to_dst_dtype(image, dst_dtype)) {
    if (request->output.dst_dtype == PIXEL_DTYPE_U8) {
      return write_openjpeg_image_typed<uint8_t>(
          request, image, row_stride, plane_bytes, planar);
    }
    if (request->output.dst_dtype == PIXEL_DTYPE_S8) {
      return write_openjpeg_image_typed<int8_t>(
          request, image, row_stride, plane_bytes, planar);
    }
    if (request->output.dst_dtype == PIXEL_DTYPE_U16) {
      return write_openjpeg_image_typed<uint16_t>(
          request, image, row_stride, plane_bytes, planar);
    }
    if (request->output.dst_dtype == PIXEL_DTYPE_S16) {
      return write_openjpeg_image_typed<int16_t>(
          request, image, row_stride, plane_bytes, planar);
    }
    if (request->output.dst_dtype == PIXEL_DTYPE_U32) {
      return write_openjpeg_image_typed<uint32_t>(
          request, image, row_stride, plane_bytes, planar);
    }
    if (request->output.dst_dtype == PIXEL_DTYPE_S32) {
      return write_openjpeg_image_typed<int32_t>(
          request, image, row_stride, plane_bytes, planar);
    }
  }

  if (!::pixel::codec_common::write_loaded_converted_rows_to_dst(
          request, row_stride, planar,
          [&](std::size_t row, std::size_t col, std::size_t comp) -> int32_t {
            return image->comps[comp].data[row * static_cast<std::size_t>(cols) + col];
          })) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "decode_frame",
        "unsupported destination dtype");
  }
  return PIXEL_CODEC_ERR_OK;
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
    const pixel_error_code valid_ec = validate_decoder_request(c, request);
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

    const pixel_error_code write_ec =
        write_openjpeg_image_to_output(c, request, decoded_image.get());
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

}  // namespace pixel::openjpeg_codec
