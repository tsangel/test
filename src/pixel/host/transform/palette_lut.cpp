#include "transform_detail.hpp"

namespace dicom::pixel {

using transform_detail::dispatch_palette_source_dtype;
using transform_detail::lut_covers_integer_stored_range;
using transform_detail::throw_transform_argument_error;
using transform_detail::validate_palette_lut_or_throw;
using transform_detail::validate_palette_transform_pair_or_throw;

PixelLayout make_palette_output_layout(PixelLayout src, const PaletteLut& lut) {
	// Palette LUT expands indexed samples into RGB/RGBA while preserving frame geometry.
	const auto lut_info = validate_palette_lut_or_throw(lut);
	return src.with_samples(
	           lut_info.has_alpha ? std::uint16_t{4} : std::uint16_t{3},
	           Photometric::rgb, src.planar)
	    .with_data_type(lut_info.destination_dtype, lut_info.bits_per_entry)
	    .packed();
}

PixelBuffer apply_palette_lut(ConstPixelSpan src, const PaletteLut& lut) {
	// The owning convenience overload allocates the canonical packed RGB/RGBA destination.
	auto dst = PixelBuffer::allocate(make_palette_output_layout(src.layout, lut));
	apply_palette_lut_into(src, dst.span(), lut);
	return dst;
}

void apply_palette_lut_into(ConstPixelSpan src, PixelSpan dst, const PaletteLut& lut) {
	// Palette LUT validation covers both table shape and source/destination layout rules.
	const auto lut_info = validate_palette_lut_or_throw(lut);
	const auto layout_info = validate_palette_transform_pair_or_throw(src, dst, lut_info);
	const bool clamp_indices = !lut_covers_integer_stored_range(
	    src.layout, lut.first_mapped, lut_info.entry_count);

	switch (lut_info.destination_dtype) {
	case DataType::u8:
		dispatch_palette_source_dtype<std::uint8_t>(
		    src, dst, layout_info, lut, clamp_indices);
		return;
	case DataType::u16:
		dispatch_palette_source_dtype<std::uint16_t>(
		    src, dst, layout_info, lut, clamp_indices);
		return;
	default:
		throw_transform_argument_error("palette LUT destination dtype is not supported");
	}
}

} // namespace dicom::pixel
