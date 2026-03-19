#include "pixel/host/encode/encode_set_pixel_data_runner.hpp"

#include "pixel/host/adapter/host_adapter.hpp"
#include "pixel/host/support/dicom_pixel_support.hpp"
#include "pixel/host/encode/encode_metadata_updater.hpp"
#include "pixel/host/encode/encode_target_policy.hpp"
#include "pixel/host/encode/multicomponent_transform_policy.hpp"
#include "pixel/host/error/codec_error.hpp"

#if defined(DICOMSDL_PIXEL_RUNTIME_ENABLED)
#include "pixel/runtime/runtime_registry.hpp"
#endif

#if defined(DICOMSDL_PIXEL_RUNTIME_ENABLED)
#include <charls/charls.h>

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#include <fmt/format.h>
#endif

#include <cstddef>
#include <optional>

namespace dicom::pixel::detail {

namespace {

[[nodiscard]] uint32_t encode_codec_profile_code_from_transfer_syntax_or_throw(
    std::string_view file_path, uid::WellKnown transfer_syntax) {
	uint32_t codec_profile_code = PIXEL_CODEC_PROFILE_UNKNOWN;
	if (::pixel::runtime::codec_profile_code_from_transfer_syntax(
	        transfer_syntax, &codec_profile_code)) {
		return codec_profile_code;
	}

	throw_codec_error_with_context("DicomFile::set_pixel_data", file_path,
	    transfer_syntax, std::nullopt,
	    CodecError{
	        .code = CodecStatusCode::unsupported,
	        .stage = "plugin_lookup",
	        .detail = "transfer syntax is not mapped to a runtime codec profile",
	    });
}

} // namespace

#if defined(DICOMSDL_PIXEL_RUNTIME_ENABLED)

namespace {

constexpr std::size_t kMaxOptionCount = 64;
constexpr std::size_t kMaxOptionKeyBytes = 128;
constexpr std::size_t kMaxOptionValueBytes = 1024;
class EncoderContextGuard {
public:
	explicit EncoderContextGuard(
	    ::pixel::runtime::HostEncoderContext* ctx) noexcept
	    : ctx_(ctx) {}
	EncoderContextGuard(const EncoderContextGuard&) = delete;
	EncoderContextGuard& operator=(const EncoderContextGuard&) = delete;
	~EncoderContextGuard() {
		::pixel::runtime::destroy_host_encoder_context(ctx_);
	}

private:
	::pixel::runtime::HostEncoderContext* ctx_{nullptr};
};

struct RuntimeOptionListStorage {
	std::vector<std::string> keys{};
	std::vector<std::string> values{};
	std::vector<pixel_option_kv> items{};
	pixel_option_list list{};
};

struct EncapsulatedEncodeInput {
	const std::uint8_t* source_base{nullptr};
	std::size_t frame_count{0};
	std::size_t source_frame_stride{0};
	std::size_t source_frame_size_bytes{0};
	bool source_aliases_current_native_pixel_data{false};
};

[[nodiscard]] std::size_t saturating_add_size_t(
    std::size_t lhs, std::size_t rhs) noexcept {
	const auto max_value = (std::numeric_limits<std::size_t>::max)();
	if (lhs > max_value - rhs) {
		return max_value;
	}
	return lhs + rhs;
}

[[nodiscard]] bool checked_add_size_t(
    std::size_t lhs, std::size_t rhs, std::size_t* out) noexcept {
	if (out == nullptr) {
		return false;
	}
	const auto max_value = (std::numeric_limits<std::size_t>::max)();
	if (lhs > max_value - rhs) {
		return false;
	}
	*out = lhs + rhs;
	return true;
}

[[nodiscard]] bool checked_mul_size_t(
    std::size_t lhs, std::size_t rhs, std::size_t* out) noexcept {
	if (out == nullptr) {
		return false;
	}
	const auto max_value = (std::numeric_limits<std::size_t>::max)();
	if (lhs != 0 && rhs > max_value / lhs) {
		return false;
	}
	*out = lhs * rhs;
	return true;
}

[[nodiscard]] std::size_t with_encode_headroom(std::size_t size) noexcept {
	const auto fractional = size / std::size_t{8};
	const auto minimum = std::size_t{64} * 1024u;
	return saturating_add_size_t(size, (std::max)(fractional, minimum));
}

[[nodiscard]] std::optional<std::size_t> estimate_jpegls_encoded_capacity(
    const pixel::PixelLayout& source_layout) {
	const auto bytes_per_sample = bytes_per_sample_of(source_layout.data_type);
	if (bytes_per_sample == 0 || source_layout.empty()) {
		return std::nullopt;
	}

	const auto rows = static_cast<std::size_t>(source_layout.rows);
	const auto cols = static_cast<std::size_t>(source_layout.cols);
	const auto samples = static_cast<std::size_t>(source_layout.samples_per_pixel);
	if (rows > static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)()) ||
	    cols > static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)()) ||
	    samples > static_cast<std::size_t>((std::numeric_limits<std::int32_t>::max)())) {
		return std::nullopt;
	}

	const auto bits_stored =
	    source_layout.bits_stored > 0 ? static_cast<int>(source_layout.bits_stored)
	                                  : static_cast<int>(bytes_per_sample * 8u);
	if (bits_stored <= 0 ||
	    bits_stored > static_cast<int>(bytes_per_sample * 8u)) {
		return std::nullopt;
	}

	try {
		charls::jpegls_encoder encoder{};
		encoder.frame_info(charls::frame_info{
		    static_cast<std::uint32_t>(cols),
		    static_cast<std::uint32_t>(rows),
		    bits_stored,
		    static_cast<int32_t>(samples),
		});
		if (samples > 1) {
			encoder.interleave_mode(source_layout.planar == pixel::Planar::interleaved
			        ? charls::interleave_mode::sample
			        : charls::interleave_mode::none);
		}
		return encoder.estimated_destination_size();
	} catch (...) {
		return std::nullopt;
	}
}

[[nodiscard]] std::optional<std::size_t> estimate_rle_encoded_capacity(
    const pixel::PixelLayout& source_layout) noexcept {
	const auto bytes_per_sample = bytes_per_sample_of(source_layout.data_type);
	if (bytes_per_sample == 0 || source_layout.empty()) {
		return std::nullopt;
	}

	const auto rows = static_cast<std::size_t>(source_layout.rows);
	const auto cols = static_cast<std::size_t>(source_layout.cols);
	const auto samples = static_cast<std::size_t>(source_layout.samples_per_pixel);

	std::size_t segment_count = 0;
	if (!checked_mul_size_t(samples, bytes_per_sample, &segment_count) ||
	    segment_count == 0 || segment_count > 15u) {
		return std::nullopt;
	}

	std::size_t pixels = 0;
	if (!checked_mul_size_t(rows, cols, &pixels)) {
		return std::nullopt;
	}

	// Mirror the codec's own PackBits reserve rule: payload + payload/128 + slack.
	std::size_t segment_upper_bound = 0;
	if (!checked_add_size_t(pixels, pixels / 128u, &segment_upper_bound) ||
	    !checked_add_size_t(segment_upper_bound, 16u, &segment_upper_bound)) {
		return std::nullopt;
	}

	std::size_t segment_bytes_upper_bound = 0;
	if (!checked_mul_size_t(segment_count, segment_upper_bound,
	        &segment_bytes_upper_bound)) {
		return std::nullopt;
	}

	std::size_t total_upper_bound = 0;
	if (!checked_add_size_t(std::size_t{64}, segment_bytes_upper_bound,
	        &total_upper_bound)) {
		return std::nullopt;
	}
	return total_upper_bound;
}

[[nodiscard]] std::optional<std::size_t> estimate_profile_specific_encoded_capacity(
    uint32_t codec_profile_code, const pixel::PixelLayout& source_layout) {
	switch (codec_profile_code) {
	case PIXEL_CODEC_PROFILE_JPEGLS_LOSSLESS:
	case PIXEL_CODEC_PROFILE_JPEGLS_NEAR_LOSSLESS:
		return estimate_jpegls_encoded_capacity(source_layout);
	case PIXEL_CODEC_PROFILE_RLE_LOSSLESS:
		return estimate_rle_encoded_capacity(source_layout);
	default:
		return std::nullopt;
	}
}

[[nodiscard]] std::size_t estimate_initial_encoded_capacity(
    uint32_t codec_profile_code, const pixel::PixelLayout& source_layout,
    std::span<const std::uint8_t> source_frame,
    std::optional<std::size_t> previous_hint) {
	std::size_t capacity = with_encode_headroom(source_frame.size());
	if (const auto profile_specific_capacity =
	        estimate_profile_specific_encoded_capacity(codec_profile_code, source_layout);
	    profile_specific_capacity && *profile_specific_capacity > capacity) {
		capacity = *profile_specific_capacity;
	}
	if (previous_hint && *previous_hint > capacity) {
		capacity = *previous_hint;
	}
	return capacity;
}

[[nodiscard]] std::size_t encoded_size_to_size_t_or_throw(
    std::string_view file_path, uid::WellKnown transfer_syntax,
    std::size_t frame_index, uint64_t encoded_size_u64) {
	const auto max_size_u64 =
	    static_cast<uint64_t>((std::numeric_limits<std::size_t>::max)());
	if (encoded_size_u64 > max_size_u64) {
		throw_codec_error_with_context("DicomFile::set_pixel_data", file_path,
		    transfer_syntax, frame_index,
		    CodecError{
		        .code = CodecStatusCode::internal_error,
		        .stage = "encode_frame",
		        .detail = "encoded size exceeds size_t range",
		    });
	}
	return static_cast<std::size_t>(encoded_size_u64);
}

[[nodiscard]] std::size_t required_source_bytes_for_encode_input_or_throw(
    std::string_view file_path, uid::WellKnown transfer_syntax,
    const EncapsulatedEncodeInput& encode_input) {
	if (encode_input.frame_count == 0) {
		return 0;
	}
	const auto max_value = (std::numeric_limits<std::size_t>::max)();
	const auto last_frame_index = encode_input.frame_count - std::size_t{1};
	if (encode_input.source_frame_stride != 0 &&
	    last_frame_index > max_value / encode_input.source_frame_stride) {
		throw_codec_error_with_context("DicomFile::set_pixel_data", file_path,
		    transfer_syntax, std::nullopt,
		    CodecError{
		        .code = CodecStatusCode::internal_error,
		        .stage = "copy_source",
		        .detail = "source frame offsets overflow size_t",
		    });
	}
	const auto last_frame_begin =
	    last_frame_index * encode_input.source_frame_stride;
	if (last_frame_begin > max_value - encode_input.source_frame_size_bytes) {
		throw_codec_error_with_context("DicomFile::set_pixel_data", file_path,
		    transfer_syntax, std::nullopt,
		    CodecError{
		        .code = CodecStatusCode::internal_error,
		        .stage = "copy_source",
		        .detail = "source copy size overflows size_t",
		    });
	}
	return last_frame_begin + encode_input.source_frame_size_bytes;
}

[[nodiscard]] const ::pixel::runtime::BindingRegistry* get_runtime_registry() {
	return ::pixel::runtime::current_registry();
}

[[nodiscard]] CodecStatusCode map_runtime_error_code(pixel_error_code ec) noexcept {
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
		out_stage = "encode_frame";
	}
	if (out_reason.empty()) {
		out_reason.assign(raw_detail);
	}
	if (out_reason.empty()) {
		out_reason = "encoder runtime host adapter failed";
	}
}

[[nodiscard]] std::string copy_encoder_error_detail(
    const ::pixel::runtime::HostEncoderContext& ctx) {
	std::array<char, 1024> buffer{};
	const auto copied = ::pixel::runtime::copy_host_encoder_last_error_detail(
	    &ctx, buffer.data(), static_cast<uint32_t>(buffer.size()));
	if (copied == 0) {
		return {};
	}
	return std::string(buffer.data(), buffer.data() + copied);
}

[[noreturn]] void throw_runtime_encode_error(
    std::string_view file_path, uid::WellKnown transfer_syntax,
    std::optional<std::size_t> frame_index, pixel_error_code ec,
    std::string_view raw_detail) {
	CodecError encode_error{};
	encode_error.code = map_runtime_error_code(ec);
	parse_runtime_detail_or_default(raw_detail, encode_error.stage, encode_error.detail);
	throw_codec_error_with_context("DicomFile::set_pixel_data", file_path,
	    transfer_syntax, frame_index, encode_error);
}

[[nodiscard]] bool should_skip_option_for_profile(
    uint32_t codec_profile_code, std::string_view key) noexcept {
	switch (codec_profile_code) {
	case PIXEL_CODEC_PROFILE_JPEG2000_LOSSLESS:
	case PIXEL_CODEC_PROFILE_JPEG2000_LOSSY:
		return key == "color_transform";
	case PIXEL_CODEC_PROFILE_HTJ2K_LOSSLESS:
	case PIXEL_CODEC_PROFILE_HTJ2K_LOSSLESS_RPCL:
	case PIXEL_CODEC_PROFILE_HTJ2K_LOSSY:
		return key == "color_transform" || key == "threads";
	default:
		return false;
	}
}

bool build_runtime_option_list_from_pairs(std::span<const CodecOptionKv> option_pairs,
    uint32_t codec_profile_code, RuntimeOptionListStorage& out_storage,
    CodecError& out_error) noexcept {
	out_storage = {};
	out_error = {};

	if (option_pairs.size() >
	    static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())) {
		set_codec_error(out_error, CodecStatusCode::internal_error, "parse_options",
		    "option count exceeds uint32 range");
		return false;
	}
	if (option_pairs.size() > kMaxOptionCount) {
		set_codec_error(out_error, CodecStatusCode::invalid_argument, "parse_options",
		    fmt::format(
		        "option count exceeds max_option_count (count={}, max={})",
		        option_pairs.size(), kMaxOptionCount));
		return false;
	}

	out_storage.keys.reserve(option_pairs.size());
	out_storage.values.reserve(option_pairs.size());
	out_storage.items.reserve(option_pairs.size());
	for (const auto& pair : option_pairs) {
		if (should_skip_option_for_profile(codec_profile_code, pair.key)) {
			continue;
		}
		if (pair.key.empty()) {
			set_codec_error(out_error, CodecStatusCode::invalid_argument, "parse_options",
			    "empty option key is not allowed");
			return false;
		}
		if (pair.key.size() > kMaxOptionKeyBytes) {
			set_codec_error(out_error, CodecStatusCode::invalid_argument, "parse_options",
			    fmt::format(
			        "option key exceeds max_option_key_bytes (key='{}', size={}, max={})",
			        pair.key, pair.key.size(), kMaxOptionKeyBytes));
			return false;
		}

		out_storage.keys.emplace_back(pair.key);
		std::string value{};
		std::visit([&value](const auto& option_value) {
			using value_type = std::decay_t<decltype(option_value)>;
			if constexpr (std::is_same_v<value_type, std::int64_t>) {
				value = std::to_string(option_value);
			} else if constexpr (std::is_same_v<value_type, double>) {
				value = fmt::format("{:.17g}", option_value);
			} else if constexpr (std::is_same_v<value_type, std::string>) {
				value = option_value;
			} else {
				value = option_value ? "true" : "false";
			}
		}, pair.value);
		if (value.size() > kMaxOptionValueBytes) {
			set_codec_error(out_error, CodecStatusCode::invalid_argument, "parse_options",
			    fmt::format(
			        "option value exceeds max_option_value_bytes "
			        "(key='{}', size={}, max={})",
			        pair.key, value.size(), kMaxOptionValueBytes));
			return false;
		}
		out_storage.values.push_back(std::move(value));
	}

	for (std::size_t i = 0; i < out_storage.keys.size(); ++i) {
		out_storage.items.push_back(pixel_option_kv{
		    out_storage.keys[i].c_str(), out_storage.values[i].c_str()});
	}
	out_storage.list.items =
	    out_storage.items.empty() ? nullptr : out_storage.items.data();
	out_storage.list.count = static_cast<uint32_t>(out_storage.items.size());
	return true;
}

[[nodiscard]] std::vector<std::uint8_t> encode_frame_with_runtime_or_throw(
    ::pixel::runtime::HostEncoderContext& ctx, std::string_view file_path,
    uid::WellKnown transfer_syntax, uint32_t codec_profile_code,
    const pixel::PixelLayout& source_layout,
    std::span<const std::uint8_t> source_frame,
    bool use_multicomponent_transform, std::size_t frame_index,
    std::size_t* capacity_hint) {
	// Rebuild the single-frame layout once so the runtime adapter receives the
	// exact storage contract for the current source span.
	const auto frame_layout = source_layout.single_frame();

	if (::pixel::runtime::host_encoder_context_supports_context_buffer(&ctx)) {
		pixel_const_buffer encoded_view{};
		const pixel_error_code encode_ec =
		    ::pixel::runtime::encode_frame_to_context_buffer_with_host_context(
		        &ctx, &frame_layout, source_frame, use_multicomponent_transform,
		        &encoded_view);
		if (encode_ec != PIXEL_CODEC_ERR_OK) {
			throw_runtime_encode_error(file_path, transfer_syntax, frame_index,
			    encode_ec, copy_encoder_error_detail(ctx));
		}
		const auto encoded_size = encoded_size_to_size_t_or_throw(
		    file_path, transfer_syntax, frame_index, encoded_view.size);
		std::vector<std::uint8_t> encoded_frame(
		    encoded_view.data, encoded_view.data + encoded_size);
		if (capacity_hint != nullptr) {
			*capacity_hint = with_encode_headroom(encoded_frame.size());
		}
		return encoded_frame;
	}

	const std::optional<std::size_t> hint =
	    (capacity_hint != nullptr && *capacity_hint != 0) ? std::optional<std::size_t>(*capacity_hint)
	                                                      : std::nullopt;
	std::vector<std::uint8_t> encoded_frame(
	    estimate_initial_encoded_capacity(codec_profile_code, source_layout,
	        source_frame, hint));
	uint64_t actual_size = static_cast<uint64_t>(encoded_frame.size());
	pixel_error_code encode_ec = ::pixel::runtime::encode_frame_with_host_context(
	    &ctx, &frame_layout, source_frame, use_multicomponent_transform,
	    pixel_output_buffer{
	        encoded_frame.empty() ? nullptr : encoded_frame.data(),
	        static_cast<uint64_t>(encoded_frame.size()),
	    },
	    &actual_size);
	if (encode_ec == PIXEL_CODEC_ERR_OUTPUT_TOO_SMALL) {
		const auto required_size =
		    encoded_size_to_size_t_or_throw(file_path, transfer_syntax,
		        frame_index, actual_size);
		encoded_frame.resize(required_size);
		actual_size = static_cast<uint64_t>(encoded_frame.size());
		encode_ec = ::pixel::runtime::encode_frame_with_host_context(
		    &ctx, &frame_layout, source_frame, use_multicomponent_transform,
		    pixel_output_buffer{
		        encoded_frame.empty() ? nullptr : encoded_frame.data(),
		        static_cast<uint64_t>(encoded_frame.size()),
		    },
		    &actual_size);
	}
	if (encode_ec != PIXEL_CODEC_ERR_OK) {
		throw_runtime_encode_error(file_path, transfer_syntax, frame_index,
		    encode_ec, copy_encoder_error_detail(ctx));
	}
	if (actual_size > static_cast<uint64_t>(encoded_frame.size())) {
		throw_codec_error_with_context("DicomFile::set_pixel_data", file_path,
		    transfer_syntax, frame_index,
		    CodecError{
		        .code = CodecStatusCode::internal_error,
		        .stage = "encode_frame",
		        .detail = "encoded size grew beyond allocated output buffer",
		    });
	}
	encoded_frame.resize(static_cast<std::size_t>(actual_size));
	if (capacity_hint != nullptr) {
		*capacity_hint = with_encode_headroom(encoded_frame.size());
	}
	return encoded_frame;
}

template <typename FrameProvider, typename FrameSink>
void encode_frames_with_runtime_or_throw_impl(DicomFile& file,
    uid::WellKnown transfer_syntax, const pixel::PixelLayout& source_layout,
    uint32_t codec_profile_code, std::span<const CodecOptionKv> codec_options,
    bool use_multicomponent_transform, std::size_t frame_count,
    FrameProvider&& frame_provider, FrameSink&& frame_sink) {
	const auto* registry = get_runtime_registry();
	if (registry == nullptr) {
		CodecError encode_error{};
		encode_error.code = CodecStatusCode::unsupported;
		encode_error.stage = "plugin_lookup";
		encode_error.detail = "runtime registry is not available";
		throw_codec_error_with_context("DicomFile::set_pixel_data", file.path(),
		    transfer_syntax, std::nullopt, encode_error);
	}

	CodecError option_error{};
	RuntimeOptionListStorage option_storage{};
	if (!build_runtime_option_list_from_pairs(
	        codec_options, codec_profile_code, option_storage, option_error)) {
		throw_codec_error_with_context("DicomFile::set_pixel_data", file.path(),
		    transfer_syntax, std::nullopt, option_error);
	}
	const pixel_option_list* option_ptr =
	    option_storage.list.count == 0 ? nullptr : &option_storage.list;

	::pixel::runtime::HostEncoderContext encoder_ctx{};
	const EncoderContextGuard guard(&encoder_ctx);
	const pixel_error_code configure_ec =
	    ::pixel::runtime::configure_host_encoder_context(
	        &encoder_ctx, registry, transfer_syntax, option_ptr);
	const std::string configure_detail = copy_encoder_error_detail(encoder_ctx);
	if (configure_ec == PIXEL_CODEC_ERR_UNSUPPORTED) {
		throw_codec_error_with_context("DicomFile::set_pixel_data", file.path(),
		    transfer_syntax, std::nullopt,
		    CodecError{
		        .code = CodecStatusCode::unsupported,
		        .stage = "plugin_lookup",
		        .detail = "encoder binding is not registered in runtime registry",
		    });
	}
	if (configure_ec != PIXEL_CODEC_ERR_OK) {
		throw_runtime_encode_error(
		    file.path(), transfer_syntax, std::nullopt, configure_ec, configure_detail);
	}

	std::size_t encoded_capacity_hint = 0;
	for (std::size_t frame_index = 0; frame_index < frame_count; ++frame_index) {
		auto encoded_frame = encode_frame_with_runtime_or_throw(encoder_ctx,
		    file.path(), transfer_syntax, codec_profile_code, source_layout,
		    frame_provider(frame_index), use_multicomponent_transform,
		    frame_index, &encoded_capacity_hint);
		frame_sink(frame_index, std::move(encoded_frame));
	}
}

[[nodiscard]] bool encode_encapsulated_pixel_data_with_runtime_or_throw(DicomFile& file,
    uid::WellKnown transfer_syntax, const pixel::PixelLayout& source_layout,
    const EncapsulatedEncodeInput& encode_input, uint32_t codec_profile_code,
    std::span<const CodecOptionKv> codec_options, bool use_multicomponent_transform) {
	std::vector<std::uint8_t> source_storage{};
	const std::uint8_t* source_base = encode_input.source_base;
	if (encode_input.source_aliases_current_native_pixel_data) {
		const auto source_bytes_to_copy =
		    required_source_bytes_for_encode_input_or_throw(
		        file.path(), transfer_syntax, encode_input);
		source_storage.assign(
		    encode_input.source_base,
		    encode_input.source_base + source_bytes_to_copy);
		source_base = source_storage.data();
	}

	file.reset_encapsulated_pixel_data(encode_input.frame_count);
	encode_frames_with_runtime_or_throw_impl(file, transfer_syntax, source_layout,
	    codec_profile_code, codec_options, use_multicomponent_transform,
	    encode_input.frame_count,
	    [&](std::size_t frame_index) {
		    const auto* source_frame =
		        source_base + frame_index * encode_input.source_frame_stride;
		    return std::span<const std::uint8_t>(
		        source_frame, encode_input.source_frame_size_bytes);
	    },
	    [&](std::size_t frame_index, std::vector<std::uint8_t>&& encoded_frame) {
		    file.set_encoded_pixel_frame(frame_index, std::move(encoded_frame));
	    });
	return true;
}

}  // namespace

void encode_frames_from_frame_provider_with_runtime_or_throw(
    DicomFile& file, uid::WellKnown transfer_syntax,
    const pixel::PixelLayout& source_layout, uint32_t codec_profile_code,
    std::span<const CodecOptionKv> codec_options,
    bool use_multicomponent_transform, std::size_t frame_count,
    const std::function<std::span<const std::uint8_t>(std::size_t)>& frame_provider,
    const std::function<void(std::size_t, std::vector<std::uint8_t>&&)>& frame_sink) {
	encode_frames_with_runtime_or_throw_impl(file, transfer_syntax, source_layout,
	    codec_profile_code, codec_options, use_multicomponent_transform,
	    frame_count, frame_provider, frame_sink);
}
#endif

void run_set_pixel_data_with_computed_codec_options(DicomFile& file,
    uid::WellKnown transfer_syntax, pixel::ConstPixelSpan source,
    std::span<const CodecOptionKv> codec_options) {
	const auto file_path = file.path();
	const auto codec_profile_code =
	    encode_codec_profile_code_from_transfer_syntax_or_throw(
	        file_path, transfer_syntax);
	const auto source_layout =
	    support_detail::compute_encode_source_layout_or_throw(source, file_path);
	validate_encode_profile_source_constraints(codec_profile_code,
	    source_layout.bits_allocated, source_layout.bits_stored, file_path);
	const bool use_multicomponent_transform =
	    should_use_multicomponent_transform(transfer_syntax, codec_profile_code,
	        codec_options, source_layout.samples_per_pixel,
	        file_path);
	const pixel::Photometric output_photometric =
	    compute_output_photometric_for_encode_profile(codec_profile_code,
	        use_multicomponent_transform, source.layout.photometric);

	auto& dataset = file.dataset();
	dataset.ensure_loaded(Tag(0xFFFFu, 0xFFFFu));
	update_pixel_metadata_for_set_pixel_data(dataset, file_path, transfer_syntax,
	    source.layout,
	    is_rle_encode_profile(codec_profile_code), output_photometric,
	    source_layout.bits_allocated,
	    source_layout.bits_stored, source_layout.high_bit,
	    source_layout.pixel_representation, source_layout.source_row_stride,
	    source_layout.source_frame_stride);

	// set_transfer_syntax(native->encapsulated) can pass a span that aliases current
	// native PixelData bytes. We must preserve those bytes until all frames are encoded.
	const bool source_aliases_current_native_pixel_data =
	    support_detail::source_aliases_native_pixel_data(dataset, source.bytes);
	const EncapsulatedEncodeInput encapsulated_encode_input{
	    .source_base = source.bytes.data(),
	    .frame_count = source_layout.frames,
	    .source_frame_stride = source_layout.source_frame_stride,
	    .source_frame_size_bytes = source_layout.source_frame_size_bytes,
	    .source_aliases_current_native_pixel_data = source_aliases_current_native_pixel_data,
	};

	if (is_native_uncompressed_encode_profile(codec_profile_code)) {
		// Native targets only need row/frame copies from the declared source span.
		const support_detail::NativePixelCopyInput native_copy_input{
		    .source_bytes = source.bytes,
		    .rows = source_layout.rows,
		    .frames = source_layout.frames,
		    .samples_per_pixel = source_layout.samples_per_pixel,
		    .planar_source = source_layout.planar_source,
		    .row_payload_bytes = source_layout.row_payload_bytes,
		    .source_row_stride = source_layout.source_row_stride,
		    .source_plane_stride = source_layout.source_plane_stride,
		    .source_frame_stride = source_layout.source_frame_stride,
		    .destination_frame_payload = source_layout.destination_frame_payload,
		    .destination_total_bytes = source_layout.destination_total_bytes,
		};
		auto native_pixel_data = support_detail::build_native_pixel_payload(
		    native_copy_input);
		file.set_native_pixel_data(std::move(native_pixel_data));
	} else {
#if defined(DICOMSDL_PIXEL_RUNTIME_ENABLED)
		// Encapsulated targets still route through the runtime encoder bridge, which
		// now consumes normalized PixelLayout metadata plus frame byte views.
		if (!encode_encapsulated_pixel_data_with_runtime_or_throw(file, transfer_syntax,
		        source.layout, encapsulated_encode_input, codec_profile_code, codec_options,
		        use_multicomponent_transform)) {
			CodecError encode_error{};
			encode_error.code = CodecStatusCode::unsupported;
			encode_error.stage = "plugin_lookup";
			encode_error.detail = "runtime registry is not available";
			throw_codec_error_with_context("DicomFile::set_pixel_data", file_path,
			    transfer_syntax, std::nullopt, encode_error);
		}
#else
		CodecError encode_error{};
		encode_error.code = CodecStatusCode::unsupported;
		encode_error.stage = "plugin_lookup";
		encode_error.detail = "runtime is disabled at build time";
		throw_codec_error_with_context("DicomFile::set_pixel_data", file_path,
		    transfer_syntax, std::nullopt, encode_error);
#endif
	}

	const auto encoded_payload_bytes =
	    encode_profile_uses_lossy_compression(codec_profile_code)
	    ? encoded_payload_size_from_pixel_sequence(dataset, file_path, transfer_syntax)
	    : std::size_t{0};
	update_lossy_compression_metadata_for_set_pixel_data(dataset, file_path,
	    transfer_syntax, codec_profile_code, source_layout.destination_total_bytes,
	    encoded_payload_bytes);
}

void run_set_pixel_data_from_frame_provider_with_computed_codec_options_impl(
    DicomFile& file, uid::WellKnown transfer_syntax,
    const pixel::PixelLayout& source_layout,
    std::span<const CodecOptionKv> codec_options,
    const std::function<std::span<const std::uint8_t>(std::size_t)>& frame_provider,
    bool stream_output_frames) {
	const auto file_path = file.path();
	const auto codec_profile_code =
	    encode_codec_profile_code_from_transfer_syntax_or_throw(
	        file_path, transfer_syntax);
	const auto encode_source_layout =
	    support_detail::compute_encode_source_layout_without_source_bytes_or_throw(
	        source_layout, file_path);
	validate_encode_profile_source_constraints(codec_profile_code,
	    encode_source_layout.bits_allocated, encode_source_layout.bits_stored,
	    file_path);
	const bool use_multicomponent_transform =
	    should_use_multicomponent_transform(transfer_syntax, codec_profile_code,
	        codec_options, encode_source_layout.samples_per_pixel,
	        file_path);
	const pixel::Photometric output_photometric =
	    compute_output_photometric_for_encode_profile(codec_profile_code,
	        use_multicomponent_transform, source_layout.photometric);

	auto& dataset = file.dataset();
	dataset.ensure_loaded(Tag(0xFFFFu, 0xFFFFu));
	update_pixel_metadata_for_set_pixel_data(dataset, file_path, transfer_syntax,
	    source_layout,
	    is_rle_encode_profile(codec_profile_code),
	    output_photometric, encode_source_layout.bits_allocated,
	    encode_source_layout.bits_stored, encode_source_layout.high_bit,
	    encode_source_layout.pixel_representation,
	    encode_source_layout.source_row_stride,
	    encode_source_layout.source_frame_stride);

	if (is_native_uncompressed_encode_profile(codec_profile_code)) {
		throw_codec_error_with_context("DicomFile::set_pixel_data", file_path,
		    transfer_syntax, std::nullopt,
		    CodecError{
		        .code = CodecStatusCode::invalid_argument,
		        .stage = "validate_target",
		        .detail = "frame provider path requires encapsulated target transfer syntax",
		    });
	}

#if defined(DICOMSDL_PIXEL_RUNTIME_ENABLED)
	if (stream_output_frames) {
		file.reset_encapsulated_pixel_data(encode_source_layout.frames);
		encode_frames_with_runtime_or_throw_impl(file, transfer_syntax, source_layout,
		    codec_profile_code, codec_options, use_multicomponent_transform,
		    encode_source_layout.frames, frame_provider,
		    [&](std::size_t frame_index, std::vector<std::uint8_t>&& encoded_frame) {
			    file.set_encoded_pixel_frame(frame_index, std::move(encoded_frame));
		    });
	} else {
		std::vector<std::vector<std::uint8_t>> encoded_frames;
		encoded_frames.reserve(encode_source_layout.frames);
		encode_frames_with_runtime_or_throw_impl(file, transfer_syntax, source_layout,
		    codec_profile_code, codec_options, use_multicomponent_transform,
		    encode_source_layout.frames, frame_provider,
		    [&](std::size_t, std::vector<std::uint8_t>&& encoded_frame) {
			    encoded_frames.push_back(std::move(encoded_frame));
		    });

		file.reset_encapsulated_pixel_data(encode_source_layout.frames);
		for (std::size_t frame_index = 0; frame_index < encoded_frames.size();
		     ++frame_index) {
			file.set_encoded_pixel_frame(
			    frame_index, std::move(encoded_frames[frame_index]));
		}
	}
#else
	CodecError encode_error{};
	encode_error.code = CodecStatusCode::unsupported;
	encode_error.stage = "plugin_lookup";
	encode_error.detail = "runtime is disabled at build time";
	throw_codec_error_with_context("DicomFile::set_pixel_data", file_path,
	    transfer_syntax, std::nullopt, encode_error);
#endif

	const auto encoded_payload_bytes =
	    encode_profile_uses_lossy_compression(codec_profile_code)
	    ? encoded_payload_size_from_pixel_sequence(dataset, file_path, transfer_syntax)
	    : std::size_t{0};
	update_lossy_compression_metadata_for_set_pixel_data(dataset, file_path,
	    transfer_syntax, codec_profile_code,
	    encode_source_layout.destination_total_bytes,
	    encoded_payload_bytes);
}

void run_set_pixel_data_from_frame_provider_with_computed_codec_options(
    DicomFile& file, uid::WellKnown transfer_syntax,
    const pixel::PixelLayout& source_layout,
    std::span<const CodecOptionKv> codec_options,
    const std::function<std::span<const std::uint8_t>(std::size_t)>& frame_provider) {
	run_set_pixel_data_from_frame_provider_with_computed_codec_options_impl(
	    file, transfer_syntax, source_layout, codec_options, frame_provider,
	    false);
}

void run_set_pixel_data_from_frame_provider_streaming_with_computed_codec_options(
    DicomFile& file, uid::WellKnown transfer_syntax,
    const pixel::PixelLayout& source_layout,
    std::span<const CodecOptionKv> codec_options,
    const std::function<std::span<const std::uint8_t>(std::size_t)>& frame_provider) {
	run_set_pixel_data_from_frame_provider_with_computed_codec_options_impl(
	    file, transfer_syntax, source_layout, codec_options, frame_provider,
	    true);
}

} // namespace dicom::pixel::detail

