#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace dicom {

std::vector<std::uint8_t> normalize_big_endian_dataset(
    std::span<const std::uint8_t> full_input, std::size_t dataset_start_offset);

std::vector<std::uint8_t> convert_little_endian_dataset_to_big_endian(
    std::span<const std::uint8_t> full_input, std::size_t dataset_start_offset);

} // namespace dicom
