#pragma once

#include <optional>
#include <string>

#include "../dicom.h"
#include "storage_classifier.hpp"

namespace dicom::storage {

[[nodiscard]] inline std::optional<std::string> find_storage_sop_class_uid(
    const DataSet& dataset) {
    if (const auto sop_class_uid = dataset.get_value<std::string>("SOPClassUID");
        sop_class_uid && !sop_class_uid->empty()) {
        return uid::normalize_uid_text(*sop_class_uid);
    }
    if (const auto media_storage_sop_class_uid =
            dataset.get_value<std::string>("MediaStorageSOPClassUID");
        media_storage_sop_class_uid && !media_storage_sop_class_uid->empty()) {
        return uid::normalize_uid_text(*media_storage_sop_class_uid);
    }
    return std::nullopt;
}

[[nodiscard]] inline std::optional<StorageClassifier> make_storage_classifier(
    const DataSet& dataset) {
    const auto sop_class_uid = find_storage_sop_class_uid(dataset);
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
