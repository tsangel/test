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
		ok &= ds.set_value("Rows"_tag, 512L);
		ok &= ds.set_value("Columns"_tag, -1L); // Intentional failure.
		ok &= ds.set_value("BitsAllocated"_tag, 16L);
		ok &= ds.set_value(
		    "SOPInstanceUID"_tag, dicom::VR::UI, std::string_view("1.2.840.10008.5.1.4.1.1.2"));
		ok &= ds.set_value(
		    "TransferSyntaxUID"_tag, dicom::VR::UI, std::string_view("not-a-uid")); // Intentional failure.
	} catch (const std::exception& ex) {
		std::cerr << "set_value threw: " << ex.what() << '\n';
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
