#include <array>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include <dicom.h>
#include "codec_builtin_flags.hpp"

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

void append_u64_le(std::vector<std::uint8_t>& out, std::uint64_t v) {
	out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
	out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
	out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFu));
	out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFFu));
	out.push_back(static_cast<std::uint8_t>((v >> 32) & 0xFFu));
	out.push_back(static_cast<std::uint8_t>((v >> 40) & 0xFFu));
	out.push_back(static_cast<std::uint8_t>((v >> 48) & 0xFFu));
	out.push_back(static_cast<std::uint8_t>((v >> 56) & 0xFFu));
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

void append_explicit_vr_le_32(std::vector<std::uint8_t>& out, dicom::Tag tag,
    const char vr0, const char vr1, const std::vector<std::uint8_t>& value,
    bool undefined_length = false) {
	append_u16_le(out, tag.group());
	append_u16_le(out, tag.element());
	out.push_back(static_cast<std::uint8_t>(vr0));
	out.push_back(static_cast<std::uint8_t>(vr1));
	append_u16_le(out, 0);
	const auto value_length =
	    undefined_length ? 0xFFFFFFFFu : static_cast<std::uint32_t>(value.size());
	append_u32_le(out, value_length);
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

std::vector<std::uint8_t> build_big_endian_malformed_ow_body() {
	std::vector<std::uint8_t> body;
	append_explicit_vr_be_32(
	    body, dicom::Tag(0x7FE0u, 0x0010u), 'O', 'W', std::vector<std::uint8_t>{0x7F});
	return body;
}

std::vector<std::uint8_t> build_encapsulated_uncompressed_pixel_body() {
	std::vector<std::uint8_t> body;

	append_explicit_vr_le_16(
	    body, dicom::Tag(0x0028u, 0x0002u), 'U', 'S', std::vector<std::uint8_t>{0x01, 0x00});
	append_explicit_vr_le_16(
	    body, dicom::Tag(0x0028u, 0x0004u), 'C', 'S',
	    std::vector<std::uint8_t>{'M', 'O', 'N', 'O', 'C', 'H', 'R', 'O', 'M', 'E', '2', ' '});
	append_explicit_vr_le_16(
	    body, dicom::Tag(0x0028u, 0x0010u), 'U', 'S', std::vector<std::uint8_t>{0x01, 0x00});
	append_explicit_vr_le_16(
	    body, dicom::Tag(0x0028u, 0x0011u), 'U', 'S', std::vector<std::uint8_t>{0x01, 0x00});
	append_explicit_vr_le_16(
	    body, dicom::Tag(0x0028u, 0x0100u), 'U', 'S', std::vector<std::uint8_t>{0x10, 0x00});
	append_explicit_vr_le_16(
	    body, dicom::Tag(0x0028u, 0x0101u), 'U', 'S', std::vector<std::uint8_t>{0x10, 0x00});
	append_explicit_vr_le_16(
	    body, dicom::Tag(0x0028u, 0x0102u), 'U', 'S', std::vector<std::uint8_t>{0x0F, 0x00});
	append_explicit_vr_le_16(
	    body, dicom::Tag(0x0028u, 0x0103u), 'U', 'S', std::vector<std::uint8_t>{0x00, 0x00});

	std::vector<std::uint8_t> encapsulated_pixel_value;
	append_u16_le(encapsulated_pixel_value, 0xFFFEu);
	append_u16_le(encapsulated_pixel_value, 0xE000u);
	append_u32_le(encapsulated_pixel_value, 0u);

	append_u16_le(encapsulated_pixel_value, 0xFFFEu);
	append_u16_le(encapsulated_pixel_value, 0xE000u);
	append_u32_le(encapsulated_pixel_value, 2u);
	encapsulated_pixel_value.push_back(0x34u);
	encapsulated_pixel_value.push_back(0x12u);

	append_u16_le(encapsulated_pixel_value, 0xFFFEu);
	append_u16_le(encapsulated_pixel_value, 0xE0DDu);
	append_u32_le(encapsulated_pixel_value, 0u);

	append_explicit_vr_le_32(
	    body, dicom::Tag(0x7FE0u, 0x0010u), 'O', 'B', encapsulated_pixel_value, true);
	return body;
}

std::vector<std::uint8_t> build_encapsulated_uncompressed_pixel_body_with_eot() {
	std::vector<std::uint8_t> body;

	append_explicit_vr_le_16(
	    body, dicom::Tag(0x0028u, 0x0002u), 'U', 'S', std::vector<std::uint8_t>{0x01, 0x00});
	append_explicit_vr_le_16(
	    body, dicom::Tag(0x0028u, 0x0004u), 'C', 'S',
	    std::vector<std::uint8_t>{'M', 'O', 'N', 'O', 'C', 'H', 'R', 'O', 'M', 'E', '2', ' '});
	append_explicit_vr_le_16(
	    body, dicom::Tag(0x0028u, 0x0010u), 'U', 'S', std::vector<std::uint8_t>{0x01, 0x00});
	append_explicit_vr_le_16(
	    body, dicom::Tag(0x0028u, 0x0011u), 'U', 'S', std::vector<std::uint8_t>{0x01, 0x00});
	append_explicit_vr_le_16(
	    body, dicom::Tag(0x0028u, 0x0100u), 'U', 'S', std::vector<std::uint8_t>{0x08, 0x00});
	append_explicit_vr_le_16(
	    body, dicom::Tag(0x0028u, 0x0101u), 'U', 'S', std::vector<std::uint8_t>{0x08, 0x00});
	append_explicit_vr_le_16(
	    body, dicom::Tag(0x0028u, 0x0102u), 'U', 'S', std::vector<std::uint8_t>{0x07, 0x00});
	append_explicit_vr_le_16(
	    body, dicom::Tag(0x0028u, 0x0103u), 'U', 'S', std::vector<std::uint8_t>{0x00, 0x00});
	append_explicit_vr_le_16(
	    body, dicom::Tag(0x0028u, 0x0008u), 'I', 'S', std::vector<std::uint8_t>{'2', ' '});

	std::vector<std::uint8_t> eot_offsets;
	append_u64_le(eot_offsets, 0u);
	append_u64_le(eot_offsets, 10u);
	append_explicit_vr_le_32(
	    body, dicom::Tag(0x7FE0u, 0x0001u), 'O', 'V', eot_offsets);

	std::vector<std::uint8_t> eot_lengths;
	append_u64_le(eot_lengths, 2u);
	append_u64_le(eot_lengths, 2u);
	append_explicit_vr_le_32(
	    body, dicom::Tag(0x7FE0u, 0x0002u), 'O', 'V', eot_lengths);

	std::vector<std::uint8_t> encapsulated_pixel_value;
	append_u16_le(encapsulated_pixel_value, 0xFFFEu);
	append_u16_le(encapsulated_pixel_value, 0xE000u);
	append_u32_le(encapsulated_pixel_value, 0u);

	append_u16_le(encapsulated_pixel_value, 0xFFFEu);
	append_u16_le(encapsulated_pixel_value, 0xE000u);
	append_u32_le(encapsulated_pixel_value, 2u);
	encapsulated_pixel_value.push_back(0x11u);
	encapsulated_pixel_value.push_back(0x12u);

	append_u16_le(encapsulated_pixel_value, 0xFFFEu);
	append_u16_le(encapsulated_pixel_value, 0xE000u);
	append_u32_le(encapsulated_pixel_value, 2u);
	encapsulated_pixel_value.push_back(0x21u);
	encapsulated_pixel_value.push_back(0x22u);

	append_u16_le(encapsulated_pixel_value, 0xFFFEu);
	append_u16_le(encapsulated_pixel_value, 0xE0DDu);
	append_u32_le(encapsulated_pixel_value, 0u);

	append_explicit_vr_le_32(
	    body, dicom::Tag(0x7FE0u, 0x0010u), 'O', 'B', encapsulated_pixel_value, true);
	return body;
}

void require_transfer_syntax(const dicom::DataSet& ds, dicom::uid::WellKnown expected,
    const char* context) {
	if (ds.transfer_syntax_uid() != expected) {
		fail(std::string(context) + ": transfer syntax mismatch");
	}
}

void require_nested_patient_name(const dicom::DataSet& ds, std::string_view expected,
    const char* context) {
	const auto& nested_name = ds.get_dataelement("00081111.0.00100010");
	if (nested_name.is_missing()) {
		fail(std::string(context) + ": nested PatientName not found");
	}
	const auto nested_name_sv = nested_name.to_string_view();
	if (!nested_name_sv || *nested_name_sv != expected) {
		fail(std::string(context) + ": nested PatientName mismatch");
	}
}

void require_patient_name_rows_cols(const dicom::DicomFile& file, std::string_view expected_name,
    long expected_rows, long expected_cols, const char* context) {
	const auto& name = file.get_dataelement("PatientName"_tag);
	if (name.is_missing()) {
		fail(std::string(context) + ": PatientName missing");
	}
	const auto name_sv = name.to_string_view();
	if (!name_sv || *name_sv != expected_name) {
		fail(std::string(context) + ": PatientName mismatch");
	}
	const auto& info = file.pixeldata_info();
	if (info.rows != expected_rows || info.cols != expected_cols) {
		fail(std::string(context) + ": rows/cols mismatch");
	}
}

} // namespace

int main() {
	// 1) Deflated stream parse + transfer syntax + pixeldata_info refresh regression.
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

	const auto& patient_name = deflated_ds.get_dataelement("PatientName"_tag);
	if (patient_name.is_missing()) {
		fail("deflated: missing PatientName");
	}
	const auto patient_name_sv = patient_name.to_string_view();
	if (!patient_name_sv || *patient_name_sv != "DOE^JOHN") {
		fail("deflated: PatientName mismatch");
	}

	const auto& info_before = deflated_file->pixeldata_info();
	if (info_before.rows != 3 || info_before.cols != 5) {
		fail("deflated: unexpected initial pixeldata_info rows/cols");
	}
	const auto& file_info_before = deflated_file->pixeldata_info();
	if (file_info_before.rows != 3 || file_info_before.cols != 5) {
		fail("deflated: DicomFile pixeldata_info forwarding mismatch");
	}

	deflated_file->attach_to_memory(
	    "deflated-rows7-cols9", deflated_rows7_cols9.data(), deflated_rows7_cols9.size(), true);
	deflated_file->read_attached_stream();
	require_transfer_syntax(deflated_ds, "DeflatedExplicitVRLittleEndian"_uid, "deflated reread");

	const auto& info_after = deflated_file->pixeldata_info();
	if (info_after.rows != 7 || info_after.cols != 9) {
		fail("deflated reread: pixeldata_info was not refreshed");
	}
	const auto& file_info_after = deflated_file->pixeldata_info();
	if (file_info_after.rows != 7 || file_info_after.cols != 9) {
		fail("deflated reread: DicomFile pixeldata_info was not refreshed");
	}

	const auto deflated_roundtrip_bytes = deflated_file->write_bytes();
	auto deflated_roundtrip = dicom::read_bytes(
	    "deflated-write-roundtrip", deflated_roundtrip_bytes.data(), deflated_roundtrip_bytes.size());
	if (!deflated_roundtrip) {
		fail("deflated write roundtrip read_bytes returned null");
	}
	require_transfer_syntax(
	    deflated_roundtrip->dataset(), "DeflatedExplicitVRLittleEndian"_uid, "deflated write");
	const auto& deflated_roundtrip_name =
	    deflated_roundtrip->get_dataelement("PatientName"_tag);
	if (deflated_roundtrip_name.is_missing()) {
		fail("deflated write: missing PatientName");
	}
	const auto deflated_roundtrip_name_sv = deflated_roundtrip_name.to_string_view();
	if (!deflated_roundtrip_name_sv || *deflated_roundtrip_name_sv != "DOE^JOHN") {
		fail("deflated write: PatientName mismatch");
	}
	const auto& deflated_roundtrip_info = deflated_roundtrip->pixeldata_info();
	if (deflated_roundtrip_info.rows != 7 || deflated_roundtrip_info.cols != 9) {
		fail("deflated write: rows/cols mismatch");
	}
	dicom::WriteOptions rebuilt_meta_opts{};
	rebuilt_meta_opts.keep_existing_meta = false;
	const auto deflated_rebuilt_bytes = deflated_file->write_bytes(rebuilt_meta_opts);
	auto deflated_rebuilt = dicom::read_bytes(
	    "deflated-write-rebuilt-meta", deflated_rebuilt_bytes.data(), deflated_rebuilt_bytes.size());
	if (!deflated_rebuilt) {
		fail("deflated write rebuilt-meta read_bytes returned null");
	}
	require_transfer_syntax(
	    deflated_rebuilt->dataset(), "DeflatedExplicitVRLittleEndian"_uid, "deflated rebuilt");

	// 2) Big-endian transfer syntax + nested sequence parse mode regression.
	const auto be_sequence_file = build_part10(
	    "1.2.840.10008.1.2.2", build_big_endian_sequence_body());
	auto be_seq = dicom::read_bytes("be-sequence", be_sequence_file.data(), be_sequence_file.size());
	if (!be_seq) {
		fail("be sequence read_bytes returned null");
	}
	auto& be_seq_ds = be_seq->dataset();
	require_transfer_syntax(be_seq_ds, "ExplicitVRBigEndian"_uid, "be sequence");

	const auto& nested_name = be_seq_ds.get_dataelement("00081111.0.00100010");
	if (nested_name.is_missing()) {
		fail("be sequence: nested PatientName not found");
	}
	const auto nested_name_sv = nested_name.to_string_view();
	if (!nested_name_sv || *nested_name_sv != "NEST^BE") {
		fail("be sequence: nested PatientName mismatch");
	}

	const auto be_seq_roundtrip_bytes = be_seq->write_bytes();
	auto be_seq_roundtrip = dicom::read_bytes(
	    "be-sequence-write-roundtrip", be_seq_roundtrip_bytes.data(), be_seq_roundtrip_bytes.size());
	if (!be_seq_roundtrip) {
		fail("be sequence write roundtrip read_bytes returned null");
	}
	auto& be_seq_roundtrip_ds = be_seq_roundtrip->dataset();
	require_transfer_syntax(be_seq_roundtrip_ds, "ExplicitVRBigEndian"_uid, "be sequence write");
	const auto& nested_name_roundtrip =
	    be_seq_roundtrip_ds.get_dataelement("00081111.0.00100010");
	if (nested_name_roundtrip.is_missing()) {
		fail("be sequence write: nested PatientName not found");
	}
	const auto nested_name_roundtrip_sv = nested_name_roundtrip.to_string_view();
	if (!nested_name_roundtrip_sv || *nested_name_roundtrip_sv != "NEST^BE") {
		fail("be sequence write: nested PatientName mismatch");
	}
	const auto be_seq_rebuilt_bytes = be_seq->write_bytes(rebuilt_meta_opts);
	auto be_seq_rebuilt = dicom::read_bytes(
	    "be-sequence-write-rebuilt-meta", be_seq_rebuilt_bytes.data(), be_seq_rebuilt_bytes.size());
	if (!be_seq_rebuilt) {
		fail("be sequence write rebuilt-meta read_bytes returned null");
	}
	require_transfer_syntax(
	    be_seq_rebuilt->dataset(), "ExplicitVRBigEndian"_uid, "be sequence rebuilt");

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

	const auto be_raw_roundtrip_bytes = be_raw->write_bytes();
	auto be_raw_roundtrip = dicom::read_bytes(
	    "be-raw-write-roundtrip", be_raw_roundtrip_bytes.data(), be_raw_roundtrip_bytes.size());
	if (!be_raw_roundtrip) {
		fail("be raw write roundtrip read_bytes returned null");
	}
	auto& be_raw_roundtrip_ds = be_raw_roundtrip->dataset();
	require_transfer_syntax(be_raw_roundtrip_ds, "ExplicitVRBigEndian"_uid, "be raw write");
	const auto decoded_roundtrip = be_raw_roundtrip->pixel_data(0);
	if (decoded_roundtrip.size() != sizeof(std::uint16_t)) {
		fail("be raw write: decoded byte length mismatch");
	}
	std::uint16_t decoded_roundtrip_value = 0;
	std::memcpy(&decoded_roundtrip_value, decoded_roundtrip.data(), sizeof(decoded_roundtrip_value));
	if (decoded_roundtrip_value != 0x1234u) {
		fail("be raw write: endian swap/interpretation mismatch");
	}

	// 4) Native pixel data can be converted to RLE and back to native.
	auto be_raw_to_rle = dicom::read_bytes(
	    "be-raw-to-rle", be_raw_file.data(), be_raw_file.size());
	if (!be_raw_to_rle) {
		fail("be raw to rle read_bytes returned null");
	}
	be_raw_to_rle->set_transfer_syntax("RLELossless"_uid);
	require_transfer_syntax(be_raw_to_rle->dataset(), "RLELossless"_uid, "be raw to rle");
	const auto& be_raw_to_rle_pixel = be_raw_to_rle->get_dataelement("PixelData"_tag);
	if (be_raw_to_rle_pixel.is_missing() || !be_raw_to_rle_pixel.vr().is_pixel_sequence()) {
		fail("be raw to rle: expected encapsulated PixelData");
	}
	const auto be_raw_to_rle_decoded = be_raw_to_rle->pixel_data(0);
	if (be_raw_to_rle_decoded.size() != sizeof(std::uint16_t)) {
		fail("be raw to rle: decoded byte length mismatch");
	}
	std::uint16_t be_raw_to_rle_value = 0;
	std::memcpy(&be_raw_to_rle_value, be_raw_to_rle_decoded.data(), sizeof(be_raw_to_rle_value));
	if (be_raw_to_rle_value != 0x1234u) {
		fail("be raw to rle: decoded value mismatch");
	}

	const auto be_raw_to_rle_bytes = be_raw_to_rle->write_bytes();
	auto be_raw_to_rle_roundtrip = dicom::read_bytes(
	    "be-raw-to-rle-roundtrip", be_raw_to_rle_bytes.data(), be_raw_to_rle_bytes.size());
	if (!be_raw_to_rle_roundtrip) {
		fail("be raw to rle roundtrip read_bytes returned null");
	}
	require_transfer_syntax(
	    be_raw_to_rle_roundtrip->dataset(), "RLELossless"_uid, "be raw to rle roundtrip");
	const auto be_raw_to_rle_roundtrip_decoded = be_raw_to_rle_roundtrip->pixel_data(0);
	if (be_raw_to_rle_roundtrip_decoded.size() != sizeof(std::uint16_t)) {
		fail("be raw to rle roundtrip: decoded byte length mismatch");
	}
	std::uint16_t be_raw_to_rle_roundtrip_value = 0;
	std::memcpy(
	    &be_raw_to_rle_roundtrip_value, be_raw_to_rle_roundtrip_decoded.data(),
	    sizeof(be_raw_to_rle_roundtrip_value));
	if (be_raw_to_rle_roundtrip_value != 0x1234u) {
		fail("be raw to rle roundtrip: decoded value mismatch");
	}

	be_raw_to_rle->set_transfer_syntax("ExplicitVRLittleEndian"_uid);
	require_transfer_syntax(
	    be_raw_to_rle->dataset(), "ExplicitVRLittleEndian"_uid, "be raw rle to native");
	const auto& be_raw_to_native_pixel = be_raw_to_rle->get_dataelement("PixelData"_tag);
	if (be_raw_to_native_pixel.is_missing() || be_raw_to_native_pixel.vr().is_pixel_sequence()) {
		fail("be raw rle to native: expected native PixelData");
	}
	const auto be_raw_to_native_bytes = be_raw_to_native_pixel.value_span();
	if (be_raw_to_native_bytes.size() != 2 ||
	    be_raw_to_native_bytes[0] != 0x34u ||
	    be_raw_to_native_bytes[1] != 0x12u) {
		fail("be raw rle to native: native PixelData bytes mismatch");
	}

	// JPEG2000/JPEG-LS regression blocks rely on builtin codec dispatch.
	if (dicom::test::kJpeg2kBuiltin) {
		// 5) Native pixel data can be converted to JPEG2000 Lossless and back to native.
		auto be_raw_to_j2k = dicom::read_bytes(
		    "be-raw-to-j2k", be_raw_file.data(), be_raw_file.size());
		if (!be_raw_to_j2k) {
			fail("be raw to j2k read_bytes returned null");
		}
		be_raw_to_j2k->set_transfer_syntax("JPEG2000Lossless"_uid);
		require_transfer_syntax(
		    be_raw_to_j2k->dataset(), "JPEG2000Lossless"_uid, "be raw to j2k");
		const auto& be_raw_to_j2k_pixel = be_raw_to_j2k->get_dataelement("PixelData"_tag);
		if (be_raw_to_j2k_pixel.is_missing() || !be_raw_to_j2k_pixel.vr().is_pixel_sequence()) {
			fail("be raw to j2k: expected encapsulated PixelData");
		}
		const auto be_raw_to_j2k_decoded = be_raw_to_j2k->pixel_data(0);
		if (be_raw_to_j2k_decoded.size() != sizeof(std::uint16_t)) {
			fail("be raw to j2k: decoded byte length mismatch");
		}
		std::uint16_t be_raw_to_j2k_value = 0;
		std::memcpy(&be_raw_to_j2k_value, be_raw_to_j2k_decoded.data(), sizeof(be_raw_to_j2k_value));
		if (be_raw_to_j2k_value != 0x1234u) {
			fail("be raw to j2k: decoded value mismatch");
		}

		const auto be_raw_to_j2k_bytes = be_raw_to_j2k->write_bytes();
		auto be_raw_to_j2k_roundtrip = dicom::read_bytes(
		    "be-raw-to-j2k-roundtrip", be_raw_to_j2k_bytes.data(), be_raw_to_j2k_bytes.size());
		if (!be_raw_to_j2k_roundtrip) {
			fail("be raw to j2k roundtrip read_bytes returned null");
		}
		require_transfer_syntax(
		    be_raw_to_j2k_roundtrip->dataset(), "JPEG2000Lossless"_uid, "be raw to j2k roundtrip");
		const auto be_raw_to_j2k_roundtrip_decoded = be_raw_to_j2k_roundtrip->pixel_data(0);
		if (be_raw_to_j2k_roundtrip_decoded.size() != sizeof(std::uint16_t)) {
			fail("be raw to j2k roundtrip: decoded byte length mismatch");
		}
		std::uint16_t be_raw_to_j2k_roundtrip_value = 0;
		std::memcpy(
		    &be_raw_to_j2k_roundtrip_value, be_raw_to_j2k_roundtrip_decoded.data(),
		    sizeof(be_raw_to_j2k_roundtrip_value));
		if (be_raw_to_j2k_roundtrip_value != 0x1234u) {
			fail("be raw to j2k roundtrip: decoded value mismatch");
		}

		be_raw_to_j2k->set_transfer_syntax("ExplicitVRLittleEndian"_uid);
		require_transfer_syntax(
		    be_raw_to_j2k->dataset(), "ExplicitVRLittleEndian"_uid, "be raw j2k to native");
		const auto& be_raw_j2k_to_native_pixel = be_raw_to_j2k->get_dataelement("PixelData"_tag);
		if (be_raw_j2k_to_native_pixel.is_missing() ||
		    be_raw_j2k_to_native_pixel.vr().is_pixel_sequence()) {
			fail("be raw j2k to native: expected native PixelData");
		}
		const auto be_raw_j2k_to_native_bytes = be_raw_j2k_to_native_pixel.value_span();
		if (be_raw_j2k_to_native_bytes.size() != 2 ||
		    be_raw_j2k_to_native_bytes[0] != 0x34u ||
		    be_raw_j2k_to_native_bytes[1] != 0x12u) {
			fail("be raw j2k to native: native PixelData bytes mismatch");
		}

		// 5b) Native pixel data can be converted to lossy JPEG2000 with default options.
		auto be_raw_to_j2k_lossy = dicom::read_bytes(
		    "be-raw-to-j2k-lossy", be_raw_file.data(), be_raw_file.size());
		if (!be_raw_to_j2k_lossy) {
			fail("be raw to lossy j2k read_bytes returned null");
		}
		be_raw_to_j2k_lossy->set_transfer_syntax("JPEG2000"_uid);
		require_transfer_syntax(
		    be_raw_to_j2k_lossy->dataset(), "JPEG2000"_uid, "be raw to lossy j2k");
		const auto& be_raw_to_j2k_lossy_pixel =
		    be_raw_to_j2k_lossy->get_dataelement("PixelData"_tag);
		if (be_raw_to_j2k_lossy_pixel.is_missing() || !be_raw_to_j2k_lossy_pixel.vr().is_pixel_sequence()) {
			fail("be raw to lossy j2k: expected encapsulated PixelData");
		}
		const auto be_raw_to_j2k_lossy_decoded = be_raw_to_j2k_lossy->pixel_data(0);
		if (be_raw_to_j2k_lossy_decoded.size() != sizeof(std::uint16_t)) {
			fail("be raw to lossy j2k: decoded byte length mismatch");
		}
		be_raw_to_j2k_lossy->set_transfer_syntax("ExplicitVRLittleEndian"_uid);
		require_transfer_syntax(
		    be_raw_to_j2k_lossy->dataset(), "ExplicitVRLittleEndian"_uid, "be raw lossy j2k to native");
		const auto& be_raw_lossy_j2k_to_native_pixel =
		    be_raw_to_j2k_lossy->get_dataelement("PixelData"_tag);
		if (be_raw_lossy_j2k_to_native_pixel.is_missing() ||
		    be_raw_lossy_j2k_to_native_pixel.vr().is_pixel_sequence()) {
			fail("be raw lossy j2k to native: expected native PixelData");
		}
		if (be_raw_lossy_j2k_to_native_pixel.value_span().size() != sizeof(std::uint16_t)) {
			fail("be raw lossy j2k to native: native PixelData byte length mismatch");
		}
	}

	if (dicom::test::kJpeg2kBuiltin && dicom::test::kJpegLsBuiltin) {
		// 5a) Encapsulated-to-encapsulated transcoding works in one set_transfer_syntax call.
		auto be_raw_encap_chain = dicom::read_bytes(
		    "be-raw-encap-chain", be_raw_file.data(), be_raw_file.size());
		if (!be_raw_encap_chain) {
			fail("be raw encapsulated chain read_bytes returned null");
		}
		be_raw_encap_chain->set_transfer_syntax("RLELossless"_uid);
		require_transfer_syntax(
		    be_raw_encap_chain->dataset(), "RLELossless"_uid, "be raw to rle chain");
		be_raw_encap_chain->set_transfer_syntax("JPEG2000Lossless"_uid);
		require_transfer_syntax(
		    be_raw_encap_chain->dataset(), "JPEG2000Lossless"_uid, "be raw rle to j2k chain");
		const auto& be_raw_rle_to_j2k_pixel =
		    be_raw_encap_chain->get_dataelement("PixelData"_tag);
		if (be_raw_rle_to_j2k_pixel.is_missing() ||
		    !be_raw_rle_to_j2k_pixel.vr().is_pixel_sequence()) {
			fail("be raw rle to j2k chain: expected encapsulated PixelData");
		}
		const auto be_raw_rle_to_j2k_decoded = be_raw_encap_chain->pixel_data(0);
		if (be_raw_rle_to_j2k_decoded.size() != sizeof(std::uint16_t)) {
			fail("be raw rle to j2k chain: decoded byte length mismatch");
		}
		std::uint16_t be_raw_rle_to_j2k_value = 0;
		std::memcpy(
		    &be_raw_rle_to_j2k_value, be_raw_rle_to_j2k_decoded.data(), sizeof(be_raw_rle_to_j2k_value));
		if (be_raw_rle_to_j2k_value != 0x1234u) {
			fail("be raw rle to j2k chain: decoded value mismatch");
		}
		be_raw_encap_chain->set_transfer_syntax("JPEGLSLossless"_uid);
		require_transfer_syntax(
		    be_raw_encap_chain->dataset(), "JPEGLSLossless"_uid, "be raw j2k to jpegls chain");
		const auto& be_raw_j2k_to_jpegls_pixel =
		    be_raw_encap_chain->get_dataelement("PixelData"_tag);
		if (be_raw_j2k_to_jpegls_pixel.is_missing() ||
		    !be_raw_j2k_to_jpegls_pixel.vr().is_pixel_sequence()) {
			fail("be raw j2k to jpegls chain: expected encapsulated PixelData");
		}
		const auto be_raw_j2k_to_jpegls_decoded = be_raw_encap_chain->pixel_data(0);
		if (be_raw_j2k_to_jpegls_decoded.size() != sizeof(std::uint16_t)) {
			fail("be raw j2k to jpegls chain: decoded byte length mismatch");
		}
		std::uint16_t be_raw_j2k_to_jpegls_value = 0;
		std::memcpy(&be_raw_j2k_to_jpegls_value,
		    be_raw_j2k_to_jpegls_decoded.data(), sizeof(be_raw_j2k_to_jpegls_value));
		if (be_raw_j2k_to_jpegls_value != 0x1234u) {
			fail("be raw j2k to jpegls chain: decoded value mismatch");
		}
	}

	// 6) Encapsulated-uncompressed transfer syntax can be normalized to native uncompressed.
	const auto encapsulated_uncompressed_file = build_part10(
	    "1.2.840.10008.1.2.1.98", build_encapsulated_uncompressed_pixel_body());
	auto encapsulated_uncompressed = dicom::read_bytes(
	    "encap-uncompressed", encapsulated_uncompressed_file.data(), encapsulated_uncompressed_file.size());
	if (!encapsulated_uncompressed) {
		fail("encap-uncompressed read_bytes returned null");
	}
	require_transfer_syntax(encapsulated_uncompressed->dataset(),
	    "EncapsulatedUncompressedExplicitVRLittleEndian"_uid, "encap-uncompressed initial");
	const auto& pixel_data_before = encapsulated_uncompressed->get_dataelement("PixelData"_tag);
	if (pixel_data_before.is_missing() || !pixel_data_before.vr().is_pixel_sequence()) {
		fail("encap-uncompressed: expected encapsulated PixelData before conversion");
	}
	encapsulated_uncompressed->set_transfer_syntax("ExplicitVRLittleEndian"_uid);
	auto& encapsulated_uncompressed_ds = encapsulated_uncompressed->dataset();
	require_transfer_syntax(
	    encapsulated_uncompressed_ds, "ExplicitVRLittleEndian"_uid, "encap-uncompressed converted");
	const auto& pixel_data_after = encapsulated_uncompressed_ds.get_dataelement("PixelData"_tag);
	if (pixel_data_after.is_missing() || pixel_data_after.vr().is_pixel_sequence()) {
		fail("encap-uncompressed: expected native PixelData after conversion");
	}
	const auto pixel_bytes_after = pixel_data_after.value_span();
	if (pixel_bytes_after.size() != 2 || pixel_bytes_after[0] != 0x34u || pixel_bytes_after[1] != 0x12u) {
		fail("encap-uncompressed: native PixelData bytes mismatch");
	}
	const auto decoded_after_conversion = encapsulated_uncompressed->pixel_data(0);
	if (decoded_after_conversion.size() != sizeof(std::uint16_t)) {
		fail("encap-uncompressed: decoded frame size mismatch after conversion");
	}
	std::uint16_t decoded_after_conversion_value = 0;
	std::memcpy(
	    &decoded_after_conversion_value, decoded_after_conversion.data(), sizeof(decoded_after_conversion_value));
	if (decoded_after_conversion_value != 0x1234u) {
		fail("encap-uncompressed: decoded value mismatch after conversion");
	}
	const auto encap_to_native_bytes = encapsulated_uncompressed->write_bytes();
	auto encap_to_native_roundtrip = dicom::read_bytes(
	    "encap-uncompressed-to-native", encap_to_native_bytes.data(), encap_to_native_bytes.size());
	if (!encap_to_native_roundtrip) {
		fail("encap-uncompressed-to-native read_bytes returned null");
	}
	require_transfer_syntax(
	    encap_to_native_roundtrip->dataset(), "ExplicitVRLittleEndian"_uid, "encap-uncompressed write");

	// 6a) Encapsulated PixelData with EOT should load frame boundaries from EOT.
	const auto encapsulated_eot_file = build_part10(
	    "1.2.840.10008.1.2.1.98", build_encapsulated_uncompressed_pixel_body_with_eot());
	auto encapsulated_eot = dicom::read_bytes(
	    "encap-uncompressed-eot", encapsulated_eot_file.data(), encapsulated_eot_file.size());
	if (!encapsulated_eot) {
		fail("encap-uncompressed-eot read_bytes returned null");
	}
	auto& eot_pixel_data = encapsulated_eot->get_dataelement("PixelData"_tag);
	if (eot_pixel_data.is_missing() || !eot_pixel_data.vr().is_pixel_sequence()) {
		fail("encap-uncompressed-eot: expected encapsulated PixelData");
	}
	auto* eot_pixel_sequence = eot_pixel_data.as_pixel_sequence();
	if (!eot_pixel_sequence) {
		fail("encap-uncompressed-eot: expected pixel sequence");
	}
	if (eot_pixel_sequence->basic_offset_table_count() != 0) {
		fail("encap-uncompressed-eot: BOT must be empty when EOT is present");
	}
	if (eot_pixel_sequence->extended_offset_table_count() != 2) {
		fail("encap-uncompressed-eot: expected two EOT entries");
	}
	if (eot_pixel_sequence->number_of_frames() != 2) {
		fail("encap-uncompressed-eot: expected two frames");
	}
	const auto eot_frame0 = eot_pixel_sequence->frame_encoded_span(0);
	const auto eot_frame1 = eot_pixel_sequence->frame_encoded_span(1);
	if (eot_frame0.size() != 2 || eot_frame0[0] != 0x11u || eot_frame0[1] != 0x12u) {
		fail("encap-uncompressed-eot: frame 0 payload mismatch");
	}
	if (eot_frame1.size() != 2 || eot_frame1[0] != 0x21u || eot_frame1[1] != 0x22u) {
		fail("encap-uncompressed-eot: frame 1 payload mismatch");
	}

	// 7) Malformed big-endian payload should fail during normalization.
	const auto be_malformed_file = build_part10(
	    "1.2.840.10008.1.2.2", build_big_endian_malformed_ow_body());
	bool malformed_failed = false;
	try {
		(void)dicom::read_bytes(
		    "be-malformed", be_malformed_file.data(), be_malformed_file.size());
	} catch (const std::exception&) {
		malformed_failed = true;
	}
	if (!malformed_failed) {
		fail("be malformed: expected read failure");
	}

	// 8) Multi-step transfer syntax write/read cycles.
	auto roundtrip_with_target_ts = [&](auto file, dicom::uid::WellKnown target_ts,
	                                   const char* context) {
		if (!file) {
			fail(std::string(context) + ": null file input");
		}

		file->set_transfer_syntax(target_ts);

		const auto bytes = file->write_bytes();
		auto next = dicom::read_bytes(context, bytes.data(), bytes.size());
		if (!next) {
			fail(std::string(context) + ": read_bytes after write returned null");
		}
		require_transfer_syntax(next->dataset(), target_ts, context);
		return next;
	};

	auto lb_cycle =
	    dicom::read_bytes("lb-cycle-seed", be_sequence_file.data(), be_sequence_file.size());
	if (!lb_cycle) {
		fail("lb-cycle-seed read_bytes returned null");
	}
	require_nested_patient_name(lb_cycle->dataset(), "NEST^BE", "lb-cycle-seed");

	lb_cycle = roundtrip_with_target_ts(
	    std::move(lb_cycle), "ExplicitVRLittleEndian"_uid, "lb-cycle-L1");
	require_nested_patient_name(lb_cycle->dataset(), "NEST^BE", "lb-cycle-L1");
	lb_cycle = roundtrip_with_target_ts(
	    std::move(lb_cycle), "ExplicitVRBigEndian"_uid, "lb-cycle-B1");
	require_nested_patient_name(lb_cycle->dataset(), "NEST^BE", "lb-cycle-B1");
	lb_cycle = roundtrip_with_target_ts(
	    std::move(lb_cycle), "ExplicitVRLittleEndian"_uid, "lb-cycle-L2");
	require_nested_patient_name(lb_cycle->dataset(), "NEST^BE", "lb-cycle-L2");
	lb_cycle = roundtrip_with_target_ts(
	    std::move(lb_cycle), "ExplicitVRBigEndian"_uid, "lb-cycle-B2");
	require_nested_patient_name(lb_cycle->dataset(), "NEST^BE", "lb-cycle-B2");

	auto id_cycle = dicom::read_bytes(
	    "id-cycle-seed", deflated_rows7_cols9.data(), deflated_rows7_cols9.size());
	if (!id_cycle) {
		fail("id-cycle-seed read_bytes returned null");
	}
	require_patient_name_rows_cols(*id_cycle, "DOE^JOHN", 7, 9, "id-cycle-seed");

	id_cycle = roundtrip_with_target_ts(
	    std::move(id_cycle), "ExplicitVRLittleEndian"_uid, "id-cycle-I1");
	require_patient_name_rows_cols(*id_cycle, "DOE^JOHN", 7, 9, "id-cycle-I1");
	id_cycle = roundtrip_with_target_ts(
	    std::move(id_cycle), "DeflatedExplicitVRLittleEndian"_uid, "id-cycle-D1");
	require_patient_name_rows_cols(*id_cycle, "DOE^JOHN", 7, 9, "id-cycle-D1");
	id_cycle = roundtrip_with_target_ts(
	    std::move(id_cycle), "ExplicitVRLittleEndian"_uid, "id-cycle-I2");
	require_patient_name_rows_cols(*id_cycle, "DOE^JOHN", 7, 9, "id-cycle-I2");
	id_cycle = roundtrip_with_target_ts(
	    std::move(id_cycle), "DeflatedExplicitVRLittleEndian"_uid, "id-cycle-D2");
	require_patient_name_rows_cols(*id_cycle, "DOE^JOHN", 7, 9, "id-cycle-D2");

	return 0;
}
