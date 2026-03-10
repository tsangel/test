#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>

#include "internal.hpp"

namespace pixel::rle_codec_v2 {

namespace {

template <typename Ctx>
void set_detail_impl(Ctx* ctx, const char* detail) {
  if (ctx == nullptr) {
    return;
  }
  copy_text(ctx->last_error_detail, kLastErrorDetailCapacity, detail);
}

template <typename Ctx>
void clear_detail_impl(Ctx* ctx) {
  set_detail_impl(ctx, "");
}

template <typename Ctx>
uint32_t copy_last_error_detail_impl_t(
    const Ctx* ctx, char* out_detail, uint32_t out_detail_capacity) {
  if (out_detail == nullptr || out_detail_capacity == 0) {
    return 0;
  }
  if (ctx == nullptr) {
    out_detail[0] = '\0';
    return 0;
  }
  copy_text(out_detail, out_detail_capacity, ctx->last_error_detail);
  return static_cast<uint32_t>(std::strlen(out_detail));
}

template <typename Ctx>
pixel_error_code_v2 fail_detail_impl(
    Ctx* ctx, pixel_error_code_v2 code, const char* stage, const char* reason) {
  char detail[kLastErrorDetailCapacity];
  std::snprintf(detail, sizeof(detail), "stage=%s;reason=%s",
      (stage != nullptr ? stage : "unknown"),
      (reason != nullptr ? reason : "unknown"));
  set_detail_impl(ctx, detail);
  return code;
}

template <typename Ctx>
pixel_error_code_v2 fail_detail_u32_impl(Ctx* ctx, pixel_error_code_v2 code,
    const char* stage, const char* reason_fmt, uint32_t value) {
  char reason[320];
  std::snprintf(reason, sizeof(reason), reason_fmt, static_cast<unsigned>(value));
  return fail_detail_impl(ctx, code, stage, reason);
}

}  // namespace

void copy_text(char* dst, std::size_t dst_capacity, const char* src) {
  if (dst == nullptr || dst_capacity == 0) {
    return;
  }
  if (src == nullptr) {
    dst[0] = '\0';
    return;
  }
  const std::size_t n = std::strlen(src);
  const std::size_t count = (n < (dst_capacity - 1)) ? n : (dst_capacity - 1);
  std::memcpy(dst, src, count);
  dst[count] = '\0';
}

void set_detail(DecoderCtx* ctx, const char* detail) {
  set_detail_impl(ctx, detail);
}

void set_detail(EncoderCtx* ctx, const char* detail) {
  set_detail_impl(ctx, detail);
}

void clear_detail(DecoderCtx* ctx) {
  clear_detail_impl(ctx);
}

void clear_detail(EncoderCtx* ctx) {
  clear_detail_impl(ctx);
}

uint32_t copy_last_error_detail_impl(
    const DecoderCtx* ctx, char* out_detail, uint32_t out_detail_capacity) {
  return copy_last_error_detail_impl_t(ctx, out_detail, out_detail_capacity);
}

uint32_t copy_last_error_detail_impl(
    const EncoderCtx* ctx, char* out_detail, uint32_t out_detail_capacity) {
  return copy_last_error_detail_impl_t(ctx, out_detail, out_detail_capacity);
}

pixel_error_code_v2 fail_detail(
    DecoderCtx* ctx, pixel_error_code_v2 code, const char* stage, const char* reason) {
  return fail_detail_impl(ctx, code, stage, reason);
}

pixel_error_code_v2 fail_detail(
    EncoderCtx* ctx, pixel_error_code_v2 code, const char* stage, const char* reason) {
  return fail_detail_impl(ctx, code, stage, reason);
}

pixel_error_code_v2 fail_detail_u32(DecoderCtx* ctx, pixel_error_code_v2 code,
    const char* stage, const char* reason_fmt, uint32_t value) {
  return fail_detail_u32_impl(ctx, code, stage, reason_fmt, value);
}

pixel_error_code_v2 fail_detail_u32(EncoderCtx* ctx, pixel_error_code_v2 code,
    const char* stage, const char* reason_fmt, uint32_t value) {
  return fail_detail_u32_impl(ctx, code, stage, reason_fmt, value);
}

namespace {

constexpr pixel_supported_profile_flags_v2 kSupportedProfileFlags =
    PIXEL_CODEC_PROFILE_RLE_LOSSLESS_DEC_V2 |
    PIXEL_CODEC_PROFILE_RLE_LOSSLESS_ENC_V2;

}  // namespace

pixel_supported_profile_flags_v2 supported_profile_flags() {
  return kSupportedProfileFlags;
}

bool is_supported_decoder_profile(uint32_t codec_profile_code) {
  if (codec_profile_code > 31u) {
    return false;
  }
  const pixel_supported_profile_flags_v2 bit =
      PIXEL_CODEC_PROFILE_DEC_FLAG_V2(codec_profile_code);
  return (supported_profile_flags() & bit) != 0;
}

bool is_supported_encoder_profile(uint32_t codec_profile_code) {
  if (codec_profile_code > 31u) {
    return false;
  }
  const pixel_supported_profile_flags_v2 bit =
      PIXEL_CODEC_PROFILE_ENC_FLAG_V2(codec_profile_code);
  return (supported_profile_flags() & bit) != 0;
}

bool mul_u64(uint64_t a, uint64_t b, uint64_t* out) {
  if (out == nullptr) {
    return false;
  }
  if (a != 0 && b > (std::numeric_limits<uint64_t>::max() / a)) {
    return false;
  }
  *out = a * b;
  return true;
}

bool u64_to_size(uint64_t value, std::size_t* out) {
  if (out == nullptr) {
    return false;
  }
  if (value > static_cast<uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return false;
  }
  *out = static_cast<std::size_t>(value);
  return true;
}

bool dtype_info_from_code(uint8_t code, DtypeInfo* out) {
  if (out == nullptr) {
    return false;
  }
  switch (code) {
  case PIXEL_DTYPE_U8_V2:
    *out = DtypeInfo{1, false, false};
    return true;
  case PIXEL_DTYPE_S8_V2:
    *out = DtypeInfo{1, true, false};
    return true;
  case PIXEL_DTYPE_U16_V2:
    *out = DtypeInfo{2, false, false};
    return true;
  case PIXEL_DTYPE_S16_V2:
    *out = DtypeInfo{2, true, false};
    return true;
  case PIXEL_DTYPE_U32_V2:
    *out = DtypeInfo{4, false, false};
    return true;
  case PIXEL_DTYPE_S32_V2:
    *out = DtypeInfo{4, true, false};
    return true;
  case PIXEL_DTYPE_F32_V2:
    *out = DtypeInfo{4, true, true};
    return true;
  case PIXEL_DTYPE_F64_V2:
    *out = DtypeInfo{8, true, true};
    return true;
  default:
    return false;
  }
}

bool is_valid_planar_code(uint8_t code) {
  return code == PIXEL_PLANAR_INTERLEAVED_V2 || code == PIXEL_PLANAR_PLANAR_V2;
}

bool is_planar_code(uint8_t code) {
  return code == PIXEL_PLANAR_PLANAR_V2;
}

pixel_error_code_v2 parse_decoder_options(
    DecoderCtx* ctx, const pixel_option_list_v2* options) {
  if (options == nullptr) {
    return PIXEL_CODEC_ERR_OK;
  }
  if (options->count > 0 && options->items == nullptr) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "parse_options",
        "option list has count but items is null");
  }
  if (options->count == 0) {
    return PIXEL_CODEC_ERR_OK;
  }
  return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "parse_options",
      "RLE decoder does not accept options");
}

pixel_error_code_v2 parse_encoder_options(
    EncoderCtx* ctx, const pixel_option_list_v2* options) {
  if (options == nullptr) {
    return PIXEL_CODEC_ERR_OK;
  }
  if (options->count > 0 && options->items == nullptr) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "parse_options",
        "option list has count but items is null");
  }
  if (options->count == 0) {
    return PIXEL_CODEC_ERR_OK;
  }
  return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "parse_options",
      "RLE encoder does not accept options");
}

void* decoder_create() {
  return new (std::nothrow) DecoderCtx{};
}

void decoder_destroy(void* ctx) {
  delete static_cast<DecoderCtx*>(ctx);
}

pixel_error_code_v2 decoder_configure(void* ctx, uint32_t codec_profile_code,
    const pixel_option_list_v2* options) {
  auto* c = static_cast<DecoderCtx*>(ctx);
  if (c == nullptr) {
    return PIXEL_CODEC_ERR_INVALID_ARGUMENT;
  }
  if (!is_supported_decoder_profile(codec_profile_code)) {
    return fail_detail_u32(c, PIXEL_CODEC_ERR_UNSUPPORTED, "configure",
        "unsupported decoder codec_profile_code=%u for rle",
        codec_profile_code);
  }
  const pixel_error_code_v2 ec = parse_decoder_options(c, options);
  if (ec != PIXEL_CODEC_ERR_OK) {
    return ec;
  }
  c->codec_profile_code = codec_profile_code;
  c->configured = true;
  clear_detail(c);
  return PIXEL_CODEC_ERR_OK;
}

uint32_t decoder_copy_last_error_detail(
    const void* ctx, char* out_detail, uint32_t out_detail_capacity) {
  return copy_last_error_detail_impl(
      static_cast<const DecoderCtx*>(ctx), out_detail, out_detail_capacity);
}

void* encoder_create() {
  return new (std::nothrow) EncoderCtx{};
}

void encoder_destroy(void* ctx) {
  delete static_cast<EncoderCtx*>(ctx);
}

pixel_error_code_v2 encoder_configure(void* ctx, uint32_t codec_profile_code,
    const pixel_option_list_v2* options) {
  auto* c = static_cast<EncoderCtx*>(ctx);
  if (c == nullptr) {
    return PIXEL_CODEC_ERR_INVALID_ARGUMENT;
  }
  if (!is_supported_encoder_profile(codec_profile_code)) {
    return fail_detail_u32(c, PIXEL_CODEC_ERR_UNSUPPORTED, "configure",
        "unsupported encoder codec_profile_code=%u for rle",
        codec_profile_code);
  }
  const pixel_error_code_v2 ec = parse_encoder_options(c, options);
  if (ec != PIXEL_CODEC_ERR_OK) {
    return ec;
  }
  c->codec_profile_code = codec_profile_code;
  c->configured = true;
  clear_detail(c);
  return PIXEL_CODEC_ERR_OK;
}

uint32_t encoder_copy_last_error_detail(
    const void* ctx, char* out_detail, uint32_t out_detail_capacity) {
  return copy_last_error_detail_impl(
      static_cast<const EncoderCtx*>(ctx), out_detail, out_detail_capacity);
}

}  // namespace pixel::rle_codec_v2
