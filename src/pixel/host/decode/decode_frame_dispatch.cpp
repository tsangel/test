#include "pixel/host/decode/decode_frame_dispatch.hpp"

#include "pixel/host/error/codec_error.hpp"
#include "pixel/host/support/dicom_pixel_support.hpp"

#if defined(DICOMSDL_PIXEL_RUNTIME_ENABLED)
#include "pixel/codecs/uncompressed_v2/direct_api_v2.hpp"
#include "pixel/host/adapter/host_adapter_v2.hpp"
#include "pixel/host/support/abi_convert_v2.hpp"
#include "pixel/runtime/runtime_registry_v2.hpp"
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
#include <thread>
#include <vector>

namespace dicom::pixel::detail {

namespace {

#if defined(DICOMSDL_PIXEL_RUNTIME_ENABLED)
[[nodiscard]] const ::pixel::runtime_v2::BindingRegistryV2* get_runtime_registry();
#endif

struct EffectiveExecutionThreadSettings {
	std::size_t worker_count{1};
	int codec_threads{1};
};

[[nodiscard]] std::size_t resolve_hardware_thread_count() noexcept {
	const unsigned int hw_threads = std::thread::hardware_concurrency();
	return hw_threads == 0u ? std::size_t{1}
	                        : static_cast<std::size_t>(hw_threads);
}

void validate_decode_thread_options_or_throw(
    const DicomFile& df, std::size_t frame_index, const DecodePlan& plan,
    std::string_view api_name) {
	if (plan.options.worker_threads < -1) {
		throw_codec_error_with_context(api_name, df.path(), plan.info.ts, frame_index,
		    CodecError{
		        .code = CodecStatusCode::invalid_argument,
		        .stage = "validate_plan",
		        .detail = "worker_threads must be -1, 0, or positive",
		    });
	}
	if (plan.options.codec_threads < -1) {
		throw_codec_error_with_context(api_name, df.path(), plan.info.ts, frame_index,
		    CodecError{
		        .code = CodecStatusCode::invalid_argument,
		        .stage = "validate_plan",
		        .detail = "codec_threads must be -1, 0, or positive",
		    });
	}
}

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
	validate_decode_thread_options_or_throw(
	    df, frame_index, plan, "pixel::decode_frame_into");
}

void validate_decode_all_frames_request_or_throw(
    const DicomFile& df, std::span<std::uint8_t> dst, const DecodePlan& plan) {
	if (!plan.info.ts.valid() || plan.strides.frame == 0) {
		const auto transfer_syntax =
		    plan.info.ts.valid() ? plan.info.ts : df.transfer_syntax_uid();
		throw_codec_error_with_context("pixel::decode_all_frames_into", df.path(),
		    transfer_syntax, 0,
		    CodecError{
		        .code = CodecStatusCode::invalid_argument,
		        .stage = "validate_plan",
		        .detail = "decode plan is not initialized; call create_decode_plan()",
		    });
	}
	validate_decode_thread_options_or_throw(
	    df, 0, plan, "pixel::decode_all_frames_into");
	if (plan.info.frames <= 0) {
		throw_codec_error_with_context("pixel::decode_all_frames_into", df.path(),
		    plan.info.ts, 0,
		    CodecError{
		        .code = CodecStatusCode::invalid_argument,
		        .stage = "validate_plan",
		        .detail = "decode plan does not describe any frames",
		    });
	}

	const auto frames = static_cast<std::size_t>(plan.info.frames);
	if (plan.strides.frame > (std::numeric_limits<std::size_t>::max() / frames)) {
		throw_codec_error_with_context("pixel::decode_all_frames_into", df.path(),
		    plan.info.ts, 0,
		    CodecError{
		        .code = CodecStatusCode::invalid_argument,
		        .stage = "validate_dst",
		        .detail = "decoded output size overflow for all frames",
		    });
	}

	const auto required_bytes = plan.strides.frame * frames;
	if (dst.size() < required_bytes) {
		throw_codec_error_with_context("pixel::decode_all_frames_into", df.path(),
		    plan.info.ts, 0,
		    CodecError{
		        .code = CodecStatusCode::invalid_argument,
		        .stage = "validate_dst",
		        .detail = "destination buffer is smaller than required decoded size",
		    });
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

[[noreturn]] void throw_decode_all_frames_cancelled(const DicomFile& df,
    const DecodePlan& plan, std::size_t completed, std::size_t total) {
	throw_codec_error_with_context("pixel::decode_all_frames_into", df.path(),
	    plan.info.ts, std::nullopt,
	    CodecError{
	        .code = CodecStatusCode::cancelled,
	        .stage = "cancel",
	        .detail = "decode cancelled by observer after " +
	            std::to_string(completed) + " of " + std::to_string(total) +
	            " frames",
	    });
}

[[nodiscard]] int resolve_auto_codec_thread_cap(
    uid::WellKnown transfer_syntax) noexcept {
	if (transfer_syntax.is_jpeg2000()) {
		return 8;
	}
	if (transfer_syntax.is_jpegxl()) {
		return 4;
	}
	if (!transfer_syntax.is_htj2k()) {
		return 1;
	}

#if defined(DICOMSDL_PIXEL_RUNTIME_ENABLED)
	const auto* registry = get_runtime_registry();
	if (registry != nullptr) {
		uint32_t codec_profile_code = PIXEL_CODEC_PROFILE_UNKNOWN_V2;
		if (::pixel::runtime_v2::codec_profile_code_from_transfer_syntax(
		        transfer_syntax, &codec_profile_code)) {
			const auto* binding = registry->find_decoder_binding(codec_profile_code);
			if (binding != nullptr && binding->display_name != nullptr) {
				const std::string_view display_name{binding->display_name};
				if (display_name.find("OpenJPEG") != std::string_view::npos) {
					return 8;
				}
				if (display_name.find("HTJ2K") != std::string_view::npos) {
					return 1;
				}
			}
		}
	}
#endif

	return get_htj2k_decoder_backend() == Htj2kDecoderBackend::openjpeg ? 8 : 1;
}

[[nodiscard]] EffectiveExecutionThreadSettings resolve_decode_frame_thread_settings(
    const DecodePlan& plan) noexcept {
	const int codec_cap = resolve_auto_codec_thread_cap(plan.info.ts);
	const int codec_threads =
	    plan.options.codec_threads == -1 ? codec_cap : plan.options.codec_threads;
	return EffectiveExecutionThreadSettings{
	    .worker_count = std::size_t{1},
	    .codec_threads = codec_threads,
	};
}

[[nodiscard]] EffectiveExecutionThreadSettings resolve_decode_all_frames_thread_settings(
    const DecodePlan& plan) noexcept {
	const auto frames =
	    plan.info.frames > 0 ? static_cast<std::size_t>(plan.info.frames) : std::size_t{1};
	const auto hardware_threads = resolve_hardware_thread_count();

	std::size_t worker_count = std::size_t{1};
	if (frames > std::size_t{1}) {
		if (plan.options.worker_threads == -1) {
			worker_count = (frames < hardware_threads) ? frames : hardware_threads;
		} else if (plan.options.worker_threads > 1) {
			worker_count = static_cast<std::size_t>(plan.options.worker_threads);
			if (worker_count > frames) {
				worker_count = frames;
			}
		}
	}

	const int codec_cap = resolve_auto_codec_thread_cap(plan.info.ts);
	int codec_threads = plan.options.codec_threads;
	if (codec_threads == -1) {
		if (worker_count >= std::size_t{4}) {
			codec_threads = 1;
		} else {
			std::size_t budget = hardware_threads;
			if (worker_count > std::size_t{1}) {
				budget /= worker_count;
			}
			if (budget == std::size_t{0}) {
				budget = std::size_t{1};
			}
			const auto capped_budget = static_cast<std::size_t>(codec_cap);
			if (budget > capped_budget) {
				budget = capped_budget;
			}
			codec_threads = static_cast<int>(budget);
		}
	}

	return EffectiveExecutionThreadSettings{
	    .worker_count = worker_count,
	    .codec_threads = codec_threads,
	};
}

} // namespace

#if defined(DICOMSDL_PIXEL_RUNTIME_ENABLED)
namespace {

constexpr int kNoThreadOption = std::numeric_limits<int>::min();

// Reuse one decoder context per thread to avoid paying full configure/create
// cost on every frame decode when the registry/options are unchanged.
struct DecoderContextCache {
	::pixel::runtime_v2::HostDecoderContextV2 ctx{};
	const ::pixel::runtime_v2::BindingRegistryV2* registry{nullptr};
	std::uint64_t registry_generation{0};
	std::uint32_t transfer_syntax_index{0};
	int codec_thread_option{kNoThreadOption};
	bool configured{false};

	DecoderContextCache() = default;

	~DecoderContextCache() {
		::pixel::runtime_v2::destroy_host_decoder_context_v2(&ctx);
	}

	DecoderContextCache(const DecoderContextCache&) = delete;
	DecoderContextCache& operator=(const DecoderContextCache&) = delete;
};

[[nodiscard]] DecoderContextCache& runtime_decoder_context_cache() {
	thread_local DecoderContextCache cache{};
	return cache;
}

// Return the current process-wide runtime registry, if one is installed.
[[nodiscard]] const ::pixel::runtime_v2::BindingRegistryV2* get_runtime_registry() {
	// Runtime dispatch is optional, so callers must handle a null registry.
	return ::pixel::runtime_v2::current_registry();
}

[[nodiscard]] std::uint64_t get_runtime_registry_generation() {
	return ::pixel::runtime_v2::current_registry_generation();
}

void populate_direct_decode_request(pixel_decoder_request_v2& request,
    uint32_t codec_profile_code, const PixelDataInfo& info,
    std::span<const uint8_t> prepared_source, std::span<uint8_t> destination,
    const DecodePlan& plan) {
	::pixel::runtime_v2::DtypeMeta source_dtype{};
	if (!::pixel::runtime_v2::resolve_dtype_meta(info.sv_dtype, &source_dtype)) {
		source_dtype.code = PIXEL_DTYPE_UNKNOWN_V2;
	}
	const uint8_t dst_dtype =
	    plan.modality_value_transform.enabled ? PIXEL_DTYPE_F32_V2 : source_dtype.code;
	const bool mct_capable_profile =
	    codec_profile_code == PIXEL_CODEC_PROFILE_JPEG2000_LOSSLESS_V2 ||
	    codec_profile_code == PIXEL_CODEC_PROFILE_JPEG2000_LOSSY_V2 ||
	    codec_profile_code == PIXEL_CODEC_PROFILE_HTJ2K_LOSSLESS_V2 ||
	    codec_profile_code == PIXEL_CODEC_PROFILE_HTJ2K_LOSSLESS_RPCL_V2 ||
	    codec_profile_code == PIXEL_CODEC_PROFILE_HTJ2K_LOSSY_V2;

	request = {};
	request.struct_size = sizeof(pixel_decoder_request_v2);
	request.abi_version = PIXEL_DECODER_PLUGIN_ABI_V2;

	request.source.struct_size = sizeof(pixel_decoder_source_v2);
	request.source.abi_version = PIXEL_DECODER_PLUGIN_ABI_V2;
	request.source.source_buffer.data = prepared_source.data();
	request.source.source_buffer.size = static_cast<uint64_t>(prepared_source.size());

	request.frame.struct_size = sizeof(pixel_decoder_frame_info_v2);
	request.frame.abi_version = PIXEL_DECODER_PLUGIN_ABI_V2;
	request.frame.codec_profile_code = codec_profile_code;
	request.frame.source_dtype = source_dtype.code;
	request.frame.source_planar =
	    ::pixel::runtime_v2::to_planar_code(info.planar_configuration);
	request.frame.rows = info.rows;
	request.frame.cols = info.cols;
	request.frame.samples_per_pixel = info.samples_per_pixel;
	request.frame.bits_stored = info.bits_stored;
	request.frame.decode_mct =
	    (mct_capable_profile && plan.options.decode_mct) ? 1u : 0u;

	request.output.struct_size = sizeof(pixel_decoder_output_v2);
	request.output.abi_version = PIXEL_DECODER_PLUGIN_ABI_V2;
	request.output.dst = destination.data();
	request.output.dst_size = static_cast<uint64_t>(destination.size());
	request.output.row_stride = static_cast<uint64_t>(plan.strides.row);
	request.output.frame_stride = static_cast<uint64_t>(plan.strides.frame);
	request.output.dst_dtype = dst_dtype;
	request.output.dst_planar =
	    ::pixel::runtime_v2::to_planar_code(plan.options.planar_out);

	request.value_transform.struct_size = sizeof(pixel_decoder_value_transform_v2);
	request.value_transform.abi_version = PIXEL_DECODER_PLUGIN_ABI_V2;
	request.value_transform.transform_kind = PIXEL_DECODER_VALUE_TRANSFORM_NONE_V2;
	request.value_transform.rescale_slope = 1.0;
	request.value_transform.rescale_intercept = 0.0;

	if (!plan.modality_value_transform.enabled) {
		return;
	}

	if (plan.modality_value_transform.modality_lut.has_value()) {
		const auto& lut = *plan.modality_value_transform.modality_lut;
		request.value_transform.transform_kind =
		    PIXEL_DECODER_VALUE_TRANSFORM_MODALITY_LUT_V2;
		request.value_transform.lut_first_mapped = lut.first_mapped;
		request.value_transform.lut_value_count =
		    static_cast<uint64_t>(lut.values.size());
		request.value_transform.lut_values_f32.data =
		    reinterpret_cast<const uint8_t*>(lut.values.data());
		request.value_transform.lut_values_f32.size =
		    static_cast<uint64_t>(lut.values.size() * sizeof(float));
		return;
	}

	request.value_transform.transform_kind = PIXEL_DECODER_VALUE_TRANSFORM_RESCALE_V2;
	request.value_transform.rescale_slope = plan.modality_value_transform.rescale_slope;
	request.value_transform.rescale_intercept =
	    plan.modality_value_transform.rescale_intercept;
}

[[nodiscard]] std::string copy_core_error_detail(
    const ::pixel::core_v2::ErrorState& state) {
	std::array<char, 1024> buffer{};
	const uint32_t copied = ::pixel::core_v2::copy_last_error_detail(
	    &state, buffer.data(), static_cast<uint32_t>(buffer.size()));
	if (copied == 0) {
		return {};
	}
	return std::string(buffer.data(), buffer.data() + copied);
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

[[nodiscard]] support_detail::NativeDecodeSourceView prepare_native_decode_source_view_or_throw(
    const DicomFile& df, const PixelDataInfo& info,
    uid::WellKnown transfer_syntax, std::size_t frame_index) {
	try {
		(void)frame_index;
		return support_detail::compute_native_decode_source_view_or_throw(df, info);
	} catch (const std::bad_alloc&) {
		throw_codec_error_with_context("pixel::decode_frame_into", df.path(),
		    transfer_syntax, frame_index,
		    CodecError{
		        .code = CodecStatusCode::internal_error,
		        .stage = "allocate",
		        .detail = "memory allocation failed",
		    });
	} catch (const std::exception& e) {
		throw_codec_error_with_context("pixel::decode_frame_into", df.path(),
		    transfer_syntax, frame_index,
		    CodecError{
		        .code = CodecStatusCode::invalid_argument,
		        .stage = "load_frame_source",
		        .detail = e.what(),
		    });
	} catch (...) {
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
[[nodiscard]] bool try_decode_frame_with_runtime_source(
    std::string_view file_path, std::span<const std::uint8_t> prepared_source,
    std::size_t frame_index, std::span<std::uint8_t> dst, const DecodePlan& plan,
    int codec_threads_override) {
	const auto& info = plan.info;
	const auto* registry = get_runtime_registry();
	const auto registry_generation = get_runtime_registry_generation();

	// Runtime dispatch is optional; callers fall back when no registry is installed.
	if (registry == nullptr) {
		return false;
	}

	auto& cache = runtime_decoder_context_cache();
	auto* const ctx = &cache.ctx;

	// Only codecs with an exposed thread knob need runtime configure options.
	pixel_option_kv_v2 option_item{};
	pixel_option_list_v2 option_list{};
	char threads_text[32]{};
	int codec_thread_option = kNoThreadOption;
	if (info.ts.is_jpeg2000() || info.ts.is_htj2k() || info.ts.is_jpegxl()) {
		codec_thread_option = codec_threads_override;
		std::snprintf(
		    threads_text, sizeof(threads_text), "%d", codec_threads_override);
		option_item.key = "threads";
		option_item.value = threads_text;
		option_list.items = &option_item;
		option_list.count = 1u;
	}
	const pixel_option_list_v2* option_ptr =
	    option_list.count == 0 ? nullptr : &option_list;

	const auto transfer_syntax_index = info.ts.raw_index();
	bool needs_configure = true;
	if (cache.configured && cache.registry == registry &&
	    cache.registry_generation == registry_generation &&
	    cache.transfer_syntax_index == transfer_syntax_index &&
	    cache.codec_thread_option == codec_thread_option) {
		needs_configure = false;
	}

	pixel_error_code_v2 configure_ec = PIXEL_CODEC_ERR_OK;
	std::string configure_detail{};
	if (needs_configure) {
		configure_ec = ::pixel::runtime_v2::configure_host_decoder_context_v2(
		    ctx, registry, info.ts, option_ptr);
		configure_detail = copy_decoder_error_detail(*ctx);
	}

	// Translate configure-time binding failures into the public host error model.
	if (configure_ec == PIXEL_CODEC_ERR_UNSUPPORTED) {
		cache.configured = false;
		throw_codec_error_with_context("pixel::decode_frame_into", file_path, info.ts,
		    frame_index,
		    CodecError{
		        .code = CodecStatusCode::unsupported,
		        .stage = "plugin_lookup",
		        .detail = "decoder binding is not registered in runtime registry",
		    });
	}
	if (configure_ec != PIXEL_CODEC_ERR_OK) {
		cache.configured = false;
		throw_runtime_decode_error(
		    file_path, info.ts, frame_index, configure_ec, configure_detail);
	}
	if (needs_configure) {
		cache.configured = true;
		cache.registry = registry;
		cache.registry_generation = registry_generation;
		cache.transfer_syntax_index = transfer_syntax_index;
		cache.codec_thread_option = codec_thread_option;
	}

	// Convert the plan transform to the host ABI form and decode from caller-owned bytes.
	const auto host_transform =
	    prepare_host_value_transform(plan.modality_value_transform);
	const ::pixel::runtime_v2::HostModalityValueTransformV2* transform_ptr =
	    host_transform.kind == ::pixel::runtime_v2::HostModalityValueTransformKindV2::kNone
	        ? nullptr
	        : &host_transform;

	// Decode using the plan's metadata, output layout, and effective options.
	const pixel_error_code_v2 decode_ec =
	    ::pixel::runtime_v2::decode_frame_with_host_context_v2(
	        ctx, &info, prepared_source, dst, &plan.strides, &plan.options,
	        transform_ptr);

	// Normalize runtime decode failures before they cross the public API boundary.
	if (decode_ec == PIXEL_CODEC_ERR_UNSUPPORTED) {
		throw_codec_error_with_context("pixel::decode_frame_into", file_path, info.ts,
		    frame_index,
		    CodecError{
		        .code = CodecStatusCode::unsupported,
		        .stage = "decode_frame",
		        .detail = "runtime decoder does not support this request",
		    });
	}
	if (decode_ec != PIXEL_CODEC_ERR_OK) {
		throw_runtime_decode_error(file_path, info.ts, frame_index, decode_ec,
		    copy_decoder_error_detail(*ctx));
	}
	return true;
}

[[nodiscard]] bool try_decode_frame_with_runtime(const DicomFile& df,
    std::size_t frame_index, std::span<std::uint8_t> dst, const DecodePlan& plan,
    int codec_threads_override) {
	const auto computed_source =
	    prepare_runtime_frame_source_or_throw(df, plan.info, plan.info.ts, frame_index);
	return try_decode_frame_with_runtime_source(
	    df.path(), computed_source.bytes, frame_index, dst, plan,
	    codec_threads_override);
}

[[nodiscard]] bool try_copy_frame_with_builtin_native_fast_path(
    const DicomFile& df, std::size_t frame_index, std::span<std::uint8_t> dst,
    const DecodePlan& plan) {
	const auto& info = plan.info;
	// REF-style raw frames can skip decode entirely and copy from the native source
	// when the requested output layout already matches the stored layout.
	if (!info.ts.is_uncompressed() || info.ts.is_encapsulated()) {
		return false;
	}
	if (plan.modality_value_transform.enabled) {
		return false;
	}
	if (info.rows <= 0 || info.cols <= 0 || info.samples_per_pixel <= 0) {
		return false;
	}

	const auto rows = static_cast<std::size_t>(info.rows);
	const auto samples = static_cast<std::size_t>(info.samples_per_pixel);
	support_detail::NativeDecodeSourceView native_source{};
	try {
		native_source = support_detail::compute_native_decode_source_view_or_throw(df, info);
	} catch (...) {
		return false;
	}
	const bool source_planar =
	    native_source.planar_source && samples > std::size_t{1};
	const bool output_planar =
	    plan.options.planar_out == Planar::planar && samples > std::size_t{1};

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
	if (row_payload == 0 || plan.strides.row < row_payload ||
	    dst.size() < plan.strides.frame) {
		return false;
	}

	if (!source_planar || samples == 1) {
		if (plan.strides.row == row_payload) {
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
			std::memcpy(dst.data() + r * plan.strides.row, src.data() + src_offset,
			    row_payload);
		}
		return true;
	}

	const std::size_t plane_bytes = row_payload * rows;
	if (src.size() < plane_bytes * samples) {
		return false;
	}
	const std::size_t dst_plane_bytes = plan.strides.row * rows;
	for (std::size_t s = 0; s < samples; ++s) {
		const auto* src_plane = src.data() + s * plane_bytes;
		auto* dst_plane = dst.data() + s * dst_plane_bytes;
		for (std::size_t r = 0; r < rows; ++r) {
			std::memcpy(dst_plane + r * plan.strides.row,
			    src_plane + r * row_payload, row_payload);
		}
	}
	return true;
}

[[nodiscard]] bool try_decode_frame_with_builtin_uncompressed_core_source(
    std::string_view file_path, std::span<const std::uint8_t> prepared_source,
    std::size_t frame_index, std::span<std::uint8_t> dst, const DecodePlan& plan) {
	const auto& info = plan.info;
	// Fall back to the uncompressed core decoder when raw-copy is not enough,
	// e.g. layout conversion, value transform, or encapsulated-uncompressed input.
	if (!info.ts.is_uncompressed()) {
		return false;
	}

	const auto* registry = get_runtime_registry();
	if (registry == nullptr) {
		return false;
	}

	uint32_t codec_profile_code = PIXEL_CODEC_PROFILE_UNKNOWN_V2;
	if (!::pixel::runtime_v2::codec_profile_code_from_transfer_syntax(
	        info.ts, &codec_profile_code)) {
		return false;
	}
	const auto* binding = registry->find_decoder_binding(codec_profile_code);
	if (binding == nullptr ||
	    binding->binding_kind != ::pixel::runtime_v2::DecoderBindingKind::kCoreDirect) {
		return false;
	}

	pixel_decoder_request_v2 request{};
	populate_direct_decode_request(
	    request, codec_profile_code, info, prepared_source, dst, plan);
	if (request.frame.source_dtype == PIXEL_DTYPE_UNKNOWN_V2) {
		return false;
	}

	::pixel::core_v2::ErrorState error_state{};
	const pixel_error_code_v2 decode_ec =
	    ::pixel::core_v2::decode_uncompressed_frame(&error_state, &request);
	if (decode_ec != PIXEL_CODEC_ERR_OK) {
		throw_runtime_decode_error(file_path, info.ts, frame_index, decode_ec,
		    copy_core_error_detail(error_state));
	}
	return true;
}

[[nodiscard]] bool try_decode_frame_with_builtin_uncompressed_core_path(const DicomFile& df,
    std::size_t frame_index, std::span<std::uint8_t> dst, const DecodePlan& plan) {
	const auto computed_source =
	    prepare_runtime_frame_source_or_throw(df, plan.info, plan.info.ts, frame_index);
	return try_decode_frame_with_builtin_uncompressed_core_source(
	    df.path(), computed_source.bytes, frame_index, dst, plan);
}

[[nodiscard]] bool try_decode_frame_with_builtin_uncompressed_fast_paths(const DicomFile& df,
    std::size_t frame_index, std::span<std::uint8_t> dst, const DecodePlan& plan) {
	if (!plan.info.ts.is_uncompressed()) {
		return false;
	}

	// Fast-path order matters:
	// 1. REF/native raw copy
	// 2. uncompressed core decode
	// Everything else falls through to the generic runtime path below.
	if (try_copy_frame_with_builtin_native_fast_path(
		        df, frame_index, dst, plan)) {
		return true;
	}
	if (try_decode_frame_with_builtin_uncompressed_core_path(
	        df, frame_index, dst, plan)) {
		return true;
	}
	return false;
}

}  // namespace
#endif

void dispatch_decode_frame_with_codec_threads(const DicomFile& df,
    std::size_t frame_index, std::span<std::uint8_t> dst, const DecodePlan& plan,
    int codec_threads) {
	validate_decode_plan_or_throw(df, frame_index, plan);
#if defined(DICOMSDL_PIXEL_RUNTIME_ENABLED)
	// Try the narrow builtin hot paths first before paying the full runtime
	// adapter/setup cost for general codec dispatch.
	if (try_decode_frame_with_builtin_uncompressed_fast_paths(
	        df, frame_index, dst, plan)) {
		return;
	}
	// Prefer the runtime-backed path when that subsystem is compiled in and initialized.
	if (try_decode_frame_with_runtime(
	        df, frame_index, dst, plan, codec_threads)) {
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

// Dispatch one frame decode through the runtime path or fail with a stable unsupported error.
void dispatch_decode_frame(const DicomFile& df, std::size_t frame_index,
    std::span<std::uint8_t> dst, const DecodePlan& plan) {
	const auto settings = resolve_decode_frame_thread_settings(plan);
	dispatch_decode_frame_with_codec_threads(
	    df, frame_index, dst, plan, settings.codec_threads);
}

void dispatch_decode_prepared_frame(std::string_view file_path,
    std::size_t frame_index, std::span<const std::uint8_t> prepared_source,
    std::span<std::uint8_t> dst, const DecodePlan& plan) {
	if (!plan.info.ts.valid() || plan.strides.frame == 0) {
		throw_codec_error_with_context("pixel::decode_frame_into", file_path,
		    plan.info.ts, frame_index,
		    CodecError{
		        .code = CodecStatusCode::invalid_argument,
		        .stage = "validate_plan",
		        .detail = "decode plan is not initialized; call create_decode_plan()",
		    });
	}
	if (plan.options.worker_threads < -1) {
		throw_codec_error_with_context("pixel::decode_frame_into", file_path,
		    plan.info.ts, frame_index,
		    CodecError{
		        .code = CodecStatusCode::invalid_argument,
		        .stage = "validate_plan",
		        .detail = "worker_threads must be -1, 0, or positive",
		    });
	}
	if (plan.options.codec_threads < -1) {
		throw_codec_error_with_context("pixel::decode_frame_into", file_path,
		    plan.info.ts, frame_index,
		    CodecError{
		        .code = CodecStatusCode::invalid_argument,
		        .stage = "validate_plan",
		        .detail = "codec_threads must be -1, 0, or positive",
		    });
	}

#if defined(DICOMSDL_PIXEL_RUNTIME_ENABLED)
	if (plan.info.ts.is_uncompressed() &&
	    try_decode_frame_with_builtin_uncompressed_core_source(
	        file_path, prepared_source, frame_index, dst, plan)) {
		return;
	}
	const auto settings = resolve_decode_frame_thread_settings(plan);
	if (try_decode_frame_with_runtime_source(
	        file_path, prepared_source, frame_index, dst, plan,
	        settings.codec_threads)) {
		return;
	}
#endif

	CodecError decode_error{};
	decode_error.code = CodecStatusCode::unsupported;
	decode_error.stage = "plugin_lookup";
	decode_error.detail = "runtime registry is not available";
	throw_codec_error_with_context("pixel::decode_frame_into", file_path,
	    plan.info.ts, frame_index, decode_error);
}

void dispatch_decode_all_frames(
    const DicomFile& df, std::span<std::uint8_t> dst, const DecodePlan& plan) {
	dispatch_decode_all_frames(df, dst, plan, nullptr);
}

void dispatch_decode_all_frames(const DicomFile& df,
    std::span<std::uint8_t> dst, const DecodePlan& plan,
    const ExecutionObserver* observer) {
	validate_decode_all_frames_request_or_throw(df, dst, plan);
	const auto frames = static_cast<std::size_t>(plan.info.frames);
	const auto settings = resolve_decode_all_frames_thread_settings(plan);
	const auto worker_count = settings.worker_count;
	const auto notify_every = resolve_execution_notify_every(observer);
	if (should_cancel_execution(observer)) {
		throw_decode_all_frames_cancelled(df, plan, 0, frames);
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
			auto frame_dst = dst.subspan(frame_index * plan.strides.frame, plan.strides.frame);
			try {
				dispatch_decode_frame_with_codec_threads(
				    df, frame_index, frame_dst, plan, settings.codec_threads);
			} catch (...) {
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
		throw_decode_all_frames_cancelled(df, plan, completed, frames);
	}
}

} // namespace dicom::pixel::detail
