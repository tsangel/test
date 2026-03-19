#pragma once

#include "dicom.h"
#include "pixel_codec_plugin_abi.h"

#include <optional>
#include <string_view>

namespace dicom::pixel::detail {

[[nodiscard]] bool is_native_uncompressed_encode_profile(
    uint32_t codec_profile_code) noexcept;

[[nodiscard]] bool is_rle_encode_profile(uint32_t codec_profile_code) noexcept;

[[nodiscard]] bool is_jpeg2000_encode_profile(uint32_t codec_profile_code) noexcept;

[[nodiscard]] bool is_htj2k_encode_profile(uint32_t codec_profile_code) noexcept;

void validate_encode_profile_source_constraints(uint32_t codec_profile_code,
    int bits_allocated, int bits_stored, std::string_view file_path);

[[nodiscard]] pixel::Photometric compute_output_photometric_for_encode_profile(
    uint32_t codec_profile_code, bool use_multicomponent_transform,
    pixel::Photometric source_photometric) noexcept;

[[nodiscard]] bool encode_profile_uses_lossy_compression(
    uint32_t codec_profile_code) noexcept;

[[nodiscard]] std::optional<std::string_view> lossy_method_for_encode_profile(
    uint32_t codec_profile_code) noexcept;

} // namespace dicom::pixel::detail
