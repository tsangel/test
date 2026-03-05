#include "pixel_/encode/core/encode_source_layout_resolver.hpp"

#include "diagnostics.h"

#include <cstddef>
#include <limits>
#include <optional>

namespace dicom::pixel::detail {

namespace {

std::size_t bytes_per_sample_of(pixel::DataType dtype) noexcept {
	switch (dtype) {
	case pixel::DataType::u8:
	case pixel::DataType::s8:
		return 1;
	case pixel::DataType::u16:
	case pixel::DataType::s16:
		return 2;
	case pixel::DataType::u32:
	case pixel::DataType::s32:
	case pixel::DataType::f32:
		return 4;
	case pixel::DataType::f64:
		return 8;
	default:
		return 0;
	}
}

struct NativeSourceLayout {
	int bits_allocated{0};
	int pixel_representation{0}; // 0 unsigned, 1 signed
};

[[nodiscard]] std::optional<NativeSourceLayout> native_source_layout_of(
    pixel::DataType data_type) noexcept {
	switch (data_type) {
	case pixel::DataType::u8:
		return NativeSourceLayout{8, 0};
	case pixel::DataType::s8:
		return NativeSourceLayout{8, 1};
	case pixel::DataType::u16:
		return NativeSourceLayout{16, 0};
	case pixel::DataType::s16:
		return NativeSourceLayout{16, 1};
	case pixel::DataType::u32:
		return NativeSourceLayout{32, 0};
	case pixel::DataType::s32:
		return NativeSourceLayout{32, 1};
	default:
		return std::nullopt;
	}
}

} // namespace

ResolvedEncodeSourceLayout resolve_encode_source_layout_or_throw(
    const pixel::PixelSource& source, std::string_view file_path) {
	const auto native_layout = native_source_layout_of(source.data_type);
	if (!native_layout) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} reason=source.data_type must be one of u8/s8/u16/s16/u32/s32 for current implementation",
		    file_path);
	}
	const auto bytes_per_sample = bytes_per_sample_of(source.data_type);
	if (bytes_per_sample == 0) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} reason=invalid source.data_type",
		    file_path);
	}

	if (source.rows <= 0 || source.cols <= 0 || source.frames <= 0 ||
	    source.samples_per_pixel <= 0) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} reason=rows/cols/frames/samples_per_pixel must be positive",
		    file_path);
	}
	if (source.rows > 65535 || source.cols > 65535) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} reason=rows/cols must be <= 65535",
		    file_path);
	}

	const auto rows = static_cast<std::size_t>(source.rows);
	const auto cols = static_cast<std::size_t>(source.cols);
	const auto frames = static_cast<std::size_t>(source.frames);
	const auto samples_per_pixel = static_cast<std::size_t>(source.samples_per_pixel);
	const bool planar_source =
	    source.planar == pixel::Planar::planar && samples_per_pixel > std::size_t{1};
	constexpr std::size_t kSizeMax = std::numeric_limits<std::size_t>::max();

	if (!planar_source && samples_per_pixel > kSizeMax / cols) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} reason=row samples overflows size_t",
		    file_path);
	}
	const std::size_t row_samples = planar_source ? cols : cols * samples_per_pixel;
	if (row_samples > kSizeMax / bytes_per_sample) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} reason=row payload bytes overflows size_t",
		    file_path);
	}
	const std::size_t row_payload_bytes = row_samples * bytes_per_sample;

	const std::size_t source_row_stride =
	    source.row_stride == 0 ? row_payload_bytes : source.row_stride;
	if (source_row_stride < row_payload_bytes) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} reason=row_stride({}) is smaller than row payload({})",
		    file_path, source_row_stride, row_payload_bytes);
	}
	if (source_row_stride > kSizeMax / rows) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} reason=source plane stride overflows size_t",
		    file_path);
	}
	const std::size_t source_plane_stride = source_row_stride * rows;

	std::size_t source_frame_size_bytes = source_plane_stride;
	if (planar_source) {
		if (samples_per_pixel > kSizeMax / source_plane_stride) {
			diag::error_and_throw(
			    "DicomFile::set_pixel_data file={} reason=source frame size bytes overflows size_t",
			    file_path);
		}
		source_frame_size_bytes = source_plane_stride * samples_per_pixel;
	}
	const std::size_t source_frame_stride =
	    source.frame_stride == 0 ? source_frame_size_bytes : source.frame_stride;
	if (source_frame_stride < source_frame_size_bytes) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} reason=frame_stride({}) is smaller than frame size({})",
		    file_path, source_frame_stride, source_frame_size_bytes);
	}

	if (row_payload_bytes > kSizeMax / rows) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} reason=destination plane stride overflows size_t",
		    file_path);
	}
	const std::size_t destination_plane_stride = row_payload_bytes * rows;
	std::size_t destination_frame_payload = destination_plane_stride;
	if (planar_source) {
		if (samples_per_pixel > kSizeMax / destination_plane_stride) {
			diag::error_and_throw(
			    "DicomFile::set_pixel_data file={} reason=destination frame size bytes overflows size_t",
			    file_path);
		}
		destination_frame_payload = destination_plane_stride * samples_per_pixel;
	}
	if (frames > kSizeMax / destination_frame_payload) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} reason=destination total bytes overflows size_t",
		    file_path);
	}
	const std::size_t destination_total_bytes = destination_frame_payload * frames;

	if (frames > std::size_t{1} && (frames - 1) > kSizeMax / source_frame_stride) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} reason=source last frame begin overflows size_t",
		    file_path);
	}
	const std::size_t source_last_frame_begin = (frames - 1) * source_frame_stride;
	const std::size_t source_last_plane_used =
	    (source_plane_stride - source_row_stride) + row_payload_bytes;
	const std::size_t source_last_frame_used = planar_source
	                                               ? (source_frame_size_bytes - source_plane_stride) +
	                                                     source_last_plane_used
	                                               : source_last_plane_used;
	if (source_last_frame_begin > kSizeMax - source_last_frame_used) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} reason=minimum source byte requirement overflows size_t",
		    file_path);
	}
	const std::size_t source_required_bytes = source_last_frame_begin + source_last_frame_used;
	if (source.bytes.size() < source_required_bytes) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} reason=source bytes({}) are shorter than required({})",
		    file_path, source.bytes.size(), source_required_bytes);
	}

	const int bits_allocated = native_layout->bits_allocated;
	const int bits_stored = source.bits_stored > 0 ? source.bits_stored : bits_allocated;
	const int pixel_representation = native_layout->pixel_representation;
	if (bits_stored <= 0 || bits_stored > bits_allocated) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} reason=bits_stored({}) must be in [1, bits_allocated({})]",
		    file_path, bits_stored, bits_allocated);
	}

	return ResolvedEncodeSourceLayout{
	    .bytes_per_sample = bytes_per_sample,
	    .rows = rows,
	    .cols = cols,
	    .frames = frames,
	    .samples_per_pixel = samples_per_pixel,
	    .planar_source = planar_source,
	    .row_payload_bytes = row_payload_bytes,
	    .source_row_stride = source_row_stride,
	    .source_plane_stride = source_plane_stride,
	    .source_frame_size_bytes = source_frame_size_bytes,
	    .source_frame_stride = source_frame_stride,
	    .destination_frame_payload = destination_frame_payload,
	    .destination_total_bytes = destination_total_bytes,
	    .bits_allocated = bits_allocated,
	    .bits_stored = bits_stored,
	    .pixel_representation = pixel_representation,
	    .high_bit = bits_stored - 1,
	};
}

} // namespace dicom::pixel::detail
