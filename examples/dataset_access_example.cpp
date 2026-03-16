#include <dicom.h>

#include <iostream>
#include <string_view>

using namespace dicom::literals;

int main() {
	dicom::DataSet ds;

	auto& rows = ds.ensure_dataelement("Rows"_tag, dicom::VR::US);
	rows.from_long(512);

	if (!ds.set_value("Columns"_tag, 256L)) {
		std::cerr << "Failed to assign Columns\n";
		return 1;
	}

	auto& private_elem = ds.ensure_dataelement(dicom::Tag(0x0009, 0x0030), dicom::VR::US);
	private_elem.from_long(16);

	auto& preserved = ds.ensure_dataelement("Rows"_tag, dicom::VR::UL);
	std::cout << "Rows VR after ensure(UL): " << preserved.vr().str() << "\n";
	std::cout << "Rows value: " << ds.get_value<long>("Rows"_tag, 0L) << "\n";
	std::cout << "Columns value: " << ds.get_value<long>("Columns"_tag, 0L) << "\n";
	std::cout << "StudyDescription: "
	          << ds.get_value<std::string_view>(
	                 "StudyDescription"_tag, std::string_view("<missing>"))
	          << "\n";
	std::cout << "Private value: " << private_elem.to_long().value_or(0) << "\n";

	return 0;
}
