#include "pixel/host/encode/encode_metadata_updater.hpp"

#include "pixel/host/encode/encode_target_policy.hpp"
#include "pixel/host/error/codec_error.hpp"
#include "photometric_text_detail.hpp"
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

[[nodiscard]] std::size_t encoded_payload_size_from_frame_or_throw(
    const PixelFrame& frame, std::size_t frame_index) {
	const auto contiguous_bytes = frame.encoded_data_size();
	if (contiguous_bytes != 0) {
		return contiguous_bytes;
	}

	const auto& fragments = frame.fragments();
	if (fragments.empty()) {
		throw_frame_codec_stage_exception(frame_index,
		    CodecStatusCode::internal_error, "update_lossy_metadata",
		    "encoded frame has no payload while updating lossy metadata");
	}

	std::size_t total_bytes = 0;
	constexpr std::size_t kSizeMax = std::numeric_limits<std::size_t>::max();
	for (const auto& fragment : fragments) {
		if (total_bytes > kSizeMax - fragment.length) {
			throw_codec_stage_exception(CodecStatusCode::internal_error,
			    "update_lossy_metadata",
			    "encoded payload size overflows size_t while updating lossy metadata");
		}
		total_bytes += fragment.length;
	}
	return total_bytes;
}

}  // namespace

std::size_t encoded_payload_size_from_pixel_sequence(const DataSet& dataset) {
	const auto& pixel_data = dataset["PixelData"_tag];
	if (!pixel_data || !pixel_data.vr().is_pixel_sequence()) {
		throw_codec_stage_exception(CodecStatusCode::invalid_argument,
		    "update_lossy_metadata",
		    "lossy metadata update requires encapsulated PixelData sequence");
	}
	const auto* pixel_sequence = pixel_data.as_pixel_sequence();
	if (!pixel_sequence) {
		throw_codec_stage_exception(CodecStatusCode::invalid_argument,
		    "update_lossy_metadata",
		    "lossy metadata update requires encapsulated PixelData sequence");
	}

	std::size_t encoded_payload_bytes = 0;
	constexpr std::size_t kSizeMax = std::numeric_limits<std::size_t>::max();
	for (std::size_t frame_index = 0; frame_index < pixel_sequence->number_of_frames();
	     ++frame_index) {
		const auto* frame = pixel_sequence->frame(frame_index);
		if (!frame) {
			throw_frame_codec_stage_exception(frame_index,
			    CodecStatusCode::internal_error, "update_lossy_metadata",
			    "encoded frame missing while updating lossy metadata");
		}
		const auto frame_bytes =
		    encoded_payload_size_from_frame_or_throw(*frame, frame_index);
		if (encoded_payload_bytes > kSizeMax - frame_bytes) {
			throw_codec_stage_exception(CodecStatusCode::internal_error,
			    "update_lossy_metadata",
			    "encoded payload size overflows size_t while updating lossy metadata");
		}
		encoded_payload_bytes += frame_bytes;
	}
	return encoded_payload_bytes;
}

void update_lossy_compression_metadata_for_set_pixel_data(DataSet& dataset,
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
			throw_codec_stage_exception(CodecStatusCode::internal_error,
			    "update_lossy_metadata",
			    "failed to update LossyImageCompression metadata for non-lossy encode");
		}
		return;
	}

	if (encoded_payload_bytes == 0 || uncompressed_payload_bytes == 0) {
		throw_codec_stage_exception(CodecStatusCode::invalid_argument,
		    "update_lossy_metadata",
		    "cannot compute lossy compression ratio from zero payload size (uncompressed={} encoded={})",
		    uncompressed_payload_bytes, encoded_payload_bytes);
	}
	const auto method = lossy_method_for_encode_profile(codec_profile_code);
	if (!method) {
		throw_codec_stage_exception(CodecStatusCode::internal_error,
		    "update_lossy_metadata",
		    "missing lossy compression method mapping for transfer syntax");
	}

	const double ratio = static_cast<double>(uncompressed_payload_bytes) /
	    static_cast<double>(encoded_payload_bytes);
	if (!std::isfinite(ratio) || ratio <= 0.0) {
		throw_codec_stage_exception(CodecStatusCode::internal_error,
		    "update_lossy_metadata",
		    "invalid lossy compression ratio computed from uncompressed={} encoded={}",
		    uncompressed_payload_bytes, encoded_payload_bytes);
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
		throw_codec_stage_exception(CodecStatusCode::internal_error,
		    "update_lossy_metadata",
		    "failed to update lossy compression metadata ratio={} method={} history_vm={}",
		    ratio, *method, methods.size());
	}
}

void update_pixel_metadata_for_set_pixel_data(DataSet& dataset,
    const pixel::PixelLayout& source_layout, bool target_is_rle,
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

	const auto photometric_text = pixel::detail::to_photometric_text(output_photometric);
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
		throw_codec_stage_exception(CodecStatusCode::internal_error,
		    "update_pixel_metadata",
		    "failed to update one or more pixel metadata tags requested rows={} cols={} frames={} spp={} bits_allocated={} bits_stored={} high_bit={} pixel_representation={} photometric={} planar={} row_stride={} frame_stride={}",
		    source_layout.rows, source_layout.cols, source_layout.frames,
		    source_layout.samples_per_pixel, bits_allocated, bits_stored, high_bit,
		    pixel_representation, photometric_text,
		    source_layout.planar == pixel::Planar::planar ? "planar" : "interleaved",
		    source_row_stride, source_frame_stride);
	}
}

} // namespace dicom::pixel::detail
