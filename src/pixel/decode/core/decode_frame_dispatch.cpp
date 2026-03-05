#include "pixel/decode/core/decode_frame_dispatch.hpp"

#include "pixel/decode/core/decode_codec_impl_detail.hpp"
#include "pixel/registry/codec_registry.hpp"

#if defined(DICOMSDL_PIXEL_RUNTIME_ENABLED)
#include "pixel/runtime/host_adapter_v2.hpp"
#include "pixel/runtime/runtime_registry_v2.hpp"
#endif

#include <array>
#include <cctype>
#include <cstdio>
#include <exception>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <vector>

namespace dicom::pixel::detail {

#if defined(DICOMSDL_PIXEL_RUNTIME_ENABLED)
namespace {

struct ResolvedDecodeFrameSource {
	std::span<const std::uint8_t> bytes{};
	std::vector<std::uint8_t> owned_bytes{};
};

constexpr std::string_view kRuntimePluginKey = "runtime";
constexpr std::string_view kDirectPluginKey = "direct";
constexpr int kNoThreadOption = std::numeric_limits<int>::min();

struct DecoderContextCache {
	::pixel::runtime_v2::HostDecoderContextV2 ctx{};
	const ::pixel::runtime_v2::PluginRegistryV2* registry{nullptr};
	std::uint32_t transfer_syntax_index{0};
	int thread_option{kNoThreadOption};
	bool configured{false};

	~DecoderContextCache() {
		::pixel::runtime_v2::destroy_host_decoder_context_v2(&ctx);
	}
};

[[nodiscard]] DecoderContextCache& runtime_decoder_context_cache() {
	thread_local DecoderContextCache cache{};
	return cache;
}

[[nodiscard]] const ::pixel::runtime_v2::PluginRegistryV2* get_runtime_registry() {
	return ::pixel::runtime_v2::current_registry();
}

[[nodiscard]] CodecStatusCode map_runtime_error_code(pixel_error_code_v2 ec) noexcept {
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

void parse_runtime_detail_or_default(
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
		out_reason = "decoder runtime host adapter failed";
	}
}

[[nodiscard]] std::string decorate_runtime_detail_with_callsite_context(
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
		detail = "decoder runtime host adapter failed";
	}
	if (detail.rfind("file=", 0) == 0) {
		return detail;
	}
	return "file=" + std::string(file_path) + " frame=" +
	    std::to_string(frame_index) + " " + detail;
}

[[noreturn]] void throw_runtime_decode_error(
    std::string_view file_path, uid::WellKnown transfer_syntax,
    std::string_view plugin_key, std::size_t frame_index, pixel_error_code_v2 ec,
    std::string_view raw_detail) {
	CodecError decode_error{};
	decode_error.code = map_runtime_error_code(ec);
	parse_runtime_detail_or_default(raw_detail, decode_error.stage, decode_error.detail);
	decode_error.detail = decorate_runtime_detail_with_callsite_context(
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

[[nodiscard]] ResolvedDecodeFrameSource resolve_decode_source_for_runtime_or_throw(
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

[[nodiscard]] bool is_transfer_syntax_direct_decode_candidate(
    uid::WellKnown transfer_syntax) noexcept {
	return transfer_syntax.is_uncompressed() ||
	    transfer_syntax.is_jpeg_family() || transfer_syntax.is_jpegls() ||
	    transfer_syntax.is_jpeg2000() || transfer_syntax.is_htj2k() ||
	    transfer_syntax.is_jpegxl();
}

[[nodiscard]] bool try_dispatch_decode_frame_with_direct(const DicomFile& df,
    const DecodeValueTransform& value_transform, std::size_t frame_index,
    std::span<std::uint8_t> dst, const DecodeStrides& dst_strides,
    const DecodeOptions& effective_opt) {
	const auto& info = df.pixeldata_info();
	if (!is_transfer_syntax_direct_decode_candidate(info.ts)) {
		return false;
	}

	const auto resolved_source = resolve_decode_source_for_runtime_or_throw(
	    df, info.ts, frame_index, kDirectPluginKey);

	CodecError decode_error{};
	bool decoded = false;
	if (info.ts.is_uncompressed()) {
		if (info.ts.is_encapsulated()) {
			decoded = decode_encapsulated_uncompressed_into(info, value_transform, dst,
			    dst_strides, effective_opt, decode_error, resolved_source.bytes);
		} else {
			decoded = decode_raw_into(info, value_transform, dst, dst_strides,
			    effective_opt, decode_error, resolved_source.bytes);
		}
	} else if (info.ts.is_htj2k()) {
		decoded = decode_htj2k_into(info, value_transform, dst, dst_strides,
		    effective_opt, decode_error, resolved_source.bytes);
	} else if (info.ts.is_jpeg2000()) {
		decoded = decode_jpeg2k_into(info, value_transform, dst, dst_strides,
		    effective_opt, decode_error, resolved_source.bytes);
	} else if (info.ts.is_jpegls()) {
		decoded = decode_jpegls_into(info, value_transform, dst, dst_strides,
		    effective_opt, decode_error, resolved_source.bytes);
	} else if (info.ts.is_jpegxl()) {
		decoded = decode_jpegxl_into(info, value_transform, dst, dst_strides,
		    effective_opt, decode_error, resolved_source.bytes);
	} else if (info.ts.is_jpeg_family()) {
		decoded = decode_jpeg_into(info, value_transform, dst, dst_strides,
		    effective_opt, decode_error, resolved_source.bytes);
	}

	if (decoded) {
		return true;
	}

	// Fallback to runtime path only when direct path reports unsupported.
	if (decode_error.code == CodecStatusCode::unsupported) {
		return false;
	}

	if (decode_error.code == CodecStatusCode::ok) {
		decode_error.code = CodecStatusCode::backend_error;
		decode_error.stage = "decode_frame";
		decode_error.detail = "direct decode path failed";
	}
	throw_codec_error_with_context("pixel::decode_frame_into", df.path(), info.ts,
	    kDirectPluginKey, frame_index, decode_error);
}

[[nodiscard]] bool try_dispatch_decode_frame_with_runtime(const DicomFile& df,
    const DecodeValueTransform& value_transform, std::size_t frame_index,
    std::span<std::uint8_t> dst, const DecodeStrides& dst_strides,
    const DecodeOptions& effective_opt) {
	const auto& info = df.pixeldata_info();
	const auto* registry = get_runtime_registry();
	if (registry == nullptr) {
		return false;
	}

	auto& cache = runtime_decoder_context_cache();
	auto* const ctx = &cache.ctx;

	pixel_option_kv_v2 option_item{};
	pixel_option_list_v2 option_list{};
	char threads_text[32]{};
	int thread_option = kNoThreadOption;
	if (info.ts.is_jpeg2000() || info.ts.is_jpegxl()) {
		thread_option = effective_opt.decoder_threads;
		std::snprintf(threads_text, sizeof(threads_text), "%d",
		    effective_opt.decoder_threads);
		option_item.key = "threads";
		option_item.value = threads_text;
		option_list.items = &option_item;
		option_list.count = 1u;
	}
	const pixel_option_list_v2* option_ptr =
	    option_list.count == 0 ? nullptr : &option_list;

	const std::uint32_t transfer_syntax_index = info.ts.raw_index();
	bool needs_configure = true;
	if (cache.configured && cache.registry == registry &&
	    cache.transfer_syntax_index == transfer_syntax_index &&
	    cache.thread_option == thread_option) {
		needs_configure = false;
	}

	pixel_error_code_v2 configure_ec = PIXEL_CODEC_ERR_OK;
	std::string configure_detail{};
	if (needs_configure) {
		configure_ec = ::pixel::runtime_v2::configure_host_decoder_context_v2(
		    ctx, registry, info.ts, option_ptr);
		configure_detail = copy_decoder_error_detail(*ctx);
	}
	const std::string_view plugin_key = kRuntimePluginKey;
	if (configure_ec == PIXEL_CODEC_ERR_UNSUPPORTED) {
		cache.configured = false;
		throw_codec_error_with_context("pixel::decode_frame_into", df.path(), info.ts,
		    plugin_key, frame_index,
		    CodecError{
		        .code = CodecStatusCode::unsupported,
		        .stage = "plugin_lookup",
		        .detail = "plugin is not registered in runtime registry",
		    });
	}
	if (configure_ec != PIXEL_CODEC_ERR_OK) {
		cache.configured = false;
		throw_runtime_decode_error(df.path(), info.ts, plugin_key, frame_index,
		    configure_ec, configure_detail);
	}
	if (needs_configure) {
		cache.configured = true;
		cache.registry = registry;
		cache.transfer_syntax_index = transfer_syntax_index;
		cache.thread_option = thread_option;
	}

	const auto resolved_source = resolve_decode_source_for_runtime_or_throw(
	    df, info.ts, frame_index, plugin_key);
	const auto host_transform = resolve_host_value_transform(value_transform);
	const ::pixel::runtime_v2::HostValueTransformSpecV2* transform_ptr =
	    host_transform.kind == ::pixel::runtime_v2::HostValueTransformKindV2::kNone
	    ? nullptr
	    : &host_transform;
	const pixel_error_code_v2 decode_ec =
	    ::pixel::runtime_v2::decode_frame_with_host_context_v2(
	        ctx, &info, resolved_source.bytes, dst, &dst_strides, &effective_opt,
	        transform_ptr);
	if (decode_ec == PIXEL_CODEC_ERR_UNSUPPORTED) {
		throw_codec_error_with_context("pixel::decode_frame_into", df.path(), info.ts,
		    plugin_key, frame_index,
		    CodecError{
		        .code = CodecStatusCode::unsupported,
		        .stage = "decode_frame",
		        .detail = "runtime decoder does not support this request",
		    });
	}
	if (decode_ec != PIXEL_CODEC_ERR_OK) {
		throw_runtime_decode_error(df.path(), info.ts, plugin_key, frame_index, decode_ec,
		    copy_decoder_error_detail(*ctx));
	}
	return true;
}

}  // namespace
#endif

void dispatch_decode_frame_with_resolved_transform(const DicomFile& df,
    const DecodeValueTransform& value_transform, std::size_t frame_index,
    std::span<std::uint8_t> dst, const DecodeStrides& dst_strides,
    const DecodeOptions& effective_opt) {
#if defined(DICOMSDL_PIXEL_RUNTIME_ENABLED)
	if (try_dispatch_decode_frame_with_direct(
	        df, value_transform, frame_index, dst, dst_strides, effective_opt)) {
		return;
	}
	if (try_dispatch_decode_frame_with_runtime(
	        df, value_transform, frame_index, dst, dst_strides, effective_opt)) {
		return;
	}
#endif
	CodecError decode_error{};
	decode_error.code = CodecStatusCode::unsupported;
	decode_error.stage = "plugin_lookup";
	decode_error.detail = "runtime registry is not available";
	std::string_view plugin_key = "runtime";
#if defined(DICOMSDL_PIXEL_RUNTIME_ENABLED)
	plugin_key = kRuntimePluginKey;
#endif
	throw_codec_error_with_context("pixel::decode_frame_into", df.path(),
	    df.pixeldata_info().ts, plugin_key, frame_index, decode_error);
}

} // namespace dicom::pixel::detail
