#include <algorithm>
#include <cstdint>
#include <array>
#include <fstream>
#include <string>
#include <vector>
#include <iostream>
#include <sstream>
#include <exception>
#include <stdexcept>
#include <cstdlib>
#include <cstdio>
#include <atomic>
#include <mutex>
#include <thread>

#include <dicom.h>
#include <instream.h>
#include "codec_builtin_flags.hpp"

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
	if (px_vr.uses_specific_character_set()) fail("PX should not use specific character set");
	if (px_vr.allows_multiple_text_values()) fail("PX should not allow multiple text values");
	if (px_vr.str() != std::string_view("PX")) fail("PX string mismatch");

	const auto pn_vr = dicom::VR::PN;
	if (!pn_vr.uses_specific_character_set()) fail("PN should use specific character set");
	if (!pn_vr.allows_multiple_text_values()) fail("PN should allow multiple text values");

	const auto ut_vr = dicom::VR::UT;
	if (!ut_vr.uses_specific_character_set()) fail("UT should use specific character set");
	if (ut_vr.allows_multiple_text_values()) fail("UT should not allow multiple text values");

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
			dicom::DataElement signed_int_elem("Rows"_tag, dicom::VR::SL, 0, 0, nullptr);
			if (!signed_int_elem.from_int(12345)) {
				fail("DataElement::from_int should encode SL");
			}
			if (signed_int_elem.to_int().value_or(0) != 12345) {
				fail("DataElement::from_int SL roundtrip mismatch");
			}
		}
		{
			dicom::DataElement inline_elem("Rows"_tag, dicom::VR::OB, 0, 0, nullptr);
			inline_elem.reserve_value_bytes(3);
		if (inline_elem.length() != 3) {
			fail("DataElement::reserve_value_bytes should update length");
		}
		if (inline_elem.storage_kind() != dicom::DataElement::StorageKind::inline_bytes) {
			fail("DataElement::reserve_value_bytes should use inline storage for small values");
		}
		if (inline_elem.value_span().size() != 3) {
			fail("DataElement::reserve_value_bytes should expose reserved span size");
		}

		dicom::DataElement heap_elem("Rows"_tag, dicom::VR::OB, 0, 0, nullptr);
		heap_elem.reserve_value_bytes(dicom::DataElement::kInlineStorageBytes + 1);
		if (heap_elem.storage_kind() != dicom::DataElement::StorageKind::heap) {
			fail("DataElement::reserve_value_bytes should use heap storage for large values");
		}
		auto first_heap_ptr = heap_elem.value_span().data();
		heap_elem.reserve_value_bytes(dicom::DataElement::kInlineStorageBytes + 1);
			if (heap_elem.value_span().data() != first_heap_ptr) {
				fail("DataElement::reserve_value_bytes should reuse heap when capacity is sufficient");
			}
		}
		{
			dicom::DataElement bytes_elem("Rows"_tag, dicom::VR::OB, 0, 0, nullptr);
			const std::array<std::uint8_t, 8> raw{
			    0x01u, 0x00u, 0xFFu, 0x7Fu, 0x78u, 0x56u, 0x34u, 0x12u};
			bytes_elem.set_value_bytes(raw);
			auto bytes = bytes_elem.value_span();
			if (bytes.size() != raw.size()) {
				fail("DataElement::value_span should expose raw bytes");
			}
			for (std::size_t i = 0; i < raw.size(); ++i) {
				if (bytes[i] != raw[i]) {
					fail("DataElement::value_span value mismatch");
				}
			}
		}
		{
			dicom::DataElement moved_bytes_elem("Rows"_tag, dicom::VR::OB, 0, 0, nullptr);
			std::vector<std::uint8_t> raw(1024);
			for (std::size_t i = 0; i < raw.size(); ++i) {
				raw[i] = static_cast<std::uint8_t>(i & 0xFFu);
			}
			moved_bytes_elem.set_value_bytes(std::move(raw));
			if (moved_bytes_elem.storage_kind() != dicom::DataElement::StorageKind::owned_bytes) {
				fail("DataElement::set_value_bytes(move) should adopt vector storage");
			}
			const auto bytes = moved_bytes_elem.value_span();
			if (bytes.size() != 1024) {
				fail("DataElement::set_value_bytes(move) size mismatch");
			}
			if (bytes[0] != 0x00u || bytes[1] != 0x01u || bytes[255] != 0xFFu) {
				fail("DataElement::set_value_bytes(move) value mismatch");
			}
		}
		{
			DataSet ds;
			auto& no_binding = ds.add_dataelement("Rows"_tag, dicom::VR::US);
			if (no_binding.storage_kind() != dicom::DataElement::StorageKind::none) {
				fail("DataSet::add_dataelement(tag, vr) should keep storage_kind none");
			}
			auto& bound = ds.add_dataelement("Rows"_tag, dicom::VR::US, 123, 2);
			if (bound.storage_kind() != dicom::DataElement::StorageKind::stream ||
			    bound.offset() != 123) {
				fail("DataSet::add_dataelement(tag, vr, offset, length) should bind stream metadata");
			}
			auto& rebound = ds.add_dataelement("Rows"_tag, dicom::VR::US);
			if (rebound.storage_kind() != dicom::DataElement::StorageKind::none) {
				fail("DataSet::add_dataelement(tag, vr) should clear prior stream binding on replace");
			}
		}
		{
			dicom::DataElement padded_move_elem("Rows"_tag, dicom::VR::OB, 0, 0, nullptr);
			std::vector<std::uint8_t> odd_bytes{0x11u, 0x22u, 0x33u};
			padded_move_elem.adopt_value_bytes(std::move(odd_bytes));
			const auto bytes = padded_move_elem.value_span();
			if (bytes.size() != 4) {
				fail("DataElement::adopt_value_bytes should pad odd-length values");
			}
			if (bytes[0] != 0x11u || bytes[1] != 0x22u || bytes[2] != 0x33u) {
				fail("DataElement::adopt_value_bytes should preserve original payload");
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
			dicom::DataElement vector_elem("Rows"_tag, dicom::VR::US, 0, 0, nullptr);
			const std::array<long, 3> values{1, 2, 3};
			if (!vector_elem.from_long_vector(values)) {
				fail("DataElement::from_long_vector should encode US");
		}
		auto decoded = vector_elem.to_long_vector();
		if (!decoded || *decoded != std::vector<long>{1, 2, 3}) {
			fail("DataElement::from_long_vector US roundtrip mismatch");
		}
		const std::array<long, 2> invalid_values{-1, 2};
			if (vector_elem.from_long_vector(invalid_values)) {
				fail("DataElement::from_long_vector should reject out-of-range value for US");
			}
		}
		{
			dicom::DataElement vector_elem("Rows"_tag, dicom::VR::US, 0, 0, nullptr);
			const std::array<int, 3> values{1, 2, 3};
			if (!vector_elem.from_int_vector(values)) {
				fail("DataElement::from_int_vector should encode US");
			}
			auto decoded = vector_elem.to_int_vector();
			if (!decoded || *decoded != std::vector<int>{1, 2, 3}) {
				fail("DataElement::from_int_vector US roundtrip mismatch");
			}
			const std::array<int, 2> invalid_values{-1, 2};
			if (vector_elem.from_int_vector(invalid_values)) {
				fail("DataElement::from_int_vector should reject out-of-range value for US");
			}
		}
		{
			dicom::DataElement signed_very_long_elem("Rows"_tag, dicom::VR::SV, 0, 0, nullptr);
			constexpr long long kValue = 4294967296LL;
			if (!signed_very_long_elem.from_longlong(kValue)) {
				fail("DataElement::from_longlong should encode SV");
			}
			if (signed_very_long_elem.to_longlong().value_or(0) != kValue) {
				fail("DataElement::from_longlong SV roundtrip mismatch");
			}
			dicom::DataElement signed_long_elem("Rows"_tag, dicom::VR::SL, 0, 0, nullptr);
			if (signed_long_elem.from_longlong(kValue)) {
				fail("DataElement::from_longlong should reject out-of-range value for SL");
			}
		}
		{
			dicom::DataElement vector_elem("Rows"_tag, dicom::VR::SV, 0, 0, nullptr);
			const std::array<long long, 2> values{4294967296LL, 7LL};
			if (!vector_elem.from_longlong_vector(values)) {
				fail("DataElement::from_longlong_vector should encode SV");
			}
			auto decoded = vector_elem.to_longlong_vector();
			if (!decoded || *decoded != std::vector<long long>{4294967296LL, 7LL}) {
				fail("DataElement::from_longlong_vector SV roundtrip mismatch");
			}
			dicom::DataElement narrow_elem("Rows"_tag, dicom::VR::SL, 0, 0, nullptr);
			if (narrow_elem.from_longlong_vector(values)) {
				fail("DataElement::from_longlong_vector should reject out-of-range value for SL");
			}
		}
		{
			dicom::DataElement fd_elem("SliceThickness"_tag, dicom::VR::FD, 0, 0, nullptr);
			if (!fd_elem.from_double(12.5)) {
				fail("DataElement::from_double should encode FD");
			}
			if (fd_elem.to_double().value_or(0.0) != 12.5) {
				fail("DataElement::from_double FD roundtrip mismatch");
			}

			dicom::DataElement fl_elem("SliceThickness"_tag, dicom::VR::FL, 0, 0, nullptr);
			if (!fl_elem.from_double(3.25)) {
				fail("DataElement::from_double should encode FL");
			}
			if (fl_elem.to_double().value_or(0.0) != 3.25) {
				fail("DataElement::from_double FL roundtrip mismatch");
			}

			dicom::DataElement ds_elem("SliceThickness"_tag, dicom::VR::DS, 0, 0, nullptr);
			if (!ds_elem.from_double(1.5)) {
				fail("DataElement::from_double should encode DS");
			}
			if (ds_elem.to_double().value_or(0.0) != 1.5) {
				fail("DataElement::from_double DS roundtrip mismatch");
			}

			dicom::DataElement unsupported_elem("Rows"_tag, dicom::VR::US, 0, 0, nullptr);
			if (unsupported_elem.from_double(1.0)) {
				fail("DataElement::from_double should reject unsupported VR");
			}
		}
		{
			dicom::DataElement fd_vec_elem("SliceThickness"_tag, dicom::VR::FD, 0, 0, nullptr);
			const std::array<double, 3> values{1.5, 2.25, 3.75};
			if (!fd_vec_elem.from_double_vector(values)) {
				fail("DataElement::from_double_vector should encode FD");
			}
			auto decoded = fd_vec_elem.to_double_vector();
			if (!decoded || *decoded != std::vector<double>{1.5, 2.25, 3.75}) {
				fail("DataElement::from_double_vector FD roundtrip mismatch");
			}

			dicom::DataElement ds_vec_elem("SliceThickness"_tag, dicom::VR::DS, 0, 0, nullptr);
			if (!ds_vec_elem.from_double_vector(values)) {
				fail("DataElement::from_double_vector should encode DS");
			}
			auto ds_decoded = ds_vec_elem.to_double_vector();
			if (!ds_decoded || *ds_decoded != std::vector<double>{1.5, 2.25, 3.75}) {
				fail("DataElement::from_double_vector DS roundtrip mismatch");
			}
		}
		{
			const dicom::Tag offending_tag(0x0000, 0x0901);
			dicom::DataElement tag_elem(offending_tag, dicom::VR::AT, 0, 0, nullptr);
			const dicom::Tag expected(0x0010, 0x0020);
			if (!tag_elem.from_tag(expected)) {
				fail("DataElement::from_tag should encode AT");
			}
			if (tag_elem.to_tag().value_or(dicom::Tag()) != expected) {
				fail("DataElement::from_tag AT roundtrip mismatch");
			}

			dicom::DataElement tag_vec_elem(offending_tag, dicom::VR::AT, 0, 0, nullptr);
			const std::array<dicom::Tag, 3> tags{
			    dicom::Tag(0x0010, 0x0010),
			    dicom::Tag(0x0010, 0x0020),
			    dicom::Tag(0x0008, 0x0018)};
			if (!tag_vec_elem.from_tag_vector(tags)) {
				fail("DataElement::from_tag_vector should encode AT");
			}
			auto decoded = tag_vec_elem.to_tag_vector();
			if (!decoded ||
			    *decoded != std::vector<dicom::Tag>{dicom::Tag(0x0010, 0x0010),
			        dicom::Tag(0x0010, 0x0020), dicom::Tag(0x0008, 0x0018)}) {
				fail("DataElement::from_tag_vector AT roundtrip mismatch");
			}

			dicom::DataElement unsupported_elem("Rows"_tag, dicom::VR::US, 0, 0, nullptr);
			if (unsupported_elem.from_tag(expected)) {
				fail("DataElement::from_tag should reject non-AT VR");
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
			dicom::DataElement patient_names("PatientName"_tag, dicom::VR::PN, 0, 0, nullptr);
			const std::array<std::string_view, 2> names{"DOE^JOHN", "SMITH^ALICE"};
			if (!patient_names.from_string_views(names)) {
				fail("DataElement::from_string_views should encode PN");
			}
			auto decoded = patient_names.to_string_views();
			if (!decoded ||
			    *decoded != std::vector<std::string_view>{"DOE^JOHN", "SMITH^ALICE"}) {
				fail("DataElement::from_string_views PN roundtrip mismatch");
			}

			dicom::DataElement ui_values("SOPInstanceUID"_tag, dicom::VR::UI, 0, 0, nullptr);
			const std::array<std::string_view, 2> uids{"1.2.3", "1.2.840.10008.1.2"};
			if (!ui_values.from_string_views(uids)) {
				fail("DataElement::from_string_views should encode multi-value UI");
			}
			auto ui_decoded = ui_values.to_string_views();
			if (!ui_decoded ||
			    *ui_decoded != std::vector<std::string_view>{"1.2.3", "1.2.840.10008.1.2"}) {
				fail("DataElement::from_string_views UI roundtrip mismatch");
			}

			dicom::DataElement url_elem("RetrieveURL"_tag, dicom::VR::UR, 0, 0, nullptr);
			const std::array<std::string_view, 2> urls{"https://a", "https://b"};
			if (url_elem.from_string_views(urls)) {
				fail("DataElement::from_string_views should reject multi-value UR");
			}
		}
		{
			const std::string utf8_name("\xED\x99\x8D\xEA\xB8\xB8\xEB\x8F\x99", 9);
			dicom::DicomFile utf8_file;
			utf8_file.set_declared_specific_charset(dicom::SpecificCharacterSet::ISO_IR_192);
			auto& utf8_patient_name = utf8_file.add_dataelement("PatientName"_tag, dicom::VR::PN);
			if (!utf8_patient_name.from_utf8_view(utf8_name)) {
				fail("DataElement::from_utf8_view should encode UTF-8 text using current charset");
			}
			if (utf8_patient_name.value_span().empty()) {
				fail("from_utf8_view should materialize raw bytes immediately");
			}
			const auto utf8_owned = utf8_patient_name.to_utf8_string();
			if (!utf8_owned || *utf8_owned != utf8_name) {
				fail("DataElement::to_utf8_string should return owned UTF-8 text");
			}
			auto raw_utf8 = utf8_patient_name.to_string_view();
			if (!raw_utf8 || *raw_utf8 != utf8_name) {
				fail("UTF-8 raw storage should remain accessible through to_string_view");
			}

			auto& utf8_patient_names = utf8_file.add_dataelement("OtherPatientNames"_tag, dicom::VR::PN);
			const std::array<std::string_view, 2> utf8_names{utf8_name, "DOE^JOHN"};
			if (!utf8_patient_names.from_utf8_views(utf8_names)) {
				fail("DataElement::from_utf8_views should encode multi-value UTF-8 text");
			}
			const auto utf8_owned_values = utf8_patient_names.to_utf8_strings();
			if (!utf8_owned_values || utf8_owned_values->size() != 2 ||
			    (*utf8_owned_values)[0] != utf8_name || (*utf8_owned_values)[1] != "DOE^JOHN") {
				fail("DataElement::to_utf8_strings should return owned UTF-8 values");
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
			const auto implicit_le = dicom::uid::from_keyword("ImplicitVRLittleEndian");
			const auto deflated_le = dicom::uid::from_keyword("DeflatedExplicitVRLittleEndian");
			const auto encapsulated_uncompressed =
			    dicom::uid::from_keyword("EncapsulatedUncompressedExplicitVRLittleEndian");
			const auto jpeg_baseline = dicom::uid::from_keyword("JPEGBaseline8Bit");
			const auto jpeg_lossless_sv1 = dicom::uid::from_keyword("JPEGLosslessSV1");
			const auto jpegls_lossless = dicom::uid::from_keyword("JPEGLSLossless");
			const auto jpeg2000_lossless = dicom::uid::from_keyword("JPEG2000Lossless");
			const auto jpeg2000_lossy = dicom::uid::from_keyword("JPEG2000");
			const auto htj2k_lossless = dicom::uid::from_keyword("HTJ2KLossless");
			const auto jpegxl = dicom::uid::from_keyword("JPEGXL");
			const auto rle_lossless = dicom::uid::from_keyword("RLELossless");
			if (!implicit_le || !deflated_le || !encapsulated_uncompressed || !jpeg_baseline ||
			    !jpeg_lossless_sv1 || !jpegls_lossless || !jpeg2000_lossless ||
			    !jpeg2000_lossy || !htj2k_lossless || !jpegxl || !rle_lossless) {
				fail("uid::from_keyword should resolve transfer syntax classification fixtures");
			}

			if (!implicit_le->is_uncompressed() || implicit_le->is_encapsulated()) {
				fail("ImplicitVRLittleEndian classification mismatch");
			}
			if (!implicit_le->is_lossless() || implicit_le->is_lossy() ||
			    !implicit_le->supports_pixel_encode() || !implicit_le->supports_pixel_decode()) {
				fail("ImplicitVRLittleEndian capability mismatch");
			}
			if (!deflated_le->is_uncompressed() || deflated_le->is_encapsulated()) {
				fail("DeflatedExplicitVRLittleEndian classification mismatch");
			}
			if (!encapsulated_uncompressed->is_uncompressed() ||
			    !encapsulated_uncompressed->is_encapsulated()) {
				fail("EncapsulatedUncompressedExplicitVRLittleEndian classification mismatch");
			}
			if (!encapsulated_uncompressed->is_lossless() || encapsulated_uncompressed->is_lossy() ||
			    !encapsulated_uncompressed->supports_pixel_encode() ||
			    !encapsulated_uncompressed->supports_pixel_decode()) {
				fail("EncapsulatedUncompressedExplicitVRLittleEndian capability mismatch");
			}
			if (jpeg_baseline->is_uncompressed() || !jpeg_baseline->is_encapsulated()) {
				fail("JPEGBaseline8Bit classification mismatch");
			}
			if (jpeg_baseline->is_lossless() || !jpeg_baseline->is_lossy() ||
			    !jpeg_baseline->supports_pixel_encode() ||
			    !jpeg_baseline->supports_pixel_decode()) {
				fail("JPEGBaseline8Bit capability mismatch");
			}
			if (!jpeg_lossless_sv1->is_jpeg_lossless() || !jpeg_lossless_sv1->is_lossless() ||
			    jpeg_lossless_sv1->is_lossy() || !jpeg_lossless_sv1->supports_pixel_encode() ||
			    !jpeg_lossless_sv1->supports_pixel_decode()) {
				fail("JPEGLosslessSV1 capability mismatch");
			}
			if (!jpegls_lossless->is_jpegls() || !jpegls_lossless->is_lossless() ||
			    jpegls_lossless->is_lossy() || !jpegls_lossless->supports_pixel_encode() ||
			    !jpegls_lossless->supports_pixel_decode()) {
				fail("JPEGLSLossless capability mismatch");
			}
			if (!jpeg2000_lossless->is_jpeg2000() || jpeg2000_lossless->is_htj2k() ||
			    !jpeg2000_lossless->is_lossless() || jpeg2000_lossless->is_lossy() ||
			    !jpeg2000_lossless->supports_pixel_encode() ||
			    !jpeg2000_lossless->supports_pixel_decode()) {
				fail("JPEG2000Lossless capability mismatch");
			}
			if (!jpeg2000_lossy->is_jpeg2000() || jpeg2000_lossy->is_htj2k() ||
			    jpeg2000_lossy->is_lossless() || !jpeg2000_lossy->is_lossy() ||
			    !jpeg2000_lossy->supports_pixel_encode() ||
			    !jpeg2000_lossy->supports_pixel_decode()) {
				fail("JPEG2000 capability mismatch");
			}
			if (!htj2k_lossless->is_htj2k() || !htj2k_lossless->is_lossless() ||
			    htj2k_lossless->is_lossy() || !htj2k_lossless->supports_pixel_encode() ||
			    !htj2k_lossless->supports_pixel_decode()) {
				fail("HTJ2KLossless capability mismatch");
			}
			if (!jpegxl->is_jpegxl() || jpegxl->is_lossless() || !jpegxl->is_lossy() ||
			    !jpegxl->supports_pixel_encode() || !jpegxl->supports_pixel_decode()) {
				fail("JPEGXL capability mismatch");
			}
			if (!rle_lossless->is_rle() || !rle_lossless->is_encapsulated()) {
				fail("RLELossless classification mismatch");
			}
			if (!rle_lossless->is_lossless() || rle_lossless->is_lossy() ||
			    !rle_lossless->supports_pixel_encode() ||
			    !rle_lossless->supports_pixel_decode()) {
				fail("RLELossless capability mismatch");
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


    return 0;
}
