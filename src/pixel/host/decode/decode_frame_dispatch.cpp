#include "pixel/host/decode/decode_frame_dispatch.hpp"
#include "pixel/host/decode/decode_thread_policy.hpp"

#include "diagnostics.h"
#include "pixel/host/error/codec_error.hpp"
#include "pixel/host/support/dicom_pixel_support.hpp"

#if defined(DICOMSDL_PIXEL_RUNTIME_ENABLED)
#include "pixel/codecs/uncompressed/direct_api.hpp"
#include "pixel/host/adapter/host_adapter.hpp"
#include "pixel/host/support/abi_convert.hpp"
#include "pixel/runtime/runtime_registry.hpp"
#endif

#include <array>
#include <cctype>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <atomic>
#include <exception>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <vector>

namespace dicom::pixel::detail {
namespace diag = dicom::diag;

namespace {

#if defined(DICOMSDL_PIXEL_RUNTIME_ENABLED)
[[nodiscard]] const ::pixel::runtime::BindingRegistry* get_runtime_registry();
#endif

struct DecodeDispatchRequestView {
	uid::WellKnown transfer_syntax{};
	PixelLayout source_layout{};
	DecodeOptions options{};
	PixelLayout output_layout{};
};

template <typename... Args>
[[noreturn]] void throw_decode_stage_exception(std::optional<std::size_t> frame_index,
    CodecStatusCode code, std::string_view stage,
    fmt::format_string<Args...> reason_format, Args&&... args) {
	if (frame_index.has_value()) {
		throw_frame_codec_stage_exception(*frame_index, code, stage, reason_format,
		    std::forward<Args>(args)...);
	}
	throw_codec_stage_exception(code, stage, reason_format,
	    std::forward<Args>(args)...);
}
[[nodiscard]] DecodeDispatchRequestView build_decode_dispatch_request_view(
    const DicomFile& df, const DecodePlan& plan) {
	return DecodeDispatchRequestView{
	    .transfer_syntax = df.transfer_syntax_uid(),
	    .source_layout = support_detail::compute_decode_source_layout(df),
	    .options = plan.options,
	    .output_layout = plan.output_layout,
	};
}

[[nodiscard]] DecodeDispatchRequestView build_decode_dispatch_request_view(
    uid::WellKnown transfer_syntax, const PixelLayout& source_layout,
    const DecodePlan& plan) {
	return DecodeDispatchRequestView{
	    .transfer_syntax = transfer_syntax,
	    .source_layout = source_layout,
	    .options = plan.options,
	    .output_layout = plan.output_layout,
	};
}

void validate_decode_thread_settings_or_throw(
    const DecodeDispatchRequestView& request, std::optional<std::size_t> frame_index) {
	if (request.options.worker_threads < -1) {
		throw_decode_stage_exception(frame_index,
		    CodecStatusCode::invalid_argument, "validate_plan",
		    "worker_threads must be -1, 0, or positive");
	}
	if (request.options.codec_threads < -1) {
		throw_decode_stage_exception(frame_index,
		    CodecStatusCode::invalid_argument, "validate_plan",
		    "codec_threads must be -1, 0, or positive");
	}
}

void validate_decode_frame_request_or_throw(
    const DecodeDispatchRequestView& request, std::size_t frame_index) {
	// A valid frame decode request needs the current source metadata and a
	// concrete destination layout.
	if (!request.transfer_syntax.valid() || request.source_layout.empty() ||
	    request.output_layout.empty()) {
		throw_decode_stage_exception(frame_index,
		    CodecStatusCode::invalid_argument, "validate_plan",
		    "decode plan is not initialized; call create_decode_plan()");
	}
	validate_decode_thread_settings_or_throw(request, frame_index);
}

void validate_decode_all_frames_request_or_throw(
    const DecodeDispatchRequestView& request, std::span<std::uint8_t> dst) {
	// Batch decode shares the same layout and threading invariants as single-frame decode.
	if (!request.transfer_syntax.valid() || request.source_layout.empty() ||
	    request.output_layout.empty()) {
		throw_decode_stage_exception(std::nullopt,
		    CodecStatusCode::invalid_argument, "validate_plan",
		    "decode plan is not initialized; call create_decode_plan()");
	}
	validate_decode_thread_settings_or_throw(request, std::nullopt);
	if (request.output_layout.frames == 0) {
		throw_decode_stage_exception(std::nullopt,
		    CodecStatusCode::invalid_argument, "validate_plan",
		    "decode plan does not describe any frames");
	}

	const auto frames = static_cast<std::size_t>(request.output_layout.frames);
	if (request.output_layout.frame_stride >
	    (std::numeric_limits<std::size_t>::max() / frames)) {
		throw_decode_stage_exception(std::nullopt,
		    CodecStatusCode::invalid_argument, "validate_dst",
		    "decoded output size overflow for all frames");
	}

	const auto required_bytes = request.output_layout.frame_stride * frames;
	// The destination must cover the full contiguous multi-frame storage span.
	if (dst.size() < required_bytes) {
		throw_decode_stage_exception(std::nullopt,
		    CodecStatusCode::invalid_argument, "validate_dst",
		    "destination buffer is smaller than required decoded size");
	}
}

[[nodiscard]] bool should_cancel_execution(
    const ExecutionObserver* observer) noexcept {
	return observer != nullptr && observer->should_cancel != nullptr &&
	       observer->should_cancel(observer->user_data);
}

[[nodiscard]] std::size_t resolve_execution_notify_every(
    const ExecutionObserver* observer) noexcept {
	if (observer == nullptr) {
		return std::size_t{0};
	}
	return observer->notify_every <= std::size_t{1} ? std::size_t{1}
	                                                : observer->notify_every;
}

[[noreturn]] void throw_decode_all_frames_cancelled(
    std::size_t completed, std::size_t total) {
	throw_codec_stage_exception(CodecStatusCode::cancelled, "cancel",
	    "decode cancelled by observer after {} of {} frames",
	    completed, total);
}

[[nodiscard]] EffectiveExecutionThreadSettings resolve_decode_frame_thread_settings(
    const DecodeDispatchRequestView& request) noexcept {
	return ::dicom::pixel::detail::resolve_decode_frame_thread_settings(
	    request.transfer_syntax, request.options, resolve_hardware_thread_count());
}

[[nodiscard]] EffectiveExecutionThreadSettings resolve_decode_all_frames_thread_settings(
    const DecodeDispatchRequestView& request) noexcept {
	const auto frames = request.output_layout.frames > 0
	                        ? static_cast<std::size_t>(request.output_layout.frames)
	                        : std::size_t{1};
	return ::dicom::pixel::detail::resolve_decode_all_frames_thread_settings(
	    request.transfer_syntax, request.options, frames,
	    resolve_hardware_thread_count());
}

} // namespace

#if defined(DICOMSDL_PIXEL_RUNTIME_ENABLED)
namespace {

constexpr int kNoThreadOption = std::numeric_limits<int>::min();

// Reuse one decoder context per thread to avoid paying full configure/create
// cost on every frame decode when the registry/options are unchanged.
struct DecoderContextCache {
	::pixel::runtime::HostDecoderContext ctx{};
	const ::pixel::runtime::BindingRegistry* registry{nullptr};
	std::uint64_t registry_generation{0};
	std::uint32_t transfer_syntax_index{0};
	int codec_thread_option{kNoThreadOption};
	bool configured{false};

	DecoderContextCache() = default;

	~DecoderContextCache() {
		::pixel::runtime::destroy_host_decoder_context(&ctx);
	}

	DecoderContextCache(const DecoderContextCache&) = delete;
	DecoderContextCache& operator=(const DecoderContextCache&) = delete;
};

[[nodiscard]] DecoderContextCache& runtime_decoder_context_cache() {
	thread_local DecoderContextCache cache{};
	return cache;
}

// Return the current process-wide runtime registry, if one is installed.
[[nodiscard]] const ::pixel::runtime::BindingRegistry* get_runtime_registry() {
	// Runtime dispatch is optional, so callers must handle a null registry.
	return ::pixel::runtime::current_registry();
}

[[nodiscard]] std::uint64_t get_runtime_registry_generation() {
	return ::pixel::runtime::current_registry_generation();
}

struct ResolvedDecoderBinding {
	uint32_t codec_profile_code{PIXEL_CODEC_PROFILE_UNKNOWN};
	const ::pixel::runtime::DecoderBinding* binding{nullptr};
};

[[nodiscard]] bool try_resolve_runtime_decoder_binding(
    uid::WellKnown transfer_syntax, ResolvedDecoderBinding* out_binding) {
	if (out_binding == nullptr) {
		return false;
	}

	const auto* registry = get_runtime_registry();
	if (registry == nullptr) {
		return false;
	}

	uint32_t codec_profile_code = PIXEL_CODEC_PROFILE_UNKNOWN;
	if (!::pixel::runtime::codec_profile_code_from_transfer_syntax(
	        transfer_syntax, &codec_profile_code)) {
		return false;
	}

	const auto* binding = registry->find_decoder_binding(codec_profile_code);
	if (binding == nullptr ||
	    binding->binding_kind == ::pixel::runtime::DecoderBindingKind::kNone) {
		return false;
	}

	out_binding->codec_profile_code = codec_profile_code;
	out_binding->binding = binding;
	return true;
}

void build_direct_decode_request_for_core(pixel_decoder_request& request,
    uint32_t codec_profile_code, const DecodeDispatchRequestView& decode_request,
    std::span<const uint8_t> prepared_source, std::span<uint8_t> destination) {
	::pixel::runtime::DtypeMeta source_dtype{};
	if (!::pixel::runtime::resolve_dtype_meta(
	        decode_request.source_layout.data_type, &source_dtype)) {
		source_dtype.code = PIXEL_DTYPE_UNKNOWN;
	}
	::pixel::runtime::DtypeMeta dst_dtype{};
	if (!::pixel::runtime::resolve_dtype_meta(
	        decode_request.output_layout.data_type, &dst_dtype)) {
		dst_dtype.code = PIXEL_DTYPE_UNKNOWN;
	}

	// Reuse the shared ABI request builder so builtin core-direct decode stays in lockstep
	// with the plugin-backed request contract.
	::pixel::runtime::build_decoder_request(codec_profile_code, source_dtype.code,
	    decode_request.source_layout, prepared_source, destination,
	    dst_dtype.code == PIXEL_DTYPE_UNKNOWN ? source_dtype.code : dst_dtype.code,
	    decode_request.output_layout.planar,
	    static_cast<uint64_t>(decode_request.output_layout.row_stride),
	    static_cast<uint64_t>(decode_request.output_layout.frame_stride),
	    ::pixel::runtime::is_mct_capable_profile(codec_profile_code) &&
	        decode_request.options.decode_mct,
	    &request);
}

[[nodiscard]] std::string copy_core_error_detail(
    const ::pixel::core::ErrorState& state) {
	std::array<char, 1024> buffer{};
	const uint32_t copied = ::pixel::core::copy_last_error_detail(
	    &state, buffer.data(), static_cast<uint32_t>(buffer.size()));
	if (copied == 0) {
		return {};
	}
	return std::string(buffer.data(), buffer.data() + copied);
}

// Collapse runtime-specific error codes into the host-facing codec status categories.
[[nodiscard]] CodecStatusCode map_runtime_error_code(pixel_error_code ec) noexcept {
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

// Translate one runtime decoder failure into the host codec_error exception shape.
[[noreturn]] void throw_runtime_decode_error(
    std::size_t frame_index, pixel_error_code ec,
    std::string_view raw_detail) {
	CodecError decode_error{};

	// Convert runtime status/detail into the host error schema before throwing.
	decode_error.code = map_runtime_error_code(ec);
	parse_runtime_detail_or_default(raw_detail, decode_error.stage, decode_error.detail);
	throw_codec_exception(frame_index, decode_error);
}

// Copy the current runtime decoder detail string into an owning std::string.
[[nodiscard]] std::string copy_decoder_error_detail(
    const ::pixel::runtime::HostDecoderContext& ctx) {
	std::array<char, 1024> buffer{};

	// Runtime adapters expose detail through a copy API rather than shared ownership.
	const auto copied = ::pixel::runtime::copy_host_decoder_last_error_detail(
	    &ctx, buffer.data(), static_cast<uint32_t>(buffer.size()));
	if (copied == 0) {
		return {};
	}
	return std::string(buffer.data(), buffer.data() + copied);
}

// Prepare one frame source for runtime decode and convert loader failures into codec errors.
[[nodiscard]] support_detail::PreparedDecodeFrameSource prepare_runtime_frame_source_or_throw(
    const DicomFile& df, const DecodeDispatchRequestView& request,
    std::size_t frame_index) {
	try {
		// Delegate actual source materialization to the pixel-data support layer.
		return support_detail::prepare_decode_frame_source_or_throw(
		    df, request.source_layout, frame_index);
	} catch (const std::bad_alloc&) {
		// Report allocation failures with a stable host-side stage label.
		throw_frame_codec_stage_exception(frame_index,
		    CodecStatusCode::internal_error, "allocate",
		    "memory allocation failed");
	} catch (const std::exception& e) {
		// Surface ordinary loader/validation failures as invalid arguments.
		throw_frame_codec_stage_exception(frame_index,
		    CodecStatusCode::invalid_argument, "load_frame_source", "{}",
		    e.what());
	} catch (...) {
		// Preserve the host error shape even for non-standard exceptions.
		throw_frame_codec_stage_exception(frame_index,
		    CodecStatusCode::backend_error, "load_frame_source",
		    "non-standard exception");
	}
}

// Try the runtime-backed decode path and return false only when no runtime registry is present.
[[nodiscard]] bool try_decode_frame_with_runtime_source(
    const DecodeDispatchRequestView& request,
    std::span<const std::uint8_t> prepared_source, std::size_t frame_index,
    std::span<std::uint8_t> dst, int codec_threads_override) {
	const auto* registry = get_runtime_registry();
	const auto registry_generation = get_runtime_registry_generation();

	// Runtime dispatch is optional; callers fall back when no registry is installed.
	if (registry == nullptr) {
		return false;
	}

	auto& cache = runtime_decoder_context_cache();
	auto* const ctx = &cache.ctx;

	// Only codecs with an exposed thread knob need runtime configure options.
	pixel_option_kv option_item{};
	pixel_option_list option_list{};
	char threads_text[32]{};
	int codec_thread_option = kNoThreadOption;
	if (request.transfer_syntax.is_jpeg2000() || request.transfer_syntax.is_htj2k() ||
	    request.transfer_syntax.is_jpegxl()) {
		codec_thread_option = codec_threads_override;
		std::snprintf(
		    threads_text, sizeof(threads_text), "%d", codec_threads_override);
		option_item.key = "threads";
		option_item.value = threads_text;
		option_list.items = &option_item;
		option_list.count = 1u;
	}
	const pixel_option_list* option_ptr =
	    option_list.count == 0 ? nullptr : &option_list;

	const auto transfer_syntax_index = request.transfer_syntax.raw_index();
	bool needs_configure = true;
	if (cache.configured && cache.registry == registry &&
	    cache.registry_generation == registry_generation &&
	    cache.transfer_syntax_index == transfer_syntax_index &&
	    cache.codec_thread_option == codec_thread_option) {
		needs_configure = false;
	}

	pixel_error_code configure_ec = PIXEL_CODEC_ERR_OK;
	std::string configure_detail{};
	if (needs_configure) {
		configure_ec = ::pixel::runtime::configure_host_decoder_context(
		    ctx, registry, request.transfer_syntax, option_ptr);
		configure_detail = copy_decoder_error_detail(*ctx);
	}

	// Translate configure-time binding failures into the public host error model.
	if (configure_ec == PIXEL_CODEC_ERR_UNSUPPORTED) {
		cache.configured = false;
		throw_frame_codec_stage_exception(frame_index,
		    CodecStatusCode::unsupported, "plugin_lookup",
		    "decoder binding is not registered in runtime registry");
	}
	if (configure_ec != PIXEL_CODEC_ERR_OK) {
		cache.configured = false;
		throw_runtime_decode_error(frame_index, configure_ec, configure_detail);
	}
	if (needs_configure) {
		cache.configured = true;
		cache.registry = registry;
		cache.registry_generation = registry_generation;
		cache.transfer_syntax_index = transfer_syntax_index;
		cache.codec_thread_option = codec_thread_option;
	}

	// Decode using the plan's metadata, output layout, and effective options.
	const pixel_error_code decode_ec =
	    ::pixel::runtime::decode_frame_with_host_context(
	        ctx, &request.source_layout, prepared_source, dst, &request.output_layout,
	        &request.options);

	// Normalize runtime decode failures before they cross the public API boundary.
	if (decode_ec == PIXEL_CODEC_ERR_UNSUPPORTED) {
		throw_frame_codec_stage_exception(frame_index,
		    CodecStatusCode::unsupported, "decode_frame",
		    "runtime decoder does not support this request");
	}
	if (decode_ec != PIXEL_CODEC_ERR_OK) {
		throw_runtime_decode_error(frame_index, decode_ec,
		    copy_decoder_error_detail(*ctx));
	}
	return true;
}

[[nodiscard]] bool try_decode_frame_with_runtime(const DicomFile& df,
    const DecodeDispatchRequestView& request, std::size_t frame_index,
    std::span<std::uint8_t> dst, int codec_threads_override) {
	const auto computed_source = prepare_runtime_frame_source_or_throw(
	    df, request, frame_index);
	return try_decode_frame_with_runtime_source(
	    request, computed_source.bytes, frame_index, dst, codec_threads_override);
}

[[nodiscard]] bool try_copy_frame_with_builtin_native_fast_path(
    const DicomFile& df, const DecodeDispatchRequestView& request,
    std::size_t frame_index, std::span<std::uint8_t> dst) {
	// REF-style raw frames can skip decode entirely and copy from the native source
	// when the requested output layout already matches the stored layout.
	if (!request.transfer_syntax.is_uncompressed() ||
	    request.transfer_syntax.is_encapsulated()) {
		return false;
	}
	if (request.source_layout.rows == 0 || request.source_layout.cols == 0 ||
	    request.source_layout.samples_per_pixel == 0) {
		return false;
	}

	const auto rows = static_cast<std::size_t>(request.source_layout.rows);
	const auto samples =
	    static_cast<std::size_t>(request.source_layout.samples_per_pixel);
	support_detail::NativeDecodeSourceView native_source{};
	try {
		native_source =
		    support_detail::compute_native_decode_source_view_or_throw(
		        df, request.source_layout);
	} catch (...) {
		return false;
	}
	const bool source_planar =
	    native_source.planar_source && samples > std::size_t{1};
	const bool output_planar =
	    request.output_layout.planar == Planar::planar && samples > std::size_t{1};

	// Raw host-copy fast path only handles layout-equivalent requests.
	if (samples > 1 && source_planar != output_planar) {
		return false;
	}

	std::span<const std::uint8_t> src{};
	try {
		src = support_detail::native_decode_frame_bytes_or_throw(native_source, frame_index);
	} catch (...) {
		return false;
	}
	const std::size_t row_payload = native_source.row_payload_bytes;
	if (row_payload == 0 || request.output_layout.row_stride < row_payload ||
	    dst.size() < request.output_layout.frame_stride) {
		return false;
	}

	if (!source_planar || samples == 1) {
		if (request.output_layout.row_stride == row_payload) {
			const std::size_t copy_bytes = row_payload * rows;
			if (src.size() < copy_bytes) {
				return false;
			}
			std::memcpy(dst.data(), src.data(), copy_bytes);
			return true;
		}

		for (std::size_t r = 0; r < rows; ++r) {
			const auto src_offset = r * row_payload;
			if (src.size() < src_offset + row_payload) {
				return false;
			}
			std::memcpy(
			    dst.data() + r * request.output_layout.row_stride,
			    src.data() + src_offset,
			    row_payload);
		}
		return true;
	}

	const std::size_t plane_bytes = row_payload * rows;
	if (src.size() < plane_bytes * samples) {
		return false;
	}
	const std::size_t dst_plane_bytes = request.output_layout.row_stride * rows;
	for (std::size_t s = 0; s < samples; ++s) {
		const auto* src_plane = src.data() + s * plane_bytes;
		auto* dst_plane = dst.data() + s * dst_plane_bytes;
		for (std::size_t r = 0; r < rows; ++r) {
			std::memcpy(dst_plane + r * request.output_layout.row_stride,
			    src_plane + r * row_payload, row_payload);
		}
	}
	return true;
}

[[nodiscard]] bool try_decode_frame_with_builtin_uncompressed_core_source(
    const DecodeDispatchRequestView& decode_request,
    std::span<const std::uint8_t> prepared_source, std::size_t frame_index,
    std::span<std::uint8_t> dst) {
	// Fall back to the uncompressed core decoder when raw-copy is not enough,
	// e.g. layout conversion, value transform, or encapsulated-uncompressed input.
	if (!decode_request.transfer_syntax.is_uncompressed()) {
		return false;
	}

	ResolvedDecoderBinding binding{};
	// Backend selection stays separate from request construction so it is easy
	// to see why the builtin core path is, or is not, available.
	if (!try_resolve_runtime_decoder_binding(decode_request.transfer_syntax, &binding) ||
	    binding.binding->binding_kind !=
	        ::pixel::runtime::DecoderBindingKind::kCoreDirect) {
		return false;
	}

	pixel_decoder_request core_request{};
	// Build the core request only after backend selection says this path applies.
	build_direct_decode_request_for_core(
	    core_request, binding.codec_profile_code, decode_request, prepared_source, dst);
	if (core_request.frame.source_dtype == PIXEL_DTYPE_UNKNOWN) {
		return false;
	}

	::pixel::core::ErrorState error_state{};
	const pixel_error_code decode_ec =
	    ::pixel::core::decode_uncompressed_frame(&error_state, &core_request);
	if (decode_ec != PIXEL_CODEC_ERR_OK) {
		throw_runtime_decode_error(frame_index, decode_ec,
		    copy_core_error_detail(error_state));
	}
	return true;
}

[[nodiscard]] bool try_decode_frame_with_builtin_uncompressed_core_path(const DicomFile& df,
    const DecodeDispatchRequestView& request, std::size_t frame_index,
    std::span<std::uint8_t> dst) {
	const auto computed_source =
	    prepare_runtime_frame_source_or_throw(df, request, frame_index);
	return try_decode_frame_with_builtin_uncompressed_core_source(
	    request, computed_source.bytes, frame_index, dst);
}

[[nodiscard]] bool try_decode_frame_with_builtin_uncompressed_fast_paths(const DicomFile& df,
    const DecodeDispatchRequestView& request, std::size_t frame_index,
    std::span<std::uint8_t> dst) {
	if (!request.transfer_syntax.is_uncompressed()) {
		return false;
	}

	// Fast-path order matters:
	// 1. REF/native raw copy
	// 2. uncompressed core decode
	// Everything else falls through to the generic runtime path below.
	if (try_copy_frame_with_builtin_native_fast_path(
		        df, request, frame_index, dst)) {
		return true;
	}
	if (try_decode_frame_with_builtin_uncompressed_core_path(
	        df, request, frame_index, dst)) {
		return true;
	}
	return false;
}

}  // namespace
#endif

void dispatch_decode_frame_with_codec_threads(const DicomFile& df,
    const DecodeDispatchRequestView& request, std::size_t frame_index,
    std::span<std::uint8_t> dst, int codec_threads) {
	validate_decode_frame_request_or_throw(request, frame_index);
#if defined(DICOMSDL_PIXEL_RUNTIME_ENABLED)
	// Try the narrow builtin hot paths first before paying the full runtime
	// adapter/setup cost for general codec dispatch.
	if (try_decode_frame_with_builtin_uncompressed_fast_paths(
	        df, request, frame_index, dst)) {
		return;
	}
	// Prefer the runtime-backed path when that subsystem is compiled in and initialized.
	if (try_decode_frame_with_runtime(df, request, frame_index, dst, codec_threads)) {
		return;
	}
#endif

	// Without a runtime registry there is currently no alternate backend in this layer.
	throw_frame_codec_stage_exception(frame_index,
	    CodecStatusCode::unsupported, "plugin_lookup",
	    "runtime registry is not available");
}

// Dispatch one frame decode through the runtime path or fail with a stable unsupported error.
void dispatch_decode_frame(const DicomFile& df, std::size_t frame_index,
    std::span<std::uint8_t> dst, const DecodePlan& plan) {
	const auto request = build_decode_dispatch_request_view(df, plan);
	const auto settings = resolve_decode_frame_thread_settings(request);
	dispatch_decode_frame_with_codec_threads(df, request, frame_index, dst,
	    settings.codec_threads);
}

void dispatch_decode_prepared_frame(const DicomFile& df,
    const PixelLayout& source_layout,
    std::size_t frame_index, std::span<const std::uint8_t> prepared_source,
	std::span<std::uint8_t> dst, const DecodePlan& plan) {
	try {
		const auto request = build_decode_dispatch_request_view(
		    df.transfer_syntax_uid(), source_layout, plan);
		// This entry point is used when another layer already materialized one frame source.
		validate_decode_frame_request_or_throw(request, frame_index);

#if defined(DICOMSDL_PIXEL_RUNTIME_ENABLED)
		// Prepared-frame dispatch skips the raw-copy shortcut because the caller already
		// chose the source representation; builtin uncompressed decode is still worth trying.
		if (request.transfer_syntax.is_uncompressed() &&
		    try_decode_frame_with_builtin_uncompressed_core_source(
		        request, prepared_source, frame_index, dst)) {
			return;
		}
		const auto settings = resolve_decode_frame_thread_settings(request);
		if (try_decode_frame_with_runtime_source(
		        request, prepared_source, frame_index, dst, settings.codec_threads)) {
			return;
		}
#endif

		throw_frame_codec_stage_exception(frame_index,
		    CodecStatusCode::unsupported, "plugin_lookup",
		    "runtime registry is not available");
	} catch (const diag::DicomException& ex) {
		rethrow_codec_exception_at_boundary(
		    "pixel::decode_frame_into", df, ex);
	}
}

void dispatch_decode_all_frames(
    const DicomFile& df, std::span<std::uint8_t> dst, const DecodePlan& plan) {
	dispatch_decode_all_frames(df, dst, plan, nullptr);
}

void dispatch_decode_all_frames(const DicomFile& df,
    std::span<std::uint8_t> dst, const DecodePlan& plan,
    const ExecutionObserver* observer) {
	const auto request = build_decode_dispatch_request_view(df, plan);
	validate_decode_all_frames_request_or_throw(request, dst);
	const auto frames = static_cast<std::size_t>(request.output_layout.frames);
	const auto settings = resolve_decode_all_frames_thread_settings(request);
	const auto worker_count = settings.worker_count;
	const auto notify_every = resolve_execution_notify_every(observer);
	if (should_cancel_execution(observer)) {
		throw_decode_all_frames_cancelled(0, frames);
	}

	std::atomic<std::size_t> next_frame{0};
	std::atomic<std::size_t> completed_frames{0};
	std::atomic<bool> stop{false};
	std::atomic<bool> cancelled{false};
	std::mutex error_mutex{};
	std::mutex progress_mutex{};
	std::exception_ptr first_error{};

	const auto notify_progress = [&](std::size_t completed) {
		if (observer == nullptr || observer->on_progress == nullptr ||
		    notify_every == std::size_t{0}) {
			return;
		}
		if (completed != frames &&
		    (completed % notify_every) != std::size_t{0}) {
			return;
		}
		std::lock_guard<std::mutex> lock(progress_mutex);
		observer->on_progress(completed, frames, observer->user_data);
	};

	const auto worker = [&]() {
		// Each worker claims frame indices monotonically and decodes into the
		// corresponding frame-sized window inside the contiguous destination buffer.
		while (!stop.load(std::memory_order_acquire)) {
			if (should_cancel_execution(observer)) {
				cancelled.store(true, std::memory_order_release);
				stop.store(true, std::memory_order_release);
				return;
			}
			const auto frame_index = next_frame.fetch_add(1, std::memory_order_relaxed);
			if (frame_index >= frames) {
				return;
			}
			auto frame_dst = dst.subspan(
			    frame_index * request.output_layout.frame_stride,
			    request.output_layout.frame_stride);
			try {
				dispatch_decode_frame_with_codec_threads(df, request, frame_index,
				    frame_dst,
				    settings.codec_threads);
			} catch (...) {
				// Preserve only the first failure and ask the remaining workers to stop.
				{
					std::lock_guard<std::mutex> lock(error_mutex);
					if (!first_error) {
						first_error = std::current_exception();
					}
				}
				stop.store(true, std::memory_order_release);
				return;
			}
			const auto completed =
			    completed_frames.fetch_add(1, std::memory_order_acq_rel) + 1;
			notify_progress(completed);
		}
	};

	if (worker_count <= std::size_t{1}) {
		worker();
	} else {
		std::vector<std::thread> workers{};
		workers.reserve(worker_count - 1);
		for (std::size_t i = 1; i < worker_count; ++i) {
			workers.emplace_back(worker);
		}
		worker();
		for (auto& thread : workers) {
			thread.join();
		}
	}
	if (first_error) {
		std::rethrow_exception(first_error);
	}
	const auto completed = completed_frames.load(std::memory_order_acquire);
	if (completed == frames) {
		return;
	}
	if (cancelled.load(std::memory_order_acquire)) {
		throw_decode_all_frames_cancelled(completed, frames);
	}
}

} // namespace dicom::pixel::detail

