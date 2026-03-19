#include "builtin_api.hpp"

#include "internal.hpp"

namespace pixel::jpegxl_codec {

const pixel_decoder_plugin_api& decoder_builtin_api() noexcept {
  static const pixel_decoder_plugin_api api = []() {
    pixel_decoder_plugin_api out{};
    out.struct_size = sizeof(pixel_decoder_plugin_api);
    out.abi_version = PIXEL_DECODER_PLUGIN_ABI;
    out.info.struct_size = sizeof(pixel_decoder_plugin_info);
    out.info.abi_version = PIXEL_DECODER_PLUGIN_ABI;
    out.info.display_name = "JPEG-XL Decoder (builtin)";
    out.info.supported_profile_flags = pixel::jpegxl_codec::supported_profile_flags();
    out.create = &pixel::jpegxl_codec::decoder_create;
    out.destroy = &pixel::jpegxl_codec::decoder_destroy;
    out.configure = &pixel::jpegxl_codec::decoder_configure;
    out.decode_frame = &pixel::jpegxl_codec::decoder_decode_frame;
    out.copy_last_error_detail = &pixel::jpegxl_codec::decoder_copy_last_error_detail;
    return out;
  }();
  return api;
}

const pixel_encoder_plugin_api& encoder_builtin_api() noexcept {
  static const pixel_encoder_plugin_api api = []() {
    pixel_encoder_plugin_api out{};
    out.struct_size = sizeof(pixel_encoder_plugin_api);
    out.abi_version = PIXEL_ENCODER_PLUGIN_ABI;
    out.info.struct_size = sizeof(pixel_encoder_plugin_info);
    out.info.abi_version = PIXEL_ENCODER_PLUGIN_ABI;
    out.info.display_name = "JPEG-XL Encoder (builtin)";
    out.info.supported_profile_flags = pixel::jpegxl_codec::supported_profile_flags();
    out.create = &pixel::jpegxl_codec::encoder_create;
    out.destroy = &pixel::jpegxl_codec::encoder_destroy;
    out.configure = &pixel::jpegxl_codec::encoder_configure;
    out.encode_frame = &pixel::jpegxl_codec::encoder_encode_frame;
    out.copy_last_error_detail = &pixel::jpegxl_codec::encoder_copy_last_error_detail;
    out.encode_frame_to_context_buffer =
        &pixel::jpegxl_codec::encoder_encode_frame_to_context_buffer;
    out.get_encoded_buffer = &pixel::jpegxl_codec::encoder_get_encoded_buffer;
    return out;
  }();
  return api;
}

}  // namespace pixel::jpegxl_codec
