#include "pixel/decode/core/decode_frame_dispatch.hpp"

#include "pixel/decode/core/decode_frame_source_resolver.hpp"
#include "pixel/decode/core/decode_plugin_dispatch.hpp"
#include "pixel/plugin_abi/external/plugin_external_bridge.hpp"

#if defined(DICOMSDL_PIXEL_V2_RUNTIME_ENABLED)
#include "pixel_/runtime/host_adapter_v2.hpp"
#include "pixel_/runtime/registry_bootstrap_v2.hpp"
#endif

#include <array>
#include <cctype>
#include <cstdio>
#include <exception>
#include <new>
#include <string>
#include <string_view>

namespace dicom::pixel::detail {

#if defined(DICOMSDL_PIXEL_V2_RUNTIME_ENABLED)
namespace {

constexpr std::string_view kV2RuntimePluginKey = "v2-runtime";

[[nodiscard]] std::string_view legacy_plugin_key_for_transfer_syntax(
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

class DecoderContextGuard {
public:
	explicit DecoderContextGuard(
	    ::pixel::runtime_v2::HostDecoderContextV2* ctx) noexcept
	    : ctx_(ctx) {}
	DecoderContextGuard(const DecoderContextGuard&) = delete;
	DecoderContextGuard& operator=(const DecoderContextGuard&) = delete;
	~DecoderContextGuard() {
		::pixel::runtime_v2::destroy_host_decoder_context_v2(ctx_);
	}

private:
	::pixel::runtime_v2::HostDecoderContextV2* ctx_{nullptr};
};

[[nodiscard]] const ::pixel::runtime_v2::PluginRegistryV2* get_runtime_registry_v2() {
	static ::pixel::runtime_v2::PluginRegistryRuntimeV2 runtime_state{};
	static const bool kInitialized =
	    ::pixel::runtime_v2::initialize_registry_v2({}, &runtime_state, nullptr);
	if (!kInitialized) {
		return nullptr;
	}
	return &runtime_state.registry;
}

[[nodiscard]] CodecStatusCode map_error_code_v2(pixel_error_code_v2 ec) noexcept {
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

void parse_v2_detail_or_default(
    std::string_view raw_detail, std::string& out_stage, std::string& out_reason) {
	constexpr std::string_view kStagePrefix = "stage=";
	constexpr std::string_view kReasonMarker = ";reason=";

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

	if (out_stage.empty()) {
		out_stage = "decode_frame";
	}
	if (out_reason.empty()) {
		out_reason.assign(raw_detail);
	}
	if (out_reason.empty()) {
		out_reason = "decoder v2 host adapter failed";
	}
}

[[nodiscard]] std::string decorate_v2_detail_with_callsite_context(
    std::string detail, std::string_view file_path, std::size_t frame_index) {
	if (detail.rfind("pixel::decode_frame_into ", 0) == 0) {
		const std::size_t reason_pos = detail.find("reason=");
		if (reason_pos != std::string::npos) {
			detail = detail.substr(reason_pos + 7);
		}
	}
	while (!detail.empty() &&
	       std::isspace(static_cast<unsigned char>(detail.front())) != 0) {
		detail.erase(detail.begin());
	}
	if (detail.empty()) {
		detail = "decoder v2 host adapter failed";
	}
	if (detail.rfind("file=", 0) == 0) {
		return detail;
	}
	return "file=" + std::string(file_path) + " frame=" +
	    std::to_string(frame_index) + " " + detail;
}

[[noreturn]] void throw_v2_decode_error(
    std::string_view file_path, uid::WellKnown transfer_syntax,
    std::string_view plugin_key, std::size_t frame_index, pixel_error_code_v2 ec,
    std::string_view raw_detail) {
	CodecError decode_error{};
	decode_error.code = map_error_code_v2(ec);
	parse_v2_detail_or_default(raw_detail, decode_error.stage, decode_error.detail);
	decode_error.detail = decorate_v2_detail_with_callsite_context(
	    std::move(decode_error.detail), file_path, frame_index);
	throw_codec_error_with_context("pixel::decode_frame_into", file_path, transfer_syntax,
	    plugin_key, frame_index, decode_error);
}

[[nodiscard]] std::string copy_decoder_error_detail(
    const ::pixel::runtime_v2::HostDecoderContextV2& ctx) {
	std::array<char, 1024> buffer{};
	const auto copied = ::pixel::runtime_v2::copy_host_decoder_last_error_detail_v2(
	    &ctx, buffer.data(), static_cast<uint32_t>(buffer.size()));
	if (copied == 0) {
		return {};
	}
	return std::string(buffer.data(), buffer.data() + copied);
}

[[nodiscard]] ResolvedDecodeFrameSource resolve_decode_source_for_v2_or_throw(
    const DicomFile& df, uid::WellKnown transfer_syntax, std::size_t frame_index,
    std::string_view plugin_key) {
	const auto& info = df.pixeldata_info();
	const auto& ds = df.dataset();

	ResolvedDecodeFrameSource resolved{};
	try {
		if (transfer_syntax.is_uncompressed() && !transfer_syntax.is_encapsulated()) {
			const auto source = load_native_frame_source(ds, df.path(), info, frame_index);
			resolved.bytes = source.contiguous;
			return resolved;
		}

		const auto source = load_encapsulated_frame_source(
		    ds, df.path(), frame_index, plugin_key);
		resolved.bytes = materialize_encapsulated_frame_source(
		    df.path(), frame_index, plugin_key, source, resolved.owned_bytes);
		return resolved;
	} catch (const std::bad_alloc&) {
		throw_codec_error_with_context("pixel::decode_frame_into", df.path(),
		    transfer_syntax, plugin_key, frame_index,
		    CodecError{
		        .code = CodecStatusCode::internal_error,
		        .stage = "allocate",
		        .detail = "memory allocation failed",
		    });
	} catch (const std::exception& e) {
		throw_codec_error_with_context("pixel::decode_frame_into", df.path(),
		    transfer_syntax, plugin_key, frame_index,
		    CodecError{
		        .code = CodecStatusCode::invalid_argument,
		        .stage = "load_frame_source",
		        .detail = e.what(),
		    });
	} catch (...) {
		throw_codec_error_with_context("pixel::decode_frame_into", df.path(),
		    transfer_syntax, plugin_key, frame_index,
		    CodecError{
		        .code = CodecStatusCode::backend_error,
		        .stage = "load_frame_source",
		        .detail = "non-standard exception",
		    });
	}
}

[[nodiscard]] ::pixel::runtime_v2::HostValueTransformSpecV2 resolve_host_value_transform(
    const DecodeValueTransform& value_transform) {
	::pixel::runtime_v2::HostValueTransformSpecV2 host_transform{};
	if (!value_transform.enabled) {
		return host_transform;
	}
	if (value_transform.modality_lut.has_value()) {
		host_transform.kind = ::pixel::runtime_v2::HostValueTransformKindV2::kModalityLut;
		host_transform.modality_lut = &(*value_transform.modality_lut);
		return host_transform;
	}
	host_transform.kind = ::pixel::runtime_v2::HostValueTransformKindV2::kRescale;
	host_transform.rescale_slope = value_transform.rescale_slope;
	host_transform.rescale_intercept = value_transform.rescale_intercept;
	return host_transform;
}

[[nodiscard]] bool try_dispatch_decode_frame_with_v2_runtime(const DicomFile& df,
    const DecodeValueTransform& value_transform, std::size_t frame_index,
    std::span<std::uint8_t> dst, const DecodeStrides& dst_strides,
    const DecodeOptions& effective_opt) {
	const auto& info = df.pixeldata_info();
	const auto* registry = get_runtime_registry_v2();
	const std::string_view legacy_plugin_key = legacy_plugin_key_for_transfer_syntax(info.ts);
	const std::string_view plugin_key_fallback =
	    legacy_plugin_key.empty() ? kV2RuntimePluginKey : legacy_plugin_key;
	if (registry == nullptr) {
		return false;
	}
	if (!legacy_plugin_key.empty() && has_external_decoder_bridge(legacy_plugin_key)) {
		// Keep legacy external bridge override semantics for decode tests and runtime parity.
		return false;
	}

	::pixel::runtime_v2::HostDecoderContextV2 ctx{};
	const DecoderContextGuard guard(&ctx);

	pixel_option_kv_v2 option_item{};
	pixel_option_list_v2 option_list{};
	char threads_text[32]{};
	if (info.ts.is_jpeg2000() || info.ts.is_jpegxl()) {
		std::snprintf(threads_text, sizeof(threads_text), "%d",
		    effective_opt.decoder_threads);
		option_item.key = "threads";
		option_item.value = threads_text;
		option_list.items = &option_item;
		option_list.count = 1u;
	}
	const pixel_option_list_v2* option_ptr =
	    option_list.count == 0 ? nullptr : &option_list;

	const pixel_error_code_v2 configure_ec =
	    ::pixel::runtime_v2::configure_host_decoder_context_v2(
	        &ctx, registry, info.ts, option_ptr);
	const std::string configure_detail = copy_decoder_error_detail(ctx);
	const std::string_view plugin_key = plugin_key_fallback;
	if (configure_ec == PIXEL_CODEC_ERR_UNSUPPORTED) {
		return false;
	}
	if (configure_ec != PIXEL_CODEC_ERR_OK) {
		throw_v2_decode_error(df.path(), info.ts, plugin_key, frame_index,
		    configure_ec, configure_detail);
	}

	const auto resolved_source = resolve_decode_source_for_v2_or_throw(
	    df, info.ts, frame_index, plugin_key);
	const auto host_transform = resolve_host_value_transform(value_transform);
	const ::pixel::runtime_v2::HostValueTransformSpecV2* transform_ptr =
	    host_transform.kind == ::pixel::runtime_v2::HostValueTransformKindV2::kNone
	    ? nullptr
	    : &host_transform;
	const pixel_error_code_v2 decode_ec =
	    ::pixel::runtime_v2::decode_frame_with_host_context_v2(
	        &ctx, &info, resolved_source.bytes, dst, &dst_strides, &effective_opt,
	        transform_ptr);
	if (decode_ec == PIXEL_CODEC_ERR_UNSUPPORTED) {
		return false;
	}
	if (decode_ec != PIXEL_CODEC_ERR_OK) {
		throw_v2_decode_error(df.path(), info.ts, plugin_key, frame_index, decode_ec,
		    copy_decoder_error_detail(ctx));
	}
	return true;
}

}  // namespace
#endif

namespace {

void dispatch_decode_frame_with_legacy_registry(const DicomFile& df,
    const DecodeValueTransform& value_transform, std::size_t frame_index,
    std::span<std::uint8_t> dst, const DecodeStrides& dst_strides,
    const DecodeOptions& effective_opt) {
	const auto& info = df.pixeldata_info();

	const auto& codec_registry = global_codec_registry();
	// Keep dispatch stable for the whole decode call while plugin dispatchers may be swapped.
	[[maybe_unused]] const auto dispatch_lock = codec_registry.acquire_dispatch_read_lock();
	const auto* binding = codec_registry.find_binding(info.ts);
	CodecError decode_error{};
	if (!binding || !binding->decode_supported) {
		decode_error.code = CodecStatusCode::unsupported;
		decode_error.stage = "plugin_lookup";
		decode_error.detail =
		    "transfer syntax is not supported for decode by codec registry binding";
		throw_codec_error_with_context("pixel::decode_frame_into",
		    df.path(), info.ts, "<none>", frame_index, decode_error);
	}
	const auto* plugin = codec_registry.resolve_decoder_plugin(*binding);
	if (!plugin) {
		decode_error.code = CodecStatusCode::internal_error;
		decode_error.stage = "plugin_lookup";
		decode_error.detail = "registry binding references a missing plugin";
		throw_codec_error_with_context("pixel::decode_frame_into",
		    df.path(), info.ts, binding->plugin_key, frame_index, decode_error);
	}
	if (!plugin->decode_frame) {
		decode_error.code = CodecStatusCode::internal_error;
		decode_error.stage = "plugin_lookup";
		decode_error.detail = "registered decode plugin has no dispatcher";
		throw_codec_error_with_context("pixel::decode_frame_into",
		    df.path(), info.ts, binding->plugin_key, frame_index, decode_error);
	}

	const auto resolved_source =
	    resolve_decode_frame_source_or_throw(df, *binding, frame_index);
	const CodecDecodeFrameInput decode_input{
	    .info = info,
	    .value_transform = value_transform,
	    .prepared_source = resolved_source.bytes,
	    .destination = dst,
	    .destination_strides = dst_strides,
	    .options = effective_opt,
	};

	invoke_decode_plugin_or_throw(
	    df, *binding, *plugin, decode_input, frame_index);
}

}  // namespace

void dispatch_decode_frame_with_resolved_transform(const DicomFile& df,
    const DecodeValueTransform& value_transform, std::size_t frame_index,
    std::span<std::uint8_t> dst, const DecodeStrides& dst_strides,
    const DecodeOptions& effective_opt) {
#if defined(DICOMSDL_PIXEL_V2_RUNTIME_ENABLED)
	if (try_dispatch_decode_frame_with_v2_runtime(
	        df, value_transform, frame_index, dst, dst_strides, effective_opt)) {
		return;
	}
#endif
	dispatch_decode_frame_with_legacy_registry(
	    df, value_transform, frame_index, dst, dst_strides, effective_opt);
}

} // namespace dicom::pixel::detail
