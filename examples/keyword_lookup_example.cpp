#include <iomanip>
#include <iostream>
#include <string_view>

#include <dicom.h>

int main(int argc, char** argv) {
    if (argc >= 2 && (std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h")) {
        std::cout << "Usage: " << argv[0] << " <keyword> [<keyword> ...]\n";
        using namespace dicom::literals;
        const dicom::Tag literal_tag = "PatientName"_tag;
        std::cout << "Example literal: \"PatientName\"_tag -> ("
                  << std::hex << std::uppercase << std::setfill('0')
                  << std::setw(4) << literal_tag.group() << ','
                  << std::setw(4) << literal_tag.element() << ")\n"
                  << std::dec << std::nouppercase;
        std::cout << std::setfill(' ');
        return 0;
    }

    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <keyword> [<keyword> ...]\n";
        return 1;
    }
    for (int i = 1; i < argc; ++i) {
        const std::string_view keyword{argv[i]};
        const auto tag = dicom::lookup::keyword_to_tag(keyword);
        if (tag.empty()) {
            std::cout << keyword << ": not found\n";
        } else {
            std::cout << keyword << " -> " << tag << "\n";
        }
    }
    return 0;
}
