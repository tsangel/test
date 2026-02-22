#include "pixel_decoder_detail.hpp"

#include <cstring>

#include "dicom_endian.h"
#include "diagnostics.h"

using namespace dicom::literals;
namespace diag = dicom::diag;

namespace dicom {
namespace pixel::detail {

namespace {

struct raw_source {
	const DataElement* element{nullptr};
	const char* name{"PixelData"};
};

raw_source select_raw_source(const DicomFile& df, DataType sv_dtype) {
	const auto& ds = df.dataset();
	switch (sv_dtype) {
	case DataType::f32:
		return raw_source{&ds["FloatPixelData"_tag], "FloatPixelData"};
	case DataType::f64:
		return raw_source{&ds["DoubleFloatPixelData"_tag], "DoubleFloatPixelData"};
	default:
		return raw_source{&ds["PixelData"_tag], "PixelData"};
	}
}

} // namespace

void decode_raw_into(const DicomFile& df, const DicomFile::pixel_info_t& info,
    std::size_t frame_index, std::span<std::uint8_t> dst,
    const DecodeStrides& dst_strides, const DecodeOptions& opt) {
	const auto& ds = df.dataset();
	if (!info.has_pixel_data) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=sv_dtype is unknown", df.path());
	}

	if (info.rows <= 0 || info.cols <= 0 || info.samples_per_pixel <= 0) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=invalid Rows/Columns/SamplesPerPixel",
		    df.path());
	}

	const auto samples_per_pixel_value = info.samples_per_pixel;
	if (samples_per_pixel_value != 1 && samples_per_pixel_value != 3 && samples_per_pixel_value != 4) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=only SamplesPerPixel=1/3/4 is supported in current raw path",
		    df.path());
	}
	const auto samples_per_pixel = static_cast<std::size_t>(samples_per_pixel_value);

	const auto src_bytes_per_sample = sv_dtype_bytes(info.sv_dtype);
	if (src_bytes_per_sample == 0) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=only sv_dtype=u8/s8/u16/s16/u32/s32/f32/f64 is supported in current raw path",
		    df.path());
	}
	const std::size_t dst_bytes_per_sample = opt.scaled ? sizeof(float) : src_bytes_per_sample;

	if (info.frames <= 0) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=invalid NumberOfFrames",
		    df.path());
	}

	const auto rows = static_cast<std::size_t>(info.rows);
	const auto cols = static_cast<std::size_t>(info.cols);
	const auto frame_count = static_cast<std::size_t>(info.frames);
	if (frame_index >= frame_count) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} frame={} reason=frame index out of range (frames={})",
		    df.path(), frame_index, frame_count);
	}

	const auto source = select_raw_source(df, info.sv_dtype);
	if (!source.element || !(*source.element)) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=missing {}", df.path(), source.name);
	}
	if (source.element->vr().is_pixel_sequence()) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=encapsulated {} is not raw",
		    df.path(), source.name);
	}

	const auto src_planar = info.planar_configuration;
	const auto dst_planar = opt.planar_out;
	const auto transform = select_planar_transform(src_planar, dst_planar);

	const std::size_t src_row_components =
	    (src_planar == Planar::interleaved) ? samples_per_pixel : std::size_t{1};
	const std::size_t dst_row_components =
	    (dst_planar == Planar::interleaved) ? samples_per_pixel : std::size_t{1};

	const auto src = source.element->value_span();
	const std::size_t src_row_bytes = cols * src_row_components * src_bytes_per_sample;
	std::size_t src_frame_bytes = src_row_bytes * rows;
	if (src_planar == Planar::planar) {
		src_frame_bytes *= samples_per_pixel;
	}

	const std::size_t dst_min_row_bytes = cols * dst_row_components * dst_bytes_per_sample;
	if (dst_strides.row < dst_min_row_bytes) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=row stride too small (need>={}, got={})",
		    df.path(), dst_min_row_bytes, dst_strides.row);
	}
	std::size_t min_frame_bytes = dst_strides.row * rows;
	if (dst_planar == Planar::planar) {
		min_frame_bytes *= samples_per_pixel;
	}
	if (dst_strides.frame < min_frame_bytes) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=frame stride too small (need>={}, got={})",
		    df.path(), min_frame_bytes, dst_strides.frame);
	}
	if (dst.size() < dst_strides.frame) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=destination too small (need={}, got={})",
		    df.path(), dst_strides.frame, dst.size());
	}

	const std::size_t src_frame_offset = frame_index * src_frame_bytes;
	if (src.size() < src_frame_offset + src_frame_bytes) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason={} length is shorter than expected for frame {}",
		    df.path(), source.name, frame_index);
	}

	const bool source_little_endian = ds.is_little_endian();
	const bool needs_swap =
	    (src_bytes_per_sample > 1) && (source_little_endian != endian::host_is_little_endian());

	const auto* src_frame = src.data() + src_frame_offset;
	if (opt.scaled) {
		decode_mono_scaled_into_f32(
		    df, info, src_frame, dst, dst_strides, rows, cols, src_row_bytes);
		return;
	}

	// Fast path for raw copies when no byte swap or Planar transform work is needed.
	const bool equivalent_single_channel_layout = samples_per_pixel == 1;
	const bool interleaved_no_transform =
	    transform == planar_transform::interleaved_to_interleaved;
	if (!needs_swap && dst_strides.row == src_row_bytes &&
	    (equivalent_single_channel_layout || interleaved_no_transform)) {
		std::memcpy(dst.data(), src_frame, src_row_bytes * rows);
		return;
	}

	run_planar_transform_copy(transform, src_bytes_per_sample, needs_swap,
	    src_frame, dst.data(), rows, cols, samples_per_pixel,
	    src_row_bytes, dst_strides.row);
}

} // namespace pixel::detail
} // namespace dicom
