#pragma once

#include "dicom.h"

#include <cstddef>
#include <span>
#include <string_view>

namespace dicom::pixel::detail {

[[nodiscard]] bool should_use_multicomponent_transform(uid::WellKnown transfer_syntax,
    uint32_t codec_profile_code, std::span<const CodecOptionKv> codec_options,
    std::size_t samples_per_pixel);

} // namespace dicom::pixel::detail
