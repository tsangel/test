#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace dicom {

std::vector<std::uint8_t> normalize_big_endian_dataset(
    std::span<const std::uint8_t> full_input, std::size_t dataset_start_offset,
    const std::string& file_path);

} // namespace dicom
