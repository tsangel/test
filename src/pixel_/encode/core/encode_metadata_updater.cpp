#include "pixel_/encode/core/encode_metadata_updater.hpp"

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

[[nodiscard]] bool has_prior_lossy_history(const DataSet& dataset) {
	const auto& lossy = dataset["LossyImageCompression"_tag];
	const auto lossy_value = lossy.to_string_view();
	return lossy_value && *lossy_value == "01";
}

[[nodiscard]] std::vector<std::string> read_lossy_method_values(
    const DataSet& dataset) {
	std::vector<std::string> methods;
	const auto& method_elem = dataset["LossyImageCompressionMethod"_tag];
	const auto parsed = method_elem.to_string_views();
	if (!parsed) {
		return methods;
	}
	methods.reserve(parsed->size());
	for (const auto value : *parsed) {
		methods.emplace_back(value);
	}
	return methods;
}

[[nodiscard]] std::vector<double> read_lossy_ratio_values(const DataSet& dataset) {
	const auto& ratio_elem = dataset["LossyImageCompressionRatio"_tag];
	const auto parsed = ratio_elem.to_double_vector();
	if (!parsed) {
		return {};
	}
	return *parsed;
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
    const PixelEncodeTarget& target, std::size_t uncompressed_payload_bytes,
    std::size_t encoded_payload_bytes) {
	const bool current_encode_is_lossy = target_uses_lossy_compression(target);
	const bool had_prior_lossy = has_prior_lossy_history(dataset);

	bool ok = true;
	if (!current_encode_is_lossy) {
		auto* lossy_elem = dataset.add_dataelement("LossyImageCompression"_tag, VR::CS);
		ok &= lossy_elem &&
		    lossy_elem->from_string_view(had_prior_lossy ? "01" : "00");
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
	const auto method = lossy_method_for_target(target);
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
		methods = read_lossy_method_values(dataset);
		ratios = read_lossy_ratio_values(dataset);
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

	auto* lossy_elem = dataset.add_dataelement("LossyImageCompression"_tag, VR::CS);
	ok &= lossy_elem && lossy_elem->from_string_view("01");

	auto* ratio_elem = dataset.add_dataelement("LossyImageCompressionRatio"_tag, VR::DS);
	ok &= ratio_elem &&
	    ratio_elem->from_double_vector(std::span<const double>(ratios.data(), ratios.size()));

	auto* method_elem = dataset.add_dataelement("LossyImageCompressionMethod"_tag, VR::CS);
	ok &= method_elem &&
	    method_elem->from_string_views(std::span<const std::string_view>(
	        method_views.data(), method_views.size()));

	if (!ok) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} ts={} reason=failed to update lossy compression metadata ratio={} method={} history_vm={}",
		    file_path, transfer_syntax.value(), ratio, *method, methods.size());
	}
}

void update_pixel_metadata_for_set_pixel_data(DataSet& dataset, std::string_view file_path,
    uid::WellKnown transfer_syntax, const pixel::PixelSource& source, bool target_is_rle,
    pixel::Photometric output_photometric, int bits_allocated, int bits_stored,
    int high_bit, int pixel_representation,
    std::size_t source_row_stride, std::size_t source_frame_stride) {
	bool ok = true;
	ok &= dataset.add_dataelement("Rows"_tag, VR::US)
	          ->from_long(static_cast<long>(source.rows));
	ok &= dataset.add_dataelement("Columns"_tag, VR::US)
	          ->from_long(static_cast<long>(source.cols));
	ok &= dataset.add_dataelement("SamplesPerPixel"_tag, VR::US)
	          ->from_long(static_cast<long>(source.samples_per_pixel));
	ok &= dataset.add_dataelement("BitsAllocated"_tag, VR::US)
	          ->from_long(static_cast<long>(bits_allocated));
	ok &= dataset.add_dataelement("BitsStored"_tag, VR::US)
	          ->from_long(static_cast<long>(bits_stored));
	ok &= dataset.add_dataelement("HighBit"_tag, VR::US)
	          ->from_long(static_cast<long>(high_bit));
	ok &= dataset.add_dataelement("PixelRepresentation"_tag, VR::US)
	          ->from_long(static_cast<long>(pixel_representation));

	const auto photometric_text = to_photometric_text(output_photometric);
	ok &= dataset.add_dataelement("PhotometricInterpretation"_tag, VR::CS)
	          ->from_string_view(photometric_text);

	if (source.frames > 1) {
		ok &= dataset.add_dataelement("NumberOfFrames"_tag, VR::IS)
		          ->from_long(static_cast<long>(source.frames));
	} else {
		dataset.remove_dataelement("NumberOfFrames"_tag);
	}
	if (source.samples_per_pixel > 1) {
		const long planar_configuration = target_is_rle
		                                      ? 1L
		                                      : (source.planar == pixel::Planar::planar ? 1L : 0L);
		ok &= dataset.add_dataelement("PlanarConfiguration"_tag, VR::US)
		          ->from_long(planar_configuration);
	} else {
		dataset.remove_dataelement("PlanarConfiguration"_tag);
	}
	if (!ok) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} ts={} reason=failed to update one or more pixel metadata tags requested rows={} cols={} frames={} spp={} bits_allocated={} bits_stored={} high_bit={} pixel_representation={} photometric={} planar={} row_stride={} frame_stride={}",
		    file_path, transfer_syntax.value(), source.rows, source.cols, source.frames,
		    source.samples_per_pixel, bits_allocated, bits_stored, high_bit,
		    pixel_representation, photometric_text,
		    source.planar == pixel::Planar::planar ? "planar" : "interleaved",
		    source_row_stride, source_frame_stride);
	}
}

} // namespace dicom::pixel::detail
