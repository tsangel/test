#pragma once

#include "pixel/registry/codec_registry.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace dicom::pixel::detail {

struct ResolvedDecodeFrameSource {
	std::span<const std::uint8_t> bytes{};
	std::vector<std::uint8_t> owned_bytes{};
};

[[nodiscard]] ResolvedDecodeFrameSource resolve_decode_frame_source_or_throw(
    const DicomFile& df, const TransferSyntaxPluginBinding& binding,
    std::size_t frame_index);

} // namespace dicom::pixel::detail
