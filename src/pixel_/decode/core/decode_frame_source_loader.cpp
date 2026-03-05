#include "pixel_/decode/core/decode_codec_impl_detail.hpp"

#include "diagnostics.h"

#include <limits>
#include <string_view>
#include <vector>

using namespace dicom::literals;
namespace diag = dicom::diag;

namespace dicom {
namespace pixel::detail {

PlanarTransform select_planar_transform(Planar src_planar, Planar dst_planar) noexcept {
	if (src_planar == Planar::interleaved) {
		return (dst_planar == Planar::interleaved)
		           ? PlanarTransform::interleaved_to_interleaved
		           : PlanarTransform::interleaved_to_planar;
	}
	return (dst_planar == Planar::interleaved)
	           ? PlanarTransform::planar_to_interleaved
	           : PlanarTransform::planar_to_planar;
}

std::size_t sv_dtype_bytes(DataType sv_dtype) noexcept {
	switch (sv_dtype) {
	case DataType::u8:
	case DataType::s8:
		return 1;
	case DataType::u16:
	case DataType::s16:
		return 2;
	case DataType::u32:
	case DataType::s32:
	case DataType::f32:
		return 4;
	case DataType::f64:
		return 8;
	default:
		return 0;
	}
}

EncapsulatedFrameSource load_encapsulated_frame_source(const DataSet& ds,
    std::string_view file_path, std::size_t frame_index, std::string_view codec_name) {
	const auto& pixel_data = ds["PixelData"_tag];
	if (!pixel_data || !pixel_data.vr().is_pixel_sequence()) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason={} requires encapsulated PixelData",
		    file_path, codec_name);
	}

	const auto* pixel_sequence = pixel_data.as_pixel_sequence();
	if (!pixel_sequence) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason={} pixel sequence is missing",
		    file_path, codec_name);
	}

	const auto frame_count = pixel_sequence->number_of_frames();
	if (frame_index >= frame_count) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} frame={} reason={} frame index out of range (frames={})",
		    file_path, frame_index, codec_name, frame_count);
	}

	const auto* frame = pixel_sequence->frame(frame_index);
	if (!frame) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} frame={} reason={} frame is missing",
		    file_path, frame_index, codec_name);
	}

	EncapsulatedFrameSource source{};
	source.frame = frame;
	if (frame->encoded_data_size() != 0) {
		source.contiguous = frame->encoded_data_view();
		source.total_size = source.contiguous.size();
		return source;
	}

	const auto& fragments = frame->fragments();
	if (fragments.empty()) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} frame={} reason={} frame has no fragments",
		    file_path, frame_index, codec_name);
	}
	for (const auto& fragment : fragments) {
		if (fragment.length == 0) {
			diag::error_and_throw(
			    "pixel::decode_frame_into file={} frame={} reason={} zero-length fragment is not supported",
			    file_path, frame_index, codec_name);
		}
	}

	auto* mutable_sequence = const_cast<PixelSequence*>(pixel_sequence);
	source.contiguous = mutable_sequence->frame_encoded_span(frame_index);
	if (!source.contiguous.empty()) {
		source.total_size = source.contiguous.size();
		return source;
	}

	const auto* stream = pixel_sequence->stream();
	if (!stream) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} frame={} reason={} pixel sequence stream is missing",
		    file_path, frame_index, codec_name);
	}
	source.stream = stream;

	if (fragments.size() == 1) {
		const auto& fragment = fragments.front();
		source.contiguous = stream->get_span(fragment.offset, fragment.length);
		source.total_size = source.contiguous.size();
		return source;
	}

	source.fragments = &fragments;
	std::size_t total_size = 0;
	for (const auto& fragment : fragments) {
		if (fragment.length > std::numeric_limits<std::size_t>::max() - total_size) {
			diag::error_and_throw(
			    "pixel::decode_frame_into file={} frame={} reason={} frame size overflow",
			    file_path, frame_index, codec_name);
		}
		total_size += fragment.length;
	}
	source.total_size = total_size;
	return source;
}

namespace {

struct NativeSourceElement {
	const DataElement* element{nullptr};
	std::string_view name{"PixelData"};
};

NativeSourceElement select_native_source_element(
    const DataSet& ds, DataType sv_dtype) {
	switch (sv_dtype) {
	case DataType::f32:
		return NativeSourceElement{&ds["FloatPixelData"_tag], "FloatPixelData"};
	case DataType::f64:
		return NativeSourceElement{&ds["DoubleFloatPixelData"_tag], "DoubleFloatPixelData"};
	default:
		return NativeSourceElement{&ds["PixelData"_tag], "PixelData"};
	}
}

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

} // namespace

NativeFrameSource load_native_frame_source(
    const DataSet& ds, std::string_view file_path,
    const pixel::PixelDataInfo& info, std::size_t frame_index) {
	const auto sv_dtype = info.sv_dtype;
	const auto source = select_native_source_element(ds, sv_dtype);
	if (!source.element || !(*source.element)) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=missing {}",
		    file_path, source.name);
	}
	if (source.element->vr().is_pixel_sequence()) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=encapsulated {} is not raw",
		    file_path, source.name);
	}

	if (info.rows <= 0 || info.cols <= 0 || info.samples_per_pixel <= 0 ||
	    info.frames <= 0) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=invalid raw pixel metadata",
		    file_path);
	}

	const auto bytes_per_sample = sv_dtype_bytes(info.sv_dtype);
	if (bytes_per_sample == 0) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=unsupported sv_dtype for native raw source",
		    file_path);
	}

	const auto rows = static_cast<std::size_t>(info.rows);
	const auto cols = static_cast<std::size_t>(info.cols);
	const auto samples_per_pixel = static_cast<std::size_t>(info.samples_per_pixel);
	const auto frame_count = static_cast<std::size_t>(info.frames);
	if (frame_index >= frame_count) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} frame={} reason=raw frame index out of range (frames={})",
		    file_path, frame_index, frame_count);
	}

	const auto src_planar = info.planar_configuration;
	const std::size_t src_row_components =
	    (src_planar == Planar::interleaved) ? samples_per_pixel : std::size_t{1};
	std::size_t src_row_pixels = 0;
	std::size_t src_row_bytes = 0;
	if (!checked_mul_size_t(cols, src_row_components, src_row_pixels) ||
	    !checked_mul_size_t(src_row_pixels, bytes_per_sample, src_row_bytes)) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=raw row bytes exceed size_t range",
		    file_path);
	}

	std::size_t src_frame_bytes = 0;
	if (!checked_mul_size_t(src_row_bytes, rows, src_frame_bytes)) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=raw frame bytes exceed size_t range",
		    file_path);
	}
	if (src_planar == Planar::planar &&
	    !checked_mul_size_t(src_frame_bytes, samples_per_pixel, src_frame_bytes)) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=raw planar frame bytes exceed size_t range",
		    file_path);
	}

	NativeFrameSource native_source{};
	native_source.contiguous = source.element->value_span();
	native_source.name = source.name;
	std::size_t src_frame_offset = 0;
	if (!checked_mul_size_t(frame_index, src_frame_bytes, src_frame_offset)) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=raw frame offset exceeds size_t range",
		    file_path);
	}
	if (native_source.contiguous.size() < src_frame_offset ||
	    native_source.contiguous.size() - src_frame_offset < src_frame_bytes) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} frame={} reason={} length is shorter than expected",
		    file_path, frame_index, native_source.name);
	}
	native_source.contiguous =
	    native_source.contiguous.subspan(src_frame_offset, src_frame_bytes);
	return native_source;
}

std::span<const std::uint8_t> materialize_encapsulated_frame_source(
    std::string_view file_path, std::size_t frame_index, std::string_view codec_name,
    const EncapsulatedFrameSource& source, std::vector<std::uint8_t>& out_owned) {
	if (!source.contiguous.empty()) {
		return source.contiguous;
	}
	if (!source.frame || !source.stream || !source.fragments) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} frame={} reason={} frame source is not materializable",
		    file_path, frame_index, codec_name);
	}
	out_owned = source.frame->coalesce_encoded_data(*source.stream);
	return std::span<const std::uint8_t>(out_owned);
}

} // namespace pixel::detail

} // namespace dicom
