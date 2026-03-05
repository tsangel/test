#include "dicom.h"

#include "pixel/encode/core/encode_set_pixel_data_runner.hpp"
#include "diagnostics.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dicom {

namespace pixel::detail {

namespace {
using namespace dicom::literals;

constexpr std::string_view kRuntimePluginKey = "runtime";
constexpr std::int64_t kDefaultNearLosslessJpegLsError = 2;
constexpr double kDefaultLossyJ2kTargetPsnr = 45.0;
constexpr double kDefaultLossyJpegXlDistance = 1.0;

void validate_transfer_syntax_for_encode_or_throw(
    std::string_view function_name, uid::WellKnown transfer_syntax) {
	if (!transfer_syntax.valid() ||
	    transfer_syntax.uid_type() != UidType::TransferSyntax) {
		diag::error_and_throw(
		    "{} reason=transfer_syntax must be a valid Transfer Syntax UID",
		    function_name);
	}
}

std::vector<pixel::CodecOptionKv> default_codec_options_for_transfer_syntax_or_throw(
    std::string_view function_name, uid::WellKnown transfer_syntax) {
	std::vector<pixel::CodecOptionKv> options{};

	if (transfer_syntax.is_uncompressed() || transfer_syntax.is_rle()) {
		return options;
	}
	if (transfer_syntax.is_jpeg_family() && !transfer_syntax.is_jpegls() &&
	    !transfer_syntax.is_jpeg2000() && !transfer_syntax.is_htj2k() &&
	    !transfer_syntax.is_jpegxl()) {
		options.push_back(CodecOptionKv{
		    .key = "quality",
		    .value = CodecOptionValue{std::int64_t{90}},
		});
		return options;
	}
	if (transfer_syntax.is_jpegls()) {
		const auto near_lossless_error = transfer_syntax.is_lossy()
		    ? kDefaultNearLosslessJpegLsError
		    : std::int64_t{0};
		options.push_back(CodecOptionKv{
		    .key = "near_lossless_error",
		    .value = CodecOptionValue{near_lossless_error},
		});
		return options;
	}
	if (transfer_syntax.is_jpeg2000() || transfer_syntax.is_htj2k()) {
		options.push_back(CodecOptionKv{
		    .key = "target_bpp",
		    .value = CodecOptionValue{0.0},
		});
		options.push_back(CodecOptionKv{
		    .key = "target_psnr",
		    .value = CodecOptionValue{
		        transfer_syntax.is_lossy() ? kDefaultLossyJ2kTargetPsnr : 0.0},
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
	}
	if (transfer_syntax.is_jpegxl()) {
		if (transfer_syntax == "JPEGXLJPEGRecompression"_uid) {
			diag::error_and_throw(
			    "{} ts={} reason=JPEGXLJPEGRecompression transfer syntax is decode-only",
			    function_name, transfer_syntax.value());
		}
		options.push_back(CodecOptionKv{
		    .key = "distance",
		    .value = CodecOptionValue{
		        transfer_syntax.is_lossless() ? 0.0 : kDefaultLossyJpegXlDistance},
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
	}

	diag::error_and_throw(
	    "{} ts={} reason=transfer syntax is not supported for encoding",
	    function_name, transfer_syntax.value());
}

std::vector<pixel::CodecOptionKv> build_codec_option_pairs_from_text_or_throw(
    std::string_view function_name, std::string_view file_path,
    uid::WellKnown transfer_syntax, std::span<const pixel::CodecOptionTextKv> codec_opt,
    std::vector<std::string>* owned_option_keys = nullptr) {
	std::vector<pixel::CodecOptionKv> pairs{};
	pairs.reserve(codec_opt.size());
	if (owned_option_keys != nullptr) {
		owned_option_keys->clear();
		owned_option_keys->reserve(codec_opt.size());
	}
	for (const auto& option : codec_opt) {
		if (option.key.empty()) {
			diag::error_and_throw(
			    "{} file={} ts={} plugin={} reason=codec option key must not be empty",
			    function_name, file_path, transfer_syntax.value(),
			    kRuntimePluginKey);
		}
		std::string_view option_key = option.key;
		if (owned_option_keys != nullptr) {
			owned_option_keys->emplace_back(option.key);
			option_key = owned_option_keys->back();
		}
		pairs.push_back(pixel::CodecOptionKv{
		    .key = option_key,
		    .value = pixel::CodecOptionValue{std::string(option.value)},
		});
	}
	return pairs;
}

void validate_encoder_context_for_set_pixel_data_or_throw(
    std::string_view file_path, uid::WellKnown transfer_syntax,
    bool encoder_context_configured, uid::WellKnown encoder_context_transfer_syntax) {
	if (!encoder_context_configured) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} ts={} reason=encoder context is not configured",
		    file_path, transfer_syntax.value());
	}
	if (!encoder_context_transfer_syntax.valid() ||
	    encoder_context_transfer_syntax != transfer_syntax) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} ts={} ctx_ts={} reason=encoder context transfer syntax mismatch",
		    file_path, transfer_syntax.value(),
		    encoder_context_transfer_syntax.value());
	}
}

void update_transfer_syntax_uid_element_after_set_pixel_data_or_throw(
    DicomFile& file, uid::WellKnown transfer_syntax) {
	DataElement* transfer_syntax_element =
	    file.add_dataelement("(0002,0010)"_tag, VR::UI);
	if (!transfer_syntax_element ||
	    !transfer_syntax_element->from_transfer_syntax_uid(transfer_syntax)) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} reason=failed to update (0002,0010) TransferSyntaxUID",
		    file.path());
	}
}

}  // namespace

}  // namespace pixel::detail

void pixel::EncoderContext::set_configured_state(uid::WellKnown transfer_syntax,
    std::string plugin_key, std::vector<std::string> option_keys,
    std::vector<pixel::CodecOptionKv> codec_options) {
	transfer_syntax_uid_ = transfer_syntax;
	plugin_key_ = std::move(plugin_key);
	option_keys_ = std::move(option_keys);
	codec_options_ = std::move(codec_options);
	configured_ = true;
}

void pixel::EncoderContext::configure(uid::WellKnown transfer_syntax) {
	pixel::detail::validate_transfer_syntax_for_encode_or_throw(
	    "pixel::EncoderContext::configure", transfer_syntax);
	auto codec_options = pixel::detail::default_codec_options_for_transfer_syntax_or_throw(
	    "pixel::EncoderContext::configure", transfer_syntax);
	set_configured_state(transfer_syntax, std::string(pixel::detail::kRuntimePluginKey),
	    {}, std::move(codec_options));
}

void pixel::EncoderContext::configure(uid::WellKnown transfer_syntax,
    std::span<const pixel::CodecOptionTextKv> codec_opt) {
	pixel::detail::validate_transfer_syntax_for_encode_or_throw(
	    "pixel::EncoderContext::configure", transfer_syntax);
	std::vector<std::string> option_keys{};
	auto codec_options = pixel::detail::build_codec_option_pairs_from_text_or_throw(
	    "pixel::EncoderContext::configure", "<context>", transfer_syntax, codec_opt,
	    &option_keys);
	set_configured_state(transfer_syntax, std::string(pixel::detail::kRuntimePluginKey),
	    std::move(option_keys), std::move(codec_options));
}

pixel::EncoderContext pixel::create_encoder_context(
    uid::WellKnown transfer_syntax) {
	pixel::EncoderContext context{};
	context.configure(transfer_syntax);
	return context;
}

pixel::EncoderContext pixel::create_encoder_context(uid::WellKnown transfer_syntax,
    std::span<const pixel::CodecOptionTextKv> codec_opt) {
	pixel::EncoderContext context{};
	context.configure(transfer_syntax, codec_opt);
	return context;
}

void DicomFile::finalize_set_pixel_data_transfer_syntax(
    uid::WellKnown transfer_syntax) {
	set_transfer_syntax_state_only(transfer_syntax);
	pixel::detail::update_transfer_syntax_uid_element_after_set_pixel_data_or_throw(
	    *this, transfer_syntax);
}

void DicomFile::set_pixel_data(uid::WellKnown transfer_syntax,
    const pixel::PixelSource& source) {
	pixel::detail::validate_transfer_syntax_for_encode_or_throw(
	    "DicomFile::set_pixel_data", transfer_syntax);
	const auto codec_options =
	    pixel::detail::default_codec_options_for_transfer_syntax_or_throw(
	        "DicomFile::set_pixel_data", transfer_syntax);
	pixel::detail::run_set_pixel_data_with_resolved_codec_options(
	    *this, transfer_syntax, source, codec_options);
	finalize_set_pixel_data_transfer_syntax(transfer_syntax);
}

void DicomFile::set_pixel_data(uid::WellKnown transfer_syntax,
    const pixel::PixelSource& source, const pixel::EncoderContext& encoder_ctx) {
	pixel::detail::validate_encoder_context_for_set_pixel_data_or_throw(path(),
	    transfer_syntax, encoder_ctx.configured_, encoder_ctx.transfer_syntax_uid_);
	pixel::detail::run_set_pixel_data_with_resolved_codec_options(
	    *this, transfer_syntax, source, encoder_ctx.codec_options_);
	finalize_set_pixel_data_transfer_syntax(transfer_syntax);
}

void DicomFile::set_pixel_data(uid::WellKnown transfer_syntax,
    const pixel::PixelSource& source,
    std::span<const pixel::CodecOptionTextKv> codec_opt) {
	pixel::detail::validate_transfer_syntax_for_encode_or_throw(
	    "DicomFile::set_pixel_data", transfer_syntax);
	auto codec_options = pixel::detail::build_codec_option_pairs_from_text_or_throw(
	    "DicomFile::set_pixel_data", path(), transfer_syntax, codec_opt);
	pixel::detail::run_set_pixel_data_with_resolved_codec_options(
	    *this, transfer_syntax, source, codec_options);
	finalize_set_pixel_data_transfer_syntax(transfer_syntax);
}

} // namespace dicom
