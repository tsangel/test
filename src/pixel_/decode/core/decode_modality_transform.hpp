#pragma once

#include "dicom.h"
#include "pixel_/registry/codec_registry.hpp"

namespace dicom::pixel::detail {

struct ResolvedDecodeValueTransform {
	DecodeValueTransform transform{};
	DecodeOptions options{};
};

[[nodiscard]] ResolvedDecodeValueTransform resolve_decode_value_transform(
    const DicomFile& df, const DecodeOptions& opt);

} // namespace dicom::pixel::detail
