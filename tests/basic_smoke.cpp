#include <cstdint>
#include <fstream>
#include <string>
#include <vector>
#include <iostream>
#include <cstdlib>
#include <cstdio>

#include <dicom.h>
#include <instream.h>

int main() {
	using dicom::lookup::keyword_to_tag_vr;
	using dicom::lookup::tag_to_keyword;
	using dicom::DataSet;
	using dicom::read_bytes;
	using dicom::read_file;
	using namespace dicom::literals;

	auto fail = [](const std::string& msg) {
		std::cerr << msg << std::endl;
		std::exit(1);
	};

	const auto [tag, vr] = keyword_to_tag_vr("PatientName");
	if (!tag) fail("keyword_to_tag_vr returned null tag");
	if (tag.value() != 0x00100010u) fail("tag value mismatch");
	if (vr.str() != std::string_view("PN")) fail("vr mismatch");
	if (tag_to_keyword(tag.value()) != std::string_view("PatientName")) fail("keyword roundtrip mismatch");

	const auto seq_vr = dicom::VR::SQ;
	if (!seq_vr.is_sequence()) fail("SQ should be sequence");
	if (seq_vr.is_pixel_sequence()) fail("SQ should not be pixel sequence");

	const auto px_vr = dicom::VR::PX;
	if (px_vr.is_sequence()) fail("PX should not be sequence");
	if (!px_vr.is_pixel_sequence()) fail("PX should be pixel sequence");
	if (!px_vr.is_binary()) fail("PX should be binary");
	if (px_vr.str() != std::string_view("PX")) fail("PX string mismatch");

	const dicom::Tag literal_tag = "Rows"_tag;
	if (literal_tag.value() != 0x00280010u) fail("literal tag mismatch");

	std::string tmp_dir = ".";
	if (const char* env = std::getenv("TMPDIR"); env && *env) {
		tmp_dir = env;
	} else if (const char* env = std::getenv("TEMP"); env && *env) {
		tmp_dir = env;
	} else if (const char* env = std::getenv("TMP"); env && *env) {
		tmp_dir = env;
	}
	if (!tmp_dir.empty() && tmp_dir.back() != '/' && tmp_dir.back() != '\\') {
		tmp_dir.push_back('/');
	}
	const std::string file_path = tmp_dir + "dicomsdl_basic_smoke.dcm";
	{
		std::ofstream os(file_path, std::ios::binary);
		os << "DICM";
	}
	{
		const auto file = read_file(file_path);
		if (!file) fail("read_file returned null");
		if (file->path() != file_path) fail("file path mismatch");
		if (file->stream().datasize() != 4) fail("file datasize mismatch");
		if (file->size() != file->dataset().size()) fail("DicomFile size forwarding mismatch");
		const auto file_dump = file->dump();
		if (file_dump.find("TAG\tVR\tLEN\tVM\tOFFSET\tVALUE\tKEYWORD") == std::string::npos) {
			fail("DicomFile dump header missing");
		}
		const auto file_dump_no_offset = file->dump(80, false);
		if (file_dump_no_offset.find("TAG\tVR\tLEN\tVM\tVALUE\tKEYWORD") == std::string::npos) {
			fail("DicomFile dump(include_offset=false) header missing");
		}
		if (file_dump_no_offset.find("OFFSET\tVALUE") != std::string::npos) {
			fail("DicomFile dump(include_offset=false) should hide OFFSET column");
		}

		auto* rows = file->add_dataelement("Rows"_tag, dicom::VR::US, 0, 2);
		if (!rows || rows == dicom::NullElement()) fail("DicomFile add_dataelement failed");
		const auto file_dump_after_add = file->dump();
		if (file_dump_after_add.find("'00280010'") == std::string::npos) {
			fail("DicomFile dump should include Rows");
		}
		const std::size_t long_desc_size = 256;
		void* long_desc_ptr = ::operator new(long_desc_size);
		std::memset(long_desc_ptr, static_cast<int>('A'), long_desc_size);
		auto* study_desc =
		    file->add_dataelement("StudyDescription"_tag, dicom::VR::LO, 0, long_desc_size);
		if (!study_desc || study_desc == dicom::NullElement()) fail("DicomFile add StudyDescription failed");
		study_desc->set_data(long_desc_ptr);
		const auto truncated_dump = file->dump();
		const auto study_desc_pos = truncated_dump.find("'00081030'");
		if (study_desc_pos == std::string::npos) fail("DicomFile dump should include StudyDescription");
		const auto study_desc_line_end = truncated_dump.find('\n', study_desc_pos);
		const auto study_desc_line = truncated_dump.substr(
		    study_desc_pos, study_desc_line_end == std::string::npos
		                        ? std::string::npos
		                        : study_desc_line_end - study_desc_pos);
		if (study_desc_line.find("...") == std::string::npos) {
			fail("DicomFile dump should truncate long VALUE with ellipsis");
		}
		const auto wide_dump = file->dump(1000);
		const auto wide_pos = wide_dump.find("'00081030'");
		if (wide_pos == std::string::npos) fail("DicomFile wide dump should include StudyDescription");
		const auto wide_line_end = wide_dump.find('\n', wide_pos);
		const auto wide_line = wide_dump.substr(
		    wide_pos, wide_line_end == std::string::npos
		                  ? std::string::npos
		                  : wide_line_end - wide_pos);
		if (wide_line.find("...") != std::string::npos) {
			fail("DicomFile dump(max_print_chars) should relax truncation");
		}
		if (!file->get_dataelement("Rows"_tag) || file->get_dataelement("Rows"_tag) == dicom::NullElement()) {
			fail("DicomFile get_dataelement(Tag) failed");
		}
		if (!file->get_dataelement("Rows") || file->get_dataelement("Rows") == dicom::NullElement()) {
			fail("DicomFile get_dataelement(string_view) failed");
		}
		const auto& rows_ref = (*file)["Rows"_tag];
		if (rows_ref.tag().value() != "Rows"_tag.value()) fail("DicomFile operator[] failed");
		file->remove_dataelement("Rows"_tag);
		if (file->get_dataelement("Rows"_tag) != dicom::NullElement()) {
			fail("DicomFile remove_dataelement failed");
		}

		std::size_t file_iter_count = 0;
		for (const auto& elem : *file) {
			(void)elem;
			++file_iter_count;
		}
		if (file_iter_count != file->dataset().size()) fail("DicomFile iterator mismatch");

		DataSet manual;
		manual.attach_to_file(file_path);
		if (manual.path() != file_path) fail("manual path mismatch");
		if (manual.stream().datasize() != 4) fail("manual datasize mismatch");
		const auto ds_dump = file->dataset().dump();
		if (ds_dump.find("TAG\tVR\tLEN\tVM\tOFFSET\tVALUE\tKEYWORD") == std::string::npos) {
			fail("DataSet dump header missing");
		}
		const auto ds_dump_no_offset = file->dataset().dump(80, false);
		if (ds_dump_no_offset.find("TAG\tVR\tLEN\tVM\tVALUE\tKEYWORD") == std::string::npos) {
			fail("DataSet dump(include_offset=false) header missing");
		}
		if (ds_dump_no_offset.find("OFFSET\tVALUE") != std::string::npos) {
			fail("DataSet dump(include_offset=false) should hide OFFSET column");
		}
	}

	const std::vector<std::uint8_t> buffer{0x01, 0x02, 0x03, 0x04};
	const auto mem = read_bytes("buffer", buffer.data(), buffer.size());
	if (mem->stream().datasize() != buffer.size()) fail("mem datasize mismatch");

	auto owned_buffer = std::vector<std::uint8_t>{0x0A, 0x0B};
	const auto mem_owned = read_bytes("owned-buffer", std::move(owned_buffer));
	if (mem_owned->stream().datasize() != 2) fail("mem_owned datasize mismatch");

	DataSet manual_mem;
	manual_mem.attach_to_memory("manual-buffer", buffer.data(), buffer.size());
	if (manual_mem.stream().datasize() != buffer.size()) fail("manual_mem datasize mismatch");

	std::remove(file_path.c_str());

	return 0;
}
