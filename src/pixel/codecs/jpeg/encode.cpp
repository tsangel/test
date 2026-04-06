#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4819)
#endif
#include <turbojpeg.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <type_traits>
#include <vector>

#include "../common/integral_hotpath.hpp"
#include "internal.hpp"

namespace pixel::jpeg_codec {

namespace {

class TurboJpegHandleGuard {
public:
  explicit TurboJpegHandleGuard(tjhandle handle) noexcept : handle_(handle) {}
  ~TurboJpegHandleGuard() {
    if (handle_ != nullptr) {
      tj3Destroy(handle_);
    }
  }

  TurboJpegHandleGuard(const TurboJpegHandleGuard&) = delete;
  TurboJpegHandleGuard& operator=(const TurboJpegHandleGuard&) = delete;

  [[nodiscard]] tjhandle get() const noexcept {
    return handle_;
  }

private:
  tjhandle handle_{nullptr};
};

class TurboJpegBufferGuard {
public:
  TurboJpegBufferGuard() = default;
  ~TurboJpegBufferGuard() {
    if (data_ != nullptr) {
      tj3Free(data_);
    }
  }

  TurboJpegBufferGuard(const TurboJpegBufferGuard&) = delete;
  TurboJpegBufferGuard& operator=(const TurboJpegBufferGuard&) = delete;

  [[nodiscard]] unsigned char** out_ptr() noexcept {
    return &data_;
  }

  [[nodiscard]] unsigned char* get() const noexcept {
    return data_;
  }

private:
  unsigned char* data_{nullptr};
};

struct SourceFrameLayout {
  bool source_is_planar{false};
  uint64_t row_payload{0};
  uint64_t row_stride{0};
  uint64_t plane_stride{0};
  uint64_t minimum_frame_bytes{0};
};

int pixel_format_for_samples(std::size_t samples_per_pixel) {
  switch (samples_per_pixel) {
  case 1:
    return TJPF_GRAY;
  case 3:
    return TJPF_RGB;
  case 4:
    return TJPF_CMYK;
  default:
    return TJPF_UNKNOWN;
  }
}

int colorspace_for_samples(std::size_t samples_per_pixel, bool lossless) {
  switch (samples_per_pixel) {
  case 1:
    return TJCS_GRAY;
  case 3:
    return lossless ? TJCS_RGB : TJCS_YCbCr;
  case 4:
    return TJCS_CMYK;
  default:
    return -1;
  }
}

pixel_error_code set_turbojpeg_param(
    EncoderCtx* ctx, tjhandle handle, int param, int value, const char* param_name) {
  if (tj3Set(handle, param, value) == 0) {
    return PIXEL_CODEC_ERR_OK;
  }
  const char* msg = tj3GetErrorStr(handle);
  char reason[320];
  std::snprintf(reason, sizeof(reason),
      "failed to set TurboJPEG param %s=%d (%s)",
      param_name, value,
      (msg != nullptr && msg[0] != '\0') ? msg : "unknown error");
  return fail_detail(ctx, PIXEL_CODEC_ERR_FAILED, "encode_frame", reason);
}

pixel_error_code validate_encoder_request(
    EncoderCtx* ctx, const pixel_encoder_request* request,
    DtypeInfo* out_source_dtype,
    SourceFrameLayout* out_source_layout,
    bool* out_source_signed,
    bool* out_lossless) {
  if (request->struct_size < sizeof(pixel_encoder_request) ||
      request->source.struct_size < sizeof(pixel_encoder_source) ||
      request->frame.struct_size < sizeof(pixel_encoder_frame_info) ||
      request->output.struct_size < sizeof(pixel_encoder_output)) {
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

  if (request->frame.bits_allocated > static_cast<int32_t>(source_dtype.bytes * 8u)) {
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

  const bool lossless =
      request->frame.codec_profile_code == PIXEL_CODEC_PROFILE_JPEG_LOSSLESS;
  if (!lossless && request->frame.bits_stored > 12) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "lossy JPEG supports precision up to 12 bits");
  }

  if (ctx->has_colorspace || ctx->has_subsampling) {
    if (request->frame.codec_profile_code != PIXEL_CODEC_PROFILE_JPEG_LOSSY) {
      return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
          "color_space/subsampling options require lossy JPEG");
    }
    if (samples != 3) {
      return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
          "color_space/subsampling options require samples_per_pixel=3");
    }
    if (ctx->has_subsampling &&
        (!ctx->has_colorspace || ctx->colorspace != TJCS_YCbCr)) {
      return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
          "subsampling option requires color_space=ybr");
    }
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

  if (ctx->quality < 1 || ctx->quality > 100) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "quality must be in [1,100]");
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

pixel_error_code build_normalized_interleaved(
    EncoderCtx* ctx, const pixel_encoder_request* request,
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
  const uint8_t* source = request->source.source_buffer.data;
  const int bits_stored = request->frame.bits_stored;
  const uint32_t mask = ::pixel::codec_common::bit_mask_u32_fast(bits_stored);

  auto build_fast = [&](auto source_bytes_tag, auto source_signed_tag, auto source_planar_tag) {
    constexpr uint32_t kSourceBytes = decltype(source_bytes_tag)::value;
    constexpr bool kSourceSigned = decltype(source_signed_tag)::value;
    constexpr bool kSourcePlanar = decltype(source_planar_tag)::value;
    constexpr std::size_t kSourceBytesSz = static_cast<std::size_t>(kSourceBytes);
    const std::size_t interleaved_pixel_stride = samples_sz * kSourceBytesSz;

    if constexpr (kSourcePlanar) {
      for (std::size_t row = 0; row < rows_sz; ++row) {
        std::array<const uint8_t*, 4> src_rows{};
        for (std::size_t comp = 0; comp < samples_sz; ++comp) {
          src_rows[comp] =
              source + comp * source_plane_stride_sz + row * source_row_stride_sz;
        }
        uint8_t* dst_row = out_interleaved->data() + row * row_bytes_sz;
        for (std::size_t col = 0; col < cols_sz; ++col) {
          uint8_t* dst_pixel = dst_row + col * interleaved_pixel_stride;
          for (std::size_t comp = 0; comp < samples_sz; ++comp) {
            const uint32_t raw =
                static_cast<uint32_t>(
                    ::pixel::codec_common::load_native_integral_as_i32<
                        kSourceBytes, kSourceSigned>(
                        src_rows[comp] + col * kSourceBytesSz, bits_stored)) & mask;
            ::pixel::codec_common::store_raw_native_integral<kSourceBytes>(
                dst_pixel + comp * kSourceBytesSz, raw);
          }
        }
      }
      return;
    }

    const std::size_t source_pixel_stride = samples_sz * kSourceBytesSz;
    for (std::size_t row = 0; row < rows_sz; ++row) {
      const uint8_t* src_row = source + row * source_row_stride_sz;
      uint8_t* dst_row = out_interleaved->data() + row * row_bytes_sz;
      for (std::size_t col = 0; col < cols_sz; ++col) {
        const uint8_t* src_pixel = src_row + col * source_pixel_stride;
        uint8_t* dst_pixel = dst_row + col * interleaved_pixel_stride;
        for (std::size_t comp = 0; comp < samples_sz; ++comp) {
          const uint32_t raw =
              static_cast<uint32_t>(
                  ::pixel::codec_common::load_native_integral_as_i32<
                      kSourceBytes, kSourceSigned>(
                      src_pixel + comp * kSourceBytesSz, bits_stored)) & mask;
          ::pixel::codec_common::store_raw_native_integral<kSourceBytes>(
              dst_pixel + comp * kSourceBytesSz, raw);
        }
      }
    }
  };

  switch (source_dtype.bytes) {
  case 1:
    if (source_signed) {
      if (source_layout.source_is_planar) {
        build_fast(std::integral_constant<uint32_t, 1>{},
            std::true_type{}, std::true_type{});
      } else {
        build_fast(std::integral_constant<uint32_t, 1>{},
            std::true_type{}, std::false_type{});
      }
    } else {
      if (source_layout.source_is_planar) {
        build_fast(std::integral_constant<uint32_t, 1>{},
            std::false_type{}, std::true_type{});
      } else {
        build_fast(std::integral_constant<uint32_t, 1>{},
            std::false_type{}, std::false_type{});
      }
    }
    return PIXEL_CODEC_ERR_OK;
  case 2:
    if (source_signed) {
      if (source_layout.source_is_planar) {
        build_fast(std::integral_constant<uint32_t, 2>{},
            std::true_type{}, std::true_type{});
      } else {
        build_fast(std::integral_constant<uint32_t, 2>{},
            std::true_type{}, std::false_type{});
      }
    } else {
      if (source_layout.source_is_planar) {
        build_fast(std::integral_constant<uint32_t, 2>{},
            std::false_type{}, std::true_type{});
      } else {
        build_fast(std::integral_constant<uint32_t, 2>{},
            std::false_type{}, std::false_type{});
      }
    }
    return PIXEL_CODEC_ERR_OK;
  default:
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "encode_frame",
        "unsupported source sample width");
  }
}

}  // namespace

pixel_error_code encoder_encode_frame_to_context_buffer(
    void* ctx, const pixel_encoder_request* request) {
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

    const pixel_error_code valid_ec = validate_encoder_request(c, request,
        &source_dtype,
        &source_layout,
        &source_signed,
        &lossless);
    if (valid_ec != PIXEL_CODEC_ERR_OK) {
      return valid_ec;
    }

    std::vector<uint8_t> interleaved{};
    const pixel_error_code normalize_ec = build_normalized_interleaved(c, request,
        source_dtype, source_layout, source_signed, &interleaved);
    if (normalize_ec != PIXEL_CODEC_ERR_OK) {
      return normalize_ec;
    }

    const std::size_t cols_sz = static_cast<std::size_t>(request->frame.cols);
    const std::size_t rows_sz = static_cast<std::size_t>(request->frame.rows);
    const std::size_t samples_sz = static_cast<std::size_t>(request->frame.samples_per_pixel);

    const int pixel_format = pixel_format_for_samples(samples_sz);
    if (pixel_format == TJPF_UNKNOWN) {
      return fail_detail(c, PIXEL_CODEC_ERR_UNSUPPORTED, "encode_frame",
          "unsupported samples_per_pixel");
    }

    const int colorspace =
        c->has_colorspace ? c->colorspace : colorspace_for_samples(samples_sz, lossless);
    if (colorspace < 0) {
      return fail_detail(c, PIXEL_CODEC_ERR_UNSUPPORTED, "encode_frame",
          "unsupported JPEG colorspace for samples_per_pixel");
    }

    const std::size_t source_stride_bytes = cols_sz * samples_sz * source_dtype.bytes;
    if ((source_stride_bytes % source_dtype.bytes) != 0) {
      return fail_detail(c, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "encode_frame",
          "source stride is not aligned to sample size");
    }

    const std::size_t source_pitch_samples = source_stride_bytes / source_dtype.bytes;
    if (cols_sz > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        rows_sz > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        source_pitch_samples > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
      return fail_detail(c, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "encode_frame",
          "rows/cols/stride exceed TurboJPEG int range");
    }

    const int pitch_samples = static_cast<int>(source_pitch_samples);

    TurboJpegHandleGuard handle(tj3Init(TJINIT_COMPRESS));
    if (handle.get() == nullptr) {
      return fail_detail(c, PIXEL_CODEC_ERR_FAILED, "encode_frame",
          "failed to initialize TurboJPEG compressor");
    }

    {
      const pixel_error_code ec =
          set_turbojpeg_param(c, handle.get(), TJPARAM_LOSSLESS, lossless ? 1 : 0, "LOSSLESS");
      if (ec != PIXEL_CODEC_ERR_OK) {
        return ec;
      }
    }
    {
      const pixel_error_code ec = set_turbojpeg_param(
          c, handle.get(), TJPARAM_PRECISION, request->frame.bits_stored, "PRECISION");
      if (ec != PIXEL_CODEC_ERR_OK) {
        return ec;
      }
    }
    {
      const pixel_error_code ec =
          set_turbojpeg_param(c, handle.get(), TJPARAM_COLORSPACE, colorspace, "COLORSPACE");
      if (ec != PIXEL_CODEC_ERR_OK) {
        return ec;
      }
    }
    {
      const int subsamp = (samples_sz == std::size_t{1})
          ? TJSAMP_GRAY
          : (c->has_subsampling
                    ? c->subsamp
                    : ((!lossless && c->has_colorspace &&
                           c->colorspace == TJCS_YCbCr)
                              ? TJSAMP_422
                              : TJSAMP_444));
      const pixel_error_code ec =
          set_turbojpeg_param(c, handle.get(), TJPARAM_SUBSAMP, subsamp, "SUBSAMP");
      if (ec != PIXEL_CODEC_ERR_OK) {
        return ec;
      }
    }

    if (lossless) {
      const pixel_error_code psv_ec =
          set_turbojpeg_param(c, handle.get(), TJPARAM_LOSSLESSPSV, 1, "LOSSLESSPSV");
      if (psv_ec != PIXEL_CODEC_ERR_OK) {
        return psv_ec;
      }
      const pixel_error_code pt_ec =
          set_turbojpeg_param(c, handle.get(), TJPARAM_LOSSLESSPT, 0, "LOSSLESSPT");
      if (pt_ec != PIXEL_CODEC_ERR_OK) {
        return pt_ec;
      }
    } else {
      const pixel_error_code quality_ec =
          set_turbojpeg_param(c, handle.get(), TJPARAM_QUALITY, c->quality, "QUALITY");
      if (quality_ec != PIXEL_CODEC_ERR_OK) {
        return quality_ec;
      }
    }

    TurboJpegBufferGuard encoded_buffer{};
    std::size_t encoded_size = 0;
    int rc = -1;

    if (request->frame.bits_stored <= 8) {
      rc = tj3Compress8(handle.get(), interleaved.data(),
          static_cast<int>(cols_sz), pitch_samples, static_cast<int>(rows_sz),
          pixel_format, encoded_buffer.out_ptr(), &encoded_size);
    } else if (request->frame.bits_stored <= 12) {
      rc = tj3Compress12(handle.get(),
          reinterpret_cast<const short*>(interleaved.data()),
          static_cast<int>(cols_sz), pitch_samples, static_cast<int>(rows_sz),
          pixel_format, encoded_buffer.out_ptr(), &encoded_size);
    } else {
      rc = tj3Compress16(handle.get(),
          reinterpret_cast<const unsigned short*>(interleaved.data()),
          static_cast<int>(cols_sz), pitch_samples, static_cast<int>(rows_sz),
          pixel_format, encoded_buffer.out_ptr(), &encoded_size);
    }

    if (rc != 0) {
      const char* msg = tj3GetErrorStr(handle.get());
      return fail_detail(c, PIXEL_CODEC_ERR_FAILED, "encode_frame",
          (msg != nullptr && msg[0] != '\0') ? msg : "JPEG encode failed");
    }

    if (encoded_buffer.get() == nullptr || encoded_size == 0) {
      return fail_detail(c, PIXEL_CODEC_ERR_FAILED, "encode_frame",
          "TurboJPEG produced empty codestream");
    }

    c->encoded_buffer.resize(encoded_size);
    std::memcpy(c->encoded_buffer.data(), encoded_buffer.get(), encoded_size);
    auto* mutable_request = const_cast<pixel_encoder_request*>(request);
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

pixel_error_code encoder_encode_frame(
    void* ctx, const pixel_encoder_request* request) {
  auto* c = static_cast<EncoderCtx*>(ctx);
  const pixel_error_code encode_ec =
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

pixel_error_code encoder_get_encoded_buffer(
    const void* ctx, pixel_const_buffer* out_encoded_buffer) {
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

}  // namespace pixel::jpeg_codec
