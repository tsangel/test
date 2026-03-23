#include "transform_detail.hpp"

namespace dicom::pixel {

using transform_detail::MonochromeTransformLayoutInfo;
using transform_detail::dispatch_float_destination_dtype;
using transform_detail::dispatch_integral_source_dtype;
using transform_detail::lut_covers_integer_stored_range;
using transform_detail::lookup_modality_lut_value;
using transform_detail::throw_transform_argument_error;
using transform_detail::validate_monochrome_transform_pair_or_throw;

PixelLayout make_modality_lut_output_layout(PixelLayout src) {
	// The default Modality LUT output stays packed and widens to float32.
	return src.with_data_type(DataType::f32).packed();
}

PixelBuffer apply_modality_lut(ConstPixelSpan src, const ModalityLut& lut) {
	// The owning convenience overload allocates the default packed float32 destination.
	auto dst = PixelBuffer::allocate(make_modality_lut_output_layout(src.layout));
	apply_modality_lut_into(src, dst.span(), lut);
	return dst;
}

void apply_modality_lut_into(ConstPixelSpan src, PixelSpan dst, const ModalityLut& lut) {
	// Modality LUT requires one stored-value sample per pixel and a non-empty lookup table.
	const auto layout_info = validate_monochrome_transform_pair_or_throw(src, dst);
	if (lut.values.empty()) {
		throw_transform_argument_error("Modality LUT values must not be empty");
	}
	const bool clamp_indices =
	    !lut_covers_integer_stored_range(src.layout, lut.first_mapped, lut.values.size());

	dispatch_float_destination_dtype(
	    src, dst, layout_info,
	    [&]<typename Dst>(ConstPixelSpan src_view, PixelSpan dst_view,
	        const MonochromeTransformLayoutInfo& info) {
		    dispatch_integral_source_dtype<Dst>(
		        src_view, dst_view, info,
		        [&](auto stored_value, std::size_t /*frame_index*/) -> double {
			        if (clamp_indices) {
				        return static_cast<double>(lookup_modality_lut_value<true>(
				            lut, static_cast<std::int64_t>(stored_value)));
			        }
			        return static_cast<double>(lookup_modality_lut_value<false>(
			            lut, static_cast<std::int64_t>(stored_value)));
		        });
	    });
}

} // namespace dicom::pixel
