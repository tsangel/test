#include "pixel_/decode/core/decode_entrypoint_runner.hpp"

#include "pixel_/decode/core/decode_frame_dispatch.hpp"
#include "pixel_/decode/core/decode_modality_transform.hpp"

namespace dicom::pixel::detail {

namespace {

void decode_frame_with_resolved_transform(const DicomFile& df,
    std::size_t frame_index, std::span<std::uint8_t> dst,
    const DecodeStrides& dst_strides,
    const ResolvedDecodeValueTransform& resolved) {
	dispatch_decode_frame_with_resolved_transform(
	    df, resolved.transform, frame_index, dst, dst_strides, resolved.options);
}

} // namespace

bool should_use_scaled_output_with_resolved_options(
    const DicomFile& df, const DecodeOptions& opt) {
	return resolve_decode_value_transform(df, opt).transform.enabled;
}

void run_decode_frame_with_resolved_options(const DicomFile& df,
    std::size_t frame_index, std::span<std::uint8_t> dst,
    const DecodeOptions& opt) {
	const auto resolved = resolve_decode_value_transform(df, opt);
	const auto dst_strides = df.calc_decode_strides(resolved.options);
	decode_frame_with_resolved_transform(
	    df, frame_index, dst, dst_strides, resolved);
}

void run_decode_frame_with_resolved_options(const DicomFile& df,
    std::size_t frame_index, std::span<std::uint8_t> dst,
    const DecodeStrides& dst_strides, const DecodeOptions& opt) {
	const auto resolved = resolve_decode_value_transform(df, opt);
	decode_frame_with_resolved_transform(
	    df, frame_index, dst, dst_strides, resolved);
}

} // namespace dicom::pixel::detail
