#pragma once

#include "dicom.h"

#include <cstddef>
#include <span>
#include <string_view>

namespace dicom::pixel::detail {

[[nodiscard]] bool should_use_multicomponent_transform(uid::WellKnown transfer_syntax,
    bool is_j2k_target, bool is_htj2k_target, std::span<const CodecOptionKv> codec_options,
    std::size_t samples_per_pixel, std::string_view file_path);

} // namespace dicom::pixel::detail
