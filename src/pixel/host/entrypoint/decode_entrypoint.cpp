#include "dicom.h"

#include "pixel/host/decode/decode_frame_dispatch.hpp"
#include "pixel/host/decode/decode_plan_compute.hpp"
#include "pixel/host/support/dicom_pixel_support.hpp"

namespace dicom {

namespace pixel {

DecodePlan create_decode_plan(const DicomFile& df, const DecodeOptions& opt) {
	DecodePlan plan{};
	// Snapshot pixel metadata once so decode_frame_into() can stay on the hot path.
	const auto source_layout = support_detail::compute_decode_source_layout(df);
	// Store the caller policy unchanged so every decode entry point uses the same plan contract.
	plan.options = opt;
	// Precompute the exact destination layout expected by the decode path.
	plan.output_layout =
	    detail::compute_decode_output_layout_or_throw(df.path(), source_layout, plan.options);
	return plan;
}

void decode_frame_into(const DicomFile& df, std::size_t frame_index,
    std::span<std::uint8_t> dst, const DecodePlan& plan) {
	// Keep the public wrapper thin so dispatch logic stays in one implementation unit.
	detail::dispatch_decode_frame(df, frame_index, dst, plan);
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

void decode_all_frames_into(
    const DicomFile& df, std::span<std::uint8_t> dst, const DecodePlan& plan) {
	// Batch decode shares the same dispatcher and validation rules as single-frame decode.
	detail::dispatch_decode_all_frames(df, dst, plan);
}

PixelBuffer decode_all_frames(const DicomFile& df, const DecodePlan& plan) {
	// Full-volume decode owns exactly the storage implied by the planned output layout.
	auto decoded = PixelBuffer::allocate(plan.output_layout);

	// Reuse the existing batch path so validation and backend selection stay centralized.
	decode_all_frames_into(df, decoded.bytes, plan);
	return decoded;
}

void decode_all_frames_into(const DicomFile& df, std::span<std::uint8_t> dst,
    const DecodePlan& plan, const ExecutionObserver* observer) {
	// Observer-aware decode is just the same batch path with cooperative progress/cancel hooks.
	detail::dispatch_decode_all_frames(df, dst, plan, observer);
}

PixelBuffer decode_all_frames(
    const DicomFile& df, const DecodePlan& plan, const ExecutionObserver* observer) {
	// The owning convenience keeps observer-aware decode on the same dispatcher.
	auto decoded = PixelBuffer::allocate(plan.output_layout);

	// Reuse the observer-enabled batch path without duplicating scheduling logic.
	decode_all_frames_into(df, decoded.bytes, plan, observer);
	return decoded;
}

} // namespace pixel

} // namespace dicom
