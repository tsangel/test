#include "pixel/host/encode/encode_set_pixel_data_runner.hpp"

#include "pixel/host/adapter/host_adapter_v2.hpp"
#include "pixel/host/support/dicom_pixel_support.hpp"
#include "pixel/host/encode/encode_metadata_updater.hpp"
#include "pixel/host/encode/encode_target_policy.hpp"
#include "pixel/host/encode/multicomponent_transform_policy.hpp"
#include "pixel/host/error/codec_error.hpp"

#if defined(DICOMSDL_PIXEL_RUNTIME_ENABLED)
#include "pixel/runtime/runtime_registry_v2.hpp"
#endif

#if defined(DICOMSDL_PIXEL_RUNTIME_ENABLED)
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
	uint32_t codec_profile_code = PIXEL_CODEC_PROFILE_UNKNOWN_V2;
	if (::pixel::runtime_v2::codec_profile_code_from_transfer_syntax(
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
	    ::pixel::runtime_v2::HostEncoderContextV2* ctx) noexcept
	    : ctx_(ctx) {}
	EncoderContextGuard(const EncoderContextGuard&) = delete;
	EncoderContextGuard& operator=(const EncoderContextGuard&) = delete;
	~EncoderContextGuard() {
		::pixel::runtime_v2::destroy_host_encoder_context_v2(ctx_);
	}

private:
	::pixel::runtime_v2::HostEncoderContextV2* ctx_{nullptr};
};

struct RuntimeOptionListStorage {
	std::vector<std::string> keys{};
	std::vector<std::string> values{};
	std::vector<pixel_option_kv_v2> items{};
	pixel_option_list_v2 list{};
};

struct EncapsulatedEncodeInput {
	const std::uint8_t* source_base{nullptr};
	std::size_t frame_count{0};
	std::size_t source_frame_stride{0};
	std::size_t source_frame_size_bytes{0};
	bool source_aliases_current_native_pixel_data{false};
};

[[nodiscard]] const ::pixel::runtime_v2::BindingRegistryV2* get_runtime_registry() {
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
    const ::pixel::runtime_v2::HostEncoderContextV2& ctx) {
	std::array<char, 1024> buffer{};
	const auto copied = ::pixel::runtime_v2::copy_host_encoder_last_error_detail_v2(
	    &ctx, buffer.data(), static_cast<uint32_t>(buffer.size()));
	if (copied == 0) {
		return {};
	}
	return std::string(buffer.data(), buffer.data() + copied);
}

[[noreturn]] void throw_runtime_encode_error(
    std::string_view file_path, uid::WellKnown transfer_syntax,
    std::optional<std::size_t> frame_index, pixel_error_code_v2 ec,
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
	case PIXEL_CODEC_PROFILE_JPEG2000_LOSSLESS_V2:
	case PIXEL_CODEC_PROFILE_JPEG2000_LOSSY_V2:
		return key == "color_transform";
	case PIXEL_CODEC_PROFILE_HTJ2K_LOSSLESS_V2:
	case PIXEL_CODEC_PROFILE_HTJ2K_LOSSLESS_RPCL_V2:
	case PIXEL_CODEC_PROFILE_HTJ2K_LOSSY_V2:
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
		out_storage.items.push_back(pixel_option_kv_v2{
		    out_storage.keys[i].c_str(), out_storage.values[i].c_str()});
	}
	out_storage.list.items =
	    out_storage.items.empty() ? nullptr : out_storage.items.data();
	out_storage.list.count = static_cast<uint32_t>(out_storage.items.size());
	return true;
}

[[nodiscard]] std::vector<std::uint8_t> encode_frame_with_runtime_or_throw(
    ::pixel::runtime_v2::HostEncoderContextV2& ctx, std::string_view file_path,
    uid::WellKnown transfer_syntax, const pixel::PixelSource& source,
    std::span<const std::uint8_t> source_frame,
    bool use_multicomponent_transform, std::size_t frame_index) {
	pixel::PixelSource frame_source = source;
	frame_source.frames = 1;
	frame_source.frame_stride = 0;

	uint64_t encoded_size = 0;
	pixel_error_code_v2 encode_ec = ::pixel::runtime_v2::encode_frame_with_host_context_v2(
	    &ctx, &frame_source, source_frame, use_multicomponent_transform,
	    pixel_output_buffer_v2{nullptr, 0u}, &encoded_size);
	if (encode_ec != PIXEL_CODEC_ERR_OUTPUT_TOO_SMALL &&
	    encode_ec != PIXEL_CODEC_ERR_OK) {
		throw_runtime_encode_error(file_path, transfer_syntax, frame_index,
		    encode_ec, copy_encoder_error_detail(ctx));
	}

	const auto max_size_u64 =
	    static_cast<uint64_t>((std::numeric_limits<std::size_t>::max)());
	if (encoded_size > max_size_u64) {
		throw_codec_error_with_context("DicomFile::set_pixel_data", file_path,
		    transfer_syntax, frame_index,
		    CodecError{
		        .code = CodecStatusCode::internal_error,
		        .stage = "encode_frame",
		        .detail = "encoded size exceeds size_t range",
		    });
	}

	std::vector<std::uint8_t> encoded_frame(static_cast<std::size_t>(encoded_size));
	uint64_t actual_size = encoded_size;
	encode_ec = ::pixel::runtime_v2::encode_frame_with_host_context_v2(
	    &ctx, &frame_source, source_frame, use_multicomponent_transform,
	    pixel_output_buffer_v2{
	        encoded_frame.empty() ? nullptr : encoded_frame.data(),
	        static_cast<uint64_t>(encoded_frame.size()),
	    },
	    &actual_size);
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
	return encoded_frame;
}

[[nodiscard]] bool encode_encapsulated_pixel_data_with_runtime_or_throw(DicomFile& file,
    uid::WellKnown transfer_syntax, const pixel::PixelSource& source,
    const EncapsulatedEncodeInput& encode_input, uint32_t codec_profile_code,
    std::span<const CodecOptionKv> codec_options, bool use_multicomponent_transform) {
	const auto* registry = get_runtime_registry();
	if (registry == nullptr) {
		return false;
	}

	CodecError option_error{};
	RuntimeOptionListStorage option_storage{};
	if (!build_runtime_option_list_from_pairs(
	        codec_options, codec_profile_code, option_storage, option_error)) {
		throw_codec_error_with_context("DicomFile::set_pixel_data", file.path(),
		    transfer_syntax, std::nullopt, option_error);
	}
	const pixel_option_list_v2* option_ptr =
	    option_storage.list.count == 0 ? nullptr : &option_storage.list;

	::pixel::runtime_v2::HostEncoderContextV2 encoder_ctx{};
	const EncoderContextGuard guard(&encoder_ctx);
	const pixel_error_code_v2 configure_ec =
	    ::pixel::runtime_v2::configure_host_encoder_context_v2(
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

	const auto encode_frame_or_throw = [&](std::size_t frame_index,
	                                   std::span<const std::uint8_t> source_frame_view) {
			return encode_frame_with_runtime_or_throw(encoder_ctx, file.path(),
			    transfer_syntax, source, source_frame_view,
			    use_multicomponent_transform, frame_index);
		};

	if (encode_input.source_aliases_current_native_pixel_data) {
		std::vector<std::vector<std::uint8_t>> encoded_frames;
		encoded_frames.reserve(encode_input.frame_count);
		for (std::size_t frame_index = 0; frame_index < encode_input.frame_count;
		     ++frame_index) {
			const auto* source_frame =
			    encode_input.source_base + frame_index * encode_input.source_frame_stride;
			const auto source_frame_view = std::span<const std::uint8_t>(
			    source_frame, encode_input.source_frame_size_bytes);
			encoded_frames.push_back(
			    encode_frame_or_throw(frame_index, source_frame_view));
		}
		file.reset_encapsulated_pixel_data(encode_input.frame_count);
		for (std::size_t frame_index = 0; frame_index < encode_input.frame_count;
		     ++frame_index) {
			file.set_encoded_pixel_frame(
			    frame_index, std::move(encoded_frames[frame_index]));
		}
		return true;
	}

	file.reset_encapsulated_pixel_data(encode_input.frame_count);
	for (std::size_t frame_index = 0; frame_index < encode_input.frame_count;
	     ++frame_index) {
		const auto* source_frame =
		    encode_input.source_base + frame_index * encode_input.source_frame_stride;
		const auto source_frame_view = std::span<const std::uint8_t>(
		    source_frame, encode_input.source_frame_size_bytes);
		auto encoded_frame = encode_frame_or_throw(frame_index, source_frame_view);
		file.set_encoded_pixel_frame(frame_index, std::move(encoded_frame));
	}
	return true;
}

}  // namespace
#endif

void run_set_pixel_data_with_computed_codec_options(DicomFile& file,
    uid::WellKnown transfer_syntax, const pixel::PixelSource& source,
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
	        use_multicomponent_transform, source.photometric);

	auto& dataset = file.dataset();
	dataset.ensure_loaded(Tag(0xFFFFu, 0xFFFFu));
	update_pixel_metadata_for_set_pixel_data(dataset, file_path, transfer_syntax, source,
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
		if (!encode_encapsulated_pixel_data_with_runtime_or_throw(file, transfer_syntax,
		        source, encapsulated_encode_input, codec_profile_code, codec_options,
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

} // namespace dicom::pixel::detail
