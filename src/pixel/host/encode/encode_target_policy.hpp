#pragma once

#include "dicom.h"
#include "pixel_codec_plugin_abi.h"

#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace dicom::pixel::detail {

[[nodiscard]] bool is_native_uncompressed_encode_profile(
    uint32_t codec_profile_code) noexcept;

[[nodiscard]] bool is_rle_encode_profile(uint32_t codec_profile_code) noexcept;

[[nodiscard]] bool is_jpeg2000_encode_profile(uint32_t codec_profile_code) noexcept;

[[nodiscard]] bool is_htj2k_encode_profile(uint32_t codec_profile_code) noexcept;

void validate_encode_profile_source_constraints(uint32_t codec_profile_code,
    int bits_allocated, int bits_stored);
[[nodiscard]] pixel::Photometric compute_output_photometric_for_encode_profile(
    uid::WellKnown transfer_syntax, uint32_t codec_profile_code,
    std::span<const CodecOptionKv> codec_options,
    bool use_multicomponent_transform, pixel::Photometric source_photometric,
    std::size_t samples_per_pixel,
    bool decoded_source_is_rgb_domain_for_jpeg = false);
[[nodiscard]] std::vector<CodecOptionKv>
build_single_frame_effective_codec_options_for_target_or_throw(
    uid::WellKnown transfer_syntax, uint32_t codec_profile_code,
    std::span<const CodecOptionKv> codec_options,
    pixel::Photometric source_photometric, pixel::Photometric target_photometric,
    std::size_t samples_per_pixel);

[[nodiscard]] bool encode_profile_uses_lossy_compression(
    uint32_t codec_profile_code) noexcept;

[[nodiscard]] std::optional<std::string_view> lossy_method_for_encode_profile(
    uint32_t codec_profile_code) noexcept;

} // namespace dicom::pixel::detail
