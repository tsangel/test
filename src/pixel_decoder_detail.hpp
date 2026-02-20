#pragma once

#include "dicom.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace dicom::pixel::detail {

enum class decode_backend : std::uint8_t {
	raw = 0,
	rle,
	jpeg_family,
	unsupported,
};

decode_backend select_decode_backend(uid::WellKnown ts) noexcept;

enum class planar_transform : std::uint8_t {
	interleaved_to_interleaved = 0,
	interleaved_to_planar,
	planar_to_interleaved,
	planar_to_planar,
};

planar_transform select_planar_transform(planar src_planar, planar dst_planar) noexcept;

std::size_t sv_dtype_bytes(dtype sv_dtype) noexcept;

void decode_mono_scaled_into_f32(const DataSet& ds, const DataSet::pixel_info_t& info,
    const std::uint8_t* src_frame, std::span<std::uint8_t> dst,
    const strides& dst_strides, std::size_t rows, std::size_t cols,
    std::size_t src_row_bytes);

void run_planar_transform_copy(planar_transform transform, std::size_t bytes_per_sample, bool needs_swap,
    const std::uint8_t* src_frame, std::uint8_t* dst_base,
    std::size_t rows, std::size_t cols, std::size_t samples_per_pixel,
    std::size_t src_row_bytes, std::size_t dst_row_bytes);

void decode_raw_into(const DataSet& ds, const DataSet::pixel_info_t& info,
    std::size_t frame_index, std::span<std::uint8_t> dst,
    const strides& dst_strides, const decode_opts& opt);

void decode_rle_into(const DataSet& ds, const DataSet::pixel_info_t& info,
    std::size_t frame_index, std::span<std::uint8_t> dst,
    const strides& dst_strides, const decode_opts& opt);

void decode_jpeg2k_into(const DataSet& ds, const DataSet::pixel_info_t& info,
    std::size_t frame_index, std::span<std::uint8_t> dst,
    const strides& dst_strides, const decode_opts& opt);

void decode_htj2k_into(const DataSet& ds, const DataSet::pixel_info_t& info,
    std::size_t frame_index, std::span<std::uint8_t> dst,
    const strides& dst_strides, const decode_opts& opt);

void decode_jpegls_into(const DataSet& ds, const DataSet::pixel_info_t& info,
    std::size_t frame_index, std::span<std::uint8_t> dst,
    const strides& dst_strides, const decode_opts& opt);

void decode_jpeg_into(const DataSet& ds, const DataSet::pixel_info_t& info,
    std::size_t frame_index, std::span<std::uint8_t> dst,
    const strides& dst_strides, const decode_opts& opt);

} // namespace dicom::pixel::detail
