#ifndef DICOMSDL_PIXEL_ENCODER_PLUGIN_ABI_H
#define DICOMSDL_PIXEL_ENCODER_PLUGIN_ABI_H

#include "pixel_codec_plugin_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PIXEL_ENCODER_PLUGIN_ABI 0x00030000u

typedef struct pixel_encoder_source {
  uint32_t struct_size;
  uint32_t abi_version;
  pixel_const_buffer source_buffer;
} pixel_encoder_source;

typedef struct pixel_encoder_frame_info {
  uint32_t struct_size;
  uint32_t abi_version;

  uint32_t codec_profile_code;
  uint8_t source_dtype;
  uint8_t source_planar;
  uint16_t reserved0;

  int32_t rows;
  int32_t cols;
  int32_t samples_per_pixel;

  int32_t bits_allocated;
  int32_t bits_stored;
  int32_t pixel_representation;

  uint64_t source_row_stride;
  uint64_t source_plane_stride;
  uint64_t source_frame_size_bytes;

  uint32_t use_multicomponent_transform;
} pixel_encoder_frame_info;

typedef struct pixel_encoder_output {
  uint32_t struct_size;
  uint32_t abi_version;

  pixel_output_buffer encoded_buffer;
  uint64_t encoded_size;
} pixel_encoder_output;

typedef struct pixel_encoder_request {
  uint32_t struct_size;
  uint32_t abi_version;

  pixel_encoder_source source;
  pixel_encoder_frame_info frame;
  pixel_encoder_output output;
} pixel_encoder_request;

typedef struct pixel_encoder_plugin_info {
  uint32_t struct_size;
  uint32_t abi_version;
  const char* display_name;
  pixel_supported_profile_flags supported_profile_flags;
} pixel_encoder_plugin_info;

typedef void* (*pixel_encoder_create_fn)(void);
typedef void (*pixel_encoder_destroy_fn)(void* ctx);
typedef pixel_error_code (*pixel_encoder_configure_fn)(
    void* ctx, uint32_t codec_profile_code, const pixel_option_list* options);
typedef pixel_error_code (*pixel_encoder_encode_frame_fn)(
    void* ctx, const pixel_encoder_request* request);
typedef pixel_error_code (*pixel_encoder_encode_frame_to_context_buffer_fn)(
    void* ctx, const pixel_encoder_request* request);
typedef pixel_error_code (*pixel_encoder_get_encoded_buffer_fn)(
    const void* ctx, pixel_const_buffer* out_encoded_buffer);

typedef struct pixel_encoder_plugin_api {
  uint32_t struct_size;
  uint32_t abi_version;

  pixel_encoder_plugin_info info;
  pixel_encoder_create_fn create;
  pixel_encoder_destroy_fn destroy;
  pixel_encoder_configure_fn configure;
  pixel_encoder_encode_frame_fn encode_frame;
  pixel_copy_last_error_detail_fn copy_last_error_detail;
  pixel_encoder_encode_frame_to_context_buffer_fn encode_frame_to_context_buffer;
  pixel_encoder_get_encoded_buffer_fn get_encoded_buffer;
} pixel_encoder_plugin_api;

#ifndef PIXEL_ENCODER_PLUGIN_API_NO_ENTRYPOINT_DECL
int pixel_get_encoder_plugin_api(pixel_encoder_plugin_api* out_api);
#endif

#ifdef __cplusplus
}
#endif

#endif  // DICOMSDL_PIXEL_ENCODER_PLUGIN_ABI_H
