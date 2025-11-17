// Auto-generated from misc/dictionary/_specific_character_sets.tsv
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace dicom {

enum class SpecificCharacterSet : std::uint16_t {
    Unknown = 0,
    NONE = 1,
    ISO_IR_6 = 2,
    ISO_IR_100 = 3,
    ISO_IR_101 = 4,
    ISO_IR_109 = 5,
    ISO_IR_110 = 6,
    ISO_IR_144 = 7,
    ISO_IR_127 = 8,
    ISO_IR_126 = 9,
    ISO_IR_138 = 10,
    ISO_IR_148 = 11,
    ISO_IR_203 = 12,
    ISO_IR_13 = 13,
    ISO_IR_166 = 14,
    ISO_2022_IR_6 = 15,
    ISO_2022_IR_100 = 16,
    ISO_2022_IR_101 = 17,
    ISO_2022_IR_109 = 18,
    ISO_2022_IR_110 = 19,
    ISO_2022_IR_144 = 20,
    ISO_2022_IR_127 = 21,
    ISO_2022_IR_126 = 22,
    ISO_2022_IR_138 = 23,
    ISO_2022_IR_148 = 24,
    ISO_2022_IR_203 = 25,
    ISO_2022_IR_13 = 26,
    ISO_2022_IR_166 = 27,
    ISO_2022_IR_87 = 28,
    ISO_2022_IR_159 = 29,
    ISO_2022_IR_149 = 30,
    ISO_2022_IR_58 = 31,
    ISO_IR_192 = 32,
    GB18030 = 33,
    GBK = 34,
};

struct SpecificCharacterSetInfo {
    SpecificCharacterSet value;
    std::string_view defined_term;
    std::string_view description;
};

inline constexpr std::array<SpecificCharacterSetInfo, 34> kSpecificCharacterSetInfo = {{
    {SpecificCharacterSet::NONE, "none", "Default repertoire"},
    {SpecificCharacterSet::ISO_IR_6, "ISO_IR 6", "Default repertoire"},
    {SpecificCharacterSet::ISO_IR_100, "ISO_IR 100", "Latin alphabet No. 1"},
    {SpecificCharacterSet::ISO_IR_101, "ISO_IR 101", "Latin alphabet No. 2"},
    {SpecificCharacterSet::ISO_IR_109, "ISO_IR 109", "Latin alphabet No. 3"},
    {SpecificCharacterSet::ISO_IR_110, "ISO_IR 110", "Latin alphabet No. 4"},
    {SpecificCharacterSet::ISO_IR_144, "ISO_IR 144", "Cyrillic"},
    {SpecificCharacterSet::ISO_IR_127, "ISO_IR 127", "Arabic"},
    {SpecificCharacterSet::ISO_IR_126, "ISO_IR 126", "Greek"},
    {SpecificCharacterSet::ISO_IR_138, "ISO_IR 138", "Hebrew"},
    {SpecificCharacterSet::ISO_IR_148, "ISO_IR 148", "Latin alphabet No. 5"},
    {SpecificCharacterSet::ISO_IR_203, "ISO_IR 203", "Latin alphabet No. 9"},
    {SpecificCharacterSet::ISO_IR_13, "ISO_IR 13", "Japanese"},
    {SpecificCharacterSet::ISO_IR_166, "ISO_IR 166", "Thai"},
    {SpecificCharacterSet::ISO_2022_IR_6, "ISO 2022 IR 6", "Default repertoire"},
    {SpecificCharacterSet::ISO_2022_IR_100, "ISO 2022 IR 100", "Latin alphabet No. 1"},
    {SpecificCharacterSet::ISO_2022_IR_101, "ISO 2022 IR 101", "Latin alphabet No. 2"},
    {SpecificCharacterSet::ISO_2022_IR_109, "ISO 2022 IR 109", "Latin alphabet No. 3"},
    {SpecificCharacterSet::ISO_2022_IR_110, "ISO 2022 IR 110", "Latin alphabet No. 4"},
    {SpecificCharacterSet::ISO_2022_IR_144, "ISO 2022 IR 144", "Cyrillic"},
    {SpecificCharacterSet::ISO_2022_IR_127, "ISO 2022 IR 127", "Arabic"},
    {SpecificCharacterSet::ISO_2022_IR_126, "ISO 2022 IR 126", "Greek"},
    {SpecificCharacterSet::ISO_2022_IR_138, "ISO 2022 IR 138", "Hebrew"},
    {SpecificCharacterSet::ISO_2022_IR_148, "ISO 2022 IR 148", "Latin alphabet No. 5"},
    {SpecificCharacterSet::ISO_2022_IR_203, "ISO 2022 IR 203", "Latin alphabet No. 9"},
    {SpecificCharacterSet::ISO_2022_IR_13, "ISO 2022 IR 13", "Japanese"},
    {SpecificCharacterSet::ISO_2022_IR_166, "ISO 2022 IR 166", "Thai"},
    {SpecificCharacterSet::ISO_2022_IR_87, "ISO 2022 IR 87", "Japanese"},
    {SpecificCharacterSet::ISO_2022_IR_159, "ISO 2022 IR 159", "Japanese"},
    {SpecificCharacterSet::ISO_2022_IR_149, "ISO 2022 IR 149", "Korean"},
    {SpecificCharacterSet::ISO_2022_IR_58, "ISO 2022 IR 58", "Simplified Chinese"},
    {SpecificCharacterSet::ISO_IR_192, "ISO_IR 192", "Unicode in UTF-8"},
    {SpecificCharacterSet::GB18030, "GB18030", "GB18030"},
    {SpecificCharacterSet::GBK, "GBK", "GBK"},
}};

namespace detail {

constexpr bool sv_equal(std::string_view lhs, std::string_view rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        if (lhs[i] != rhs[i]) {
            return false;
        }
    }
    return true;
}

} // namespace detail

inline constexpr SpecificCharacterSet specific_character_set_from_term(std::string_view term) {
    for (const auto& info : kSpecificCharacterSetInfo) {
        if (detail::sv_equal(info.defined_term, term)) {
            return info.value;
        }
    }
    return SpecificCharacterSet::Unknown;
}

inline constexpr const SpecificCharacterSetInfo* specific_character_set_info(SpecificCharacterSet set) {
    for (const auto& info : kSpecificCharacterSetInfo) {
        if (info.value == set) {
            return &info;
        }
    }
    return nullptr;
}

} // namespace dicom
