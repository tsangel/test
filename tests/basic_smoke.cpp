#include <cassert>
#include <string>

#include <dicom.h>

int main() {
	using dicom::lookup::keyword_to_tag_vr;
	using dicom::lookup::tag_to_keyword;
	using dicom::DicomFile;
	using namespace dicom::literals;

	const auto [tag, vr] = keyword_to_tag_vr("PatientName");
	assert(tag);
	assert(tag.value() == 0x00100010u);
	assert(vr.str() == std::string_view("PN"));
	assert(tag_to_keyword(tag.value()) == std::string_view("PatientName"));

	const dicom::Tag literal_tag = "Rows"_tag;
	assert(literal_tag.value() == 0x00280010u);

	const auto file = DicomFile::attach("/tmp/nonexistent.dcm");
	assert(file);
	assert(file->path() == "/tmp/nonexistent.dcm");

	return 0;
}
