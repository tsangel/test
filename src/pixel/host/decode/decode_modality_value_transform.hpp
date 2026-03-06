#pragma once

#include "dicom.h"

namespace dicom::pixel::detail {

using ModalityValueTransform = pixel::ModalityValueTransform;

[[nodiscard]] ModalityValueTransform compute_modality_value_transform(
    const DicomFile& df, const PixelDataInfo& info, const DecodeOptions& opt);

} // namespace dicom::pixel::detail
