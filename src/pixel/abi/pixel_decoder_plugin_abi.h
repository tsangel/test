#ifndef DICOMSDL_PIXEL_DECODER_PLUGIN_ABI_H
#define DICOMSDL_PIXEL_DECODER_PLUGIN_ABI_H

#include "pixel_codec_plugin_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PIXEL_DECODER_PLUGIN_ABI 0x00030000u

typedef struct pixel_decoder_source {
  uint32_t struct_size;
  uint32_t abi_version;
  pixel_const_buffer source_buffer;
} pixel_decoder_source;

typedef struct pixel_decoder_frame_info {
  uint32_t struct_size;
  uint32_t abi_version;

  uint32_t codec_profile_code;
  uint8_t source_dtype;
  uint8_t source_planar;
  uint16_t reserved0;

  int32_t rows;
  int32_t cols;
  int32_t samples_per_pixel;
  int32_t bits_stored;

  uint32_t decode_mct;
  uint32_t reserved1;
} pixel_decoder_frame_info;

typedef struct pixel_decoder_output {
  uint32_t struct_size;
  uint32_t abi_version;

  uint8_t* dst;
  uint64_t dst_size;
  uint64_t row_stride;
  uint64_t frame_stride;

  uint8_t dst_dtype;
  uint8_t dst_planar;
  uint16_t reserved0;
} pixel_decoder_output;

typedef struct pixel_decoder_request {
  uint32_t struct_size;
  uint32_t abi_version;

  pixel_decoder_source source;
  pixel_decoder_frame_info frame;
  pixel_decoder_output output;
} pixel_decoder_request;

typedef struct pixel_decoder_plugin_info {
  uint32_t struct_size;
  uint32_t abi_version;
  const char* display_name;
  pixel_supported_profile_flags supported_profile_flags;
} pixel_decoder_plugin_info;

typedef void* (*pixel_decoder_create_fn)(void);
typedef void (*pixel_decoder_destroy_fn)(void* ctx);
typedef pixel_error_code (*pixel_decoder_configure_fn)(
    void* ctx, uint32_t codec_profile_code, const pixel_option_list* options);
typedef pixel_error_code (*pixel_decoder_decode_frame_fn)(
    void* ctx, const pixel_decoder_request* request);

typedef struct pixel_decoder_plugin_api {
  uint32_t struct_size;
  uint32_t abi_version;

  pixel_decoder_plugin_info info;
  pixel_decoder_create_fn create;
  pixel_decoder_destroy_fn destroy;
  pixel_decoder_configure_fn configure;
  pixel_decoder_decode_frame_fn decode_frame;
  pixel_copy_last_error_detail_fn copy_last_error_detail;
} pixel_decoder_plugin_api;

#ifndef PIXEL_DECODER_PLUGIN_API_NO_ENTRYPOINT_DECL
int pixel_get_decoder_plugin_api(pixel_decoder_plugin_api* out_api);
#endif

#ifdef __cplusplus
}
#endif

#endif  // DICOMSDL_PIXEL_DECODER_PLUGIN_ABI_H
