#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "internal.hpp"

namespace pixel::openjpeg_plugin_v2 {

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

}  // namespace pixel::openjpeg_plugin_v2
