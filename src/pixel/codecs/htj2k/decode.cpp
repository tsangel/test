#include <ojph_codestream.h>
#include <ojph_file.h>
#include <ojph_mem.h>
#include <ojph_params.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <string>
#include <vector>

#include "../common/decode_fastpath.hpp"
#include "internal.hpp"

namespace pixel::htj2k_codec {

namespace {

template <typename DstT>
inline void store_htj2k_value(uint8_t* dst, DstT value) {
  std::memcpy(dst, &value, sizeof(DstT));
}

const int32_t* openjph_line_as_i32(const ojph::line_buf* line, std::size_t cols,
    std::vector<int32_t>* scratch) {
  if (line == nullptr) {
    return nullptr;
  }
  if (line->size < cols) {
    return nullptr;
  }

  const bool is_integer = (line->flags & ojph::line_buf::LFT_INTEGER) != 0;
  if (is_integer) {
    return line->i32;
  }

  if (scratch == nullptr || line->f32 == nullptr) {
    return nullptr;
  }

  scratch->resize(cols);
  for (std::size_t c = 0; c < cols; ++c) {
    (*scratch)[c] = static_cast<int32_t>(std::lround(line->f32[c]));
  }
  return scratch->data();
}

bool can_write_openjph_direct_to_dst_dtype(
    ojph::param_siz siz, uint32_t components, const DtypeInfo& dst_dtype) {
  if (dst_dtype.is_float) {
    return false;
  }
  for (uint32_t comp = 0; comp < components; ++comp) {
    if (!::pixel::codec_common::decoded_integral_component_fits_dst_dtype(
            siz.get_bit_depth(comp), siz.is_signed(comp), dst_dtype)) {
      return false;
    }
  }
  return true;
}

pixel_error_code validate_decoder_request(
    DecoderCtx* ctx, const pixel_decoder_request* request,
    DtypeInfo* out_source_dtype, DtypeInfo* out_dst_dtype,
    uint64_t* out_row_stride, bool* out_output_planar) {
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
      source_dtype.is_float || source_dtype.bytes == 0 || source_dtype.bytes > 4) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "source_dtype must be integral <=32-bit type");
  }

  if (request->frame.bits_stored <= 0 ||
      request->frame.bits_stored > static_cast<int32_t>(source_dtype.bytes * 8u)) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "bits_stored must be in [1,source_dtype width]");
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
  *out_output_planar = output_planar;
  return PIXEL_CODEC_ERR_OK;
}

pixel_error_code write_htj2k_decoded_samples(DecoderCtx* ctx,
    const pixel_decoder_request* request,
    const std::vector<int32_t>& decoded_interleaved,
    DtypeInfo dst_dtype,
    uint64_t row_stride,
    bool output_planar) {
  const uint64_t rows = static_cast<uint64_t>(request->frame.rows);
  const uint64_t cols = static_cast<uint64_t>(request->frame.cols);
  const uint64_t samples = static_cast<uint64_t>(request->frame.samples_per_pixel);

  const std::size_t rows_sz = static_cast<std::size_t>(rows);
  const std::size_t cols_sz = static_cast<std::size_t>(cols);
  const std::size_t samples_sz = static_cast<std::size_t>(samples);

  const uint64_t total_samples_u64 = rows * cols * samples;
  if (decoded_interleaved.size() != static_cast<std::size_t>(total_samples_u64)) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_FAILED, "decode_frame",
        "decoded sample count mismatch");
  }

  const std::size_t row_stride_sz = static_cast<std::size_t>(row_stride);
  const std::size_t dst_sample_bytes = static_cast<std::size_t>(dst_dtype.bytes);

  if (output_planar && samples_sz > 1) {
    const std::size_t plane_bytes = row_stride_sz * rows_sz;
    for (std::size_t comp = 0; comp < samples_sz; ++comp) {
      uint8_t* dst_plane = request->output.dst + comp * plane_bytes;
      for (std::size_t r = 0; r < rows_sz; ++r) {
        uint8_t* dst_row = dst_plane + r * row_stride_sz;
        for (std::size_t c = 0; c < cols_sz; ++c) {
          const std::size_t src_index = (r * cols_sz + c) * samples_sz + comp;
          const int32_t sample = decoded_interleaved[src_index];
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
      const std::size_t src_base = (r * cols_sz + c) * samples_sz;
      for (std::size_t comp = 0; comp < samples_sz; ++comp) {
        const int32_t sample = decoded_interleaved[src_base + comp];
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

template <typename DstT>
void write_openjph_component_line(const pixel_decoder_request* request,
    uint64_t row_stride, bool output_planar, std::size_t rows, std::size_t row,
    std::size_t comp, std::size_t cols, std::size_t samples,
    const int32_t* samples_line) {
  const std::size_t row_stride_sz = static_cast<std::size_t>(row_stride);
  if (samples == 1) {
    uint8_t* dst_row = request->output.dst + row * row_stride_sz;
    ::pixel::codec_common::write_typed_integral_mono_row<DstT>(
        dst_row, cols, samples_line);
    return;
  }

  if (output_planar) {
    const std::size_t plane_bytes = row_stride_sz * rows;
    uint8_t* dst_row = request->output.dst + comp * plane_bytes + row * row_stride_sz;
    ::pixel::codec_common::write_typed_integral_mono_row<DstT>(
        dst_row, cols, samples_line);
    return;
  }

  uint8_t* dst_row = request->output.dst + row * row_stride_sz;
  const std::size_t pixel_stride = samples * sizeof(DstT);
  for (std::size_t c = 0; c < cols; ++c) {
    store_htj2k_value(dst_row + c * pixel_stride + comp * sizeof(DstT),
        static_cast<DstT>(samples_line[c]));
  }
}

template <typename DstT>
void write_openjph_interleaved_row(const pixel_decoder_request* request,
    uint64_t row_stride, std::size_t row, std::size_t cols, std::size_t samples,
    const int32_t* const* component_lines) {
  const std::size_t row_stride_sz = static_cast<std::size_t>(row_stride);
  uint8_t* dst_row = request->output.dst + row * row_stride_sz;
  ::pixel::codec_common::write_typed_integral_interleaved_row<DstT>(
      dst_row, cols, samples,
      [&](std::size_t c, std::size_t comp) -> int32_t {
        return component_lines[comp][c];
      });
}

template <typename DstT>
pixel_error_code decode_openjph_direct_to_output(DecoderCtx* ctx,
    ojph::codestream* codestream, const pixel_decoder_request* request,
    uint64_t row_stride, bool output_planar) {
  if (codestream == nullptr) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_FAILED, "decode_frame",
        "OpenJPH codestream is null");
  }

  const std::size_t rows = static_cast<std::size_t>(request->frame.rows);
  const std::size_t cols = static_cast<std::size_t>(request->frame.cols);
  const std::size_t samples = static_cast<std::size_t>(request->frame.samples_per_pixel);
  std::vector<int32_t> scratch_line{};

  if (codestream->is_planar()) {
    for (std::size_t comp = 0; comp < samples; ++comp) {
      for (std::size_t row = 0; row < rows; ++row) {
        ojph::ui32 comp_num = 0;
        ojph::line_buf* line = codestream->pull(comp_num);
        if (comp_num != static_cast<ojph::ui32>(comp)) {
          return fail_detail(ctx, PIXEL_CODEC_ERR_FAILED, "decode_frame",
              "OpenJPH planar pull order mismatch");
        }
        const int32_t* samples_line = openjph_line_as_i32(line, cols, &scratch_line);
        if (samples_line == nullptr) {
          return fail_detail(ctx, PIXEL_CODEC_ERR_FAILED, "decode_frame",
              "OpenJPH returned invalid line buffer");
        }
        write_openjph_component_line<DstT>(request, row_stride, output_planar,
            rows, row, comp, cols, samples, samples_line);
      }
    }
    return PIXEL_CODEC_ERR_OK;
  }

  std::vector<std::vector<int32_t>> scratch_lines(samples);
  std::vector<const int32_t*> component_lines(samples, nullptr);
  for (std::size_t row = 0; row < rows; ++row) {
    for (std::size_t comp = 0; comp < samples; ++comp) {
      ojph::ui32 comp_num = 0;
      ojph::line_buf* line = codestream->pull(comp_num);
      if (comp_num != static_cast<ojph::ui32>(comp)) {
        return fail_detail(ctx, PIXEL_CODEC_ERR_FAILED, "decode_frame",
            "OpenJPH interleaved pull order mismatch");
      }
      const int32_t* samples_line =
          openjph_line_as_i32(line, cols, &scratch_lines[comp]);
      if (samples_line == nullptr) {
        return fail_detail(ctx, PIXEL_CODEC_ERR_FAILED, "decode_frame",
            "OpenJPH returned invalid line buffer");
      }
      component_lines[comp] = samples_line;
      if (output_planar || samples == 1) {
        write_openjph_component_line<DstT>(request, row_stride, output_planar,
            rows, row, comp, cols, samples, samples_line);
      }
    }
    if (!output_planar && samples > 1) {
      write_openjph_interleaved_row<DstT>(request, row_stride,
          row, cols, samples, component_lines.data());
    }
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
    DtypeInfo source_dtype{};
    DtypeInfo dst_dtype{};
    uint64_t row_stride = 0;
    bool output_planar = false;

    const pixel_error_code valid_ec = validate_decoder_request(c, request,
        &source_dtype, &dst_dtype,
        &row_stride, &output_planar);
    if (valid_ec != PIXEL_CODEC_ERR_OK) {
      return valid_ec;
    }

    ojph::mem_infile infile{};
    infile.open(request->source.source_buffer.data,
        static_cast<std::size_t>(request->source.source_buffer.size));

    ojph::codestream codestream{};
    codestream.set_planar(false);
    codestream.read_headers(&infile);

    const auto siz = codestream.access_siz();
    const uint32_t components = siz.get_num_components();
    const uint64_t rows = static_cast<uint64_t>(request->frame.rows);
    const uint64_t cols = static_cast<uint64_t>(request->frame.cols);
    const uint64_t samples = static_cast<uint64_t>(request->frame.samples_per_pixel);

    if (components != samples) {
      return fail_detail(c, PIXEL_CODEC_ERR_FAILED, "decode_frame",
          "decoded component count mismatch");
    }

    for (uint32_t comp = 0; comp < components; ++comp) {
      const uint64_t recon_width = siz.get_recon_width(comp);
      const uint64_t recon_height = siz.get_recon_height(comp);
      if (recon_width != cols || recon_height != rows) {
        return fail_detail(c, PIXEL_CODEC_ERR_FAILED, "decode_frame",
            "decoded dimensions mismatch");
      }

      const bool decoded_signed = siz.is_signed(comp);
      if (decoded_signed != source_dtype.is_signed) {
        return fail_detail(c, PIXEL_CODEC_ERR_FAILED, "decode_frame",
            "decoded signedness mismatch");
      }

      const uint32_t precision = siz.get_bit_depth(comp);
      if (precision == 0 || precision > source_dtype.bytes * 8u) {
        return fail_detail(c, PIXEL_CODEC_ERR_FAILED, "decode_frame",
            "decoded precision exceeds source dtype width");
      }

      if (request->frame.bits_stored > 0 &&
          precision > static_cast<uint32_t>(request->frame.bits_stored)) {
        const uint32_t decoded_bytes = (precision + 7u) / 8u;
        const uint32_t metadata_bytes =
            (static_cast<uint32_t>(request->frame.bits_stored) + 7u) / 8u;
        if (decoded_bytes > metadata_bytes) {
          return fail_detail(c, PIXEL_CODEC_ERR_FAILED, "decode_frame",
              "decoded precision exceeds metadata bits_stored width");
        }
      }
    }

    codestream.create();

    if (can_write_openjph_direct_to_dst_dtype(siz, components, dst_dtype)) {
      pixel_error_code direct_ec = PIXEL_CODEC_ERR_UNSUPPORTED;
      if (request->output.dst_dtype == PIXEL_DTYPE_U8) {
        direct_ec = decode_openjph_direct_to_output<uint8_t>(
            c, &codestream, request, row_stride, output_planar);
      } else if (request->output.dst_dtype == PIXEL_DTYPE_S8) {
        direct_ec = decode_openjph_direct_to_output<int8_t>(
            c, &codestream, request, row_stride, output_planar);
      } else if (request->output.dst_dtype == PIXEL_DTYPE_U16) {
        direct_ec = decode_openjph_direct_to_output<uint16_t>(
            c, &codestream, request, row_stride, output_planar);
      } else if (request->output.dst_dtype == PIXEL_DTYPE_S16) {
        direct_ec = decode_openjph_direct_to_output<int16_t>(
            c, &codestream, request, row_stride, output_planar);
      } else if (request->output.dst_dtype == PIXEL_DTYPE_U32) {
        direct_ec = decode_openjph_direct_to_output<uint32_t>(
            c, &codestream, request, row_stride, output_planar);
      } else if (request->output.dst_dtype == PIXEL_DTYPE_S32) {
        direct_ec = decode_openjph_direct_to_output<int32_t>(
            c, &codestream, request, row_stride, output_planar);
      }

      codestream.close();
      infile.close();
      if (direct_ec != PIXEL_CODEC_ERR_OK) {
        return direct_ec;
      }

      clear_detail(c);
      return PIXEL_CODEC_ERR_OK;
    }

    const uint64_t total_samples_u64 = rows * cols * samples;
    std::vector<int32_t> decoded(static_cast<std::size_t>(total_samples_u64), int32_t{0});
    std::vector<int32_t> scratch_line{};

    const std::size_t rows_sz = static_cast<std::size_t>(rows);
    const std::size_t cols_sz = static_cast<std::size_t>(cols);
    const std::size_t samples_sz = static_cast<std::size_t>(samples);

    if (codestream.is_planar()) {
      for (std::size_t comp = 0; comp < samples_sz; ++comp) {
        for (std::size_t row = 0; row < rows_sz; ++row) {
          ojph::ui32 comp_num = 0;
          ojph::line_buf* line = codestream.pull(comp_num);
          if (comp_num != static_cast<ojph::ui32>(comp)) {
            return fail_detail(c, PIXEL_CODEC_ERR_FAILED, "decode_frame",
                "OpenJPH planar pull order mismatch");
          }
          const int32_t* samples_line = openjph_line_as_i32(line, cols_sz, &scratch_line);
          if (samples_line == nullptr) {
            return fail_detail(c, PIXEL_CODEC_ERR_FAILED, "decode_frame",
                "OpenJPH returned invalid line buffer");
          }
          for (std::size_t col = 0; col < cols_sz; ++col) {
            const std::size_t dst_index = (row * cols_sz + col) * samples_sz + comp;
            decoded[dst_index] = samples_line[col];
          }
        }
      }
    } else {
      for (std::size_t row = 0; row < rows_sz; ++row) {
        for (std::size_t comp = 0; comp < samples_sz; ++comp) {
          ojph::ui32 comp_num = 0;
          ojph::line_buf* line = codestream.pull(comp_num);
          if (comp_num != static_cast<ojph::ui32>(comp)) {
            return fail_detail(c, PIXEL_CODEC_ERR_FAILED, "decode_frame",
                "OpenJPH interleaved pull order mismatch");
          }
          const int32_t* samples_line = openjph_line_as_i32(line, cols_sz, &scratch_line);
          if (samples_line == nullptr) {
            return fail_detail(c, PIXEL_CODEC_ERR_FAILED, "decode_frame",
                "OpenJPH returned invalid line buffer");
          }
          for (std::size_t col = 0; col < cols_sz; ++col) {
            const std::size_t dst_index = (row * cols_sz + col) * samples_sz + comp;
            decoded[dst_index] = samples_line[col];
          }
        }
      }
    }

    codestream.close();
    infile.close();

    const pixel_error_code write_ec = write_htj2k_decoded_samples(c, request,
        decoded, dst_dtype, row_stride, output_planar);
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

}  // namespace pixel::htj2k_codec
