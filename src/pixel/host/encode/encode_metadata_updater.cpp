#include "pixel/host/encode/encode_metadata_updater.hpp"

#include "pixel/host/encode/encode_target_policy.hpp"
#include "diagnostics.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dicom::pixel::detail {
using namespace dicom::literals;

namespace {

[[nodiscard]] std::string_view to_photometric_text(pixel::Photometric photometric) noexcept {
	switch (photometric) {
	case pixel::Photometric::monochrome1:
		return "MONOCHROME1";
	case pixel::Photometric::monochrome2:
		return "MONOCHROME2";
	case pixel::Photometric::palette_color:
		return "PALETTE COLOR";
	case pixel::Photometric::rgb:
		return "RGB";
	case pixel::Photometric::ybr_full:
		return "YBR_FULL";
	case pixel::Photometric::ybr_full_422:
		return "YBR_FULL_422";
	case pixel::Photometric::ybr_rct:
		return "YBR_RCT";
	case pixel::Photometric::ybr_ict:
		return "YBR_ICT";
	default:
		return "MONOCHROME2";
	}
}

} // namespace

std::size_t encoded_payload_size_from_pixel_sequence(const DataSet& dataset,
    std::string_view file_path, uid::WellKnown transfer_syntax) {
	const auto& pixel_data = dataset["PixelData"_tag];
	if (!pixel_data || !pixel_data.vr().is_pixel_sequence()) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} ts={} reason=lossy metadata update requires encapsulated PixelData sequence",
		    file_path, transfer_syntax.value());
	}
	const auto* pixel_sequence = pixel_data.as_pixel_sequence();
	if (!pixel_sequence) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} ts={} reason=lossy metadata update requires encapsulated PixelData sequence",
		    file_path, transfer_syntax.value());
	}

	std::size_t encoded_payload_bytes = 0;
	constexpr std::size_t kSizeMax = std::numeric_limits<std::size_t>::max();
	for (std::size_t frame_index = 0; frame_index < pixel_sequence->number_of_frames();
	     ++frame_index) {
		const auto* frame = pixel_sequence->frame(frame_index);
		if (!frame) {
			diag::error_and_throw(
			    "DicomFile::set_pixel_data file={} ts={} reason=encoded frame {} missing while updating lossy metadata",
			    file_path, transfer_syntax.value(), frame_index);
		}
		const auto frame_bytes = frame->encoded_data_size();
		if (encoded_payload_bytes > kSizeMax - frame_bytes) {
			diag::error_and_throw(
			    "DicomFile::set_pixel_data file={} ts={} reason=encoded payload size overflows size_t while updating lossy metadata",
			    file_path, transfer_syntax.value());
		}
		encoded_payload_bytes += frame_bytes;
	}
	return encoded_payload_bytes;
}

void update_lossy_compression_metadata_for_set_pixel_data(DataSet& dataset,
    std::string_view file_path, uid::WellKnown transfer_syntax,
    uint32_t codec_profile_code, std::size_t uncompressed_payload_bytes,
    std::size_t encoded_payload_bytes) {
	const bool current_encode_is_lossy =
	    encode_profile_uses_lossy_compression(codec_profile_code);
	const bool had_prior_lossy =
	    dataset.get_value<std::string_view>("LossyImageCompression"_tag)
	        .value_or(std::string_view()) == "01";

	bool ok = true;
	if (!current_encode_is_lossy) {
		ok &= dataset.set_value("LossyImageCompression"_tag,
		    std::string_view(had_prior_lossy ? "01" : "00"));
		if (!had_prior_lossy) {
			dataset.remove_dataelement("LossyImageCompressionRatio"_tag);
			dataset.remove_dataelement("LossyImageCompressionMethod"_tag);
		}
		if (!ok) {
			diag::error_and_throw(
			    "DicomFile::set_pixel_data file={} ts={} reason=failed to update LossyImageCompression metadata for non-lossy encode",
			    file_path, transfer_syntax.value());
		}
		return;
	}

	if (encoded_payload_bytes == 0 || uncompressed_payload_bytes == 0) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} ts={} reason=cannot compute lossy compression ratio from zero payload size (uncompressed={} encoded={})",
		    file_path, transfer_syntax.value(), uncompressed_payload_bytes,
		    encoded_payload_bytes);
	}
	const auto method = lossy_method_for_encode_profile(codec_profile_code);
	if (!method) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} ts={} reason=missing lossy compression method mapping for transfer syntax",
		    file_path, transfer_syntax.value());
	}

	const double ratio = static_cast<double>(uncompressed_payload_bytes) /
	    static_cast<double>(encoded_payload_bytes);
	if (!std::isfinite(ratio) || ratio <= 0.0) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} ts={} reason=invalid lossy compression ratio computed from uncompressed={} encoded={}",
		    file_path, transfer_syntax.value(), uncompressed_payload_bytes,
		    encoded_payload_bytes);
	}

	std::vector<std::string> methods;
	std::vector<double> ratios;
	if (had_prior_lossy) {
		methods = dataset.get_value<std::vector<std::string>>(
		              "LossyImageCompressionMethod"_tag)
		              .value_or(std::vector<std::string>{});
		ratios = dataset.get_value<std::vector<double>>(
		             "LossyImageCompressionRatio"_tag)
		             .value_or(std::vector<double>{});
		const auto paired_count = std::min(methods.size(), ratios.size());
		methods.resize(paired_count);
		ratios.resize(paired_count);
	}
	methods.emplace_back(*method);
	ratios.push_back(ratio);

	std::vector<std::string_view> method_views;
	method_views.reserve(methods.size());
	for (const auto& value : methods) {
		method_views.emplace_back(value);
	}

	ok &= dataset.set_value("LossyImageCompression"_tag, std::string_view("01"));
	ok &= dataset.set_value("LossyImageCompressionRatio"_tag,
	    std::span<const double>(ratios.data(), ratios.size()));
	ok &= dataset.set_value("LossyImageCompressionMethod"_tag,
	    std::span<const std::string_view>(method_views.data(), method_views.size()));

	if (!ok) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} ts={} reason=failed to update lossy compression metadata ratio={} method={} history_vm={}",
		    file_path, transfer_syntax.value(), ratio, *method, methods.size());
	}
}

void update_pixel_metadata_for_set_pixel_data(DataSet& dataset, std::string_view file_path,
    uid::WellKnown transfer_syntax, const pixel::PixelLayout& source_layout,
    bool target_is_rle,
    pixel::Photometric output_photometric, int bits_allocated, int bits_stored,
    int high_bit, int pixel_representation,
    std::size_t source_row_stride, std::size_t source_frame_stride) {
	bool ok = true;
	// Write the normalized geometry exactly once so every encode path uses the
	// same metadata source of truth.
	ok &= dataset.set_value("Rows"_tag, static_cast<long>(source_layout.rows));
	ok &= dataset.set_value("Columns"_tag, static_cast<long>(source_layout.cols));
	ok &= dataset.set_value(
	    "SamplesPerPixel"_tag, static_cast<long>(source_layout.samples_per_pixel));
	ok &= dataset.set_value("BitsAllocated"_tag, static_cast<long>(bits_allocated));
	ok &= dataset.set_value("BitsStored"_tag, static_cast<long>(bits_stored));
	ok &= dataset.set_value("HighBit"_tag, static_cast<long>(high_bit));
	ok &= dataset.set_value(
	    "PixelRepresentation"_tag, static_cast<long>(pixel_representation));

	const auto photometric_text = to_photometric_text(output_photometric);
	ok &= dataset.set_value("PhotometricInterpretation"_tag, photometric_text);

	if (source_layout.frames > 1u) {
		ok &= dataset.set_value(
		    "NumberOfFrames"_tag, static_cast<long>(source_layout.frames));
	} else {
		dataset.remove_dataelement("NumberOfFrames"_tag);
	}
	if (source_layout.samples_per_pixel > 1u) {
		const long planar_configuration = target_is_rle
		                                      ? 1L
		                                      : (source_layout.planar == pixel::Planar::planar ? 1L : 0L);
		ok &= dataset.set_value("PlanarConfiguration"_tag, planar_configuration);
	} else {
		dataset.remove_dataelement("PlanarConfiguration"_tag);
	}
	if (!ok) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} ts={} reason=failed to update one or more pixel metadata tags requested rows={} cols={} frames={} spp={} bits_allocated={} bits_stored={} high_bit={} pixel_representation={} photometric={} planar={} row_stride={} frame_stride={}",
		    file_path, transfer_syntax.value(), source_layout.rows, source_layout.cols,
		    source_layout.frames, source_layout.samples_per_pixel, bits_allocated,
		    bits_stored, high_bit,
		    pixel_representation, photometric_text,
		    source_layout.planar == pixel::Planar::planar ? "planar" : "interleaved",
		    source_row_stride, source_frame_stride);
	}
}

} // namespace dicom::pixel::detail
