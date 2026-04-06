#include "pixel/host/encode/encode_target_policy.hpp"

#include "pixel/host/error/codec_error.hpp"
#include "diagnostics.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace dicom::pixel::detail {

namespace {

enum class JpegColorSpaceOption : std::uint8_t {
	unspecified = 0,
	rgb,
	ybr,
};

enum class JpegSubsamplingOption : std::uint8_t {
	unspecified = 0,
	s444,
	s422,
};

struct JpegColorOptionState {
	bool has_color_space{false};
	bool has_subsampling{false};
	bool valid_color_space{true};
	bool valid_subsampling{true};
	JpegColorSpaceOption color_space{JpegColorSpaceOption::unspecified};
	JpegSubsamplingOption subsampling{JpegSubsamplingOption::unspecified};
};

[[nodiscard]] bool is_jpeg_encode_profile(uint32_t codec_profile_code) noexcept {
	return codec_profile_code == PIXEL_CODEC_PROFILE_JPEG_LOSSLESS ||
	    codec_profile_code == PIXEL_CODEC_PROFILE_JPEG_LOSSY;
}

[[nodiscard]] bool option_key_matches_exact(
    std::string_view key, std::string_view expected) noexcept {
	return key == expected;
}

[[nodiscard]] std::string_view trim_ascii_space(std::string_view text) noexcept {
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
	return text;
}

[[nodiscard]] bool equals_ascii_case_insensitive(
    std::string_view lhs, std::string_view rhs) noexcept {
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
}

[[nodiscard]] bool try_decode_codec_string_option(
    const pixel::CodecOptionValue& value, std::string_view& out_text) noexcept {
	if (const auto* string_value = std::get_if<std::string>(&value)) {
		out_text = trim_ascii_space(*string_value);
		return !out_text.empty();
	}
	return false;
}

[[nodiscard]] bool try_decode_jpeg_color_space_option(
    const pixel::CodecOptionValue& value,
    JpegColorSpaceOption& out_value) noexcept {
	std::string_view text{};
	if (!try_decode_codec_string_option(value, text)) {
		return false;
	}
	if (equals_ascii_case_insensitive(text, "rgb")) {
		out_value = JpegColorSpaceOption::rgb;
		return true;
	}
	if (equals_ascii_case_insensitive(text, "ybr") ||
	    equals_ascii_case_insensitive(text, "ycbcr")) {
		out_value = JpegColorSpaceOption::ybr;
		return true;
	}
	return false;
}

[[nodiscard]] bool try_decode_jpeg_subsampling_option(
    const pixel::CodecOptionValue& value,
    JpegSubsamplingOption& out_value) noexcept {
	if (const auto* int_value = std::get_if<std::int64_t>(&value)) {
		if (*int_value == 444) {
			out_value = JpegSubsamplingOption::s444;
			return true;
		}
		if (*int_value == 422) {
			out_value = JpegSubsamplingOption::s422;
			return true;
		}
		return false;
	}
	if (const auto* double_value = std::get_if<double>(&value)) {
		if (*double_value == 444.0) {
			out_value = JpegSubsamplingOption::s444;
			return true;
		}
		if (*double_value == 422.0) {
			out_value = JpegSubsamplingOption::s422;
			return true;
		}
		return false;
	}
	std::string_view text{};
	if (!try_decode_codec_string_option(value, text)) {
		return false;
	}
	if (text == "444" || text == "4:4:4") {
		out_value = JpegSubsamplingOption::s444;
		return true;
	}
	if (text == "422" || text == "4:2:2") {
		out_value = JpegSubsamplingOption::s422;
		return true;
	}
	return false;
}

[[nodiscard]] JpegColorOptionState lookup_jpeg_color_options(
    std::span<const CodecOptionKv> codec_options) noexcept {
	JpegColorOptionState state{};
	for (const auto& option : codec_options) {
		if (option_key_matches_exact(option.key, "color_space")) {
			state.has_color_space = true;
			state.valid_color_space =
			    try_decode_jpeg_color_space_option(option.value, state.color_space);
			continue;
		}
		if (option_key_matches_exact(option.key, "subsampling")) {
			state.has_subsampling = true;
			state.valid_subsampling =
			    try_decode_jpeg_subsampling_option(option.value, state.subsampling);
			continue;
		}
	}
	return state;
}

[[nodiscard]] pixel::Photometric compute_jpeg_output_photometric_or_throw(
    uid::WellKnown transfer_syntax, uint32_t codec_profile_code,
    std::span<const CodecOptionKv> codec_options,
    pixel::Photometric source_photometric, std::size_t samples_per_pixel,
    bool decoded_source_is_rgb_domain_for_jpeg) {
	using namespace dicom::literals;

	const auto jpeg_options = lookup_jpeg_color_options(codec_options);
	if (!jpeg_options.has_color_space && !jpeg_options.has_subsampling) {
		return source_photometric;
	}
	if (!jpeg_options.valid_color_space) {
		throw_codec_stage_exception(CodecStatusCode::invalid_argument,
		    "validate_options",
		    "jpeg color_space option must be 'rgb' or 'ybr'");
	}
	if (!jpeg_options.valid_subsampling) {
		throw_codec_stage_exception(CodecStatusCode::invalid_argument,
		    "validate_options",
		    "jpeg subsampling option must be '444' or '422'");
	}
	if (codec_profile_code != PIXEL_CODEC_PROFILE_JPEG_LOSSY) {
		throw_codec_stage_exception(CodecStatusCode::invalid_argument,
		    "validate_options",
		    "jpeg color_space/subsampling options require a lossy JPEG transfer syntax");
	}
	if (transfer_syntax != "JPEGBaseline8Bit"_uid) {
		throw_codec_stage_exception(CodecStatusCode::invalid_argument,
		    "validate_options",
		    "jpeg color_space/subsampling options require JPEGBaseline8Bit target transfer syntax");
	}
	if (samples_per_pixel != std::size_t{3}) {
		throw_codec_stage_exception(CodecStatusCode::invalid_argument,
		    "validate_target",
		    "jpeg color_space/subsampling options require samples_per_pixel=3");
	}
	if (source_photometric != pixel::Photometric::rgb &&
	    !decoded_source_is_rgb_domain_for_jpeg) {
		throw_codec_stage_exception(CodecStatusCode::invalid_argument,
		    "validate_target",
		    "jpeg color_space/subsampling options currently require source photometric RGB");
	}
	if (jpeg_options.has_subsampling &&
	    (!jpeg_options.has_color_space ||
	        jpeg_options.color_space != JpegColorSpaceOption::ybr)) {
		throw_codec_stage_exception(CodecStatusCode::invalid_argument,
		    "validate_options",
		    "jpeg subsampling option requires color_space='ybr'");
	}
	if (jpeg_options.color_space == JpegColorSpaceOption::rgb) {
		return pixel::Photometric::rgb;
	}
	if (jpeg_options.has_subsampling &&
	    jpeg_options.subsampling != JpegSubsamplingOption::s422) {
		throw_codec_stage_exception(CodecStatusCode::invalid_argument,
		    "validate_options",
		    "jpeg color_space='ybr' requires subsampling=422 for JPEGBaseline8Bit");
	}
	return pixel::Photometric::ybr_full_422;
}

} // namespace

bool is_native_uncompressed_encode_profile(uint32_t codec_profile_code) noexcept {
	return codec_profile_code == PIXEL_CODEC_PROFILE_NATIVE_UNCOMPRESSED;
}

bool is_rle_encode_profile(uint32_t codec_profile_code) noexcept {
	return codec_profile_code == PIXEL_CODEC_PROFILE_RLE_LOSSLESS;
}

bool is_jpeg2000_encode_profile(uint32_t codec_profile_code) noexcept {
	return codec_profile_code == PIXEL_CODEC_PROFILE_JPEG2000_LOSSLESS ||
	    codec_profile_code == PIXEL_CODEC_PROFILE_JPEG2000_LOSSY;
}

bool is_htj2k_encode_profile(uint32_t codec_profile_code) noexcept {
	return codec_profile_code == PIXEL_CODEC_PROFILE_HTJ2K_LOSSLESS ||
	    codec_profile_code == PIXEL_CODEC_PROFILE_HTJ2K_LOSSLESS_RPCL ||
	    codec_profile_code == PIXEL_CODEC_PROFILE_HTJ2K_LOSSY;
}

void validate_encode_profile_source_constraints(uint32_t codec_profile_code,
    int bits_allocated, int bits_stored) {
	(void)codec_profile_code;
	(void)bits_allocated;
	(void)bits_stored;
}

pixel::Photometric compute_output_photometric_for_encode_profile(
    uid::WellKnown transfer_syntax, uint32_t codec_profile_code,
    std::span<const CodecOptionKv> codec_options,
    bool use_multicomponent_transform, pixel::Photometric source_photometric,
    std::size_t samples_per_pixel,
    bool decoded_source_is_rgb_domain_for_jpeg) {
	if (is_jpeg_encode_profile(codec_profile_code)) {
		return compute_jpeg_output_photometric_or_throw(
		    transfer_syntax, codec_profile_code, codec_options, source_photometric,
		    samples_per_pixel, decoded_source_is_rgb_domain_for_jpeg);
	}
	if (!use_multicomponent_transform) {
		return source_photometric;
	}
	if (codec_profile_code == PIXEL_CODEC_PROFILE_JPEG2000_LOSSLESS ||
	    codec_profile_code == PIXEL_CODEC_PROFILE_HTJ2K_LOSSLESS ||
	    codec_profile_code == PIXEL_CODEC_PROFILE_HTJ2K_LOSSLESS_RPCL) {
		return pixel::Photometric::ybr_rct;
	}
	return pixel::Photometric::ybr_ict;
}

bool encode_profile_uses_lossy_compression(uint32_t codec_profile_code) noexcept {
	return codec_profile_code == PIXEL_CODEC_PROFILE_JPEG2000_LOSSY ||
	    codec_profile_code == PIXEL_CODEC_PROFILE_HTJ2K_LOSSY ||
	    codec_profile_code == PIXEL_CODEC_PROFILE_JPEGLS_NEAR_LOSSLESS ||
	    codec_profile_code == PIXEL_CODEC_PROFILE_JPEG_LOSSY ||
	    codec_profile_code == PIXEL_CODEC_PROFILE_JPEGXL_LOSSY;
}

std::optional<std::string_view> lossy_method_for_encode_profile(
    uint32_t codec_profile_code) noexcept {
	if (codec_profile_code == PIXEL_CODEC_PROFILE_HTJ2K_LOSSY) {
		return std::string_view("ISO_15444_15");
	}
	if (codec_profile_code == PIXEL_CODEC_PROFILE_JPEG2000_LOSSY) {
		return std::string_view("ISO_15444_1");
	}
	if (codec_profile_code == PIXEL_CODEC_PROFILE_JPEGLS_NEAR_LOSSLESS) {
		return std::string_view("ISO_14495_1");
	}
	if (codec_profile_code == PIXEL_CODEC_PROFILE_JPEG_LOSSY) {
		return std::string_view("ISO_10918_1");
	}
	if (codec_profile_code == PIXEL_CODEC_PROFILE_JPEGXL_LOSSY) {
		return std::string_view("ISO_18181_1");
	}
	return std::nullopt;
}

} // namespace dicom::pixel::detail
