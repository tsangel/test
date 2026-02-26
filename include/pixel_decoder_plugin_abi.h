#ifndef DICOMSDL_PIXEL_DECODER_PLUGIN_ABI_H
#define DICOMSDL_PIXEL_DECODER_PLUGIN_ABI_H

#include "pixel_codec_plugin_abi_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DICOMSDL_DECODER_PLUGIN_ABI_V1 0x00010000u

typedef struct dicomsdl_decoder_source_v1 {
  uint32_t struct_size;
  uint32_t abi_version;
  dicomsdl_const_buffer source_buffer;
} dicomsdl_decoder_source_v1;

typedef struct dicomsdl_decoder_frame_info_v1 {
  uint32_t struct_size;
  uint32_t abi_version;

  uint16_t transfer_syntax_code;
  uint8_t source_dtype;
  uint8_t source_planar;

  int32_t rows;
  int32_t cols;
  int32_t samples_per_pixel;
  int32_t bits_stored;

  uint32_t decode_mct;
  uint32_t reserved0;
} dicomsdl_decoder_frame_info_v1;

typedef struct dicomsdl_decoder_output_v1 {
  uint32_t struct_size;
  uint32_t abi_version;

  uint8_t* dst;
  uint64_t dst_size;
  uint64_t row_stride;
  uint64_t frame_stride;

  uint8_t dst_dtype;
  uint8_t dst_planar;
  uint16_t reserved0;
} dicomsdl_decoder_output_v1;

typedef enum dicomsdl_decoder_value_transform_kind_v1 {
  DICOMSDL_DECODER_VALUE_TRANSFORM_NONE = 0,
  DICOMSDL_DECODER_VALUE_TRANSFORM_RESCALE = 1,
  DICOMSDL_DECODER_VALUE_TRANSFORM_MODALITY_LUT = 2
} dicomsdl_decoder_value_transform_kind_v1;

typedef struct dicomsdl_decoder_value_transform_v1 {
  uint32_t struct_size;
  uint32_t abi_version;

  uint32_t transform_kind;
  uint32_t reserved0;

  double rescale_slope;
  double rescale_intercept;

  int64_t lut_first_mapped;
  uint64_t lut_value_count;
  dicomsdl_const_buffer lut_values_f32;
} dicomsdl_decoder_value_transform_v1;

typedef struct dicomsdl_decoder_request_v1 {
  uint32_t struct_size;
  uint32_t abi_version;

  dicomsdl_decoder_source_v1 source;
  dicomsdl_decoder_frame_info_v1 frame;
  dicomsdl_decoder_output_v1 output;
  dicomsdl_decoder_value_transform_v1 value_transform;
} dicomsdl_decoder_request_v1;

typedef struct dicomsdl_decoder_plugin_info_v1 {
  uint32_t struct_size;
  uint32_t abi_version;
  const char* plugin_key;
  const char* display_name;
} dicomsdl_decoder_plugin_info_v1;

typedef void* (*dicomsdl_decoder_create_fn)(void);
typedef void (*dicomsdl_decoder_destroy_fn)(void* ctx);
typedef int (*dicomsdl_decoder_configure_fn)(
    void* ctx, uint16_t transfer_syntax_code,
    const dicomsdl_codec_option_list_v1* options,
    dicomsdl_codec_error_v1* error);
typedef int (*dicomsdl_decoder_decode_frame_fn)(
    void* ctx, const dicomsdl_decoder_request_v1* request,
    dicomsdl_codec_error_v1* error);

typedef struct dicomsdl_decoder_plugin_api_v1 {
  uint32_t struct_size;
  uint32_t abi_version;

  dicomsdl_decoder_plugin_info_v1 info;
  dicomsdl_decoder_create_fn create;
  dicomsdl_decoder_destroy_fn destroy;
  dicomsdl_decoder_configure_fn configure;
  dicomsdl_decoder_decode_frame_fn decode_frame;
} dicomsdl_decoder_plugin_api_v1;

int dicomsdl_get_decoder_plugin_api_v1(dicomsdl_decoder_plugin_api_v1* out_api);

#ifdef __cplusplus
}
#endif

#endif  // DICOMSDL_PIXEL_DECODER_PLUGIN_ABI_H
