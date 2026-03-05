#include "dicom.h"

#include "pixel/decode/core/decode_entrypoint_runner.hpp"

namespace dicom {

namespace pixel {

bool should_output_modality_value(const DicomFile& df, const DecodeOptions& opt) {
	return detail::should_output_modality_value_with_computed_options(df, opt);
}

void decode_frame_into(const DicomFile& df, std::size_t frame_index,
    std::span<std::uint8_t> dst, const DecodeOptions& opt) {
	detail::run_decode_frame_with_computed_options(df, frame_index, dst, opt);
}

void decode_frame_into(const DicomFile& df, std::size_t frame_index,
    std::span<std::uint8_t> dst, const DecodeStrides& dst_strides,
    const DecodeOptions& opt) {
	detail::run_decode_frame_with_computed_options(
	    df, frame_index, dst, dst_strides, opt);
}

} // namespace pixel

} // namespace dicom
