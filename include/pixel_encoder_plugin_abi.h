#ifndef DICOMSDL_PIXEL_ENCODER_PLUGIN_ABI_H
#define DICOMSDL_PIXEL_ENCODER_PLUGIN_ABI_H

#include "pixel_codec_plugin_abi_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DICOMSDL_ENCODER_PLUGIN_ABI_V1 0x00010000u

typedef struct dicomsdl_encoder_source_v1 {
  uint32_t struct_size;
  uint32_t abi_version;
  dicomsdl_const_buffer source_buffer;
} dicomsdl_encoder_source_v1;

typedef struct dicomsdl_encoder_frame_info_v1 {
  uint32_t struct_size;
  uint32_t abi_version;

  uint16_t transfer_syntax_code;
  uint8_t source_dtype;
  uint8_t source_planar;

  int32_t rows;
  int32_t cols;
  int32_t samples_per_pixel;

  int32_t bits_allocated;
  int32_t bits_stored;
  int32_t pixel_representation;

  uint64_t source_row_stride;
  uint64_t source_plane_stride;
  uint64_t source_frame_size_bytes;

  uint32_t codec_profile_code;
  uint32_t use_multicomponent_transform;
} dicomsdl_encoder_frame_info_v1;

typedef struct dicomsdl_encoder_output_v1 {
  uint32_t struct_size;
  uint32_t abi_version;

  dicomsdl_buffer encoded_buffer;
  uint64_t encoded_size;
} dicomsdl_encoder_output_v1;

typedef struct dicomsdl_encoder_request_v1 {
  uint32_t struct_size;
  uint32_t abi_version;

  dicomsdl_encoder_source_v1 source;
  dicomsdl_encoder_frame_info_v1 frame;
  dicomsdl_encoder_output_v1 output;
} dicomsdl_encoder_request_v1;

typedef struct dicomsdl_encoder_plugin_info_v1 {
  uint32_t struct_size;
  uint32_t abi_version;
  const char* plugin_key;
  const char* display_name;
} dicomsdl_encoder_plugin_info_v1;

typedef void* (*dicomsdl_encoder_create_fn)(void);
typedef void (*dicomsdl_encoder_destroy_fn)(void* ctx);
typedef int (*dicomsdl_encoder_configure_fn)(
    void* ctx, uint16_t transfer_syntax_code,
    const dicomsdl_codec_option_list_v1* options,
    dicomsdl_codec_error_v1* error);
typedef int (*dicomsdl_encoder_encode_frame_fn)(
    void* ctx, const dicomsdl_encoder_request_v1* request,
    dicomsdl_codec_error_v1* error);

typedef struct dicomsdl_encoder_plugin_api_v1 {
  uint32_t struct_size;
  uint32_t abi_version;

  dicomsdl_encoder_plugin_info_v1 info;
  dicomsdl_encoder_create_fn create;
  dicomsdl_encoder_destroy_fn destroy;
  dicomsdl_encoder_configure_fn configure;
  dicomsdl_encoder_encode_frame_fn encode_frame;
} dicomsdl_encoder_plugin_api_v1;

#ifndef DICOMSDL_ENCODER_PLUGIN_API_NO_ENTRYPOINT_DECL
int dicomsdl_get_encoder_plugin_api_v1(dicomsdl_encoder_plugin_api_v1* out_api);
#endif

#ifdef __cplusplus
}
#endif

#endif  // DICOMSDL_PIXEL_ENCODER_PLUGIN_ABI_H
