#include <bit>
#include <cstdint>

#include "core_api_v2.hpp"
#include "plugin_registry_v2.hpp"

#if defined(DICOMSDL_PIXEL_V2_RUNTIME_WITH_RLE_STATIC)
#include "../plugins/static/rle_v2/static_api.hpp"
#endif
#if defined(DICOMSDL_PIXEL_V2_RUNTIME_WITH_OPENJPEG_STATIC)
#include "../plugins/shared/openjpeg_v2/static_api.hpp"
#endif
#if defined(DICOMSDL_PIXEL_V2_RUNTIME_WITH_JPEG_STATIC)
#include "../plugins/shared/jpeg_v2/static_api.hpp"
#endif
#if defined(DICOMSDL_PIXEL_V2_RUNTIME_WITH_JPEGLS_STATIC)
#include "../plugins/shared/jpegls_v2/static_api.hpp"
#endif
#if defined(DICOMSDL_PIXEL_V2_RUNTIME_WITH_HTJ2K_STATIC)
#include "../plugins/shared/htj2k_v2/static_api.hpp"
#endif
#if defined(DICOMSDL_PIXEL_V2_RUNTIME_WITH_JPEGXL_STATIC)
#include "../plugins/shared/jpegxl_v2/static_api.hpp"
#endif

namespace pixel::runtime_v2 {

namespace {

constexpr uint64_t kLower32Mask = UINT64_C(0x00000000FFFFFFFF);

bool has_non_empty_string(const char* text) {
  return text != nullptr && text[0] != '\0';
}

const char* sanitize_display_name(const char* display_name, const char* fallback) {
  if (has_non_empty_string(display_name)) {
    return display_name;
  }
  return fallback;
}

bool validate_decoder_api(const pixel_decoder_plugin_api_v2* api) {
  if (api == nullptr) {
    return false;
  }
  if (api->struct_size < sizeof(pixel_decoder_plugin_api_v2) ||
      api->abi_version != PIXEL_DECODER_PLUGIN_ABI_V2) {
    return false;
  }
  if (api->info.struct_size < sizeof(pixel_decoder_plugin_info_v2) ||
      api->info.abi_version != PIXEL_DECODER_PLUGIN_ABI_V2) {
    return false;
  }
  if (api->create == nullptr || api->destroy == nullptr || api->configure == nullptr ||
      api->decode_frame == nullptr || api->copy_last_error_detail == nullptr) {
    return false;
  }
  return true;
}

bool validate_encoder_api(const pixel_encoder_plugin_api_v2* api) {
  if (api == nullptr) {
    return false;
  }
  if (api->struct_size < sizeof(pixel_encoder_plugin_api_v2) ||
      api->abi_version != PIXEL_ENCODER_PLUGIN_ABI_V2) {
    return false;
  }
  if (api->info.struct_size < sizeof(pixel_encoder_plugin_info_v2) ||
      api->info.abi_version != PIXEL_ENCODER_PLUGIN_ABI_V2) {
    return false;
  }
  if (api->create == nullptr || api->destroy == nullptr || api->configure == nullptr ||
      api->encode_frame == nullptr || api->copy_last_error_detail == nullptr) {
    return false;
  }
  return true;
}

}  // namespace

void PluginRegistryV2::clear() {
  slots_.fill(CodecSlot{});
}

bool PluginRegistryV2::apply_decoder_flags(pixel_supported_profile_flags_v2 supported_profile_flags,
    DecoderBindingKind binding_kind, const pixel_decoder_plugin_api_v2* api,
    const char* display_name) {
  const uint64_t decode_bits = supported_profile_flags & kLower32Mask;
  if (decode_bits == 0) {
    return false;
  }

  uint64_t bits = decode_bits;
  while (bits != 0) {
    const uint32_t slot = static_cast<uint32_t>(std::countr_zero(bits));
    slots_[slot].decoder.binding_kind = binding_kind;
    slots_[slot].decoder.plugin_api = api;
    slots_[slot].decoder.display_name = display_name;
    bits &= (bits - 1u);
  }
  return true;
}

bool PluginRegistryV2::apply_encoder_flags(pixel_supported_profile_flags_v2 supported_profile_flags,
    EncoderBindingKind binding_kind, const pixel_encoder_plugin_api_v2* api,
    const char* display_name) {
  const uint64_t encode_bits = (supported_profile_flags >> 32u) & kLower32Mask;
  if (encode_bits == 0) {
    return false;
  }

  uint64_t bits = encode_bits;
  while (bits != 0) {
    const uint32_t slot = static_cast<uint32_t>(std::countr_zero(bits));
    slots_[slot].encoder.binding_kind = binding_kind;
    slots_[slot].encoder.plugin_api = api;
    slots_[slot].encoder.display_name = display_name;
    bits &= (bits - 1u);
  }
  return true;
}

bool PluginRegistryV2::register_decoder_api(const pixel_decoder_plugin_api_v2* api) {
  if (!validate_decoder_api(api)) {
    return false;
  }
  const char* display_name = sanitize_display_name(api->info.display_name, "decoder-plugin");
  return apply_decoder_flags(
      api->info.supported_profile_flags, DecoderBindingKind::kPluginApi, api, display_name);
}

bool PluginRegistryV2::register_encoder_api(const pixel_encoder_plugin_api_v2* api) {
  if (!validate_encoder_api(api)) {
    return false;
  }
  const char* display_name = sanitize_display_name(api->info.display_name, "encoder-plugin");
  return apply_encoder_flags(
      api->info.supported_profile_flags, EncoderBindingKind::kPluginApi, api, display_name);
}

bool PluginRegistryV2::register_core_routes(
    pixel_supported_profile_flags_v2 supported_profile_flags, const char* display_name) {
  const char* safe_display_name = sanitize_display_name(display_name, "core");
  const bool decoder_ok = apply_decoder_flags(
      supported_profile_flags, DecoderBindingKind::kCoreDirect, nullptr, safe_display_name);
  const bool encoder_ok = apply_encoder_flags(
      supported_profile_flags, EncoderBindingKind::kCoreDirect, nullptr, safe_display_name);
  return decoder_ok || encoder_ok;
}

const CodecSlot* PluginRegistryV2::find_slot(uint32_t profile_code) const {
  if (profile_code >= kSlotCount) {
    return nullptr;
  }
  return &slots_[profile_code];
}

const DecoderBinding* PluginRegistryV2::find_decoder_binding(uint32_t profile_code) const {
  const CodecSlot* slot = find_slot(profile_code);
  if (slot == nullptr || slot->decoder.binding_kind == DecoderBindingKind::kNone) {
    return nullptr;
  }
  return &slot->decoder;
}

const EncoderBinding* PluginRegistryV2::find_encoder_binding(uint32_t profile_code) const {
  const CodecSlot* slot = find_slot(profile_code);
  if (slot == nullptr || slot->encoder.binding_kind == EncoderBindingKind::kNone) {
    return nullptr;
  }
  return &slot->encoder;
}

void init_builtin_registry_v2(PluginRegistryV2* registry) {
  if (registry == nullptr) {
    return;
  }

  registry->clear();
  registry->register_core_routes(pixel::core_v2::supported_profile_flags(), "core");

#if defined(DICOMSDL_PIXEL_V2_RUNTIME_WITH_RLE_STATIC)
  {
    const auto& decoder_api = pixel::rle_plugin_v2::decoder_static_api();
    const auto& encoder_api = pixel::rle_plugin_v2::encoder_static_api();
    registry->register_decoder_api(&decoder_api);
    registry->register_encoder_api(&encoder_api);
  }
#endif
#if defined(DICOMSDL_PIXEL_V2_RUNTIME_WITH_JPEG_STATIC)
  {
    const auto& decoder_api = pixel::jpeg_plugin_v2::decoder_static_api();
    const auto& encoder_api = pixel::jpeg_plugin_v2::encoder_static_api();
    registry->register_decoder_api(&decoder_api);
    registry->register_encoder_api(&encoder_api);
  }
#endif
#if defined(DICOMSDL_PIXEL_V2_RUNTIME_WITH_JPEGLS_STATIC)
  {
    const auto& decoder_api = pixel::jpegls_plugin_v2::decoder_static_api();
    const auto& encoder_api = pixel::jpegls_plugin_v2::encoder_static_api();
    registry->register_decoder_api(&decoder_api);
    registry->register_encoder_api(&encoder_api);
  }
#endif
#if defined(DICOMSDL_PIXEL_V2_RUNTIME_WITH_HTJ2K_STATIC)
  {
    const auto& decoder_api = pixel::htj2k_plugin_v2::decoder_static_api();
    const auto& encoder_api = pixel::htj2k_plugin_v2::encoder_static_api();
    registry->register_decoder_api(&decoder_api);
    registry->register_encoder_api(&encoder_api);
  }
#endif
#if defined(DICOMSDL_PIXEL_V2_RUNTIME_WITH_OPENJPEG_STATIC)
  {
    const auto& decoder_api = pixel::openjpeg_plugin_v2::decoder_static_api();
    const auto& encoder_api = pixel::openjpeg_plugin_v2::encoder_static_api();
    registry->register_decoder_api(&decoder_api);
    registry->register_encoder_api(&encoder_api);
  }
#endif
#if defined(DICOMSDL_PIXEL_V2_RUNTIME_WITH_JPEGXL_STATIC)
  {
    const auto& decoder_api = pixel::jpegxl_plugin_v2::decoder_static_api();
    const auto& encoder_api = pixel::jpegxl_plugin_v2::encoder_static_api();
    registry->register_decoder_api(&decoder_api);
    registry->register_encoder_api(&encoder_api);
  }
#endif
}

uint32_t register_loaded_plugins_v2(PluginRegistryV2* registry,
    const LoadedPluginApisV2* loaded_plugins, uint32_t loaded_plugin_count) {
  if (registry == nullptr || loaded_plugins == nullptr || loaded_plugin_count == 0) {
    return 0;
  }

  uint32_t registered_plugin_count = 0;
  for (uint32_t i = 0; i < loaded_plugin_count; ++i) {
    const LoadedPluginApisV2& entry = loaded_plugins[i];
    bool any_registered = false;
    if (entry.decoder_api != nullptr) {
      any_registered = registry->register_decoder_api(entry.decoder_api) || any_registered;
    }
    if (entry.encoder_api != nullptr) {
      any_registered = registry->register_encoder_api(entry.encoder_api) || any_registered;
    }
    if (any_registered) {
      ++registered_plugin_count;
    }
  }
  return registered_plugin_count;
}

}  // namespace pixel::runtime_v2
