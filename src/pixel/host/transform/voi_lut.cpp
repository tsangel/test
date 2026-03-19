#include "transform_detail.hpp"

namespace dicom::pixel {

using transform_detail::MonochromeTransformLayoutInfo;
using transform_detail::VoiLutInfo;
using transform_detail::dispatch_integral_source_dtype;
using transform_detail::lookup_voi_lut_value;
using transform_detail::lut_covers_integer_stored_range;
using transform_detail::throw_transform_argument_error;
using transform_detail::validate_voi_lut_or_throw;
using transform_detail::validate_voi_lut_transform_pair_or_throw;

PixelLayout make_voi_lut_output_layout(PixelLayout src, const VoiLut& lut) {
	// VOI LUT preserves monochrome geometry and chooses output width from LUT metadata.
	const auto lut_info = validate_voi_lut_or_throw(lut, "make_voi_lut_output_layout");
	return src.with_data_type(lut_info.destination_dtype, lut_info.bits_per_entry).packed();
}

PixelBuffer apply_voi_lut(ConstPixelSpan src, const VoiLut& lut) {
	// The owning convenience overload allocates the canonical packed grayscale destination.
	auto dst = PixelBuffer::allocate(make_voi_lut_output_layout(src.layout, lut));
	apply_voi_lut_into(src, dst.span(), lut);
	return dst;
}

template <typename Dst>
void apply_voi_lut_into_impl(ConstPixelSpan src, PixelSpan dst,
    const MonochromeTransformLayoutInfo& layout_info, const VoiLut& lut,
    bool clamp_indices) {
	// Dispatch once on the source dtype so the hot loop only performs LUT index math and stores.
	dispatch_integral_source_dtype<Dst>(
	    src, dst, layout_info, "apply_voi_lut_into",
	    [&](auto stored_value, std::size_t /*frame_index*/) -> Dst {
		    if (clamp_indices) {
			    return static_cast<Dst>(lookup_voi_lut_value<true>(
			        lut, static_cast<std::int64_t>(stored_value)));
		    }
		    return static_cast<Dst>(lookup_voi_lut_value<false>(
		        lut, static_cast<std::int64_t>(stored_value)));
	    });
}

void apply_voi_lut_into(ConstPixelSpan src, PixelSpan dst, const VoiLut& lut) {
	// VOI LUT validation covers both table shape and source/destination layout rules.
	const auto lut_info = validate_voi_lut_or_throw(lut, "apply_voi_lut_into");
	const auto layout_info =
	    validate_voi_lut_transform_pair_or_throw(src, dst, lut_info, "apply_voi_lut_into");
	const bool clamp_indices =
	    !lut_covers_integer_stored_range(src.layout, lut.first_mapped, lut_info.entry_count);

	switch (lut_info.destination_dtype) {
	case DataType::u8:
		apply_voi_lut_into_impl<std::uint8_t>(src, dst, layout_info, lut, clamp_indices);
		return;
	case DataType::u16:
		apply_voi_lut_into_impl<std::uint16_t>(src, dst, layout_info, lut, clamp_indices);
		return;
	default:
		throw_transform_argument_error(
		    "apply_voi_lut_into", "VOI LUT destination dtype is not supported");
	}
}

} // namespace dicom::pixel
