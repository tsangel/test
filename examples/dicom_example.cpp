#include <iostream>
#include <dicom.h>

using dicom::DicomFile;

int main() {
	auto dicom = DicomFile::attach("/tmp/sample.dcm");
	std::cout << "Dicom path: " << dicom->path() << std::endl;
	return 0;
}
