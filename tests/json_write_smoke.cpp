#include <algorithm>
#include <map>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <dicom.h>

using namespace dicom::literals;

namespace {

[[noreturn]] void fail(const std::string& msg) {
	std::cerr << msg << std::endl;
	std::exit(1);
}

void expect_true(bool condition, const std::string& msg) {
	if (!condition) {
		fail(msg);
	}
}

void expect_contains(std::string_view haystack, std::string_view needle, const std::string& msg) {
	if (haystack.find(needle) == std::string_view::npos) {
		fail(msg + " missing=" + std::string(needle));
	}
}

void expect_not_contains(
    std::string_view haystack, std::string_view needle, const std::string& msg) {
	if (haystack.find(needle) != std::string_view::npos) {
		fail(msg + " unexpected=" + std::string(needle));
	}
}

void populate_json_fixture_file(dicom::DicomFile& file) {
	auto& patient_name = file.add_dataelement("PatientName"_tag, dicom::VR::PN);
	if (!patient_name.from_string_view("Doe^Jane")) {
		fail("fixture PatientName assignment failed");
	}

	auto& study_uid = file.add_dataelement("StudyInstanceUID"_tag, dicom::VR::UI);
	if (!study_uid.from_uid_string("1.2.826.0.1.3680043.10.543.1")) {
		fail("fixture StudyInstanceUID assignment failed");
	}

	auto& series_uid = file.add_dataelement("SeriesInstanceUID"_tag, dicom::VR::UI);
	if (!series_uid.from_uid_string("1.2.826.0.1.3680043.10.543.2")) {
		fail("fixture SeriesInstanceUID assignment failed");
	}

	auto& instance_uid = file.add_dataelement("SOPInstanceUID"_tag, dicom::VR::UI);
	if (!instance_uid.from_uid_string("1.2.826.0.1.3680043.10.543.3")) {
		fail("fixture SOPInstanceUID assignment failed");
	}

	auto& pixel_data = file.add_dataelement("PixelData"_tag, dicom::VR::OB);
	pixel_data.set_value_bytes(
	    std::vector<std::uint8_t>{0x01u, 0x02u, 0x03u, 0x04u});
}

void test_default_excludes_group_0002() {
	auto file = dicom::read_file("tests/test_le.dcm");
	expect_true(static_cast<bool>(file), "test_le.dcm should read");
	expect_true(
	    file->get_dataelement(dicom::Tag(0x0002u, 0x0010u)).is_present(),
	    "test_le.dcm should contain TransferSyntaxUID in file meta");

	const auto default_result = file->write_json();
	expect_not_contains(
	    default_result.json, "\"00020010\"",
	    "write_json default should exclude file meta group 0002");
	expect_not_contains(
	    default_result.json, "\"00020000\"",
	    "write_json should always exclude group length tags");

	dicom::JsonWriteOptions include_meta_options;
	include_meta_options.include_group_0002 = true;
	const auto include_meta_result = file->write_json(include_meta_options);
	expect_contains(
	    include_meta_result.json, "\"00020010\"",
	    "write_json(include_group_0002=true) should include file meta");
	expect_not_contains(
	    include_meta_result.json, "\"00020000\"",
	    "write_json(include_group_0002=true) should still exclude group length tags");
}

void test_inline_binary_and_person_name() {
	dicom::DicomFile file;
	populate_json_fixture_file(file);
	const auto result = file.write_json();

	expect_true(result.bulk_parts.empty(), "default write_json should not emit bulk parts");
	expect_contains(
	    result.json, "\"00100010\":{\"vr\":\"PN\",\"Value\":[{\"Alphabetic\":\"Doe^Jane\"}]}",
	    "PN should serialize as DICOM JSON person-name object");
	expect_contains(
	    result.json, "\"7FE00010\":{\"vr\":\"OB\",\"InlineBinary\":\"AQIDBA==\"}",
	    "binary value should serialize as InlineBinary by default");
}

void test_bulk_uri_mode_collects_parts() {
	dicom::DicomFile file;
	populate_json_fixture_file(file);
	dicom::JsonWriteOptions options;
	options.bulk_data_mode = dicom::JsonBulkDataMode::uri;
	options.bulk_data_threshold = 4u;
	options.bulk_data_uri_template =
	    "/dicomweb/studies/{study}/series/{series}/instances/{instance}/bulk/{tag}";

	const auto result = file.write_json(options);
	const std::string expected_uri =
	    "/dicomweb/studies/1.2.826.0.1.3680043.10.543.1/"
	    "series/1.2.826.0.1.3680043.10.543.2/"
	    "instances/1.2.826.0.1.3680043.10.543.3/bulk/7FE00010";

	expect_contains(
	    result.json,
	    "\"7FE00010\":{\"vr\":\"OB\",\"BulkDataURI\":\"" + expected_uri + "\"}",
	    "uri mode should emit BulkDataURI once the threshold is met");
	expect_not_contains(
	    result.json, "\"InlineBinary\"",
	    "uri mode should replace InlineBinary with BulkDataURI for threshold-matching values");
	expect_true(result.bulk_parts.size() == 1u, "uri mode should collect one bulk part");
	expect_true(result.bulk_parts[0].uri == expected_uri, "bulk part URI should match JSON");
	expect_true(
	    result.bulk_parts[0].media_type == "application/octet-stream",
	    "generic bulk parts should default to application/octet-stream");
	expect_true(
	    result.bulk_parts[0].transfer_syntax_uid == "ExplicitVRLittleEndian"_uid.value(),
	    "PixelData bulk parts should expose the dataset transfer syntax when known");
	const auto source_pixel_bytes = file.get_dataelement("PixelData"_tag).value_span();
	expect_true(
	    result.bulk_parts[0].bytes().size() == source_pixel_bytes.size() &&
	        std::equal(result.bulk_parts[0].bytes().begin(), result.bulk_parts[0].bytes().end(),
	            source_pixel_bytes.begin(), source_pixel_bytes.end()),
	    "bulk part bytes should preserve the original value field");
	expect_true(
	    result.bulk_parts[0].bytes().data() == source_pixel_bytes.data(),
	    "raw bulk parts should borrow the source value field without copying");
}

void test_omit_mode_keeps_vr_only() {
	dicom::DicomFile file;
	populate_json_fixture_file(file);
	dicom::JsonWriteOptions options;
	options.bulk_data_mode = dicom::JsonBulkDataMode::omit;

	const auto result = file.write_json(options);
	expect_true(result.bulk_parts.empty(), "omit mode should not collect bulk parts");
	expect_contains(
	    result.json, "\"7FE00010\":{\"vr\":\"OB\"}",
	    "omit mode should keep the attribute and VR");
	expect_not_contains(
	    result.json, "\"InlineBinary\"",
	    "omit mode should not serialize InlineBinary");
	expect_not_contains(
	    result.json, "\"BulkDataURI\"",
	    "omit mode should not serialize BulkDataURI");
}

void test_multiframe_compressed_bulk_uses_frame_uris() {
	dicom::DicomFile file;
	auto& study_uid = file.add_dataelement("StudyInstanceUID"_tag, dicom::VR::UI);
	if (!study_uid.from_uid_string("1.2.826.0.1.3680043.10.543.11")) {
		fail("multiframe StudyInstanceUID assignment failed");
	}
	auto& series_uid = file.add_dataelement("SeriesInstanceUID"_tag, dicom::VR::UI);
	if (!series_uid.from_uid_string("1.2.826.0.1.3680043.10.543.12")) {
		fail("multiframe SeriesInstanceUID assignment failed");
	}
	auto& instance_uid = file.add_dataelement("SOPInstanceUID"_tag, dicom::VR::UI);
	if (!instance_uid.from_uid_string("1.2.826.0.1.3680043.10.543.13")) {
		fail("multiframe SOPInstanceUID assignment failed");
	}
	auto& transfer_syntax =
	    file.add_dataelement(dicom::Tag(0x0002u, 0x0010u), dicom::VR::UI);
	if (!transfer_syntax.from_transfer_syntax_uid("JPEGLSLossless"_uid)) {
		fail("multiframe TransferSyntaxUID assignment failed");
	}

	file.reset_encapsulated_pixel_data(2);
	file.set_encoded_pixel_frame(0, std::vector<std::uint8_t>{0x11u, 0x22u, 0x33u});
	file.set_encoded_pixel_frame(1, std::vector<std::uint8_t>{0x44u, 0x55u});
	auto& private_bulk = file.add_dataelement(dicom::Tag(0x0009u, 0x1001u), dicom::VR::OB);
	private_bulk.set_value_bytes(std::vector<std::uint8_t>{
	    0x90u, 0x91u, 0x92u, 0x93u, 0x94u, 0x95u, 0x96u, 0x97u, 0x98u, 0x99u});

	dicom::JsonWriteOptions options;
	options.bulk_data_mode = dicom::JsonBulkDataMode::uri;
	options.bulk_data_threshold = 8u;
	options.bulk_data_uri_template =
	    "/dicomweb/studies/{study}/series/{series}/instances/{instance}/bulk/{tag}";
	options.pixel_data_uri_template =
	    "/dicomweb/studies/{study}/series/{series}/instances/{instance}/frames";

	const auto result = file.write_json(options);
	const std::string base_uri =
	    "/dicomweb/studies/1.2.826.0.1.3680043.10.543.11/"
	    "series/1.2.826.0.1.3680043.10.543.12/"
	    "instances/1.2.826.0.1.3680043.10.543.13/frames";

	expect_contains(
	    result.json,
	    "\"7FE00010\":{\"vr\":\"OB\",\"BulkDataURI\":\"" + base_uri + "\"}",
	    "compressed multiframe PixelData should expose a directly dereferenceable bulk URI in JSON");
	expect_contains(
	    result.json,
	    "\"00091001\":{\"vr\":\"OB\",\"BulkDataURI\":\""
	        "/dicomweb/studies/1.2.826.0.1.3680043.10.543.11/"
	        "series/1.2.826.0.1.3680043.10.543.12/"
	        "instances/1.2.826.0.1.3680043.10.543.13/bulk/00091001\"}",
	    "non-PixelData bulk attributes should continue using the generic bulk URI template");
	expect_true(
	    result.bulk_parts.size() == 3u,
	    "compressed multiframe PixelData plus one extra bulk attribute should emit three bulk parts");
	std::map<std::string, std::span<const std::uint8_t>> bulk_parts_by_uri;
	for (const auto& part : result.bulk_parts) {
		bulk_parts_by_uri.emplace(part.uri, part.bytes());
	}
	expect_true(
	    bulk_parts_by_uri.size() == 3u,
	    "compressed multiframe bulk parts should expose unique URIs");
	expect_true(
	    bulk_parts_by_uri.find(base_uri + "/1") != bulk_parts_by_uri.end() &&
	        bulk_parts_by_uri.find(base_uri + "/2") != bulk_parts_by_uri.end() &&
	        bulk_parts_by_uri.find(
	            "/dicomweb/studies/1.2.826.0.1.3680043.10.543.11/"
	            "series/1.2.826.0.1.3680043.10.543.12/"
	            "instances/1.2.826.0.1.3680043.10.543.13/bulk/00091001") !=
	            bulk_parts_by_uri.end(),
	    "compressed multiframe bulk parts should use 1-based frame URIs under the bulk URI");
	const auto frame0 = file.encoded_pixel_frame_view(0);
	const auto frame1 = file.encoded_pixel_frame_view(1);
	expect_true(
	    bulk_parts_by_uri[base_uri + "/1"].data() == frame0.data() &&
	        bulk_parts_by_uri[base_uri + "/1"].size() == frame0.size(),
	    "frame 1 bulk part should borrow encoded frame 1 bytes");
	expect_true(
	    bulk_parts_by_uri[base_uri + "/2"].data() == frame1.data() &&
	        bulk_parts_by_uri[base_uri + "/2"].size() == frame1.size(),
	    "frame 2 bulk part should borrow encoded frame 2 bytes");
	for (const auto& part : result.bulk_parts) {
		if (part.uri == base_uri + "/1" || part.uri == base_uri + "/2") {
			expect_true(
			    part.media_type == "image/jls",
			    "compressed PixelData frame parts should carry the JPEG-LS media type");
			expect_true(
			    part.transfer_syntax_uid == "JPEGLSLossless"_uid.value(),
			    "compressed PixelData frame parts should expose the transfer syntax UID");
		}
		if (part.uri.find("/bulk/00091001") != std::string::npos) {
			expect_true(
			    part.media_type == "application/octet-stream",
			    "non-PixelData bulk parts should keep application/octet-stream");
			expect_true(
			    part.transfer_syntax_uid.empty(),
			    "non-PixelData bulk parts should not expose a transfer syntax UID");
		}
	}
	const auto private_bulk_bytes = private_bulk.value_span();
	expect_true(
	    bulk_parts_by_uri
	            ["/dicomweb/studies/1.2.826.0.1.3680043.10.543.11/"
	             "series/1.2.826.0.1.3680043.10.543.12/"
	             "instances/1.2.826.0.1.3680043.10.543.13/bulk/00091001"]
	                .data() == private_bulk_bytes.data() &&
	        bulk_parts_by_uri
	                ["/dicomweb/studies/1.2.826.0.1.3680043.10.543.11/"
	                 "series/1.2.826.0.1.3680043.10.543.12/"
	                 "instances/1.2.826.0.1.3680043.10.543.13/bulk/00091001"]
	                    .size() == private_bulk_bytes.size(),
	    "non-PixelData bulk part should continue borrowing the generic value span");
}

void test_multiframe_native_bulk_uses_frame_uris() {
	dicom::DicomFile file;
	auto& study_uid = file.add_dataelement("StudyInstanceUID"_tag, dicom::VR::UI);
	if (!study_uid.from_uid_string("1.2.826.0.1.3680043.10.543.31")) {
		fail("native multiframe StudyInstanceUID assignment failed");
	}
	auto& series_uid = file.add_dataelement("SeriesInstanceUID"_tag, dicom::VR::UI);
	if (!series_uid.from_uid_string("1.2.826.0.1.3680043.10.543.32")) {
		fail("native multiframe SeriesInstanceUID assignment failed");
	}
	auto& instance_uid = file.add_dataelement("SOPInstanceUID"_tag, dicom::VR::UI);
	if (!instance_uid.from_uid_string("1.2.826.0.1.3680043.10.543.33")) {
		fail("native multiframe SOPInstanceUID assignment failed");
	}
	expect_true(file.set_value("Rows"_tag, 2L), "Rows assignment failed");
	expect_true(file.set_value("Columns"_tag, 1L), "Columns assignment failed");
	expect_true(file.set_value("SamplesPerPixel"_tag, 1L), "SamplesPerPixel assignment failed");
	expect_true(file.set_value("BitsAllocated"_tag, 16L), "BitsAllocated assignment failed");
	expect_true(file.set_value("BitsStored"_tag, 16L), "BitsStored assignment failed");
	expect_true(file.set_value("HighBit"_tag, 15L), "HighBit assignment failed");
	expect_true(
	    file.set_value("PixelRepresentation"_tag, 0L), "PixelRepresentation assignment failed");
	expect_true(
	    file.set_value("NumberOfFrames"_tag, 2L), "NumberOfFrames assignment failed");
	auto& photometric =
	    file.add_dataelement("PhotometricInterpretation"_tag, dicom::VR::CS);
	expect_true(
	    photometric.from_string_view("MONOCHROME2"),
	    "PhotometricInterpretation assignment failed");
	auto& transfer_syntax =
	    file.add_dataelement(dicom::Tag(0x0002u, 0x0010u), dicom::VR::UI);
	if (!transfer_syntax.from_transfer_syntax_uid("ExplicitVRLittleEndian"_uid)) {
		fail("native multiframe TransferSyntaxUID assignment failed");
	}

	auto& pixel_data = file.add_dataelement("PixelData"_tag, dicom::VR::OW);
	pixel_data.set_value_bytes(
	    std::vector<std::uint8_t>{0x01u, 0x00u, 0x02u, 0x00u, 0x03u, 0x00u, 0x04u, 0x00u});

	dicom::JsonWriteOptions options;
	options.bulk_data_mode = dicom::JsonBulkDataMode::uri;
	options.bulk_data_threshold = 4u;
	options.pixel_data_uri_template =
	    "/dicomweb/studies/{study}/series/{series}/instances/{instance}/frames";

	const auto result = file.write_json(options);
	const std::string base_uri =
	    "/dicomweb/studies/1.2.826.0.1.3680043.10.543.31/"
	    "series/1.2.826.0.1.3680043.10.543.32/"
	    "instances/1.2.826.0.1.3680043.10.543.33/frames";
	expect_contains(
	    result.json,
	    "\"7FE00010\":{\"vr\":\"OW\",\"BulkDataURI\":\"" + base_uri + "\"}",
	    "native multiframe PixelData should expose a frame base URI in JSON");
	expect_true(
	    result.bulk_parts.size() == 1u,
	    "native multiframe PixelData should emit one aggregate bulk part");
	std::map<std::string, std::span<const std::uint8_t>> bulk_parts_by_uri;
	for (const auto& part : result.bulk_parts) {
		bulk_parts_by_uri.emplace(part.uri, part.bytes());
	}
	expect_true(
	    bulk_parts_by_uri.find(base_uri) != bulk_parts_by_uri.end(),
	    "native multiframe bulk parts should reuse the JSON BulkDataURI");
	const auto pixel_bytes = pixel_data.value_span();
	expect_true(
	    bulk_parts_by_uri[base_uri].data() == pixel_bytes.data() &&
	        bulk_parts_by_uri[base_uri].size() == pixel_bytes.size(),
	    "native multiframe bulk part should borrow the full PixelData value");
	expect_true(
	    result.bulk_parts[0].media_type == "application/octet-stream",
	    "native PixelData bulk should expose application/octet-stream");
	expect_true(
	    result.bulk_parts[0].transfer_syntax_uid == "ExplicitVRLittleEndian"_uid.value(),
	    "native PixelData bulk should expose its transfer syntax UID when known");
}

void test_nested_bulk_uses_dotted_tag_paths_for_tag_placeholder() {
	dicom::DicomFile file;
	auto& study_uid = file.add_dataelement("StudyInstanceUID"_tag, dicom::VR::UI);
	if (!study_uid.from_uid_string("1.2.826.0.1.3680043.10.543.41")) {
		fail("nested bulk StudyInstanceUID assignment failed");
	}
	auto& series_uid = file.add_dataelement("SeriesInstanceUID"_tag, dicom::VR::UI);
	if (!series_uid.from_uid_string("1.2.826.0.1.3680043.10.543.42")) {
		fail("nested bulk SeriesInstanceUID assignment failed");
	}
	auto& instance_uid = file.add_dataelement("SOPInstanceUID"_tag, dicom::VR::UI);
	if (!instance_uid.from_uid_string("1.2.826.0.1.3680043.10.543.43")) {
		fail("nested bulk SOPInstanceUID assignment failed");
	}

	auto& seq_element = file.add_dataelement(dicom::Tag(0x2200u, 0x2200u), dicom::VR::SQ);
	auto* sequence = seq_element.as_sequence();
	if (!sequence) {
		fail("nested bulk sequence assignment failed");
	}
	auto* item0 = sequence->add_dataset();
	auto* item1 = sequence->add_dataset();
	if (!item0 || !item1) {
		fail("nested bulk item allocation failed");
	}
	item0->add_dataelement(dicom::Tag(0x1234u, 0x0012u), dicom::VR::OB)
	    .set_value_bytes(std::vector<std::uint8_t>{0x01u, 0x02u, 0x03u, 0x04u});
	item1->add_dataelement(dicom::Tag(0x1234u, 0x0012u), dicom::VR::OB)
	    .set_value_bytes(std::vector<std::uint8_t>{0x05u, 0x06u, 0x07u, 0x08u});

	dicom::JsonWriteOptions options;
	options.bulk_data_mode = dicom::JsonBulkDataMode::uri;
	options.bulk_data_threshold = 4u;
	options.bulk_data_uri_template =
	    "/dicomweb/studies/{study}/series/{series}/instances/{instance}/bulk/{tag}";

	const auto result = file.write_json(options);
	expect_contains(
	    result.json,
	    "\"12340012\":{\"vr\":\"OB\",\"BulkDataURI\":\""
	    "/dicomweb/studies/1.2.826.0.1.3680043.10.543.41/"
	    "series/1.2.826.0.1.3680043.10.543.42/"
	    "instances/1.2.826.0.1.3680043.10.543.43/bulk/22002200.0.12340012\"}",
	    "first nested bulk element should expand {tag} to a dotted tag path");
	expect_contains(
	    result.json,
	    "\"12340012\":{\"vr\":\"OB\",\"BulkDataURI\":\""
	    "/dicomweb/studies/1.2.826.0.1.3680043.10.543.41/"
	    "series/1.2.826.0.1.3680043.10.543.42/"
	    "instances/1.2.826.0.1.3680043.10.543.43/bulk/22002200.1.12340012\"}",
	    "second nested bulk element should get a distinct dotted tag path");
	expect_true(
	    result.bulk_parts.size() == 2u,
	    "nested duplicate tags in different items should produce distinct bulk URIs");
}

void test_missing_template_placeholder_throws() {
	dicom::DataSet dataset;
	auto& pixel_data = dataset.add_dataelement("PixelData"_tag, dicom::VR::OB);
	pixel_data.set_value_bytes(std::vector<std::uint8_t>{0x01u, 0x02u, 0x03u, 0x04u});

	dicom::JsonWriteOptions options;
	options.bulk_data_mode = dicom::JsonBulkDataMode::uri;
	options.bulk_data_threshold = 4u;
	options.bulk_data_uri_template = "/dicomweb/studies/{study}/bulk/{tag}";

	bool threw = false;
	try {
		(void)dataset.write_json(options);
	} catch (const std::exception&) {
		threw = true;
	}
	expect_true(
	    threw,
	    "write_json should fail when the URI template references a missing placeholder value");
}

void test_frame_template_without_generic_bulk_template_throws_helpful_error() {
	dicom::DicomFile file;
	auto& study_uid = file.add_dataelement("StudyInstanceUID"_tag, dicom::VR::UI);
	if (!study_uid.from_uid_string("1.2.826.0.1.3680043.10.543.21")) {
		fail("frame-template StudyInstanceUID assignment failed");
	}
	auto& series_uid = file.add_dataelement("SeriesInstanceUID"_tag, dicom::VR::UI);
	if (!series_uid.from_uid_string("1.2.826.0.1.3680043.10.543.22")) {
		fail("frame-template SeriesInstanceUID assignment failed");
	}
	auto& instance_uid = file.add_dataelement("SOPInstanceUID"_tag, dicom::VR::UI);
	if (!instance_uid.from_uid_string("1.2.826.0.1.3680043.10.543.23")) {
		fail("frame-template SOPInstanceUID assignment failed");
	}

	file.reset_encapsulated_pixel_data(2);
	file.set_encoded_pixel_frame(0, std::vector<std::uint8_t>{0x11u, 0x22u, 0x33u});
	file.set_encoded_pixel_frame(1, std::vector<std::uint8_t>{0x44u, 0x55u});
	auto& private_bulk = file.add_dataelement(dicom::Tag(0x0009u, 0x1001u), dicom::VR::OB);
	private_bulk.set_value_bytes(std::vector<std::uint8_t>{
	    0x90u, 0x91u, 0x92u, 0x93u, 0x94u, 0x95u, 0x96u, 0x97u, 0x98u, 0x99u});

	dicom::JsonWriteOptions options;
	options.bulk_data_mode = dicom::JsonBulkDataMode::uri;
	options.bulk_data_threshold = 8u;
	options.bulk_data_uri_template =
	    "/dicomweb/studies/{study}/series/{series}/instances/{instance}/frames";

	bool threw = false;
	try {
		(void)file.write_json(options);
	} catch (const std::exception& ex) {
		threw = true;
		const std::string message = ex.what();
		expect_contains(
		    message, "duplicate BulkDataURI generated",
		    "frame-style generic template should explain the duplicate URI failure");
		expect_contains(
		    message, "pixel_data_uri_template",
		    "duplicate URI failure should mention pixel_data_uri_template");
		expect_contains(
		    message, "{tag}",
		    "duplicate URI failure should recommend keeping {tag} in the generic bulk path");
	}
	expect_true(
	    threw,
	    "frame-style generic template should fail when a dataset also contains non-PixelData bulk");
}

}  // namespace

int main() {
	test_default_excludes_group_0002();
	test_inline_binary_and_person_name();
	test_bulk_uri_mode_collects_parts();
	test_omit_mode_keeps_vr_only();
	test_multiframe_compressed_bulk_uses_frame_uris();
	test_multiframe_native_bulk_uses_frame_uris();
	test_nested_bulk_uses_dotted_tag_paths_for_tag_placeholder();
	test_missing_template_placeholder_throws();
	test_frame_template_without_generic_bulk_template_throws_helpful_error();
	return 0;
}
