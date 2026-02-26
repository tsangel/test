#pragma once

#include "dicom.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace dicom::pixel::detail {

struct CodecError;
struct DecodeValueTransform;

enum class PlanarTransform : std::uint8_t {
	interleaved_to_interleaved = 0,
	interleaved_to_planar,
	planar_to_interleaved,
	planar_to_planar,
};

PlanarTransform select_planar_transform(Planar src_planar, Planar dst_planar) noexcept;

std::size_t sv_dtype_bytes(DataType sv_dtype) noexcept;

struct EncapsulatedFrameSource {
	const PixelFrame* frame{nullptr};
	const InStream* stream{nullptr};
	std::span<const std::uint8_t> contiguous{};
	const std::vector<PixelFragment>* fragments{nullptr};
	std::size_t total_size{0};
};

EncapsulatedFrameSource load_encapsulated_frame_source(const DataSet& ds,
    std::string_view file_path, std::size_t frame_index, std::string_view codec_name);

struct NativeFrameSource {
	std::span<const std::uint8_t> contiguous{};
	std::string_view name{"PixelData"};
};

NativeFrameSource load_native_frame_source(
    const DataSet& ds, std::string_view file_path,
    const pixel::PixelDataInfo& info, std::size_t frame_index);

std::span<const std::uint8_t> materialize_encapsulated_frame_source(
    std::string_view file_path, std::size_t frame_index, std::string_view codec_name,
    const EncapsulatedFrameSource& source, std::vector<std::uint8_t>& out_owned);

void decode_mono_scaled_into_f32(const DecodeValueTransform& value_transform,
    const pixel::PixelDataInfo& info, const std::uint8_t* src_frame,
    std::span<std::uint8_t> dst,
    const DecodeStrides& dst_strides, std::size_t rows, std::size_t cols,
    std::size_t src_row_bytes);

void run_planar_transform_copy(PlanarTransform transform, std::size_t bytes_per_sample,
    const std::uint8_t* src_frame, std::uint8_t* dst_base,
    std::size_t rows, std::size_t cols, std::size_t samples_per_pixel,
    std::size_t src_row_bytes, std::size_t dst_row_bytes);

bool decode_raw_into(const pixel::PixelDataInfo& info,
    const DecodeValueTransform& value_transform,
    std::span<std::uint8_t> dst,
    const DecodeStrides& dst_strides, const DecodeOptions& opt,
    CodecError& out_error,
    std::span<const std::uint8_t> prepared_source = {}) noexcept;

bool decode_encapsulated_uncompressed_into(const pixel::PixelDataInfo& info,
    const DecodeValueTransform& value_transform,
    std::span<std::uint8_t> dst, const DecodeStrides& dst_strides,
    const DecodeOptions& opt, CodecError& out_error,
    std::span<const std::uint8_t> prepared_source = {}) noexcept;

bool decode_jpeg2k_into(const pixel::PixelDataInfo& info,
    const DecodeValueTransform& value_transform,
    std::span<std::uint8_t> dst,
    const DecodeStrides& dst_strides, const DecodeOptions& opt, CodecError& out_error,
    std::span<const std::uint8_t> prepared_source = {}) noexcept;

bool decode_htj2k_into(const pixel::PixelDataInfo& info,
    const DecodeValueTransform& value_transform,
    std::span<std::uint8_t> dst,
    const DecodeStrides& dst_strides, const DecodeOptions& opt, CodecError& out_error,
    std::span<const std::uint8_t> prepared_source = {}) noexcept;

bool decode_jpegls_into(const pixel::PixelDataInfo& info,
    const DecodeValueTransform& value_transform,
    std::span<std::uint8_t> dst,
    const DecodeStrides& dst_strides, const DecodeOptions& opt, CodecError& out_error,
    std::span<const std::uint8_t> prepared_source = {}) noexcept;

bool decode_jpegxl_into(const pixel::PixelDataInfo& info,
    const DecodeValueTransform& value_transform,
    std::span<std::uint8_t> dst,
    const DecodeStrides& dst_strides, const DecodeOptions& opt, CodecError& out_error,
    std::span<const std::uint8_t> prepared_source = {}) noexcept;

bool decode_jpeg_into(const pixel::PixelDataInfo& info,
    const DecodeValueTransform& value_transform,
    std::span<std::uint8_t> dst,
    const DecodeStrides& dst_strides, const DecodeOptions& opt, CodecError& out_error,
    std::span<const std::uint8_t> prepared_source = {}) noexcept;

} // namespace dicom::pixel::detail
