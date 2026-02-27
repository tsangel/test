#pragma once

#include "pixel/registry/codec_registry.hpp"

#include <cstddef>

namespace dicom::pixel::detail {

void invoke_decode_plugin_or_throw(const DicomFile& df,
    const TransferSyntaxPluginBinding& binding, const CodecPlugin& plugin,
    const CodecDecodeFrameInput& decode_input, std::size_t frame_index);

} // namespace dicom::pixel::detail
