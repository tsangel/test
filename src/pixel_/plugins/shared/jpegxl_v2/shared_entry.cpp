#include "internal.hpp"

#if defined(_WIN32)
#define PIXEL_PLUGIN_EXPORT __declspec(dllexport)
#else
#define PIXEL_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

extern "C" {

PIXEL_PLUGIN_EXPORT int pixel_get_decoder_plugin_api_v2(
    pixel_decoder_plugin_api_v2* out_api) {
  if (out_api == nullptr) {
    return 0;
  }
  if (out_api->abi_version != PIXEL_DECODER_PLUGIN_ABI_V2 ||
      out_api->struct_size < sizeof(pixel_decoder_plugin_api_v2)) {
    return 0;
  }

  pixel_decoder_plugin_api_v2 api{};
  api.struct_size = sizeof(pixel_decoder_plugin_api_v2);
  api.abi_version = PIXEL_DECODER_PLUGIN_ABI_V2;
  api.info.struct_size = sizeof(pixel_decoder_plugin_info_v2);
  api.info.abi_version = PIXEL_DECODER_PLUGIN_ABI_V2;
  api.info.display_name = "JPEG-XL v2 Decoder";
  api.info.supported_profile_flags = pixel::jpegxl_plugin_v2::supported_profile_flags();
  api.create = &pixel::jpegxl_plugin_v2::decoder_create;
  api.destroy = &pixel::jpegxl_plugin_v2::decoder_destroy;
  api.configure = &pixel::jpegxl_plugin_v2::decoder_configure;
  api.decode_frame = &pixel::jpegxl_plugin_v2::decoder_decode_frame;
  api.copy_last_error_detail = &pixel::jpegxl_plugin_v2::decoder_copy_last_error_detail;
  *out_api = api;
  return 1;
}

PIXEL_PLUGIN_EXPORT int pixel_get_encoder_plugin_api_v2(
    pixel_encoder_plugin_api_v2* out_api) {
  if (out_api == nullptr) {
    return 0;
  }
  if (out_api->abi_version != PIXEL_ENCODER_PLUGIN_ABI_V2 ||
      out_api->struct_size < sizeof(pixel_encoder_plugin_api_v2)) {
    return 0;
  }

  pixel_encoder_plugin_api_v2 api{};
  api.struct_size = sizeof(pixel_encoder_plugin_api_v2);
  api.abi_version = PIXEL_ENCODER_PLUGIN_ABI_V2;
  api.info.struct_size = sizeof(pixel_encoder_plugin_info_v2);
  api.info.abi_version = PIXEL_ENCODER_PLUGIN_ABI_V2;
  api.info.display_name = "JPEG-XL v2 Encoder";
  api.info.supported_profile_flags = pixel::jpegxl_plugin_v2::supported_profile_flags();
  api.create = &pixel::jpegxl_plugin_v2::encoder_create;
  api.destroy = &pixel::jpegxl_plugin_v2::encoder_destroy;
  api.configure = &pixel::jpegxl_plugin_v2::encoder_configure;
  api.encode_frame = &pixel::jpegxl_plugin_v2::encoder_encode_frame;
  api.copy_last_error_detail = &pixel::jpegxl_plugin_v2::encoder_copy_last_error_detail;
  *out_api = api;
  return 1;
}

}  // extern "C"
