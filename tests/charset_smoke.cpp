#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <dicom.h>

int main() {
	using dicom::read_bytes;
	using namespace dicom::literals;

	auto fail = [](const std::string& msg) {
		std::cerr << msg << std::endl;
		std::exit(1);
	};

	auto expect_bytes = [&](std::span<const std::uint8_t> actual, const auto& expected, std::string_view msg) {
		if (actual.size() != expected.size() ||
		    !std::equal(actual.begin(), actual.end(), expected.begin(), expected.end())) {
			fail(std::string(msg));
		}
	};

	auto exercise_single_byte_charset = [&](dicom::SpecificCharacterSet charset, std::string_view utf8_text,
	                                     std::span<const std::uint8_t> raw_bytes) {
		const auto* info = dicom::specific_character_set_info(charset);
		if (!info) {
			fail("SpecificCharacterSet registry metadata should exist for single-byte charset test");
		}
		const std::string term(info->defined_term);
		std::vector<std::uint8_t> expected_write_bytes(raw_bytes.begin(), raw_bytes.end());
		if ((expected_write_bytes.size() % 2u) != 0u) {
			expected_write_bytes.push_back(0x20u);
		}

		dicom::DicomFile encoded_file;
		encoded_file.set_declared_specific_charset(charset);
		auto& encoded_name = encoded_file.add_dataelement("PatientName"_tag, dicom::VR::PN);
		if (!encoded_name.from_utf8_view(utf8_text)) {
			fail(term + " UTF-8 input assignment should succeed");
		}
		const auto encoded_bytes = encoded_file.write_bytes();
		auto encoded_roundtrip = read_bytes(term + "-roundtrip", encoded_bytes.data(), encoded_bytes.size());
		if (!encoded_roundtrip) {
			fail(term + " roundtrip returned null");
		}
		const auto charset_term =
		    encoded_roundtrip->get_dataelement("SpecificCharacterSet"_tag).to_string_view();
		if (!charset_term || *charset_term != term) {
			fail(term + " write should inject matching SpecificCharacterSet");
		}
		expect_bytes(encoded_roundtrip->get_dataelement("PatientName"_tag).value_span(), expected_write_bytes,
		    term + " write should encode the expected single-byte repertoire");

		dicom::DicomFile raw_file;
		auto& raw_charset = raw_file.add_dataelement("SpecificCharacterSet"_tag, dicom::VR::CS);
		if (!raw_charset.from_string_view(term)) {
			fail(term + " SpecificCharacterSet raw assignment should succeed");
		}
		auto& raw_name = raw_file.add_dataelement("PatientName"_tag, dicom::VR::PN);
		const std::string raw_value(reinterpret_cast<const char*>(raw_bytes.data()), raw_bytes.size());
		if (!raw_name.from_string_view(raw_value)) {
			fail(term + " raw PatientName assignment should succeed");
		}
		const auto decoded_name = raw_name.to_utf8_string();
		if (!decoded_name || *decoded_name != utf8_text) {
			fail(term + " raw decode should match expected UTF-8 text");
		}
		if (raw_file.dump().find(std::string(utf8_text)) == std::string::npos) {
			fail(term + " dump should render decoded UTF-8 text");
		}
	};

	{
		const std::array<std::uint8_t, 1> latin2_bytes{0xA1u};
		const std::string latin2_utf8("\xC4\x84", 2);  // U+0104
		exercise_single_byte_charset(dicom::SpecificCharacterSet::ISO_IR_101, latin2_utf8, latin2_bytes);
		exercise_single_byte_charset(
		    dicom::SpecificCharacterSet::ISO_2022_IR_101, latin2_utf8, latin2_bytes);
	}

	{
		const std::array<std::uint8_t, 1> latin3_bytes{0xA1u};
		const std::string latin3_utf8("\xC4\xA6", 2);  // U+0126
		exercise_single_byte_charset(dicom::SpecificCharacterSet::ISO_IR_109, latin3_utf8, latin3_bytes);
		exercise_single_byte_charset(
		    dicom::SpecificCharacterSet::ISO_2022_IR_109, latin3_utf8, latin3_bytes);
	}

	{
		const std::array<std::uint8_t, 1> latin4_bytes{0xC0u};
		const std::string latin4_utf8("\xC4\x80", 2);  // U+0100
		exercise_single_byte_charset(dicom::SpecificCharacterSet::ISO_IR_110, latin4_utf8, latin4_bytes);
		exercise_single_byte_charset(
		    dicom::SpecificCharacterSet::ISO_2022_IR_110, latin4_utf8, latin4_bytes);
	}

	{
		const std::array<std::uint8_t, 1> latin9_bytes{0xA4u};
		const std::string latin9_utf8("\xE2\x82\xAC", 3);  // U+20AC
		exercise_single_byte_charset(dicom::SpecificCharacterSet::ISO_IR_203, latin9_utf8, latin9_bytes);
		exercise_single_byte_charset(
		    dicom::SpecificCharacterSet::ISO_2022_IR_203, latin9_utf8, latin9_bytes);
	}

	{
		const std::array<std::uint8_t, 1> thai_bytes{0xA1u};
		const std::string thai_utf8("\xE0\xB8\x81", 3);  // U+0E01
		exercise_single_byte_charset(dicom::SpecificCharacterSet::ISO_IR_166, thai_utf8, thai_bytes);
		exercise_single_byte_charset(
		    dicom::SpecificCharacterSet::ISO_2022_IR_166, thai_utf8, thai_bytes);
	}

	{
		const std::string utf8_name("\xED\x99\x8D\xEA\xB8\xB8\xEB\x8F\x99", 9);
		dicom::DicomFile utf8_file;
		utf8_file.set_declared_specific_charset(dicom::SpecificCharacterSet::ISO_IR_192);
		auto& patient_name = utf8_file.add_dataelement("PatientName"_tag, dicom::VR::PN);
		if (!patient_name.from_utf8_view(utf8_name)) {
			fail("UTF-8 patient name assignment should succeed");
		}

		const auto bytes = utf8_file.write_bytes();
		auto roundtrip = read_bytes("utf8-roundtrip", bytes.data(), bytes.size());
		if (!roundtrip) fail("utf8 roundtrip returned null");
		const auto charset = roundtrip->get_dataelement("SpecificCharacterSet"_tag).to_string_view();
		if (!charset || *charset != "ISO_IR 192") {
			fail("UTF-8 write should inject ISO_IR 192");
		}
		const auto roundtrip_name = roundtrip->get_dataelement("PatientName"_tag).to_utf8_string();
		if (!roundtrip_name || *roundtrip_name != utf8_name) {
			fail("UTF-8 write/read roundtrip mismatch");
		}

		dicom::DicomFile default_file;
		auto& default_name = default_file.add_dataelement("PatientName"_tag, dicom::VR::PN);
		if (default_name.from_utf8_view(utf8_name)) {
			fail("from_utf8_view should reject non-ASCII text without a charset declaration");
		}
	}

	{
		const std::string utf8_latin1_name("Gr\xC3\xB6\xC3\x9F" "e", 7);
		const std::string latin1_name("Gr\xF6\xDF" "e", 5);
		const std::array<std::uint8_t, 6> expected_name{
		    static_cast<std::uint8_t>('G'), static_cast<std::uint8_t>('r'), 0xF6u, 0xDFu,
		    static_cast<std::uint8_t>('e'), 0x20u};
		dicom::DicomFile latin1_file;
		latin1_file.set_declared_specific_charset(dicom::SpecificCharacterSet::ISO_IR_100);
		auto& patient_name = latin1_file.add_dataelement("PatientName"_tag, dicom::VR::PN);
		if (!patient_name.from_utf8_view(utf8_latin1_name)) {
			fail("ISO_IR 100 UTF-8 input assignment should succeed");
		}

		const auto bytes = latin1_file.write_bytes();
		auto roundtrip = read_bytes("latin1-roundtrip", bytes.data(), bytes.size());
		if (!roundtrip) fail("latin1 roundtrip returned null");
		const auto charset = roundtrip->get_dataelement("SpecificCharacterSet"_tag).to_string_view();
		if (!charset || *charset != "ISO_IR 100") {
			fail("ISO_IR 100 write should inject SpecificCharacterSet");
		}
		expect_bytes(roundtrip->get_dataelement("PatientName"_tag).value_span(), expected_name,
		    "ISO_IR 100 write should encode Latin-1 bytes");

		dicom::DicomFile raw_latin1_file;
		auto& raw_charset = raw_latin1_file.add_dataelement("SpecificCharacterSet"_tag, dicom::VR::CS);
		if (!raw_charset.from_string_view("ISO_IR 100")) {
			fail("ISO_IR 100 SpecificCharacterSet raw assignment should succeed");
		}
		auto& raw_patient_name = raw_latin1_file.add_dataelement("PatientName"_tag, dicom::VR::PN);
		if (!raw_patient_name.from_string_view(latin1_name)) {
			fail("raw ISO_IR 100 PatientName assignment should succeed");
		}
		const auto decoded_name = raw_patient_name.to_utf8_string();
		if (!decoded_name || *decoded_name != utf8_latin1_name) {
			fail("to_utf8_string should decode raw ISO_IR 100 text");
		}
		if (raw_latin1_file.dump().find(utf8_latin1_name) == std::string::npos) {
			fail("dump should render ISO_IR 100 text as UTF-8");
		}

		raw_latin1_file.set_specific_charset(dicom::SpecificCharacterSet::ISO_IR_192);
		const auto utf8_bytes = raw_latin1_file.write_bytes();
		auto utf8_roundtrip = read_bytes("latin1-to-utf8-roundtrip", utf8_bytes.data(), utf8_bytes.size());
		if (!utf8_roundtrip) fail("latin1 to utf8 roundtrip returned null");
		const auto utf8_charset =
		    utf8_roundtrip->get_dataelement("SpecificCharacterSet"_tag).to_string_view();
		if (!utf8_charset || *utf8_charset != "ISO_IR 192") {
			fail("ISO_IR 100 to ISO_IR 192 should update SpecificCharacterSet");
		}
		const auto utf8_roundtrip_name =
		    utf8_roundtrip->get_dataelement("PatientName"_tag).to_utf8_string();
		if (!utf8_roundtrip_name || *utf8_roundtrip_name != utf8_latin1_name) {
			fail("ISO_IR 100 to ISO_IR 192 transcode mismatch");
		}

		dicom::DicomFile failing_file;
		failing_file.set_declared_specific_charset(dicom::SpecificCharacterSet::ISO_IR_100);
		auto& failing_name = failing_file.add_dataelement("PatientName"_tag, dicom::VR::PN);
		const std::string korean_name("\xED\x99\x8D\xEA\xB8\xB8\xEB\x8F\x99", 9);
		if (failing_name.from_utf8_view(korean_name)) {
			fail("from_utf8_view should reject characters outside ISO_IR 100");
		}

		dicom::DicomFile qmark_file;
		qmark_file.set_declared_specific_charset(dicom::SpecificCharacterSet::ISO_IR_100);
		auto& qmark_name = qmark_file.add_dataelement("PatientName"_tag, dicom::VR::PN);
		bool qmark_replaced = false;
		if (!qmark_name.from_utf8_view(
		        korean_name, dicom::CharsetEncodeErrorPolicy::replace_qmark, &qmark_replaced)) {
			fail("from_utf8_view replace_qmark should succeed for ISO_IR 100");
		}
		if (!qmark_replaced) {
			fail("from_utf8_view replace_qmark should report replacement");
		}
		const auto qmark_raw = qmark_name.to_string_view();
		if (!qmark_raw || *qmark_raw != "???") {
			fail("from_utf8_view replace_qmark should store question-mark replacements");
		}

		dicom::DicomFile unicode_escape_file;
		unicode_escape_file.set_declared_specific_charset(dicom::SpecificCharacterSet::ISO_IR_100);
		auto& unicode_escape_name =
		    unicode_escape_file.add_dataelement("PatientName"_tag, dicom::VR::PN);
		if (!unicode_escape_name.from_utf8_view(
		        korean_name, dicom::CharsetEncodeErrorPolicy::replace_unicode_escape)) {
			fail("from_utf8_view replace_unicode_escape should succeed for ISO_IR 100");
		}
		const auto unicode_escape_raw = unicode_escape_name.to_string_view();
		if (!unicode_escape_raw ||
		    *unicode_escape_raw != "(U+D64D)(U+AE38)(U+B3D9)") {
			fail("from_utf8_view replace_unicode_escape should store code point escapes");
		}
	}

	{
		const std::string utf8_latin1_name("Gr\xC3\xB6\xC3\x9F" "e", 7);
		const std::string latin1_name("Gr\xF6\xDF" "e", 5);
		const std::array<std::uint8_t, 6> expected_name{
		    static_cast<std::uint8_t>('G'), static_cast<std::uint8_t>('r'), 0xF6u, 0xDFu,
		    static_cast<std::uint8_t>('e'), 0x20u};
		const std::array<std::uint8_t, 4> pn_reset_expected{0xF6u, 0x5Eu, 0xDFu, 0x20u};
		dicom::DicomFile iso2022_file;
		iso2022_file.set_declared_specific_charset(dicom::SpecificCharacterSet::ISO_2022_IR_100);
		auto& patient_name = iso2022_file.add_dataelement("PatientName"_tag, dicom::VR::PN);
		if (!patient_name.from_utf8_view(utf8_latin1_name)) {
			fail("ISO 2022 IR 100 UTF-8 input assignment should succeed");
		}

		const auto bytes = iso2022_file.write_bytes();
		auto roundtrip = read_bytes("iso2022-latin1-roundtrip", bytes.data(), bytes.size());
		if (!roundtrip) fail("ISO 2022 IR 100 roundtrip returned null");
		const auto charset = roundtrip->get_dataelement("SpecificCharacterSet"_tag).to_string_view();
		if (!charset || *charset != "ISO 2022 IR 100") {
			fail("ISO 2022 IR 100 write should inject SpecificCharacterSet");
		}
		expect_bytes(roundtrip->get_dataelement("PatientName"_tag).value_span(), expected_name,
		    "ISO 2022 IR 100 should omit the initial G1 escape");

		dicom::DicomFile raw_file;
		auto& raw_charset = raw_file.add_dataelement("SpecificCharacterSet"_tag, dicom::VR::CS);
		if (!raw_charset.from_string_view("ISO 2022 IR 100")) {
			fail("ISO 2022 IR 100 SpecificCharacterSet raw assignment should succeed");
		}
		auto& raw_patient_name = raw_file.add_dataelement("PatientName"_tag, dicom::VR::PN);
		if (!raw_patient_name.from_string_view(latin1_name)) {
			fail("raw ISO 2022 IR 100 PatientName assignment should succeed");
		}
		const auto decoded_name = raw_patient_name.to_utf8_string();
		if (!decoded_name || *decoded_name != utf8_latin1_name) {
			fail("to_utf8_string should decode raw ISO 2022 IR 100 text");
		}

		dicom::DicomFile escaped_file;
		auto& escaped_charset = escaped_file.add_dataelement("SpecificCharacterSet"_tag, dicom::VR::CS);
		if (!escaped_charset.from_string_view("ISO 2022 IR 100")) {
			fail("escaped ISO 2022 IR 100 SpecificCharacterSet assignment should succeed");
		}
		auto& escaped_patient_name = escaped_file.add_dataelement("PatientName"_tag, dicom::VR::PN);
		const std::string escaped_latin1_name("\x1B\x2D\x41Gr\xF6\xDF" "e", 8);
		if (!escaped_patient_name.from_string_view(escaped_latin1_name)) {
			fail("escaped ISO 2022 IR 100 PatientName assignment should succeed");
		}
		const auto escaped_decoded_name = escaped_patient_name.to_utf8_string();
		if (!escaped_decoded_name || *escaped_decoded_name != utf8_latin1_name) {
			fail("to_utf8_string should ignore the declared ISO 2022 IR 100 escape sequence");
		}

		dicom::DicomFile undeclared_escape_file;
		auto& undeclared_charset =
		    undeclared_escape_file.add_dataelement("SpecificCharacterSet"_tag, dicom::VR::CS);
		if (!undeclared_charset.from_string_view("ISO 2022 IR 100")) {
			fail("undeclared escape charset assignment should succeed");
		}
		auto& undeclared_name =
		    undeclared_escape_file.add_dataelement("PatientName"_tag, dicom::VR::PN);
		const std::string utf8_cyrillic_name(
		    "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82", 12);
		const std::string escaped_cyrillic_name("\x1B\x2D\x4C\xBF\xE0\xD8\xD2\xD5\xE2", 9);
		if (!undeclared_name.from_string_view(escaped_cyrillic_name)) {
			fail("undeclared ISO 2022 escape PatientName assignment should succeed");
		}
		const auto lenient_decoded_name = undeclared_name.to_utf8_string();
		if (!lenient_decoded_name || *lenient_decoded_name != utf8_cyrillic_name) {
			fail("decoder should accept supported undeclared ISO 2022 escape sequences");
		}

		dicom::DicomFile unknown_escape_file;
		auto& unknown_escape_charset =
		    unknown_escape_file.add_dataelement("SpecificCharacterSet"_tag, dicom::VR::CS);
		if (!unknown_escape_charset.from_string_view("ISO 2022 IR 100")) {
			fail("unknown escape charset assignment should succeed");
		}
		auto& unknown_escape_name =
		    unknown_escape_file.add_dataelement("PatientName"_tag, dicom::VR::PN);
		const std::string unknown_escape_raw("\x1B\x25\x47" "A", 4);
		if (!unknown_escape_name.from_string_view(unknown_escape_raw)) {
			fail("unknown escape PatientName assignment should succeed");
		}
		if (unknown_escape_name.to_utf8_string().has_value()) {
			fail("decoder should reject unknown undeclared ISO 2022 escape sequences");
		}
		bool unknown_escape_replaced = false;
		const auto unknown_escape_fffd = unknown_escape_name.to_utf8_string(
		    dicom::CharsetDecodeErrorPolicy::replace_fffd, &unknown_escape_replaced);
		if (!unknown_escape_replaced) {
			fail("to_utf8_string replace_fffd should report replacement");
		}
		const std::string replacement_fffd("\xEF\xBF\xBD", 3);
		if (!unknown_escape_fffd || *unknown_escape_fffd != replacement_fffd) {
			fail("to_utf8_string replace_fffd should replace unknown ISO 2022 escape sequences");
		}
		const auto unknown_escape_hex =
		    unknown_escape_name.to_utf8_string(dicom::CharsetDecodeErrorPolicy::replace_hex_escape);
		if (!unknown_escape_hex || *unknown_escape_hex != "(0x1B)(0x25)(0x47)(0x41)") {
			fail("to_utf8_string replace_hex_escape should expose unknown ISO 2022 escape bytes");
		}

		dicom::DicomFile pn_reset_file;
		pn_reset_file.set_declared_specific_charset(dicom::SpecificCharacterSet::ISO_2022_IR_100);
		auto& pn_reset_name = pn_reset_file.add_dataelement("PatientName"_tag, dicom::VR::PN);
		const std::string utf8_latin1_pn("\xC3\xB6^\xC3\x9F", 5);
		if (!pn_reset_name.from_utf8_view(utf8_latin1_pn)) {
			fail("ISO 2022 IR 100 PN reset assignment should succeed");
		}
		const auto pn_reset_bytes = pn_reset_file.write_bytes();
		auto pn_reset_roundtrip =
		    read_bytes("iso2022-latin1-pn-reset-roundtrip", pn_reset_bytes.data(), pn_reset_bytes.size());
		if (!pn_reset_roundtrip) fail("ISO 2022 IR 100 PN reset roundtrip returned null");
		expect_bytes(pn_reset_roundtrip->get_dataelement("PatientName"_tag).value_span(), pn_reset_expected,
		    "ISO 2022 IR 100 should not re-designate initial G1 after PN reset");
	}

	{
		dicom::DicomFile invalid_multi_term_file;
		auto& invalid_charset =
		    invalid_multi_term_file.add_dataelement("SpecificCharacterSet"_tag, dicom::VR::CS);
		const std::array<std::string_view, 2> invalid_terms{"ISO_IR 100", "ISO_IR 144"};
		if (!invalid_charset.from_string_views(invalid_terms)) {
			fail("non-ISO 2022 multi-term SpecificCharacterSet assignment should succeed as raw CS");
		}
		auto& invalid_name =
		    invalid_multi_term_file.add_dataelement("PatientName"_tag, dicom::VR::PN);
		if (!invalid_name.from_string_view("Gr\xF6\xDF" "e")) {
			fail("non-ISO 2022 multi-term PatientName assignment should succeed");
		}
		if (invalid_name.to_utf8_string().has_value()) {
			fail("non-ISO 2022 multi-term SpecificCharacterSet should be rejected for decode");
		}
	}

	{
		const std::string utf8_korean_name("\xED\x99\x8D\xEA\xB8\xB8\xEB\x8F\x99", 9);
		dicom::DicomFile strict_file;
		strict_file.set_declared_specific_charset(dicom::SpecificCharacterSet::ISO_IR_192);
		auto& strict_name = strict_file.add_dataelement("PatientName"_tag, dicom::VR::PN);
		if (!strict_name.from_utf8_view(utf8_korean_name)) {
			fail("UTF-8 source assignment should succeed before strict charset mutation");
		}
		try {
			strict_file.set_specific_charset(
			    dicom::SpecificCharacterSet::ISO_IR_100,
			    dicom::CharsetEncodeErrorPolicy::strict);
			fail("set_specific_charset strict should throw for unrepresentable characters");
		} catch (const std::exception&) {
		}
		const auto strict_charset =
		    strict_file.get_dataelement("SpecificCharacterSet"_tag).to_string_view();
		if (!strict_charset || *strict_charset != "ISO_IR 192") {
			fail("strict set_specific_charset failure should leave declared charset unchanged");
		}

		dicom::DicomFile qmark_transcode_file;
		qmark_transcode_file.set_declared_specific_charset(dicom::SpecificCharacterSet::ISO_IR_192);
		auto& qmark_transcode_name =
		    qmark_transcode_file.add_dataelement("PatientName"_tag, dicom::VR::PN);
		if (!qmark_transcode_name.from_utf8_view(utf8_korean_name)) {
			fail("UTF-8 source assignment should succeed before replace_qmark charset mutation");
		}
		bool qmark_transcode_replaced = false;
		qmark_transcode_file.set_specific_charset(dicom::SpecificCharacterSet::ISO_IR_100,
		    dicom::CharsetEncodeErrorPolicy::replace_qmark, &qmark_transcode_replaced);
		if (!qmark_transcode_replaced) {
			fail("replace_qmark set_specific_charset should report replacement");
		}
		const auto qmark_transcode_charset =
		    qmark_transcode_file.get_dataelement("SpecificCharacterSet"_tag).to_string_view();
		if (!qmark_transcode_charset || *qmark_transcode_charset != "ISO_IR 100") {
			fail("replace_qmark set_specific_charset should update SpecificCharacterSet");
		}
		const auto qmark_transcode_raw =
		    qmark_transcode_file.get_dataelement("PatientName"_tag).to_string_view();
		if (!qmark_transcode_raw || *qmark_transcode_raw != "???") {
			fail("replace_qmark set_specific_charset should store question-mark replacements");
		}

		dicom::DicomFile unicode_escape_transcode_file;
		unicode_escape_transcode_file.set_declared_specific_charset(
		    dicom::SpecificCharacterSet::ISO_IR_192);
		auto& unicode_escape_transcode_name =
		    unicode_escape_transcode_file.add_dataelement("PatientName"_tag, dicom::VR::PN);
		if (!unicode_escape_transcode_name.from_utf8_view(utf8_korean_name)) {
			fail(
			    "UTF-8 source assignment should succeed before replace_unicode_escape charset mutation");
		}
		unicode_escape_transcode_file.set_specific_charset(
		    dicom::SpecificCharacterSet::ISO_IR_100,
		    dicom::CharsetEncodeErrorPolicy::replace_unicode_escape);
		const auto unicode_escape_transcode_raw =
		    unicode_escape_transcode_file.get_dataelement("PatientName"_tag).to_string_view();
		if (!unicode_escape_transcode_raw ||
		    *unicode_escape_transcode_raw != "(U+D64D)(U+AE38)(U+B3D9)") {
			fail("replace_unicode_escape set_specific_charset should store code point escapes");
		}
	}

	{
		const std::string utf8_cyrillic_name(
		    "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82", 12);
		const std::string cyrillic_name("\xBF\xE0\xD8\xD2\xD5\xE2", 6);
		const std::array<std::uint8_t, 6> cyrillic_bytes{0xBFu, 0xE0u, 0xD8u, 0xD2u, 0xD5u, 0xE2u};

		dicom::DicomFile cyrillic_file;
		cyrillic_file.set_declared_specific_charset(dicom::SpecificCharacterSet::ISO_IR_144);
		auto& patient_name = cyrillic_file.add_dataelement("PatientName"_tag, dicom::VR::PN);
		if (!patient_name.from_utf8_view(utf8_cyrillic_name)) {
			fail("ISO_IR 144 UTF-8 input assignment should succeed");
		}
		const auto bytes = cyrillic_file.write_bytes();
		auto roundtrip = read_bytes("cyrillic-roundtrip", bytes.data(), bytes.size());
		if (!roundtrip) fail("ISO_IR 144 roundtrip returned null");
		expect_bytes(roundtrip->get_dataelement("PatientName"_tag).value_span(), cyrillic_bytes,
		    "ISO_IR 144 write should encode Cyrillic bytes");

		dicom::DicomFile raw_cyrillic_file;
		auto& raw_charset =
		    raw_cyrillic_file.add_dataelement("SpecificCharacterSet"_tag, dicom::VR::CS);
		if (!raw_charset.from_string_view("ISO_IR 144")) {
			fail("ISO_IR 144 SpecificCharacterSet raw assignment should succeed");
		}
		auto& raw_patient_name = raw_cyrillic_file.add_dataelement("PatientName"_tag, dicom::VR::PN);
		if (!raw_patient_name.from_string_view(cyrillic_name)) {
			fail("raw ISO_IR 144 PatientName assignment should succeed");
		}
		const auto decoded_name = raw_patient_name.to_utf8_string();
		if (!decoded_name || *decoded_name != utf8_cyrillic_name) {
			fail("to_utf8_string should decode raw ISO_IR 144 text");
		}
		if (raw_cyrillic_file.dump().find(utf8_cyrillic_name) == std::string::npos) {
			fail("dump should render ISO_IR 144 text as UTF-8");
		}

		raw_cyrillic_file.set_specific_charset(dicom::SpecificCharacterSet::ISO_IR_192);
		const auto utf8_bytes = raw_cyrillic_file.write_bytes();
		auto utf8_roundtrip =
		    read_bytes("cyrillic-to-utf8-roundtrip", utf8_bytes.data(), utf8_bytes.size());
		if (!utf8_roundtrip) fail("cyrillic to utf8 roundtrip returned null");
		const auto utf8_roundtrip_name =
		    utf8_roundtrip->get_dataelement("PatientName"_tag).to_utf8_string();
		if (!utf8_roundtrip_name || *utf8_roundtrip_name != utf8_cyrillic_name) {
			fail("ISO_IR 144 to ISO_IR 192 transcode mismatch");
		}

		dicom::DicomFile iso2022_file;
		iso2022_file.set_declared_specific_charset(dicom::SpecificCharacterSet::ISO_2022_IR_144);
		auto& iso2022_patient_name = iso2022_file.add_dataelement("PatientName"_tag, dicom::VR::PN);
		if (!iso2022_patient_name.from_utf8_view(utf8_cyrillic_name)) {
			fail("ISO 2022 IR 144 UTF-8 input assignment should succeed");
		}
		const auto iso2022_bytes = iso2022_file.write_bytes();
		auto iso2022_roundtrip =
		    read_bytes("iso2022-cyrillic-roundtrip", iso2022_bytes.data(), iso2022_bytes.size());
		if (!iso2022_roundtrip) fail("ISO 2022 IR 144 roundtrip returned null");
		expect_bytes(iso2022_roundtrip->get_dataelement("PatientName"_tag).value_span(), cyrillic_bytes,
		    "ISO 2022 IR 144 should omit the initial G1 escape");
	}

	{
		const std::string utf8_korean_name("\xEA\xB0\x80\xEB\x82\x98", 6);
		const std::array<std::uint8_t, 4> ksx1001_bytes{0xB0u, 0xA1u, 0xB3u, 0xAAu};
		dicom::DicomFile korean_file;
		korean_file.set_declared_specific_charset(dicom::SpecificCharacterSet::ISO_2022_IR_149);
		auto& patient_name = korean_file.add_dataelement("PatientName"_tag, dicom::VR::PN);
		if (!patient_name.from_utf8_view(utf8_korean_name)) {
			fail("ISO 2022 IR 149 UTF-8 input assignment should succeed");
		}
		const auto bytes = korean_file.write_bytes();
		auto roundtrip = read_bytes("iso2022-korean-roundtrip", bytes.data(), bytes.size());
		if (!roundtrip) fail("ISO 2022 IR 149 roundtrip returned null");
		expect_bytes(roundtrip->get_dataelement("PatientName"_tag).value_span(), ksx1001_bytes,
		    "ISO 2022 IR 149 should omit the initial G1 escape");

		dicom::DicomFile raw_file;
		auto& raw_charset = raw_file.add_dataelement("SpecificCharacterSet"_tag, dicom::VR::CS);
		if (!raw_charset.from_string_view("ISO 2022 IR 149")) {
			fail("ISO 2022 IR 149 SpecificCharacterSet raw assignment should succeed");
		}
		auto& raw_patient_name = raw_file.add_dataelement("PatientName"_tag, dicom::VR::PN);
		if (!raw_patient_name.from_string_view(
		        std::string(reinterpret_cast<const char*>(ksx1001_bytes.data()), ksx1001_bytes.size()))) {
			fail("raw ISO 2022 IR 149 PatientName assignment should succeed");
		}
		const auto decoded_name = raw_patient_name.to_utf8_string();
		if (!decoded_name || *decoded_name != utf8_korean_name) {
			fail("to_utf8_string should decode raw ISO 2022 IR 149 text");
		}
	}

	{
		const std::string utf8_chinese("\xE4\xB8\xAD\xE6\x96\x87", 6);
		const std::array<std::uint8_t, 4> gb2312_bytes{0xD6u, 0xD0u, 0xCEu, 0xC4u};
		dicom::DicomFile chinese_file;
		chinese_file.set_declared_specific_charset(dicom::SpecificCharacterSet::ISO_2022_IR_58);
		auto& patient_name = chinese_file.add_dataelement("PatientName"_tag, dicom::VR::PN);
		if (!patient_name.from_utf8_view(utf8_chinese)) {
			fail("ISO 2022 IR 58 UTF-8 input assignment should succeed");
		}
		const auto bytes = chinese_file.write_bytes();
		auto roundtrip = read_bytes("iso2022-chinese-roundtrip", bytes.data(), bytes.size());
		if (!roundtrip) fail("ISO 2022 IR 58 roundtrip returned null");
		expect_bytes(roundtrip->get_dataelement("PatientName"_tag).value_span(), gb2312_bytes,
		    "ISO 2022 IR 58 should omit the initial G1 escape");

		dicom::DicomFile raw_file;
		auto& raw_charset = raw_file.add_dataelement("SpecificCharacterSet"_tag, dicom::VR::CS);
		if (!raw_charset.from_string_view("ISO 2022 IR 58")) {
			fail("ISO 2022 IR 58 SpecificCharacterSet raw assignment should succeed");
		}
		auto& raw_patient_name = raw_file.add_dataelement("PatientName"_tag, dicom::VR::PN);
		if (!raw_patient_name.from_string_view(
		        std::string(reinterpret_cast<const char*>(gb2312_bytes.data()), gb2312_bytes.size()))) {
			fail("raw ISO 2022 IR 58 PatientName assignment should succeed");
		}
		const auto decoded_name = raw_patient_name.to_utf8_string();
		if (!decoded_name || *decoded_name != utf8_chinese) {
			fail("to_utf8_string should decode raw ISO 2022 IR 58 text");
		}
	}

	{
		const std::string utf8_jis0208("\xE4\xBA\x9C", 3);
		dicom::DicomFile jisx0208_file;
		jisx0208_file.set_declared_specific_charset(dicom::SpecificCharacterSet::ISO_2022_IR_87);
		auto& patient_name = jisx0208_file.add_dataelement("PatientName"_tag, dicom::VR::PN);
		if (!patient_name.from_utf8_view(utf8_jis0208)) {
			fail("ISO 2022 IR 87 UTF-8 input assignment should succeed");
		}
		const auto bytes = jisx0208_file.write_bytes();
		auto roundtrip = read_bytes("iso2022-jis0208-roundtrip", bytes.data(), bytes.size());
		if (!roundtrip) fail("ISO 2022 IR 87 roundtrip returned null");
		if (roundtrip->get_dataelement("PatientName"_tag).length() != 2) {
			fail("ISO 2022 IR 87 should omit the initial G0 escape");
		}
		const auto decoded_name = roundtrip->get_dataelement("PatientName"_tag).to_utf8_string();
		if (!decoded_name || *decoded_name != utf8_jis0208) {
			fail("ISO 2022 IR 87 roundtrip mismatch");
		}

		const std::string utf8_jis0212("\xE4\xB8\x82", 3);
		dicom::DicomFile jisx0212_file;
		jisx0212_file.set_declared_specific_charset(dicom::SpecificCharacterSet::ISO_2022_IR_159);
		auto& jisx0212_name = jisx0212_file.add_dataelement("PatientName"_tag, dicom::VR::PN);
		if (!jisx0212_name.from_utf8_view(utf8_jis0212)) {
			fail("ISO 2022 IR 159 UTF-8 input assignment should succeed");
		}
		const auto jisx0212_bytes = jisx0212_file.write_bytes();
		auto jisx0212_roundtrip =
		    read_bytes("iso2022-jis0212-roundtrip", jisx0212_bytes.data(), jisx0212_bytes.size());
		if (!jisx0212_roundtrip) fail("ISO 2022 IR 159 roundtrip returned null");
		if (jisx0212_roundtrip->get_dataelement("PatientName"_tag).length() != 2) {
			fail("ISO 2022 IR 159 should omit the initial G0 escape");
		}
		const auto jisx0212_roundtrip_name =
		    jisx0212_roundtrip->get_dataelement("PatientName"_tag).to_utf8_string();
		if (!jisx0212_roundtrip_name || *jisx0212_roundtrip_name != utf8_jis0212) {
			fail("ISO 2022 IR 159 roundtrip mismatch");
		}
	}

	{
		const std::string utf8_katakana("\xEF\xBD\xB6\xEF\xBE\x85", 6);
		const std::array<std::uint8_t, 2> jisx0201_bytes{0xB6u, 0xC5u};
		dicom::DicomFile jisx0201_file;
		jisx0201_file.set_declared_specific_charset(dicom::SpecificCharacterSet::ISO_IR_13);
		auto& patient_name = jisx0201_file.add_dataelement("PatientName"_tag, dicom::VR::PN);
		if (!patient_name.from_utf8_view(utf8_katakana)) {
			fail("ISO_IR 13 UTF-8 input assignment should succeed");
		}
		const auto bytes = jisx0201_file.write_bytes();
		auto roundtrip = read_bytes("jisx0201-roundtrip", bytes.data(), bytes.size());
		if (!roundtrip) fail("ISO_IR 13 roundtrip returned null");
		expect_bytes(roundtrip->get_dataelement("PatientName"_tag).value_span(), jisx0201_bytes,
		    "ISO_IR 13 write should encode half-width katakana bytes");

		dicom::DicomFile iso2022_file;
		iso2022_file.set_declared_specific_charset(dicom::SpecificCharacterSet::ISO_2022_IR_13);
		auto& iso2022_name = iso2022_file.add_dataelement("PatientName"_tag, dicom::VR::PN);
		if (!iso2022_name.from_utf8_view(utf8_katakana)) {
			fail("ISO 2022 IR 13 UTF-8 input assignment should succeed");
		}
		const auto iso2022_bytes = iso2022_file.write_bytes();
		auto iso2022_roundtrip =
		    read_bytes("iso2022-jisx0201-roundtrip", iso2022_bytes.data(), iso2022_bytes.size());
		if (!iso2022_roundtrip) fail("ISO 2022 IR 13 roundtrip returned null");
		expect_bytes(iso2022_roundtrip->get_dataelement("PatientName"_tag).value_span(), jisx0201_bytes,
		    "ISO 2022 IR 13 should omit the initial G1 escape");

		dicom::DicomFile escaped_file;
		auto& escaped_charset = escaped_file.add_dataelement("SpecificCharacterSet"_tag, dicom::VR::CS);
		if (!escaped_charset.from_string_view("ISO 2022 IR 13")) {
			fail("ISO 2022 IR 13 SpecificCharacterSet assignment should succeed");
		}
		auto& escaped_patient_name = escaped_file.add_dataelement("PatientName"_tag, dicom::VR::PN);
		const std::string escaped_katakana_name("\x1B\x29\x49\xB6\xC5", 5);
		if (!escaped_patient_name.from_string_view(escaped_katakana_name)) {
			fail("escaped ISO 2022 IR 13 PatientName assignment should succeed");
		}
		const auto decoded_name = escaped_patient_name.to_utf8_string();
		if (!decoded_name || *decoded_name != utf8_katakana) {
			fail("to_utf8_string should decode escaped ISO 2022 IR 13 text");
		}
	}

	{
		const std::string utf8_katakana("\xEF\xBD\xB6\xEF\xBE\x85", 6);
		const std::string utf8_jis0208("\xE4\xBA\x9C", 3);
		const std::string utf8_japanese_name = utf8_katakana + "^" + utf8_jis0208;
		const std::array<std::uint8_t, 8> expected_bytes{
		    0xB6u, 0xC5u, 0x5Eu, 0x1Bu, 0x24u, 0x42u, 0x30u, 0x21u};
		const std::array<std::string_view, 2> expected_charsets{
		    "ISO 2022 IR 13", "ISO 2022 IR 87"};

		dicom::DicomFile japanese_file;
		auto& charset = japanese_file.add_dataelement("SpecificCharacterSet"_tag, dicom::VR::CS);
		if (!charset.from_string_views(expected_charsets)) {
			fail("multi-term SpecificCharacterSet assignment should succeed");
		}
		auto& patient_name = japanese_file.add_dataelement("PatientName"_tag, dicom::VR::PN);
		if (!patient_name.from_utf8_view(utf8_japanese_name)) {
			fail("multi-term Japanese UTF-8 input assignment should succeed");
		}
		const auto encoded_bytes = japanese_file.write_bytes();
		auto roundtrip =
		    read_bytes("multi-term-japanese-roundtrip", encoded_bytes.data(), encoded_bytes.size());
		if (!roundtrip) fail("multi-term Japanese roundtrip returned null");
		const auto roundtrip_charset =
		    roundtrip->get_dataelement("SpecificCharacterSet"_tag).to_string_views();
		if (!roundtrip_charset || roundtrip_charset->size() != expected_charsets.size() ||
		    !std::equal(roundtrip_charset->begin(), roundtrip_charset->end(), expected_charsets.begin())) {
			fail("write_bytes should preserve multi-term SpecificCharacterSet");
		}
		expect_bytes(roundtrip->get_dataelement("PatientName"_tag).value_span(), expected_bytes,
		    "multi-term Japanese write should switch between declared ISO 2022 charsets");
		const auto decoded_name = roundtrip->get_dataelement("PatientName"_tag).to_utf8_string();
		if (!decoded_name || *decoded_name != utf8_japanese_name) {
			fail("to_utf8_string should decode multi-term ISO 2022 Japanese text");
		}

		dicom::DicomFile target_file;
		target_file.set_declared_specific_charset({
		    dicom::SpecificCharacterSet::ISO_2022_IR_13,
		    dicom::SpecificCharacterSet::ISO_2022_IR_87});
		auto& target_patient_name = target_file.add_dataelement("PatientName"_tag, dicom::VR::PN);
		if (!target_patient_name.from_utf8_view(utf8_japanese_name)) {
			fail("multi-term declared charset UTF-8 input assignment should succeed");
		}
		const auto target_bytes = target_file.write_bytes();
		auto target_roundtrip =
		    read_bytes("multi-term-target-roundtrip", target_bytes.data(), target_bytes.size());
		if (!target_roundtrip) fail("multi-term declared charset roundtrip returned null");
		const auto target_charset =
		    target_roundtrip->get_dataelement("SpecificCharacterSet"_tag).to_string_views();
		if (!target_charset || target_charset->size() != expected_charsets.size() ||
		    !std::equal(target_charset->begin(), target_charset->end(), expected_charsets.begin())) {
			fail("write_bytes should preserve declared multi-term SpecificCharacterSet");
		}
		expect_bytes(target_roundtrip->get_dataelement("PatientName"_tag).value_span(), expected_bytes,
		    "write_bytes should encode bytes for declared multi-term SpecificCharacterSet");
	}

	{
		const std::array<std::string_view, 2> japanese_charsets{
		    "ISO 2022 IR 13", "ISO 2022 IR 87"};
		const std::array<std::string_view, 2> japanese_values{
		    "\xEF\xBD\xB6", "\xEF\xBE\x85"};
		const std::array<std::uint8_t, 4> expected_bytes{0xB6u, 0x5Cu, 0xC5u, 0x20u};

		dicom::DicomFile japanese_file;
		auto& charset = japanese_file.add_dataelement("SpecificCharacterSet"_tag, dicom::VR::CS);
		if (!charset.from_string_views(japanese_charsets)) {
			fail("multi-term reset SpecificCharacterSet assignment should succeed");
		}
		auto& patient_name = japanese_file.add_dataelement("PatientName"_tag, dicom::VR::PN);
		if (!patient_name.from_utf8_views(japanese_values)) {
			fail("multi-term reset UTF-8 multi-value assignment should succeed");
		}

		const auto bytes = japanese_file.write_bytes();
		auto roundtrip = read_bytes("multi-term-reset-roundtrip", bytes.data(), bytes.size());
		if (!roundtrip) fail("multi-term reset roundtrip returned null");
		expect_bytes(roundtrip->get_dataelement("PatientName"_tag).value_span(), expected_bytes,
		    "multi-term ISO 2022 write should not re-designate initial state after value reset");
		const auto decoded_values = roundtrip->get_dataelement("PatientName"_tag).to_utf8_strings();
		if (!decoded_values || decoded_values->size() != 2 ||
		    (*decoded_values)[0] != japanese_values[0] ||
		    (*decoded_values)[1] != japanese_values[1]) {
			fail("to_utf8_strings should decode ISO 2022 values written without repeated initial designation");
		}
	}

	{
		const std::string utf8_name = std::string("Gr") + "\xD0\x9F" + "^" + "\xC3\xB1";
		const std::array<std::string_view, 2> latin_cyrillic_charsets{
		    "ISO 2022 IR 100", "ISO 2022 IR 144"};
		const std::string raw_name("\x47\x72\x1B\x2D\x4C\xBF\x5E\xF1", 8);

		dicom::DicomFile latin_cyrillic_file;
		auto& charset =
		    latin_cyrillic_file.add_dataelement("SpecificCharacterSet"_tag, dicom::VR::CS);
		if (!charset.from_string_views(latin_cyrillic_charsets)) {
			fail("Latin/Cyrillic multi-term SpecificCharacterSet assignment should succeed");
		}
		auto& patient_name =
		    latin_cyrillic_file.add_dataelement("PatientName"_tag, dicom::VR::PN);
		if (!patient_name.from_string_view(raw_name)) {
			fail("multi-term ISO 2022 PN raw assignment should succeed");
		}
		const auto decoded_name = patient_name.to_utf8_string();
		if (!decoded_name || *decoded_name != utf8_name) {
			fail("to_utf8_string should reset ISO 2022 G1 state at PN boundaries");
		}
	}

	{
		const std::string utf8_chinese("\xE4\xB8\xAD\xE6\x96\x87", 6);
		const std::array<std::uint8_t, 4> gbk_bytes{0xD6u, 0xD0u, 0xCEu, 0xC4u};
		dicom::DicomFile gbk_file;
		gbk_file.set_declared_specific_charset(dicom::SpecificCharacterSet::GBK);
		auto& patient_name = gbk_file.add_dataelement("PatientName"_tag, dicom::VR::PN);
		if (!patient_name.from_utf8_view(utf8_chinese)) {
			fail("GBK UTF-8 input assignment should succeed");
		}

		const auto bytes = gbk_file.write_bytes();
		auto roundtrip = read_bytes("gbk-roundtrip", bytes.data(), bytes.size());
		if (!roundtrip) fail("gbk roundtrip returned null");
		expect_bytes(roundtrip->get_dataelement("PatientName"_tag).value_span(), gbk_bytes,
		    "GBK write should encode GBK bytes");

		dicom::DicomFile raw_gbk_file;
		auto& raw_charset = raw_gbk_file.add_dataelement("SpecificCharacterSet"_tag, dicom::VR::CS);
		if (!raw_charset.from_string_view("GBK")) {
			fail("GBK SpecificCharacterSet raw assignment should succeed");
		}
		auto& raw_patient_name = raw_gbk_file.add_dataelement("PatientName"_tag, dicom::VR::PN);
		if (!raw_patient_name.from_string_view(
		        std::string(reinterpret_cast<const char*>(gbk_bytes.data()), gbk_bytes.size()))) {
			fail("raw GBK PatientName assignment should succeed");
		}
		const auto decoded_name = raw_patient_name.to_utf8_string();
		if (!decoded_name || *decoded_name != utf8_chinese) {
			fail("to_utf8_string should decode raw GBK text");
		}

		dicom::DicomFile failing_file;
		failing_file.set_declared_specific_charset(dicom::SpecificCharacterSet::GBK);
		auto& failing_name = failing_file.add_dataelement("PatientName"_tag, dicom::VR::PN);
		const std::string utf8_extension_b("\xF0\xA0\x80\x80", 4);
		if (failing_name.from_utf8_view(utf8_extension_b)) {
			fail("from_utf8_view should reject characters outside GBK");
		}
	}

	{
		const std::string utf8_extension_b("\xF0\xA0\x80\x80", 4);
		const std::array<std::uint8_t, 4> gb18030_bytes{0x95u, 0x32u, 0x82u, 0x36u};
		dicom::DicomFile gb18030_file;
		gb18030_file.set_declared_specific_charset(dicom::SpecificCharacterSet::GB18030);
		auto& patient_name = gb18030_file.add_dataelement("PatientName"_tag, dicom::VR::PN);
		if (!patient_name.from_utf8_view(utf8_extension_b)) {
			fail("GB18030 UTF-8 input assignment should succeed");
		}

		const auto bytes = gb18030_file.write_bytes();
		auto roundtrip = read_bytes("gb18030-roundtrip", bytes.data(), bytes.size());
		if (!roundtrip) fail("gb18030 roundtrip returned null");
		expect_bytes(roundtrip->get_dataelement("PatientName"_tag).value_span(), gb18030_bytes,
		    "GB18030 write should encode four-byte sequence");

		dicom::DicomFile raw_file;
		auto& raw_charset = raw_file.add_dataelement("SpecificCharacterSet"_tag, dicom::VR::CS);
		if (!raw_charset.from_string_view("GB18030")) {
			fail("GB18030 SpecificCharacterSet raw assignment should succeed");
		}
		auto& raw_patient_name = raw_file.add_dataelement("PatientName"_tag, dicom::VR::PN);
		if (!raw_patient_name.from_string_view(
		        std::string(reinterpret_cast<const char*>(gb18030_bytes.data()), gb18030_bytes.size()))) {
			fail("raw GB18030 PatientName assignment should succeed");
		}
		const auto decoded_name = raw_patient_name.to_utf8_string();
		if (!decoded_name || *decoded_name != utf8_extension_b) {
			fail("to_utf8_string should decode raw GB18030 text");
		}
	}

	{
		const std::string utf8_name_with_spaces(
		    "\xED\x99\x8D\xEA\xB8\xB8\xEB\x8F\x99  ", 11);
		dicom::DicomFile raw_utf8_file;
		auto& charset = raw_utf8_file.add_dataelement("SpecificCharacterSet"_tag, dicom::VR::CS);
		if (!charset.from_string_view("ISO_IR 192")) {
			fail("ISO_IR 192 SpecificCharacterSet raw assignment should succeed");
		}
		auto& patient_name = raw_utf8_file.add_dataelement("PatientName"_tag, dicom::VR::PN);
		if (!patient_name.from_string_view(utf8_name_with_spaces)) {
			fail("raw ISO_IR 192 PatientName assignment should succeed");
		}
		const auto original_raw = patient_name.value_span();

		const auto bytes = raw_utf8_file.write_bytes();
		auto roundtrip = read_bytes("raw-utf8-same-charset", bytes.data(), bytes.size());
		if (!roundtrip) fail("raw UTF-8 same-charset roundtrip returned null");
		const auto roundtrip_raw = roundtrip->get_dataelement("PatientName"_tag).value_span();
		if (roundtrip_raw.size() != original_raw.size() ||
		    !std::equal(original_raw.begin(), original_raw.end(), roundtrip_raw.begin(), roundtrip_raw.end())) {
			fail("same-charset write should preserve raw UTF-8 bytes");
		}
	}

	return 0;
}


