#include "pixel/encode/core/multicomponent_option_resolver.hpp"

#include "diagnostics.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

namespace dicom::pixel::detail {
using namespace dicom::literals;

namespace {

struct BoolOptionLookupResult {
	bool found{false};
	bool valid{true};
	bool value{false};
};

[[nodiscard]] bool is_jpeg2000_mc_transfer_syntax(uid::WellKnown transfer_syntax) noexcept {
	return transfer_syntax == "JPEG2000MCLossless"_uid ||
	    transfer_syntax == "JPEG2000MC"_uid;
}

[[nodiscard]] bool option_key_matches_exact(
    std::string_view key, std::string_view expected) noexcept {
	return key == expected;
}

[[nodiscard]] bool try_decode_codec_bool_option(
    const pixel::CodecOptionValue& value, bool& out_value) noexcept {
	if (const auto* bool_value = std::get_if<bool>(&value)) {
		out_value = *bool_value;
		return true;
	}
	if (const auto* int_value = std::get_if<std::int64_t>(&value)) {
		if (*int_value == 0) {
			out_value = false;
			return true;
		}
		if (*int_value == 1) {
			out_value = true;
			return true;
		}
		return false;
	}
	if (const auto* double_value = std::get_if<double>(&value)) {
		if (!std::isfinite(*double_value)) {
			return false;
		}
		if (*double_value == 0.0) {
			out_value = false;
			return true;
		}
		if (*double_value == 1.0) {
			out_value = true;
			return true;
		}
		return false;
	}
	if (const auto* string_value = std::get_if<std::string>(&value)) {
		std::string_view text(*string_value);
		while (!text.empty() &&
		       (text.front() == ' ' || text.front() == '\t' ||
		           text.front() == '\n' || text.front() == '\r')) {
			text.remove_prefix(1);
		}
		while (!text.empty() &&
		       (text.back() == ' ' || text.back() == '\t' ||
		           text.back() == '\n' || text.back() == '\r')) {
			text.remove_suffix(1);
		}
		if (text.empty()) {
			return false;
		}
		if (text == "0") {
			out_value = false;
			return true;
		}
		if (text == "1") {
			out_value = true;
			return true;
		}

		const auto equals_ascii_case_insensitive =
		    [](std::string_view lhs, std::string_view rhs) noexcept {
			    if (lhs.size() != rhs.size()) {
				    return false;
			    }
			    for (std::size_t index = 0; index < lhs.size(); ++index) {
				    char left = lhs[index];
				    char right = rhs[index];
				    if (left >= 'A' && left <= 'Z') {
					    left = static_cast<char>(left - 'A' + 'a');
				    }
				    if (right >= 'A' && right <= 'Z') {
					    right = static_cast<char>(right - 'A' + 'a');
				    }
				    if (left != right) {
					    return false;
				    }
			    }
			    return true;
		    };
		if (equals_ascii_case_insensitive(text, "true")) {
			out_value = true;
			return true;
		}
		if (equals_ascii_case_insensitive(text, "false")) {
			out_value = false;
			return true;
		}
		return false;
	}
	return false;
}

[[nodiscard]] BoolOptionLookupResult lookup_use_mct_option(
    std::span<const CodecOptionKv> codec_options) noexcept {
	for (const auto& option : codec_options) {
		if (!option_key_matches_exact(option.key, "color_transform") &&
		    !option_key_matches_exact(option.key, "mct") &&
		    !option_key_matches_exact(option.key, "use_mct")) {
			continue;
		}
		BoolOptionLookupResult result{};
		result.found = true;
		bool parsed = false;
		result.valid = try_decode_codec_bool_option(option.value, parsed);
		result.value = parsed;
		return result;
	}
	return BoolOptionLookupResult{};
}

} // namespace

bool resolve_use_multicomponent_transform(uid::WellKnown transfer_syntax,
    bool is_j2k_target, bool is_htj2k_target, std::span<const CodecOptionKv> codec_options,
    std::size_t samples_per_pixel, std::string_view file_path) {
	const auto mct_option = lookup_use_mct_option(codec_options);
	if (mct_option.found && !mct_option.valid) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} reason=color_transform/mct option must be bool (or 0/1)",
		    file_path);
	}
	const bool use_color_transform = mct_option.found ? mct_option.value : true;

	if (is_j2k_target) {
		if (is_jpeg2000_mc_transfer_syntax(transfer_syntax)) {
			if (!use_color_transform) {
				diag::error_and_throw(
				    "DicomFile::set_pixel_data file={} ts={} reason=JPEG2000 MC transfer syntax requires color transform enabled",
				    file_path, transfer_syntax.value());
			}
			if (samples_per_pixel != std::size_t{3}) {
				diag::error_and_throw(
				    "DicomFile::set_pixel_data file={} ts={} reason=JPEG2000 MC transfer syntax requires samples_per_pixel=3",
				    file_path, transfer_syntax.value());
			}
			return true;
		}
		return use_color_transform && samples_per_pixel == std::size_t{3};
	}
	if (is_htj2k_target) {
		return use_color_transform && samples_per_pixel == std::size_t{3};
	}
	return false;
}

} // namespace dicom::pixel::detail
