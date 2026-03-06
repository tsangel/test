#pragma once

#include "dicom.h"
#include "diagnostics.h"

#include <cstddef>
#include <limits>
#include <string_view>

namespace dicom::pixel::detail {

[[nodiscard]] inline bool is_valid_alignment(std::uint16_t alignment) noexcept {
	constexpr std::uint16_t kMaxAlignment = 4096;
	if (alignment == 0 || alignment == 1) {
		return true;
	}
	return alignment <= kMaxAlignment &&
	    (alignment & static_cast<std::uint16_t>(alignment - 1)) == 0;
}

// Compute packed/aligned output strides from the finalized decode plan inputs.
// - bytes_per_sample: float32 when modality output is enabled, otherwise from sv_dtype
// - row stride: cols * (planar ? 1 : samples_per_pixel) * bytes_per_sample, then alignment
// - frame stride: row stride * rows, and multiply by samples_per_pixel again for planar output
[[nodiscard]] inline DecodeStrides compute_decode_strides_or_throw(
    std::string_view file_path, const PixelDataInfo& info,
    const DecodeOptions& effective_opt) {
	if (!is_valid_alignment(effective_opt.alignment)) {
		diag::error_and_throw(
		    "DicomFile::calc_decode_strides file={} reason=alignment must be 0/1 or power-of-two <= 4096",
		    file_path);
	}

	constexpr int kMaxRowsOrColumns = 65535;
	constexpr int kMaxSamplesPerPixel = 4;
	if (info.rows <= 0 || info.cols <= 0 || info.samples_per_pixel <= 0) {
		diag::error_and_throw(
		    "DicomFile::calc_decode_strides file={} reason=invalid Rows/Columns/SamplesPerPixel",
		    file_path);
	}
	if (info.rows > kMaxRowsOrColumns || info.cols > kMaxRowsOrColumns) {
		diag::error_and_throw(
		    "DicomFile::calc_decode_strides file={} reason=Rows/Columns must be <= 65535",
		    file_path);
	}
	if (info.samples_per_pixel > kMaxSamplesPerPixel) {
		diag::error_and_throw(
		    "DicomFile::calc_decode_strides file={} reason=SamplesPerPixel must be <= 4",
		    file_path);
	}

	const auto rows = static_cast<std::size_t>(info.rows);
	const auto cols = static_cast<std::size_t>(info.cols);
	const auto samples_per_pixel = static_cast<std::size_t>(info.samples_per_pixel);
	const auto bytes_per_sample = effective_opt.to_modality_value
	                                  ? sizeof(float)
	                                  : bytes_per_sample_of(info.sv_dtype);
	if (bytes_per_sample == 0) {
		diag::error_and_throw(
		    "DicomFile::calc_decode_strides file={} reason=unsupported or unknown sv_dtype",
		    file_path);
	}

	const auto row_components = (effective_opt.planar_out == Planar::planar)
	                                ? std::size_t{1}
	                                : samples_per_pixel;
	std::size_t row_stride = cols * row_components * bytes_per_sample;
	const std::size_t alignment = (effective_opt.alignment <= 1)
	                                  ? std::size_t{1}
	                                  : static_cast<std::size_t>(effective_opt.alignment);
	if (alignment > 1) {
		row_stride = ((row_stride + alignment - 1) / alignment) * alignment;
	}

	std::size_t frame_stride = row_stride * rows;
	if (effective_opt.planar_out == Planar::planar) {
		frame_stride *= samples_per_pixel;
	}

	return DecodeStrides{row_stride, frame_stride};
}

} // namespace dicom::pixel::detail
