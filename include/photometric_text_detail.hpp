#pragma once

#include "dicom.h"

#include <optional>
#include <string_view>

namespace dicom::pixel::detail {

[[nodiscard]] inline char ascii_upper_photometric_char(char value) noexcept {
	return (value >= 'a' && value <= 'z')
	           ? static_cast<char>(value - ('a' - 'A'))
	           : value;
}

[[nodiscard]] inline bool ascii_iequals_photometric_text(
    std::string_view lhs, std::string_view rhs) noexcept {
	if (lhs.size() != rhs.size()) {
		return false;
	}
	for (std::size_t index = 0; index < lhs.size(); ++index) {
		// parse_photometric_text() only passes uppercase ASCII literals on the
		// right-hand side, so normalizing the input text is sufficient here.
		if (ascii_upper_photometric_char(lhs[index]) != rhs[index]) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] inline std::optional<Photometric> parse_photometric_text(
    std::string_view text) noexcept {
	switch (text.size()) {
	case 3:
		if (ascii_iequals_photometric_text(text, "RGB")) {
			return Photometric::rgb;
		}
		if (ascii_iequals_photometric_text(text, "XYB")) {
			return Photometric::xyb;
		}
		if (ascii_iequals_photometric_text(text, "HSV")) {
			return Photometric::hsv;
		}
		break;
	case 4:
		if (ascii_iequals_photometric_text(text, "CMYK")) {
			return Photometric::cmyk;
		}
		if (ascii_iequals_photometric_text(text, "ARGB")) {
			return Photometric::argb;
		}
		break;
	case 7:
		if (ascii_iequals_photometric_text(text, "YBR_RCT")) {
			return Photometric::ybr_rct;
		}
		if (ascii_iequals_photometric_text(text, "YBR_ICT")) {
			return Photometric::ybr_ict;
		}
		break;
	case 8:
		if (ascii_iequals_photometric_text(text, "YBR_FULL")) {
			return Photometric::ybr_full;
		}
		break;
	case 11:
		if (ascii_iequals_photometric_text(text, "MONOCHROME2")) {
			return Photometric::monochrome2;
		}
		if (ascii_iequals_photometric_text(text, "MONOCHROME1")) {
			return Photometric::monochrome1;
		}
		break;
	case 12:
		if (ascii_iequals_photometric_text(text, "YBR_FULL_422")) {
			return Photometric::ybr_full_422;
		}
		break;
	case 13:
		if (ascii_iequals_photometric_text(text, "PALETTE COLOR")) {
			return Photometric::palette_color;
		}
		break;
	case 15:
		if (ascii_iequals_photometric_text(text, "YBR_PARTIAL_420")) {
			return Photometric::ybr_partial_420;
		}
		if (ascii_iequals_photometric_text(text, "YBR_PARTIAL_422")) {
			return Photometric::ybr_partial_422;
		}
		break;
	default:
		break;
	}
	return std::nullopt;
}

[[nodiscard]] inline std::string_view to_photometric_text(
    Photometric photometric) noexcept {
	switch (photometric) {
	case Photometric::monochrome1:
		return "MONOCHROME1";
	case Photometric::monochrome2:
		return "MONOCHROME2";
	case Photometric::palette_color:
		return "PALETTE COLOR";
	case Photometric::rgb:
		return "RGB";
	case Photometric::ybr_full:
		return "YBR_FULL";
	case Photometric::ybr_full_422:
		return "YBR_FULL_422";
	case Photometric::ybr_rct:
		return "YBR_RCT";
	case Photometric::ybr_ict:
		return "YBR_ICT";
	case Photometric::ybr_partial_420:
		return "YBR_PARTIAL_420";
	case Photometric::xyb:
		return "XYB";
	case Photometric::hsv:
		return "HSV";
	case Photometric::argb:
		return "ARGB";
	case Photometric::cmyk:
		return "CMYK";
	case Photometric::ybr_partial_422:
		return "YBR_PARTIAL_422";
	default:
		return "MONOCHROME2";
	}
}

}  // namespace dicom::pixel::detail
