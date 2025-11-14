#include <iomanip>
#include <iostream>
#include <string_view>

#include <dicom.h>

namespace {

void print_usage(const char* argv0) {
    std::cout << "Usage: " << argv0 << " <uid-or-keyword> [<uid-or-keyword> ...]\n";
    using namespace dicom::literals;
    constexpr dicom::Uid implicit = "ImplicitVRLittleEndian"_uid;
    std::cout << "Example literal: \"ImplicitVRLittleEndian\"_uid -> "
              << implicit.value() << " (" << implicit.keyword() << ")\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h") {
        print_usage(argv[0]);
        return argc < 2 ? 1 : 0;
    }

    for (int i = 1; i < argc; ++i) {
        const std::string_view text{argv[i]};
        const dicom::Uid uid = dicom::Uid::lookup(text);
        if (!uid) {
            std::cout << text << ": not found\n";
            continue;
        }
        std::cout << text << " -> value=" << uid.value();
        if (!uid.keyword().empty()) {
            std::cout << ", keyword=" << uid.keyword();
        }
        std::cout << ", name=" << uid.name() << ", type=" << uid.type() << "\n";
    }
    return 0;
}
