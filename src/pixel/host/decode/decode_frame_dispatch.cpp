#include "pixel/host/decode/decode_frame_dispatch.hpp"

#include "pixel/host/error/codec_error.hpp"
#include "pixel/host/support/dicom_pixel_support.hpp"

#if defined(DICOMSDL_PIXEL_RUNTIME_ENABLED)
#include "pixel/codecs/uncompressed_v2/direct_api_v2.hpp"
#if defined(DICOMSDL_PIXEL_RLE_STATIC_PLUGIN_ENABLED) && DICOMSDL_PIXEL_RLE_STATIC_PLUGIN_ENABLED
#include "pixel/codecs/rle_v2/builtin_api.hpp"
#endif
#include "pixel/host/adapter/host_adapter_v2.hpp"
#include "pixel/runtime/runtime_registry_v2.hpp"
#endif

#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <limits>
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

constexpr int kNoThreadOption = std::numeric_limits<int>::min();

// Reuse one decoder context per thread to avoid paying full configure/create
// cost on every frame decode when the registry/options are unchanged.
struct DecoderContextCache {
	::pixel::runtime_v2::HostDecoderContextV2 ctx{};
	const ::pixel::runtime_v2::PluginRegistryV2* registry{nullptr};
	std::uint64_t registry_generation{0};
	std::uint32_t transfer_syntax_index{0};
	int thread_option{kNoThreadOption};
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
[[nodiscard]] const ::pixel::runtime_v2::PluginRegistryV2* get_runtime_registry() {
	// Runtime dispatch is optional, so callers must handle a null registry.
	return ::pixel::runtime_v2::current_registry();
}

[[nodiscard]] std::uint64_t get_runtime_registry_generation() {
	return ::pixel::runtime_v2::current_registry_generation();
}

[[nodiscard]] uint8_t to_direct_dtype_code(dicom::pixel::DataType data_type) noexcept {
	switch (data_type) {
	case dicom::pixel::DataType::u8:
		return PIXEL_DTYPE_U8_V2;
	case dicom::pixel::DataType::s8:
		return PIXEL_DTYPE_S8_V2;
	case dicom::pixel::DataType::u16:
		return PIXEL_DTYPE_U16_V2;
	case dicom::pixel::DataType::s16:
		return PIXEL_DTYPE_S16_V2;
	case dicom::pixel::DataType::u32:
		return PIXEL_DTYPE_U32_V2;
	case dicom::pixel::DataType::s32:
		return PIXEL_DTYPE_S32_V2;
	case dicom::pixel::DataType::f32:
		return PIXEL_DTYPE_F32_V2;
	case dicom::pixel::DataType::f64:
		return PIXEL_DTYPE_F64_V2;
	case dicom::pixel::DataType::unknown:
	default:
		return PIXEL_DTYPE_UNKNOWN_V2;
	}
}

[[nodiscard]] uint8_t to_direct_planar_code(dicom::pixel::Planar planar) noexcept {
	return planar == dicom::pixel::Planar::planar
	    ? PIXEL_PLANAR_PLANAR_V2
	    : PIXEL_PLANAR_INTERLEAVED_V2;
}

void populate_direct_decode_request(pixel_decoder_request_v2& request,
    uint32_t codec_profile_code, const PixelDataInfo& info,
    std::span<const uint8_t> prepared_source, std::span<uint8_t> destination,
    const DecodePlan& plan) {
	const uint8_t source_dtype = to_direct_dtype_code(info.sv_dtype);
	const uint8_t dst_dtype =
	    plan.modality_value_transform.enabled ? PIXEL_DTYPE_F32_V2 : source_dtype;
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
	request.frame.source_dtype = source_dtype;
	request.frame.source_planar = to_direct_planar_code(info.planar_configuration);
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
	request.output.dst_planar = to_direct_planar_code(plan.options.planar_out);

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

#if defined(DICOMSDL_PIXEL_RLE_STATIC_PLUGIN_ENABLED) && DICOMSDL_PIXEL_RLE_STATIC_PLUGIN_ENABLED
[[nodiscard]] bool checked_mul_size_t(
    std::size_t lhs, std::size_t rhs, std::size_t& out) noexcept {
	if (lhs == 0 || rhs == 0) {
		out = 0;
		return true;
	}
	if (lhs > std::numeric_limits<std::size_t>::max() / rhs) {
		return false;
	}
	out = lhs * rhs;
	return true;
}

[[nodiscard]] uint32_t direct_bit_mask_u32(int bits) noexcept {
	if (bits <= 0) {
		return 0;
	}
	if (bits >= 32) {
		return 0xFFFFFFFFu;
	}
	return (uint32_t{1} << static_cast<unsigned>(bits)) - 1u;
}

[[nodiscard]] int32_t direct_sign_extend_u32(uint32_t raw, int bits) noexcept {
	if (bits <= 0) {
		return 0;
	}
	if (bits >= 32) {
		return static_cast<int32_t>(raw);
	}
	const int shift = 32 - bits;
	return static_cast<int32_t>(raw << static_cast<unsigned>(shift)) >>
	    static_cast<unsigned>(shift);
}

[[nodiscard]] bool load_direct_integral_sample(const uint8_t* src,
    std::size_t sample_bytes, bool is_signed, int bits_stored,
    int32_t* out_value) noexcept {
	if (src == nullptr || out_value == nullptr) {
		return false;
	}

	uint32_t raw = 0;
	switch (sample_bytes) {
	case 1:
		raw = src[0];
		break;
	case 2: {
		uint16_t value = 0;
		std::memcpy(&value, src, sizeof(value));
		raw = value;
		break;
	}
	case 4: {
		uint32_t value = 0;
		std::memcpy(&value, src, sizeof(value));
		raw = value;
		break;
	}
	default:
		return false;
	}

	if (is_signed) {
		*out_value = direct_sign_extend_u32(raw, bits_stored);
		return true;
	}

	raw &= direct_bit_mask_u32(bits_stored);
	*out_value = static_cast<int32_t>(raw);
	return true;
}

[[nodiscard]] float apply_direct_modality_transform(
    const ModalityValueTransform& value_transform, int32_t sample_value) {
	if (value_transform.modality_lut.has_value()) {
		const auto& lut = *value_transform.modality_lut;
		if (lut.values.empty()) {
			return 0.0f;
		}
		std::int64_t idx =
		    static_cast<std::int64_t>(sample_value) - lut.first_mapped;
		if (idx < 0) {
			idx = 0;
		}
		const auto max_index =
		    static_cast<std::int64_t>(lut.values.size() - std::size_t{1});
		if (idx > max_index) {
			idx = max_index;
		}
		return lut.values[static_cast<std::size_t>(idx)];
	}

	return static_cast<float>(
	    static_cast<double>(sample_value) * value_transform.rescale_slope +
	    value_transform.rescale_intercept);
}

void decode_rle_packbits_segment_or_throw(std::size_t segment_index,
    std::span<const std::uint8_t> encoded, std::span<std::uint8_t> decoded) {
	std::size_t in = 0;
	std::size_t out = 0;
	while (out < decoded.size()) {
		if (in >= encoded.size()) {
			throw std::runtime_error(
			    "RLE segment ended early at index=" + std::to_string(segment_index));
		}

		const auto control = static_cast<std::int8_t>(encoded[in++]);
		if (control >= 0) {
			const auto literal_count = static_cast<std::size_t>(control) + 1u;
			if (in + literal_count > encoded.size() ||
			    out + literal_count > decoded.size()) {
				throw std::runtime_error(
				    "RLE literal run out of bounds at segment=" +
				    std::to_string(segment_index));
			}
			std::memcpy(decoded.data() + out, encoded.data() + in, literal_count);
			in += literal_count;
			out += literal_count;
			continue;
		}

		if (control >= -127) {
			const auto repeat_count = static_cast<std::size_t>(1 - control);
			if (in >= encoded.size() || out + repeat_count > decoded.size()) {
				throw std::runtime_error(
				    "RLE repeat run out of bounds at segment=" +
				    std::to_string(segment_index));
			}
			std::memset(decoded.data() + out, encoded[in], repeat_count);
			++in;
			out += repeat_count;
			continue;
		}
	}
}

void decode_rle_single_channel_direct_or_throw(
    std::span<const std::uint8_t> encoded_frame, const PixelDataInfo& info,
    std::span<std::uint8_t> dst, const DecodePlan& plan) {
	if (encoded_frame.size() < 64) {
		throw std::runtime_error("RLE codestream is shorter than 64-byte header");
	}

	const auto sample_bytes = bytes_per_sample_of(info.sv_dtype);
	if (sample_bytes == 0 || sample_bytes > 4) {
		throw std::runtime_error(
		    "single-channel direct RLE path requires integral 8/16/32-bit source dtype");
	}

	const auto rows = static_cast<std::size_t>(info.rows);
	const auto cols = static_cast<std::size_t>(info.cols);
	std::size_t row_payload = 0;
	std::size_t plane_bytes = 0;
	if (!checked_mul_size_t(cols, sample_bytes, row_payload) ||
	    !checked_mul_size_t(row_payload, rows, plane_bytes)) {
		throw std::runtime_error("RLE decoded frame size overflow");
	}
	if (dst.size() < plan.strides.frame) {
		throw std::runtime_error("destination buffer is too small");
	}

	const uint32_t segment_count_u32 = endian::load_le<std::uint32_t>(encoded_frame.data());
	const std::size_t segment_count = static_cast<std::size_t>(segment_count_u32);
	if (segment_count != sample_bytes) {
		throw std::runtime_error("RLE segment count does not match expected single-channel layout");
	}

	std::array<std::size_t, 15> offsets{};
	for (std::size_t i = 0; i < segment_count; ++i) {
		offsets[i] = static_cast<std::size_t>(
		    endian::load_le<std::uint32_t>(
		        encoded_frame.data() + 4u + i * sizeof(std::uint32_t)));
	}
	for (std::size_t i = 0; i < segment_count; ++i) {
		const std::size_t start = offsets[i];
		const std::size_t end =
		    (i + 1u < segment_count) ? offsets[i + 1u] : encoded_frame.size();
		if (start < 64u || start >= encoded_frame.size() ||
		    end < start || end > encoded_frame.size()) {
			throw std::runtime_error(
			    "invalid RLE segment range at index=" + std::to_string(i));
		}
	}

	std::vector<std::uint8_t> byte_plane(rows * cols, uint8_t{0});
	std::vector<std::uint8_t> decoded_plane{};
	const bool needs_value_transform = plan.modality_value_transform.enabled;
	if (needs_value_transform) {
		decoded_plane.assign(plane_bytes, uint8_t{0});
	}

	for (std::size_t byte_plane_index = 0; byte_plane_index < sample_bytes;
	     ++byte_plane_index) {
		const std::size_t segment_start = offsets[byte_plane_index];
		const std::size_t segment_end =
		    (byte_plane_index + 1u < segment_count)
		        ? offsets[byte_plane_index + 1u]
		        : encoded_frame.size();
		decode_rle_packbits_segment_or_throw(
		    byte_plane_index,
		    encoded_frame.subspan(segment_start, segment_end - segment_start),
		    std::span<std::uint8_t>(byte_plane));

		const std::size_t component_byte_index = sample_bytes - 1u - byte_plane_index;
		if (needs_value_transform) {
			for (std::size_t r = 0; r < rows; ++r) {
				const auto* src_row = byte_plane.data() + r * cols;
				auto* dst_row = decoded_plane.data() + r * row_payload;
				for (std::size_t c = 0; c < cols; ++c) {
					dst_row[c * sample_bytes + component_byte_index] = src_row[c];
				}
			}
			continue;
		}

		for (std::size_t r = 0; r < rows; ++r) {
			const auto* src_row = byte_plane.data() + r * cols;
			auto* dst_row = dst.data() + r * plan.strides.row;
			for (std::size_t c = 0; c < cols; ++c) {
				dst_row[c * sample_bytes + component_byte_index] = src_row[c];
			}
		}
	}

	if (!needs_value_transform) {
		return;
	}

	for (std::size_t r = 0; r < rows; ++r) {
		const auto* src_row = decoded_plane.data() + r * row_payload;
		auto* dst_row = reinterpret_cast<float*>(dst.data() + r * plan.strides.row);
		for (std::size_t c = 0; c < cols; ++c) {
			int32_t sample_value = 0;
			if (!load_direct_integral_sample(src_row + c * sample_bytes, sample_bytes,
			        info.sv_dtype == DataType::s8 || info.sv_dtype == DataType::s16 ||
			            info.sv_dtype == DataType::s32,
			        info.bits_stored, &sample_value)) {
				throw std::runtime_error("failed to parse decoded RLE sample");
			}
			dst_row[c] = apply_direct_modality_transform(
			    plan.modality_value_transform, sample_value);
		}
	}
}
#endif

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
	int thread_option = kNoThreadOption;
	if (info.ts.is_jpeg2000() || info.ts.is_jpegxl()) {
		thread_option = plan.options.decoder_threads;
		std::snprintf(threads_text, sizeof(threads_text), "%d",
		    plan.options.decoder_threads);
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

	// Translate configure-time binding failures into the public host error model.
	if (configure_ec == PIXEL_CODEC_ERR_UNSUPPORTED) {
		cache.configured = false;
		throw_codec_error_with_context("pixel::decode_frame_into", df.path(), info.ts,
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
		    df.path(), info.ts, frame_index, configure_ec, configure_detail);
	}
	if (needs_configure) {
		cache.configured = true;
		cache.registry = registry;
		cache.registry_generation = registry_generation;
		cache.transfer_syntax_index = transfer_syntax_index;
		cache.thread_option = thread_option;
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

#if defined(DICOMSDL_PIXEL_RLE_STATIC_PLUGIN_ENABLED) && DICOMSDL_PIXEL_RLE_STATIC_PLUGIN_ENABLED
[[nodiscard]] bool try_decode_frame_with_builtin_rle_direct(const DicomFile& df,
    std::size_t frame_index, std::span<std::uint8_t> dst, const DecodePlan& plan) {
	const auto& info = plan.info;
	if (!info.ts.is_rle()) {
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
	const auto& builtin_api = ::pixel::rle_plugin_v2::decoder_builtin_api();
	if (binding == nullptr || binding->binding_kind != ::pixel::runtime_v2::DecoderBindingKind::kPluginApi ||
	    binding->plugin_api != &builtin_api) {
		return false;
	}
	if (info.samples_per_pixel != 1 || plan.modality_value_transform.enabled &&
	    !(info.sv_dtype == DataType::u8 || info.sv_dtype == DataType::s8 ||
	        info.sv_dtype == DataType::u16 || info.sv_dtype == DataType::s16 ||
	        info.sv_dtype == DataType::u32 || info.sv_dtype == DataType::s32)) {
		return false;
	}

	const auto computed_source =
	    prepare_runtime_frame_source_or_throw(df, info, info.ts, frame_index);
	try {
		decode_rle_single_channel_direct_or_throw(
		    computed_source.bytes, info, dst, plan);
	} catch (const std::bad_alloc&) {
		throw_codec_error_with_context("pixel::decode_frame_into", df.path(), info.ts,
		    frame_index,
		    CodecError{
		        .code = CodecStatusCode::internal_error,
		        .stage = "allocate",
		        .detail = "memory allocation failed",
		    });
	} catch (const std::exception& e) {
		throw_runtime_decode_error(df.path(), info.ts, frame_index,
		    PIXEL_CODEC_ERR_INVALID_ARGUMENT,
		    std::string("stage=decode_frame;reason=") + e.what());
	}
	return true;
}
#endif

[[nodiscard]] bool try_decode_frame_with_builtin_uncompressed_host_copy(
    const DicomFile& df, std::size_t frame_index, std::span<std::uint8_t> dst,
    const DecodePlan& plan) {
	const auto& info = plan.info;
	if (!info.ts.is_uncompressed()) {
		return false;
	}
	if (plan.modality_value_transform.enabled) {
		return false;
	}
	if (info.rows <= 0 || info.cols <= 0 || info.samples_per_pixel <= 0) {
		return false;
	}

	const auto sample_bytes = bytes_per_sample_of(info.sv_dtype);
	if (sample_bytes == 0) {
		return false;
	}

	const auto rows = static_cast<std::size_t>(info.rows);
	const auto cols = static_cast<std::size_t>(info.cols);
	const auto samples = static_cast<std::size_t>(info.samples_per_pixel);
	const bool source_planar =
	    info.planar_configuration == Planar::planar && samples > std::size_t{1};
	const bool output_planar =
	    plan.options.planar_out == Planar::planar && samples > std::size_t{1};

	// Raw host-copy fast path only handles layout-equivalent requests.
	if (samples > 1 && source_planar != output_planar) {
		return false;
	}

	const auto computed_source =
	    prepare_runtime_frame_source_or_throw(df, info, info.ts, frame_index);
	const auto src = computed_source.bytes;
	if (src.empty()) {
		return false;
	}

	const std::size_t row_payload =
	    cols * (source_planar ? std::size_t{1} : samples) * sample_bytes;
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

[[nodiscard]] bool try_decode_frame_with_builtin_core_direct(const DicomFile& df,
    std::size_t frame_index, std::span<std::uint8_t> dst, const DecodePlan& plan) {
	const auto& info = plan.info;
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

	const auto computed_source =
	    prepare_runtime_frame_source_or_throw(df, info, info.ts, frame_index);
	pixel_decoder_request_v2 request{};
	populate_direct_decode_request(
	    request, codec_profile_code, info, computed_source.bytes, dst, plan);
	if (request.frame.source_dtype == PIXEL_DTYPE_UNKNOWN_V2) {
		return false;
	}

	::pixel::core_v2::ErrorState error_state{};
	const pixel_error_code_v2 decode_ec =
	    ::pixel::core_v2::decode_uncompressed_frame(&error_state, &request);
	if (decode_ec != PIXEL_CODEC_ERR_OK) {
		throw_runtime_decode_error(df.path(), info.ts, frame_index, decode_ec,
		    copy_core_error_detail(error_state));
	}
	return true;
}

[[nodiscard]] bool try_decode_frame_with_builtin_direct(const DicomFile& df,
    std::size_t frame_index, std::span<std::uint8_t> dst, const DecodePlan& plan) {
	if (try_decode_frame_with_builtin_uncompressed_host_copy(
	        df, frame_index, dst, plan)) {
		return true;
	}
	if (try_decode_frame_with_builtin_core_direct(df, frame_index, dst, plan)) {
		return true;
	}
#if defined(DICOMSDL_PIXEL_RLE_STATIC_PLUGIN_ENABLED) && DICOMSDL_PIXEL_RLE_STATIC_PLUGIN_ENABLED
	if (try_decode_frame_with_builtin_rle_direct(df, frame_index, dst, plan)) {
		return true;
	}
	#endif
	return false;
}

}  // namespace
#endif

// Dispatch one frame decode through the runtime path or fail with a stable unsupported error.
void dispatch_decode_frame(const DicomFile& df, std::size_t frame_index,
    std::span<std::uint8_t> dst, const DecodePlan& plan) {
	validate_decode_plan_or_throw(df, frame_index, plan);
#if defined(DICOMSDL_PIXEL_RUNTIME_ENABLED)
	if (try_decode_frame_with_builtin_direct(df, frame_index, dst, plan)) {
		return;
	}
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
