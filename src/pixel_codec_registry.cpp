#include "pixel_codec_registry.hpp"

#include "diagnostics.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <variant>

namespace dicom::pixel::detail {
using namespace dicom::literals;
namespace diag = dicom::diag;

std::string_view codec_status_code_name(codec_status_code code) noexcept {
	switch (code) {
	case codec_status_code::ok:
		return "ok";
	case codec_status_code::invalid_argument:
		return "invalid_argument";
	case codec_status_code::unsupported:
		return "unsupported";
	case codec_status_code::backend_error:
		return "backend_error";
	case codec_status_code::internal_error:
		return "internal_error";
	}
	return "unknown";
}

std::string format_codec_error_context(std::string_view function_name,
    std::string_view file_path, uid::WellKnown transfer_syntax,
    std::string_view plugin_key, std::optional<std::size_t> frame_index,
    const codec_error& error) {
	const auto status = codec_status_code_name(error.code);
	const std::string_view stage =
	    error.stage.empty() ? std::string_view("unknown") : std::string_view(error.stage);
	const std::string_view detail =
	    error.detail.empty() ? std::string_view("unspecified codec error")
	                         : std::string_view(error.detail);

	if (frame_index.has_value()) {
		return fmt::format(
		    "{} file={} ts={} plugin={} frame={} status={} stage={} reason={}",
		    function_name, file_path, transfer_syntax.value(), plugin_key,
		    *frame_index, status, stage, detail);
	}
	return fmt::format(
	    "{} file={} ts={} plugin={} status={} stage={} reason={}",
	    function_name, file_path, transfer_syntax.value(), plugin_key, status,
	    stage, detail);
}

[[noreturn]] void throw_codec_error_with_context(
    std::string_view function_name, std::string_view file_path,
    uid::WellKnown transfer_syntax, std::string_view plugin_key,
    std::optional<std::size_t> frame_index, const codec_error& error) {
	diag::error_and_throw("{}", format_codec_error_context(
	    function_name, file_path, transfer_syntax, plugin_key, frame_index, error));
}

bool encode_frame_plugin_encapsulated_uncompressed(
    const CodecEncodeFrameInput& input, const CodecOptions& parsed_options,
    std::vector<std::uint8_t>& out_encoded_frame,
    codec_error& out_error) noexcept;
bool encode_frame_plugin_rle(const CodecEncodeFrameInput& input,
    const CodecOptions& parsed_options,
    std::vector<std::uint8_t>& out_encoded_frame,
    codec_error& out_error) noexcept;
bool encode_frame_plugin_jpeg2k(const CodecEncodeFrameInput& input,
    const CodecOptions& parsed_options,
    std::vector<std::uint8_t>& out_encoded_frame,
    codec_error& out_error) noexcept;
bool encode_frame_plugin_htj2k(const CodecEncodeFrameInput& input,
    const CodecOptions& parsed_options,
    std::vector<std::uint8_t>& out_encoded_frame,
    codec_error& out_error) noexcept;
bool encode_frame_plugin_jpegls(const CodecEncodeFrameInput& input,
    const CodecOptions& parsed_options,
    std::vector<std::uint8_t>& out_encoded_frame,
    codec_error& out_error) noexcept;
bool encode_frame_plugin_jpeg(const CodecEncodeFrameInput& input,
    const CodecOptions& parsed_options,
    std::vector<std::uint8_t>& out_encoded_frame,
    codec_error& out_error) noexcept;
bool encode_frame_plugin_jpegxl(const CodecEncodeFrameInput& input,
    const CodecOptions& parsed_options,
    std::vector<std::uint8_t>& out_encoded_frame,
    codec_error& out_error) noexcept;

bool decode_frame_plugin_native(
    const CodecDecodeFrameInput& input, codec_error& out_error) noexcept;
bool decode_frame_plugin_encapsulated_uncompressed(
    const CodecDecodeFrameInput& input, codec_error& out_error) noexcept;
bool decode_frame_plugin_rle(
    const CodecDecodeFrameInput& input, codec_error& out_error) noexcept;
bool decode_frame_plugin_jpeg2k(
    const CodecDecodeFrameInput& input, codec_error& out_error) noexcept;
bool decode_frame_plugin_htj2k(
    const CodecDecodeFrameInput& input, codec_error& out_error) noexcept;
bool decode_frame_plugin_jpegls(
    const CodecDecodeFrameInput& input, codec_error& out_error) noexcept;
bool decode_frame_plugin_jpeg(
    const CodecDecodeFrameInput& input, codec_error& out_error) noexcept;
bool decode_frame_plugin_jpegxl(
    const CodecDecodeFrameInput& input, codec_error& out_error) noexcept;

namespace {

constexpr double kDefaultLossyJ2kTargetPsnr = 45.0;
constexpr int kDefaultNearLosslessJpegLsError = 2;
constexpr double kDefaultLossyJpegXlDistance = 1.0;

constexpr std::array<codec_option_schema, 0> kNoOptionSchema{};
constexpr std::array<codec_option_schema, 1> kJpegOptionSchema{{
    {"quality", "int", "[1,100]", "90..95"},
}};
constexpr std::array<codec_option_schema, 1> kJpegLsOptionSchema{{
    {"near_lossless_error", "int", "[0,255]", "lossless=0, near-lossless start 2..3"},
}};
constexpr std::array<codec_option_schema, 4> kJpeg2kOptionSchema{{
    {"target_bpp", "double", ">=0", "start 1.5 (1.0 smaller, 2.0 higher quality)"},
    {"target_psnr", "double", ">=0", "start 45 (40 smaller, 55 higher quality)"},
    {"threads", "int", "[-1,0] or >0", "-1(auto), 0(library default), >0 explicit"},
    {"color_transform", "bool", "{true,false}", "RGB source: usually true"},
}};
constexpr std::array<codec_option_schema, 4> kHtj2kOptionSchema{{
    {"target_bpp", "double", ">=0", "start 1.5 (1.0 smaller, 2.0 higher quality)"},
    {"target_psnr", "double", ">=0", "start 50 (45 smaller, 60 higher quality)"},
    {"threads", "int", "[-1,0] or >0", "-1(auto), 0(library default), >0 explicit"},
    {"color_transform", "bool", "{true,false}", "RGB source: usually true"},
}};
constexpr std::array<codec_option_schema, 3> kJpegXlOptionSchema{{
    {"distance", "double", "[0,25]", "lossless=0, lossy start 1.5 (0.5..3.0)"},
    {"effort", "int", "[1,10]", "start 7 (5..8)"},
    {"threads", "int", "[-1,0] or >0", "-1(auto), 0(library default), >0 explicit"},
}};

template <std::size_t N>
[[nodiscard]] constexpr std::span<const codec_option_schema> option_schema_span(
    const std::array<codec_option_schema, N>& option_schema) noexcept {
	return std::span<const codec_option_schema>(option_schema.data(),
	    option_schema.size());
}

[[nodiscard]] std::string normalize_option_key(std::string_view key) {
	std::string normalized{};
	normalized.reserve(key.size());
	for (const unsigned char ch : key) {
		if (ch == '_' || ch == '-' || ch == ' ' || ch == '\t') {
			continue;
		}
		normalized.push_back(static_cast<char>(std::tolower(ch)));
	}
	return normalized;
}

[[nodiscard]] bool option_key_matches(
    std::string_view key, std::string_view expected) {
	return normalize_option_key(key) == normalize_option_key(expected);
}

[[nodiscard]] std::optional<double> option_value_as_double(
    const codec_option_value& value) {
	if (const auto* v = std::get_if<double>(&value)) {
		return *v;
	}
	if (const auto* v = std::get_if<std::int64_t>(&value)) {
		return static_cast<double>(*v);
	}
	return std::nullopt;
}

[[nodiscard]] std::optional<std::int64_t> option_value_as_int64(
    const codec_option_value& value) {
	if (const auto* v = std::get_if<std::int64_t>(&value)) {
		return *v;
	}
	if (const auto* v = std::get_if<double>(&value)) {
		if (!std::isfinite(*v) || std::floor(*v) != *v) {
			return std::nullopt;
		}
		if (*v < static_cast<double>(std::numeric_limits<std::int64_t>::min()) ||
		    *v > static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
			return std::nullopt;
		}
		return static_cast<std::int64_t>(*v);
	}
	return std::nullopt;
}

[[nodiscard]] std::optional<bool> option_value_as_bool(
    const codec_option_value& value) {
	if (const auto* v = std::get_if<bool>(&value)) {
		return *v;
	}
	if (const auto* v = std::get_if<std::int64_t>(&value)) {
		if (*v == 0) {
			return false;
		}
		if (*v == 1) {
			return true;
		}
		return std::nullopt;
	}
	if (const auto* v = std::get_if<double>(&value)) {
		if (!std::isfinite(*v)) {
			return std::nullopt;
		}
		if (*v == 0.0) {
			return false;
		}
		if (*v == 1.0) {
			return true;
		}
		return std::nullopt;
	}
	return std::nullopt;
}

void push_option(codec_option_pairs& pairs, std::string_view key, double value) {
	pairs.push_back(codec_option_kv{key, codec_option_value{value}});
}

void push_option(codec_option_pairs& pairs, std::string_view key, std::int64_t value) {
	pairs.push_back(codec_option_kv{key, codec_option_value{value}});
}

void push_option(codec_option_pairs& pairs, std::string_view key, bool value) {
	pairs.push_back(codec_option_kv{key, codec_option_value{value}});
}

[[nodiscard]] std::optional<CodecOptions> default_no_compression(
    uid::WellKnown transfer_syntax) {
	(void)transfer_syntax;
	return CodecOptions{NoCompression{}};
}

[[nodiscard]] std::optional<CodecOptions> default_rle_options(
    uid::WellKnown transfer_syntax) {
	(void)transfer_syntax;
	return CodecOptions{RleOptions{}};
}

[[nodiscard]] std::optional<CodecOptions> default_jpeg_options(
    uid::WellKnown transfer_syntax) {
	(void)transfer_syntax;
	return CodecOptions{JpegOptions{}};
}

[[nodiscard]] std::optional<CodecOptions> default_jpegls_options(
    uid::WellKnown transfer_syntax) {
	JpegLsOptions options{};
	if (transfer_syntax.is_lossy()) {
		options.near_lossless_error = kDefaultNearLosslessJpegLsError;
	}
	return CodecOptions{options};
}

[[nodiscard]] std::optional<CodecOptions> default_jpeg2k_options(
    uid::WellKnown transfer_syntax) {
	J2kOptions options{};
	if (transfer_syntax.is_lossy()) {
		options.target_psnr = kDefaultLossyJ2kTargetPsnr;
	}
	return CodecOptions{options};
}

[[nodiscard]] std::optional<CodecOptions> default_htj2k_options(
    uid::WellKnown transfer_syntax) {
	Htj2kOptions options{};
	if (transfer_syntax.is_lossy()) {
		options.target_psnr = kDefaultLossyJ2kTargetPsnr;
	}
	return CodecOptions{options};
}

[[nodiscard]] std::optional<CodecOptions> default_jpegxl_options(
    uid::WellKnown transfer_syntax) {
	JpegXlOptions options{};
	if (transfer_syntax == "JPEGXLLossless"_uid) {
		options.distance = 0.0;
	} else if (transfer_syntax == "JPEGXL"_uid) {
		options.distance = kDefaultLossyJpegXlDistance;
	}
	return CodecOptions{options};
}

[[nodiscard]] std::optional<std::string> validate_no_compression_options(
    uid::WellKnown transfer_syntax, const CodecOptions& codec_opt) {
	(void)transfer_syntax;
	if (!std::holds_alternative<NoCompression>(codec_opt)) {
		return std::string("codec requires NoCompression options");
	}
	return std::nullopt;
}

[[nodiscard]] std::optional<std::string> validate_rle_options(
    uid::WellKnown transfer_syntax, const CodecOptions& codec_opt) {
	(void)transfer_syntax;
	if (!std::holds_alternative<RleOptions>(codec_opt)) {
		return std::string("codec requires RleOptions");
	}
	return std::nullopt;
}

[[nodiscard]] std::optional<std::string> validate_jpeg_options(
    uid::WellKnown transfer_syntax, const CodecOptions& codec_opt) {
	(void)transfer_syntax;
	if (!std::holds_alternative<JpegOptions>(codec_opt)) {
		return std::string("codec requires JpegOptions");
	}
	const auto& options = std::get<JpegOptions>(codec_opt);
	if (options.quality < 1 || options.quality > 100) {
		return std::string("JpegOptions.quality must be in [1,100]");
	}
	return std::nullopt;
}

[[nodiscard]] std::optional<std::string> validate_jpegls_options(
    uid::WellKnown transfer_syntax, const CodecOptions& codec_opt) {
	if (!std::holds_alternative<JpegLsOptions>(codec_opt)) {
		return std::string("codec requires JpegLsOptions");
	}
	const auto& options = std::get<JpegLsOptions>(codec_opt);
	if (options.near_lossless_error < 0 || options.near_lossless_error > 255) {
		return std::string("JpegLsOptions.near_lossless_error must be in [0,255]");
	}
	if (transfer_syntax.is_lossless() && options.near_lossless_error != 0) {
		return std::string("JPEG-LS lossless transfer syntax requires near_lossless_error=0");
	}
	if (transfer_syntax.is_lossy() && options.near_lossless_error <= 0) {
		return std::string("JPEG-LS lossy transfer syntax requires near_lossless_error>0");
	}
	return std::nullopt;
}

[[nodiscard]] std::optional<std::string> validate_jpeg2k_options(
    uid::WellKnown transfer_syntax, const CodecOptions& codec_opt) {
	(void)transfer_syntax;
	if (!std::holds_alternative<J2kOptions>(codec_opt)) {
		return std::string("codec requires J2kOptions");
	}
	const auto& options = std::get<J2kOptions>(codec_opt);
	if (options.target_bpp < 0.0 || options.target_psnr < 0.0) {
		return std::string("J2kOptions.target_bpp/target_psnr must be >= 0");
	}
	if (options.threads < -1) {
		return std::string("J2kOptions.threads must be -1, 0, or positive");
	}
	return std::nullopt;
}

[[nodiscard]] std::optional<std::string> validate_htj2k_options(
    uid::WellKnown transfer_syntax, const CodecOptions& codec_opt) {
	(void)transfer_syntax;
	if (!std::holds_alternative<Htj2kOptions>(codec_opt)) {
		return std::string("codec requires Htj2kOptions");
	}
	const auto& options = std::get<Htj2kOptions>(codec_opt);
	if (options.target_bpp < 0.0 || options.target_psnr < 0.0) {
		return std::string("Htj2kOptions.target_bpp/target_psnr must be >= 0");
	}
	if (options.threads < -1) {
		return std::string("Htj2kOptions.threads must be -1, 0, or positive");
	}
	return std::nullopt;
}

[[nodiscard]] std::optional<std::string> validate_jpegxl_options(
    uid::WellKnown transfer_syntax, const CodecOptions& codec_opt) {
	if (!std::holds_alternative<JpegXlOptions>(codec_opt)) {
		return std::string("codec requires JpegXlOptions");
	}
	const auto& options = std::get<JpegXlOptions>(codec_opt);
	if (options.threads < -1) {
		return std::string("JpegXlOptions.threads must be -1, 0, or positive");
	}
	if (options.effort < 1 || options.effort > 10) {
		return std::string("JpegXlOptions.effort must be in [1,10]");
	}
	if (!std::isfinite(options.distance) || options.distance < 0.0 ||
	    options.distance > 25.0) {
		return std::string("JpegXlOptions.distance must be in [0,25]");
	}
	if (transfer_syntax == "JPEGXLJPEGRecompression"_uid) {
		return std::string(
		    "JPEGXLJPEGRecompression transfer syntax is decode-only");
	}
	if (transfer_syntax == "JPEGXLLossless"_uid && options.distance != 0.0) {
		return std::string(
		    "JPEGXLLossless transfer syntax requires distance=0");
	}
	if (transfer_syntax == "JPEGXL"_uid && options.distance <= 0.0) {
		return std::string("JPEGXL transfer syntax requires distance>0");
	}
	return std::nullopt;
}

[[nodiscard]] std::optional<std::string> export_no_compression_options(
    uid::WellKnown transfer_syntax, const CodecOptions& codec_opt,
    codec_option_pairs& out_pairs) {
	(void)transfer_syntax;
	out_pairs.clear();
	if (!std::holds_alternative<NoCompression>(codec_opt)) {
		return std::string("codec requires NoCompression options");
	}
	return std::nullopt;
}

[[nodiscard]] std::optional<std::string> parse_no_compression_options(
    uid::WellKnown transfer_syntax, std::span<const codec_option_kv> in_pairs,
    CodecOptions& out_codec_opt) {
	if (!in_pairs.empty()) {
		return std::string("NoCompression options do not accept key/value pairs");
	}
	out_codec_opt = CodecOptions{NoCompression{}};
	return validate_no_compression_options(transfer_syntax, out_codec_opt);
}

[[nodiscard]] std::optional<std::string> export_rle_options(
    uid::WellKnown transfer_syntax, const CodecOptions& codec_opt,
    codec_option_pairs& out_pairs) {
	(void)transfer_syntax;
	out_pairs.clear();
	if (!std::holds_alternative<RleOptions>(codec_opt)) {
		return std::string("codec requires RleOptions");
	}
	return std::nullopt;
}

[[nodiscard]] std::optional<std::string> parse_rle_options(
    uid::WellKnown transfer_syntax, std::span<const codec_option_kv> in_pairs,
    CodecOptions& out_codec_opt) {
	if (!in_pairs.empty()) {
		return std::string("RLE options do not accept key/value pairs");
	}
	out_codec_opt = CodecOptions{RleOptions{}};
	return validate_rle_options(transfer_syntax, out_codec_opt);
}

[[nodiscard]] std::optional<std::string> export_jpeg_options(
    uid::WellKnown transfer_syntax, const CodecOptions& codec_opt,
    codec_option_pairs& out_pairs) {
	if (const auto validation_error =
	        validate_jpeg_options(transfer_syntax, codec_opt)) {
		return validation_error;
	}
	const auto& options = std::get<JpegOptions>(codec_opt);
	out_pairs.clear();
	push_option(out_pairs, "quality", static_cast<std::int64_t>(options.quality));
	return std::nullopt;
}

[[nodiscard]] std::optional<std::string> parse_jpeg_options(
    uid::WellKnown transfer_syntax, std::span<const codec_option_kv> in_pairs,
    CodecOptions& out_codec_opt) {
	JpegOptions options{};
	for (const auto& kv : in_pairs) {
		if (option_key_matches(kv.key, "quality")) {
			const auto parsed = option_value_as_int64(kv.value);
			if (!parsed || *parsed < static_cast<std::int64_t>(std::numeric_limits<int>::min()) ||
			    *parsed > static_cast<std::int64_t>(std::numeric_limits<int>::max())) {
				return std::string("quality must be an integer in [1,100]");
			}
			options.quality = static_cast<int>(*parsed);
			continue;
		}
		return std::string("unknown JPEG option key: ") + std::string(kv.key);
	}
	out_codec_opt = CodecOptions{options};
	return validate_jpeg_options(transfer_syntax, out_codec_opt);
}

[[nodiscard]] std::optional<std::string> export_jpegls_options(
    uid::WellKnown transfer_syntax, const CodecOptions& codec_opt,
    codec_option_pairs& out_pairs) {
	if (const auto validation_error =
	        validate_jpegls_options(transfer_syntax, codec_opt)) {
		return validation_error;
	}
	const auto& options = std::get<JpegLsOptions>(codec_opt);
	out_pairs.clear();
	push_option(out_pairs, "near_lossless_error",
	    static_cast<std::int64_t>(options.near_lossless_error));
	return std::nullopt;
}

[[nodiscard]] std::optional<std::string> parse_jpegls_options(
    uid::WellKnown transfer_syntax, std::span<const codec_option_kv> in_pairs,
    CodecOptions& out_codec_opt) {
	JpegLsOptions options{};
	for (const auto& kv : in_pairs) {
		if (option_key_matches(kv.key, "near_lossless_error")) {
			const auto parsed = option_value_as_int64(kv.value);
			if (!parsed || *parsed < static_cast<std::int64_t>(std::numeric_limits<int>::min()) ||
			    *parsed > static_cast<std::int64_t>(std::numeric_limits<int>::max())) {
				return std::string("near_lossless_error must be an integer in [0,255]");
			}
			options.near_lossless_error = static_cast<int>(*parsed);
			continue;
		}
		return std::string("unknown JPEG-LS option key: ") + std::string(kv.key);
	}
	out_codec_opt = CodecOptions{options};
	return validate_jpegls_options(transfer_syntax, out_codec_opt);
}

[[nodiscard]] std::optional<std::string> export_jpeg2k_options(
    uid::WellKnown transfer_syntax, const CodecOptions& codec_opt,
    codec_option_pairs& out_pairs) {
	if (const auto validation_error =
	        validate_jpeg2k_options(transfer_syntax, codec_opt)) {
		return validation_error;
	}
	const auto& options = std::get<J2kOptions>(codec_opt);
	out_pairs.clear();
	push_option(out_pairs, "target_bpp", options.target_bpp);
	push_option(out_pairs, "target_psnr", options.target_psnr);
	push_option(out_pairs, "threads", static_cast<std::int64_t>(options.threads));
	push_option(out_pairs, "color_transform", options.use_color_transform);
	return std::nullopt;
}

[[nodiscard]] std::optional<std::string> parse_jpeg2k_options(
    uid::WellKnown transfer_syntax, std::span<const codec_option_kv> in_pairs,
    CodecOptions& out_codec_opt) {
	J2kOptions options{};
	for (const auto& kv : in_pairs) {
		if (option_key_matches(kv.key, "target_bpp") ||
		    option_key_matches(kv.key, "bpp")) {
			const auto parsed = option_value_as_double(kv.value);
			if (!parsed) {
				return std::string("target_bpp must be numeric");
			}
			options.target_bpp = *parsed;
			continue;
		}
		if (option_key_matches(kv.key, "target_psnr") ||
		    option_key_matches(kv.key, "psnr")) {
			const auto parsed = option_value_as_double(kv.value);
			if (!parsed) {
				return std::string("target_psnr must be numeric");
			}
			options.target_psnr = *parsed;
			continue;
		}
		if (option_key_matches(kv.key, "threads")) {
			const auto parsed = option_value_as_int64(kv.value);
			if (!parsed || *parsed < static_cast<std::int64_t>(std::numeric_limits<int>::min()) ||
			    *parsed > static_cast<std::int64_t>(std::numeric_limits<int>::max())) {
				return std::string("threads must be an integer");
			}
			options.threads = static_cast<int>(*parsed);
			continue;
		}
		if (option_key_matches(kv.key, "color_transform") ||
		    option_key_matches(kv.key, "mct") ||
		    option_key_matches(kv.key, "use_mct")) {
			const auto parsed = option_value_as_bool(kv.value);
			if (!parsed) {
				return std::string("color_transform/mct must be bool (or 0/1)");
			}
			options.use_color_transform = *parsed;
			continue;
		}
		return std::string("unknown JPEG2000 option key: ") + std::string(kv.key);
	}
	out_codec_opt = CodecOptions{options};
	return validate_jpeg2k_options(transfer_syntax, out_codec_opt);
}

[[nodiscard]] std::optional<std::string> export_htj2k_options(
    uid::WellKnown transfer_syntax, const CodecOptions& codec_opt,
    codec_option_pairs& out_pairs) {
	if (const auto validation_error =
	        validate_htj2k_options(transfer_syntax, codec_opt)) {
		return validation_error;
	}
	const auto& options = std::get<Htj2kOptions>(codec_opt);
	out_pairs.clear();
	push_option(out_pairs, "target_bpp", options.target_bpp);
	push_option(out_pairs, "target_psnr", options.target_psnr);
	push_option(out_pairs, "threads", static_cast<std::int64_t>(options.threads));
	push_option(out_pairs, "color_transform", options.use_color_transform);
	return std::nullopt;
}

[[nodiscard]] std::optional<std::string> parse_htj2k_options(
    uid::WellKnown transfer_syntax, std::span<const codec_option_kv> in_pairs,
    CodecOptions& out_codec_opt) {
	Htj2kOptions options{};
	for (const auto& kv : in_pairs) {
		if (option_key_matches(kv.key, "target_bpp") ||
		    option_key_matches(kv.key, "bpp")) {
			const auto parsed = option_value_as_double(kv.value);
			if (!parsed) {
				return std::string("target_bpp must be numeric");
			}
			options.target_bpp = *parsed;
			continue;
		}
		if (option_key_matches(kv.key, "target_psnr") ||
		    option_key_matches(kv.key, "psnr")) {
			const auto parsed = option_value_as_double(kv.value);
			if (!parsed) {
				return std::string("target_psnr must be numeric");
			}
			options.target_psnr = *parsed;
			continue;
		}
		if (option_key_matches(kv.key, "threads")) {
			const auto parsed = option_value_as_int64(kv.value);
			if (!parsed || *parsed < static_cast<std::int64_t>(std::numeric_limits<int>::min()) ||
			    *parsed > static_cast<std::int64_t>(std::numeric_limits<int>::max())) {
				return std::string("threads must be an integer");
			}
			options.threads = static_cast<int>(*parsed);
			continue;
		}
		if (option_key_matches(kv.key, "color_transform") ||
		    option_key_matches(kv.key, "mct") ||
		    option_key_matches(kv.key, "use_mct")) {
			const auto parsed = option_value_as_bool(kv.value);
			if (!parsed) {
				return std::string("color_transform/mct must be bool (or 0/1)");
			}
			options.use_color_transform = *parsed;
			continue;
		}
		return std::string("unknown HTJ2K option key: ") + std::string(kv.key);
	}
	out_codec_opt = CodecOptions{options};
	return validate_htj2k_options(transfer_syntax, out_codec_opt);
}

[[nodiscard]] std::optional<std::string> export_jpegxl_options(
    uid::WellKnown transfer_syntax, const CodecOptions& codec_opt,
    codec_option_pairs& out_pairs) {
	if (const auto validation_error =
	        validate_jpegxl_options(transfer_syntax, codec_opt)) {
		return validation_error;
	}
	const auto& options = std::get<JpegXlOptions>(codec_opt);
	out_pairs.clear();
	push_option(out_pairs, "distance", options.distance);
	push_option(out_pairs, "effort", static_cast<std::int64_t>(options.effort));
	push_option(out_pairs, "threads", static_cast<std::int64_t>(options.threads));
	return std::nullopt;
}

[[nodiscard]] std::optional<std::string> parse_jpegxl_options(
    uid::WellKnown transfer_syntax, std::span<const codec_option_kv> in_pairs,
    CodecOptions& out_codec_opt) {
	JpegXlOptions options{};
	for (const auto& kv : in_pairs) {
		if (option_key_matches(kv.key, "distance")) {
			const auto parsed = option_value_as_double(kv.value);
			if (!parsed) {
				return std::string("distance must be numeric");
			}
			options.distance = *parsed;
			continue;
		}
		if (option_key_matches(kv.key, "effort")) {
			const auto parsed = option_value_as_int64(kv.value);
			if (!parsed || *parsed < static_cast<std::int64_t>(std::numeric_limits<int>::min()) ||
			    *parsed > static_cast<std::int64_t>(std::numeric_limits<int>::max())) {
				return std::string("effort must be an integer");
			}
			options.effort = static_cast<int>(*parsed);
			continue;
		}
		if (option_key_matches(kv.key, "threads")) {
			const auto parsed = option_value_as_int64(kv.value);
			if (!parsed || *parsed < static_cast<std::int64_t>(std::numeric_limits<int>::min()) ||
			    *parsed > static_cast<std::int64_t>(std::numeric_limits<int>::max())) {
				return std::string("threads must be an integer");
			}
			options.threads = static_cast<int>(*parsed);
			continue;
		}
		return std::string("unknown JPEG-XL option key: ") + std::string(kv.key);
	}
	out_codec_opt = CodecOptions{options};
	return validate_jpegxl_options(transfer_syntax, out_codec_opt);
}

[[nodiscard]] std::string_view plugin_key_for_transfer_syntax(
    uid::WellKnown transfer_syntax) noexcept {
	if (transfer_syntax.is_uncompressed()) {
		return transfer_syntax.is_encapsulated()
		           ? std::string_view("encapsulated-uncompressed")
		           : std::string_view("native");
	}
	if (transfer_syntax.is_rle()) {
		return std::string_view("rle");
	}
	if (transfer_syntax.is_htj2k()) {
		return std::string_view("htj2k");
	}
	if (transfer_syntax.is_jpeg2000()) {
		return std::string_view("jpeg2k");
	}
	if (transfer_syntax.is_jpegls()) {
		return std::string_view("jpegls");
	}
	if (transfer_syntax.is_jpegxl()) {
		return std::string_view("jpegxl");
	}
	if (transfer_syntax.is_jpeg_family()) {
		return std::string_view("jpeg");
	}
	return {};
}

[[nodiscard]] codec_profile profile_for_transfer_syntax(
    uid::WellKnown transfer_syntax) noexcept {
	if (transfer_syntax.is_uncompressed()) {
		return transfer_syntax.is_encapsulated()
		           ? codec_profile::encapsulated_uncompressed
		           : codec_profile::native_uncompressed;
	}
	if (transfer_syntax.is_rle()) {
		return codec_profile::rle_lossless;
	}
	if (transfer_syntax.is_htj2k()) {
		if (transfer_syntax == "HTJ2KLosslessRPCL"_uid) {
			return codec_profile::htj2k_lossless_rpcl;
		}
		return transfer_syntax.is_lossless() ? codec_profile::htj2k_lossless
		                                     : codec_profile::htj2k_lossy;
	}
	if (transfer_syntax.is_jpeg2000()) {
		return transfer_syntax.is_lossless() ? codec_profile::jpeg2000_lossless
		                                     : codec_profile::jpeg2000_lossy;
	}
	if (transfer_syntax.is_jpegls()) {
		return transfer_syntax.is_lossless()
		           ? codec_profile::jpegls_lossless
		           : codec_profile::jpegls_near_lossless;
	}
	if (transfer_syntax.is_jpegxl()) {
		if (transfer_syntax == "JPEGXLJPEGRecompression"_uid) {
			return codec_profile::jpegxl_jpeg_recompression;
		}
		return transfer_syntax.is_lossless() ? codec_profile::jpegxl_lossless
		                                     : codec_profile::jpegxl_lossy;
	}
	if (transfer_syntax.is_jpeg_family()) {
		return transfer_syntax.is_lossless() ? codec_profile::jpeg_lossless
		                                     : codec_profile::jpeg_lossy;
	}
	return codec_profile::unknown;
}

void register_plugin_if_missing(codec_registry& registry,
    const codec_plugin& plugin) {
	if (registry.find_plugin(plugin.key)) {
		return;
	}
	(void)registry.register_plugin(plugin);
}

void register_binding_if_missing(codec_registry& registry,
    const transfer_syntax_plugin_binding& binding) {
	if (registry.find_binding(binding.transfer_syntax)) {
		return;
	}
	(void)registry.register_binding(binding);
}

} // namespace

bool codec_registry::register_plugin(const codec_plugin& plugin) {
	if (plugin.key.empty() || find_plugin(plugin.key)) {
		return false;
	}
	plugins_.push_back(plugin);
	const auto plugin_index = plugins_.size() - 1;
	for (auto& binding : bindings_) {
		if (binding.plugin_key == plugin.key &&
		    binding.plugin_index == kInvalidPluginIndex) {
			binding.plugin_index = plugin_index;
		}
	}
	return true;
}

bool codec_registry::register_binding(
    const transfer_syntax_plugin_binding& binding) {
	if (!binding.transfer_syntax.valid() ||
	    binding.transfer_syntax.uid_type() != UidType::TransferSyntax ||
	    binding.plugin_key.empty() || find_binding(binding.transfer_syntax)) {
		return false;
	}
	auto normalized = binding;
	normalized.plugin_index = find_plugin_index(binding.plugin_key);
	bindings_.push_back(normalized);
	return true;
}

std::size_t codec_registry::find_plugin_index(
    std::string_view plugin_key) const noexcept {
	for (std::size_t i = 0; i < plugins_.size(); ++i) {
		if (plugins_[i].key == plugin_key) {
			return i;
		}
	}
	return kInvalidPluginIndex;
}

const codec_plugin* codec_registry::find_plugin(
    std::string_view plugin_key) const noexcept {
	const auto plugin_index = find_plugin_index(plugin_key);
	if (plugin_index != kInvalidPluginIndex) {
		return &plugins_[plugin_index];
	}
	return nullptr;
}

const transfer_syntax_plugin_binding* codec_registry::find_binding(
    uid::WellKnown transfer_syntax) const noexcept {
	for (const auto& binding : bindings_) {
		if (binding.transfer_syntax == transfer_syntax) {
			return &binding;
		}
	}
	return nullptr;
}

const codec_plugin* codec_registry::resolve_plugin(
    uid::WellKnown transfer_syntax, codec_direction direction) const noexcept {
	const auto* binding = find_binding(transfer_syntax);
	if (!binding) {
		return nullptr;
	}
	if (direction == codec_direction::encode && !binding->encode_supported) {
		return nullptr;
	}
	if (direction == codec_direction::decode && !binding->decode_supported) {
		return nullptr;
	}
	if (binding->plugin_index < plugins_.size()) {
		const auto& plugin = plugins_[binding->plugin_index];
		if (plugin.key == binding->plugin_key) {
			return &plugin;
		}
	}
	return find_plugin(binding->plugin_key);
}

void codec_registry::clear() {
	plugins_.clear();
	bindings_.clear();
}

codec_registry& global_codec_registry() {
	static codec_registry registry = [] {
		codec_registry value{};
		register_default_codec_plugins(value);
		register_default_transfer_syntax_bindings(value);
		return value;
	}();
	return registry;
}

void register_default_codec_plugins(codec_registry& registry) {
		register_plugin_if_missing(registry, codec_plugin{
			.key = "native",
			.display_name = "Native Uncompressed",
			.option_schema = option_schema_span(kNoOptionSchema),
			.default_options = &default_no_compression,
			.validate_options = &validate_no_compression_options,
			.export_options = &export_no_compression_options,
			.parse_options = &parse_no_compression_options,
			.encode_frame = nullptr,
			.decode_frame = &decode_frame_plugin_native,
		});

	register_plugin_if_missing(registry, codec_plugin{
		.key = "encapsulated-uncompressed",
		.display_name = "Encapsulated Uncompressed",
		.option_schema = option_schema_span(kNoOptionSchema),
		.default_options = &default_no_compression,
		.validate_options = &validate_no_compression_options,
		.export_options = &export_no_compression_options,
			.parse_options = &parse_no_compression_options,
			.encode_frame = &encode_frame_plugin_encapsulated_uncompressed,
			.decode_frame = &decode_frame_plugin_encapsulated_uncompressed,
		});

	register_plugin_if_missing(registry, codec_plugin{
		.key = "rle",
		.display_name = "RLE Lossless",
		.option_schema = option_schema_span(kNoOptionSchema),
		.default_options = &default_rle_options,
		.validate_options = &validate_rle_options,
		.export_options = &export_rle_options,
			.parse_options = &parse_rle_options,
			.encode_frame = &encode_frame_plugin_rle,
			.decode_frame = &decode_frame_plugin_rle,
		});

	register_plugin_if_missing(registry, codec_plugin{
		.key = "jpeg2k",
		.display_name = "JPEG 2000",
		.option_schema = option_schema_span(kJpeg2kOptionSchema),
		.default_options = &default_jpeg2k_options,
		.validate_options = &validate_jpeg2k_options,
		.export_options = &export_jpeg2k_options,
			.parse_options = &parse_jpeg2k_options,
			.encode_frame = &encode_frame_plugin_jpeg2k,
			.decode_frame = &decode_frame_plugin_jpeg2k,
		});

	register_plugin_if_missing(registry, codec_plugin{
		.key = "htj2k",
		.display_name = "HTJ2K",
		.option_schema = option_schema_span(kHtj2kOptionSchema),
		.default_options = &default_htj2k_options,
		.validate_options = &validate_htj2k_options,
		.export_options = &export_htj2k_options,
			.parse_options = &parse_htj2k_options,
			.encode_frame = &encode_frame_plugin_htj2k,
			.decode_frame = &decode_frame_plugin_htj2k,
		});

	register_plugin_if_missing(registry, codec_plugin{
		.key = "jpegls",
		.display_name = "JPEG-LS",
		.option_schema = option_schema_span(kJpegLsOptionSchema),
		.default_options = &default_jpegls_options,
		.validate_options = &validate_jpegls_options,
		.export_options = &export_jpegls_options,
			.parse_options = &parse_jpegls_options,
			.encode_frame = &encode_frame_plugin_jpegls,
			.decode_frame = &decode_frame_plugin_jpegls,
		});

	register_plugin_if_missing(registry, codec_plugin{
		.key = "jpeg",
		.display_name = "JPEG",
		.option_schema = option_schema_span(kJpegOptionSchema),
		.default_options = &default_jpeg_options,
		.validate_options = &validate_jpeg_options,
		.export_options = &export_jpeg_options,
			.parse_options = &parse_jpeg_options,
			.encode_frame = &encode_frame_plugin_jpeg,
			.decode_frame = &decode_frame_plugin_jpeg,
		});

	register_plugin_if_missing(registry, codec_plugin{
		.key = "jpegxl",
		.display_name = "JPEG XL",
		.option_schema = option_schema_span(kJpegXlOptionSchema),
		.default_options = &default_jpegxl_options,
		.validate_options = &validate_jpegxl_options,
		.export_options = &export_jpegxl_options,
			.parse_options = &parse_jpegxl_options,
			.encode_frame = &encode_frame_plugin_jpegxl,
			.decode_frame = &decode_frame_plugin_jpegxl,
		});
}

void register_default_transfer_syntax_bindings(codec_registry& registry) {
	for (const auto& uid_entry : kUidRegistry) {
		if (uid_entry.uid_type != UidType::TransferSyntax) {
			continue;
		}
		const auto transfer_syntax_opt = uid::from_keyword(uid_entry.keyword);
		if (!transfer_syntax_opt) {
			continue;
		}

		const auto transfer_syntax = *transfer_syntax_opt;
		if (!transfer_syntax.supports_pixel_encode() &&
		    !transfer_syntax.supports_pixel_decode()) {
			continue;
		}

		const auto plugin_key = plugin_key_for_transfer_syntax(transfer_syntax);
		if (plugin_key.empty()) {
			continue;
		}

		register_binding_if_missing(registry, transfer_syntax_plugin_binding{
		    .transfer_syntax = transfer_syntax,
		    .plugin_key = plugin_key,
		    .profile = profile_for_transfer_syntax(transfer_syntax),
		    .encode_supported = transfer_syntax.supports_pixel_encode(),
		    .decode_supported = transfer_syntax.supports_pixel_decode(),
		});
	}
}

} // namespace dicom::pixel::detail
