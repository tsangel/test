#define DICOMSDL_DECODER_PLUGIN_API_NO_ENTRYPOINT_DECL 1
#define DICOMSDL_ENCODER_PLUGIN_API_NO_ENTRYPOINT_DECL 1

#include "pixel_/plugin_abi/shared/shared_plugin_runtime_api.hpp"

#if defined(_WIN32)
#define DICOMSDL_CODEC_PLUGIN_EXPORT __declspec(dllexport)
#else
#define DICOMSDL_CODEC_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

extern "C" {

DICOMSDL_CODEC_PLUGIN_EXPORT int dicomsdl_get_decoder_plugin_api_v1(
    dicomsdl_decoder_plugin_api_v1* out_api) {
  if (out_api == nullptr) {
    return 0;
  }
  *out_api = dicom::pixel::detail::jpeg_decoder_plugin_api_for_shared();
  return 1;
}

DICOMSDL_CODEC_PLUGIN_EXPORT int dicomsdl_get_encoder_plugin_api_v1(
    dicomsdl_encoder_plugin_api_v1* out_api) {
  if (out_api == nullptr) {
    return 0;
  }
  *out_api = dicom::pixel::detail::jpeg_encoder_plugin_api_for_shared();
  return 1;
}

}  // extern "C"
