#pragma once

#include <optional>
#include <string>

#include "../dicom.h"
#include "storage_classifier.hpp"

namespace dicom::storage {

[[nodiscard]] inline std::optional<StorageClassifier> make_storage_classifier(
    const DataSet& dataset) {
    const auto sop_class_uid = dataset.get_value<std::string>("SOPClassUID");
    if (!sop_class_uid || sop_class_uid->empty()) {
        return std::nullopt;
    }
    return StorageClassifier::from_sop_class_uid(*sop_class_uid);
}

[[nodiscard]] inline std::optional<StorageClassifier> make_storage_classifier(
    const DicomFile& file) {
    return make_storage_classifier(file.dataset());
}

} // namespace dicom::storage
