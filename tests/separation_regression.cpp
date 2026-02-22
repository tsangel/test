#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include <dicom.h>

using namespace dicom::literals;

namespace {

[[noreturn]] void fail(const std::string& msg) {
	std::cerr << msg << std::endl;
	std::exit(1);
}

void append_u16_le(std::vector<std::uint8_t>& out, std::uint16_t v) {
	out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
	out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
}

void append_u32_le(std::vector<std::uint8_t>& out, std::uint32_t v) {
	out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
	out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
	out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFu));
	out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFFu));
}

void append_u16_be(std::vector<std::uint8_t>& out, std::uint16_t v) {
	out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
	out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
}

void append_u32_be(std::vector<std::uint8_t>& out, std::uint32_t v) {
	out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFFu));
	out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFu));
	out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
	out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
}

void append_bytes(std::vector<std::uint8_t>& out, const std::vector<std::uint8_t>& value) {
	out.insert(out.end(), value.begin(), value.end());
}

void append_explicit_vr_le_16(std::vector<std::uint8_t>& out, dicom::Tag tag,
    const char vr0, const char vr1, const std::vector<std::uint8_t>& value) {
	if (value.size() > 0xFFFFu) {
		fail("append_explicit_vr_le_16 value too large");
	}
	append_u16_le(out, tag.group());
	append_u16_le(out, tag.element());
	out.push_back(static_cast<std::uint8_t>(vr0));
	out.push_back(static_cast<std::uint8_t>(vr1));
	append_u16_le(out, static_cast<std::uint16_t>(value.size()));
	append_bytes(out, value);
}

void append_explicit_vr_be_16(std::vector<std::uint8_t>& out, dicom::Tag tag,
    const char vr0, const char vr1, const std::vector<std::uint8_t>& value) {
	if (value.size() > 0xFFFFu) {
		fail("append_explicit_vr_be_16 value too large");
	}
	append_u16_be(out, tag.group());
	append_u16_be(out, tag.element());
	out.push_back(static_cast<std::uint8_t>(vr0));
	out.push_back(static_cast<std::uint8_t>(vr1));
	append_u16_be(out, static_cast<std::uint16_t>(value.size()));
	append_bytes(out, value);
}

void append_explicit_vr_be_32(std::vector<std::uint8_t>& out, dicom::Tag tag,
    const char vr0, const char vr1, const std::vector<std::uint8_t>& value) {
	append_u16_be(out, tag.group());
	append_u16_be(out, tag.element());
	out.push_back(static_cast<std::uint8_t>(vr0));
	out.push_back(static_cast<std::uint8_t>(vr1));
	append_u16_be(out, 0);
	append_u32_be(out, static_cast<std::uint32_t>(value.size()));
	append_bytes(out, value);
}

std::vector<std::uint8_t> ui_value(std::string uid) {
	if (uid.empty() || uid.back() != '\0') {
		uid.push_back('\0');
	}
	if ((uid.size() & 1u) != 0u) {
		uid.push_back('\0');
	}
	return std::vector<std::uint8_t>(uid.begin(), uid.end());
}

std::vector<std::uint8_t> build_part10(std::string transfer_syntax_uid,
    const std::vector<std::uint8_t>& encoded_dataset_body) {
	std::vector<std::uint8_t> meta_ts;
	append_explicit_vr_le_16(
	    meta_ts, dicom::Tag(0x0002u, 0x0010u), 'U', 'I', ui_value(std::move(transfer_syntax_uid)));

	std::vector<std::uint8_t> meta_gl_value;
	append_u32_le(meta_gl_value, static_cast<std::uint32_t>(meta_ts.size()));

	std::vector<std::uint8_t> meta_gl;
	append_explicit_vr_le_16(
	    meta_gl, dicom::Tag(0x0002u, 0x0000u), 'U', 'L', meta_gl_value);

	std::vector<std::uint8_t> out(128, 0);
	out.insert(out.end(), {'D', 'I', 'C', 'M'});
	append_bytes(out, meta_gl);
	append_bytes(out, meta_ts);
	append_bytes(out, encoded_dataset_body);
	return out;
}

std::vector<std::uint8_t> build_deflated_dataset_body(std::uint16_t rows, std::uint16_t cols) {
	// Raw DEFLATE payload bytes generated from an explicit-VR-little-endian body that contains:
	// (0010,0010) PN=DOE^JOHN, (0028,0010) US=rows, (0028,0011) US=cols.
	static const std::array<std::uint8_t, 33> kRows3Cols5 = {
	    0x13, 0x60, 0x10, 0x60, 0x08, 0xF0, 0xE3, 0x60, 0x70, 0xF1, 0x77,
	    0x8D, 0xF3, 0xF2, 0xF7, 0xF0, 0xD3, 0x00, 0xF2, 0x43, 0x83, 0x99,
	    0x18, 0x98, 0x19, 0x34, 0x18, 0x04, 0xC1, 0x2C, 0x56, 0x06, 0x00};
	static const std::array<std::uint8_t, 33> kRows7Cols9 = {
	    0x13, 0x60, 0x10, 0x60, 0x08, 0xF0, 0xE3, 0x60, 0x70, 0xF1, 0x77,
	    0x8D, 0xF3, 0xF2, 0xF7, 0xF0, 0xD3, 0x00, 0xF2, 0x43, 0x83, 0x99,
	    0x18, 0xD8, 0x19, 0x34, 0x18, 0x04, 0xC1, 0x2C, 0x4E, 0x06, 0x00};

	if (rows == 3 && cols == 5) {
		return std::vector<std::uint8_t>(kRows3Cols5.begin(), kRows3Cols5.end());
	}
	if (rows == 7 && cols == 9) {
		return std::vector<std::uint8_t>(kRows7Cols9.begin(), kRows7Cols9.end());
	}
	fail("unsupported rows/cols fixture for deflated regression test");
}

std::vector<std::uint8_t> build_big_endian_sequence_body() {
	std::vector<std::uint8_t> item_payload;
	append_explicit_vr_be_16(
	    item_payload, dicom::Tag(0x0010u, 0x0010u), 'P', 'N',
	    std::vector<std::uint8_t>{'N', 'E', 'S', 'T', '^', 'B', 'E', ' '});

	std::vector<std::uint8_t> sequence_value;
	append_u16_be(sequence_value, 0xFFFEu);
	append_u16_be(sequence_value, 0xE000u);
	append_u32_be(sequence_value, static_cast<std::uint32_t>(item_payload.size()));
	append_bytes(sequence_value, item_payload);

	std::vector<std::uint8_t> body;
	append_explicit_vr_be_32(
	    body, dicom::Tag(0x0008u, 0x1111u), 'S', 'Q', sequence_value);
	return body;
}

std::vector<std::uint8_t> build_big_endian_raw_pixel_body() {
	std::vector<std::uint8_t> body;

	append_explicit_vr_be_16(
	    body, dicom::Tag(0x0028u, 0x0002u), 'U', 'S', std::vector<std::uint8_t>{0x00, 0x01});
	append_explicit_vr_be_16(
	    body, dicom::Tag(0x0028u, 0x0004u), 'C', 'S',
	    std::vector<std::uint8_t>{'M', 'O', 'N', 'O', 'C', 'H', 'R', 'O', 'M', 'E', '2', ' '});
	append_explicit_vr_be_16(
	    body, dicom::Tag(0x0028u, 0x0010u), 'U', 'S', std::vector<std::uint8_t>{0x00, 0x01});
	append_explicit_vr_be_16(
	    body, dicom::Tag(0x0028u, 0x0011u), 'U', 'S', std::vector<std::uint8_t>{0x00, 0x01});
	append_explicit_vr_be_16(
	    body, dicom::Tag(0x0028u, 0x0100u), 'U', 'S', std::vector<std::uint8_t>{0x00, 0x10});
	append_explicit_vr_be_16(
	    body, dicom::Tag(0x0028u, 0x0101u), 'U', 'S', std::vector<std::uint8_t>{0x00, 0x10});
	append_explicit_vr_be_16(
	    body, dicom::Tag(0x0028u, 0x0102u), 'U', 'S', std::vector<std::uint8_t>{0x00, 0x0F});
	append_explicit_vr_be_16(
	    body, dicom::Tag(0x0028u, 0x0103u), 'U', 'S', std::vector<std::uint8_t>{0x00, 0x00});

	append_explicit_vr_be_32(
	    body, dicom::Tag(0x7FE0u, 0x0010u), 'O', 'W', std::vector<std::uint8_t>{0x12, 0x34});
	return body;
}

void require_transfer_syntax(const dicom::DataSet& ds, dicom::uid::WellKnown expected,
    const char* context) {
	if (ds.transfer_syntax_uid() != expected) {
		fail(std::string(context) + ": transfer syntax mismatch");
	}
}

} // namespace

int main() {
	// 1) Deflated stream parse + transfer syntax + pixel_info cache lifecycle regression.
	const auto deflated_rows3_cols5 = build_part10(
	    "1.2.840.10008.1.2.1.99", build_deflated_dataset_body(3, 5));
	const auto deflated_rows7_cols9 = build_part10(
	    "1.2.840.10008.1.2.1.99", build_deflated_dataset_body(7, 9));

	auto deflated_file =
	    dicom::read_bytes("deflated-rows3-cols5", deflated_rows3_cols5.data(), deflated_rows3_cols5.size());
	if (!deflated_file) {
		fail("deflated read_bytes returned null");
	}
	auto& deflated_ds = deflated_file->dataset();
	require_transfer_syntax(deflated_ds, "DeflatedExplicitVRLittleEndian"_uid, "deflated initial");

	const auto* patient_name = deflated_ds.get_dataelement("PatientName"_tag);
	if (patient_name->is_missing()) {
		fail("deflated: missing PatientName");
	}
	const auto patient_name_sv = patient_name->to_string_view();
	if (!patient_name_sv || *patient_name_sv != "DOE^JOHN") {
		fail("deflated: PatientName mismatch");
	}

	const auto& info_before = deflated_file->pixel_info();
	if (info_before.rows != 3 || info_before.cols != 5) {
		fail("deflated: unexpected initial pixel_info rows/cols");
	}
	const auto& file_info_before = deflated_file->pixel_info();
	if (file_info_before.rows != 3 || file_info_before.cols != 5) {
		fail("deflated: DicomFile pixel_info forwarding mismatch");
	}

	deflated_file->attach_to_memory(
	    "deflated-rows7-cols9", deflated_rows7_cols9.data(), deflated_rows7_cols9.size(), true);
	deflated_file->read_attached_stream();
	require_transfer_syntax(deflated_ds, "DeflatedExplicitVRLittleEndian"_uid, "deflated reread");

	const auto& info_after = deflated_file->pixel_info();
	if (info_after.rows != 7 || info_after.cols != 9) {
		fail("deflated reread: pixel_info cache was not refreshed");
	}
	const auto& file_info_after = deflated_file->pixel_info();
	if (file_info_after.rows != 7 || file_info_after.cols != 9) {
		fail("deflated reread: DicomFile pixel_info cache was not refreshed");
	}

	// 2) Big-endian transfer syntax + nested sequence parse mode regression.
	const auto be_sequence_file = build_part10(
	    "1.2.840.10008.1.2.2", build_big_endian_sequence_body());
	auto be_seq = dicom::read_bytes("be-sequence", be_sequence_file.data(), be_sequence_file.size());
	if (!be_seq) {
		fail("be sequence read_bytes returned null");
	}
	auto& be_seq_ds = be_seq->dataset();
	require_transfer_syntax(be_seq_ds, "ExplicitVRBigEndian"_uid, "be sequence");

	const auto* nested_name = be_seq_ds.get_dataelement("00081111.0.00100010");
	if (nested_name->is_missing()) {
		fail("be sequence: nested PatientName not found");
	}
	const auto nested_name_sv = nested_name->to_string_view();
	if (!nested_name_sv || *nested_name_sv != "NEST^BE") {
		fail("be sequence: nested PatientName mismatch");
	}

	// 3) Decode backend endianness reference regression (big-endian raw pixel data).
	const auto be_raw_file = build_part10(
	    "1.2.840.10008.1.2.2", build_big_endian_raw_pixel_body());
	auto be_raw = dicom::read_bytes("be-raw", be_raw_file.data(), be_raw_file.size());
	if (!be_raw) {
		fail("be raw read_bytes returned null");
	}
	auto& be_raw_ds = be_raw->dataset();
	require_transfer_syntax(be_raw_ds, "ExplicitVRBigEndian"_uid, "be raw");

	const auto decoded = be_raw->pixel_data(0);
	if (decoded.size() != sizeof(std::uint16_t)) {
		fail("be raw: decoded byte length mismatch");
	}
	std::uint16_t decoded_value = 0;
	std::memcpy(&decoded_value, decoded.data(), sizeof(decoded_value));
	if (decoded_value != 0x1234u) {
		fail("be raw: endian swap/interpretation mismatch");
	}
	const auto decoded_from_file = be_raw->pixel_data(0);
	if (decoded_from_file.size() != decoded.size() ||
	    std::memcmp(decoded_from_file.data(), decoded.data(), decoded.size()) != 0) {
		fail("be raw: DicomFile pixel_data forwarding mismatch");
	}

	return 0;
}
