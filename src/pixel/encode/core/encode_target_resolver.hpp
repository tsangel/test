#pragma once

#include "pixel/registry/codec_registry.hpp"

#include <optional>
#include <string_view>

namespace dicom::pixel::detail {

struct PixelEncodeTarget {
	bool is_native_uncompressed{false};
	bool is_encapsulated_uncompressed{false};
	bool is_rle{false};
	bool is_j2k{false};
	bool is_j2k_lossless{false};
	bool is_j2k_lossy{false};
	bool is_htj2k{false};
	bool is_htj2k_lossless{false};
	bool is_htj2k_lossy{false};
	bool is_jpegls{false};
	bool is_jpegls_lossless{false};
	bool is_jpegls_lossy{false};
	bool is_jpeg{false};
	bool is_jpeg_lossless{false};
	bool is_jpeg_lossy{false};
	bool is_jpegxl{false};
	bool is_jpegxl_lossless{false};
	bool is_jpegxl_lossy{false};
};

[[nodiscard]] PixelEncodeTarget classify_pixel_encode_target(
    const TransferSyntaxPluginBinding& binding) noexcept;

void validate_target_source_constraints(const PixelEncodeTarget& target,
    int bits_allocated, int bits_stored, std::string_view file_path);

[[nodiscard]] pixel::Photometric resolve_output_photometric(
    const PixelEncodeTarget& target, bool use_multicomponent_transform,
    pixel::Photometric source_photometric) noexcept;

[[nodiscard]] bool target_uses_lossy_compression(const PixelEncodeTarget& target) noexcept;

[[nodiscard]] std::optional<std::string_view> lossy_method_for_target(
    const PixelEncodeTarget& target) noexcept;

} // namespace dicom::pixel::detail
