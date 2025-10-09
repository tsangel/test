#include <dicom.h>

std::unique_ptr<DicomFile> DicomFile::attach(const std::string& path) {
	return std::unique_ptr<DicomFile>(new DicomFile(path));
}

DicomFile::DicomFile(const std::string& path) : path_(path) {}

const std::string& DicomFile::path() const {
	return path_;
}
