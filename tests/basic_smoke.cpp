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

		auto& rows = file->add_dataelement("Rows"_tag, dicom::VR::US, 0, 2);
		if (!rows) fail("DicomFile add_dataelement failed");
		const auto file_dump_after_add = file->dump();
		if (file_dump_after_add.find("'00280010'") == std::string::npos) {
			fail("DicomFile dump should include Rows");
		}
		const std::size_t long_desc_size = 256;
		std::vector<std::uint8_t> long_desc_value(long_desc_size, static_cast<std::uint8_t>('A'));
		auto& study_desc =
		    file->add_dataelement("StudyDescription"_tag, dicom::VR::LO, 0, long_desc_size);
		if (!study_desc) fail("DicomFile add StudyDescription failed");
		study_desc.set_value_bytes(long_desc_value);
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
		const auto& rows_by_tag = file->get_dataelement("Rows"_tag);
		if (rows_by_tag.is_missing()) {
			fail("DicomFile get_dataelement(Tag) failed");
		}
		const auto& rows_by_keyword = file->get_dataelement("Rows");
		if (rows_by_keyword.is_missing()) {
			fail("DicomFile get_dataelement(string_view) failed");
		}
		const auto& rows_ref = (*file)["Rows"_tag];
		if (rows_ref.tag().value() != "Rows"_tag.value()) fail("DicomFile operator[] failed");
		file->remove_dataelement("Rows"_tag);
		if (file->get_dataelement("Rows"_tag).is_present()) {
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

	dicom::DicomFile native_pixels;
	{
		auto& bits_allocated = native_pixels.add_dataelement("BitsAllocated"_tag, dicom::VR::US);
		if (!bits_allocated.from_long(16)) {
			fail("failed to set BitsAllocated for native pixel API test");
		}
		std::vector<std::uint8_t> native_pixel_bytes{0x34, 0x12, 0x78, 0x56};
		native_pixels.set_native_pixel_data(std::move(native_pixel_bytes));
		const auto& native_pixel_elem = native_pixels.get_dataelement("PixelData"_tag);
		if (native_pixel_elem.is_missing()) {
			fail("set_native_pixel_data should create PixelData");
		}
		if (native_pixel_elem.vr() != dicom::VR::OW) {
			fail("set_native_pixel_data should infer OW for BitsAllocated=16");
		}
		const auto span = native_pixel_elem.value_span();
		if (span.size() != 4 || span[0] != 0x34 || span[1] != 0x12 || span[2] != 0x78 || span[3] != 0x56) {
			fail("set_native_pixel_data payload mismatch");
		}
	}
	dicom::DicomFile set_pixel_data_file;
	{
		std::vector<std::uint8_t> source_bytes(40, 0xEEu);
		const auto write_row = [&](std::size_t frame, std::size_t row,
		                       std::array<std::uint8_t, 6> payload) {
			const std::size_t frame_stride = 20;
			const std::size_t row_stride = 8;
			const std::size_t base = frame * frame_stride + row * row_stride;
			for (std::size_t i = 0; i < payload.size(); ++i) {
				source_bytes[base + i] = payload[i];
			}
		};
		write_row(0, 0, {0x01u, 0x00u, 0x02u, 0x00u, 0x03u, 0x00u});
		write_row(0, 1, {0x04u, 0x00u, 0x05u, 0x00u, 0x06u, 0x00u});
		write_row(1, 0, {0x11u, 0x00u, 0x12u, 0x00u, 0x13u, 0x00u});
		write_row(1, 1, {0x14u, 0x00u, 0x15u, 0x00u, 0x16u, 0x00u});

		dicom::pixel::PixelSource source{};
		source.bytes = std::span<const std::uint8_t>(source_bytes.data(), source_bytes.size());
		source.data_type = dicom::pixel::DataType::u16;
		source.rows = 2;
		source.cols = 3;
		source.frames = 2;
		source.samples_per_pixel = 1;
		source.row_stride = 8;
		source.frame_stride = 20;
		source.photometric = dicom::pixel::Photometric::monochrome2;
		set_pixel_data_file.set_pixel_data(
		    "ExplicitVRLittleEndian"_uid, source);

		const auto& pixel_data = set_pixel_data_file.get_dataelement("PixelData"_tag);
		if (pixel_data.is_missing()) {
			fail("set_pixel_data should create PixelData");
		}
		if (pixel_data.vr() != dicom::VR::OW) {
			fail("set_pixel_data should create OW for 16-bit source");
		}
		const auto pixel_bytes = pixel_data.value_span();
		const std::vector<std::uint8_t> expected{
		    0x01u, 0x00u, 0x02u, 0x00u, 0x03u, 0x00u,
		    0x04u, 0x00u, 0x05u, 0x00u, 0x06u, 0x00u,
		    0x11u, 0x00u, 0x12u, 0x00u, 0x13u, 0x00u,
		    0x14u, 0x00u, 0x15u, 0x00u, 0x16u, 0x00u};
		if (pixel_bytes.size() != expected.size()) {
			fail("set_pixel_data pixel byte size mismatch");
		}
		for (std::size_t i = 0; i < expected.size(); ++i) {
			if (pixel_bytes[i] != expected[i]) {
				fail("set_pixel_data should strip row/frame padding and keep payload bytes");
			}
		}

		if (set_pixel_data_file["Rows"_tag].to_long().value_or(0) != 2) {
			fail("set_pixel_data should update Rows");
		}
		if (set_pixel_data_file["Columns"_tag].to_long().value_or(0) != 3) {
			fail("set_pixel_data should update Columns");
		}
		if (set_pixel_data_file["SamplesPerPixel"_tag].to_long().value_or(0) != 1) {
			fail("set_pixel_data should update SamplesPerPixel");
		}
		if (set_pixel_data_file["BitsAllocated"_tag].to_long().value_or(0) != 16) {
			fail("set_pixel_data should update BitsAllocated");
		}
		if (set_pixel_data_file["BitsStored"_tag].to_long().value_or(0) != 16) {
			fail("set_pixel_data should update BitsStored");
		}
		if (set_pixel_data_file["HighBit"_tag].to_long().value_or(-1) != 15) {
			fail("set_pixel_data should update HighBit");
		}
		if (set_pixel_data_file["PixelRepresentation"_tag].to_long().value_or(-1) != 0) {
			fail("set_pixel_data should update PixelRepresentation");
		}
		if (set_pixel_data_file["PhotometricInterpretation"_tag].to_string_view().value_or("") !=
		    "MONOCHROME2") {
			fail("set_pixel_data should update PhotometricInterpretation");
		}
		if (set_pixel_data_file["NumberOfFrames"_tag].to_long().value_or(0) != 2) {
			fail("set_pixel_data should update NumberOfFrames");
		}
		if (set_pixel_data_file["PlanarConfiguration"_tag].is_present()) {
			fail("set_pixel_data should remove PlanarConfiguration for single-sample data");
		}
			if (!set_pixel_data_file.transfer_syntax_uid().valid() ||
			    set_pixel_data_file.transfer_syntax_uid().value() !=
			        "ExplicitVRLittleEndian"_uid.value()) {
				fail("set_pixel_data should update runtime transfer syntax");
			}
			const auto& transfer_syntax_elem =
			    set_pixel_data_file.get_dataelement("TransferSyntaxUID"_tag);
			const auto ts_uid = transfer_syntax_elem.to_transfer_syntax_uid();
			if (!ts_uid || ts_uid->value() != "ExplicitVRLittleEndian"_uid.value()) {
				fail("set_pixel_data should synchronize TransferSyntaxUID in file meta");
			}
			if (set_pixel_data_file["LossyImageCompression"_tag].to_string_view().value_or("") !=
			    std::string_view("00")) {
				fail("set_pixel_data native uncompressed should set LossyImageCompression to 00");
			}
			if (set_pixel_data_file["LossyImageCompressionRatio"_tag].is_present()) {
				fail("set_pixel_data native uncompressed should remove LossyImageCompressionRatio");
			}
			if (set_pixel_data_file["LossyImageCompressionMethod"_tag].is_present()) {
				fail("set_pixel_data native uncompressed should remove LossyImageCompressionMethod");
			}

			bool unsupported_codec_threw = false;
			try {
				const std::array<dicom::pixel::CodecOptionTextKv, 1> invalid_codec_options{{
				    {"", "1"},
				}};
				set_pixel_data_file.set_pixel_data(
				    "ExplicitVRLittleEndian"_uid, source,
				    std::span<const dicom::pixel::CodecOptionTextKv>(invalid_codec_options));
			} catch (const std::exception&) {
				unsupported_codec_threw = true;
			}
			if (!unsupported_codec_threw) {
				fail("set_pixel_data should reject empty codec option key");
			}
	}
		dicom::DicomFile set_pixel_data_rle_file;
		{
			std::vector<std::uint8_t> source_bytes(40, 0xEEu);
		const auto write_row = [&](std::size_t frame, std::size_t row,
		                       std::array<std::uint8_t, 6> payload) {
			const std::size_t frame_stride = 20;
			const std::size_t row_stride = 8;
			const std::size_t base = frame * frame_stride + row * row_stride;
			for (std::size_t i = 0; i < payload.size(); ++i) {
				source_bytes[base + i] = payload[i];
			}
		};
		write_row(0, 0, {0x01u, 0x00u, 0x02u, 0x00u, 0x03u, 0x00u});
		write_row(0, 1, {0x04u, 0x00u, 0x05u, 0x00u, 0x06u, 0x00u});
		write_row(1, 0, {0x11u, 0x00u, 0x12u, 0x00u, 0x13u, 0x00u});
		write_row(1, 1, {0x14u, 0x00u, 0x15u, 0x00u, 0x16u, 0x00u});

		dicom::pixel::PixelSource source{};
		source.bytes = std::span<const std::uint8_t>(source_bytes.data(), source_bytes.size());
		source.data_type = dicom::pixel::DataType::u16;
		source.rows = 2;
		source.cols = 3;
		source.frames = 2;
		source.samples_per_pixel = 1;
		source.row_stride = 8;
		source.frame_stride = 20;
		source.photometric = dicom::pixel::Photometric::monochrome2;
		set_pixel_data_rle_file.set_pixel_data(
		    "RLELossless"_uid, source);

		const auto& pixel_data = set_pixel_data_rle_file.get_dataelement("PixelData"_tag);
		if (pixel_data.is_missing()) {
			fail("RLE set_pixel_data should create PixelData");
		}
		if (!pixel_data.vr().is_pixel_sequence()) {
			fail("RLE set_pixel_data should create encapsulated PixelData");
		}
		const auto* pixel_sequence = pixel_data.as_pixel_sequence();
		if (!pixel_sequence || pixel_sequence->number_of_frames() != 2) {
			fail("RLE set_pixel_data should create expected frame count");
		}
		if (!set_pixel_data_rle_file.transfer_syntax_uid().valid() ||
		    set_pixel_data_rle_file.transfer_syntax_uid().value() !=
		        "RLELossless"_uid.value()) {
			fail("RLE set_pixel_data should update runtime transfer syntax");
		}
		const auto& transfer_syntax_elem =
		    set_pixel_data_rle_file.get_dataelement("TransferSyntaxUID"_tag);
		const auto ts_uid = transfer_syntax_elem.to_transfer_syntax_uid();
		if (!ts_uid || ts_uid->value() != "RLELossless"_uid.value()) {
			fail("RLE set_pixel_data should synchronize TransferSyntaxUID in file meta");
		}

		const std::vector<std::uint8_t> expected_frame0{
		    0x01u, 0x00u, 0x02u, 0x00u, 0x03u, 0x00u,
		    0x04u, 0x00u, 0x05u, 0x00u, 0x06u, 0x00u};
		const std::vector<std::uint8_t> expected_frame1{
		    0x11u, 0x00u, 0x12u, 0x00u, 0x13u, 0x00u,
		    0x14u, 0x00u, 0x15u, 0x00u, 0x16u, 0x00u};
			const auto decoded_frame0 = set_pixel_data_rle_file.pixel_data(0);
			const auto decoded_frame1 = set_pixel_data_rle_file.pixel_data(1);
			if (decoded_frame0 != expected_frame0) {
				fail("RLE set_pixel_data frame #0 roundtrip mismatch");
			}
			if (decoded_frame1 != expected_frame1) {
				fail("RLE set_pixel_data frame #1 roundtrip mismatch");
			}

			const auto rle_encoder_context =
			    dicom::pixel::create_encoder_context("RLELossless"_uid);
			dicom::DicomFile set_pixel_data_with_context_file;
			set_pixel_data_with_context_file.set_pixel_data(
			    "RLELossless"_uid, source, rle_encoder_context);
			if (set_pixel_data_with_context_file.pixel_data(0) != expected_frame0) {
				fail("set_pixel_data with reusable encoder context frame #0 mismatch");
			}
			if (set_pixel_data_with_context_file.pixel_data(1) != expected_frame1) {
				fail("set_pixel_data with reusable encoder context frame #1 mismatch");
			}

			dicom::DicomFile transcode_with_context_file;
			transcode_with_context_file.set_pixel_data(
			    "ExplicitVRLittleEndian"_uid, source);
			transcode_with_context_file.set_transfer_syntax(
			    "RLELossless"_uid, rle_encoder_context);
			const auto& transcoded_pixel_data =
			    transcode_with_context_file.get_dataelement("PixelData"_tag);
			if (!transcoded_pixel_data.vr().is_pixel_sequence()) {
				fail("set_transfer_syntax with encoder context should produce encapsulated PixelData");
			}

			bool context_mismatch_threw = false;
			try {
				transcode_with_context_file.set_transfer_syntax(
				    "JPEG2000Lossless"_uid, rle_encoder_context);
			} catch (const std::exception&) {
				context_mismatch_threw = true;
			}
			if (!context_mismatch_threw) {
				fail("set_transfer_syntax should reject encoder context transfer syntax mismatch");
			}

			std::atomic<bool> worker_failed{false};
			std::string worker_failure_message{};
			std::mutex worker_failure_mutex{};
			const auto worker_run = [&]() {
				try {
					auto worker_context =
					    dicom::pixel::create_encoder_context("RLELossless"_uid);
					for (int iteration = 0; iteration < 4; ++iteration) {
						dicom::DicomFile worker_file;
						worker_file.set_pixel_data("RLELossless"_uid, source, worker_context);
						if (worker_file.pixel_data(0) != expected_frame0 ||
						    worker_file.pixel_data(1) != expected_frame1) {
							throw std::runtime_error(
							    "worker context reuse roundtrip mismatch");
						}
					}
				} catch (const std::exception& e) {
					worker_failed.store(true);
					std::scoped_lock lock(worker_failure_mutex);
					worker_failure_message = e.what();
				}
			};

			std::thread worker0(worker_run);
			std::thread worker1(worker_run);
			worker0.join();
			worker1.join();
			if (worker_failed.load()) {
				fail("worker-local encoder context reuse failed: " + worker_failure_message);
			}
		}
	dicom::DicomFile set_pixel_data_encap_uncompressed_file;
	{
		std::vector<std::uint8_t> source_bytes(40, 0xEEu);
		const auto write_row = [&](std::size_t frame, std::size_t row,
		                       std::array<std::uint8_t, 6> payload) {
			const std::size_t frame_stride = 20;
			const std::size_t row_stride = 8;
			const std::size_t base = frame * frame_stride + row * row_stride;
			for (std::size_t i = 0; i < payload.size(); ++i) {
				source_bytes[base + i] = payload[i];
			}
		};
		write_row(0, 0, {0x01u, 0x00u, 0x02u, 0x00u, 0x03u, 0x00u});
		write_row(0, 1, {0x04u, 0x00u, 0x05u, 0x00u, 0x06u, 0x00u});
		write_row(1, 0, {0x11u, 0x00u, 0x12u, 0x00u, 0x13u, 0x00u});
		write_row(1, 1, {0x14u, 0x00u, 0x15u, 0x00u, 0x16u, 0x00u});

		dicom::pixel::PixelSource source{};
		source.bytes = std::span<const std::uint8_t>(source_bytes.data(), source_bytes.size());
		source.data_type = dicom::pixel::DataType::u16;
		source.rows = 2;
		source.cols = 3;
		source.frames = 2;
		source.samples_per_pixel = 1;
		source.row_stride = 8;
		source.frame_stride = 20;
		source.photometric = dicom::pixel::Photometric::monochrome2;
		set_pixel_data_encap_uncompressed_file.set_pixel_data(
		    "EncapsulatedUncompressedExplicitVRLittleEndian"_uid, source);

		auto& pixel_data = set_pixel_data_encap_uncompressed_file.get_dataelement("PixelData"_tag);
		if (pixel_data.is_missing()) {
			fail("EncapsulatedUncompressed set_pixel_data should create PixelData");
		}
		if (!pixel_data.vr().is_pixel_sequence()) {
			fail("EncapsulatedUncompressed set_pixel_data should create encapsulated PixelData");
		}
		auto* pixel_sequence = pixel_data.as_pixel_sequence();
		if (!pixel_sequence || pixel_sequence->number_of_frames() != 2) {
			fail("EncapsulatedUncompressed set_pixel_data should create expected frame count");
		}
		const std::vector<std::uint8_t> expected_frame0{
		    0x01u, 0x00u, 0x02u, 0x00u, 0x03u, 0x00u,
		    0x04u, 0x00u, 0x05u, 0x00u, 0x06u, 0x00u};
		const std::vector<std::uint8_t> expected_frame1{
		    0x11u, 0x00u, 0x12u, 0x00u, 0x13u, 0x00u,
		    0x14u, 0x00u, 0x15u, 0x00u, 0x16u, 0x00u};
		const auto frame0_encoded = pixel_sequence->frame_encoded_span(0);
		const auto frame1_encoded = pixel_sequence->frame_encoded_span(1);
		if (frame0_encoded.size() != expected_frame0.size() ||
		    !std::equal(frame0_encoded.begin(), frame0_encoded.end(), expected_frame0.begin())) {
			fail("EncapsulatedUncompressed set_pixel_data frame #0 payload mismatch");
		}
		if (frame1_encoded.size() != expected_frame1.size() ||
		    !std::equal(frame1_encoded.begin(), frame1_encoded.end(), expected_frame1.begin())) {
			fail("EncapsulatedUncompressed set_pixel_data frame #1 payload mismatch");
		}
		if (!set_pixel_data_encap_uncompressed_file.transfer_syntax_uid().valid() ||
		    set_pixel_data_encap_uncompressed_file.transfer_syntax_uid().value() !=
		        "EncapsulatedUncompressedExplicitVRLittleEndian"_uid.value()) {
			fail("EncapsulatedUncompressed set_pixel_data should update runtime transfer syntax");
		}
		const auto& transfer_syntax_elem =
		    set_pixel_data_encap_uncompressed_file.get_dataelement("TransferSyntaxUID"_tag);
		const auto ts_uid = transfer_syntax_elem.to_transfer_syntax_uid();
		if (!ts_uid ||
		    ts_uid->value() != "EncapsulatedUncompressedExplicitVRLittleEndian"_uid.value()) {
			fail("EncapsulatedUncompressed set_pixel_data should synchronize TransferSyntaxUID in file meta");
		}

		const auto decoded_frame0 = set_pixel_data_encap_uncompressed_file.pixel_data(0);
		const auto decoded_frame1 = set_pixel_data_encap_uncompressed_file.pixel_data(1);
		if (decoded_frame0 != expected_frame0) {
			fail("EncapsulatedUncompressed set_pixel_data frame #0 roundtrip mismatch");
		}
		if (decoded_frame1 != expected_frame1) {
			fail("EncapsulatedUncompressed set_pixel_data frame #1 roundtrip mismatch");
		}
	}
	if (dicom::test::kJpeg2kBuiltin) {
		dicom::DicomFile set_pixel_data_j2k_file;
		{
		std::vector<std::uint8_t> source_bytes(40, 0xEEu);
		const auto write_row = [&](std::size_t frame, std::size_t row,
		                       std::array<std::uint8_t, 6> payload) {
			const std::size_t frame_stride = 20;
			const std::size_t row_stride = 8;
			const std::size_t base = frame * frame_stride + row * row_stride;
			for (std::size_t i = 0; i < payload.size(); ++i) {
				source_bytes[base + i] = payload[i];
			}
		};
		write_row(0, 0, {0x01u, 0x00u, 0x02u, 0x00u, 0x03u, 0x00u});
		write_row(0, 1, {0x04u, 0x00u, 0x05u, 0x00u, 0x06u, 0x00u});
		write_row(1, 0, {0x11u, 0x00u, 0x12u, 0x00u, 0x13u, 0x00u});
		write_row(1, 1, {0x14u, 0x00u, 0x15u, 0x00u, 0x16u, 0x00u});

		dicom::pixel::PixelSource source{};
		source.bytes = std::span<const std::uint8_t>(source_bytes.data(), source_bytes.size());
		source.data_type = dicom::pixel::DataType::u16;
		source.rows = 2;
		source.cols = 3;
		source.frames = 2;
		source.samples_per_pixel = 1;
		source.row_stride = 8;
		source.frame_stride = 20;
		source.photometric = dicom::pixel::Photometric::monochrome2;
		set_pixel_data_j2k_file.set_pixel_data(
		    "JPEG2000Lossless"_uid, source);

		const auto& pixel_data = set_pixel_data_j2k_file.get_dataelement("PixelData"_tag);
		if (pixel_data.is_missing()) {
			fail("J2K set_pixel_data should create PixelData");
		}
		if (!pixel_data.vr().is_pixel_sequence()) {
			fail("J2K set_pixel_data should create encapsulated PixelData");
		}
		const auto* pixel_sequence = pixel_data.as_pixel_sequence();
		if (!pixel_sequence || pixel_sequence->number_of_frames() != 2) {
			fail("J2K set_pixel_data should create expected frame count");
		}
		if (!set_pixel_data_j2k_file.transfer_syntax_uid().valid() ||
		    set_pixel_data_j2k_file.transfer_syntax_uid().value() !=
		        "JPEG2000Lossless"_uid.value()) {
			fail("J2K set_pixel_data should update runtime transfer syntax");
		}
		const auto& transfer_syntax_elem =
		    set_pixel_data_j2k_file.get_dataelement("TransferSyntaxUID"_tag);
		const auto ts_uid = transfer_syntax_elem.to_transfer_syntax_uid();
		if (!ts_uid || ts_uid->value() != "JPEG2000Lossless"_uid.value()) {
			fail("J2K set_pixel_data should synchronize TransferSyntaxUID in file meta");
		}

		const std::vector<std::uint8_t> expected_frame0{
		    0x01u, 0x00u, 0x02u, 0x00u, 0x03u, 0x00u,
		    0x04u, 0x00u, 0x05u, 0x00u, 0x06u, 0x00u};
		const std::vector<std::uint8_t> expected_frame1{
		    0x11u, 0x00u, 0x12u, 0x00u, 0x13u, 0x00u,
		    0x14u, 0x00u, 0x15u, 0x00u, 0x16u, 0x00u};
		const auto decoded_frame0 = set_pixel_data_j2k_file.pixel_data(0);
		const auto decoded_frame1 = set_pixel_data_j2k_file.pixel_data(1);
		if (decoded_frame0 != expected_frame0) {
			fail("J2K set_pixel_data frame #0 roundtrip mismatch");
		}
		if (decoded_frame1 != expected_frame1) {
			fail("J2K set_pixel_data frame #1 roundtrip mismatch");
		}
		}
	}
	if (dicom::test::kHtj2kBuiltin) {
		dicom::DicomFile set_pixel_data_htj2k_file;
		{
		std::vector<std::uint8_t> source_bytes(40, 0xEEu);
		const auto write_row = [&](std::size_t frame, std::size_t row,
		                       std::array<std::uint8_t, 6> payload) {
			const std::size_t frame_stride = 20;
			const std::size_t row_stride = 8;
			const std::size_t base = frame * frame_stride + row * row_stride;
			for (std::size_t i = 0; i < payload.size(); ++i) {
				source_bytes[base + i] = payload[i];
			}
		};
		write_row(0, 0, {0x01u, 0x00u, 0x02u, 0x00u, 0x03u, 0x00u});
		write_row(0, 1, {0x04u, 0x00u, 0x05u, 0x00u, 0x06u, 0x00u});
		write_row(1, 0, {0x11u, 0x00u, 0x12u, 0x00u, 0x13u, 0x00u});
		write_row(1, 1, {0x14u, 0x00u, 0x15u, 0x00u, 0x16u, 0x00u});

		dicom::pixel::PixelSource source{};
		source.bytes = std::span<const std::uint8_t>(source_bytes.data(), source_bytes.size());
		source.data_type = dicom::pixel::DataType::u16;
		source.rows = 2;
		source.cols = 3;
		source.frames = 2;
		source.samples_per_pixel = 1;
		source.row_stride = 8;
		source.frame_stride = 20;
		source.photometric = dicom::pixel::Photometric::monochrome2;
		set_pixel_data_htj2k_file.set_pixel_data(
		    "HTJ2KLossless"_uid, source);

		const auto& pixel_data = set_pixel_data_htj2k_file.get_dataelement("PixelData"_tag);
		if (pixel_data.is_missing()) {
			fail("HTJ2K set_pixel_data should create PixelData");
		}
		if (!pixel_data.vr().is_pixel_sequence()) {
			fail("HTJ2K set_pixel_data should create encapsulated PixelData");
		}
		const auto* pixel_sequence = pixel_data.as_pixel_sequence();
		if (!pixel_sequence || pixel_sequence->number_of_frames() != 2) {
			fail("HTJ2K set_pixel_data should create expected frame count");
		}
		if (!set_pixel_data_htj2k_file.transfer_syntax_uid().valid() ||
		    set_pixel_data_htj2k_file.transfer_syntax_uid().value() !=
		        "HTJ2KLossless"_uid.value()) {
			fail("HTJ2K set_pixel_data should update runtime transfer syntax");
		}
		const auto& transfer_syntax_elem =
		    set_pixel_data_htj2k_file.get_dataelement("TransferSyntaxUID"_tag);
		const auto ts_uid = transfer_syntax_elem.to_transfer_syntax_uid();
		if (!ts_uid || ts_uid->value() != "HTJ2KLossless"_uid.value()) {
			fail("HTJ2K set_pixel_data should synchronize TransferSyntaxUID in file meta");
		}

		const std::vector<std::uint8_t> expected_frame0{
		    0x01u, 0x00u, 0x02u, 0x00u, 0x03u, 0x00u,
		    0x04u, 0x00u, 0x05u, 0x00u, 0x06u, 0x00u};
		const std::vector<std::uint8_t> expected_frame1{
		    0x11u, 0x00u, 0x12u, 0x00u, 0x13u, 0x00u,
		    0x14u, 0x00u, 0x15u, 0x00u, 0x16u, 0x00u};
		const auto decoded_frame0 = set_pixel_data_htj2k_file.pixel_data(0);
		const auto decoded_frame1 = set_pixel_data_htj2k_file.pixel_data(1);
		if (decoded_frame0 != expected_frame0) {
			fail("HTJ2K set_pixel_data frame #0 roundtrip mismatch");
		}
		if (decoded_frame1 != expected_frame1) {
			fail("HTJ2K set_pixel_data frame #1 roundtrip mismatch");
		}
		}
	}
		{
			const std::vector<std::uint8_t> rgb_source{
			    0x10u, 0x20u, 0x30u, 0x40u, 0x50u, 0x60u,
			    0x70u, 0x80u, 0x90u, 0xA0u, 0xB0u, 0xC0u};
		dicom::pixel::PixelSource color_source{};
		color_source.bytes =
		    std::span<const std::uint8_t>(rgb_source.data(), rgb_source.size());
		color_source.data_type = dicom::pixel::DataType::u8;
		color_source.rows = 2;
		color_source.cols = 2;
		color_source.frames = 1;
			color_source.samples_per_pixel = 3;
			color_source.planar = dicom::pixel::Planar::interleaved;
			color_source.photometric = dicom::pixel::Photometric::rgb;

			std::vector<std::uint8_t> lossy_quality_bytes(64 * 64 * 3, 0u);
			for (std::size_t row = 0; row < 64; ++row) {
				for (std::size_t col = 0; col < 64; ++col) {
					const std::size_t base = (row * 64 + col) * 3;
					lossy_quality_bytes[base + 0] =
					    static_cast<std::uint8_t>((row * 3 + col) & 0xFFu);
					lossy_quality_bytes[base + 1] =
					    static_cast<std::uint8_t>((row * 5 + col * 2) & 0xFFu);
					lossy_quality_bytes[base + 2] =
					    static_cast<std::uint8_t>((row * 7 + col * 4) & 0xFFu);
				}
			}
			dicom::pixel::PixelSource lossy_quality_source{};
			lossy_quality_source.bytes = std::span<const std::uint8_t>(
			    lossy_quality_bytes.data(), lossy_quality_bytes.size());
			lossy_quality_source.data_type = dicom::pixel::DataType::u8;
			lossy_quality_source.rows = 64;
			lossy_quality_source.cols = 64;
			lossy_quality_source.frames = 1;
			lossy_quality_source.samples_per_pixel = 3;
			lossy_quality_source.planar = dicom::pixel::Planar::interleaved;
			lossy_quality_source.photometric = dicom::pixel::Photometric::rgb;

			const auto first_encoded_frame_size = [&fail](const dicom::DicomFile& file,
			                                     std::string_view label) -> std::size_t {
					const auto& pixel_data = file.get_dataelement("PixelData"_tag);
					if (pixel_data.is_missing() || !pixel_data.vr().is_pixel_sequence()) {
						fail(std::string(label) + " should create encapsulated PixelData");
					}
					const auto* pixel_sequence = pixel_data.as_pixel_sequence();
					if (!pixel_sequence || pixel_sequence->number_of_frames() != 1) {
						fail(std::string(label) + " should create exactly one frame");
					}
					const auto* frame = pixel_sequence->frame(0);
					if (!frame) {
						fail(std::string(label) + " is missing frame #0");
					}
					return frame->encoded_data_size();
				};

		if (dicom::test::kJpeg2kBuiltin) {
			dicom::DicomFile j2k_default_mct;
			j2k_default_mct.set_pixel_data(
			    "JPEG2000Lossless"_uid, color_source);
			if (j2k_default_mct["PhotometricInterpretation"_tag].to_string_view().value_or("") !=
			    std::string_view("YBR_RCT")) {
				fail("J2K default color transform should update PhotometricInterpretation to YBR_RCT");
			}

			const std::array<dicom::pixel::CodecOptionTextKv, 1> j2k_without_mct_text_options{{
			    {"color_transform", "false"},
			}};
			dicom::DicomFile j2k_no_mct;
			j2k_no_mct.set_pixel_data("JPEG2000Lossless"_uid, color_source,
			    std::span<const dicom::pixel::CodecOptionTextKv>(j2k_without_mct_text_options));
			if (j2k_no_mct["PhotometricInterpretation"_tag].to_string_view().value_or("") !=
			    std::string_view("RGB")) {
				fail("J2K disabled color transform should keep PhotometricInterpretation as RGB");
			}
			dicom::DicomFile j2k_no_mct_text;
			j2k_no_mct_text.set_pixel_data(
		    "JPEG2000Lossless"_uid, color_source,
		    std::span<const dicom::pixel::CodecOptionTextKv>(j2k_without_mct_text_options));
		if (j2k_no_mct_text["PhotometricInterpretation"_tag].to_string_view().value_or("") !=
		    std::string_view("RGB")) {
			fail("J2K text options should keep PhotometricInterpretation as RGB");
		}

		bool rejected_j2k_mc_without_mct = false;
			try {
				dicom::DicomFile j2k_mc_no_mct;
				j2k_mc_no_mct.set_pixel_data(
				    "JPEG2000MCLossless"_uid, color_source,
				    std::span<const dicom::pixel::CodecOptionTextKv>(j2k_without_mct_text_options));
			} catch (const std::exception&) {
				rejected_j2k_mc_without_mct = true;
			}
		if (!rejected_j2k_mc_without_mct) {
			fail("JPEG2000MCLossless should reject use_color_transform=false");
		}

				const std::array<dicom::pixel::CodecOptionTextKv, 1> j2k_lossy_options{{
				    {"target_psnr", "45"},
				}};
				dicom::DicomFile j2k_lossy_mct;
				j2k_lossy_mct.set_pixel_data("JPEG2000"_uid, color_source,
				    std::span<const dicom::pixel::CodecOptionTextKv>(j2k_lossy_options));
			if (j2k_lossy_mct["PhotometricInterpretation"_tag].to_string_view().value_or("") !=
			    std::string_view("YBR_ICT")) {
				fail("J2K lossy color transform should update PhotometricInterpretation to YBR_ICT");
			}
			if (j2k_lossy_mct["LossyImageCompression"_tag].to_string_view().value_or("") !=
			    std::string_view("01")) {
				fail("J2K lossy set_pixel_data should set LossyImageCompression to 01");
			}
			const auto j2k_lossy_methods =
			    j2k_lossy_mct["LossyImageCompressionMethod"_tag].to_string_views();
			if (!j2k_lossy_methods || j2k_lossy_methods->size() != 1 ||
			    (*j2k_lossy_methods)[0] != std::string_view("ISO_15444_1")) {
				fail("J2K lossy set_pixel_data should set LossyImageCompressionMethod=ISO_15444_1");
			}
			const auto j2k_lossy_ratios =
			    j2k_lossy_mct["LossyImageCompressionRatio"_tag].to_double_vector();
			if (!j2k_lossy_ratios || j2k_lossy_ratios->size() != 1 ||
			    !((*j2k_lossy_ratios)[0] > 0.0)) {
				fail("J2K lossy set_pixel_data should set a positive LossyImageCompressionRatio");
			}
			j2k_lossy_mct.set_transfer_syntax("ExplicitVRLittleEndian"_uid);
			if (j2k_lossy_mct["LossyImageCompression"_tag].to_string_view().value_or("") !=
			    std::string_view("01")) {
				fail("LossyImageCompression should remain 01 after lossy-to-native conversion");
			}
			j2k_lossy_mct.set_transfer_syntax("JPEG2000"_uid);
			const auto j2k_lossy_methods_twice =
			    j2k_lossy_mct["LossyImageCompressionMethod"_tag].to_string_views();
			if (!j2k_lossy_methods_twice || j2k_lossy_methods_twice->size() != 2 ||
			    (*j2k_lossy_methods_twice)[0] != std::string_view("ISO_15444_1") ||
			    (*j2k_lossy_methods_twice)[1] != std::string_view("ISO_15444_1")) {
				fail("Repeated J2K lossy encode should append LossyImageCompressionMethod");
			}
			const auto j2k_lossy_ratios_twice =
			    j2k_lossy_mct["LossyImageCompressionRatio"_tag].to_double_vector();
			if (!j2k_lossy_ratios_twice || j2k_lossy_ratios_twice->size() != 2 ||
			    !((*j2k_lossy_ratios_twice)[0] > 0.0) ||
			    !((*j2k_lossy_ratios_twice)[1] > 0.0)) {
				fail("Repeated J2K lossy encode should append positive LossyImageCompressionRatio values");
			}

			dicom::DicomFile j2k_lossy_text_ts;
			j2k_lossy_text_ts.set_pixel_data(
			    "ExplicitVRLittleEndian"_uid, color_source);
			const std::array<dicom::pixel::CodecOptionTextKv, 2> j2k_lossy_text_options{{
			    {"target_psnr", "45"},
			    {"color_transform", "true"},
			}};
			j2k_lossy_text_ts.set_transfer_syntax(
			    "JPEG2000"_uid,
			    std::span<const dicom::pixel::CodecOptionTextKv>(j2k_lossy_text_options));
			if (j2k_lossy_text_ts["PhotometricInterpretation"_tag].to_string_view().value_or("") !=
			    std::string_view("YBR_ICT")) {
				fail("J2K text options via set_transfer_syntax should update PhotometricInterpretation to YBR_ICT");
			}
				if (j2k_lossy_text_ts["LossyImageCompression"_tag].to_string_view().value_or("") !=
				    std::string_view("01")) {
					fail("J2K text options via set_transfer_syntax should set LossyImageCompression to 01");
				}

				const std::array<dicom::pixel::CodecOptionTextKv, 1> j2k_lossy_psnr30{{
				    {"target_psnr", "30"},
				}};
				const std::array<dicom::pixel::CodecOptionTextKv, 1> j2k_lossy_psnr50{{
				    {"target_psnr", "50"},
				}};
				dicom::DicomFile j2k_psnr30;
				j2k_psnr30.set_pixel_data("JPEG2000"_uid, lossy_quality_source,
				    std::span<const dicom::pixel::CodecOptionTextKv>(j2k_lossy_psnr30));
				dicom::DicomFile j2k_psnr50;
				j2k_psnr50.set_pixel_data("JPEG2000"_uid, lossy_quality_source,
				    std::span<const dicom::pixel::CodecOptionTextKv>(j2k_lossy_psnr50));
				const auto j2k_size_psnr30 =
				    first_encoded_frame_size(j2k_psnr30, "J2K target_psnr=30");
				const auto j2k_size_psnr50 =
				    first_encoded_frame_size(j2k_psnr50, "J2K target_psnr=50");
				if (j2k_size_psnr50 <= j2k_size_psnr30) {
					fail("Higher JPEG2000 target_psnr should produce a larger encoded frame");
				}

				const std::array<dicom::pixel::CodecOptionTextKv, 1> j2k_lossy_bpp1{{
				    {"target_bpp", "1.0"},
				}};
				const std::array<dicom::pixel::CodecOptionTextKv, 1> j2k_lossy_bpp4{{
				    {"target_bpp", "4.0"},
				}};
				dicom::DicomFile j2k_bpp1;
				j2k_bpp1.set_pixel_data("JPEG2000"_uid, lossy_quality_source,
				    std::span<const dicom::pixel::CodecOptionTextKv>(j2k_lossy_bpp1));
				dicom::DicomFile j2k_bpp4;
				j2k_bpp4.set_pixel_data("JPEG2000"_uid, lossy_quality_source,
				    std::span<const dicom::pixel::CodecOptionTextKv>(j2k_lossy_bpp4));
				const auto j2k_size_bpp1 =
				    first_encoded_frame_size(j2k_bpp1, "J2K target_bpp=1.0");
				const auto j2k_size_bpp4 =
				    first_encoded_frame_size(j2k_bpp4, "J2K target_bpp=4.0");
				if (j2k_size_bpp4 <= j2k_size_bpp1) {
					fail("Higher JPEG2000 target_bpp should produce a larger encoded frame");
				}
			}

			if (dicom::test::kHtj2kBuiltin) {
			dicom::DicomFile htj2k_default_mct;
			htj2k_default_mct.set_pixel_data(
			    "HTJ2KLossless"_uid, color_source);
			if (htj2k_default_mct["PhotometricInterpretation"_tag].to_string_view().value_or("") !=
			    std::string_view("YBR_RCT")) {
				fail("HTJ2K default color transform should update PhotometricInterpretation to YBR_RCT");
			}

			const std::array<dicom::pixel::CodecOptionTextKv, 1> htj2k_without_mct_options{{
			    {"color_transform", "false"},
			}};
			dicom::DicomFile htj2k_no_mct;
			htj2k_no_mct.set_pixel_data("HTJ2KLossless"_uid, color_source,
			    std::span<const dicom::pixel::CodecOptionTextKv>(htj2k_without_mct_options));
			if (htj2k_no_mct["PhotometricInterpretation"_tag].to_string_view().value_or("") !=
			    std::string_view("RGB")) {
				fail("HTJ2K disabled color transform should keep PhotometricInterpretation as RGB");
			}

				const std::array<dicom::pixel::CodecOptionTextKv, 1> htj2k_lossy_options{{
				    {"target_psnr", "45"},
				}};
				dicom::DicomFile htj2k_lossy_mct;
				htj2k_lossy_mct.set_pixel_data("HTJ2K"_uid, color_source,
				    std::span<const dicom::pixel::CodecOptionTextKv>(htj2k_lossy_options));
			if (htj2k_lossy_mct["PhotometricInterpretation"_tag].to_string_view().value_or("") !=
			    std::string_view("YBR_ICT")) {
				fail("HTJ2K lossy color transform should update PhotometricInterpretation to YBR_ICT");
			}
			if (htj2k_lossy_mct["LossyImageCompression"_tag].to_string_view().value_or("") !=
			    std::string_view("01")) {
				fail("HTJ2K lossy set_pixel_data should set LossyImageCompression to 01");
			}
			const auto htj2k_lossy_methods =
			    htj2k_lossy_mct["LossyImageCompressionMethod"_tag].to_string_views();
			if (!htj2k_lossy_methods || htj2k_lossy_methods->size() != 1 ||
			    (*htj2k_lossy_methods)[0] != std::string_view("ISO_15444_15")) {
				fail("HTJ2K lossy set_pixel_data should set LossyImageCompressionMethod=ISO_15444_15");
			}
			const auto htj2k_lossy_ratios =
			    htj2k_lossy_mct["LossyImageCompressionRatio"_tag].to_double_vector();
				if (!htj2k_lossy_ratios || htj2k_lossy_ratios->size() != 1 ||
				    !((*htj2k_lossy_ratios)[0] > 0.0)) {
					fail("HTJ2K lossy set_pixel_data should set a positive LossyImageCompressionRatio");
				}

				const std::array<dicom::pixel::CodecOptionTextKv, 1> htj2k_lossy_psnr30{{
				    {"target_psnr", "30"},
				}};
				const std::array<dicom::pixel::CodecOptionTextKv, 1> htj2k_lossy_psnr50{{
				    {"target_psnr", "50"},
				}};
				dicom::DicomFile htj2k_psnr30;
				htj2k_psnr30.set_pixel_data("HTJ2K"_uid, lossy_quality_source,
				    std::span<const dicom::pixel::CodecOptionTextKv>(htj2k_lossy_psnr30));
				dicom::DicomFile htj2k_psnr50;
				htj2k_psnr50.set_pixel_data("HTJ2K"_uid, lossy_quality_source,
				    std::span<const dicom::pixel::CodecOptionTextKv>(htj2k_lossy_psnr50));
				const auto htj2k_size_psnr30 =
				    first_encoded_frame_size(htj2k_psnr30, "HTJ2K target_psnr=30");
				const auto htj2k_size_psnr50 =
				    first_encoded_frame_size(htj2k_psnr50, "HTJ2K target_psnr=50");
				if (htj2k_size_psnr50 <= htj2k_size_psnr30) {
					fail("Higher HTJ2K target_psnr should produce a larger encoded frame");
				}
			}
		}
	if (dicom::test::kJpegLsBuiltin) {
		dicom::DicomFile set_pixel_data_jpegls_file;
		{
		std::vector<std::uint8_t> source_bytes(40, 0xEEu);
		const auto write_row = [&](std::size_t frame, std::size_t row,
		                       std::array<std::uint8_t, 6> payload) {
			const std::size_t frame_stride = 20;
			const std::size_t row_stride = 8;
			const std::size_t base = frame * frame_stride + row * row_stride;
			for (std::size_t i = 0; i < payload.size(); ++i) {
				source_bytes[base + i] = payload[i];
			}
		};
		write_row(0, 0, {0x01u, 0x00u, 0x02u, 0x00u, 0x03u, 0x00u});
		write_row(0, 1, {0x04u, 0x00u, 0x05u, 0x00u, 0x06u, 0x00u});
		write_row(1, 0, {0x11u, 0x00u, 0x12u, 0x00u, 0x13u, 0x00u});
		write_row(1, 1, {0x14u, 0x00u, 0x15u, 0x00u, 0x16u, 0x00u});

		dicom::pixel::PixelSource source{};
		source.bytes = std::span<const std::uint8_t>(source_bytes.data(), source_bytes.size());
		source.data_type = dicom::pixel::DataType::u16;
		source.rows = 2;
		source.cols = 3;
		source.frames = 2;
		source.samples_per_pixel = 1;
		source.row_stride = 8;
		source.frame_stride = 20;
		source.photometric = dicom::pixel::Photometric::monochrome2;
		set_pixel_data_jpegls_file.set_pixel_data(
		    "JPEGLSLossless"_uid, source);

		const auto& pixel_data = set_pixel_data_jpegls_file.get_dataelement("PixelData"_tag);
		if (pixel_data.is_missing()) {
			fail("JPEG-LS set_pixel_data should create PixelData");
		}
		if (!pixel_data.vr().is_pixel_sequence()) {
			fail("JPEG-LS set_pixel_data should create encapsulated PixelData");
		}
		const auto* pixel_sequence = pixel_data.as_pixel_sequence();
		if (!pixel_sequence || pixel_sequence->number_of_frames() != 2) {
			fail("JPEG-LS set_pixel_data should create expected frame count");
		}
		if (!set_pixel_data_jpegls_file.transfer_syntax_uid().valid() ||
		    set_pixel_data_jpegls_file.transfer_syntax_uid().value() !=
		        "JPEGLSLossless"_uid.value()) {
			fail("JPEG-LS set_pixel_data should update runtime transfer syntax");
		}
		const auto& transfer_syntax_elem =
		    set_pixel_data_jpegls_file.get_dataelement("TransferSyntaxUID"_tag);
		const auto ts_uid = transfer_syntax_elem.to_transfer_syntax_uid();
		if (!ts_uid || ts_uid->value() != "JPEGLSLossless"_uid.value()) {
			fail("JPEG-LS set_pixel_data should synchronize TransferSyntaxUID in file meta");
		}

		const std::vector<std::uint8_t> expected_frame0{
		    0x01u, 0x00u, 0x02u, 0x00u, 0x03u, 0x00u,
		    0x04u, 0x00u, 0x05u, 0x00u, 0x06u, 0x00u};
		const std::vector<std::uint8_t> expected_frame1{
		    0x11u, 0x00u, 0x12u, 0x00u, 0x13u, 0x00u,
		    0x14u, 0x00u, 0x15u, 0x00u, 0x16u, 0x00u};
		const auto decoded_frame0 = set_pixel_data_jpegls_file.pixel_data(0);
		const auto decoded_frame1 = set_pixel_data_jpegls_file.pixel_data(1);
		if (decoded_frame0 != expected_frame0) {
			fail("JPEG-LS set_pixel_data frame #0 roundtrip mismatch");
		}
		if (decoded_frame1 != expected_frame1) {
			fail("JPEG-LS set_pixel_data frame #1 roundtrip mismatch");
		}
		}
	}
	if (dicom::test::kJpegBuiltin) {
		dicom::DicomFile set_pixel_data_jpeg_file;
		{
		std::vector<std::uint8_t> source_bytes(40, 0xEEu);
		const auto write_row = [&](std::size_t frame, std::size_t row,
		                       std::array<std::uint8_t, 6> payload) {
			const std::size_t frame_stride = 20;
			const std::size_t row_stride = 8;
			const std::size_t base = frame * frame_stride + row * row_stride;
			for (std::size_t i = 0; i < payload.size(); ++i) {
				source_bytes[base + i] = payload[i];
			}
		};
		write_row(0, 0, {0x01u, 0x00u, 0x02u, 0x00u, 0x03u, 0x00u});
		write_row(0, 1, {0x04u, 0x00u, 0x05u, 0x00u, 0x06u, 0x00u});
		write_row(1, 0, {0x11u, 0x00u, 0x12u, 0x00u, 0x13u, 0x00u});
		write_row(1, 1, {0x14u, 0x00u, 0x15u, 0x00u, 0x16u, 0x00u});

		dicom::pixel::PixelSource source{};
		source.bytes = std::span<const std::uint8_t>(source_bytes.data(), source_bytes.size());
		source.data_type = dicom::pixel::DataType::u16;
		source.rows = 2;
		source.cols = 3;
		source.frames = 2;
		source.samples_per_pixel = 1;
		source.row_stride = 8;
		source.frame_stride = 20;
		source.photometric = dicom::pixel::Photometric::monochrome2;
		set_pixel_data_jpeg_file.set_pixel_data(
		    "JPEGLosslessSV1"_uid, source);

		const auto& pixel_data = set_pixel_data_jpeg_file.get_dataelement("PixelData"_tag);
		if (pixel_data.is_missing()) {
			fail("JPEG set_pixel_data should create PixelData");
		}
		if (!pixel_data.vr().is_pixel_sequence()) {
			fail("JPEG set_pixel_data should create encapsulated PixelData");
		}
		const auto* pixel_sequence = pixel_data.as_pixel_sequence();
		if (!pixel_sequence || pixel_sequence->number_of_frames() != 2) {
			fail("JPEG set_pixel_data should create expected frame count");
		}
		if (!set_pixel_data_jpeg_file.transfer_syntax_uid().valid() ||
		    set_pixel_data_jpeg_file.transfer_syntax_uid().value() !=
		        "JPEGLosslessSV1"_uid.value()) {
			fail("JPEG set_pixel_data should update runtime transfer syntax");
		}
		const auto& transfer_syntax_elem =
		    set_pixel_data_jpeg_file.get_dataelement("TransferSyntaxUID"_tag);
		const auto ts_uid = transfer_syntax_elem.to_transfer_syntax_uid();
		if (!ts_uid || ts_uid->value() != "JPEGLosslessSV1"_uid.value()) {
			fail("JPEG set_pixel_data should synchronize TransferSyntaxUID in file meta");
		}

		const std::vector<std::uint8_t> expected_frame0{
		    0x01u, 0x00u, 0x02u, 0x00u, 0x03u, 0x00u,
		    0x04u, 0x00u, 0x05u, 0x00u, 0x06u, 0x00u};
		const std::vector<std::uint8_t> expected_frame1{
		    0x11u, 0x00u, 0x12u, 0x00u, 0x13u, 0x00u,
		    0x14u, 0x00u, 0x15u, 0x00u, 0x16u, 0x00u};
			const auto decoded_frame0 = set_pixel_data_jpeg_file.pixel_data(0);
			const auto decoded_frame1 = set_pixel_data_jpeg_file.pixel_data(1);
			if (decoded_frame0 != expected_frame0) {
				fail("JPEG set_pixel_data frame #0 roundtrip mismatch");
			}
			if (decoded_frame1 != expected_frame1) {
				fail("JPEG set_pixel_data frame #1 roundtrip mismatch");
			}
		}
	}
	if (dicom::test::kJpegLsBuiltin) {
		dicom::DicomFile set_pixel_data_jpegls_near_lossless_file;
		{
			const std::vector<std::uint8_t> source_bytes{
			    0x10u, 0x20u, 0x30u, 0x40u, 0x50u, 0x60u};
			dicom::pixel::PixelSource source{};
			source.bytes = std::span<const std::uint8_t>(source_bytes.data(), source_bytes.size());
			source.data_type = dicom::pixel::DataType::u8;
			source.rows = 2;
			source.cols = 3;
			source.frames = 1;
			source.samples_per_pixel = 1;
			source.photometric = dicom::pixel::Photometric::monochrome2;
			const std::array<dicom::pixel::CodecOptionTextKv, 1> options{{
			    {"near_lossless_error", "2"},
			}};
			set_pixel_data_jpegls_near_lossless_file.set_pixel_data(
			    "JPEGLSNearLossless"_uid, source,
			    std::span<const dicom::pixel::CodecOptionTextKv>(options));
			if (set_pixel_data_jpegls_near_lossless_file["LossyImageCompression"_tag]
			        .to_string_view()
			        .value_or("") != std::string_view("01")) {
				fail("JPEG-LS near-lossless should set LossyImageCompression to 01");
			}
			const auto methods = set_pixel_data_jpegls_near_lossless_file
			                         ["LossyImageCompressionMethod"_tag]
			                             .to_string_views();
			if (!methods || methods->size() != 1 ||
			    (*methods)[0] != std::string_view("ISO_14495_1")) {
				fail("JPEG-LS near-lossless should set LossyImageCompressionMethod=ISO_14495_1");
			}
			const auto ratios = set_pixel_data_jpegls_near_lossless_file
			                        ["LossyImageCompressionRatio"_tag]
			                            .to_double_vector();
			if (!ratios || ratios->size() != 1 || !((*ratios)[0] > 0.0)) {
				fail("JPEG-LS near-lossless should set a positive LossyImageCompressionRatio");
			}
		}
	}
	if (dicom::test::kJpegBuiltin) {
		dicom::DicomFile set_pixel_data_jpeg_lossy_file;
		{
			const std::vector<std::uint8_t> source_bytes{
			    0x12u, 0x34u, 0x56u, 0x78u, 0x9Au, 0xBCu};
			dicom::pixel::PixelSource source{};
			source.bytes = std::span<const std::uint8_t>(source_bytes.data(), source_bytes.size());
			source.data_type = dicom::pixel::DataType::u8;
			source.rows = 2;
			source.cols = 3;
			source.frames = 1;
			source.samples_per_pixel = 1;
			source.photometric = dicom::pixel::Photometric::monochrome2;
			const std::array<dicom::pixel::CodecOptionTextKv, 1> options{{
			    {"quality", "90"},
			}};
			set_pixel_data_jpeg_lossy_file.set_pixel_data(
			    "JPEGBaseline8Bit"_uid, source,
			    std::span<const dicom::pixel::CodecOptionTextKv>(options));
			if (set_pixel_data_jpeg_lossy_file["LossyImageCompression"_tag]
			        .to_string_view()
			        .value_or("") != std::string_view("01")) {
				fail("JPEG lossy should set LossyImageCompression to 01");
			}
			const auto methods =
			    set_pixel_data_jpeg_lossy_file["LossyImageCompressionMethod"_tag].to_string_views();
			if (!methods || methods->size() != 1 ||
			    (*methods)[0] != std::string_view("ISO_10918_1")) {
				fail("JPEG lossy should set LossyImageCompressionMethod=ISO_10918_1");
			}
			const auto ratios =
			    set_pixel_data_jpeg_lossy_file["LossyImageCompressionRatio"_tag].to_double_vector();
			if (!ratios || ratios->size() != 1 || !((*ratios)[0] > 0.0)) {
				fail("JPEG lossy should set a positive LossyImageCompressionRatio");
			}
		}
	}

	dicom::DicomFile generated;
	auto add_text_element = [&](dicom::Tag tag, dicom::VR vr, std::string_view value)
	    -> dicom::DataElement& {
		auto& element = generated.add_dataelement(tag, vr, 0, value.size());
		element.set_value_bytes(std::span<const std::uint8_t>(
		    reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
		return element;
	};

	add_text_element("SOPClassUID"_tag, dicom::VR::UI, "1.2.840.10008.5.1.4.1.1.7");
	add_text_element("SOPInstanceUID"_tag, dicom::VR::UI, dicom::uid::generate_sop_instance_uid().value());
	add_text_element("PatientName"_tag, dicom::VR::PN, "WRITE^ROUNDTRIP");

	auto& sequence_element =
	    generated.add_dataelement("ReferencedStudySequence"_tag, dicom::VR::SQ);
	if (!sequence_element) fail("failed to add sequence element");
	auto* sequence = sequence_element.as_sequence();
	if (!sequence) fail("sequence pointer is null");
	auto* sequence_item = sequence->add_dataset();
	if (!sequence_item) fail("failed to append sequence item");
	{
		auto& referenced_uid =
		    sequence_item->add_dataelement("ReferencedSOPInstanceUID"_tag, dicom::VR::UI, 0, 12);
		const std::array<std::uint8_t, 12> uid_value{
		    '1', '.', '2', '.', '3', '.', '4', '.', '5', '.', '6', '\0'};
		referenced_uid.set_value_bytes(uid_value);
	}

	generated.reset_encapsulated_pixel_data(1);
	generated.set_encoded_pixel_frame(
	    0, std::vector<std::uint8_t>{0x01, 0x02, 0x03, 0x04});
	if (generated["NumberOfFrames"_tag].to_long().value_or(0) != 1) {
		fail("reset_encapsulated_pixel_data(1) should set NumberOfFrames");
	}

	dicom::WriteOptions write_opts;
	const auto generated_bytes = generated.write_bytes(write_opts);
	if (generated_bytes.size() < 132) fail("write_bytes should include preamble + DICM");

	std::ostringstream os(std::ios::binary);
	generated.write_to_stream(os, write_opts);
	const auto streamed = os.str();
	if (streamed.size() != generated_bytes.size()) fail("write_to_stream size mismatch");

	auto generated_roundtrip = read_bytes("generated-roundtrip", generated_bytes.data(), generated_bytes.size());
	if (!generated_roundtrip) fail("generated read_bytes returned null");
	const auto& seq_roundtrip = generated_roundtrip->get_dataelement("ReferencedStudySequence"_tag);
	if (seq_roundtrip.is_missing()) fail("roundtrip missing sequence");
	const auto* seq_value = seq_roundtrip.as_sequence();
	if (!seq_value || seq_value->size() != 1) fail("roundtrip sequence item count mismatch");
	auto& pix_roundtrip = generated_roundtrip->get_dataelement("PixelData"_tag);
	if (pix_roundtrip.is_missing()) fail("roundtrip missing pixel data");
	if (!pix_roundtrip.vr().is_pixel_sequence()) fail("roundtrip pixel data should be pixel sequence");
	auto* pix_value = pix_roundtrip.as_pixel_sequence();
	if (!pix_value || pix_value->number_of_frames() != 1) fail("roundtrip pixel frame count mismatch");
	if (pix_value->basic_offset_table_count() != 1) {
		fail("roundtrip pixel sequence should include basic offset table entry");
	}
	if (!generated_roundtrip->get_dataelement("ExtendedOffsetTable"_tag).is_missing()) {
		fail("roundtrip should not emit ExtendedOffsetTable for small encapsulated payload");
	}
	if (!generated_roundtrip->get_dataelement("ExtendedOffsetTableLengths"_tag).is_missing()) {
		fail("roundtrip should not emit ExtendedOffsetTableLengths for small encapsulated payload");
	}
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
	if (generated_roundtrip_file->get_dataelement("PixelData"_tag).is_missing()) {
		fail("write_file roundtrip missing pixel data");
	}
	std::remove(roundtrip_path.c_str());

	std::remove(file_path.c_str());

	return 0;
}
