#include "pixel/host/decode/decode_modality_value_transform.hpp"

#include <optional>
#include <utility>

namespace dicom {
using namespace dicom::literals;

namespace pixel::detail {

namespace {

bool has_rescale_metadata(const DataSet& ds) {
	return static_cast<bool>(ds["RescaleSlope"_tag]) ||
	    static_cast<bool>(ds["RescaleIntercept"_tag]);
}

bool has_modality_lut_sequence(const DataSet& ds) {
	return static_cast<bool>(ds["ModalityLUTSequence"_tag]);
}

bool can_compute_modality_value_output(
    const pixel::PixelDataInfo& info, const DecodeOptions& opt) {
	if (!opt.to_modality_value) {
		return false;
	}
	if (!info.has_pixel_data) {
		return false;
	}
	return info.samples_per_pixel == 1;
}

std::optional<pixel::ModalityLut> load_modality_lut_for_modality_value_output(
    const DicomFile& df, const DataSet& ds, const pixel::PixelDataInfo& info,
    const DecodeOptions& opt) {
	if (!can_compute_modality_value_output(info, opt)) {
		return std::nullopt;
	}
	if (!has_modality_lut_sequence(ds)) {
		return std::nullopt;
	}
	// Validate and load LUT eagerly so malformed metadata still fails loudly.
	return df.modality_lut();
}

ModalityValueTransform build_modality_value_transform(const DataSet& ds,
    const pixel::PixelDataInfo& info, const DecodeOptions& opt,
    std::optional<pixel::ModalityLut> modality_lut) {
	ModalityValueTransform modality_value_transform{};
	if (!can_compute_modality_value_output(info, opt)) {
		return modality_value_transform;
	}

	if (has_modality_lut_sequence(ds)) {
		modality_value_transform.modality_lut = std::move(modality_lut);
		modality_value_transform.enabled = true;
		return modality_value_transform;
	}

	if (has_rescale_metadata(ds)) {
		modality_value_transform.rescale_slope =
		    ds["RescaleSlope"_tag].to_double().value_or(1.0);
		modality_value_transform.rescale_intercept =
		    ds["RescaleIntercept"_tag].to_double().value_or(0.0);
		modality_value_transform.enabled = true;
	}
	return modality_value_transform;
}

} // namespace

ModalityValueTransform compute_modality_value_transform(
    const DicomFile& df, const PixelDataInfo& info, const DecodeOptions& opt) {
	const auto& ds = df.dataset();
	auto modality_lut = load_modality_lut_for_modality_value_output(
	    df, ds, info, opt);
	return build_modality_value_transform(ds, info, opt, std::move(modality_lut));
}

} // namespace pixel::detail

} // namespace dicom
