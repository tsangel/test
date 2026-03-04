#pragma once

#include <cstddef>
#include <cstdint>

#include "pixel_decoder_plugin_abi_v2.h"
#include "pixel_encoder_plugin_abi_v2.h"

namespace pixel::htj2k_plugin_v2 {

constexpr std::size_t kLastErrorDetailCapacity = 512;

struct DtypeInfo {
  uint32_t bytes;
  bool is_signed;
  bool is_float;
};

struct DecoderCtx {
  uint32_t codec_profile_code{PIXEL_CODEC_PROFILE_UNKNOWN_V2};
  bool configured{false};
  char last_error_detail[kLastErrorDetailCapacity]{};
};

struct EncoderCtx {
  uint32_t codec_profile_code{PIXEL_CODEC_PROFILE_UNKNOWN_V2};
  double qstep{0.01};
  bool configured{false};
  char last_error_detail[kLastErrorDetailCapacity]{};
};

void copy_text(char* dst, std::size_t dst_capacity, const char* src);

void set_detail(DecoderCtx* ctx, const char* detail);
void set_detail(EncoderCtx* ctx, const char* detail);
void clear_detail(DecoderCtx* ctx);
void clear_detail(EncoderCtx* ctx);

uint32_t copy_last_error_detail_impl(
    const DecoderCtx* ctx, char* out_detail, uint32_t out_detail_capacity);
uint32_t copy_last_error_detail_impl(
    const EncoderCtx* ctx, char* out_detail, uint32_t out_detail_capacity);

pixel_error_code_v2 fail_detail(
    DecoderCtx* ctx, pixel_error_code_v2 code, const char* stage, const char* reason);
pixel_error_code_v2 fail_detail(
    EncoderCtx* ctx, pixel_error_code_v2 code, const char* stage, const char* reason);

pixel_error_code_v2 fail_detail_u32(DecoderCtx* ctx, pixel_error_code_v2 code,
    const char* stage, const char* reason_fmt, uint32_t value);
pixel_error_code_v2 fail_detail_u32(EncoderCtx* ctx, pixel_error_code_v2 code,
    const char* stage, const char* reason_fmt, uint32_t value);

pixel_supported_profile_flags_v2 supported_profile_flags();
bool is_supported_decoder_profile(uint32_t codec_profile_code);
bool is_supported_encoder_profile(uint32_t codec_profile_code);

bool parse_int_option(const char* value, int* out_value);
bool parse_double_option(const char* value, double* out_value);
bool mul_u64(uint64_t a, uint64_t b, uint64_t* out);

bool dtype_info_from_code(uint8_t code, DtypeInfo* out);
bool is_valid_planar_code(uint8_t code);
bool is_planar_code(uint8_t code);

bool write_integer_sample(uint8_t dst_dtype, int32_t sample, uint8_t* dst);
bool write_float_sample(uint8_t dst_dtype, double sample, uint8_t* dst);

pixel_error_code_v2 parse_decoder_options(
    DecoderCtx* ctx, const pixel_option_list_v2* options);
pixel_error_code_v2 parse_encoder_options(
    EncoderCtx* ctx, const pixel_option_list_v2* options);

void* decoder_create();
void decoder_destroy(void* ctx);
pixel_error_code_v2 decoder_configure(void* ctx, uint32_t codec_profile_code,
    const pixel_option_list_v2* options);
pixel_error_code_v2 decoder_decode_frame(
    void* ctx, const pixel_decoder_request_v2* request);
uint32_t decoder_copy_last_error_detail(
    const void* ctx, char* out_detail, uint32_t out_detail_capacity);

void* encoder_create();
void encoder_destroy(void* ctx);
pixel_error_code_v2 encoder_configure(void* ctx, uint32_t codec_profile_code,
    const pixel_option_list_v2* options);
pixel_error_code_v2 encoder_encode_frame(
    void* ctx, const pixel_encoder_request_v2* request);
uint32_t encoder_copy_last_error_detail(
    const void* ctx, char* out_detail, uint32_t out_detail_capacity);

}  // namespace pixel::htj2k_plugin_v2
