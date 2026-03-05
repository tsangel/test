#include "pixel_/encode/core/encode_codec_impl_detail.hpp"
#include "pixel_/bridge/codec_plugin_abi_adapter.hpp"

#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace dicom::pixel::detail {

namespace {

std::string normalize_encode_error_detail(std::string detail) {
	// Some encoder helpers may include "pixel::encode_* reason=...".
	// Keep only reason payload so call-site context is the single source of file/frame info.
	const auto reason_pos = detail.find("reason=");
	if (detail.rfind("pixel::encode_", 0) == 0 && reason_pos != std::string::npos) {
		detail = detail.substr(reason_pos + 7);
	}
	while (!detail.empty() &&
	       std::isspace(static_cast<unsigned char>(detail.front())) != 0) {
		detail.erase(detail.begin());
	}
	if (detail.empty()) {
		detail = "encoder plugin failed";
	}
	return detail;
}

} // namespace

void encode_encapsulated_pixel_data(const CodecEncodeFnInput& input) {
	constexpr std::string_view kFunctionName = "DicomFile::set_pixel_data";
	const auto file_path = input.file.path();
	const auto& registry = global_codec_registry();
	// Keep dispatch stable while external plugins may update encode/decode dispatchers.
	[[maybe_unused]] const auto dispatch_lock = registry.acquire_dispatch_read_lock();
	const auto* binding = registry.find_binding(input.transfer_syntax);
	const auto plugin_list = registry.plugins();
	const CodecPlugin* plugin = nullptr;
	if (binding && binding->plugin_key == input.plugin_key &&
	    binding->plugin_index < plugin_list.size()) {
		const auto& candidate = plugin_list[binding->plugin_index];
		if (candidate.key == input.plugin_key) {
			plugin = &candidate;
		}
	}
	if (!plugin) {
		plugin = registry.find_plugin(input.plugin_key);
	}
	if (!plugin) {
		throw_codec_error_with_context(kFunctionName, file_path,
		    input.transfer_syntax, input.plugin_key, std::nullopt,
		    CodecError{
		        .code = CodecStatusCode::unsupported,
		        .stage = "plugin_lookup",
		        .detail = "plugin is not registered in codec registry",
		    });
	}
	if (!plugin->encode_frame) {
		throw_codec_error_with_context(kFunctionName, file_path,
		    input.transfer_syntax, input.plugin_key, std::nullopt,
		    CodecError{
		        .code = CodecStatusCode::unsupported,
		        .stage = "plugin_lookup",
		        .detail = "plugin does not provide encode_frame dispatch",
		    });
	}
	const auto encode_frame_or_throw = [&](std::size_t frame_index,
	                                   std::span<const std::uint8_t> source_frame_view) {
			CodecEncodeFrameInput frame_input{
			    .source_frame = source_frame_view,
		    .transfer_syntax = input.transfer_syntax,
		    .rows = input.rows,
		    .cols = input.cols,
		    .samples_per_pixel = input.samples_per_pixel,
		    .bytes_per_sample = input.bytes_per_sample,
		    .bits_allocated = input.bits_allocated,
		    .bits_stored = input.bits_stored,
		    .pixel_representation = input.pixel_representation,
		    .use_multicomponent_transform = input.use_multicomponent_transform,
		    .source_planar = input.source_planar,
		    .planar_source = input.planar_source,
		    .row_payload_bytes = input.row_payload_bytes,
		    .source_row_stride = input.source_row_stride,
		    .source_plane_stride = input.source_plane_stride,
		    .source_frame_size_bytes = input.source_frame_size_bytes,
		    .destination_frame_payload = input.destination_frame_payload,
			    .profile = input.profile,
			};
			dicomsdl_encoder_request_v1 abi_request{};
			detail::abi::build_encoder_request_v1(
			    frame_input, std::span<std::uint8_t>{}, 0, abi_request);
			if (abi_request.frame.transfer_syntax_code ==
			    DICOMSDL_TRANSFER_SYNTAX_CODE_INVALID) {
				throw_codec_error_with_context(kFunctionName, file_path,
				    input.transfer_syntax, input.plugin_key, frame_index,
				    CodecError{
				        .code = CodecStatusCode::invalid_argument,
				        .stage = "validate",
				        .detail = "invalid transfer syntax code for encoder ABI request",
				    });
			}
			if (abi_request.frame.source_dtype == DICOMSDL_DTYPE_UNKNOWN) {
				throw_codec_error_with_context(kFunctionName, file_path,
				    input.transfer_syntax, input.plugin_key, frame_index,
				    CodecError{
				        .code = CodecStatusCode::invalid_argument,
				        .stage = "validate",
				        .detail = "source dtype is not representable in encoder ABI request",
				    });
			}
			if (abi_request.frame.codec_profile_code ==
			    DICOMSDL_CODEC_PROFILE_UNKNOWN) {
				throw_codec_error_with_context(kFunctionName, file_path,
				    input.transfer_syntax, input.plugin_key, frame_index,
				    CodecError{
				        .code = CodecStatusCode::invalid_argument,
				        .stage = "validate",
				        .detail = "codec profile is not representable in encoder ABI request",
				    });
			}

		std::vector<std::uint8_t> encoded_frame{};
		CodecError error{};
		const bool ok =
		    plugin->encode_frame(frame_input, input.codec_options, encoded_frame, error);
		if (!ok) {
			if (error.code == CodecStatusCode::ok) {
				error.code = CodecStatusCode::backend_error;
			}
			if (error.stage.empty()) {
				error.stage = "encode_frame";
			}
			error.detail = normalize_encode_error_detail(std::move(error.detail));
			throw_codec_error_with_context(kFunctionName, file_path,
			    input.transfer_syntax, input.plugin_key, frame_index, error);
		}
		return encoded_frame;
	};

	const auto& encode_input = input.encode_input;
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
		input.file.reset_encapsulated_pixel_data(encode_input.frame_count);
		for (std::size_t frame_index = 0; frame_index < encode_input.frame_count;
		     ++frame_index) {
			input.file.set_encoded_pixel_frame(
			    frame_index, std::move(encoded_frames[frame_index]));
		}
		return;
	}

	input.file.reset_encapsulated_pixel_data(encode_input.frame_count);
	for (std::size_t frame_index = 0; frame_index < encode_input.frame_count;
	     ++frame_index) {
		const auto* source_frame =
		    encode_input.source_base + frame_index * encode_input.source_frame_stride;
		const auto source_frame_view = std::span<const std::uint8_t>(
		    source_frame, encode_input.source_frame_size_bytes);
		auto encoded_frame = encode_frame_or_throw(frame_index, source_frame_view);
		input.file.set_encoded_pixel_frame(frame_index, std::move(encoded_frame));
	}
}

} // namespace dicom::pixel::detail
