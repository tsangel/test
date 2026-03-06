#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>

#include "internal.hpp"

namespace pixel::openjpeg_plugin_v2 {

pixel_error_code_v2 parse_decoder_options(
    DecoderCtx* ctx, const pixel_option_list_v2* options) {
  if (options == nullptr) {
    return PIXEL_CODEC_ERR_OK;
  }
  if (options->count > 0 && options->items == nullptr) {
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "parse_options",
        "option list has count but items is null");
  }
  for (uint32_t i = 0; i < options->count; ++i) {
    const pixel_option_kv_v2& kv = options->items[i];
    if (kv.key == nullptr || kv.value == nullptr) {
      return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "parse_options",
          "option key/value must not be null");
    }
    if (std::strcmp(kv.key, "threads") == 0) {
      int parsed = 0;
      if (!parse_int_option(kv.value, &parsed)) {
        return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "parse_options",
            "threads must be an integer");
      }
      if (parsed < -1) {
        return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "parse_options",
            "threads must be -1, 0, or positive");
      }
      ctx->threads = parsed;
      continue;
    }
    char reason[256];
    std::snprintf(reason, sizeof(reason), "unknown option '%s' for decoder", kv.key);
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "parse_options", reason);
  }
  return PIXEL_CODEC_ERR_OK;
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
  for (uint32_t i = 0; i < options->count; ++i) {
    const pixel_option_kv_v2& kv = options->items[i];
    if (kv.key == nullptr || kv.value == nullptr) {
      return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "parse_options",
          "option key/value must not be null");
    }
    if (std::strcmp(kv.key, "threads") == 0) {
      int parsed = 0;
      if (!parse_int_option(kv.value, &parsed)) {
        return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "parse_options",
            "threads must be an integer");
      }
      if (parsed < -1) {
        return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "parse_options",
            "threads must be -1, 0, or positive");
      }
      ctx->threads = parsed;
      continue;
    }
    if (std::strcmp(kv.key, "quality") == 0) {
      int parsed = 0;
      if (!parse_int_option(kv.value, &parsed)) {
        return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "parse_options",
            "quality must be an integer");
      }
      if (parsed < 1 || parsed > 100) {
        return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "parse_options",
            "quality must be in [1,100]");
      }
      ctx->quality = parsed;
      ctx->has_quality = true;
      continue;
    }
    if (std::strcmp(kv.key, "target_bpp") == 0) {
      double parsed = 0.0;
      if (!parse_double_option(kv.value, &parsed)) {
        return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "parse_options",
            "target_bpp must be numeric");
      }
      if (parsed < 0.0) {
        return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "parse_options",
            "target_bpp must be >= 0");
      }
      ctx->target_bpp = parsed;
      continue;
    }
    if (std::strcmp(kv.key, "target_psnr") == 0) {
      double parsed = 0.0;
      if (!parse_double_option(kv.value, &parsed)) {
        return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "parse_options",
            "target_psnr must be numeric");
      }
      if (parsed < 0.0) {
        return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "parse_options",
            "target_psnr must be >= 0");
      }
      ctx->target_psnr = parsed;
      continue;
    }
    char reason[256];
    std::snprintf(reason, sizeof(reason), "unknown option '%s' for encoder", kv.key);
    return fail_detail(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "parse_options", reason);
  }
  return PIXEL_CODEC_ERR_OK;
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
        "unsupported decoder codec_profile_code=%u for openjpeg",
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
        "unsupported encoder codec_profile_code=%u for openjpeg",
        codec_profile_code);
  }
  c->quality = 92;
  c->has_quality = false;
  c->threads = -1;
  c->target_bpp = 0.0;
  c->target_psnr = 0.0;
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

}  // namespace pixel::openjpeg_plugin_v2
