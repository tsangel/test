#pragma once

#include "dicom.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace dicom::pixel::transform_detail {

struct LayoutAccessInfo {
	bool planar{false};
	std::size_t row_value_count{0};
	std::size_t plane_stride{0};
};

struct MonochromeTransformLayoutInfo {
	std::size_t row_pixel_count{0};
};

struct PaletteLutInfo {
	std::size_t entry_count{0};
	std::uint16_t bits_per_entry{0};
	DataType destination_dtype{DataType::unknown};
	bool has_alpha{false};
};

struct VoiLutInfo {
	std::size_t entry_count{0};
	std::uint16_t bits_per_entry{0};
	DataType destination_dtype{DataType::unknown};
};

struct PaletteTransformLayoutInfo {
	bool dst_planar{false};
	std::size_t row_pixel_count{0};
	std::size_t dst_plane_stride{0};
	bool has_alpha{false};
};

[[nodiscard]] inline bool try_get_integer_stored_value_range(
    const PixelLayout& layout, std::int64_t& out_min_value,
    std::int64_t& out_max_value) noexcept {
	const auto bits_stored =
	    layout.bits_stored != 0 ? layout.bits_stored : normalized_bits_stored_of(layout.data_type);
	if (bits_stored == 0 || bits_stored > 32) {
		return false;
	}

	switch (layout.data_type) {
	case DataType::u8:
	case DataType::u16:
	case DataType::u32: {
		out_min_value = 0;
		out_max_value = bits_stored == 32 ? static_cast<std::int64_t>(0xFFFFFFFFu)
		                                  : ((std::int64_t{1} << bits_stored) - 1);
		return true;
	}
	case DataType::s8:
	case DataType::s16:
	case DataType::s32: {
		const auto magnitude_bits = static_cast<std::uint16_t>(bits_stored - 1);
		const auto positive_limit = magnitude_bits == 31
		    ? static_cast<std::int64_t>(0x7FFFFFFF)
		    : ((std::int64_t{1} << magnitude_bits) - 1);
		out_min_value = -(std::int64_t{1} << magnitude_bits);
		out_max_value = positive_limit;
		return true;
	}
	default:
		return false;
	}
}

[[nodiscard]] inline bool lut_covers_integer_stored_range(
    const PixelLayout& source_layout, std::int64_t first_mapped,
    std::size_t entry_count) noexcept {
	if (entry_count == 0) {
		return false;
	}

	std::int64_t min_value = 0;
	std::int64_t max_value = 0;
	if (!try_get_integer_stored_value_range(source_layout, min_value, max_value)) {
		return false;
	}

	const auto last_mapped =
	    first_mapped + static_cast<std::int64_t>(entry_count - std::size_t{1});
	return min_value >= first_mapped && max_value <= last_mapped;
}

[[noreturn]] inline void throw_transform_argument_error(
    std::string_view function_name, std::string_view reason) {
	throw std::invalid_argument(
	    std::string(function_name) + ": " + std::string(reason));
}

[[nodiscard]] inline LayoutAccessInfo validate_layout_storage_or_throw(
    const PixelLayout& layout, std::string_view function_name, std::string_view label) {
	if (layout.empty()) {
		throw_transform_argument_error(function_name, std::string(label) + " layout is empty");
	}

	// Sample width and planar arrangement drive every address calculation.
	const auto sample_bytes = bytes_per_sample_of(layout.data_type);
	if (sample_bytes == 0) {
		throw_transform_argument_error(
		    function_name, std::string(label) + " layout has unknown data_type");
	}
	const bool planar_layout =
	    layout.planar == Planar::planar && layout.samples_per_pixel > std::uint16_t{1};

	// Verify that one logical row fits within the declared row stride.
	std::size_t row_value_count = 0;
	if (!detail::checked_mul_size_t(
	        static_cast<std::size_t>(layout.cols),
	        planar_layout ? std::size_t{1} : static_cast<std::size_t>(layout.samples_per_pixel),
	        row_value_count)) {
		throw std::overflow_error(
		    std::string(function_name) + ": " + std::string(label) +
		    " row value count overflow");
	}
	std::size_t row_payload_bytes = 0;
	if (!detail::checked_mul_size_t(row_value_count, sample_bytes, row_payload_bytes)) {
		throw std::overflow_error(
		    std::string(function_name) + ": " + std::string(label) +
		    " row payload overflow");
	}
	if (layout.row_stride < row_payload_bytes) {
		throw_transform_argument_error(
		    function_name, std::string(label) + " row_stride is smaller than row payload");
	}

	// Derive the minimal legal frame stride from row stride and plane count.
	std::size_t plane_stride = 0;
	if (!detail::checked_mul_size_t(
	        layout.row_stride, static_cast<std::size_t>(layout.rows), plane_stride)) {
		throw std::overflow_error(
		    std::string(function_name) + ": " + std::string(label) +
		    " plane stride overflow");
	}
	std::size_t min_frame_stride = plane_stride;
	if (planar_layout &&
	    !detail::checked_mul_size_t(min_frame_stride,
	        static_cast<std::size_t>(layout.samples_per_pixel), min_frame_stride)) {
		throw std::overflow_error(
		    std::string(function_name) + ": " + std::string(label) +
		    " frame stride overflow");
	}
	if (layout.frame_stride < min_frame_stride) {
		throw_transform_argument_error(
		    function_name, std::string(label) + " frame_stride is smaller than frame payload");
	}

	return LayoutAccessInfo{
	    .planar = planar_layout,
	    .row_value_count = row_value_count,
	    .plane_stride = plane_stride,
	};
}

[[nodiscard]] inline MonochromeTransformLayoutInfo
validate_monochrome_storage_pair_or_throw(
    ConstPixelSpan src, PixelSpan dst, std::string_view function_name) {
	const auto src_info =
	    validate_layout_storage_or_throw(src.layout, function_name, "source");
	const auto dst_info =
	    validate_layout_storage_or_throw(dst.layout, function_name, "destination");
	(void)dst_info;

	// Reject malformed spans before the transform loops start walking them.
	if (!src.has_required_bytes()) {
		throw_transform_argument_error(
		    function_name, "source byte span is smaller than the declared layout");
	}
	if (!dst.has_required_bytes()) {
		throw_transform_argument_error(
		    function_name, "destination byte span is smaller than the declared layout");
	}

	// DICOM rescale and Modality LUT operate on monochrome stored values only.
	if (src.layout.samples_per_pixel != std::uint16_t{1} ||
	    dst.layout.samples_per_pixel != std::uint16_t{1}) {
		throw_transform_argument_error(
		    function_name, "source and destination layouts must carry one sample per pixel");
	}
	if (src.layout.rows != dst.layout.rows || src.layout.cols != dst.layout.cols ||
	    src.layout.frames != dst.layout.frames) {
		throw_transform_argument_error(
		    function_name, "source and destination layouts must share geometry");
	}

	return MonochromeTransformLayoutInfo{
	    .row_pixel_count = src_info.row_value_count,
	};
}

[[nodiscard]] inline MonochromeTransformLayoutInfo
validate_monochrome_transform_pair_or_throw(
    ConstPixelSpan src, PixelSpan dst, std::string_view function_name) {
	const auto info =
	    validate_monochrome_storage_pair_or_throw(src, dst, function_name);

	// Rescale and Modality LUT materialize floating-point values.
	if (dst.layout.data_type != DataType::f32 && dst.layout.data_type != DataType::f64) {
		throw_transform_argument_error(
		    function_name, "destination dtype must be float32 or float64");
	}

	return info;
}

[[nodiscard]] inline std::uint16_t normalize_palette_bits_per_entry_or_throw(
    const PaletteLut& lut, std::string_view function_name) {
	// Palette LUT metadata may state the entry width explicitly, but callers that
	// construct a LUT manually can omit it and let the implementation infer 8/16-bit output.
	if (lut.bits_per_entry != 0) {
		if (lut.bits_per_entry > 16) {
			throw_transform_argument_error(
			    function_name, "palette LUT bits_per_entry must be between 1 and 16");
		}
		return lut.bits_per_entry;
	}

	std::uint16_t max_value = 0;
	const auto update_max = [&](std::span<const std::uint16_t> values) {
		for (const auto value : values) {
			max_value = std::max(max_value, value);
		}
	};
	update_max(lut.red_values);
	update_max(lut.green_values);
	update_max(lut.blue_values);
	return max_value <= std::uint16_t{0x00FF} ? std::uint16_t{8} : std::uint16_t{16};
}

[[nodiscard]] inline PaletteLutInfo validate_palette_lut_or_throw(
    const PaletteLut& lut, std::string_view function_name) {
	// Palette LUT needs three synchronized channel tables and a consistent bit depth.
	if (lut.red_values.empty() || lut.green_values.empty() || lut.blue_values.empty()) {
		throw_transform_argument_error(
		    function_name, "palette LUT channel tables must not be empty");
	}
	if (lut.red_values.size() != lut.green_values.size() ||
	    lut.red_values.size() != lut.blue_values.size()) {
		throw_transform_argument_error(
		    function_name, "palette LUT channel tables must have matching entry counts");
	}
	if (!lut.alpha_values.empty() &&
	    lut.alpha_values.size() != lut.red_values.size()) {
		throw_transform_argument_error(
		    function_name, "palette LUT alpha channel must match the RGB entry count");
	}

	const auto bits_per_entry =
	    normalize_palette_bits_per_entry_or_throw(lut, function_name);
	const std::uint32_t value_mask =
	    bits_per_entry == 16 ? 0xFFFFu : ((1u << bits_per_entry) - 1u);
	const auto validate_channel = [&](std::span<const std::uint16_t> values,
	                                  std::string_view label) {
		for (const auto value : values) {
			if (static_cast<std::uint32_t>(value) > value_mask) {
				throw_transform_argument_error(
				    function_name,
				    std::string("palette LUT ") + std::string(label) +
				        " channel has a value outside bits_per_entry");
			}
		}
	};
	validate_channel(lut.red_values, "red");
	validate_channel(lut.green_values, "green");
	validate_channel(lut.blue_values, "blue");
	if (!lut.alpha_values.empty()) {
		validate_channel(lut.alpha_values, "alpha");
	}

	return PaletteLutInfo{
	    .entry_count = lut.red_values.size(),
	    .bits_per_entry = bits_per_entry,
	    .destination_dtype =
	        bits_per_entry <= std::uint16_t{8} ? DataType::u8 : DataType::u16,
	    .has_alpha = !lut.alpha_values.empty(),
	};
}

[[nodiscard]] inline std::uint16_t normalize_voi_bits_per_entry_or_throw(
    const VoiLut& lut, std::string_view function_name) {
	// VOI LUT metadata may omit the entry width when callers construct a table manually.
	if (lut.bits_per_entry != 0) {
		if (lut.bits_per_entry > 16) {
			throw_transform_argument_error(
			    function_name, "VOI LUT bits_per_entry must be between 1 and 16");
		}
		return lut.bits_per_entry;
	}

	std::uint16_t max_value = 0;
	for (const auto value : lut.values) {
		max_value = std::max(max_value, value);
	}
	return max_value <= std::uint16_t{0x00FF} ? std::uint16_t{8} : std::uint16_t{16};
}

[[nodiscard]] inline VoiLutInfo validate_voi_lut_or_throw(
    const VoiLut& lut, std::string_view function_name) {
	// VOI LUT uses one synchronized lookup table and one effective output width.
	if (lut.values.empty()) {
		throw_transform_argument_error(
		    function_name, "VOI LUT values must not be empty");
	}

	const auto bits_per_entry =
	    normalize_voi_bits_per_entry_or_throw(lut, function_name);
	const std::uint32_t value_mask =
	    bits_per_entry == 16 ? 0xFFFFu : ((1u << bits_per_entry) - 1u);
	for (const auto value : lut.values) {
		if (static_cast<std::uint32_t>(value) > value_mask) {
			throw_transform_argument_error(
			    function_name, "VOI LUT has a value outside bits_per_entry");
		}
	}

	return VoiLutInfo{
	    .entry_count = lut.values.size(),
	    .bits_per_entry = bits_per_entry,
	    .destination_dtype =
	        bits_per_entry <= std::uint16_t{8} ? DataType::u8 : DataType::u16,
	};
}

[[nodiscard]] inline PaletteTransformLayoutInfo validate_palette_transform_pair_or_throw(
    ConstPixelSpan src, PixelSpan dst, const PaletteLutInfo& lut_info,
    std::string_view function_name) {
	const auto src_info =
	    validate_layout_storage_or_throw(src.layout, function_name, "source");
	const auto dst_info =
	    validate_layout_storage_or_throw(dst.layout, function_name, "destination");

	// Reject malformed source and destination spans before the LUT walk starts.
	if (!src.has_required_bytes()) {
		throw_transform_argument_error(
		    function_name, "source byte span is smaller than the declared layout");
	}
	if (!dst.has_required_bytes()) {
		throw_transform_argument_error(
		    function_name, "destination byte span is smaller than the declared layout");
	}

	// Indexed palette input must be monochannel; output expands to RGB or RGBA.
	if (src.layout.samples_per_pixel != std::uint16_t{1}) {
		throw_transform_argument_error(
		    function_name, "source layout must carry one sample per pixel");
	}
	if (src.layout.rows != dst.layout.rows || src.layout.cols != dst.layout.cols ||
	    src.layout.frames != dst.layout.frames) {
		throw_transform_argument_error(
		    function_name, "source and destination layouts must share geometry");
	}
	const auto expected_samples =
	    lut_info.has_alpha ? std::uint16_t{4} : std::uint16_t{3};
	if (dst.layout.samples_per_pixel != expected_samples) {
		throw_transform_argument_error(
		    function_name, "destination layout must carry the palette channel count");
	}
	if (dst.layout.photometric != Photometric::rgb) {
		throw_transform_argument_error(
		    function_name, "destination photometric must be RGB");
	}
	if (dst.layout.data_type != lut_info.destination_dtype) {
		throw_transform_argument_error(
		    function_name, "destination dtype does not match palette LUT bit depth");
	}

	return PaletteTransformLayoutInfo{
	    .dst_planar = dst_info.planar,
	    .row_pixel_count = src_info.row_value_count,
	    .dst_plane_stride = dst_info.plane_stride,
	    .has_alpha = lut_info.has_alpha,
	};
}

[[nodiscard]] inline MonochromeTransformLayoutInfo validate_voi_lut_transform_pair_or_throw(
    ConstPixelSpan src, PixelSpan dst, const VoiLutInfo& lut_info,
    std::string_view function_name) {
	const auto info =
	    validate_monochrome_storage_pair_or_throw(src, dst, function_name);

	// VOI LUT output is currently materialized into integer grayscale storage.
	if (dst.layout.data_type != lut_info.destination_dtype) {
		throw_transform_argument_error(
		    function_name, "destination dtype does not match VOI LUT bit depth");
	}

	return info;
}

template <typename Src, typename Dst, typename Mapper>
inline void transform_monochrome_pixels_bytewise_impl(ConstPixelSpan src, PixelSpan dst,
    const MonochromeTransformLayoutInfo& layout_info, Mapper&& mapper) {
	const auto* src_bytes = src.bytes.data();
	auto* dst_bytes = dst.bytes.data();
	const auto rows = static_cast<std::size_t>(src.layout.rows);
	const auto frames = static_cast<std::size_t>(src.layout.frames);
	const auto src_sample_bytes = sizeof(Src);
	const auto dst_sample_bytes = sizeof(Dst);

	for (std::size_t frame_index = 0; frame_index < frames; ++frame_index) {
		const auto src_frame_offset = frame_index * src.layout.frame_stride;
		const auto dst_frame_offset = frame_index * dst.layout.frame_stride;

		// Monochrome transforms only need a simple frame/row/pixel walk.
		for (std::size_t row_index = 0; row_index < rows; ++row_index) {
			const auto* src_row =
			    src_bytes + src_frame_offset + row_index * src.layout.row_stride;
			auto* dst_row = dst_bytes + dst_frame_offset + row_index * dst.layout.row_stride;
			for (std::size_t value_index = 0; value_index < layout_info.row_pixel_count;
			     ++value_index) {
				Src stored_value{};
				std::memcpy(
				    &stored_value, src_row + value_index * src_sample_bytes, sizeof(Src));
				const Dst transformed_value =
				    static_cast<Dst>(mapper(stored_value, frame_index));
				std::memcpy(
				    dst_row + value_index * dst_sample_bytes, &transformed_value, sizeof(Dst));
			}
		}
	}
}

template <typename Src, typename Dst, typename Mapper>
inline void transform_monochrome_pixels_typed_impl(ConstPixelSpan src, PixelSpan dst,
    const MonochromeTransformLayoutInfo& layout_info, Mapper&& mapper) {
	const auto* src_bytes = src.bytes.data();
	auto* dst_bytes = dst.bytes.data();
	const auto rows = static_cast<std::size_t>(src.layout.rows);
	const auto frames = static_cast<std::size_t>(src.layout.frames);

	for (std::size_t frame_index = 0; frame_index < frames; ++frame_index) {
		const auto src_frame_offset = frame_index * src.layout.frame_stride;
		const auto dst_frame_offset = frame_index * dst.layout.frame_stride;

		// Typed row access lets the compiler optimize the inner monochrome loop
		// without the per-sample memcpy overhead of the bytewise fallback.
		for (std::size_t row_index = 0; row_index < rows; ++row_index) {
			const auto* src_row = reinterpret_cast<const Src*>(
			    src_bytes + src_frame_offset + row_index * src.layout.row_stride);
			auto* dst_row = reinterpret_cast<Dst*>(
			    dst_bytes + dst_frame_offset + row_index * dst.layout.row_stride);
			for (std::size_t value_index = 0; value_index < layout_info.row_pixel_count;
			     ++value_index) {
				dst_row[value_index] = static_cast<Dst>(
				    mapper(src_row[value_index], frame_index));
			}
		}
	}
}

template <typename Src, typename Dst, typename Mapper>
inline void transform_monochrome_pixels_impl(ConstPixelSpan src, PixelSpan dst,
    const MonochromeTransformLayoutInfo& layout_info, Mapper&& mapper) {
	const bool typed_access_ok = src.template is_typed_row_access_aligned<Src>() &&
	    dst.template is_typed_row_access_aligned<Dst>();
	if (typed_access_ok) {
		transform_monochrome_pixels_typed_impl<Src, Dst>(
		    src, dst, layout_info, std::forward<Mapper>(mapper));
		return;
	}
	transform_monochrome_pixels_bytewise_impl<Src, Dst>(
	    src, dst, layout_info, std::forward<Mapper>(mapper));
}

template <typename Dst, typename Mapper>
inline void dispatch_numeric_source_dtype(
    ConstPixelSpan src, PixelSpan dst, const MonochromeTransformLayoutInfo& layout_info,
    std::string_view function_name, Mapper&& mapper) {
	switch (src.layout.data_type) {
	case DataType::u8:
		transform_monochrome_pixels_impl<std::uint8_t, Dst>(src, dst, layout_info, mapper);
		return;
	case DataType::s8:
		transform_monochrome_pixels_impl<std::int8_t, Dst>(src, dst, layout_info, mapper);
		return;
	case DataType::u16:
		transform_monochrome_pixels_impl<std::uint16_t, Dst>(src, dst, layout_info, mapper);
		return;
	case DataType::s16:
		transform_monochrome_pixels_impl<std::int16_t, Dst>(src, dst, layout_info, mapper);
		return;
	case DataType::u32:
		transform_monochrome_pixels_impl<std::uint32_t, Dst>(src, dst, layout_info, mapper);
		return;
	case DataType::s32:
		transform_monochrome_pixels_impl<std::int32_t, Dst>(src, dst, layout_info, mapper);
		return;
	case DataType::f32:
		transform_monochrome_pixels_impl<float, Dst>(src, dst, layout_info, mapper);
		return;
	case DataType::f64:
		transform_monochrome_pixels_impl<double, Dst>(src, dst, layout_info, mapper);
		return;
	default:
		throw_transform_argument_error(function_name, "source dtype is not supported");
	}
}

template <typename Dst, typename Mapper>
inline void dispatch_integral_source_dtype(
    ConstPixelSpan src, PixelSpan dst, const MonochromeTransformLayoutInfo& layout_info,
    std::string_view function_name, Mapper&& mapper) {
	switch (src.layout.data_type) {
	case DataType::u8:
		transform_monochrome_pixels_impl<std::uint8_t, Dst>(src, dst, layout_info, mapper);
		return;
	case DataType::s8:
		transform_monochrome_pixels_impl<std::int8_t, Dst>(src, dst, layout_info, mapper);
		return;
	case DataType::u16:
		transform_monochrome_pixels_impl<std::uint16_t, Dst>(src, dst, layout_info, mapper);
		return;
	case DataType::s16:
		transform_monochrome_pixels_impl<std::int16_t, Dst>(src, dst, layout_info, mapper);
		return;
	case DataType::u32:
		transform_monochrome_pixels_impl<std::uint32_t, Dst>(src, dst, layout_info, mapper);
		return;
	case DataType::s32:
		transform_monochrome_pixels_impl<std::int32_t, Dst>(src, dst, layout_info, mapper);
		return;
	default:
		throw_transform_argument_error(
		    function_name, "source dtype must be an integral stored-value type");
	}
}

template <typename LayoutInfo, typename Mapper>
inline void dispatch_float_destination_dtype(
    ConstPixelSpan src, PixelSpan dst, const LayoutInfo& layout_info,
    std::string_view function_name, Mapper&& mapper) {
	switch (dst.layout.data_type) {
	case DataType::f32:
		mapper.template operator()<float>(src, dst, layout_info);
		return;
	case DataType::f64:
		mapper.template operator()<double>(src, dst, layout_info);
		return;
	default:
		throw_transform_argument_error(
		    function_name, "destination dtype must be float32 or float64");
	}
}

template <bool ClampIndex>
[[nodiscard]] inline float lookup_modality_lut_value(
    const ModalityLut& lut, std::int64_t stored_value) noexcept {
	const auto lut_index = stored_value - lut.first_mapped;
	if constexpr (ClampIndex) {
		const auto last_index = static_cast<std::int64_t>(lut.values.size() - 1);
		return lut.values[static_cast<std::size_t>(
		    std::clamp(lut_index, std::int64_t{0}, last_index))];
	} else {
		return lut.values[static_cast<std::size_t>(lut_index)];
	}
}

template <bool ClampIndex>
[[nodiscard]] inline std::uint16_t lookup_voi_lut_value(
    const VoiLut& lut, std::int64_t stored_value) noexcept {
	const auto lut_index = stored_value - lut.first_mapped;
	if constexpr (ClampIndex) {
		const auto last_index = static_cast<std::int64_t>(lut.values.size() - 1);
		return lut.values[static_cast<std::size_t>(std::clamp(
		    lut_index, std::int64_t{0}, last_index))];
	} else {
		return lut.values[static_cast<std::size_t>(lut_index)];
	}
}

template <bool ClampIndex, bool HasAlpha, typename Src, typename Dst>
inline void apply_palette_lut_planar_bytewise_impl(ConstPixelSpan src, PixelSpan dst,
    const PaletteTransformLayoutInfo& layout_info, const PaletteLut& lut) {
	const auto* src_bytes = src.bytes.data();
	auto* dst_bytes = dst.bytes.data();
	const auto rows = static_cast<std::size_t>(src.layout.rows);
	const auto frames = static_cast<std::size_t>(src.layout.frames);
	const auto src_sample_bytes = sizeof(Src);
	const auto dst_sample_bytes = sizeof(Dst);
	const auto last_index = static_cast<std::int64_t>(lut.red_values.size() - 1);

	for (std::size_t frame_index = 0; frame_index < frames; ++frame_index) {
		const auto src_frame_offset = frame_index * src.layout.frame_stride;
		const auto dst_frame_offset = frame_index * dst.layout.frame_stride;
		for (std::size_t row_index = 0; row_index < rows; ++row_index) {
			const auto* src_row =
			    src_bytes + src_frame_offset + row_index * src.layout.row_stride;
			auto* dst_red_row = dst_bytes + dst_frame_offset +
			    row_index * dst.layout.row_stride;
			auto* dst_green_row = dst_bytes + dst_frame_offset +
			    layout_info.dst_plane_stride + row_index * dst.layout.row_stride;
			auto* dst_blue_row = dst_bytes + dst_frame_offset +
			    layout_info.dst_plane_stride * std::size_t{2} +
			    row_index * dst.layout.row_stride;
			auto* dst_alpha_row = HasAlpha
			    ? dst_bytes + dst_frame_offset +
			          layout_info.dst_plane_stride * std::size_t{3} +
			          row_index * dst.layout.row_stride
			    : nullptr;
			for (std::size_t pixel_index = 0; pixel_index < layout_info.row_pixel_count;
			     ++pixel_index) {
				Src stored_value{};
				std::memcpy(
				    &stored_value, src_row + pixel_index * src_sample_bytes, sizeof(Src));
				const auto unclamped_index =
				    static_cast<std::int64_t>(stored_value) - lut.first_mapped;
				const auto lut_index = [&]() noexcept -> std::size_t {
					if constexpr (ClampIndex) {
						return static_cast<std::size_t>(std::clamp(
						    unclamped_index, std::int64_t{0}, last_index));
					}
					return static_cast<std::size_t>(unclamped_index);
				}();
				const auto red = static_cast<Dst>(lut.red_values[lut_index]);
				const auto green = static_cast<Dst>(lut.green_values[lut_index]);
				const auto blue = static_cast<Dst>(lut.blue_values[lut_index]);
				std::memcpy(dst_red_row + pixel_index * dst_sample_bytes,
				    &red, sizeof(Dst));
				std::memcpy(dst_green_row + pixel_index * dst_sample_bytes,
				    &green, sizeof(Dst));
				std::memcpy(dst_blue_row + pixel_index * dst_sample_bytes,
				    &blue, sizeof(Dst));
				if constexpr (HasAlpha) {
					const auto alpha = static_cast<Dst>(lut.alpha_values[lut_index]);
					std::memcpy(dst_alpha_row + pixel_index * dst_sample_bytes,
					    &alpha, sizeof(Dst));
				}
			}
		}
	}
}

template <bool ClampIndex, bool HasAlpha, typename Src, typename Dst>
inline void apply_palette_lut_interleaved_bytewise_impl(ConstPixelSpan src, PixelSpan dst,
    const PaletteTransformLayoutInfo& layout_info, const PaletteLut& lut) {
	const auto* src_bytes = src.bytes.data();
	auto* dst_bytes = dst.bytes.data();
	const auto rows = static_cast<std::size_t>(src.layout.rows);
	const auto frames = static_cast<std::size_t>(src.layout.frames);
	const auto src_sample_bytes = sizeof(Src);
	const auto dst_sample_bytes = sizeof(Dst);
	const auto last_index = static_cast<std::int64_t>(lut.red_values.size() - 1);

	for (std::size_t frame_index = 0; frame_index < frames; ++frame_index) {
		const auto src_frame_offset = frame_index * src.layout.frame_stride;
		const auto dst_frame_offset = frame_index * dst.layout.frame_stride;
		for (std::size_t row_index = 0; row_index < rows; ++row_index) {
			const auto* src_row =
			    src_bytes + src_frame_offset + row_index * src.layout.row_stride;
			auto* dst_row = dst_bytes + dst_frame_offset + row_index * dst.layout.row_stride;
			for (std::size_t pixel_index = 0; pixel_index < layout_info.row_pixel_count;
			     ++pixel_index) {
				Src stored_value{};
				std::memcpy(
				    &stored_value, src_row + pixel_index * src_sample_bytes, sizeof(Src));
				const auto unclamped_index =
				    static_cast<std::int64_t>(stored_value) - lut.first_mapped;
				const auto lut_index = [&]() noexcept -> std::size_t {
					if constexpr (ClampIndex) {
						return static_cast<std::size_t>(std::clamp(
						    unclamped_index, std::int64_t{0}, last_index));
					}
					return static_cast<std::size_t>(unclamped_index);
				}();
				const auto red = static_cast<Dst>(lut.red_values[lut_index]);
				const auto green = static_cast<Dst>(lut.green_values[lut_index]);
				const auto blue = static_cast<Dst>(lut.blue_values[lut_index]);
				const auto dst_pixel_offset =
				    pixel_index * std::size_t{3} * dst_sample_bytes;
				std::memcpy(dst_row + dst_pixel_offset, &red, sizeof(Dst));
				std::memcpy(dst_row + dst_pixel_offset + dst_sample_bytes,
				    &green, sizeof(Dst));
				std::memcpy(dst_row + dst_pixel_offset + dst_sample_bytes * std::size_t{2},
				    &blue, sizeof(Dst));
				if constexpr (HasAlpha) {
					const auto alpha = static_cast<Dst>(lut.alpha_values[lut_index]);
					std::memcpy(
					    dst_row + dst_pixel_offset + dst_sample_bytes * std::size_t{3},
					    &alpha, sizeof(Dst));
				}
			}
		}
	}
}

template <bool ClampIndex, bool HasAlpha, typename Src, typename Dst>
inline void apply_palette_lut_planar_typed_impl(ConstPixelSpan src, PixelSpan dst,
    const PaletteTransformLayoutInfo& layout_info, const PaletteLut& lut) {
	const auto* src_bytes = src.bytes.data();
	auto* dst_bytes = dst.bytes.data();
	const auto rows = static_cast<std::size_t>(src.layout.rows);
	const auto frames = static_cast<std::size_t>(src.layout.frames);
	const auto last_index = static_cast<std::int64_t>(lut.red_values.size() - 1);

	for (std::size_t frame_index = 0; frame_index < frames; ++frame_index) {
		const auto src_frame_offset = frame_index * src.layout.frame_stride;
		const auto dst_frame_offset = frame_index * dst.layout.frame_stride;
		for (std::size_t row_index = 0; row_index < rows; ++row_index) {
			const auto* src_row = reinterpret_cast<const Src*>(
			    src_bytes + src_frame_offset + row_index * src.layout.row_stride);
			auto* dst_red_row = reinterpret_cast<Dst*>(
			    dst_bytes + dst_frame_offset + row_index * dst.layout.row_stride);
			auto* dst_green_row = reinterpret_cast<Dst*>(
			    dst_bytes + dst_frame_offset + layout_info.dst_plane_stride +
			    row_index * dst.layout.row_stride);
			auto* dst_blue_row = reinterpret_cast<Dst*>(
			    dst_bytes + dst_frame_offset + layout_info.dst_plane_stride * std::size_t{2} +
			    row_index * dst.layout.row_stride);
			auto* dst_alpha_row = HasAlpha
			    ? reinterpret_cast<Dst*>(
			          dst_bytes + dst_frame_offset +
			          layout_info.dst_plane_stride * std::size_t{3} +
			          row_index * dst.layout.row_stride)
			    : nullptr;

			// Keep the inner loop in typed form so the compiler can optimize
			// loads/stores even though LUT gather itself remains scalar.
			for (std::size_t pixel_index = 0; pixel_index < layout_info.row_pixel_count;
			     ++pixel_index) {
				const auto unclamped_index =
				    static_cast<std::int64_t>(src_row[pixel_index]) - lut.first_mapped;
				const auto lut_index = [&]() noexcept -> std::size_t {
					if constexpr (ClampIndex) {
						return static_cast<std::size_t>(std::clamp(
						    unclamped_index, std::int64_t{0}, last_index));
					}
					return static_cast<std::size_t>(unclamped_index);
				}();
				dst_red_row[pixel_index] = static_cast<Dst>(lut.red_values[lut_index]);
				dst_green_row[pixel_index] = static_cast<Dst>(lut.green_values[lut_index]);
				dst_blue_row[pixel_index] = static_cast<Dst>(lut.blue_values[lut_index]);
				if constexpr (HasAlpha) {
					dst_alpha_row[pixel_index] =
					    static_cast<Dst>(lut.alpha_values[lut_index]);
				}
			}
		}
	}
}

template <bool ClampIndex, bool HasAlpha, typename Src, typename Dst>
inline void apply_palette_lut_interleaved_typed_impl(ConstPixelSpan src, PixelSpan dst,
    const PaletteTransformLayoutInfo& layout_info, const PaletteLut& lut) {
	const auto* src_bytes = src.bytes.data();
	auto* dst_bytes = dst.bytes.data();
	const auto rows = static_cast<std::size_t>(src.layout.rows);
	const auto frames = static_cast<std::size_t>(src.layout.frames);
	const auto last_index = static_cast<std::int64_t>(lut.red_values.size() - 1);

	for (std::size_t frame_index = 0; frame_index < frames; ++frame_index) {
		const auto src_frame_offset = frame_index * src.layout.frame_stride;
		const auto dst_frame_offset = frame_index * dst.layout.frame_stride;
		for (std::size_t row_index = 0; row_index < rows; ++row_index) {
			const auto* src_row = reinterpret_cast<const Src*>(
			    src_bytes + src_frame_offset + row_index * src.layout.row_stride);
			auto* dst_row = reinterpret_cast<Dst*>(
			    dst_bytes + dst_frame_offset + row_index * dst.layout.row_stride);

			// Typed RGB writes remove the per-sample memcpy overhead from the hot loop.
			for (std::size_t pixel_index = 0; pixel_index < layout_info.row_pixel_count;
			     ++pixel_index) {
				const auto unclamped_index =
				    static_cast<std::int64_t>(src_row[pixel_index]) - lut.first_mapped;
				const auto lut_index = [&]() noexcept -> std::size_t {
					if constexpr (ClampIndex) {
						return static_cast<std::size_t>(std::clamp(
						    unclamped_index, std::int64_t{0}, last_index));
					}
					return static_cast<std::size_t>(unclamped_index);
				}();
				const auto dst_pixel_offset =
				    pixel_index * (HasAlpha ? std::size_t{4} : std::size_t{3});
				dst_row[dst_pixel_offset] = static_cast<Dst>(lut.red_values[lut_index]);
				dst_row[dst_pixel_offset + std::size_t{1}] =
				    static_cast<Dst>(lut.green_values[lut_index]);
				dst_row[dst_pixel_offset + std::size_t{2}] =
				    static_cast<Dst>(lut.blue_values[lut_index]);
				if constexpr (HasAlpha) {
					dst_row[dst_pixel_offset + std::size_t{3}] =
					    static_cast<Dst>(lut.alpha_values[lut_index]);
				}
			}
		}
	}
}

template <typename Dst>
inline void dispatch_palette_source_dtype(ConstPixelSpan src, PixelSpan dst,
    const PaletteTransformLayoutInfo& layout_info, const PaletteLut& lut,
    bool clamp_indices) {
	const auto dispatch = [&]<typename Src>() {
		const auto typed_access_ok = src.template is_typed_row_access_aligned<Src>() &&
		    dst.template is_typed_row_access_aligned<Dst>();
		const auto dispatch_planar = [&]<bool Clamp, bool HasAlpha>() {
			if (typed_access_ok) {
				apply_palette_lut_planar_typed_impl<Clamp, HasAlpha, Src, Dst>(
				    src, dst, layout_info, lut);
				return;
			}
			apply_palette_lut_planar_bytewise_impl<Clamp, HasAlpha, Src, Dst>(
			    src, dst, layout_info, lut);
		};
		const auto dispatch_interleaved = [&]<bool Clamp, bool HasAlpha>() {
			if (typed_access_ok) {
				apply_palette_lut_interleaved_typed_impl<Clamp, HasAlpha, Src, Dst>(
				    src, dst, layout_info, lut);
				return;
			}
			apply_palette_lut_interleaved_bytewise_impl<Clamp, HasAlpha, Src, Dst>(
			    src, dst, layout_info, lut);
		};
		if (layout_info.dst_planar) {
			if (clamp_indices) {
				if (layout_info.has_alpha) {
					dispatch_planar.template operator()<true, true>();
				} else {
					dispatch_planar.template operator()<true, false>();
				}
				return;
			}
			if (layout_info.has_alpha) {
				dispatch_planar.template operator()<false, true>();
			} else {
				dispatch_planar.template operator()<false, false>();
			}
			return;
		}
		if (clamp_indices) {
			if (layout_info.has_alpha) {
				dispatch_interleaved.template operator()<true, true>();
			} else {
				dispatch_interleaved.template operator()<true, false>();
			}
			return;
		}
		if (layout_info.has_alpha) {
			dispatch_interleaved.template operator()<false, true>();
		} else {
			dispatch_interleaved.template operator()<false, false>();
		}
	};

	switch (src.layout.data_type) {
	case DataType::u8:
		dispatch.template operator()<std::uint8_t>();
		return;
	case DataType::s8:
		dispatch.template operator()<std::int8_t>();
		return;
	case DataType::u16:
		dispatch.template operator()<std::uint16_t>();
		return;
	case DataType::s16:
		dispatch.template operator()<std::int16_t>();
		return;
	case DataType::u32:
		dispatch.template operator()<std::uint32_t>();
		return;
	case DataType::s32:
		dispatch.template operator()<std::int32_t>();
		return;
	default:
		throw_transform_argument_error(
		    "apply_palette_lut_into", "source dtype must be an integral stored-value type");
	}
}

} // namespace dicom::pixel::transform_detail
