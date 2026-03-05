#pragma once

#include "dicom.h"

#include <span>

namespace dicom::pixel::detail {

void run_set_pixel_data_with_resolved_codec_options(DicomFile& file,
    uid::WellKnown transfer_syntax, const pixel::PixelSource& source,
    std::span<const CodecOptionKv> codec_options);

} // namespace dicom::pixel::detail
