#pragma once

#include "pixel_/plugin_abi/builtin/shared_plugin_export.hpp"
#include "pixel_decoder_plugin_abi.h"
#include "pixel_encoder_plugin_abi.h"

namespace dicom::pixel::detail {

DICOMSDL_CODEC_RUNTIME_API const dicomsdl_decoder_plugin_api_v1&
jpeg_decoder_plugin_api_for_shared() noexcept;
DICOMSDL_CODEC_RUNTIME_API const dicomsdl_encoder_plugin_api_v1&
jpeg_encoder_plugin_api_for_shared() noexcept;

DICOMSDL_CODEC_RUNTIME_API const dicomsdl_decoder_plugin_api_v1&
jpegls_decoder_plugin_api_for_shared() noexcept;
DICOMSDL_CODEC_RUNTIME_API const dicomsdl_encoder_plugin_api_v1&
jpegls_encoder_plugin_api_for_shared() noexcept;

DICOMSDL_CODEC_RUNTIME_API const dicomsdl_decoder_plugin_api_v1&
jpeg2k_decoder_plugin_api_for_shared() noexcept;
DICOMSDL_CODEC_RUNTIME_API const dicomsdl_encoder_plugin_api_v1&
jpeg2k_encoder_plugin_api_for_shared() noexcept;

DICOMSDL_CODEC_RUNTIME_API const dicomsdl_decoder_plugin_api_v1&
htj2k_decoder_plugin_api_for_shared() noexcept;
DICOMSDL_CODEC_RUNTIME_API const dicomsdl_encoder_plugin_api_v1&
htj2k_encoder_plugin_api_for_shared() noexcept;

DICOMSDL_CODEC_RUNTIME_API const dicomsdl_decoder_plugin_api_v1&
jpegxl_decoder_plugin_api_for_shared() noexcept;
DICOMSDL_CODEC_RUNTIME_API const dicomsdl_encoder_plugin_api_v1&
jpegxl_encoder_plugin_api_for_shared() noexcept;

}  // namespace dicom::pixel::detail

