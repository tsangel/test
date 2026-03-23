#include "pixel/host/encode/encode_options_policy.hpp"

#include "pixel/host/adapter/host_adapter.hpp"
#include "diagnostics.h"

#include <cstdint>

namespace dicom::pixel::detail {

namespace {
constexpr std::int64_t kDefaultNearLosslessJpegLsError = 2;
constexpr double kDefaultLossyJ2kTargetPsnr = 45.0;
constexpr double kDefaultLossyJpegXlDistance = 1.0;
constexpr std::string_view kConfigureApi = "pixel::EncoderContext::configure";

} // namespace

void validate_transfer_syntax_for_encode_or_throw(
    uid::WellKnown transfer_syntax) {
	if (!transfer_syntax.valid() ||
	    transfer_syntax.uid_type() != UidType::TransferSyntax) {
		diag::error_and_throw(
		    "{} reason=transfer_syntax must be a valid Transfer Syntax UID",
		    kConfigureApi);
	}
}

std::vector<CodecOptionKv> default_codec_options_for_transfer_syntax_or_throw(
    uid::WellKnown transfer_syntax) {
	std::vector<CodecOptionKv> options{};
	uint32_t codec_profile_code = PIXEL_CODEC_PROFILE_UNKNOWN;
	if (!::pixel::runtime::codec_profile_code_from_transfer_syntax(
	        transfer_syntax, &codec_profile_code)) {
		diag::error_and_throw(
		    "{} ts={} reason=transfer syntax is not supported for encoding",
		    kConfigureApi, transfer_syntax.value());
	}

	switch (codec_profile_code) {
	case PIXEL_CODEC_PROFILE_NATIVE_UNCOMPRESSED:
	case PIXEL_CODEC_PROFILE_ENCAPSULATED_UNCOMPRESSED:
	case PIXEL_CODEC_PROFILE_RLE_LOSSLESS:
		return options;
	case PIXEL_CODEC_PROFILE_JPEG_LOSSLESS:
	case PIXEL_CODEC_PROFILE_JPEG_LOSSY:
		options.push_back(CodecOptionKv{
		    .key = "quality",
		    .value = CodecOptionValue{std::int64_t{90}},
		});
		return options;
	case PIXEL_CODEC_PROFILE_JPEGLS_LOSSLESS:
	case PIXEL_CODEC_PROFILE_JPEGLS_NEAR_LOSSLESS: {
		const auto near_lossless_error =
		    codec_profile_code == PIXEL_CODEC_PROFILE_JPEGLS_NEAR_LOSSLESS
		    ? kDefaultNearLosslessJpegLsError
		    : std::int64_t{0};
		options.push_back(CodecOptionKv{
		    .key = "near_lossless_error",
		    .value = CodecOptionValue{near_lossless_error},
		});
		return options;
	}
	case PIXEL_CODEC_PROFILE_JPEG2000_LOSSLESS:
	case PIXEL_CODEC_PROFILE_JPEG2000_LOSSY:
	case PIXEL_CODEC_PROFILE_HTJ2K_LOSSLESS:
	case PIXEL_CODEC_PROFILE_HTJ2K_LOSSLESS_RPCL:
	case PIXEL_CODEC_PROFILE_HTJ2K_LOSSY:
		options.push_back(CodecOptionKv{
		    .key = "target_bpp",
		    .value = CodecOptionValue{0.0},
		});
		options.push_back(CodecOptionKv{
		    .key = "target_psnr",
		    .value = CodecOptionValue{codec_profile_code ==
		            PIXEL_CODEC_PROFILE_JPEG2000_LOSSY ||
		        codec_profile_code == PIXEL_CODEC_PROFILE_HTJ2K_LOSSY
		            ? kDefaultLossyJ2kTargetPsnr
		            : 0.0},
		});
		options.push_back(CodecOptionKv{
		    .key = "threads",
		    .value = CodecOptionValue{std::int64_t{-1}},
		});
		options.push_back(CodecOptionKv{
		    .key = "color_transform",
		    .value = CodecOptionValue{true},
		});
		return options;
	case PIXEL_CODEC_PROFILE_JPEGXL_LOSSLESS:
	case PIXEL_CODEC_PROFILE_JPEGXL_LOSSY:
		options.push_back(CodecOptionKv{
		    .key = "distance",
		    .value = CodecOptionValue{codec_profile_code ==
		            PIXEL_CODEC_PROFILE_JPEGXL_LOSSLESS
		            ? 0.0
		            : kDefaultLossyJpegXlDistance},
		});
		options.push_back(CodecOptionKv{
		    .key = "effort",
		    .value = CodecOptionValue{std::int64_t{7}},
		});
		options.push_back(CodecOptionKv{
		    .key = "threads",
		    .value = CodecOptionValue{std::int64_t{-1}},
		});
		return options;
	case PIXEL_CODEC_PROFILE_JPEGXL_JPEG_RECOMPRESSION:
		diag::error_and_throw(
		    "{} ts={} reason=JPEGXLJPEGRecompression transfer syntax is decode-only",
		    kConfigureApi, transfer_syntax.value());
	default:
		break;
	}

	diag::error_and_throw(
	    "{} ts={} reason=transfer syntax is not supported for encoding",
	    kConfigureApi, transfer_syntax.value());
}

std::vector<CodecOptionKv> build_codec_option_pairs_from_text_or_throw(
    uid::WellKnown transfer_syntax, std::span<const CodecOptionTextKv> codec_opt,
    std::vector<std::string>* owned_option_keys) {
	std::vector<CodecOptionKv> pairs{};
	pairs.reserve(codec_opt.size());
	if (owned_option_keys != nullptr) {
		owned_option_keys->clear();
		owned_option_keys->reserve(codec_opt.size());
	}
	for (const auto& option : codec_opt) {
		if (option.key.empty()) {
			diag::error_and_throw(
			    "{} ts={} reason=codec option key must not be empty",
			    kConfigureApi, transfer_syntax.value());
		}
		std::string_view option_key = option.key;
		if (owned_option_keys != nullptr) {
			owned_option_keys->emplace_back(option.key);
			option_key = owned_option_keys->back();
		}
		pairs.push_back(CodecOptionKv{
		    .key = option_key,
		    .value = CodecOptionValue{std::string(option.value)},
		});
	}
	return pairs;
}

} // namespace dicom::pixel::detail

