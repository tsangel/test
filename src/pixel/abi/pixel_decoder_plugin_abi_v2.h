#ifndef DICOMSDL_PIXEL_DECODER_PLUGIN_ABI_V2_H
#define DICOMSDL_PIXEL_DECODER_PLUGIN_ABI_V2_H

#include "pixel_codec_plugin_abi_v2.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PIXEL_DECODER_PLUGIN_ABI_V2 0x00020000u

typedef struct pixel_decoder_source_v2 {
  uint32_t struct_size;
  uint32_t abi_version;
  pixel_const_buffer_v2 source_buffer;
} pixel_decoder_source_v2;

typedef struct pixel_decoder_frame_info_v2 {
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
} pixel_decoder_frame_info_v2;

typedef struct pixel_decoder_output_v2 {
  uint32_t struct_size;
  uint32_t abi_version;

  uint8_t* dst;
  uint64_t dst_size;
  uint64_t row_stride;
  uint64_t frame_stride;

  uint8_t dst_dtype;
  uint8_t dst_planar;
  uint16_t reserved0;
} pixel_decoder_output_v2;

typedef enum pixel_decoder_value_transform_kind_v2 {
  PIXEL_DECODER_VALUE_TRANSFORM_NONE_V2 = 0,
  PIXEL_DECODER_VALUE_TRANSFORM_RESCALE_V2 = 1,
  PIXEL_DECODER_VALUE_TRANSFORM_MODALITY_LUT_V2 = 2
} pixel_decoder_value_transform_kind_v2;

typedef struct pixel_decoder_value_transform_v2 {
  uint32_t struct_size;
  uint32_t abi_version;

  uint32_t transform_kind;
  uint32_t reserved0;

  double rescale_slope;
  double rescale_intercept;

  int64_t lut_first_mapped;
  uint64_t lut_value_count;
  pixel_const_buffer_v2 lut_values_f32;
} pixel_decoder_value_transform_v2;

typedef struct pixel_decoder_request_v2 {
  uint32_t struct_size;
  uint32_t abi_version;

  pixel_decoder_source_v2 source;
  pixel_decoder_frame_info_v2 frame;
  pixel_decoder_output_v2 output;
  pixel_decoder_value_transform_v2 value_transform;
} pixel_decoder_request_v2;

typedef struct pixel_decoder_plugin_info_v2 {
  uint32_t struct_size;
  uint32_t abi_version;
  const char* display_name;
  pixel_supported_profile_flags_v2 supported_profile_flags;
} pixel_decoder_plugin_info_v2;

typedef void* (*pixel_decoder_create_fn_v2)(void);
typedef void (*pixel_decoder_destroy_fn_v2)(void* ctx);
typedef pixel_error_code_v2 (*pixel_decoder_configure_fn_v2)(
    void* ctx, uint32_t codec_profile_code, const pixel_option_list_v2* options);
typedef pixel_error_code_v2 (*pixel_decoder_decode_frame_fn_v2)(
    void* ctx, const pixel_decoder_request_v2* request);

typedef struct pixel_decoder_plugin_api_v2 {
  uint32_t struct_size;
  uint32_t abi_version;

  pixel_decoder_plugin_info_v2 info;
  pixel_decoder_create_fn_v2 create;
  pixel_decoder_destroy_fn_v2 destroy;
  pixel_decoder_configure_fn_v2 configure;
  pixel_decoder_decode_frame_fn_v2 decode_frame;
  pixel_copy_last_error_detail_fn_v2 copy_last_error_detail;
} pixel_decoder_plugin_api_v2;

#ifndef PIXEL_DECODER_PLUGIN_API_NO_ENTRYPOINT_DECL
int pixel_get_decoder_plugin_api_v2(pixel_decoder_plugin_api_v2* out_api);
#endif

#ifdef __cplusplus
}
#endif

#endif  // DICOMSDL_PIXEL_DECODER_PLUGIN_ABI_V2_H
