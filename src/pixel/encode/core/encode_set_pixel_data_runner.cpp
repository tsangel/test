#include "pixel/encode/core/encode_set_pixel_data_runner.hpp"

#include "pixel/encode/core/encode_metadata_updater.hpp"
#include "pixel/encode/core/encode_source_layout_resolver.hpp"
#include "pixel/encode/core/encode_target_resolver.hpp"
#include "pixel/encode/core/multicomponent_option_resolver.hpp"
#include "pixel/encode/core/native_pixel_copy.hpp"
#include "pixel/encode/core/encode_codec_impl_detail.hpp"

#if defined(DICOMSDL_PIXEL_V2_RUNTIME_ENABLED)
#include "pixel_/runtime/host_adapter_v2.hpp"
#include "pixel_/runtime/registry_bootstrap_v2.hpp"
#endif

#if defined(DICOMSDL_PIXEL_V2_RUNTIME_ENABLED)
#include <array>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#include <fmt/format.h>
#endif

#include <cstddef>

namespace dicom::pixel::detail {

#if defined(DICOMSDL_PIXEL_V2_RUNTIME_ENABLED)
namespace {

constexpr std::size_t kMaxOptionCount = 64;
constexpr std::size_t kMaxOptionKeyBytes = 128;
constexpr std::size_t kMaxOptionValueBytes = 1024;
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

struct V2OptionListStorage {
	std::vector<std::string> keys{};
	std::vector<std::string> values{};
	std::vector<pixel_option_kv_v2> items{};
	pixel_option_list_v2 list{};
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
		out_stage = "encode_frame";
	}
	if (out_reason.empty()) {
		out_reason.assign(raw_detail);
	}
	if (out_reason.empty()) {
		out_reason = "encoder v2 host adapter failed";
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

[[noreturn]] void throw_v2_encode_error(
    std::string_view file_path, uid::WellKnown transfer_syntax,
    std::string_view plugin_key, std::optional<std::size_t> frame_index,
    pixel_error_code_v2 ec, std::string_view raw_detail) {
	CodecError encode_error{};
	encode_error.code = map_error_code_v2(ec);
	parse_v2_detail_or_default(raw_detail, encode_error.stage, encode_error.detail);
	throw_codec_error_with_context("DicomFile::set_pixel_data", file_path, transfer_syntax,
	    plugin_key, frame_index, encode_error);
}

[[nodiscard]] bool should_skip_legacy_option_for_profile(
    uint32_t codec_profile_code, std::string_view key) noexcept {
	switch (codec_profile_code) {
	case PIXEL_CODEC_PROFILE_JPEG2000_LOSSLESS_V2:
	case PIXEL_CODEC_PROFILE_JPEG2000_LOSSY_V2:
		return key == "target_bpp" || key == "target_psnr" || key == "color_transform";
	case PIXEL_CODEC_PROFILE_HTJ2K_LOSSLESS_V2:
	case PIXEL_CODEC_PROFILE_HTJ2K_LOSSLESS_RPCL_V2:
	case PIXEL_CODEC_PROFILE_HTJ2K_LOSSY_V2:
		return key == "target_bpp" || key == "target_psnr" || key == "color_transform" ||
		    key == "threads";
	default:
		return false;
	}
}

bool build_v2_option_list_from_pairs(std::span<const CodecOptionKv> option_pairs,
    uint32_t codec_profile_code, V2OptionListStorage& out_storage,
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
		if (should_skip_legacy_option_for_profile(codec_profile_code, pair.key)) {
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

[[nodiscard]] std::vector<std::uint8_t> encode_frame_with_v2_runtime_or_throw(
    ::pixel::runtime_v2::HostEncoderContextV2& ctx, std::string_view file_path,
    uid::WellKnown transfer_syntax, std::string_view plugin_key,
    const pixel::PixelSource& source, std::span<const std::uint8_t> source_frame,
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
		throw_v2_encode_error(file_path, transfer_syntax, plugin_key, frame_index, encode_ec,
		    copy_encoder_error_detail(ctx));
	}

	const auto max_size_u64 =
	    static_cast<uint64_t>((std::numeric_limits<std::size_t>::max)());
	if (encoded_size > max_size_u64) {
		throw_codec_error_with_context("DicomFile::set_pixel_data", file_path,
		    transfer_syntax, plugin_key, frame_index,
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
		throw_v2_encode_error(file_path, transfer_syntax, plugin_key, frame_index, encode_ec,
		    copy_encoder_error_detail(ctx));
	}
	if (actual_size > static_cast<uint64_t>(encoded_frame.size())) {
		throw_codec_error_with_context("DicomFile::set_pixel_data", file_path,
		    transfer_syntax, plugin_key, frame_index,
		    CodecError{
		        .code = CodecStatusCode::internal_error,
		        .stage = "encode_frame",
		        .detail = "encoded size grew beyond allocated output buffer",
		    });
	}
	encoded_frame.resize(static_cast<std::size_t>(actual_size));
	return encoded_frame;
}

[[nodiscard]] bool encode_encapsulated_pixel_data_with_v2_runtime_or_throw(DicomFile& file,
    uid::WellKnown transfer_syntax, const pixel::PixelSource& source,
    const EncapsulatedEncodeInput& encode_input,
    std::span<const CodecOptionKv> codec_options, bool use_multicomponent_transform) {
	const std::string_view legacy_plugin_key =
	    legacy_plugin_key_for_transfer_syntax(transfer_syntax);
	const std::string_view plugin_key_fallback =
	    legacy_plugin_key.empty() ? kV2RuntimePluginKey : legacy_plugin_key;
	const auto* registry = get_runtime_registry_v2();
	if (registry == nullptr) {
		return false;
	}

	CodecError option_error{};
	V2OptionListStorage option_storage{};
	uint32_t codec_profile_code = PIXEL_CODEC_PROFILE_UNKNOWN_V2;
	if (!::pixel::runtime_v2::resolve_codec_profile_code_from_transfer_syntax_v2(
	        transfer_syntax, &codec_profile_code)) {
		return false;
	}
	if (!build_v2_option_list_from_pairs(
	        codec_options, codec_profile_code, option_storage, option_error)) {
		throw_codec_error_with_context("DicomFile::set_pixel_data", file.path(),
		    transfer_syntax, plugin_key_fallback, std::nullopt, option_error);
	}
	const pixel_option_list_v2* option_ptr =
	    option_storage.list.count == 0 ? nullptr : &option_storage.list;

	::pixel::runtime_v2::HostEncoderContextV2 encoder_ctx{};
	const EncoderContextGuard guard(&encoder_ctx);
	const pixel_error_code_v2 configure_ec =
	    ::pixel::runtime_v2::configure_host_encoder_context_v2(
	        &encoder_ctx, registry, transfer_syntax, option_ptr);
	const std::string configure_detail = copy_encoder_error_detail(encoder_ctx);
	const std::string_view plugin_key = plugin_key_fallback;
	if (configure_ec == PIXEL_CODEC_ERR_UNSUPPORTED) {
		return false;
	}
	if (configure_ec != PIXEL_CODEC_ERR_OK) {
		throw_v2_encode_error(file.path(), transfer_syntax, plugin_key, std::nullopt,
		    configure_ec, configure_detail);
	}

	const auto encode_frame_or_throw = [&](std::size_t frame_index,
	                                   std::span<const std::uint8_t> source_frame_view) {
			return encode_frame_with_v2_runtime_or_throw(encoder_ctx, file.path(),
			    transfer_syntax, plugin_key, source, source_frame_view,
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

void run_set_pixel_data_with_resolved_codec_options(DicomFile& file,
    uid::WellKnown transfer_syntax, const pixel::PixelSource& source,
    const TransferSyntaxPluginBinding& binding,
    std::span<const CodecOptionKv> codec_options) {
	const auto target = classify_pixel_encode_target(binding);
	const auto file_path = file.path();
	const auto source_layout =
	    resolve_encode_source_layout_or_throw(source, file_path);
	validate_target_source_constraints(
	    target, source_layout.bits_allocated, source_layout.bits_stored, file_path);
	const bool use_multicomponent_transform =
	    resolve_use_multicomponent_transform(transfer_syntax, target.is_j2k,
	        target.is_htj2k, codec_options, source_layout.samples_per_pixel,
	        file_path);
	const pixel::Photometric output_photometric = resolve_output_photometric(
	    target, use_multicomponent_transform, source.photometric);

	auto& dataset = file.dataset();
	dataset.ensure_loaded(Tag(0xFFFFu, 0xFFFFu));
	update_pixel_metadata_for_set_pixel_data(dataset, file_path, transfer_syntax, source,
	    target.is_rle, output_photometric, source_layout.bits_allocated,
	    source_layout.bits_stored, source_layout.high_bit,
	    source_layout.pixel_representation, source_layout.source_row_stride,
	    source_layout.source_frame_stride);

	// set_transfer_syntax(native->encapsulated) can pass a span that aliases current
	// native PixelData bytes. We must preserve those bytes until all frames are encoded.
	const bool source_aliases_current_native_pixel_data =
	    source_aliases_native_pixel_data(dataset, source.bytes);
	const EncapsulatedEncodeInput encapsulated_encode_input{
	    .source_base = source.bytes.data(),
	    .frame_count = source_layout.frames,
	    .source_frame_stride = source_layout.source_frame_stride,
	    .source_frame_size_bytes = source_layout.source_frame_size_bytes,
	    .source_aliases_current_native_pixel_data = source_aliases_current_native_pixel_data,
	};

	if (target.is_native_uncompressed) {
		const NativePixelCopyInput native_copy_input{
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
		auto native_pixel_data = build_native_pixel_payload(native_copy_input);
		file.set_native_pixel_data(std::move(native_pixel_data));
	} else {
#if defined(DICOMSDL_PIXEL_V2_RUNTIME_ENABLED)
		if (encode_encapsulated_pixel_data_with_v2_runtime_or_throw(file, transfer_syntax,
		        source, encapsulated_encode_input, codec_options,
		        use_multicomponent_transform)) {
			// v2 runtime path succeeded.
		} else {
#endif
		const CodecEncodeFnInput dispatch_input{
		    .file = file,
		    .transfer_syntax = transfer_syntax,
		    .encode_input = encapsulated_encode_input,
		    .codec_options = codec_options,
		    .rows = source_layout.rows,
		    .cols = source_layout.cols,
		    .samples_per_pixel = source_layout.samples_per_pixel,
		    .bytes_per_sample = source_layout.bytes_per_sample,
		    .bits_allocated = source_layout.bits_allocated,
		    .bits_stored = source_layout.bits_stored,
		    .pixel_representation = source_layout.pixel_representation,
		    .use_multicomponent_transform = use_multicomponent_transform,
		    .source_planar = source.planar,
		    .planar_source = source_layout.planar_source,
		    .row_payload_bytes = source_layout.row_payload_bytes,
		    .source_row_stride = source_layout.source_row_stride,
		    .source_plane_stride = source_layout.source_plane_stride,
		    .source_frame_size_bytes = source_layout.source_frame_size_bytes,
		    .destination_frame_payload = source_layout.destination_frame_payload,
		    .profile = binding.profile,
		    .plugin_key = binding.plugin_key,
		};
		encode_encapsulated_pixel_data(dispatch_input);
#if defined(DICOMSDL_PIXEL_V2_RUNTIME_ENABLED)
		}
#endif
	}

	const auto encoded_payload_bytes = target_uses_lossy_compression(target)
	    ? encoded_payload_size_from_pixel_sequence(dataset, file_path, transfer_syntax)
	    : std::size_t{0};
	update_lossy_compression_metadata_for_set_pixel_data(dataset, file_path,
	    transfer_syntax, target, source_layout.destination_total_bytes,
	    encoded_payload_bytes);
}

} // namespace dicom::pixel::detail
