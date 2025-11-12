#include <cstdint>
#include <iostream>
#include <limits>
#include <string_view>

#include "dictionary_lookup.hpp"

namespace {

bool parse_tag_digits(char ch, std::uint32_t& value, int& digits) {
    unsigned digit = 0;
    if (ch >= '0' && ch <= '9') {
        digit = static_cast<unsigned>(ch - '0');
    } else if (ch >= 'A' && ch <= 'F') {
        digit = 10u + static_cast<unsigned>(ch - 'A');
    } else if (ch >= 'a' && ch <= 'f') {
        digit = 10u + static_cast<unsigned>(ch - 'a');
    } else {
        return false;
    }
    value = (value << 4) | digit;
    ++digits;
    return true;
}

bool parse_tag(std::string_view text, std::uint32_t& value_out) {
    std::uint32_t value = 0;
    int digits = 0;
    for (char ch : text) {
        if (ch == '(' || ch == ')' || ch == ',' || ch == ' ') {
            continue;
        }
        if (!parse_tag_digits(ch, value, digits)) {
            return false;
        }
    }
    if (digits != 8) {
        return false;
    }
    value_out = value;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <tag> [<tag> ...]" << std::endl;
        std::cerr << "Accepts tags like (0010,0010) or 00100010" << std::endl;
        return 1;
    }

    for (int i = 1; i < argc; ++i) {
        const std::string_view text{argv[i]};
        std::uint32_t tag_value = 0;
        if (!parse_tag(text, tag_value)) {
            std::cout << text << ": invalid tag format" << std::endl;
            continue;
        }

        const auto* entry = dicom::lookup::tag_to_entry(tag_value);
        if (!entry) {
            std::cout << text << ": tag not found" << std::endl;
            continue;
        }

        std::cout << text << " -> keyword=" << entry->keyword << ", name=" << entry->name << std::endl;
    }

    return 0;
}
