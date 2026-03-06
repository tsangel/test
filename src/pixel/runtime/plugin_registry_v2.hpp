#pragma once

#include <array>
#include <cstdint>

#include "pixel_decoder_plugin_abi_v2.h"
#include "pixel_encoder_plugin_abi_v2.h"

namespace pixel::runtime_v2 {

enum class Htj2kDecoderBackendPreference : uint8_t {
  kAuto = 0,
  kOpenJph = 1,
  kOpenJpeg = 2,
};

enum class DecoderBindingKind : uint8_t {
  kNone = 0,
  kCoreDirect = 1,
  kPluginApi = 2,
};

enum class EncoderBindingKind : uint8_t {
  kNone = 0,
  kCoreDirect = 1,
  kPluginApi = 2,
};

struct DecoderBinding {
  DecoderBindingKind binding_kind{DecoderBindingKind::kNone};
  const pixel_decoder_plugin_api_v2* plugin_api{nullptr};
  const char* display_name{nullptr};
};

struct EncoderBinding {
  EncoderBindingKind binding_kind{EncoderBindingKind::kNone};
  const pixel_encoder_plugin_api_v2* plugin_api{nullptr};
  const char* display_name{nullptr};
};

struct CodecSlot {
  DecoderBinding decoder{};
  EncoderBinding encoder{};
};

struct LoadedPluginApisV2 {
  const pixel_decoder_plugin_api_v2* decoder_api{nullptr};
  const pixel_encoder_plugin_api_v2* encoder_api{nullptr};
};

class PluginRegistryV2 {
public:
  static constexpr uint32_t kSlotCount = 32u;

  void clear();

  bool register_decoder_api(const pixel_decoder_plugin_api_v2* api);
  bool register_encoder_api(const pixel_encoder_plugin_api_v2* api);
  bool register_core_routes(
      pixel_supported_profile_flags_v2 supported_profile_flags, const char* display_name);

  const CodecSlot* find_slot(uint32_t profile_code) const;
  const DecoderBinding* find_decoder_binding(uint32_t profile_code) const;
  const EncoderBinding* find_encoder_binding(uint32_t profile_code) const;

  const std::array<CodecSlot, kSlotCount>& slots() const noexcept {
    return slots_;
  }

private:
  bool apply_decoder_flags(pixel_supported_profile_flags_v2 supported_profile_flags,
      DecoderBindingKind binding_kind, const pixel_decoder_plugin_api_v2* api,
      const char* display_name);
  bool apply_encoder_flags(pixel_supported_profile_flags_v2 supported_profile_flags,
      EncoderBindingKind binding_kind, const pixel_encoder_plugin_api_v2* api,
      const char* display_name);

  std::array<CodecSlot, kSlotCount> slots_{};
};

void init_builtin_registry_v2(
    PluginRegistryV2* registry,
    Htj2kDecoderBackendPreference htj2k_decoder_preference =
        Htj2kDecoderBackendPreference::kAuto);
uint32_t register_loaded_plugins_v2(PluginRegistryV2* registry,
    const LoadedPluginApisV2* loaded_plugins, uint32_t loaded_plugin_count);

}  // namespace pixel::runtime_v2
