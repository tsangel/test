#ifndef DICOMSDL_PIXEL_DECODER_PLUGIN_ABI_H
#define DICOMSDL_PIXEL_DECODER_PLUGIN_ABI_H

#include "pixel_codec_plugin_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PIXEL_DECODER_PLUGIN_ABI 0x00050000u

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
  // Host-populated optional source photometric hint (low byte).
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

typedef enum pixel_decoded_color_space_code {
  PIXEL_DECODED_COLOR_SPACE_UNKNOWN = 0,
  PIXEL_DECODED_COLOR_SPACE_MONOCHROME = 1,
  PIXEL_DECODED_COLOR_SPACE_RGB = 2,
  PIXEL_DECODED_COLOR_SPACE_CMYK = 3,
  PIXEL_DECODED_COLOR_SPACE_YBR_FULL = 4,
  PIXEL_DECODED_COLOR_SPACE_YBR_FULL_422 = 5,
  PIXEL_DECODED_COLOR_SPACE_YBR_PARTIAL_420 = 6,
  PIXEL_DECODED_COLOR_SPACE_YBR_PARTIAL_422 = 7,
  PIXEL_DECODED_COLOR_SPACE_RGBA = 8,
  PIXEL_DECODED_COLOR_SPACE_PALETTE_COLOR = 9,
  PIXEL_DECODED_COLOR_SPACE_YBR_RCT = 10,
  PIXEL_DECODED_COLOR_SPACE_YBR_ICT = 11,
  PIXEL_DECODED_COLOR_SPACE_XYB = 12,
  PIXEL_DECODED_COLOR_SPACE_HSV = 13,
  PIXEL_DECODED_COLOR_SPACE_ARGB = 14
} pixel_decoded_color_space_code;

typedef enum pixel_encoded_lossy_state {
  PIXEL_ENCODED_LOSSY_STATE_UNKNOWN = 0,
  PIXEL_ENCODED_LOSSY_STATE_LOSSLESS = 1,
  PIXEL_ENCODED_LOSSY_STATE_LOSSY = 2,
  PIXEL_ENCODED_LOSSY_STATE_NEAR_LOSSLESS = 3
} pixel_encoded_lossy_state;

typedef enum pixel_decoded_planar_code {
  PIXEL_DECODED_PLANAR_UNKNOWN = 0,
  PIXEL_DECODED_PLANAR_INTERLEAVED = 1,
  PIXEL_DECODED_PLANAR_PLANAR = 2
} pixel_decoded_planar_code;

typedef struct pixel_decoder_info {
  uint32_t struct_size;
  uint32_t abi_version;
  uint8_t actual_color_space;
  uint8_t encoded_lossy_state;
  uint8_t actual_dtype;
  uint8_t actual_planar;
  uint16_t bits_per_sample;
  uint16_t reserved0;
} pixel_decoder_info;

typedef struct pixel_decoder_request {
  uint32_t struct_size;
  uint32_t abi_version;

  pixel_decoder_source source;
  pixel_decoder_frame_info frame;
  pixel_decoder_output output;
  pixel_decoder_info* decode_info;
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
