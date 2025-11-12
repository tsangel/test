#include <iostream>
#include <string_view>

#include "keyword_lookup.hpp"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <keyword> [<keyword> ...]\n";
        return 1;
    }
    for (int i = 1; i < argc; ++i) {
        const std::string_view keyword{argv[i]};
        const auto tag = dicom::lookup::keyword_to_tag_chd(keyword);
        if (tag.empty()) {
            std::cout << keyword << ": not found\n";
        } else {
            std::cout << keyword << " -> " << tag << "\n";
        }
    }
    return 0;
}
