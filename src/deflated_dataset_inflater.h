#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace dicom {

std::vector<std::uint8_t> inflate_deflated_dataset(std::span<const std::uint8_t> full_input,
    std::size_t deflated_start_offset, const std::string& file_path);

} // namespace dicom
