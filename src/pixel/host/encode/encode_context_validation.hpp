#pragma once

#include "dicom.h"

#include <string_view>

namespace dicom::pixel::detail {

void validate_encoder_context_for_set_pixel_data_or_throw(
    std::string_view file_path, uid::WellKnown transfer_syntax,
    bool encoder_context_configured, uid::WellKnown encoder_context_transfer_syntax);

} // namespace dicom::pixel::detail
