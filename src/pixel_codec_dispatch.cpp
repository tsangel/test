#include "pixel_codec_registry.hpp"

#include "pixel_decoder_detail.hpp"
#include "pixel_encoder_detail.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

namespace dicom::pixel::detail {
using namespace dicom::literals;

namespace {

[[nodiscard]] std::vector<std::uint8_t> copy_single_frame_to_native_layout(
    std::span<const std::uint8_t> source_bytes, std::size_t rows,
    std::size_t samples_per_pixel, bool planar_source,
    std::size_t row_payload_bytes, std::size_t source_row_stride,
    std::size_t source_plane_stride, std::size_t destination_frame_payload) {
	std::vector<std::uint8_t> native_pixel_data(destination_frame_payload);
	if (native_pixel_data.empty()) {
		return native_pixel_data;
	}

	const auto* source_base = source_bytes.data();
	auto* destination_base = native_pixel_data.data();

	if (planar_source) {
		const std::size_t destination_plane_stride =
		    destination_frame_payload / samples_per_pixel;
		for (std::size_t sample = 0; sample < samples_per_pixel; ++sample) {
			const auto* source_plane = source_base + sample * source_plane_stride;
			auto* destination_plane =
			    destination_base + sample * destination_plane_stride;
			for (std::size_t row = 0; row < rows; ++row) {
				std::memcpy(destination_plane + row * row_payload_bytes,
				    source_plane + row * source_row_stride, row_payload_bytes);
			}
		}
		return native_pixel_data;
	}

	for (std::size_t row = 0; row < rows; ++row) {
		std::memcpy(destination_base + row * row_payload_bytes,
		    source_base + row * source_row_stride, row_payload_bytes);
	}
	return native_pixel_data;
}

void set_codec_error(codec_error& out_error, codec_status_code code,
    std::string_view stage, std::string_view detail) {
	out_error.code = code;
	out_error.stage = std::string(stage);
	out_error.detail = std::string(detail);
}

} // namespace

bool encode_frame_plugin_encapsulated_uncompressed(
    const CodecEncodeFrameInput& input, const CodecOptions& parsed_options,
    std::vector<std::uint8_t>& out_encoded_frame,
    codec_error& out_error) noexcept {
	if (!std::holds_alternative<NoCompression>(parsed_options)) {
		set_codec_error(out_error, codec_status_code::invalid_argument,
		    "parse_options", "plugin parsed options with unexpected variant type");
		out_encoded_frame.clear();
		return false;
	}
	try {
		out_encoded_frame = copy_single_frame_to_native_layout(input.source_frame,
		    input.rows, input.samples_per_pixel, input.planar_source,
		    input.row_payload_bytes, input.source_row_stride,
		    input.source_plane_stride, input.destination_frame_payload);
		out_error = codec_error{};
		return true;
	} catch (const std::exception& e) {
		set_codec_error(out_error, codec_status_code::backend_error, "encode_frame",
		    e.what());
	} catch (...) {
		set_codec_error(out_error, codec_status_code::backend_error, "encode_frame",
		    "non-standard exception");
	}
	out_encoded_frame.clear();
	return false;
}

bool encode_frame_plugin_rle(const CodecEncodeFrameInput& input,
    const CodecOptions& parsed_options,
    std::vector<std::uint8_t>& out_encoded_frame,
    codec_error& out_error) noexcept {
	if (!std::holds_alternative<RleOptions>(parsed_options)) {
		set_codec_error(out_error, codec_status_code::invalid_argument,
		    "parse_options", "plugin parsed options with unexpected variant type");
		out_encoded_frame.clear();
		return false;
	}
	if (!try_encode_rle_frame(input.source_frame, input.rows, input.cols,
	        input.samples_per_pixel, input.bytes_per_sample, input.source_planar,
	        input.source_row_stride, out_encoded_frame, out_error)) {
		if (out_error.code == codec_status_code::ok) {
			set_codec_error(out_error, codec_status_code::backend_error,
			    "encode_frame", "RLE encode failed");
		}
		return false;
	}
	return true;
}

bool encode_frame_plugin_jpeg2k(const CodecEncodeFrameInput& input,
    const CodecOptions& parsed_options,
    std::vector<std::uint8_t>& out_encoded_frame,
    codec_error& out_error) noexcept {
	if (!std::holds_alternative<J2kOptions>(parsed_options)) {
		set_codec_error(out_error, codec_status_code::invalid_argument,
		    "parse_options", "plugin parsed options with unexpected variant type");
		out_encoded_frame.clear();
		return false;
	}
	const auto& options = std::get<J2kOptions>(parsed_options);
	const bool lossless = input.profile == codec_profile::jpeg2000_lossless;
	if (!try_encode_jpeg2k_frame(input.source_frame, input.rows, input.cols,
	        input.samples_per_pixel, input.bytes_per_sample, input.bits_allocated,
	        input.bits_stored, input.pixel_representation, input.source_planar,
	        input.source_row_stride, input.use_multicomponent_transform, lossless,
	        options, out_encoded_frame, out_error)) {
		if (out_error.code == codec_status_code::ok) {
			set_codec_error(out_error, codec_status_code::backend_error,
			    "encode_frame", "JPEG2000 encode failed");
		}
		return false;
	}
	return true;
}

bool encode_frame_plugin_htj2k(const CodecEncodeFrameInput& input,
    const CodecOptions& parsed_options,
    std::vector<std::uint8_t>& out_encoded_frame,
    codec_error& out_error) noexcept {
	if (!std::holds_alternative<Htj2kOptions>(parsed_options)) {
		set_codec_error(out_error, codec_status_code::invalid_argument,
		    "parse_options", "plugin parsed options with unexpected variant type");
		out_encoded_frame.clear();
		return false;
	}
	const auto& options = std::get<Htj2kOptions>(parsed_options);
	const bool lossless = input.profile == codec_profile::htj2k_lossless ||
	    input.profile == codec_profile::htj2k_lossless_rpcl;
	const bool rpcl_progression = input.transfer_syntax == "HTJ2KLosslessRPCL"_uid;
	if (!try_encode_htj2k_frame(input.source_frame, input.rows, input.cols,
	        input.samples_per_pixel, input.bytes_per_sample, input.bits_allocated,
	        input.bits_stored, input.pixel_representation, input.source_planar,
	        input.source_row_stride, input.use_multicomponent_transform, lossless,
	        rpcl_progression, options, out_encoded_frame, out_error)) {
		if (out_error.code == codec_status_code::ok) {
			set_codec_error(out_error, codec_status_code::backend_error,
			    "encode_frame", "HTJ2K encode failed");
		}
		return false;
	}
	return true;
}

bool encode_frame_plugin_jpegls(const CodecEncodeFrameInput& input,
    const CodecOptions& parsed_options,
    std::vector<std::uint8_t>& out_encoded_frame,
    codec_error& out_error) noexcept {
	if (!std::holds_alternative<JpegLsOptions>(parsed_options)) {
		set_codec_error(out_error, codec_status_code::invalid_argument,
		    "parse_options", "plugin parsed options with unexpected variant type");
		out_encoded_frame.clear();
		return false;
	}
	const auto& options = std::get<JpegLsOptions>(parsed_options);
	if (!try_encode_jpegls_frame(input.source_frame, input.rows, input.cols,
	        input.samples_per_pixel, input.bytes_per_sample, input.bits_allocated,
	        input.bits_stored, input.source_planar, input.source_row_stride,
	        options.near_lossless_error, out_encoded_frame,
	        out_error)) {
		if (out_error.code == codec_status_code::ok) {
			set_codec_error(out_error, codec_status_code::backend_error,
			    "encode_frame", "JPEG-LS encode failed");
		}
		return false;
	}
	return true;
}

bool encode_frame_plugin_jpeg(const CodecEncodeFrameInput& input,
    const CodecOptions& parsed_options,
    std::vector<std::uint8_t>& out_encoded_frame,
    codec_error& out_error) noexcept {
	if (!std::holds_alternative<JpegOptions>(parsed_options)) {
		set_codec_error(out_error, codec_status_code::invalid_argument,
		    "parse_options", "plugin parsed options with unexpected variant type");
		out_encoded_frame.clear();
		return false;
	}
	const auto& options = std::get<JpegOptions>(parsed_options);
	const bool lossless = input.profile == codec_profile::jpeg_lossless;
	if (!try_encode_jpeg_frame(input.source_frame, input.rows, input.cols,
	        input.samples_per_pixel, input.bytes_per_sample, input.bits_allocated,
	        input.bits_stored, input.source_planar, input.source_row_stride, lossless,
	        options, out_encoded_frame, out_error)) {
		if (out_error.code == codec_status_code::ok) {
			set_codec_error(out_error, codec_status_code::backend_error,
			    "encode_frame", "JPEG encode failed");
		}
		return false;
	}
	return true;
}

bool encode_frame_plugin_jpegxl(const CodecEncodeFrameInput& input,
    const CodecOptions& parsed_options,
    std::vector<std::uint8_t>& out_encoded_frame,
    codec_error& out_error) noexcept {
	if (!std::holds_alternative<JpegXlOptions>(parsed_options)) {
		set_codec_error(out_error, codec_status_code::invalid_argument,
		    "parse_options", "plugin parsed options with unexpected variant type");
		out_encoded_frame.clear();
		return false;
	}
	const auto& options = std::get<JpegXlOptions>(parsed_options);
	const bool lossless = input.profile == codec_profile::jpegxl_lossless;
	if (!try_encode_jpegxl_frame(input.source_frame, input.rows, input.cols,
	        input.samples_per_pixel, input.bytes_per_sample, input.bits_allocated,
	        input.bits_stored, input.pixel_representation, input.source_planar,
	        input.source_row_stride, lossless, options,
	        out_encoded_frame, out_error)) {
		if (out_error.code == codec_status_code::ok) {
			set_codec_error(out_error, codec_status_code::backend_error,
			    "encode_frame", "JPEG-XL encode failed");
		}
		return false;
	}
	return true;
}

template <typename DecodeFn>
bool decode_frame_from_input(
    const CodecDecodeFrameInput& input, codec_error& out_error,
    DecodeFn&& decode_fn) noexcept {
	return decode_fn(input.info, input.value_transform, input.destination,
	    input.destination_strides, input.options, out_error, input.prepared_source);
}

bool decode_frame_plugin_native(
    const CodecDecodeFrameInput& input, codec_error& out_error) noexcept {
	return decode_frame_from_input(input, out_error, &decode_raw_into);
}

bool decode_frame_plugin_encapsulated_uncompressed(
    const CodecDecodeFrameInput& input, codec_error& out_error) noexcept {
	return decode_frame_from_input(
	    input, out_error, &decode_encapsulated_uncompressed_into);
}

bool decode_frame_plugin_rle(
    const CodecDecodeFrameInput& input, codec_error& out_error) noexcept {
	return decode_frame_from_input(input, out_error, &decode_rle_into);
}

bool decode_frame_plugin_jpeg2k(
    const CodecDecodeFrameInput& input, codec_error& out_error) noexcept {
	return decode_frame_from_input(input, out_error, &decode_jpeg2k_into);
}

bool decode_frame_plugin_htj2k(
    const CodecDecodeFrameInput& input, codec_error& out_error) noexcept {
	return decode_frame_from_input(input, out_error, &decode_htj2k_into);
}

bool decode_frame_plugin_jpegls(
    const CodecDecodeFrameInput& input, codec_error& out_error) noexcept {
	return decode_frame_from_input(input, out_error, &decode_jpegls_into);
}

bool decode_frame_plugin_jpeg(
    const CodecDecodeFrameInput& input, codec_error& out_error) noexcept {
	return decode_frame_from_input(input, out_error, &decode_jpeg_into);
}

bool decode_frame_plugin_jpegxl(
    const CodecDecodeFrameInput& input, codec_error& out_error) noexcept {
	return decode_frame_from_input(input, out_error, &decode_jpegxl_into);
}

} // namespace dicom::pixel::detail
