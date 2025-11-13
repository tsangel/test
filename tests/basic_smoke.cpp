#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <dicom.h>
#include <instream.h>

int main() {
	using dicom::lookup::keyword_to_tag_vr;
	using dicom::lookup::tag_to_keyword;
	using dicom::DataSet;
	using dicom::read_bytes;
	using dicom::read_file;
	using namespace dicom::literals;

	const auto [tag, vr] = keyword_to_tag_vr("PatientName");
	assert(tag);
	assert(tag.value() == 0x00100010u);
	assert(vr.str() == std::string_view("PN"));
	assert(tag_to_keyword(tag.value()) == std::string_view("PatientName"));

	const dicom::Tag literal_tag = "Rows"_tag;
	assert(literal_tag.value() == 0x00280010u);

	const auto tmp_dir = std::filesystem::temp_directory_path();
	const auto file_path = tmp_dir / "dicomsdl_basic_smoke.dcm";
	{
		std::ofstream os(file_path, std::ios::binary);
		os << "DICM";
	}
	{
		const auto file = read_file(file_path.string());
		assert(file);
		assert(file->path() == file_path.string());
		assert(file->stream().datasize() == 4);

		DataSet manual;
		manual.attachToFile(file_path.string());
		assert(manual.path() == file_path.string());
		assert(manual.stream().datasize() == 4);
	}

	const std::vector<std::uint8_t> buffer{0x01, 0x02, 0x03, 0x04};
	const auto mem = read_bytes("buffer", buffer.data(), buffer.size());
	assert(mem->is_memory_backed());
	assert(mem->stream().datasize() == buffer.size());

	auto owned_buffer = std::vector<std::uint8_t>{0x0A, 0x0B};
	const auto mem_owned = read_bytes("owned-buffer", std::move(owned_buffer));
	assert(mem_owned->is_memory_backed());
	assert(mem_owned->stream().datasize() == 2);

	DataSet manual_mem;
	manual_mem.attachToMemory("manual-buffer", buffer.data(), buffer.size());
	assert(manual_mem.is_memory_backed());
	assert(manual_mem.stream().datasize() == buffer.size());

	std::filesystem::remove(file_path);

	return 0;
}
