#include "dicom.h"

#include "pixel/host/decode/decode_frame_dispatch.hpp"
#include "pixel/host/decode/decode_plan_compute.hpp"
#include "pixel/host/decode/decode_modality_value_transform.hpp"

namespace dicom {

namespace pixel {

DecodePlan create_decode_plan(const DicomFile& df, const DecodeOptions& opt) {
	DecodePlan plan{};
	// Snapshot pixel metadata once so decode_frame_into() can stay on the hot path.
	plan.info = df.pixeldata_info();
	plan.modality_value_transform =
	    detail::compute_modality_value_transform(df, plan.info, opt);
	plan.options = opt;
	plan.options.to_modality_value = plan.modality_value_transform.enabled;
	// Precompute the exact destination layout expected by the decode path.
	plan.strides =
	    detail::compute_decode_strides_or_throw(df.path(), plan.info, plan.options);
	return plan;
}

void decode_frame_into(const DicomFile& df, std::size_t frame_index,
    std::span<std::uint8_t> dst, const DecodePlan& plan) {
	detail::dispatch_decode_frame(df, frame_index, dst, plan);
}

} // namespace pixel

} // namespace dicom
