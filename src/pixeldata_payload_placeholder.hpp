#pragma once

#include "dicom.h"
#include "dicom_endian.h"
#include "diagnostics.h"

#include <algorithm>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace dicom::detail {

struct PixelDataPayloadPlaceholderMetadata {
	VR original_vr{VR::None};
	std::uint32_t original_vl{0};
	std::uint64_t payload_length{0};
};

[[nodiscard]] inline bool is_supported_pixeldata_payload_header_vr(VR vr) noexcept {
	return vr == VR::OB || vr == VR::OW || vr == VR::UN;
}

[[nodiscard]] inline VR pixeldata_payload_header_vr_or_throw(
    VR vr, std::string_view context) {
	if (vr.is_pixel_sequence()) {
		return VR::OB;
	}
	if (is_supported_pixeldata_payload_header_vr(vr)) {
		return vr;
	}
	diag::error_and_throw("{} vr={} reason=unsupported PixelData header VR",
	    context, vr.str());
}

[[nodiscard]] inline PixelDataPayloadPlaceholderMetadata
read_pixeldata_payload_placeholder_metadata(
    std::span<const std::uint8_t> value, std::string_view context) {
	if (value.size() != kPixelDataPayloadPlaceholderMetadataSize) {
		diag::error_and_throw(
		    "{} length={} reason=PixelData placeholder metadata must be exactly {} bytes",
		    context, value.size(), kPixelDataPayloadPlaceholderMetadataSize);
	}
	if (!std::equal(kPixelDataPayloadPlaceholderMagic.begin(),
	        kPixelDataPayloadPlaceholderMagic.end(), value.begin())) {
		diag::error_and_throw("{} reason=PixelData placeholder magic mismatch",
		    context);
	}
	PixelDataPayloadPlaceholderMetadata metadata{};
	metadata.original_vr =
	    VR(static_cast<char>(value[8]), static_cast<char>(value[9]));
	if (!is_supported_pixeldata_payload_header_vr(metadata.original_vr)) {
		diag::error_and_throw(
		    "{} vr={} reason=PixelData placeholder stores invalid original VR",
		    context, metadata.original_vr.str());
	}
	metadata.original_vl = endian::load_le<std::uint32_t>(value.data() + 10);
	metadata.payload_length = endian::load_le<std::uint64_t>(value.data() + 14);
	return metadata;
}

[[nodiscard]] inline std::vector<std::uint8_t> make_pixeldata_payload_placeholder_value(
    VR original_vr, std::uint32_t original_vl, std::uint64_t payload_length) {
	original_vr = pixeldata_payload_header_vr_or_throw(
	    original_vr, "make_pixeldata_payload_placeholder_value");
	std::vector<std::uint8_t> value;
	value.reserve(kPixelDataPayloadPlaceholderMetadataSize);
	value.insert(value.end(), kPixelDataPayloadPlaceholderMagic.begin(),
	    kPixelDataPayloadPlaceholderMagic.end());
	value.push_back(static_cast<std::uint8_t>(original_vr.first()));
	value.push_back(static_cast<std::uint8_t>(original_vr.second()));
	const auto vl_offset = value.size();
	value.resize(value.size() + sizeof(std::uint32_t));
	endian::store_le(value.data() + vl_offset, original_vl);
	const auto payload_len_offset = value.size();
	value.resize(value.size() + sizeof(std::uint64_t));
	endian::store_le(value.data() + payload_len_offset, payload_length);
	return value;
}

[[nodiscard]] inline bool is_legacy_pixeldata_payload_magic_only_placeholder_value(
    std::span<const std::uint8_t> value) noexcept {
	return value.size() == kPixelDataPayloadPlaceholderMagic.size() &&
	    std::equal(kPixelDataPayloadPlaceholderMagic.begin(),
	        kPixelDataPayloadPlaceholderMagic.end(), value.begin());
}

} // namespace dicom::detail
