#pragma once

#include "dicom.h"

#include <cstddef>
#include <span>
#include <string_view>

namespace dicom::pixel::detail {

struct MulticomponentTransformOptionState {
	bool found{false};
	bool valid{true};
	bool value{false};
};

[[nodiscard]] MulticomponentTransformOptionState
lookup_multicomponent_transform_option(
    std::span<const CodecOptionKv> codec_options) noexcept;

[[nodiscard]] bool should_use_multicomponent_transform(uid::WellKnown transfer_syntax,
    uint32_t codec_profile_code, std::span<const CodecOptionKv> codec_options,
    std::size_t samples_per_pixel);

} // namespace dicom::pixel::detail
