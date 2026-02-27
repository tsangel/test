#include "pixel/plugin_abi/builtin/htj2k_builtin_plugin.hpp"

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
  *out_api = dicom::pixel::detail::htj2k_decoder_plugin_api_for_shared();
  return 1;
}

DICOMSDL_CODEC_PLUGIN_EXPORT int dicomsdl_get_encoder_plugin_api_v1(
    dicomsdl_encoder_plugin_api_v1* out_api) {
  if (out_api == nullptr) {
    return 0;
  }
  *out_api = dicom::pixel::detail::htj2k_encoder_plugin_api_for_shared();
  return 1;
}

}  // extern "C"
