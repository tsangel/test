#include "pixel/decode/core/decode_entrypoint_runner.hpp"

#include "pixel/decode/core/decode_frame_dispatch.hpp"
#include "pixel/decode/core/decode_modality_value_transform.hpp"

namespace dicom::pixel::detail {

namespace {

void decode_frame_with_computed_options(const DicomFile& df,
    std::size_t frame_index, std::span<std::uint8_t> dst,
    const DecodeStrides& dst_strides,
    const DecodeContext& context) {
	dispatch_decode_frame_with_computed_options(
	    df, context.modality_value_transform, frame_index, dst, dst_strides,
	    context.effective_options);
}

} // namespace

bool should_output_modality_value_with_computed_options(
    const DicomFile& df, const DecodeOptions& opt) {
	return build_decode_context(df, opt).modality_value_transform.enabled;
}

void run_decode_frame_with_computed_options(const DicomFile& df,
    std::size_t frame_index, std::span<std::uint8_t> dst,
    const DecodeOptions& opt) {
	const auto context = build_decode_context(df, opt);
	const auto dst_strides = df.calc_decode_strides(context.effective_options);
	decode_frame_with_computed_options(
	    df, frame_index, dst, dst_strides, context);
}

void run_decode_frame_with_computed_options(const DicomFile& df,
    std::size_t frame_index, std::span<std::uint8_t> dst,
    const DecodeStrides& dst_strides, const DecodeOptions& opt) {
	const auto context = build_decode_context(df, opt);
	decode_frame_with_computed_options(
	    df, frame_index, dst, dst_strides, context);
}

} // namespace dicom::pixel::detail
