#pragma once

#include "pixel_decoder_plugin_abi.h"
#include "pixel_encoder_plugin_abi.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace dicom::pixel::detail::abi {

struct ExternalSharedLibrary {
  void* native_handle{nullptr};
  std::string path{};
};

struct ExternalDecoderPlugin {
  ExternalSharedLibrary library{};
  dicomsdl_decoder_plugin_api_v1 api{};
  void* context{nullptr};
  std::size_t retain_count{0};
  std::size_t in_flight_calls{0};
};

struct ExternalEncoderPlugin {
  ExternalSharedLibrary library{};
  dicomsdl_encoder_plugin_api_v1 api{};
  void* context{nullptr};
  std::size_t retain_count{0};
  std::size_t in_flight_calls{0};
};

[[nodiscard]] bool load_external_decoder_plugin(std::string_view library_path,
    ExternalDecoderPlugin& out_plugin, std::string& out_error);
[[nodiscard]] bool load_external_encoder_plugin(std::string_view library_path,
    ExternalEncoderPlugin& out_plugin, std::string& out_error);
[[nodiscard]] bool init_external_decoder_plugin_from_api(
    const dicomsdl_decoder_plugin_api_v1& api,
    ExternalDecoderPlugin& out_plugin, std::string& out_error);
[[nodiscard]] bool init_external_encoder_plugin_from_api(
    const dicomsdl_encoder_plugin_api_v1& api,
    ExternalEncoderPlugin& out_plugin, std::string& out_error);

void retain_external_decoder_plugin(ExternalDecoderPlugin& plugin) noexcept;
void retain_external_encoder_plugin(ExternalEncoderPlugin& plugin) noexcept;

[[nodiscard]] bool release_external_decoder_plugin(
    ExternalDecoderPlugin& plugin, std::string& out_error);
[[nodiscard]] bool release_external_encoder_plugin(
    ExternalEncoderPlugin& plugin, std::string& out_error);

[[nodiscard]] bool configure_external_decoder_plugin(
    ExternalDecoderPlugin& plugin, std::uint16_t transfer_syntax_code,
    const dicomsdl_codec_option_list_v1* options, dicomsdl_codec_error_v1* error,
    std::string& out_error);
[[nodiscard]] bool configure_external_encoder_plugin(
    ExternalEncoderPlugin& plugin, std::uint16_t transfer_syntax_code,
    const dicomsdl_codec_option_list_v1* options, dicomsdl_codec_error_v1* error,
    std::string& out_error);

[[nodiscard]] bool decode_external_frame(ExternalDecoderPlugin& plugin,
    const dicomsdl_decoder_request_v1* request, dicomsdl_codec_error_v1* error,
    std::string& out_error);
[[nodiscard]] bool encode_external_frame(ExternalEncoderPlugin& plugin,
    const dicomsdl_encoder_request_v1* request, dicomsdl_codec_error_v1* error,
    std::string& out_error);

}  // namespace dicom::pixel::detail::abi
