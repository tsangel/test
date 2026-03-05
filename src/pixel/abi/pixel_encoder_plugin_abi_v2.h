#ifndef DICOMSDL_PIXEL_ENCODER_PLUGIN_ABI_V2_H
#define DICOMSDL_PIXEL_ENCODER_PLUGIN_ABI_V2_H

#include "pixel_codec_plugin_abi_v2.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PIXEL_ENCODER_PLUGIN_ABI_V2 0x00020000u

typedef struct pixel_encoder_source_v2 {
  uint32_t struct_size;
  uint32_t abi_version;
  pixel_const_buffer_v2 source_buffer;
} pixel_encoder_source_v2;

typedef struct pixel_encoder_frame_info_v2 {
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
} pixel_encoder_frame_info_v2;

typedef struct pixel_encoder_output_v2 {
  uint32_t struct_size;
  uint32_t abi_version;

  pixel_output_buffer_v2 encoded_buffer;
  uint64_t encoded_size;
} pixel_encoder_output_v2;

typedef struct pixel_encoder_request_v2 {
  uint32_t struct_size;
  uint32_t abi_version;

  pixel_encoder_source_v2 source;
  pixel_encoder_frame_info_v2 frame;
  pixel_encoder_output_v2 output;
} pixel_encoder_request_v2;

typedef struct pixel_encoder_plugin_info_v2 {
  uint32_t struct_size;
  uint32_t abi_version;
  const char* display_name;
  pixel_supported_profile_flags_v2 supported_profile_flags;
} pixel_encoder_plugin_info_v2;

typedef void* (*pixel_encoder_create_fn_v2)(void);
typedef void (*pixel_encoder_destroy_fn_v2)(void* ctx);
typedef pixel_error_code_v2 (*pixel_encoder_configure_fn_v2)(
    void* ctx, uint32_t codec_profile_code, const pixel_option_list_v2* options);
typedef pixel_error_code_v2 (*pixel_encoder_encode_frame_fn_v2)(
    void* ctx, const pixel_encoder_request_v2* request);

typedef struct pixel_encoder_plugin_api_v2 {
  uint32_t struct_size;
  uint32_t abi_version;

  pixel_encoder_plugin_info_v2 info;
  pixel_encoder_create_fn_v2 create;
  pixel_encoder_destroy_fn_v2 destroy;
  pixel_encoder_configure_fn_v2 configure;
  pixel_encoder_encode_frame_fn_v2 encode_frame;
  pixel_copy_last_error_detail_fn_v2 copy_last_error_detail;
} pixel_encoder_plugin_api_v2;

#ifndef PIXEL_ENCODER_PLUGIN_API_NO_ENTRYPOINT_DECL
int pixel_get_encoder_plugin_api_v2(pixel_encoder_plugin_api_v2* out_api);
#endif

#ifdef __cplusplus
}
#endif

#endif  // DICOMSDL_PIXEL_ENCODER_PLUGIN_ABI_V2_H
