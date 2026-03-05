#include "pixel_/decode/core/decode_modality_transform.hpp"

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

bool can_resolve_scaled_decode_output(
    const pixel::PixelDataInfo& info, const DecodeOptions& opt) {
	if (!opt.scaled) {
		return false;
	}
	if (!info.has_pixel_data) {
		return false;
	}
	return info.samples_per_pixel == 1;
}

std::optional<pixel::ModalityLut> load_modality_lut_for_scaled_decode_output(
    const DicomFile& df, const DataSet& ds, const pixel::PixelDataInfo& info,
    const DecodeOptions& opt) {
	if (!can_resolve_scaled_decode_output(info, opt)) {
		return std::nullopt;
	}
	if (!has_modality_lut_sequence(ds)) {
		return std::nullopt;
	}
	// Validate and load LUT eagerly so malformed metadata still fails loudly.
	return df.modality_lut();
}

DecodeValueTransform build_decode_modality_transform(const DataSet& ds,
    const pixel::PixelDataInfo& info, const DecodeOptions& opt,
    std::optional<pixel::ModalityLut> modality_lut) {
	DecodeValueTransform value_transform{};
	if (!can_resolve_scaled_decode_output(info, opt)) {
		return value_transform;
	}

	if (has_modality_lut_sequence(ds)) {
		value_transform.modality_lut = std::move(modality_lut);
		value_transform.enabled = true;
		return value_transform;
	}

	if (has_rescale_metadata(ds)) {
		value_transform.rescale_slope =
		    ds["RescaleSlope"_tag].to_double().value_or(1.0);
		value_transform.rescale_intercept =
		    ds["RescaleIntercept"_tag].to_double().value_or(0.0);
		value_transform.enabled = true;
	}
	return value_transform;
}

} // namespace

ResolvedDecodeValueTransform resolve_decode_value_transform(
    const DicomFile& df, const DecodeOptions& opt) {
	const auto& info = df.pixeldata_info();
	const auto& ds = df.dataset();
	auto modality_lut = load_modality_lut_for_scaled_decode_output(
	    df, ds, info, opt);

	ResolvedDecodeValueTransform resolved{};
	resolved.transform = build_decode_modality_transform(
	    ds, info, opt, std::move(modality_lut));
	resolved.options = opt;
	resolved.options.scaled = resolved.transform.enabled;
	return resolved;
}

} // namespace pixel::detail

} // namespace dicom
