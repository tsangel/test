#pragma once

#include <cstdint>
#include <span>
#include <string>

#include "dicom.h"
#include "plugin_registry_v2.hpp"

namespace pixel::runtime_v2 {

enum class HostValueTransformKindV2 : uint8_t {
  kNone = 0,
  kRescale = 1,
  kModalityLut = 2,
};

struct HostValueTransformSpecV2 {
  HostValueTransformKindV2 kind{HostValueTransformKindV2::kNone};
  double rescale_slope{1.0};
  double rescale_intercept{0.0};
  const dicom::pixel::ModalityLut* modality_lut{nullptr};
};

struct HostDecoderContextV2 {
  uint32_t codec_profile_code{PIXEL_CODEC_PROFILE_UNKNOWN_V2};
  DecoderBindingKind binding_kind{DecoderBindingKind::kNone};
  const pixel_decoder_plugin_api_v2* plugin_api{nullptr};
  void* plugin_ctx{nullptr};
  const char* display_name{nullptr};
  std::string last_error_detail{};
};

struct HostEncoderContextV2 {
  uint32_t codec_profile_code{PIXEL_CODEC_PROFILE_UNKNOWN_V2};
  EncoderBindingKind binding_kind{EncoderBindingKind::kNone};
  const pixel_encoder_plugin_api_v2* plugin_api{nullptr};
  void* plugin_ctx{nullptr};
  const char* display_name{nullptr};
  std::string last_error_detail{};
};

bool resolve_codec_profile_code_from_transfer_syntax_v2(
    dicom::uid::WellKnown transfer_syntax, uint32_t* out_codec_profile_code) noexcept;

pixel_error_code_v2 configure_host_decoder_context_v2(HostDecoderContextV2* ctx,
    const PluginRegistryV2* registry, dicom::uid::WellKnown transfer_syntax,
    const pixel_option_list_v2* options = nullptr);

pixel_error_code_v2 configure_host_encoder_context_v2(HostEncoderContextV2* ctx,
    const PluginRegistryV2* registry, dicom::uid::WellKnown transfer_syntax,
    const pixel_option_list_v2* options = nullptr);

void destroy_host_decoder_context_v2(HostDecoderContextV2* ctx) noexcept;
void destroy_host_encoder_context_v2(HostEncoderContextV2* ctx) noexcept;

pixel_error_code_v2 decode_frame_with_host_context_v2(HostDecoderContextV2* ctx,
    const dicom::pixel::PixelDataInfo* info, std::span<const uint8_t> prepared_source,
    std::span<uint8_t> destination, const dicom::pixel::DecodeStrides* destination_strides = nullptr,
    const dicom::pixel::DecodeOptions* options = nullptr,
    const HostValueTransformSpecV2* value_transform = nullptr);

pixel_error_code_v2 encode_frame_with_host_context_v2(HostEncoderContextV2* ctx,
    const dicom::pixel::PixelSource* source, std::span<const uint8_t> source_frame,
    bool use_multicomponent_transform, pixel_output_buffer_v2 encoded_buffer,
    uint64_t* inout_encoded_size);

uint32_t copy_host_decoder_last_error_detail_v2(
    const HostDecoderContextV2* ctx, char* out_detail, uint32_t out_detail_capacity);

uint32_t copy_host_encoder_last_error_detail_v2(
    const HostEncoderContextV2* ctx, char* out_detail, uint32_t out_detail_capacity);

}  // namespace pixel::runtime_v2
