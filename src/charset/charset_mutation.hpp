#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dicom.h"

namespace dicom::charset {

[[nodiscard]] bool encode_utf8_for_element(DataElement& element,
    std::span<const std::string_view> values,
    CharsetEncodeErrorPolicy errors = CharsetEncodeErrorPolicy::strict,
    std::string* out_error = nullptr, bool* out_replaced = nullptr);

[[nodiscard]] bool set_dataset_declared_charset(DataSet& dataset,
    std::span<const SpecificCharacterSet> charsets, std::string* out_error = nullptr);

[[nodiscard]] bool transcode_dataset_charset(DataSet& dataset,
    std::span<const SpecificCharacterSet> charsets,
    CharsetEncodeErrorPolicy errors = CharsetEncodeErrorPolicy::strict,
    std::string* out_error = nullptr, bool* out_replaced = nullptr);

}  // namespace dicom::charset
