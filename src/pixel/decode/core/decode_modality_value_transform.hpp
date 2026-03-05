#pragma once

#include "dicom.h"
#include "pixel/registry/codec_registry.hpp"

namespace dicom::pixel::detail {

struct DecodeContext {
	DecodeOptions effective_options{};
	ModalityValueTransform modality_value_transform{};
};

[[nodiscard]] DecodeContext build_decode_context(
    const DicomFile& df, const DecodeOptions& opt);

} // namespace dicom::pixel::detail
