#include "dicom.h"

#include "pixel/runtime/runtime_registry.hpp"

namespace dicom::pixel {

namespace {

::pixel::runtime::Htj2kDecoderBackendPreference to_runtime_backend(
    Htj2kDecoderBackend backend) {
  switch (backend) {
  case Htj2kDecoderBackend::openjph:
    return ::pixel::runtime::Htj2kDecoderBackendPreference::kOpenJph;
  case Htj2kDecoderBackend::openjpeg:
    return ::pixel::runtime::Htj2kDecoderBackendPreference::kOpenJpeg;
  case Htj2kDecoderBackend::auto_select:
  default:
    return ::pixel::runtime::Htj2kDecoderBackendPreference::kAuto;
  }
}

Htj2kDecoderBackend from_runtime_backend(
    ::pixel::runtime::Htj2kDecoderBackendPreference backend) {
  switch (backend) {
  case ::pixel::runtime::Htj2kDecoderBackendPreference::kOpenJph:
    return Htj2kDecoderBackend::openjph;
  case ::pixel::runtime::Htj2kDecoderBackendPreference::kOpenJpeg:
    return Htj2kDecoderBackend::openjpeg;
  case ::pixel::runtime::Htj2kDecoderBackendPreference::kAuto:
  default:
    return Htj2kDecoderBackend::auto_select;
  }
}

}  // namespace

bool set_htj2k_decoder_backend(Htj2kDecoderBackend backend, std::string* out_error) {
  return ::pixel::runtime::set_htj2k_decoder_backend_preference(
      to_runtime_backend(backend), out_error);
}

Htj2kDecoderBackend get_htj2k_decoder_backend() {
  return from_runtime_backend(
      ::pixel::runtime::get_htj2k_decoder_backend_preference());
}

bool use_openjph_for_htj2k_decoding(std::string* out_error) {
  return set_htj2k_decoder_backend(Htj2kDecoderBackend::openjph, out_error);
}

bool use_openjpeg_for_htj2k_decoding(std::string* out_error) {
  return set_htj2k_decoder_backend(Htj2kDecoderBackend::openjpeg, out_error);
}

bool register_external_codec_plugin_from_library(
    std::string_view library_path, std::string* out_error) {
  return ::pixel::runtime::register_external_codec_plugin_from_library(
      library_path, out_error);
}

bool clear_external_codec_plugins(std::string* out_error) {
  return ::pixel::runtime::clear_external_codec_plugins(out_error);
}

}  // namespace dicom::pixel
