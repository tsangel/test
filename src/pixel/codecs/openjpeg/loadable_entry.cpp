#define PIXEL_DECODER_PLUGIN_API_NO_ENTRYPOINT_DECL
#define PIXEL_ENCODER_PLUGIN_API_NO_ENTRYPOINT_DECL
#include "internal.hpp"

#if defined(_WIN32)
#define PIXEL_PLUGIN_EXPORT __declspec(dllexport)
#else
#define PIXEL_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

namespace {

int build_decoder_plugin_api(pixel_decoder_plugin_api* out_api) {
  if (out_api == nullptr) {
    return 0;
  }
  if (out_api->abi_version != PIXEL_DECODER_PLUGIN_ABI ||
      out_api->struct_size < sizeof(pixel_decoder_plugin_api)) {
    return 0;
  }

  pixel_decoder_plugin_api api{};
  api.struct_size = sizeof(pixel_decoder_plugin_api);
  api.abi_version = PIXEL_DECODER_PLUGIN_ABI;
  api.info.struct_size = sizeof(pixel_decoder_plugin_info);
  api.info.abi_version = PIXEL_DECODER_PLUGIN_ABI;
  api.info.display_name = "OpenJPEG Decoder";
  api.info.supported_profile_flags = pixel::openjpeg_codec::supported_profile_flags();
  api.create = &pixel::openjpeg_codec::decoder_create;
  api.destroy = &pixel::openjpeg_codec::decoder_destroy;
  api.configure = &pixel::openjpeg_codec::decoder_configure;
  api.decode_frame = &pixel::openjpeg_codec::decoder_decode_frame;
  api.copy_last_error_detail = &pixel::openjpeg_codec::decoder_copy_last_error_detail;
  *out_api = api;
  return 1;
}

int build_encoder_plugin_api(pixel_encoder_plugin_api* out_api) {
  if (out_api == nullptr) {
    return 0;
  }
  if (out_api->abi_version != PIXEL_ENCODER_PLUGIN_ABI ||
      out_api->struct_size < sizeof(pixel_encoder_plugin_api)) {
    return 0;
  }

  pixel_encoder_plugin_api api{};
  api.struct_size = sizeof(pixel_encoder_plugin_api);
  api.abi_version = PIXEL_ENCODER_PLUGIN_ABI;
  api.info.struct_size = sizeof(pixel_encoder_plugin_info);
  api.info.abi_version = PIXEL_ENCODER_PLUGIN_ABI;
  api.info.display_name = "OpenJPEG Encoder";
  api.info.supported_profile_flags = pixel::openjpeg_codec::supported_profile_flags();
  api.create = &pixel::openjpeg_codec::encoder_create;
  api.destroy = &pixel::openjpeg_codec::encoder_destroy;
  api.configure = &pixel::openjpeg_codec::encoder_configure;
  api.encode_frame = &pixel::openjpeg_codec::encoder_encode_frame;
  api.copy_last_error_detail = &pixel::openjpeg_codec::encoder_copy_last_error_detail;
  api.encode_frame_to_context_buffer =
      &pixel::openjpeg_codec::encoder_encode_frame_to_context_buffer;
  api.get_encoded_buffer = &pixel::openjpeg_codec::encoder_get_encoded_buffer;
  *out_api = api;
  return 1;
}

}  // namespace

extern "C" {

PIXEL_PLUGIN_EXPORT int pixel_get_decoder_plugin_api(
    pixel_decoder_plugin_api* out_api) {
  return build_decoder_plugin_api(out_api);
}

PIXEL_PLUGIN_EXPORT int pixel_get_encoder_plugin_api(
    pixel_encoder_plugin_api* out_api) {
  return build_encoder_plugin_api(out_api);
}

}  // extern "C"
