#include <algorithm>
#include <cstdint>
#include <array>
#include <fstream>
#include <string>
#include <vector>
#include <iostream>
#include <sstream>
#include <exception>
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

	if (!dicom::uid::is_valid_uid_text_strict(
	        dicom::uid::uid_prefix())) {
		fail("uid_prefix should be a strict-valid UID");
	}
	if (!dicom::uid::is_valid_uid_text_strict(
	        dicom::uid::implementation_class_uid())) {
		fail("implementation_class_uid should be a strict-valid UID");
	}
	if (dicom::uid::implementation_version_name() !=
	    std::string_view("DICOMSDL 2026FEB")) {
		fail("implementation_version_name mismatch");
	}

	const auto suffixed_uid = dicom::uid::make_uid_with_suffix(42u);
	if (!suffixed_uid) fail("make_uid_with_suffix should return a value");
	if (suffixed_uid->value() != std::string_view("1.3.6.1.4.1.56559.42")) {
		fail("make_uid_with_suffix value mismatch");
	}

	const auto generated_uid = dicom::uid::generate_uid();
	if (!dicom::uid::is_valid_uid_text_strict(generated_uid.value())) {
		fail("generate_uid should return strict-valid UID text");
	}
	if (generated_uid.value().rfind(dicom::uid::uid_prefix(), 0) != 0) {
		fail("generate_uid should use DICOMSDL UID prefix");
	}
	const auto generated_uid_nothrow = dicom::uid::try_generate_uid();
	if (!generated_uid_nothrow) {
		fail("try_generate_uid should return a value");
	}
	if (!dicom::uid::is_valid_uid_text_strict(generated_uid_nothrow->value())) {
		fail("try_generate_uid should return strict-valid UID text");
	}

	const auto sop_uid = dicom::uid::generate_sop_instance_uid();
	if (!dicom::uid::is_valid_uid_text_strict(sop_uid.value())) {
		fail("generate_sop_instance_uid should return strict-valid UID text");
	}

	const auto base_uid = dicom::uid::make_generated("1.2.840.10008");
	if (!base_uid) fail("make_generated should build base UID");
	const auto composed_uid = base_uid->append(11u).append(22u).append(33u);
	if (composed_uid.value() != std::string_view("1.2.840.10008.11.22.33")) {
		fail("Generated::append should append all components");
	}
	const auto zero_component_uid = dicom::uid::make_generated("1.2.3");
	if (!zero_component_uid) fail("make_generated should build base UID with small root");
	if (zero_component_uid->append(7u).append(0u).value() != std::string_view("1.2.3.7.0")) {
		fail("Generated::append should treat 0 as valid component");
	}

	std::string long_base{"1"};
	while (long_base.size() + 2 <= 61) {
		long_base += ".1";
	}
	const auto long_base_uid = dicom::uid::make_generated(long_base);
	if (!long_base_uid) fail("make_generated should build long base UID");
	const auto compact_uid = long_base_uid->append(1234567890123456789ULL);
	if (compact_uid.value().size() > 64) fail("Generated::append compacted UID should be <= 64 chars");
	std::string truncated_prefix = long_base.substr(0, std::min<std::size_t>(30, long_base.size()));
	if (!truncated_prefix.empty() && truncated_prefix.back() != '.') {
		truncated_prefix.push_back('.');
	}
	if (compact_uid.value().rfind(truncated_prefix, 0) != 0) {
		fail("Generated::append compacted UID should keep 30-char base prefix");
	}
	if (!dicom::uid::is_valid_uid_text_strict(compact_uid.value())) {
		fail("Generated::append compacted UID should remain strict-valid");
	}

	const auto my_study_uid = dicom::uid::generate_uid();
	const auto series_uid = my_study_uid.append(23u);
	const auto instance_uid = series_uid.append(34u);
	if (!(series_uid.value().starts_with(my_study_uid.value()) &&
	        series_uid.value().ends_with(".23"))) {
		fail("Generated::append should append series component");
	}
	if (!(instance_uid.value().starts_with(series_uid.value()) &&
	        instance_uid.value().ends_with(".34"))) {
		fail("Generated::append should append instance component");
	}
	if (!dicom::uid::is_valid_uid_text_strict(instance_uid.value())) {
		fail("Generated::append chain should remain strict-valid");
	}

	{
		dicom::DataElement signed_long_elem("Rows"_tag, dicom::VR::SL, 0, 0, nullptr);
		if (!signed_long_elem.from_long(123456789L)) {
			fail("DataElement::from_long should encode SL");
		}
		if (signed_long_elem.length() != 4) {
			fail("DataElement::from_long SL length should be 4");
		}
		if (signed_long_elem.to_long().value_or(0) != 123456789L) {
			fail("DataElement::from_long SL roundtrip mismatch");
		}
	}
	{
		dicom::DataElement unsigned_short_elem("Rows"_tag, dicom::VR::US, 0, 0, nullptr);
		if (unsigned_short_elem.from_long(-1)) {
			fail("DataElement::from_long should reject negative value for US");
		}
	}
	{
		dicom::DataElement integer_string_elem("Rows"_tag, dicom::VR::IS, 0, 0, nullptr);
		if (!integer_string_elem.from_long(42L)) {
			fail("DataElement::from_long should encode IS");
		}
		const auto text = integer_string_elem.to_string_view();
		if (!text || *text != "42") {
			fail("DataElement::from_long IS string mismatch");
		}
	}
	{
		dicom::DataElement patient_name("PatientName"_tag, dicom::VR::PN, 0, 0, nullptr);
		if (!patient_name.from_string_view("DOE^JOHN")) {
			fail("DataElement::from_string_view should encode PN");
		}
		if ((patient_name.length() & 1u) != 0u) {
			fail("DataElement::from_string_view should store even-length value");
		}
		const auto text = patient_name.to_string_view();
		if (!text || *text != "DOE^JOHN") {
			fail("DataElement::from_string_view PN roundtrip mismatch");
		}
		if (!patient_name.from_string_view("A")) {
			fail("DataElement::from_string_view should encode odd-length PN");
		}
		if (patient_name.length() != 2) {
			fail("DataElement::from_string_view should pad odd-length PN to even");
		}
	}
	{
		auto ts_uid = dicom::uid::from_keyword("ImplicitVRLittleEndian");
		if (!ts_uid) {
			fail("uid::from_keyword should resolve transfer syntax UID");
		}
		dicom::DataElement ts_elem("TransferSyntaxUID"_tag, dicom::VR::UI, 0, 0, nullptr);
		if (!ts_elem.from_uid(*ts_uid)) {
			fail("DataElement::from_uid should encode well-known UID");
		}
		if (!ts_elem.from_transfer_syntax_uid(*ts_uid)) {
			fail("DataElement::from_transfer_syntax_uid should encode transfer syntax UID");
		}
		auto roundtrip = ts_elem.to_transfer_syntax_uid();
		if (!roundtrip || roundtrip->value() != ts_uid->value()) {
			fail("DataElement::from_transfer_syntax_uid roundtrip mismatch");
		}
		auto generated = dicom::uid::generate_uid();
		if (!ts_elem.from_uid(generated)) {
			fail("DataElement::from_uid(Generated) should encode generated UID");
		}
		auto generated_roundtrip = ts_elem.to_uid_string();
		if (!generated_roundtrip || *generated_roundtrip != std::string(generated.value())) {
			fail("DataElement::from_uid(Generated) roundtrip mismatch");
		}
	}
	{
		auto sop_uid = dicom::uid::from_keyword("SecondaryCaptureImageStorage");
		if (!sop_uid) {
			fail("uid::from_keyword should resolve SOP class UID");
		}
		dicom::DataElement sop_elem("SOPClassUID"_tag, dicom::VR::UI, 0, 0, nullptr);
		if (!sop_elem.from_sop_class_uid(*sop_uid)) {
			fail("DataElement::from_sop_class_uid should encode SOP class UID");
		}
		auto roundtrip = sop_elem.to_sop_class_uid();
		if (!roundtrip || roundtrip->value() != sop_uid->value()) {
			fail("DataElement::from_sop_class_uid roundtrip mismatch");
		}
		auto ts_uid = dicom::uid::from_keyword("ImplicitVRLittleEndian");
		if (!ts_uid) {
			fail("uid::from_keyword should resolve transfer syntax UID");
		}
		if (sop_elem.from_transfer_syntax_uid(*sop_uid)) {
			fail("DataElement::from_transfer_syntax_uid should reject non-transfer-syntax UID");
		}
		if (sop_elem.from_sop_class_uid(*ts_uid)) {
			fail("DataElement::from_sop_class_uid should reject transfer syntax UID");
		}
	}
	{
		dicom::DataElement uid_elem("SOPInstanceUID"_tag, dicom::VR::UI, 0, 0, nullptr);
		if (!uid_elem.from_uid_string("1.2.3")) {
			fail("DataElement::from_uid_string should accept valid UID text");
		}
		if (uid_elem.length() != 6) {
			fail("DataElement::from_uid_string should pad odd length to even");
		}
		auto uid_text = uid_elem.to_uid_string();
		if (!uid_text || *uid_text != "1.2.3") {
			fail("DataElement::from_uid_string roundtrip mismatch");
		}
		if (uid_elem.from_uid_string("1..2")) {
			fail("DataElement::from_uid_string should reject invalid UID text");
		}
	}
	{
		dicom::DataElement missing_elem;
		if (missing_elem.is_present()) {
			fail("default DataElement should be missing");
		}
		if (!missing_elem.is_missing()) {
			fail("default DataElement should report is_missing");
		}
		if (static_cast<bool>(missing_elem)) {
			fail("default DataElement bool() should be false");
		}
		dicom::DataElement present_elem("Rows"_tag, dicom::VR::US, 0, 0, nullptr);
		if (!present_elem.is_present()) {
			fail("non-None VR DataElement should be present");
		}
		if (present_elem.is_missing()) {
			fail("non-None VR DataElement should not be missing");
		}
		if (!static_cast<bool>(present_elem)) {
			fail("present DataElement bool() should be true");
		}
	}

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
		if (file->has_error()) fail("read_file should not record errors for valid input");
		if (!file->error_message().empty()) fail("error_message should be empty when no read error exists");
		if (file->path() != file_path) fail("file path mismatch");
		if (file->stream().attached_size() != 4) fail("file data_size mismatch");
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
		if (!rows || !(*rows)) fail("DicomFile add_dataelement failed");
		const auto file_dump_after_add = file->dump();
		if (file_dump_after_add.find("'00280010'") == std::string::npos) {
			fail("DicomFile dump should include Rows");
		}
		const std::size_t long_desc_size = 256;
		std::vector<std::uint8_t> long_desc_value(long_desc_size, static_cast<std::uint8_t>('A'));
		auto* study_desc =
		    file->add_dataelement("StudyDescription"_tag, dicom::VR::LO, 0, long_desc_size);
		if (!study_desc || !(*study_desc)) fail("DicomFile add StudyDescription failed");
		study_desc->set_value_bytes(long_desc_value);
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
		const auto* rows_by_tag = file->get_dataelement("Rows"_tag);
		if (rows_by_tag->is_missing()) {
			fail("DicomFile get_dataelement(Tag) failed");
		}
		const auto* rows_by_keyword = file->get_dataelement("Rows");
		if (rows_by_keyword->is_missing()) {
			fail("DicomFile get_dataelement(string_view) failed");
		}
		const auto& rows_ref = (*file)["Rows"_tag];
		if (rows_ref.tag().value() != "Rows"_tag.value()) fail("DicomFile operator[] failed");
		file->remove_dataelement("Rows"_tag);
		if (file->get_dataelement("Rows"_tag)->is_present()) {
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
		if (manual.stream().attached_size() != 4) fail("manual data_size mismatch");
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
	if (mem->stream().attached_size() != buffer.size()) fail("mem data_size mismatch");

	auto owned_buffer = std::vector<std::uint8_t>{0x0A, 0x0B};
	const auto mem_owned = read_bytes("owned-buffer", std::move(owned_buffer));
	if (mem_owned->stream().attached_size() != 2) fail("mem_owned data_size mismatch");

	const auto malformed = [] {
		std::vector<std::uint8_t> bytes(128, 0);
		bytes.insert(bytes.end(), {'D', 'I', 'C', 'M'});
		bytes.insert(bytes.end(), {0x10, 0x00, 0x10, 0x00, 'P', 'N', 0x08, 0x00});
		bytes.insert(bytes.end(), {'A', 'B'});
		return bytes;
	}();

	bool malformed_threw = false;
	try {
		[[maybe_unused]] auto should_throw =
		    read_bytes("malformed-default", malformed.data(), malformed.size());
	} catch (const std::exception&) {
		malformed_threw = true;
	}
	if (!malformed_threw) fail("malformed input should throw when keep_on_error is false");

	dicom::ReadOptions keep_opts;
	keep_opts.keep_on_error = true;
	const auto malformed_keep =
	    read_bytes("malformed-keep", malformed.data(), malformed.size(), keep_opts);
	if (!malformed_keep) fail("malformed keep_on_error read returned null");
	if (!malformed_keep->has_error()) fail("keep_on_error read should record has_error=true");
	if (malformed_keep->error_message().empty()) fail("keep_on_error read should keep error_message");
	if (malformed_keep->size() == 0) fail("keep_on_error read should preserve partially parsed elements");

	DataSet manual_mem;
	manual_mem.attach_to_memory("manual-buffer", buffer.data(), buffer.size());
	if (manual_mem.stream().attached_size() != buffer.size()) fail("manual_mem data_size mismatch");

	dicom::DicomFile generated;
	auto add_text_element = [&](dicom::Tag tag, dicom::VR vr, std::string_view value) {
		auto* element = generated.add_dataelement(tag, vr, 0, value.size());
		if (!element) {
			fail("failed to add generated text element");
		}
		element->set_value_bytes(std::span<const std::uint8_t>(
		    reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
		return element;
	};

	add_text_element("SOPClassUID"_tag, dicom::VR::UI, "1.2.840.10008.5.1.4.1.1.7");
	add_text_element("SOPInstanceUID"_tag, dicom::VR::UI, dicom::uid::generate_sop_instance_uid().value());
	add_text_element("PatientName"_tag, dicom::VR::PN, "WRITE^ROUNDTRIP");

	auto* sequence_element = generated.add_dataelement("ReferencedStudySequence"_tag, dicom::VR::SQ);
	if (!sequence_element || !(*sequence_element)) fail("failed to add sequence element");
	auto* sequence = sequence_element->as_sequence();
	if (!sequence) fail("sequence pointer is null");
	auto* sequence_item = sequence->add_dataset();
	if (!sequence_item) fail("failed to append sequence item");
	{
		auto* referenced_uid = sequence_item->add_dataelement("ReferencedSOPInstanceUID"_tag, dicom::VR::UI, 0, 12);
		if (!referenced_uid) fail("failed to add sequence item UID");
		const std::array<std::uint8_t, 12> uid_value{
		    '1', '.', '2', '.', '3', '.', '4', '.', '5', '.', '6', '\0'};
		referenced_uid->set_value_bytes(uid_value);
	}

	auto* pixel_element = generated.add_dataelement("PixelData"_tag, dicom::VR::PX);
	if (!pixel_element || !(*pixel_element)) fail("failed to add pixel element");
	auto* pixel_sequence = pixel_element->as_pixel_sequence();
	if (!pixel_sequence) fail("pixel sequence pointer is null");
	auto* frame = pixel_sequence->add_frame();
	if (!frame) fail("failed to add pixel frame");
	frame->set_encoded_data({0x01, 0x02, 0x03, 0x04});

	dicom::WriteOptions write_opts;
	const auto generated_bytes = generated.write_bytes(write_opts);
	if (generated_bytes.size() < 132) fail("write_bytes should include preamble + DICM");

	std::ostringstream os(std::ios::binary);
	generated.write_to_stream(os, write_opts);
	const auto streamed = os.str();
	if (streamed.size() != generated_bytes.size()) fail("write_to_stream size mismatch");

	auto generated_roundtrip = read_bytes("generated-roundtrip", generated_bytes.data(), generated_bytes.size());
	if (!generated_roundtrip) fail("generated read_bytes returned null");
	const auto* seq_roundtrip = generated_roundtrip->get_dataelement("ReferencedStudySequence"_tag);
	if (seq_roundtrip->is_missing()) fail("roundtrip missing sequence");
	const auto* seq_value = seq_roundtrip->as_sequence();
	if (!seq_value || seq_value->size() != 1) fail("roundtrip sequence item count mismatch");
	auto* pix_roundtrip = generated_roundtrip->get_dataelement("PixelData"_tag);
	if (pix_roundtrip->is_missing()) fail("roundtrip missing pixel data");
	if (!pix_roundtrip->vr().is_pixel_sequence()) fail("roundtrip pixel data should be pixel sequence");
	auto* pix_value = pix_roundtrip->as_pixel_sequence();
	if (!pix_value || pix_value->number_of_frames() != 1) fail("roundtrip pixel frame count mismatch");
	const auto encoded_span = pix_value->frame_encoded_span(0);
	if (encoded_span.size() != 4 ||
	    encoded_span[0] != 0x01 || encoded_span[1] != 0x02 ||
	    encoded_span[2] != 0x03 || encoded_span[3] != 0x04) {
		fail("roundtrip pixel payload mismatch");
	}

	const std::string roundtrip_path = tmp_dir + "dicomsdl_basic_smoke_roundtrip.dcm";
	generated.write_file(roundtrip_path, write_opts);
	const auto generated_roundtrip_file = read_file(roundtrip_path);
	if (!generated_roundtrip_file) fail("write_file roundtrip read returned null");
	if (generated_roundtrip_file->get_dataelement("PixelData"_tag)->is_missing()) {
		fail("write_file roundtrip missing pixel data");
	}
	std::remove(roundtrip_path.c_str());

	std::remove(file_path.c_str());

	return 0;
}
