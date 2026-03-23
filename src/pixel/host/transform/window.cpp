#include "transform_detail.hpp"

#include <cmath>
#include <limits>

namespace dicom::pixel {

namespace {

using transform_detail::MonochromeTransformLayoutInfo;
using transform_detail::dispatch_numeric_source_dtype;
using transform_detail::throw_transform_argument_error;
using transform_detail::validate_monochrome_storage_pair_or_throw;

void validate_window_transform_or_throw(
    const WindowTransform& window) {
	// Window transforms require finite center/width metadata before any sample mapping starts.
	if (!std::isfinite(window.center) || !std::isfinite(window.width)) {
		throw_transform_argument_error("window center and width must be finite");
	}

	switch (window.function) {
	case VoiLutFunction::linear:
		if (window.width < 1.0f) {
			throw_transform_argument_error("LINEAR window width must be >= 1");
		}
		return;
	case VoiLutFunction::linear_exact:
	case VoiLutFunction::sigmoid:
		if (window.width <= 0.0f) {
			throw_transform_argument_error("window width must be > 0");
		}
		return;
	default:
		throw_transform_argument_error("VOI LUT function is not supported");
	}
}

[[nodiscard]] MonochromeTransformLayoutInfo validate_window_transform_pair_or_throw(
    ConstPixelSpan src, PixelSpan dst) {
	const auto info = validate_monochrome_storage_pair_or_throw(src, dst);

	// Display window output is currently materialized into integer grayscale storage.
	if (dst.layout.data_type != DataType::u8 && dst.layout.data_type != DataType::u16) {
		throw_transform_argument_error("destination dtype must be uint8 or uint16");
	}

	return info;
}

[[nodiscard]] inline double apply_linear_window_normalized(
    double value, const WindowTransform& window) noexcept {
	if (window.width <= 1.0f) {
		return value > (static_cast<double>(window.center) - 0.5) ? 1.0 : 0.0;
	}

	const double low =
	    static_cast<double>(window.center) - 0.5 - (static_cast<double>(window.width) - 1.0) / 2.0;
	const double high =
	    static_cast<double>(window.center) - 0.5 + (static_cast<double>(window.width) - 1.0) / 2.0;
	if (value <= low) {
		return 0.0;
	}
	if (value > high) {
		return 1.0;
	}
	return (value - low) / (high - low);
}

[[nodiscard]] inline double apply_linear_exact_window_normalized(
    double value, const WindowTransform& window) noexcept {
	const double low =
	    static_cast<double>(window.center) - static_cast<double>(window.width) / 2.0;
	const double high =
	    static_cast<double>(window.center) + static_cast<double>(window.width) / 2.0;
	if (value <= low) {
		return 0.0;
	}
	if (value > high) {
		return 1.0;
	}
	return (value - low) / (high - low);
}

[[nodiscard]] inline double apply_sigmoid_window_normalized(
    double value, const WindowTransform& window) noexcept {
	const double exponent =
	    -4.0 * (value - static_cast<double>(window.center)) / static_cast<double>(window.width);
	return 1.0 / (1.0 + std::exp(exponent));
}

template <typename Dst>
[[nodiscard]] inline Dst quantize_window_output(double normalized_value) noexcept {
	const double clamped = std::clamp(normalized_value, 0.0, 1.0);
	const double scaled =
	    clamped * static_cast<double>(std::numeric_limits<Dst>::max());
	return static_cast<Dst>(scaled);
}

template <typename Dst>
void apply_window_into_impl(ConstPixelSpan src, PixelSpan dst,
    const MonochromeTransformLayoutInfo& layout_info,
    const WindowTransform& window) {
	// Dispatch once on the source dtype so the hot loop only performs scalar math and stores.
	dispatch_numeric_source_dtype<Dst>(
	    src, dst, layout_info,
	    [&](auto stored_value, std::size_t /*frame_index*/) -> Dst {
		    const double value = static_cast<double>(stored_value);
		    const double normalized = [&]() noexcept {
			    switch (window.function) {
			    case VoiLutFunction::linear:
				    return apply_linear_window_normalized(value, window);
			    case VoiLutFunction::linear_exact:
				    return apply_linear_exact_window_normalized(value, window);
			    case VoiLutFunction::sigmoid:
				    return apply_sigmoid_window_normalized(value, window);
			    default:
				    return 0.0;
			    }
		    }();
		    return quantize_window_output<Dst>(normalized);
	    });
}

} // namespace

PixelLayout make_window_output_layout(PixelLayout src, DataType dst_type) {
	// Windowing preserves geometry while materializing display-oriented grayscale output.
	if (dst_type != DataType::u8 && dst_type != DataType::u16) {
		throw std::invalid_argument(
		    "make_window_output_layout: destination dtype must be uint8 or uint16");
	}
	return src.with_data_type(dst_type).packed();
}

PixelBuffer apply_window(ConstPixelSpan src, const WindowTransform& window) {
	// The owning convenience overload allocates the canonical packed display buffer.
	auto dst = PixelBuffer::allocate(make_window_output_layout(src.layout));
	apply_window_into(src, dst.span(), window);
	return dst;
}

void apply_window_into(ConstPixelSpan src, PixelSpan dst, const WindowTransform& window) {
	// Validate metadata and layout constraints before entering the sample loop.
	validate_window_transform_or_throw(window);
	const auto layout_info = validate_window_transform_pair_or_throw(src, dst);

	switch (dst.layout.data_type) {
	case DataType::u8:
		apply_window_into_impl<std::uint8_t>(src, dst, layout_info, window);
		return;
	case DataType::u16:
		apply_window_into_impl<std::uint16_t>(src, dst, layout_info, window);
		return;
	default:
		throw_transform_argument_error("destination dtype must be uint8 or uint16");
	}
}

} // namespace dicom::pixel
