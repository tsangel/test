#include "dicom.h"

#include "diagnostics.h"
#include "pixel_decoder_plugin_abi.h"
#include "pixel/host/decode/decode_frame_dispatch.hpp"
#include "pixel/host/decode/decode_plan_compute.hpp"
#include "pixel/host/error/codec_error.hpp"
#include "pixel/host/support/dicom_pixel_support.hpp"

namespace dicom {

namespace pixel {

namespace {

[[nodiscard]] std::optional<Photometric> map_decoded_photometric(
    uint8_t color_space_code, Photometric planned_photometric) noexcept {
	switch (color_space_code) {
	case PIXEL_DECODED_COLOR_SPACE_MONOCHROME:
		switch (planned_photometric) {
		case Photometric::monochrome1:
		case Photometric::monochrome2:
		case Photometric::palette_color:
			return planned_photometric;
		default:
			return std::nullopt;
		}
	case PIXEL_DECODED_COLOR_SPACE_RGB:
		return Photometric::rgb;
	case PIXEL_DECODED_COLOR_SPACE_CMYK:
		return Photometric::cmyk;
	case PIXEL_DECODED_COLOR_SPACE_YBR_FULL:
		return Photometric::ybr_full;
	case PIXEL_DECODED_COLOR_SPACE_YBR_FULL_422:
		return Photometric::ybr_full_422;
	case PIXEL_DECODED_COLOR_SPACE_YBR_PARTIAL_420:
		return Photometric::ybr_partial_420;
	case PIXEL_DECODED_COLOR_SPACE_YBR_PARTIAL_422:
		return Photometric::ybr_partial_422;
	case PIXEL_DECODED_COLOR_SPACE_RGBA:
	case PIXEL_DECODED_COLOR_SPACE_UNKNOWN:
	default:
		return std::nullopt;
	}
}

[[nodiscard]] EncodedLossyState map_encoded_lossy_state(
    uint8_t encoded_lossy_state) noexcept {
	switch (encoded_lossy_state) {
	case PIXEL_ENCODED_LOSSY_STATE_LOSSLESS:
		return EncodedLossyState::lossless;
	case PIXEL_ENCODED_LOSSY_STATE_LOSSY:
		return EncodedLossyState::lossy;
	case PIXEL_ENCODED_LOSSY_STATE_NEAR_LOSSLESS:
		return EncodedLossyState::near_lossless;
	case PIXEL_ENCODED_LOSSY_STATE_UNKNOWN:
	default:
		return EncodedLossyState::unknown;
	}
}

[[nodiscard]] std::optional<DataType> map_decoded_dtype(
    uint8_t dtype_code) noexcept {
	switch (dtype_code) {
	case PIXEL_DTYPE_U8:
		return DataType::u8;
	case PIXEL_DTYPE_S8:
		return DataType::s8;
	case PIXEL_DTYPE_U16:
		return DataType::u16;
	case PIXEL_DTYPE_S16:
		return DataType::s16;
	case PIXEL_DTYPE_U32:
		return DataType::u32;
	case PIXEL_DTYPE_S32:
		return DataType::s32;
	case PIXEL_DTYPE_F32:
		return DataType::f32;
	case PIXEL_DTYPE_F64:
		return DataType::f64;
	case PIXEL_DTYPE_UNKNOWN:
	default:
		return std::nullopt;
	}
}

[[nodiscard]] std::optional<Planar> map_decoded_planar(
    uint8_t planar_code) noexcept {
	switch (planar_code) {
	case PIXEL_DECODED_PLANAR_INTERLEAVED:
		return Planar::interleaved;
	case PIXEL_DECODED_PLANAR_PLANAR:
		return Planar::planar;
	case PIXEL_DECODED_PLANAR_UNKNOWN:
	default:
		return std::nullopt;
	}
}

void assign_decode_info(const pixel_decoder_info& abi_info,
    Photometric planned_photometric, DecodeInfo& decode_info) {
	decode_info = DecodeInfo{
	    .photometric = map_decoded_photometric(
	        abi_info.actual_color_space, planned_photometric),
	    .encoded_lossy_state = map_encoded_lossy_state(abi_info.encoded_lossy_state),
	    .data_type = map_decoded_dtype(abi_info.actual_dtype),
	    .planar = map_decoded_planar(abi_info.actual_planar),
	    .bits_per_sample = abi_info.bits_per_sample,
	};
}

void decode_frame_into_impl(const DicomFile& df, std::size_t frame_index,
    std::span<std::uint8_t> dst, const DecodePlan& plan,
    DecodeInfo* decode_info) {
	pixel_decoder_info abi_info{};
	pixel_decoder_info* abi_info_ptr = nullptr;
	if (decode_info != nullptr) {
		abi_info.struct_size = sizeof(pixel_decoder_info);
		abi_info.abi_version = PIXEL_DECODER_PLUGIN_ABI;
		abi_info_ptr = &abi_info;
	}

	try {
		detail::dispatch_decode_frame(df, frame_index, dst, plan, abi_info_ptr);
		if (decode_info != nullptr) {
			assign_decode_info(abi_info, plan.output_layout.photometric, *decode_info);
		}
	} catch (const diag::DicomException& ex) {
		detail::rethrow_codec_exception_at_boundary_or_throw(
		    "pixel::decode_frame_into", df, ex);
	}
}

void decode_all_frames_into_impl(const DicomFile& df, std::span<std::uint8_t> dst,
    const DecodePlan& plan, DecodeInfo* decode_info,
    const ExecutionObserver* observer) {
	pixel_decoder_info abi_info{};
	pixel_decoder_info* abi_info_ptr = nullptr;
	if (decode_info != nullptr) {
		abi_info.struct_size = sizeof(pixel_decoder_info);
		abi_info.abi_version = PIXEL_DECODER_PLUGIN_ABI;
		abi_info_ptr = &abi_info;
	}

	try {
		detail::dispatch_decode_all_frames(df, dst, plan, observer, abi_info_ptr);
		if (decode_info != nullptr) {
			assign_decode_info(abi_info, plan.output_layout.photometric, *decode_info);
		}
	} catch (const diag::DicomException& ex) {
		detail::rethrow_codec_exception_at_boundary_or_throw(
		    "pixel::decode_all_frames_into", df, ex);
	}
}

} // namespace

DecodePlan create_decode_plan(const DicomFile& df, const DecodeOptions& opt) {
	DecodePlan plan{};
	// Snapshot pixel metadata once so decode_frame_into() can stay on the hot path.
	const auto source_layout = support_detail::compute_decode_source_layout(df);
	// Store the caller policy unchanged so every decode entry point uses the same plan contract.
	plan.options = opt;
	// Precompute the exact destination layout expected by the decode path.
	plan.output_layout =
	    detail::compute_decode_output_layout_or_throw(df, source_layout, plan.options);
	return plan;
}

void decode_frame_into(const DicomFile& df, std::size_t frame_index,
    std::span<std::uint8_t> dst, const DecodePlan& plan) {
	decode_frame_into_impl(df, frame_index, dst, plan, nullptr);
}

void decode_frame_into(const DicomFile& df, std::size_t frame_index,
    std::span<std::uint8_t> dst, const DecodePlan& plan, DecodeInfo& decode_info) {
	decode_frame_into_impl(df, frame_index, dst, plan, &decode_info);
}

PixelBuffer decode_frame(
    const DicomFile& df, std::size_t frame_index, const DecodePlan& plan) {
	// Single-frame decode owns only one frame worth of storage even when the plan
	// was prepared for a multi-frame pixel payload.
	auto decoded = PixelBuffer::allocate(plan.output_layout.single_frame());

	// Reuse the same validated dispatch path as the span-based API.
	decode_frame_into(df, frame_index, decoded.bytes, plan);
	return decoded;
}

PixelBuffer decode_frame(const DicomFile& df, std::size_t frame_index,
    const DecodePlan& plan, DecodeInfo& decode_info) {
	auto decoded = PixelBuffer::allocate(plan.output_layout.single_frame());
	decode_frame_into(df, frame_index, decoded.bytes, plan, decode_info);
	return decoded;
}

void decode_all_frames_into(
    const DicomFile& df, std::span<std::uint8_t> dst, const DecodePlan& plan) {
	decode_all_frames_into_impl(df, dst, plan, nullptr, nullptr);
}

void decode_all_frames_into(const DicomFile& df, std::span<std::uint8_t> dst,
    const DecodePlan& plan, DecodeInfo& decode_info) {
	decode_all_frames_into_impl(df, dst, plan, &decode_info, nullptr);
}

PixelBuffer decode_all_frames(const DicomFile& df, const DecodePlan& plan) {
	// Full-volume decode owns exactly the storage implied by the planned output layout.
	auto decoded = PixelBuffer::allocate(plan.output_layout);

	// Reuse the existing batch path so validation and backend selection stay centralized.
	decode_all_frames_into(df, decoded.bytes, plan);
	return decoded;
}

PixelBuffer decode_all_frames(
    const DicomFile& df, const DecodePlan& plan, DecodeInfo& decode_info) {
	auto decoded = PixelBuffer::allocate(plan.output_layout);
	decode_all_frames_into(df, decoded.bytes, plan, decode_info);
	return decoded;
}

void decode_all_frames_into(const DicomFile& df, std::span<std::uint8_t> dst,
    const DecodePlan& plan, const ExecutionObserver* observer) {
	decode_all_frames_into_impl(df, dst, plan, nullptr, observer);
}

void decode_all_frames_into(const DicomFile& df, std::span<std::uint8_t> dst,
    const DecodePlan& plan, DecodeInfo& decode_info,
    const ExecutionObserver* observer) {
	decode_all_frames_into_impl(df, dst, plan, &decode_info, observer);
}

PixelBuffer decode_all_frames(
    const DicomFile& df, const DecodePlan& plan, const ExecutionObserver* observer) {
	// The owning convenience keeps observer-aware decode on the same dispatcher.
	auto decoded = PixelBuffer::allocate(plan.output_layout);

	// Reuse the observer-enabled batch path without duplicating scheduling logic.
	decode_all_frames_into(df, decoded.bytes, plan, observer);
	return decoded;
}

PixelBuffer decode_all_frames(
    const DicomFile& df, const DecodePlan& plan, DecodeInfo& decode_info,
    const ExecutionObserver* observer) {
	auto decoded = PixelBuffer::allocate(plan.output_layout);
	decode_all_frames_into(df, decoded.bytes, plan, decode_info, observer);
	return decoded;
}

} // namespace pixel

} // namespace dicom
