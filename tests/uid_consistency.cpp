#include <iostream>

#include <dicom.h>
#include "uid_registry.hpp"

int main() {
    std::size_t checked = 0;
    for (std::size_t i = 0; i < dicom::kUidRegistry.size(); ++i) {
        const auto& entry = dicom::kUidRegistry[i];
        if (entry.keyword.empty()) {
            continue;
        }
        const dicom::Uid from_value = dicom::Uid::from_value(entry.value);
        const dicom::Uid from_keyword = dicom::Uid::from_keyword(entry.keyword);
        if (!from_value.valid()) {
            std::cerr << "UID value lookup failed for " << entry.value << "\n";
            return 1;
        }
        if (!from_keyword.valid()) {
            std::cerr << "UID keyword lookup failed for " << entry.keyword << "\n";
            return 1;
        }
        if (from_value != from_keyword) {
            std::cerr << "Mismatch: value " << entry.value << " != keyword "
                      << entry.keyword << " (indices " << from_value.raw_index()
                      << " vs " << from_keyword.raw_index() << ")\n";
            return 1;
        }
        ++checked;
    }
    std::cout << "Checked " << checked << " UID value/keyword pairs\n";
    return 0;
}
