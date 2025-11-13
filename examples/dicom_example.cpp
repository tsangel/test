#include <iostream>
#include <dicom.h>

using dicom::DataSet;
using dicom::read_file;

int main() {
	auto dicom = read_file("/tmp/sample.dcm");
	std::cout << "Dicom path: " << dicom->path() << std::endl;
	return 0;
}
