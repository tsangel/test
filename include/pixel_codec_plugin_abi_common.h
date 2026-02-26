#ifndef DICOMSDL_PIXEL_CODEC_PLUGIN_ABI_COMMON_H
#define DICOMSDL_PIXEL_CODEC_PLUGIN_ABI_COMMON_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DICOMSDL_CODEC_PLUGIN_ABI_V1 0x00010000u
#define DICOMSDL_TRANSFER_SYNTAX_CODE_INVALID 0xffffu

typedef enum dicomsdl_codec_status_code {
  DICOMSDL_CODEC_OK = 0,
  DICOMSDL_CODEC_INVALID_ARGUMENT = 1,
  DICOMSDL_CODEC_UNSUPPORTED = 2,
  DICOMSDL_CODEC_BACKEND_ERROR = 3,
  DICOMSDL_CODEC_INTERNAL_ERROR = 4,
  DICOMSDL_CODEC_OUTPUT_TOO_SMALL = 5
} dicomsdl_codec_status_code;

typedef enum dicomsdl_codec_stage_code {
  DICOMSDL_CODEC_STAGE_UNKNOWN = 0,
  DICOMSDL_CODEC_STAGE_PLUGIN_LOOKUP = 1,
  DICOMSDL_CODEC_STAGE_PARSE_OPTIONS = 2,
  DICOMSDL_CODEC_STAGE_VALIDATE = 3,
  DICOMSDL_CODEC_STAGE_LOAD_FRAME_SOURCE = 4,
  DICOMSDL_CODEC_STAGE_ENCODE_FRAME = 5,
  DICOMSDL_CODEC_STAGE_DECODE_FRAME = 6,
  DICOMSDL_CODEC_STAGE_POSTPROCESS = 7,
  DICOMSDL_CODEC_STAGE_ALLOCATE = 8
} dicomsdl_codec_stage_code;

typedef struct dicomsdl_codec_error_v1 {
  uint32_t struct_size;
  uint32_t abi_version;

  uint32_t status_code;
  uint32_t stage_code;

  char* detail;
  uint32_t detail_capacity;
  uint32_t detail_length;
} dicomsdl_codec_error_v1;

typedef struct dicomsdl_const_buffer {
  const uint8_t* data;
  uint64_t size;
} dicomsdl_const_buffer;

typedef struct dicomsdl_buffer {
  uint8_t* data;
  uint64_t size;
} dicomsdl_buffer;

typedef struct dicomsdl_codec_option_kv_v1 {
  const char* key;
  const char* value;
} dicomsdl_codec_option_kv_v1;

typedef struct dicomsdl_codec_option_list_v1 {
  const dicomsdl_codec_option_kv_v1* items;
  uint32_t count;
} dicomsdl_codec_option_list_v1;

typedef enum dicomsdl_planar_code_v1 {
  DICOMSDL_PLANAR_INTERLEAVED = 0,
  DICOMSDL_PLANAR_PLANAR = 1
} dicomsdl_planar_code_v1;

typedef enum dicomsdl_dtype_code_v1 {
  DICOMSDL_DTYPE_UNKNOWN = 0,
  DICOMSDL_DTYPE_U8 = 1,
  DICOMSDL_DTYPE_S8 = 2,
  DICOMSDL_DTYPE_U16 = 3,
  DICOMSDL_DTYPE_S16 = 4,
  DICOMSDL_DTYPE_U32 = 5,
  DICOMSDL_DTYPE_S32 = 6,
  DICOMSDL_DTYPE_F32 = 7,
  DICOMSDL_DTYPE_F64 = 8
} dicomsdl_dtype_code_v1;

typedef enum dicomsdl_codec_profile_code_v1 {
  DICOMSDL_CODEC_PROFILE_UNKNOWN = 0,
  DICOMSDL_CODEC_PROFILE_NATIVE_UNCOMPRESSED = 1,
  DICOMSDL_CODEC_PROFILE_ENCAPSULATED_UNCOMPRESSED = 2,
  DICOMSDL_CODEC_PROFILE_RLE_LOSSLESS = 3,
  DICOMSDL_CODEC_PROFILE_JPEG_LOSSLESS = 4,
  DICOMSDL_CODEC_PROFILE_JPEG_LOSSY = 5,
  DICOMSDL_CODEC_PROFILE_JPEGLS_LOSSLESS = 6,
  DICOMSDL_CODEC_PROFILE_JPEGLS_NEAR_LOSSLESS = 7,
  DICOMSDL_CODEC_PROFILE_JPEG2000_LOSSLESS = 8,
  DICOMSDL_CODEC_PROFILE_JPEG2000_LOSSY = 9,
  DICOMSDL_CODEC_PROFILE_HTJ2K_LOSSLESS = 10,
  DICOMSDL_CODEC_PROFILE_HTJ2K_LOSSLESS_RPCL = 11,
  DICOMSDL_CODEC_PROFILE_HTJ2K_LOSSY = 12,
  DICOMSDL_CODEC_PROFILE_JPEGXL_LOSSLESS = 13,
  DICOMSDL_CODEC_PROFILE_JPEGXL_LOSSY = 14,
  DICOMSDL_CODEC_PROFILE_JPEGXL_JPEG_RECOMPRESSION = 15
} dicomsdl_codec_profile_code_v1;

#ifdef __cplusplus
}
#endif

#endif  // DICOMSDL_PIXEL_CODEC_PLUGIN_ABI_COMMON_H
