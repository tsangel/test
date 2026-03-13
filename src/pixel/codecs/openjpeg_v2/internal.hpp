#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <openjpeg.h>

#include "pixel_decoder_plugin_abi_v2.h"
#include "pixel_encoder_plugin_abi_v2.h"

namespace pixel::openjpeg_codec_v2 {

constexpr std::size_t kLastErrorDetailCapacity = 512;

struct DtypeInfo {
  uint32_t bytes;
  bool is_signed;
  bool is_float;
};

struct OpenJpegLogSink {
  std::string warning;
  std::string error;
};

class OpjStreamDeleter {
public:
  void operator()(opj_stream_t* stream) const noexcept {
    if (stream != nullptr) {
      opj_stream_destroy(stream);
    }
  }
};

class OpjCodecDeleter {
public:
  void operator()(opj_codec_t* codec) const noexcept {
    if (codec != nullptr) {
      opj_destroy_codec(codec);
    }
  }
};

class OpjImageDeleter {
public:
  void operator()(opj_image_t* image) const noexcept {
    if (image != nullptr) {
      opj_image_destroy(image);
    }
  }
};

using opj_stream_ptr = std::unique_ptr<opj_stream_t, OpjStreamDeleter>;
using opj_codec_ptr = std::unique_ptr<opj_codec_t, OpjCodecDeleter>;
using opj_image_ptr = std::unique_ptr<opj_image_t, OpjImageDeleter>;

struct DecodeStreamContext {
  const uint8_t* data{nullptr};
  std::size_t size{0};
  std::size_t position{0};
};

struct EncodeStreamContext {
  std::vector<uint8_t> bytes;
  std::size_t position{0};
};

struct DecoderCtx {
  uint32_t codec_profile_code{PIXEL_CODEC_PROFILE_UNKNOWN_V2};
  bool configured{false};
  int threads{-1};
  char last_error_detail[kLastErrorDetailCapacity]{};
};

struct EncoderCtx {
  uint32_t codec_profile_code{PIXEL_CODEC_PROFILE_UNKNOWN_V2};
  bool configured{false};
  int quality{92};
  bool has_quality{false};
  int threads{-1};
  double target_bpp{0.0};
  double target_psnr{0.0};
  std::vector<std::uint8_t> encoded_buffer{};
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

bool load_integral_sample_from_le(const uint8_t* src, uint32_t bytes,
    bool is_signed, int bits_stored, int32_t* out_value, char* reason,
    std::size_t reason_capacity);

bool write_integer_sample(uint8_t dst_dtype, int32_t sample, uint8_t* dst);
bool write_float_sample(uint8_t dst_dtype, double sample, uint8_t* dst);

std::string trim_trailing_ws(std::string text);

void OPJ_CALLCONV opj_warning_handler(const char* message, void* user_data);
void OPJ_CALLCONV opj_error_handler(const char* message, void* user_data);

std::string openjpeg_failure_message(
    const OpenJpegLogSink& sink, const char* fallback);

OPJ_SIZE_T OPJ_CALLCONV opj_read_from_memory(
    void* out_buffer, OPJ_SIZE_T bytes_to_read, void* user_data);
OPJ_OFF_T OPJ_CALLCONV opj_skip_in_memory(OPJ_OFF_T bytes_to_skip, void* user_data);
OPJ_BOOL OPJ_CALLCONV opj_seek_in_memory(OPJ_OFF_T absolute_position, void* user_data);
opj_stream_ptr create_decode_stream(DecodeStreamContext* context);

OPJ_SIZE_T OPJ_CALLCONV opj_write_to_memory(
    void* in_buffer, OPJ_SIZE_T bytes_to_write, void* user_data);
OPJ_OFF_T OPJ_CALLCONV opj_skip_in_output(OPJ_OFF_T bytes_to_skip, void* user_data);
OPJ_BOOL OPJ_CALLCONV opj_seek_in_output(OPJ_OFF_T absolute_position, void* user_data);
opj_stream_ptr create_encode_stream(EncodeStreamContext* context);

bool resolve_thread_count(int configured_threads, OPJ_UINT32* out_thread_count);

bool decode_with_openjpeg_format(const uint8_t* data, std::size_t size,
    OPJ_CODEC_FORMAT format, OPJ_UINT32 thread_count, opj_image_ptr* out_image,
    std::string* out_error);
bool decode_with_openjpeg_auto(const uint8_t* data, std::size_t size,
    OPJ_UINT32 thread_count, opj_image_ptr* out_image, std::string* out_error);

int max_num_resolutions_for_image(std::size_t rows, std::size_t cols);
float quality_to_openjpeg_rate(int quality);
OPJ_COLOR_SPACE resolve_color_space(std::size_t samples_per_pixel);

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
pixel_error_code_v2 encoder_encode_frame_to_context_buffer(
    void* ctx, const pixel_encoder_request_v2* request);
pixel_error_code_v2 encoder_encode_frame(
    void* ctx, const pixel_encoder_request_v2* request);
pixel_error_code_v2 encoder_get_encoded_buffer(
    const void* ctx, pixel_const_buffer_v2* out_encoded_buffer);
uint32_t encoder_copy_last_error_detail(
    const void* ctx, char* out_detail, uint32_t out_detail_capacity);

}  // namespace pixel::openjpeg_codec_v2
