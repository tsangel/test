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

[[nodiscard]] inline PixelLayout compute_decode_output_layout_or_throw(
    const DicomFile& file, const PixelLayout& source_layout,
    const DecodeOptions& effective_opt) {
	// Validate caller-controlled layout policy first so plan creation fails early
	// before any backend-specific work is scheduled.
	if (!is_valid_alignment(effective_opt.alignment)) {
		diag::error_and_throw(
		    "DicomFile::calc_decode_output_layout file={} reason=alignment must be 0/1 or power-of-two <= 4096",
		    file.path());
	}

	constexpr int kMaxRowsOrColumns = 65535;
	constexpr int kMaxSamplesPerPixel = 4;
	// Decode planning only supports normalized image metadata ranges that the
	// runtime and public API can represent safely.
	if (source_layout.empty()) {
		diag::error_and_throw(
		    "DicomFile::calc_decode_output_layout file={} reason=invalid Rows/Columns/SamplesPerPixel",
		    file.path());
	}
	if (source_layout.rows > static_cast<std::uint32_t>(kMaxRowsOrColumns) ||
	    source_layout.cols > static_cast<std::uint32_t>(kMaxRowsOrColumns)) {
		diag::error_and_throw(
		    "DicomFile::calc_decode_output_layout file={} reason=Rows/Columns must be <= 65535",
		    file.path());
	}
	if (source_layout.samples_per_pixel > static_cast<std::uint16_t>(kMaxSamplesPerPixel)) {
		diag::error_and_throw(
		    "DicomFile::calc_decode_output_layout file={} reason=SamplesPerPixel must be <= 4",
		    file.path());
	}

	if (source_layout.frames == 0) {
		diag::error_and_throw(
		    "DicomFile::calc_decode_output_layout file={} reason=invalid NumberOfFrames",
		    file.path());
	}

	if (bytes_per_sample_of(source_layout.data_type) == 0) {
		diag::error_and_throw(
		    "DicomFile::calc_decode_output_layout file={} reason=unsupported or unknown source layout dtype",
		    file.path());
	}

	// Start from semantic pixel metadata, then normalize it into a packed/aligned
	// storage layout that decode_into() can validate and use directly.
	const auto photometric = source_layout.photometric;
	PixelLayout layout{
	    .data_type = source_layout.data_type,
	    .photometric = photometric,
	    .planar = effective_opt.planar_out,
	    .reserved = 0,
	    .rows = source_layout.rows,
	    .cols = source_layout.cols,
	    .frames = source_layout.frames,
	    .samples_per_pixel = source_layout.samples_per_pixel,
	    .bits_stored = source_layout.bits_stored,
	    .row_stride = 0,
	    .frame_stride = 0,
	};
	try {
		// packed() is the single place that expands alignment into concrete strides.
		return layout.packed(effective_opt.alignment);
	} catch (const std::invalid_argument& e) {
		diag::error_and_throw(
		    "DicomFile::calc_decode_output_layout file={} reason={}",
		    file.path(), e.what());
	} catch (const std::overflow_error& e) {
		diag::error_and_throw(
		    "DicomFile::calc_decode_output_layout file={} reason={}",
		    file.path(), e.what());
	}
	diag::error_and_throw(
	    "DicomFile::calc_decode_output_layout file={} reason=unexpected layout computation failure",
	    file.path());
}

} // namespace dicom::pixel::detail
