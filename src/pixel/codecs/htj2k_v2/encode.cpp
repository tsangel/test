#include <ojph_codestream.h>
#include <ojph_file.h>
#include <ojph_mem.h>
#include <ojph_params.h>

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

namespace pixel::htj2k_plugin_v2 {

namespace {

struct SourceFrameLayout {
  bool source_is_planar{false};
  uint64_t row_payload{0};
  uint64_t row_stride{0};
  uint64_t plane_stride{0};
  uint64_t minimum_frame_bytes{0};
};

uint32_t bit_mask_u32(int bits) {
  if (bits <= 0) {
    return 0;
  }
  if (bits >= 32) {
    return 0xFFFFFFFFu;
  }
  return (uint32_t{1} << static_cast<unsigned>(bits)) - 1u;
}

int32_t sign_extend_u32(uint32_t raw, int bits) {
  if (bits <= 0) {
    return 0;
  }
  if (bits >= 32) {
    return static_cast<int32_t>(raw);
  }
  const int shift = 32 - bits;
  return static_cast<int32_t>(raw << static_cast<unsigned>(shift)) >> shift;
}

bool load_u8_or_u16_sample(
    const uint8_t* sample_ptr, uint32_t bytes_per_sample, uint32_t* out_raw) {
  if (sample_ptr == nullptr || out_raw == nullptr) {
    return false;
  }
  switch (bytes_per_sample) {
  case 1:
    *out_raw = sample_ptr[0];
    return true;
  case 2:
    *out_raw = static_cast<uint32_t>(sample_ptr[0]) |
        (static_cast<uint32_t>(sample_ptr[1]) << 8);
    return true;
  default:
    return false;
  }
}

bool load_i8_or_i16_sample_from_source(const uint8_t* sample_ptr, uint32_t bytes_per_sample,
    bool source_signed, int bits_stored, int32_t* out_value) {
  if (out_value == nullptr) {
    return false;
  }
  uint32_t raw = 0;
  if (!load_u8_or_u16_sample(sample_ptr, bytes_per_sample, &raw)) {
    return false;
  }
  if (source_signed) {
    *out_value = sign_extend_u32(raw, bits_stored);
    return true;
  }
  raw &= bit_mask_u32(bits_stored);
  if (raw > static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) {
    return false;
  }
  *out_value = static_cast<int32_t>(raw);
  return true;
}

double resolve_lossy_qstep(const EncoderCtx& ctx,
    const pixel_encoder_request_v2& request) {
  if (ctx.has_explicit_qstep) {
    return ctx.qstep;
  }
  if (ctx.target_psnr > 0.0) {
    return std::clamp(std::pow(10.0, -ctx.target_psnr / 20.0), 0.00001, 0.5);
  }
  if (ctx.target_bpp > 0.0) {
    const double uncompressed_bpp = static_cast<double>(request.frame.bits_stored) *
        static_cast<double>(request.frame.samples_per_pixel);
    const double compression_ratio = uncompressed_bpp / ctx.target_bpp;
    return std::clamp(compression_ratio * 0.01, 0.00001, 0.5);
  }
  return ctx.qstep;
}

pixel_error_code_v2 validate_encoder_request(
    EncoderCtx* ctx, const pixel_encoder_request_v2* request, DtypeInfo* out_source_dtype,
    SourceFrameLayout* out_source_layout, bool* out_source_signed, bool* out_lossless,
    bool* out_rpcl_progression, double* out_lossy_qstep) {
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

  const bool source_planar = is_planar_code(request->frame.source_planar) && samples > 1;
  if (request->frame.use_multicomponent_transform != 0 && samples != 3) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "use_multicomponent_transform requires samples_per_pixel=3");
  }

  if (cols > static_cast<uint64_t>(std::numeric_limits<ojph::ui32>::max()) ||
      rows > static_cast<uint64_t>(std::numeric_limits<ojph::ui32>::max()) ||
      samples > static_cast<uint64_t>(std::numeric_limits<ojph::ui32>::max())) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "rows/cols/samples_per_pixel exceed OpenJPH range");
  }

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
  if (row_stride > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "source_row_stride exceeds uint32 range");
  }

  uint64_t plane_stride = 0;
  if (!mul_u64(row_stride, rows, &plane_stride)) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "source plane stride overflow");
  }

  uint64_t minimum_frame_bytes = plane_stride;
  if (source_planar) {
    if (!mul_u64(plane_stride, samples, &minimum_frame_bytes)) {
      return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
          "source frame byte size overflow");
    }
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
      request->frame.codec_profile_code != PIXEL_CODEC_PROFILE_HTJ2K_LOSSY_V2;
  const bool rpcl_progression =
      request->frame.codec_profile_code == PIXEL_CODEC_PROFILE_HTJ2K_LOSSLESS_RPCL_V2;

  double lossy_qstep = ctx->qstep;
  if (!lossless) {
    lossy_qstep = resolve_lossy_qstep(*ctx, *request);
  }
  if (!lossless && (lossy_qstep <= 0.0 || lossy_qstep > 0.5)) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "lossy HTJ2K requires qstep in (0,0.5]");
  }

  out_source_layout->source_is_planar = source_planar;
  out_source_layout->row_payload = row_payload;
  out_source_layout->row_stride = row_stride;
  out_source_layout->plane_stride = plane_stride;
  out_source_layout->minimum_frame_bytes = minimum_frame_bytes;
  *out_source_dtype = source_dtype;
  *out_source_signed = source_signed;
  *out_lossless = lossless;
  *out_rpcl_progression = rpcl_progression;
  if (out_lossy_qstep != nullptr) {
    *out_lossy_qstep = lossy_qstep;
  }
  return PIXEL_CODEC_ERR_OK;
}

pixel_error_code_v2 fill_openjph_line_from_source(EncoderCtx* ctx, ojph::line_buf* line,
    const uint8_t* source, const SourceFrameLayout& layout, std::size_t row,
    std::size_t component_index, std::size_t cols, std::size_t samples_per_pixel,
    uint32_t bytes_per_sample, bool source_signed, int bits_stored) {
  if (line == nullptr) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_FAILED, "encode_frame",
        "OpenJPH returned null line buffer");
  }
  if (line->size < cols) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_FAILED, "encode_frame",
        "OpenJPH line size is too small");
  }

  const bool line_is_integer = (line->flags & ojph::line_buf::LFT_INTEGER) != 0;
  if (line_is_integer && line->i32 == nullptr) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_FAILED, "encode_frame",
        "OpenJPH integer line has null data pointer");
  }
  if (!line_is_integer && line->f32 == nullptr) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_FAILED, "encode_frame",
        "OpenJPH float line has null data pointer");
  }

  const uint8_t* source_row = nullptr;
  if (layout.source_is_planar) {
    source_row = source + component_index * static_cast<std::size_t>(layout.plane_stride) +
        row * static_cast<std::size_t>(layout.row_stride);
  } else {
    source_row = source + row * static_cast<std::size_t>(layout.row_stride);
  }

  const std::size_t sample_bytes = static_cast<std::size_t>(bytes_per_sample);
  for (std::size_t col = 0; col < cols; ++col) {
    const uint8_t* sample_ptr = nullptr;
    if (layout.source_is_planar) {
      sample_ptr = source_row + col * sample_bytes;
    } else {
      sample_ptr = source_row + (col * samples_per_pixel + component_index) * sample_bytes;
    }
    int32_t sample_value = 0;
    if (!load_i8_or_i16_sample_from_source(sample_ptr, bytes_per_sample, source_signed,
            bits_stored, &sample_value)) {
      return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "encode_frame",
          "failed to read source sample");
    }

    if (line_is_integer) {
      line->i32[col] = sample_value;
    } else {
      line->f32[col] = static_cast<float>(sample_value);
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
    bool rpcl_progression = false;
    double lossy_qstep = c->qstep;

    const pixel_error_code_v2 valid_ec = validate_encoder_request(c, request,
        &source_dtype, &source_layout, &source_signed, &lossless, &rpcl_progression,
        &lossy_qstep);
    if (valid_ec != PIXEL_CODEC_ERR_OK) {
      return valid_ec;
    }

    const std::size_t rows = static_cast<std::size_t>(request->frame.rows);
    const std::size_t cols = static_cast<std::size_t>(request->frame.cols);
    const std::size_t samples_per_pixel =
        static_cast<std::size_t>(request->frame.samples_per_pixel);
    const uint8_t* source = request->source.source_buffer.data;

    ojph::codestream codestream{};
    codestream.set_planar(false);

    auto siz = codestream.access_siz();
    siz.set_image_extent(
        ojph::point(static_cast<ojph::ui32>(cols), static_cast<ojph::ui32>(rows)));
    siz.set_num_components(static_cast<ojph::ui32>(samples_per_pixel));
    for (std::size_t component = 0; component < samples_per_pixel; ++component) {
      siz.set_component(static_cast<ojph::ui32>(component), ojph::point(1, 1),
          static_cast<ojph::ui32>(request->frame.bits_stored), source_signed);
    }
    siz.set_image_offset(ojph::point(0, 0));
    siz.set_tile_size(
        ojph::size(static_cast<ojph::ui32>(cols), static_cast<ojph::ui32>(rows)));
    siz.set_tile_offset(ojph::point(0, 0));

    auto cod = codestream.access_cod();
    cod.set_progression_order(rpcl_progression ? "RPCL" : "LRCP");
    cod.set_color_transform(request->frame.use_multicomponent_transform != 0);
    cod.set_reversible(lossless);
    if (!lossless) {
      codestream.access_qcd().set_irrev_quant(static_cast<float>(lossy_qstep));
    }

    ojph::mem_outfile outfile{};
    outfile.open();
    codestream.write_headers(&outfile);

    ojph::ui32 next_component = 0;
    ojph::line_buf* line = codestream.exchange(nullptr, next_component);

    if (codestream.is_planar()) {
      for (std::size_t component = 0; component < samples_per_pixel; ++component) {
        for (std::size_t row = 0; row < rows; ++row) {
          if (next_component != static_cast<ojph::ui32>(component)) {
            return fail_detail(c, PIXEL_CODEC_ERR_FAILED, "encode_frame",
                "OpenJPH planar exchange order mismatch");
          }
          const pixel_error_code_v2 fill_ec = fill_openjph_line_from_source(c, line,
              source, source_layout, row, component, cols, samples_per_pixel,
              source_dtype.bytes, source_signed, request->frame.bits_stored);
          if (fill_ec != PIXEL_CODEC_ERR_OK) {
            return fill_ec;
          }
          line = codestream.exchange(line, next_component);
        }
      }
    } else {
      for (std::size_t row = 0; row < rows; ++row) {
        for (std::size_t component = 0; component < samples_per_pixel; ++component) {
          if (next_component != static_cast<ojph::ui32>(component)) {
            return fail_detail(c, PIXEL_CODEC_ERR_FAILED, "encode_frame",
                "OpenJPH interleaved exchange order mismatch");
          }
          const pixel_error_code_v2 fill_ec = fill_openjph_line_from_source(c, line,
              source, source_layout, row, component, cols, samples_per_pixel,
              source_dtype.bytes, source_signed, request->frame.bits_stored);
          if (fill_ec != PIXEL_CODEC_ERR_OK) {
            return fill_ec;
          }
          line = codestream.exchange(line, next_component);
        }
      }
    }

    codestream.flush();
    codestream.close();

    const std::size_t used_size = outfile.get_used_size();
    if (used_size == 0 || outfile.get_data() == nullptr) {
      return fail_detail(c, PIXEL_CODEC_ERR_FAILED, "encode_frame",
          "OpenJPH produced empty codestream");
    }

    std::vector<uint8_t> encoded(used_size);
    std::memcpy(encoded.data(), outfile.get_data(), used_size);
    outfile.close();

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

}  // namespace pixel::htj2k_plugin_v2
