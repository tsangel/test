#include <iostream>

#include <dicom.h>
#include <dataelement_registry.hpp>

int main() {
	for (const auto& entry : dicom::kDataElementRegistry) {
		if (!entry.retired.empty()) {
			continue;  // Retired entries are not part of the lookup tables.
		}
		const auto [tag, vr] = dicom::lookup::keyword_to_tag_vr(entry.keyword);
		if (!tag || tag.value() != entry.tag_value) {
			std::cerr << "Keyword lookup mismatch for " << entry.keyword << " - expected 0x"
			          << std::hex << entry.tag_value << ", got 0x" << tag.value() << std::dec << "\n";
			return 1;
		}
		const auto roundtrip_keyword = dicom::lookup::tag_to_keyword(entry.tag_value);
		if (roundtrip_keyword != entry.keyword) {
			std::cerr << "Tag lookup mismatch for 0x" << std::hex << entry.tag_value << std::dec
			          << " - expected " << entry.keyword << ", got " << roundtrip_keyword << "\n";
			return 2;
		}
		(void)vr;
	}
	return 0;
}
