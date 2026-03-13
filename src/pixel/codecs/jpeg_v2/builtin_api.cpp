#include "builtin_api.hpp"

#include "internal.hpp"

namespace pixel::jpeg_codec_v2 {

const pixel_decoder_plugin_api_v2& decoder_builtin_api() noexcept {
  static const pixel_decoder_plugin_api_v2 api = []() {
    pixel_decoder_plugin_api_v2 out{};
    out.struct_size = sizeof(pixel_decoder_plugin_api_v2);
    out.abi_version = PIXEL_DECODER_PLUGIN_ABI_V2;
    out.info.struct_size = sizeof(pixel_decoder_plugin_info_v2);
    out.info.abi_version = PIXEL_DECODER_PLUGIN_ABI_V2;
    out.info.display_name = "JPEG v2 Decoder (builtin)";
    out.info.supported_profile_flags = pixel::jpeg_codec_v2::supported_profile_flags();
    out.create = &pixel::jpeg_codec_v2::decoder_create;
    out.destroy = &pixel::jpeg_codec_v2::decoder_destroy;
    out.configure = &pixel::jpeg_codec_v2::decoder_configure;
    out.decode_frame = &pixel::jpeg_codec_v2::decoder_decode_frame;
    out.copy_last_error_detail = &pixel::jpeg_codec_v2::decoder_copy_last_error_detail;
    return out;
  }();
  return api;
}

const pixel_encoder_plugin_api_v2& encoder_builtin_api() noexcept {
  static const pixel_encoder_plugin_api_v2 api = []() {
    pixel_encoder_plugin_api_v2 out{};
    out.struct_size = sizeof(pixel_encoder_plugin_api_v2);
    out.abi_version = PIXEL_ENCODER_PLUGIN_ABI_V2;
    out.info.struct_size = sizeof(pixel_encoder_plugin_info_v2);
    out.info.abi_version = PIXEL_ENCODER_PLUGIN_ABI_V2;
    out.info.display_name = "JPEG v2 Encoder (builtin)";
    out.info.supported_profile_flags = pixel::jpeg_codec_v2::supported_profile_flags();
    out.create = &pixel::jpeg_codec_v2::encoder_create;
    out.destroy = &pixel::jpeg_codec_v2::encoder_destroy;
    out.configure = &pixel::jpeg_codec_v2::encoder_configure;
    out.encode_frame = &pixel::jpeg_codec_v2::encoder_encode_frame;
    out.copy_last_error_detail = &pixel::jpeg_codec_v2::encoder_copy_last_error_detail;
    out.encode_frame_to_context_buffer =
        &pixel::jpeg_codec_v2::encoder_encode_frame_to_context_buffer;
    out.get_encoded_buffer = &pixel::jpeg_codec_v2::encoder_get_encoded_buffer;
    return out;
  }();
  return api;
}

}  // namespace pixel::jpeg_codec_v2
