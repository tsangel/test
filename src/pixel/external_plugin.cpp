#include "dicom.h"

#include "pixel/runtime/runtime_registry_v2.hpp"

namespace dicom::pixel {

bool register_external_codec_plugin_from_library(
    std::string_view library_path, std::string* out_plugin_key,
    std::string* out_error) {
  return ::pixel::runtime_v2::register_external_codec_plugin_from_library(
      library_path, out_plugin_key, out_error);
}

bool register_external_decoder_plugin_static(
    const pixel_decoder_plugin_api_v2* api, std::string* out_error) {
  return ::pixel::runtime_v2::register_external_decoder_plugin_static(api, out_error);
}

bool register_external_encoder_plugin_static(
    const pixel_encoder_plugin_api_v2* api, std::string* out_error) {
  return ::pixel::runtime_v2::register_external_encoder_plugin_static(api, out_error);
}

bool unregister_external_codec_plugin(
    std::string_view plugin_key, std::string* out_error) {
  return ::pixel::runtime_v2::unregister_external_codec_plugin(plugin_key, out_error);
}

}  // namespace dicom::pixel
