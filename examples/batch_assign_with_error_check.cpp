#include <dicom.h>
#include <diagnostics.h>

#include <iostream>
#include <memory>

using namespace dicom::literals;

int main() {
	dicom::DataSet ds;
	auto reporter = std::make_shared<dicom::diag::BufferingReporter>(256);
	dicom::diag::set_thread_reporter(reporter);

	bool ok = true;
	try {
		ok &= ds.add_dataelement("Rows"_tag, dicom::VR::US).from_long(512);
		ok &= ds.add_dataelement("Columns"_tag, dicom::VR::US).from_long(-1); // Intentional failure.
		ok &= ds.add_dataelement("BitsAllocated"_tag, dicom::VR::US).from_long(16);
		ok &= ds.add_dataelement("SOPInstanceUID"_tag, dicom::VR::UI)
		          .from_uid_string("1.2.840.10008.5.1.4.1.1.2");
		ok &= ds.add_dataelement("TransferSyntaxUID"_tag, dicom::VR::UI)
		          .from_uid_string("not-a-uid"); // Intentional failure.
	} catch (const std::exception& ex) {
		std::cerr << "add_dataelement threw: " << ex.what() << '\n';
		dicom::diag::set_thread_reporter(nullptr);
		return 2;
	}

	if (!ok) {
		std::cerr << "One or more assignments failed:\n";
		for (const auto& message : reporter->take_messages()) {
			std::cerr << "  " << message << '\n';
		}
		dicom::diag::set_thread_reporter(nullptr);
		return 1;
	}

	std::cout << "All assignments succeeded.\n";
	dicom::diag::set_thread_reporter(nullptr);
	return 0;
}
