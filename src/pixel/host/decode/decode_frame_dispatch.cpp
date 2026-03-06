#include "pixel/host/decode/decode_frame_dispatch.hpp"

#include "pixel/host/error/codec_error.hpp"
#include "pixel/host/support/dicom_pixel_support.hpp"

#if defined(DICOMSDL_PIXEL_RUNTIME_ENABLED)
#include "pixel/host/adapter/host_adapter_v2.hpp"
#include "pixel/runtime/runtime_registry_v2.hpp"
#endif

#include <array>
#include <cctype>
#include <cstdio>
#include <exception>
#include <new>
#include <string>
#include <string_view>
#include <vector>

namespace dicom::pixel::detail {

namespace {

void validate_decode_plan_or_throw(
    const DicomFile& df, std::size_t frame_index, const DecodePlan& plan) {
	if (!plan.info.ts.valid() || plan.strides.frame == 0) {
		const auto transfer_syntax =
		    plan.info.ts.valid() ? plan.info.ts : df.transfer_syntax_uid();
		throw_codec_error_with_context("pixel::decode_frame_into", df.path(),
		    transfer_syntax, frame_index,
		    CodecError{
		        .code = CodecStatusCode::invalid_argument,
		        .stage = "validate_plan",
		        .detail = "decode plan is not initialized; call create_decode_plan()",
		    });
	}
}

} // namespace

#if defined(DICOMSDL_PIXEL_RUNTIME_ENABLED)
namespace {

// Own one host decoder context and always release it on scope exit.
struct DecoderContextGuard {
	::pixel::runtime_v2::HostDecoderContextV2 value{};

	DecoderContextGuard() = default;

	~DecoderContextGuard() {
		::pixel::runtime_v2::destroy_host_decoder_context_v2(&value);
	}

	DecoderContextGuard(const DecoderContextGuard&) = delete;
	DecoderContextGuard& operator=(const DecoderContextGuard&) = delete;
};

// Return the current process-wide runtime registry, if one is installed.
[[nodiscard]] const ::pixel::runtime_v2::PluginRegistryV2* get_runtime_registry() {
	// Runtime dispatch is optional, so callers must handle a null registry.
	return ::pixel::runtime_v2::current_registry();
}

// Collapse runtime-specific error codes into the host-facing codec status categories.
[[nodiscard]] CodecStatusCode map_runtime_error_code(pixel_error_code_v2 ec) noexcept {
	// Keep host-side error reporting stable even if runtime codes grow over time.
	switch (ec) {
	case PIXEL_CODEC_ERR_OK:
		return CodecStatusCode::ok;
	case PIXEL_CODEC_ERR_INVALID_ARGUMENT:
		return CodecStatusCode::invalid_argument;
	case PIXEL_CODEC_ERR_UNSUPPORTED:
		return CodecStatusCode::unsupported;
	case PIXEL_CODEC_ERR_OUTPUT_TOO_SMALL:
		return CodecStatusCode::invalid_argument;
	case PIXEL_CODEC_ERR_FAILED:
	default:
		return CodecStatusCode::backend_error;
	}
}

// Parse structured runtime error detail and provide stable defaults when fields are absent.
void parse_runtime_detail_or_default(
    std::string_view raw_detail, std::string& out_stage, std::string& out_reason) {
	constexpr std::string_view kStagePrefix = "stage=";
	constexpr std::string_view kReasonMarker = ";reason=";

	// Prefer the explicit "stage=...;reason=..." payload format when available.
	if (raw_detail.rfind(kStagePrefix, 0) == 0) {
		const std::size_t reason_pos = raw_detail.find(kReasonMarker);
		if (reason_pos != std::string_view::npos) {
			const auto stage_view =
			    raw_detail.substr(kStagePrefix.size(), reason_pos - kStagePrefix.size());
			const auto reason_view =
			    raw_detail.substr(reason_pos + kReasonMarker.size());
			out_stage.assign(stage_view);
			out_reason.assign(reason_view);
		}
	}

	// Fall back to generic host-side labels when the runtime detail is incomplete.
	if (out_stage.empty()) {
		out_stage = "decode_frame";
	}
	if (out_reason.empty()) {
		out_reason.assign(raw_detail);
	}
	if (out_reason.empty()) {
		out_reason = "decoder runtime host adapter failed";
	}
}

// Normalize runtime detail so callers always see file/frame context at the host API boundary.
[[nodiscard]] std::string decorate_runtime_detail_with_callsite_context(
    std::string detail, std::string_view file_path, std::size_t frame_index) {
	// Strip any duplicated host callsite prefix emitted by lower layers.
	if (detail.rfind("pixel::decode_frame_into ", 0) == 0) {
		const std::size_t reason_pos = detail.find("reason=");
		if (reason_pos != std::string::npos) {
			detail = detail.substr(reason_pos + 7);
		}
	}

	// Normalize leading whitespace and guarantee a readable fallback message.
	while (!detail.empty() &&
	       std::isspace(static_cast<unsigned char>(detail.front())) != 0) {
		detail.erase(detail.begin());
	}
	if (detail.empty()) {
		detail = "decoder runtime host adapter failed";
	}

	// Preserve already-decorated detail, otherwise prepend the host callsite context.
	if (detail.rfind("file=", 0) == 0) {
		return detail;
	}
	return "file=" + std::string(file_path) + " frame=" +
	    std::to_string(frame_index) + " " + detail;
}

// Translate one runtime decoder failure into the host codec_error exception shape.
[[noreturn]] void throw_runtime_decode_error(
    std::string_view file_path, uid::WellKnown transfer_syntax,
    std::size_t frame_index, pixel_error_code_v2 ec,
    std::string_view raw_detail) {
	CodecError decode_error{};

	// Convert runtime status/detail into the host error schema before throwing.
	decode_error.code = map_runtime_error_code(ec);
	parse_runtime_detail_or_default(raw_detail, decode_error.stage, decode_error.detail);
	decode_error.detail = decorate_runtime_detail_with_callsite_context(
	    std::move(decode_error.detail), file_path, frame_index);
	throw_codec_error_with_context("pixel::decode_frame_into", file_path,
	    transfer_syntax, frame_index, decode_error);
}

// Copy the current runtime decoder detail string into an owning std::string.
[[nodiscard]] std::string copy_decoder_error_detail(
    const ::pixel::runtime_v2::HostDecoderContextV2& ctx) {
	std::array<char, 1024> buffer{};

	// Runtime adapters expose detail through a copy API rather than shared ownership.
	const auto copied = ::pixel::runtime_v2::copy_host_decoder_last_error_detail_v2(
	    &ctx, buffer.data(), static_cast<uint32_t>(buffer.size()));
	if (copied == 0) {
		return {};
	}
	return std::string(buffer.data(), buffer.data() + copied);
}

// Prepare one frame source for runtime decode and convert loader failures into codec errors.
[[nodiscard]] support_detail::PreparedDecodeFrameSource prepare_runtime_frame_source_or_throw(
    const DicomFile& df, const PixelDataInfo& info,
    uid::WellKnown transfer_syntax, std::size_t frame_index) {
	try {
		// Delegate actual source materialization to the pixel-data support layer.
		return support_detail::prepare_decode_frame_source_or_throw(df, info, frame_index);
	} catch (const std::bad_alloc&) {
		// Report allocation failures with a stable host-side stage label.
		throw_codec_error_with_context("pixel::decode_frame_into", df.path(),
		    transfer_syntax, frame_index,
		    CodecError{
		        .code = CodecStatusCode::internal_error,
		        .stage = "allocate",
		        .detail = "memory allocation failed",
		    });
	} catch (const std::exception& e) {
		// Surface ordinary loader/validation failures as invalid arguments.
		throw_codec_error_with_context("pixel::decode_frame_into", df.path(),
		    transfer_syntax, frame_index,
		    CodecError{
		        .code = CodecStatusCode::invalid_argument,
		        .stage = "load_frame_source",
		        .detail = e.what(),
		    });
	} catch (...) {
		// Preserve the host error shape even for non-standard exceptions.
		throw_codec_error_with_context("pixel::decode_frame_into", df.path(),
		    transfer_syntax, frame_index,
		    CodecError{
		        .code = CodecStatusCode::backend_error,
		        .stage = "load_frame_source",
		        .detail = "non-standard exception",
		    });
	}
}

// Convert the public plan transform into the runtime host ABI transform payload.
[[nodiscard]] ::pixel::runtime_v2::HostModalityValueTransformV2 prepare_host_value_transform(
    const pixel::ModalityValueTransform& modality_value_transform) {
	::pixel::runtime_v2::HostModalityValueTransformV2 host_transform{};

	// Disabled transforms become a zero-initialized host payload.
	if (!modality_value_transform.enabled) {
		return host_transform;
	}

	// LUT mode points directly at the prepared LUT data owned by the decode plan.
	if (modality_value_transform.modality_lut.has_value()) {
		host_transform.kind = ::pixel::runtime_v2::HostModalityValueTransformKindV2::kModalityLut;
		host_transform.modality_lut = &(*modality_value_transform.modality_lut);
		return host_transform;
	}

	// Otherwise emit the simple rescale form expected by the runtime ABI.
	host_transform.kind = ::pixel::runtime_v2::HostModalityValueTransformKindV2::kRescale;
	host_transform.rescale_slope = modality_value_transform.rescale_slope;
	host_transform.rescale_intercept = modality_value_transform.rescale_intercept;
	return host_transform;
}

// Try the runtime-backed decode path and return false only when no runtime registry is present.
[[nodiscard]] bool try_decode_frame_with_runtime(const DicomFile& df,
    std::size_t frame_index, std::span<std::uint8_t> dst, const DecodePlan& plan) {
	const auto& info = plan.info;
	const auto* registry = get_runtime_registry();

	// Runtime dispatch is optional; callers fall back when no registry is installed.
	if (registry == nullptr) {
		return false;
	}

	// Use a fresh host decoder context for this call.
	DecoderContextGuard decoder_ctx{};
	auto* const ctx = &decoder_ctx.value;

	// Only codecs with an exposed thread knob need runtime configure options.
	pixel_option_kv_v2 option_item{};
	pixel_option_list_v2 option_list{};
	char threads_text[32]{};
	if (info.ts.is_jpeg2000() || info.ts.is_jpegxl()) {
		std::snprintf(threads_text, sizeof(threads_text), "%d",
		    plan.options.decoder_threads);
		option_item.key = "threads";
		option_item.value = threads_text;
		option_list.items = &option_item;
		option_list.count = 1u;
	}
	const pixel_option_list_v2* option_ptr =
	    option_list.count == 0 ? nullptr : &option_list;

	// Configure the runtime decoder binding for this transfer syntax and option set.
	const pixel_error_code_v2 configure_ec =
	    ::pixel::runtime_v2::configure_host_decoder_context_v2(
	        ctx, registry, info.ts, option_ptr);

	// Translate configure-time binding failures into the public host error model.
	if (configure_ec == PIXEL_CODEC_ERR_UNSUPPORTED) {
		throw_codec_error_with_context("pixel::decode_frame_into", df.path(), info.ts,
		    frame_index,
		    CodecError{
		        .code = CodecStatusCode::unsupported,
		        .stage = "plugin_lookup",
		        .detail = "decoder binding is not registered in runtime registry",
		    });
	}
	if (configure_ec != PIXEL_CODEC_ERR_OK) {
		throw_runtime_decode_error(df.path(), info.ts, frame_index, configure_ec,
	    copy_decoder_error_detail(*ctx));
	}

	// Load the frame source and convert the plan transform to the host ABI form.
	const auto computed_source =
	    prepare_runtime_frame_source_or_throw(df, info, info.ts, frame_index);
	const auto host_transform =
	    prepare_host_value_transform(plan.modality_value_transform);
	const ::pixel::runtime_v2::HostModalityValueTransformV2* transform_ptr =
	    host_transform.kind == ::pixel::runtime_v2::HostModalityValueTransformKindV2::kNone
	        ? nullptr
	        : &host_transform;

	// Decode using the plan's metadata, output layout, and effective options.
	const pixel_error_code_v2 decode_ec =
	    ::pixel::runtime_v2::decode_frame_with_host_context_v2(
	        ctx, &info, computed_source.bytes, dst, &plan.strides, &plan.options,
	        transform_ptr);

	// Normalize runtime decode failures before they cross the public API boundary.
	if (decode_ec == PIXEL_CODEC_ERR_UNSUPPORTED) {
		throw_codec_error_with_context("pixel::decode_frame_into", df.path(), info.ts,
		    frame_index,
		    CodecError{
		        .code = CodecStatusCode::unsupported,
		        .stage = "decode_frame",
		        .detail = "runtime decoder does not support this request",
		    });
	}
	if (decode_ec != PIXEL_CODEC_ERR_OK) {
		throw_runtime_decode_error(df.path(), info.ts, frame_index, decode_ec,
		    copy_decoder_error_detail(*ctx));
	}
	return true;
}

}  // namespace
#endif

// Dispatch one frame decode through the runtime path or fail with a stable unsupported error.
void dispatch_decode_frame(const DicomFile& df, std::size_t frame_index,
    std::span<std::uint8_t> dst, const DecodePlan& plan) {
	validate_decode_plan_or_throw(df, frame_index, plan);
#if defined(DICOMSDL_PIXEL_RUNTIME_ENABLED)
	// Prefer the runtime-backed path when that subsystem is compiled in and initialized.
	if (try_decode_frame_with_runtime(df, frame_index, dst, plan)) {
		return;
	}
#endif

	// Without a runtime registry there is currently no alternate backend in this layer.
	CodecError decode_error{};
	decode_error.code = CodecStatusCode::unsupported;
	decode_error.stage = "plugin_lookup";
	decode_error.detail = "runtime registry is not available";
	throw_codec_error_with_context("pixel::decode_frame_into", df.path(),
	    plan.info.ts, frame_index, decode_error);
}

} // namespace dicom::pixel::detail
