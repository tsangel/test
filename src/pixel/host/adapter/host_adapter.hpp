#pragma once

#include <cstdint>
#include <span>
#include <string>

#include "dicom.h"
#include "pixel_decoder_plugin_abi.h"
#include "pixel_encoder_plugin_abi.h"
#include "pixel/runtime/plugin_registry.hpp"

namespace pixel::runtime {

struct HostDecoderContext {
  uint32_t codec_profile_code{PIXEL_CODEC_PROFILE_UNKNOWN};
  DecoderBindingKind binding_kind{DecoderBindingKind::kNone};
  // Decoder plugins receive the current unversioned decoder request ABI.
  const pixel_decoder_plugin_api* plugin_api{nullptr};
  void* plugin_ctx{nullptr};
  const char* display_name{nullptr};
  std::string last_error_detail{};
};

struct HostEncoderContext {
  uint32_t codec_profile_code{PIXEL_CODEC_PROFILE_UNKNOWN};
  EncoderBindingKind binding_kind{EncoderBindingKind::kNone};
  const pixel_encoder_plugin_api* plugin_api{nullptr};
  void* plugin_ctx{nullptr};
  const char* display_name{nullptr};
  std::string last_error_detail{};
};

bool codec_profile_code_from_transfer_syntax(
    dicom::uid::WellKnown transfer_syntax, uint32_t* out_codec_profile_code) noexcept;

// Fully reset and configure one host decoder context for the given transfer syntax and options.
pixel_error_code configure_host_decoder_context(HostDecoderContext* ctx,
    const BindingRegistry* registry, dicom::uid::WellKnown transfer_syntax,
    const pixel_option_list* options = nullptr);

// Fully reset and configure one host encoder context for the given transfer syntax and options.
pixel_error_code configure_host_encoder_context(HostEncoderContext* ctx,
    const BindingRegistry* registry, dicom::uid::WellKnown transfer_syntax,
    const pixel_option_list* options = nullptr);

void destroy_host_decoder_context(HostDecoderContext* ctx) noexcept;
void destroy_host_encoder_context(HostEncoderContext* ctx) noexcept;

pixel_error_code decode_frame_with_host_context(HostDecoderContext* ctx,
    const dicom::pixel::PixelLayout* source_layout, std::span<const uint8_t> prepared_source,
    std::span<uint8_t> destination, const dicom::pixel::PixelLayout* output_layout = nullptr,
    const dicom::pixel::DecodeOptions* options = nullptr,
    pixel_decoder_info* decode_info = nullptr);

pixel_error_code encode_frame_with_host_context(HostEncoderContext* ctx,
    const dicom::pixel::PixelLayout* source_layout, std::span<const uint8_t> source_frame,
    bool use_multicomponent_transform, pixel_output_buffer encoded_buffer,
    uint64_t* inout_encoded_size);
bool host_encoder_context_supports_context_buffer(
    const HostEncoderContext* ctx) noexcept;
pixel_error_code encode_frame_to_context_buffer_with_host_context(
    HostEncoderContext* ctx, const dicom::pixel::PixelLayout* source_layout,
    std::span<const uint8_t> source_frame, bool use_multicomponent_transform,
    pixel_const_buffer* out_encoded_buffer);

uint32_t copy_host_decoder_last_error_detail(
    const HostDecoderContext* ctx, char* out_detail, uint32_t out_detail_capacity);

uint32_t copy_host_encoder_last_error_detail(
    const HostEncoderContext* ctx, char* out_detail, uint32_t out_detail_capacity);

}  // namespace pixel::runtime

