#include "transform_detail.hpp"

#include <cstdlib>

namespace dicom::pixel {

namespace {

using transform_detail::MonochromeTransformLayoutInfo;
using transform_detail::dispatch_float_destination_dtype;
using transform_detail::dispatch_numeric_source_dtype;
using transform_detail::throw_transform_argument_error;
using transform_detail::validate_monochrome_transform_pair_or_throw;

[[nodiscard]] bool is_scalar_rescale_forced() noexcept {
	const char* value = std::getenv("DICOMSDL_FORCE_SCALAR_RESCALE");
	return value != nullptr && value[0] != '\0' && value[0] != '0';
}

#if defined(_MSC_VER)
#define DICOMSDL_RESTRICT __restrict
#else
#define DICOMSDL_RESTRICT __restrict__
#endif

template <typename Src, typename Dst>
inline void apply_rescale_row_autovec(const Src* DICOMSDL_RESTRICT src_values,
    Dst* DICOMSDL_RESTRICT dst_values, std::size_t pixel_count, Dst slope,
    Dst intercept) noexcept {
	// This loop is intentionally structured for compiler auto-vectorization.
	// Keep the body simple and contiguous so Clang/MSVC can lower it to SIMD.
#if defined(__clang__)
#pragma clang loop vectorize(enable)
#elif defined(_MSC_VER)
#pragma loop(ivdep)
#endif
	for (std::size_t index = 0; index < pixel_count; ++index) {
		dst_values[index] = static_cast<Dst>(src_values[index]) * slope + intercept;
	}
}

template <typename Src, typename Dst>
void apply_rescale_into_typed_impl(ConstPixelSpan src, PixelSpan dst,
    const MonochromeTransformLayoutInfo& layout_info, Dst slope,
    Dst intercept) noexcept {
	const auto* src_bytes = src.bytes.data();
	auto* dst_bytes = dst.bytes.data();
	const auto rows = static_cast<std::size_t>(src.layout.rows);
	const auto frames = static_cast<std::size_t>(src.layout.frames);

	for (std::size_t frame_index = 0; frame_index < frames; ++frame_index) {
		const auto src_frame_offset = frame_index * src.layout.frame_stride;
		const auto dst_frame_offset = frame_index * dst.layout.frame_stride;
		for (std::size_t row_index = 0; row_index < rows; ++row_index) {
			const auto* src_row = reinterpret_cast<const Src*>(
			    src_bytes + src_frame_offset + row_index * src.layout.row_stride);
			auto* dst_row = reinterpret_cast<Dst*>(
			    dst_bytes + dst_frame_offset + row_index * dst.layout.row_stride);
			apply_rescale_row_autovec(
			    src_row, dst_row, layout_info.row_pixel_count, slope, intercept);
		}
	}
}

template <typename Src, typename Dst>
void apply_rescale_frames_into_typed_impl(ConstPixelSpan src, PixelSpan dst,
    const MonochromeTransformLayoutInfo& layout_info, std::span<const float> slopes,
    std::span<const float> intercepts) noexcept {
	const auto* src_bytes = src.bytes.data();
	auto* dst_bytes = dst.bytes.data();
	const auto rows = static_cast<std::size_t>(src.layout.rows);
	const auto frames = static_cast<std::size_t>(src.layout.frames);

	for (std::size_t frame_index = 0; frame_index < frames; ++frame_index) {
		const auto src_frame_offset = frame_index * src.layout.frame_stride;
		const auto dst_frame_offset = frame_index * dst.layout.frame_stride;
		const Dst slope = static_cast<Dst>(slopes[frame_index]);
		const Dst intercept = static_cast<Dst>(intercepts[frame_index]);
		for (std::size_t row_index = 0; row_index < rows; ++row_index) {
			const auto* src_row = reinterpret_cast<const Src*>(
			    src_bytes + src_frame_offset + row_index * src.layout.row_stride);
			auto* dst_row = reinterpret_cast<Dst*>(
			    dst_bytes + dst_frame_offset + row_index * dst.layout.row_stride);
			apply_rescale_row_autovec(
			    src_row, dst_row, layout_info.row_pixel_count, slope, intercept);
		}
	}
}

template <typename Dst>
[[nodiscard]] bool try_apply_rescale_into_autovec(ConstPixelSpan src, PixelSpan dst,
    const MonochromeTransformLayoutInfo& layout_info, float slope,
    float intercept) noexcept {
	if (is_scalar_rescale_forced() || src.layout.data_type == DataType::unknown) {
		return false;
	}

	switch (src.layout.data_type) {
	case DataType::u8:
		if (!src.is_typed_row_access_aligned<std::uint8_t>() ||
		    !dst.is_typed_row_access_aligned<Dst>()) {
			return false;
		}
		apply_rescale_into_typed_impl<std::uint8_t, Dst>(
		    src, dst, layout_info, static_cast<Dst>(slope), static_cast<Dst>(intercept));
		return true;
	case DataType::s8:
		if (!src.is_typed_row_access_aligned<std::int8_t>() ||
		    !dst.is_typed_row_access_aligned<Dst>()) {
			return false;
		}
		apply_rescale_into_typed_impl<std::int8_t, Dst>(
		    src, dst, layout_info, static_cast<Dst>(slope), static_cast<Dst>(intercept));
		return true;
	case DataType::u16:
		if (!src.is_typed_row_access_aligned<std::uint16_t>() ||
		    !dst.is_typed_row_access_aligned<Dst>()) {
			return false;
		}
		apply_rescale_into_typed_impl<std::uint16_t, Dst>(
		    src, dst, layout_info, static_cast<Dst>(slope), static_cast<Dst>(intercept));
		return true;
	case DataType::s16:
		if (!src.is_typed_row_access_aligned<std::int16_t>() ||
		    !dst.is_typed_row_access_aligned<Dst>()) {
			return false;
		}
		apply_rescale_into_typed_impl<std::int16_t, Dst>(
		    src, dst, layout_info, static_cast<Dst>(slope), static_cast<Dst>(intercept));
		return true;
	case DataType::u32:
		if (!src.is_typed_row_access_aligned<std::uint32_t>() ||
		    !dst.is_typed_row_access_aligned<Dst>()) {
			return false;
		}
		apply_rescale_into_typed_impl<std::uint32_t, Dst>(
		    src, dst, layout_info, static_cast<Dst>(slope), static_cast<Dst>(intercept));
		return true;
	case DataType::s32:
		if (!src.is_typed_row_access_aligned<std::int32_t>() ||
		    !dst.is_typed_row_access_aligned<Dst>()) {
			return false;
		}
		apply_rescale_into_typed_impl<std::int32_t, Dst>(
		    src, dst, layout_info, static_cast<Dst>(slope), static_cast<Dst>(intercept));
		return true;
	case DataType::f32:
		if (!src.is_typed_row_access_aligned<float>() ||
		    !dst.is_typed_row_access_aligned<Dst>()) {
			return false;
		}
		apply_rescale_into_typed_impl<float, Dst>(
		    src, dst, layout_info, static_cast<Dst>(slope), static_cast<Dst>(intercept));
		return true;
	case DataType::f64:
		if (!src.is_typed_row_access_aligned<double>() ||
		    !dst.is_typed_row_access_aligned<Dst>()) {
			return false;
		}
		apply_rescale_into_typed_impl<double, Dst>(
		    src, dst, layout_info, static_cast<Dst>(slope), static_cast<Dst>(intercept));
		return true;
	default:
		return false;
	}
}

template <typename Dst>
[[nodiscard]] bool try_apply_rescale_frames_into_autovec(ConstPixelSpan src, PixelSpan dst,
    const MonochromeTransformLayoutInfo& layout_info, std::span<const float> slopes,
    std::span<const float> intercepts) noexcept {
	if (is_scalar_rescale_forced() || src.layout.data_type == DataType::unknown) {
		return false;
	}

	switch (src.layout.data_type) {
	case DataType::u8:
		if (!src.is_typed_row_access_aligned<std::uint8_t>() ||
		    !dst.is_typed_row_access_aligned<Dst>()) {
			return false;
		}
		apply_rescale_frames_into_typed_impl<std::uint8_t, Dst>(
		    src, dst, layout_info, slopes, intercepts);
		return true;
	case DataType::s8:
		if (!src.is_typed_row_access_aligned<std::int8_t>() ||
		    !dst.is_typed_row_access_aligned<Dst>()) {
			return false;
		}
		apply_rescale_frames_into_typed_impl<std::int8_t, Dst>(
		    src, dst, layout_info, slopes, intercepts);
		return true;
	case DataType::u16:
		if (!src.is_typed_row_access_aligned<std::uint16_t>() ||
		    !dst.is_typed_row_access_aligned<Dst>()) {
			return false;
		}
		apply_rescale_frames_into_typed_impl<std::uint16_t, Dst>(
		    src, dst, layout_info, slopes, intercepts);
		return true;
	case DataType::s16:
		if (!src.is_typed_row_access_aligned<std::int16_t>() ||
		    !dst.is_typed_row_access_aligned<Dst>()) {
			return false;
		}
		apply_rescale_frames_into_typed_impl<std::int16_t, Dst>(
		    src, dst, layout_info, slopes, intercepts);
		return true;
	case DataType::u32:
		if (!src.is_typed_row_access_aligned<std::uint32_t>() ||
		    !dst.is_typed_row_access_aligned<Dst>()) {
			return false;
		}
		apply_rescale_frames_into_typed_impl<std::uint32_t, Dst>(
		    src, dst, layout_info, slopes, intercepts);
		return true;
	case DataType::s32:
		if (!src.is_typed_row_access_aligned<std::int32_t>() ||
		    !dst.is_typed_row_access_aligned<Dst>()) {
			return false;
		}
		apply_rescale_frames_into_typed_impl<std::int32_t, Dst>(
		    src, dst, layout_info, slopes, intercepts);
		return true;
	case DataType::f32:
		if (!src.is_typed_row_access_aligned<float>() ||
		    !dst.is_typed_row_access_aligned<Dst>()) {
			return false;
		}
		apply_rescale_frames_into_typed_impl<float, Dst>(
		    src, dst, layout_info, slopes, intercepts);
		return true;
	case DataType::f64:
		if (!src.is_typed_row_access_aligned<double>() ||
		    !dst.is_typed_row_access_aligned<Dst>()) {
			return false;
		}
		apply_rescale_frames_into_typed_impl<double, Dst>(
		    src, dst, layout_info, slopes, intercepts);
		return true;
	default:
		return false;
	}
}

#undef DICOMSDL_RESTRICT

} // namespace

PixelLayout make_rescale_output_layout(PixelLayout src, DataType dst_type) {
	// Rescale does not change geometry or component arrangement, only the sample dtype.
	if (dst_type != DataType::f32 && dst_type != DataType::f64) {
		throw std::invalid_argument(
		    "make_rescale_output_layout: destination dtype must be float32 or float64");
	}
	return src.with_data_type(dst_type).packed();
}

PixelBuffer apply_rescale(ConstPixelSpan src, float slope, float intercept) {
	// The owning convenience overload simply allocates the canonical packed output.
	auto dst = PixelBuffer::allocate(make_rescale_output_layout(src.layout));
	apply_rescale_into(src, dst.span(), slope, intercept);
	return dst;
}

void apply_rescale_into(ConstPixelSpan src, PixelSpan dst, float slope, float intercept) {
	// Validate source/destination layout contracts before touching caller memory.
	const auto layout_info =
	    validate_monochrome_transform_pair_or_throw(src, dst, "apply_rescale_into");
	switch (dst.layout.data_type) {
	case DataType::f32:
		if (try_apply_rescale_into_autovec<float>(src, dst, layout_info, slope, intercept)) {
			return;
		}
		break;
	case DataType::f64:
		if (try_apply_rescale_into_autovec<double>(src, dst, layout_info, slope, intercept)) {
			return;
		}
		break;
	default:
		break;
	}

	// Dispatch once on destination dtype so the inner loops can stay strongly typed.
	dispatch_float_destination_dtype(
	    src, dst, layout_info, "apply_rescale_into",
	    [&]<typename Dst>(ConstPixelSpan src_view, PixelSpan dst_view,
	        const MonochromeTransformLayoutInfo& info) {
		    dispatch_numeric_source_dtype<Dst>(
		        src_view, dst_view, info, "apply_rescale_into",
		        [=](auto stored_value, std::size_t /*frame_index*/) -> double {
			        return static_cast<double>(stored_value) * static_cast<double>(slope) +
			            static_cast<double>(intercept);
		        });
	    });
}

void apply_rescale_frames_into(ConstPixelSpan src, PixelSpan dst,
    std::span<const float> slopes, std::span<const float> intercepts) {
	// Frame-wise rescale still keeps layout identical, but requires one coefficient pair
	// for each logical frame.
	const auto layout_info =
	    validate_monochrome_transform_pair_or_throw(
	        src, dst, "apply_rescale_frames_into");
	const auto frame_count = static_cast<std::size_t>(src.layout.frames);
	if (slopes.size() != frame_count || intercepts.size() != frame_count) {
		throw_transform_argument_error(
		    "apply_rescale_frames_into",
		    "slopes and intercepts must both match the source frame count");
	}
	switch (dst.layout.data_type) {
	case DataType::f32:
		if (try_apply_rescale_frames_into_autovec<float>(
		        src, dst, layout_info, slopes, intercepts)) {
			return;
		}
		break;
	case DataType::f64:
		if (try_apply_rescale_frames_into_autovec<double>(
		        src, dst, layout_info, slopes, intercepts)) {
			return;
		}
		break;
	default:
		break;
	}

	dispatch_float_destination_dtype(
	    src, dst, layout_info, "apply_rescale_frames_into",
	    [&]<typename Dst>(ConstPixelSpan src_view, PixelSpan dst_view,
	        const MonochromeTransformLayoutInfo& info) {
		    dispatch_numeric_source_dtype<Dst>(
		        src_view, dst_view, info, "apply_rescale_frames_into",
		        [&](auto stored_value, std::size_t frame_index) -> double {
			        return static_cast<double>(stored_value) *
			                   static_cast<double>(slopes[frame_index]) +
			            static_cast<double>(intercepts[frame_index]);
		        });
	    });
}

} // namespace dicom::pixel
