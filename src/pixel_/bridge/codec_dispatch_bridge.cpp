#include "pixel_/registry/codec_registry.hpp"

#include "pixel_/decode/core/decode_codec_impl_detail.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <span>
#include <string_view>
#include <vector>

namespace dicom::pixel::detail {

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

} // namespace

bool encode_frame_plugin_encapsulated_uncompressed(
    const CodecEncodeFrameInput& input, std::span<const CodecOptionKv> encode_options,
    std::vector<std::uint8_t>& out_encoded_frame,
    CodecError& out_error) noexcept {
	(void)encode_options;
	try {
		out_encoded_frame = copy_single_frame_to_native_layout(input.source_frame,
		    input.rows, input.samples_per_pixel, input.planar_source,
		    input.row_payload_bytes, input.source_row_stride,
		    input.source_plane_stride, input.destination_frame_payload);
		out_error = CodecError{};
		return true;
	} catch (const std::exception& e) {
		set_codec_error(out_error, CodecStatusCode::backend_error, "encode_frame",
		    e.what());
	} catch (...) {
		set_codec_error(out_error, CodecStatusCode::backend_error, "encode_frame",
		    "non-standard exception");
	}
	out_encoded_frame.clear();
	return false;
}

template <typename DecodeFn>
bool decode_frame_from_input(
    const CodecDecodeFrameInput& input, CodecError& out_error,
    DecodeFn&& decode_fn) noexcept {
	return decode_fn(input.info, input.value_transform, input.destination,
	    input.destination_strides, input.options, out_error, input.prepared_source);
}

bool decode_frame_plugin_native(
    const CodecDecodeFrameInput& input, CodecError& out_error) noexcept {
	return decode_frame_from_input(input, out_error, &decode_raw_into);
}

bool decode_frame_plugin_encapsulated_uncompressed(
    const CodecDecodeFrameInput& input, CodecError& out_error) noexcept {
	return decode_frame_from_input(
	    input, out_error, &decode_encapsulated_uncompressed_into);
}

} // namespace dicom::pixel::detail
