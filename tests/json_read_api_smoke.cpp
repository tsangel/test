#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include <dicom.h>

namespace {

[[noreturn]] void fail(const std::string& message) {
	std::cerr << message << std::endl;
	std::exit(1);
}

void expect_true(bool condition, const std::string& message) {
	if (!condition) {
		fail(message);
	}
}

void test_read_json_empty_object_returns_empty_file() {
	const std::string json = "{}";
	auto result = dicom::read_json(
	    reinterpret_cast<const std::uint8_t*>(json.data()), json.size());
	expect_true(result.items.size() == 1u, "read_json object should return one dataset item");
	expect_true(static_cast<bool>(result.items[0].file), "read_json should return a DicomFile");
	expect_true(
	    result.items[0].pending_bulk_data.empty(),
	    "read_json empty object should not report pending bulk data");
	expect_true(
	    result.items[0].file->size() == 0u,
	    "read_json empty object should create an empty dataset");
}

void test_read_json_empty_array_returns_no_items() {
	const std::string json = "[]";
	auto result = dicom::read_json(
	    reinterpret_cast<const std::uint8_t*>(json.data()), json.size());
	expect_true(
	    result.items.empty(),
	    "read_json empty array should return no dataset items");
}

void test_read_json_parses_values_sequences_and_bulk_refs() {
	const std::string json =
	    R"({"00080016":{"Value":["1.2.840.10008.5.1.4.1.1.2"]},"00100010":{"vr":"PN","Value":[{"Alphabetic":"Doe^Jane"}]},"00280008":{"vr":"IS","Value":[2]},"00081140":{"vr":"SQ","Value":[{"00081150":{"vr":"UI","Value":["1.2.3"]},"00081155":{"vr":"UI","Value":["4.5.6"]}}]},"7FE00010":{"vr":"OW","BulkDataURI":"instances/1/frames"}})";
	auto result = dicom::read_json(
	    reinterpret_cast<const std::uint8_t*>(json.data()), json.size());
	expect_true(result.items.size() == 1u, "object JSON should produce one result item");
	auto& file = *result.items[0].file;
	expect_true(
	    file["SOPClassUID"].to_string_view().value_or("") == "1.2.840.10008.5.1.4.1.1.2",
	    "UI values should be parsed even when vr is omitted");
	expect_true(
	    file["PatientName"].to_utf8_string().value_or("") == "Doe^Jane",
	    "PN values should round-trip as UTF-8 DICOM PN text");
	expect_true(
	    file["ReferencedImageSequence.0.ReferencedSOPClassUID"].to_string_view().value_or("") ==
	        "1.2.3",
	    "nested SQ items should be parsed");
	expect_true(
	    result.items[0].pending_bulk_data.size() == 1u,
	    "native multi-frame PixelData BulkDataURI should remain one element-level ref");
	expect_true(
	    result.items[0].pending_bulk_data[0].kind == dicom::JsonBulkTargetKind::element &&
	        result.items[0].pending_bulk_data[0].uri == "instances/1/frames" &&
	        result.items[0].pending_bulk_data[0].media_type == "application/octet-stream" &&
	        result.items[0].pending_bulk_data[0].transfer_syntax_uid.empty(),
	    "native multi-frame PixelData should keep its original BulkDataURI without guessing a transfer syntax");
}

void test_read_json_top_level_array_returns_multiple_items() {
	const std::string json =
	    R"([{"0020000D":{"vr":"UI","Value":["1.2.3"]}},{"0020000D":{"vr":"UI","Value":["4.5.6"]}}])";
	auto result = dicom::read_json(
	    reinterpret_cast<const std::uint8_t*>(json.data()), json.size());
	expect_true(result.items.size() == 2u, "top-level array should produce multiple result items");
	expect_true(
	    result.items[0].file->get_dataelement("StudyInstanceUID").to_string_view().value_or("") ==
	        "1.2.3",
	    "first array item should parse independently");
	expect_true(
	    result.items[1].file->get_dataelement("StudyInstanceUID").to_string_view().value_or("") ==
	        "4.5.6",
	    "second array item should parse independently");
}

void test_read_json_encapsulated_multiframe_expands_frame_refs_when_file_meta_transfer_syntax_is_present() {
	const std::string json =
	    R"({"00020010":{"vr":"UI","Value":["1.2.840.10008.1.2.4.50"]},"00280008":{"vr":"IS","Value":[2]},"7FE00010":{"vr":"OB","BulkDataURI":"instances/1/frames"}})";
	auto result = dicom::read_json(
	    reinterpret_cast<const std::uint8_t*>(json.data()), json.size());
	expect_true(result.items.size() == 1u, "object JSON should produce one result item");
	expect_true(
	    result.items[0].pending_bulk_data.size() == 2u,
	    "encapsulated multi-frame PixelData should expand to one pending ref per frame");
	expect_true(
	    result.items[0].pending_bulk_data[0].kind == dicom::JsonBulkTargetKind::pixel_frame &&
	        result.items[0].pending_bulk_data[0].frame_index == 0u &&
	        result.items[0].pending_bulk_data[0].uri == "instances/1/frames/1" &&
	        result.items[0].pending_bulk_data[0].media_type == "image/jpeg" &&
	        result.items[0].pending_bulk_data[0].transfer_syntax_uid ==
	            "1.2.840.10008.1.2.4.50",
	    "first encapsulated frame URI should be expanded to /frames/1");
	expect_true(
	    result.items[0].pending_bulk_data[1].kind == dicom::JsonBulkTargetKind::pixel_frame &&
	        result.items[0].pending_bulk_data[1].frame_index == 1u &&
	        result.items[0].pending_bulk_data[1].uri == "instances/1/frames/2" &&
	        result.items[0].pending_bulk_data[1].media_type == "image/jpeg" &&
	        result.items[0].pending_bulk_data[1].transfer_syntax_uid ==
	            "1.2.840.10008.1.2.4.50",
	    "second encapsulated frame URI should be expanded to /frames/2");
}

void test_read_json_available_transfer_syntax_uid_does_not_drive_bulk_expansion() {
	const std::string json =
	    R"({"00083002":{"vr":"UI","Value":["1.2.840.10008.1.2.4.50"]},"00280008":{"vr":"IS","Value":[2]},"7FE00010":{"vr":"OB","BulkDataURI":"instances/1/frames"}})";
	auto result = dicom::read_json(
	    reinterpret_cast<const std::uint8_t*>(json.data()), json.size());
	expect_true(result.items.size() == 1u, "object JSON should produce one result item");
	expect_true(
	    result.items[0].pending_bulk_data.size() == 1u,
	    "Available Transfer Syntax UID should not by itself expand frame refs");
	expect_true(
	    result.items[0].pending_bulk_data[0].kind == dicom::JsonBulkTargetKind::element &&
	        result.items[0].pending_bulk_data[0].uri == "instances/1/frames" &&
	        result.items[0].pending_bulk_data[0].media_type == "application/octet-stream" &&
	        result.items[0].pending_bulk_data[0].transfer_syntax_uid.empty(),
	    "Available Transfer Syntax UID should not be treated as pixel bulk encoding metadata");
}

void test_read_json_encapsulated_generic_bulk_uri_expands_frame_refs() {
	const std::string json =
	    R"({"00020010":{"vr":"UI","Value":["1.2.840.10008.1.2.4.50"]},"00280008":{"vr":"IS","Value":[1]},"7FE00010":{"vr":"OB","BulkDataURI":"instances/1/bulk/7FE00010"}})";
	auto result = dicom::read_json(
	    reinterpret_cast<const std::uint8_t*>(json.data()), json.size());
	expect_true(result.items.size() == 1u, "object JSON should produce one result item");
	expect_true(
	    result.items[0].pending_bulk_data.size() == 1u,
	    "encapsulated single-frame generic PixelData URI should expand to one frame ref");
	expect_true(
	    result.items[0].pending_bulk_data[0].kind == dicom::JsonBulkTargetKind::pixel_frame &&
	        result.items[0].pending_bulk_data[0].frame_index == 0u &&
	        result.items[0].pending_bulk_data[0].uri == "instances/1/bulk/7FE00010/frames/1" &&
	        result.items[0].pending_bulk_data[0].media_type == "image/jpeg" &&
	        result.items[0].pending_bulk_data[0].transfer_syntax_uid ==
	            "1.2.840.10008.1.2.4.50",
	    "generic encapsulated PixelData URIs should expand to /frames/1 when reading JSON");
}

void test_read_json_preserves_existing_frame_specific_bulk_uri() {
	const std::string json =
	    R"({"00020010":{"vr":"UI","Value":["1.2.840.10008.1.2.4.50"]},"00280008":{"vr":"IS","Value":[1]},"7FE00010":{"vr":"OB","BulkDataURI":"instances/1/frames/1"}})";
	auto result = dicom::read_json(
	    reinterpret_cast<const std::uint8_t*>(json.data()), json.size());
	expect_true(result.items.size() == 1u, "object JSON should produce one result item");
	expect_true(
	    result.items[0].pending_bulk_data.size() == 1u,
	    "frame-specific encapsulated PixelData URI should stay as one frame ref");
	expect_true(
	    result.items[0].pending_bulk_data[0].kind == dicom::JsonBulkTargetKind::pixel_frame &&
	        result.items[0].pending_bulk_data[0].frame_index == 0u &&
	        result.items[0].pending_bulk_data[0].uri == "instances/1/frames/1" &&
	        result.items[0].pending_bulk_data[0].media_type == "image/jpeg" &&
	        result.items[0].pending_bulk_data[0].transfer_syntax_uid ==
	            "1.2.840.10008.1.2.4.50",
	    "existing frame-specific PixelData URIs should not be expanded again");
}

void test_read_json_preserves_existing_frame_list_bulk_uri() {
	const std::string json =
	    R"({"00020010":{"vr":"UI","Value":["1.2.840.10008.1.2.4.50"]},"00280008":{"vr":"IS","Value":[3]},"7FE00010":{"vr":"OB","BulkDataURI":"instances/1/frames/1,2,3"}})";
	auto result = dicom::read_json(
	    reinterpret_cast<const std::uint8_t*>(json.data()), json.size());
	expect_true(result.items.size() == 1u, "object JSON should produce one result item");
	expect_true(
	    result.items[0].pending_bulk_data.size() == 3u,
	    "frame-list encapsulated PixelData URI should expand to one ref per listed frame");
	expect_true(
	    result.items[0].pending_bulk_data[0].kind == dicom::JsonBulkTargetKind::pixel_frame &&
	        result.items[0].pending_bulk_data[0].frame_index == 0u &&
	        result.items[0].pending_bulk_data[0].uri == "instances/1/frames/1" &&
	        result.items[0].pending_bulk_data[1].frame_index == 1u &&
	        result.items[0].pending_bulk_data[1].uri == "instances/1/frames/2" &&
	        result.items[0].pending_bulk_data[2].frame_index == 2u &&
	        result.items[0].pending_bulk_data[2].uri == "instances/1/frames/3",
	    "existing frame-list PixelData URI should not be expanded twice");
}

void test_read_json_missing_vr_falls_back_for_uid_and_private_un() {
	const std::string json =
	    R"({"00080018":{"Value":["1.2.840.10008.5.1.4.1.1.2"]},"00083002":{"Value":["1.2.840.10008.1.2.4.80"]},"00091110":{"Value":["ee51d3c338c9fa07dcdf8fab027dfd6136e21f002cef5916662dce0f614ce43f"]},"00091112":{"Value":["instance"]}})";
	auto result = dicom::read_json(
	    reinterpret_cast<const std::uint8_t*>(json.data()), json.size());
	expect_true(result.items.size() == 1u, "object JSON should produce one result item");
	auto& file = *result.items[0].file;
	expect_true(
	    file["SOPInstanceUID"].vr() == dicom::VR::UI &&
	        file["SOPInstanceUID"].to_string_view().value_or("") == "1.2.840.10008.5.1.4.1.1.2",
	    "known UID-like values without vr should still resolve to UI");
	expect_true(
	    file["00083002"].vr() == dicom::VR::UI &&
	        file["00083002"].to_string_view().value_or("") == "1.2.840.10008.1.2.4.80",
	    "unknown UID-like values without vr should fall back to UI");
	expect_true(
	    file["00091110"].vr() == dicom::VR::UN &&
	        file["00091110"].to_utf8_string().value_or("") ==
	            "ee51d3c338c9fa07dcdf8fab027dfd6136e21f002cef5916662dce0f614ce43f",
	    "private string values without vr should conservatively fall back to UN");
	expect_true(
	    file["00091112"].vr() == dicom::VR::UN &&
	        file["00091112"].to_utf8_string().value_or("") == "instance" &&
	        std::string_view(reinterpret_cast<const char*>(file["00091112"].value_span().data()),
	            file["00091112"].value_span().size()) == "instance",
	    "private label values without vr should stay readable while defaulting to UN");
}

void test_read_json_missing_charset_keeps_utf8_but_blocks_raw_materialization() {
	const std::string json =
	    R"({"00080080":{"vr":"LO","Value":["\uD55C\uAE00"]}})";
	auto result = dicom::read_json(
	    reinterpret_cast<const std::uint8_t*>(json.data()), json.size());
	const auto& element = result.items[0].file->get_dataelement("InstitutionName");
	const std::string expected(
	    reinterpret_cast<const char*>(u8"한글"), sizeof(u8"한글") - 1u);
	expect_true(
	    element.to_utf8_string().value_or("") == expected,
	    "UTF-8 text should remain readable without forcing early charset encoding");
	bool threw = false;
	try {
		(void)element.value_span();
	} catch (const std::exception&) {
		threw = true;
	}
	expect_true(
	    threw,
	    "raw access should fail when non-ASCII text has no declared SpecificCharacterSet");
}

void test_set_bulk_data_element_target_writes_raw_value_bytes() {
	dicom::DicomFile file;
	dicom::JsonBulkRef ref{};
	ref.kind = dicom::JsonBulkTargetKind::element;
	ref.path = "00080016";
	ref.uri = "/bulk/00080016";
	ref.vr = dicom::VR::UI;

	const std::string uid = "1.2.840.10008.5.1.4.1.1.2";
	expect_true(
	    file.set_bulk_data(
	        ref,
	        std::span<const std::uint8_t>(
	            reinterpret_cast<const std::uint8_t*>(uid.data()), uid.size())),
	    "set_bulk_data should succeed for element targets");
	expect_true(
	    file["SOPClassUID"].to_string_view().value_or("") == uid,
	    "set_bulk_data should populate the referenced element");
}

void test_set_bulk_data_pixel_frame_target_populates_encapsulated_frame_slot() {
	dicom::DicomFile file;
	expect_true(file.set_value("NumberOfFrames", 2L), "NumberOfFrames assignment failed");

	dicom::JsonBulkRef ref{};
	ref.kind = dicom::JsonBulkTargetKind::pixel_frame;
	ref.path = "7FE00010";
	ref.frame_index = 1u;
	ref.uri = "/frames/2";

	const std::vector<std::uint8_t> frame_bytes{0x11u, 0x22u, 0x33u, 0x44u};
	expect_true(
	    file.set_bulk_data(ref, frame_bytes),
	    "set_bulk_data should succeed for pixel-frame targets");
	expect_true(
	    file["PixelData"].vr().is_pixel_sequence(),
	    "pixel-frame bulk refs should materialize encapsulated PixelData");
	const auto stored = file.encoded_pixel_frame_view(1u);
	expect_true(
	    stored.size() == frame_bytes.size() &&
	        std::equal(stored.begin(), stored.end(), frame_bytes.begin(), frame_bytes.end()),
	    "pixel-frame bulk refs should populate the requested frame bytes");
}

void test_read_json_empty_input_reports_non_json_error() {
	const std::string payload;
	bool threw = false;
	try {
		(void)dicom::read_json(
		    reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size());
	} catch (const std::exception& ex) {
		threw = true;
		const std::string message = ex.what();
		expect_true(
		    message.find("not a DICOM JSON stream") != std::string::npos,
		    "empty input error should say the payload is not a DICOM JSON stream");
		expect_true(
		    message.find("empty input") != std::string::npos,
		    "empty input error should describe the empty-input case");
	}
	expect_true(threw, "empty input should raise an error");
}

void test_read_json_gzip_input_reports_non_json_error() {
	const std::vector<std::uint8_t> gzip_like{0x1Fu, 0x8Bu, 0x08u, 0x00u, 0x00u, 0x00u};
	bool threw = false;
	try {
		(void)dicom::read_json(gzip_like.data(), gzip_like.size());
	} catch (const std::exception& ex) {
		threw = true;
		const std::string message = ex.what();
		expect_true(
		    message.find("not a DICOM JSON stream") != std::string::npos,
		    "gzip-like input error should say the payload is not a DICOM JSON stream");
		expect_true(
		    message.find("invalid JSON byte sequence") != std::string::npos,
		    "gzip-like input error should report an invalid JSON byte sequence");
	}
	expect_true(threw, "gzip-like input should raise an error");
}

void test_read_json_non_json_input_reports_expected_top_level_shape() {
	const std::string payload = "not-json";
	bool threw = false;
	try {
		(void)dicom::read_json(
		    reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size());
	} catch (const std::exception& ex) {
		threw = true;
		const std::string message = ex.what();
		expect_true(
		    message.find("not a DICOM JSON stream") != std::string::npos,
		    "plain invalid input should say the payload is not a DICOM JSON stream");
		expect_true(
		    message.find("top-level JSON object or array") != std::string::npos,
		    "plain invalid input should mention the expected top-level JSON shape");
	}
	expect_true(threw, "plain invalid input should raise an error");
}

}  // namespace

int main() {
	test_read_json_empty_object_returns_empty_file();
	test_read_json_empty_array_returns_no_items();
	test_read_json_parses_values_sequences_and_bulk_refs();
	test_read_json_top_level_array_returns_multiple_items();
	test_read_json_encapsulated_multiframe_expands_frame_refs_when_file_meta_transfer_syntax_is_present();
	test_read_json_available_transfer_syntax_uid_does_not_drive_bulk_expansion();
	test_read_json_encapsulated_generic_bulk_uri_expands_frame_refs();
	test_read_json_preserves_existing_frame_specific_bulk_uri();
	test_read_json_preserves_existing_frame_list_bulk_uri();
	test_read_json_missing_vr_falls_back_for_uid_and_private_un();
	test_read_json_missing_charset_keeps_utf8_but_blocks_raw_materialization();
	test_set_bulk_data_element_target_writes_raw_value_bytes();
	test_set_bulk_data_pixel_frame_target_populates_encapsulated_frame_slot();
	test_read_json_empty_input_reports_non_json_error();
	test_read_json_gzip_input_reports_non_json_error();
	test_read_json_non_json_input_reports_expected_top_level_shape();
	return 0;
}
