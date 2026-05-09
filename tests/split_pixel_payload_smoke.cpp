#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <exception>
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

void expect_contains(std::string_view haystack, std::string_view needle,
    std::string_view label) {
	if (haystack.find(needle) == std::string_view::npos) {
		fail(std::string(label) + " missing token: " + std::string(needle));
	}
}

template <typename Fn>
void expect_throw(std::string_view label, Fn&& fn, std::string_view token) {
	try {
		fn();
		fail(std::string(label) + " should throw");
	} catch (const std::exception& e) {
		expect_contains(e.what(), token, label);
	}
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

void append_bytes(std::vector<std::uint8_t>& out,
    const std::vector<std::uint8_t>& value) {
	out.insert(out.end(), value.begin(), value.end());
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

void append_explicit_vr_le_16(std::vector<std::uint8_t>& out, dicom::Tag tag,
    char vr0, char vr1, const std::vector<std::uint8_t>& value) {
	append_u16_le(out, tag.group());
	append_u16_le(out, tag.element());
	out.push_back(static_cast<std::uint8_t>(vr0));
	out.push_back(static_cast<std::uint8_t>(vr1));
	append_u16_le(out, static_cast<std::uint16_t>(value.size()));
	append_bytes(out, value);
}

void append_explicit_vr_le_32(std::vector<std::uint8_t>& out, dicom::Tag tag,
    char vr0, char vr1, const std::vector<std::uint8_t>& value,
    bool undefined_length = false) {
	append_u16_le(out, tag.group());
	append_u16_le(out, tag.element());
	out.push_back(static_cast<std::uint8_t>(vr0));
	out.push_back(static_cast<std::uint8_t>(vr1));
	append_u16_le(out, 0);
	append_u32_le(out, undefined_length ? 0xFFFFFFFFu : static_cast<std::uint32_t>(value.size()));
	append_bytes(out, value);
}

std::vector<std::uint8_t> placeholder_magic() {
	return std::vector<std::uint8_t>(
	    dicom::kPixelPayloadPlaceholderMagic.begin(),
	    dicom::kPixelPayloadPlaceholderMagic.end());
}

std::vector<std::uint8_t> build_part10(std::string transfer_syntax_uid,
    const std::vector<std::uint8_t>& body) {
	std::vector<std::uint8_t> meta_ts;
	append_explicit_vr_le_16(
	    meta_ts, dicom::Tag(0x0002u, 0x0010u), 'U', 'I',
	    ui_value(std::move(transfer_syntax_uid)));

	std::vector<std::uint8_t> meta_length_value;
	append_u32_le(meta_length_value, static_cast<std::uint32_t>(meta_ts.size()));

	std::vector<std::uint8_t> meta_length;
	append_explicit_vr_le_16(
	    meta_length, dicom::Tag(0x0002u, 0x0000u), 'U', 'L',
	    meta_length_value);

	std::vector<std::uint8_t> out(128, 0);
	out.insert(out.end(), {'D', 'I', 'C', 'M'});
	append_bytes(out, meta_length);
	append_bytes(out, meta_ts);
	append_bytes(out, body);
	return out;
}

void append_common_pixel_metadata(std::vector<std::uint8_t>& body,
    std::uint16_t columns, std::uint16_t bits_allocated,
    std::string frames_text) {
	append_explicit_vr_le_16(
	    body, "SamplesPerPixel"_tag, 'U', 'S', {0x01u, 0x00u});
	append_explicit_vr_le_16(
	    body, "PhotometricInterpretation"_tag, 'C', 'S',
	    {'M', 'O', 'N', 'O', 'C', 'H', 'R', 'O', 'M', 'E', '2', ' '});
	if ((frames_text.size() & 1u) != 0u) {
		frames_text.push_back(' ');
	}
	append_explicit_vr_le_16(
	    body, "NumberOfFrames"_tag, 'I', 'S',
	    std::vector<std::uint8_t>(frames_text.begin(), frames_text.end()));
	append_explicit_vr_le_16(
	    body, "Rows"_tag, 'U', 'S', {0x01u, 0x00u});
	append_explicit_vr_le_16(
	    body, "Columns"_tag, 'U', 'S',
	    {static_cast<std::uint8_t>(columns & 0xFFu),
	        static_cast<std::uint8_t>((columns >> 8) & 0xFFu)});
	append_explicit_vr_le_16(
	    body, "BitsAllocated"_tag, 'U', 'S',
	    {static_cast<std::uint8_t>(bits_allocated & 0xFFu),
	        static_cast<std::uint8_t>((bits_allocated >> 8) & 0xFFu)});
	append_explicit_vr_le_16(
	    body, "BitsStored"_tag, 'U', 'S',
	    {static_cast<std::uint8_t>(bits_allocated & 0xFFu),
	        static_cast<std::uint8_t>((bits_allocated >> 8) & 0xFFu)});
	append_explicit_vr_le_16(
	    body, "HighBit"_tag, 'U', 'S',
	    {static_cast<std::uint8_t>((bits_allocated - 1u) & 0xFFu), 0x00u});
	append_explicit_vr_le_16(
	    body, "PixelRepresentation"_tag, 'U', 'S', {0x00u, 0x00u});
}

std::vector<std::uint8_t> build_native_placeholder_part10(
    std::vector<std::uint8_t> placeholder = placeholder_magic()) {
	std::vector<std::uint8_t> body;
	append_common_pixel_metadata(body, 3, 16, "1");
	append_explicit_vr_le_32(body, "PixelData"_tag, 'O', 'B', placeholder);
	return build_part10("1.2.840.10008.1.2.1", body);
}

std::vector<std::uint8_t> build_native_full_part10(
    const std::vector<std::uint8_t>& pixel_payload) {
	std::vector<std::uint8_t> body;
	append_common_pixel_metadata(body, 3, 16, "1");
	append_explicit_vr_le_32(body, "PixelData"_tag, 'O', 'W', pixel_payload);
	return build_part10("1.2.840.10008.1.2.1", body);
}

std::vector<std::uint8_t> build_native_without_pixeldata_part10() {
	std::vector<std::uint8_t> body;
	append_common_pixel_metadata(body, 1, 16, "1");
	return build_part10("1.2.840.10008.1.2.1", body);
}

std::vector<std::uint8_t> build_native_truncated_pixeldata_header_part10() {
	std::vector<std::uint8_t> body;
	append_common_pixel_metadata(body, 3, 16, "1");
	append_u16_le(body, 0x7FE0u);
	append_u16_le(body, 0x0010u);
	body.push_back(static_cast<std::uint8_t>('O'));
	body.push_back(static_cast<std::uint8_t>('B'));
	append_u16_le(body, 0u);
	append_u16_le(body, 4u);
	return build_part10("1.2.840.10008.1.2.1", body);
}

std::vector<std::uint8_t> build_encap_placeholder_part10(std::string frames_text) {
	std::vector<std::uint8_t> body;
	append_common_pixel_metadata(body, 2, 8, std::move(frames_text));
	append_explicit_vr_le_32(body, "PixelData"_tag, 'O', 'B', placeholder_magic());
	return build_part10("1.2.840.10008.1.2.1.98", body);
}

std::vector<std::uint8_t> build_encap_full_part10(
    std::string frames_text, const std::vector<std::uint8_t>& pixel_payload) {
	std::vector<std::uint8_t> body;
	append_common_pixel_metadata(body, 2, 8, std::move(frames_text));
	append_explicit_vr_le_32(
	    body, "PixelData"_tag, 'O', 'B', pixel_payload, true);
	return build_part10("1.2.840.10008.1.2.1.98", body);
}

void append_item(std::vector<std::uint8_t>& out,
    const std::vector<std::uint8_t>& value) {
	append_u16_le(out, 0xFFFEu);
	append_u16_le(out, 0xE000u);
	append_u32_le(out, static_cast<std::uint32_t>(value.size()));
	append_bytes(out, value);
}

void append_sequence_delimiter(std::vector<std::uint8_t>& out) {
	append_u16_le(out, 0xFFFEu);
	append_u16_le(out, 0xE0DDu);
	append_u32_le(out, 0u);
}

std::vector<std::uint8_t> build_single_frame_encap_payload() {
	std::vector<std::uint8_t> payload;
	append_item(payload, {});
	append_item(payload, {0x34u, 0x12u});
	append_sequence_delimiter(payload);
	return payload;
}

std::vector<std::uint8_t> build_two_frame_encap_payload() {
	std::vector<std::uint8_t> payload;
	std::vector<std::uint8_t> bot;
	append_u32_le(bot, 0u);
	append_u32_le(bot, 10u);
	append_item(payload, bot);
	append_item(payload, {0x01u, 0x02u});
	append_item(payload, {0x03u, 0x04u});
	append_sequence_delimiter(payload);
	return payload;
}

bool bytes_equal(std::span<const std::uint8_t> lhs,
    const std::vector<std::uint8_t>& rhs) {
	return lhs.size() == rhs.size() &&
	    std::equal(lhs.begin(), lhs.end(), rhs.begin(), rhs.end());
}

bool starts_with_magic(std::span<const std::uint8_t> bytes) {
	return bytes.size() > dicom::kPixelPayloadPlaceholderMagic.size() &&
	    std::equal(dicom::kPixelPayloadPlaceholderMagic.begin(),
	        dicom::kPixelPayloadPlaceholderMagic.end(), bytes.begin());
}

std::string marker_text(std::span<const std::uint8_t> bytes) {
	if (!starts_with_magic(bytes)) {
		return {};
	}
	const auto offset = dicom::kPixelPayloadPlaceholderMagic.size();
	return std::string(
	    reinterpret_cast<const char*>(bytes.data() + offset),
	    bytes.size() - offset);
}

} // namespace

int main() {
	{
		const auto main_p10 = build_native_placeholder_part10();
		const std::vector<std::uint8_t> pixel_payload{
		    0x34u, 0x12u, 0x56u, 0x78u, 0x9Au, 0xBCu};
		auto file = dicom::read_bytes_with_pixel_payload(
		    "split-native", main_p10.data(), main_p10.size(),
		    pixel_payload.data(), pixel_payload.size());
		if (!file || !file->has_attached_pixel_payload()) {
			fail("native split payload should be attached");
		}
		auto& pixel_data = file->get_dataelement("PixelData"_tag);
		if (pixel_data.storage_kind() != dicom::DataElement::StorageKind::borrowed_bytes) {
			fail("native split PixelData should use borrowed_bytes storage");
		}
		if (pixel_data.vr() != dicom::VR::OW) {
			fail("native split PixelData should infer OW from BitsAllocated");
		}
		if (!bytes_equal(pixel_data.value_span(), pixel_payload)) {
			fail("native split PixelData value_span mismatch");
		}
		if (file->pixel_data(0) != pixel_payload) {
			fail("native split pixel decode mismatch");
		}

		file->detach_pixel_payload();
		if (file->has_attached_pixel_payload()) {
			fail("native split payload should report detached");
		}
		if (file->get_dataelement("Rows"_tag).to_long().value_or(0) != 1) {
			fail("metadata should remain available after native payload detach");
		}
		auto& detached_pixel_data = file->get_dataelement("PixelData"_tag);
		if (detached_pixel_data.storage_kind() !=
		    dicom::DataElement::StorageKind::owned_bytes ||
		    detached_pixel_data.vr() != dicom::VR::OW) {
			fail("native detach should preserve VR and replace payload with owned marker");
		}
		const auto native_marker = detached_pixel_data.value_span();
		const auto native_marker_text = marker_text(native_marker);
		if (native_marker_text.find("'7fe00010'\tOW") == std::string::npos ||
		    native_marker_text.find("\\x34\\x12\\x56\\x78") == std::string::npos) {
			fail("native detach marker should contain PixelData dump text");
		}
		const auto native_dump = file->dump();
		if (native_dump.find("'7fe00010'\tOW") == std::string::npos ||
		    native_dump.find("\\x34\\x12\\x56\\x78") == std::string::npos) {
			fail("native dump after detach should show stored PixelData dump text");
		}
		expect_throw("native decode after detach",
		    [&]() { (void)file->pixel_data(0); }, "detached");
	}

	{
		const auto main_p10 = build_native_placeholder_part10();
		auto placeholder_only =
		    dicom::read_bytes("placeholder-only", main_p10.data(), main_p10.size());
		const auto placeholder =
		    placeholder_only->get_dataelement("PixelData"_tag).value_span();
		if (!bytes_equal(placeholder, placeholder_magic())) {
			fail("plain read_bytes should preserve placeholder bytes");
		}
		if (placeholder_only->has_attached_pixel_payload()) {
			fail("plain read_bytes should not auto attach pixel payload");
		}
		expect_throw("plain placeholder decode",
		    [&]() { (void)placeholder_only->pixel_data(0); },
		    "shorter than expected");
	}

	{
		const auto main_p10 = build_encap_placeholder_part10("1");
		const auto payload = build_single_frame_encap_payload();
		const std::vector<std::uint8_t> expected{0x34u, 0x12u};
		auto file = dicom::read_bytes_with_pixel_payload(
		    "split-encap-single", main_p10.data(), main_p10.size(),
		    payload.data(), payload.size());
		if (!file || !file->has_attached_pixel_payload()) {
			fail("single-frame encapsulated split payload should be attached");
		}
		auto& pixel_data = file->get_dataelement("PixelData"_tag);
		if (!pixel_data.vr().is_pixel_sequence()) {
			fail("single-frame encapsulated split PixelData should be PX");
		}
		auto* pixel_sequence = pixel_data.as_pixel_sequence();
		if (!pixel_sequence || pixel_sequence->number_of_frames() != 1) {
			fail("single-frame encapsulated split frame count mismatch");
		}
		if (pixel_sequence->value_offset() != 0 ||
		    !pixel_sequence->stream() ||
		    pixel_sequence->stream()->identifier() != "<pixel-payload>") {
			fail("external PixelSequence backing metadata mismatch");
		}
		if (!bytes_equal(file->encoded_pixel_frame_view(0), expected)) {
			fail("single-frame encapsulated split encoded frame mismatch");
		}
		if (file->pixel_data(0) != expected) {
			fail("single-frame encapsulated split decode mismatch");
		}
		file->detach_pixel_payload();
		if (file->has_attached_pixel_payload()) {
			fail("single-frame encapsulated split payload should report detached");
		}
		auto& detached_pixel_data = file->get_dataelement("PixelData"_tag);
		if (detached_pixel_data.storage_kind() !=
		    dicom::DataElement::StorageKind::owned_bytes ||
		    detached_pixel_data.vr() != dicom::VR::PX ||
		    detached_pixel_data.as_pixel_sequence() != nullptr) {
			fail("encapsulated detach should preserve PX and replace PixelSequence with owned marker");
		}
		const auto encap_marker_text = marker_text(detached_pixel_data.value_span());
		if (encap_marker_text.find("'7fe00010'\tPX") == std::string::npos ||
		    encap_marker_text.find("PIXEL SEQUENCE WITH 1 FRAME(S)") == std::string::npos ||
		    encap_marker_text.find("FRAGMENT #0") == std::string::npos) {
			fail("encapsulated detach marker should contain PixelSequence dump text");
		}
		const auto encap_dump = file->dump();
		if (encap_dump.find("PIXEL SEQUENCE WITH 1 FRAME(S)") == std::string::npos ||
		    encap_dump.find("FRAGMENT #0") == std::string::npos) {
			fail("encapsulated dump after detach should show stored PixelSequence dump text");
		}
		expect_throw("encapsulated frame view after detach",
		    [&]() { (void)file->encoded_pixel_frame_view(0); }, "detached");
	}

	{
		const auto main_p10 = build_encap_placeholder_part10("2");
		const auto payload = build_two_frame_encap_payload();
		const std::vector<std::uint8_t> frame0{0x01u, 0x02u};
		const std::vector<std::uint8_t> frame1{0x03u, 0x04u};
		auto file = dicom::read_bytes_with_pixel_payload(
		    "split-encap-multi", main_p10.data(), main_p10.size(),
		    payload.data(), payload.size());
		auto& pixel_data = file->get_dataelement("PixelData"_tag);
		auto* pixel_sequence = pixel_data.as_pixel_sequence();
		if (!pixel_sequence || pixel_sequence->number_of_frames() != 2 ||
		    pixel_sequence->basic_offset_table_count() != 2) {
			fail("multi-frame encapsulated split indexing mismatch");
		}
		if (!bytes_equal(file->encoded_pixel_frame_view(0), frame0) ||
		    !bytes_equal(file->encoded_pixel_frame_view(1), frame1)) {
			fail("multi-frame encapsulated split encoded frame mismatch");
		}
		if (file->pixel_data(0) != frame0 || file->pixel_data(1) != frame1) {
			fail("multi-frame encapsulated split decode mismatch");
		}
	}

	{
		const std::vector<std::uint8_t> pixel_payload{
		    0x34u, 0x12u, 0x56u, 0x78u, 0x9Au, 0xBCu};
		const auto source_p10 = build_native_full_part10(pixel_payload);
		auto source = dicom::read_bytes(
		    "split-write-native-source", source_p10.data(), source_p10.size());
		const auto written = source->write_bytes_split_pixel_payload();
		if (written.pixel_payload_bytes != pixel_payload) {
			fail("native split write owned payload mismatch");
		}
		auto placeholder_only = dicom::read_bytes(
		    "split-write-native-main", written.dicom_bytes.data(),
		    written.dicom_bytes.size());
		const auto placeholder =
		    placeholder_only->get_dataelement("PixelData"_tag).value_span();
		if (!bytes_equal(placeholder, placeholder_magic())) {
			fail("native split write main should contain DXP1 placeholder");
		}
		auto roundtrip = dicom::read_bytes_with_pixel_payload(
		    "split-write-native-roundtrip", written.dicom_bytes.data(),
		    written.dicom_bytes.size(), written.pixel_payload_bytes.data(),
		    written.pixel_payload_bytes.size());
		if (roundtrip->pixel_data(0) != pixel_payload) {
			fail("native split write roundtrip pixel decode mismatch");
		}
	}

	{
		const std::vector<std::uint8_t> pixel_payload{
		    0x01u, 0x00u, 0x02u, 0x00u, 0x03u, 0x00u};
		dicom::pixel::PixelLayout layout{
		    .data_type = dicom::pixel::DataType::u16,
		    .photometric = dicom::pixel::Photometric::monochrome2,
		    .planar = dicom::pixel::Planar::interleaved,
		    .reserved = 0,
		    .rows = 1,
		    .cols = 3,
		    .frames = 1,
		    .samples_per_pixel = 1,
		    .bits_stored = 16,
		    .row_stride = 6,
		    .frame_stride = 6,
		};
		dicom::DicomFile native_source;
		native_source.set_pixel_data("ExplicitVRLittleEndian"_uid,
		    dicom::pixel::ConstPixelSpan{.layout = layout, .bytes = pixel_payload});
		const auto same_ts_split =
		    native_source.write_with_transfer_syntax_split_pixel_payload(
		        "ExplicitVRLittleEndian"_uid);
		if (same_ts_split.pixel_payload_bytes != pixel_payload) {
			fail("same transfer syntax split write payload mismatch");
		}
		const auto rle_split =
		    native_source.write_with_transfer_syntax_split_pixel_payload("RLELossless"_uid);
		if (rle_split.pixel_payload_bytes.empty()) {
			fail("native->RLE split write payload should not be empty");
		}
		auto rle_placeholder_only = dicom::read_bytes(
		    "split-write-rle-main", rle_split.dicom_bytes.data(),
		    rle_split.dicom_bytes.size());
		const auto rle_placeholder =
		    rle_placeholder_only->get_dataelement("PixelData"_tag).value_span();
		if (!bytes_equal(rle_placeholder, placeholder_magic())) {
			fail("native->RLE split write main should contain DXP1 placeholder");
		}
		auto rle_roundtrip = dicom::read_bytes_with_pixel_payload(
		    "split-write-rle-roundtrip", rle_split.dicom_bytes.data(),
		    rle_split.dicom_bytes.size(), rle_split.pixel_payload_bytes.data(),
		    rle_split.pixel_payload_bytes.size());
		if (rle_roundtrip->pixel_data(0) != pixel_payload) {
			fail("native->RLE split write roundtrip pixel mismatch");
		}

		const auto native_split =
		    rle_roundtrip->write_with_transfer_syntax_split_pixel_payload(
		        "ExplicitVRLittleEndian"_uid);
		if (native_split.pixel_payload_bytes != pixel_payload) {
			fail("RLE->native split write payload mismatch");
		}
		auto native_roundtrip = dicom::read_bytes_with_pixel_payload(
		    "split-write-native-transcode-roundtrip",
		    native_split.dicom_bytes.data(), native_split.dicom_bytes.size(),
		    native_split.pixel_payload_bytes.data(),
		    native_split.pixel_payload_bytes.size());
		if (native_roundtrip->pixel_data(0) != pixel_payload) {
			fail("RLE->native split write roundtrip pixel mismatch");
		}
	}

	{
		const auto source_payload = build_single_frame_encap_payload();
		const auto source_p10 = build_encap_full_part10("1", source_payload);
		const std::vector<std::uint8_t> expected_frame{0x34u, 0x12u};
		auto source = dicom::read_bytes(
		    "split-write-encap-source", source_p10.data(), source_p10.size());
		const auto written = source->write_bytes_split_pixel_payload();
		if (written.pixel_payload_bytes.empty()) {
			fail("encapsulated split write payload should not be empty");
		}
		auto placeholder_only = dicom::read_bytes(
		    "split-write-encap-main", written.dicom_bytes.data(),
		    written.dicom_bytes.size());
		const auto placeholder =
		    placeholder_only->get_dataelement("PixelData"_tag).value_span();
		if (!bytes_equal(placeholder, placeholder_magic())) {
			fail("encapsulated split write main should contain DXP1 placeholder");
		}
		auto roundtrip = dicom::read_bytes_with_pixel_payload(
		    "split-write-encap-roundtrip", written.dicom_bytes.data(),
		    written.dicom_bytes.size(), written.pixel_payload_bytes.data(),
		    written.pixel_payload_bytes.size());
		if (!bytes_equal(roundtrip->encoded_pixel_frame_view(0), expected_frame) ||
		    roundtrip->pixel_data(0) != expected_frame) {
			fail("encapsulated split write roundtrip pixel mismatch");
		}
	}

	{
		const auto bad_magic = build_native_placeholder_part10(
		    {'B', 'A', 'D', '!'});
		const auto bad_length = build_native_placeholder_part10(
		    {'D', 'X', 'P', '1', 'X'});
		const auto missing_pixel_data = build_native_without_pixeldata_part10();
		const std::vector<std::uint8_t> pixel_payload{
		    0x34u, 0x12u, 0x56u, 0x78u, 0x9Au, 0xBCu};

		expect_throw("bad placeholder magic", [&]() {
			(void)dicom::read_bytes_with_pixel_payload(
			    "bad-magic", bad_magic.data(), bad_magic.size(),
			    pixel_payload.data(), pixel_payload.size());
		}, "magic mismatch");

		expect_throw("bad placeholder length", [&]() {
			(void)dicom::read_bytes_with_pixel_payload(
			    "bad-length", bad_length.data(), bad_length.size(),
			    pixel_payload.data(), pixel_payload.size());
		}, "exactly 4 bytes");

		expect_throw("missing placeholder", [&]() {
			(void)dicom::read_bytes_with_pixel_payload(
			    "missing-pixeldata", missing_pixel_data.data(),
			    missing_pixel_data.size(), pixel_payload.data(),
			    pixel_payload.size());
		}, "placeholder is missing");

		expect_throw("empty external payload", [&]() {
			(void)dicom::read_bytes_with_pixel_payload(
			    "empty-payload", bad_magic.data(), bad_magic.size(),
			    pixel_payload.data(), 0);
		}, "empty pixel payload");

		expect_throw("null external payload", [&]() {
			(void)dicom::read_bytes_with_pixel_payload(
			    "null-payload", bad_magic.data(), bad_magic.size(),
			    nullptr, pixel_payload.size());
		}, "null pixel payload");
	}

	{
		dicom::ReadOptions keep_on_error;
		keep_on_error.keep_on_error = true;

		const auto bad_magic = build_native_placeholder_part10(
		    {'B', 'A', 'D', '!'});
		const std::vector<std::uint8_t> pixel_payload{
		    0x34u, 0x12u, 0x56u, 0x78u, 0x9Au, 0xBCu};
		auto kept_bad_magic = dicom::read_bytes_with_pixel_payload(
		    "bad-magic-keep", bad_magic.data(), bad_magic.size(),
		    pixel_payload.data(), pixel_payload.size(), keep_on_error);
		if (!kept_bad_magic || !kept_bad_magic->has_error()) {
			fail("keep_on_error placeholder attach failure should be recorded");
		}
		if (kept_bad_magic->has_attached_pixel_payload()) {
			fail("keep_on_error placeholder attach failure should not stay attached");
		}

		const auto truncated_main = build_native_truncated_pixeldata_header_part10();
		auto kept_truncated_main = dicom::read_bytes_with_pixel_payload(
		    "truncated-main-keep", truncated_main.data(), truncated_main.size(),
		    pixel_payload.data(), pixel_payload.size(), keep_on_error);
		if (!kept_truncated_main || !kept_truncated_main->has_error()) {
			fail("keep_on_error main parse failure should be recorded");
		}
		if (kept_truncated_main->has_attached_pixel_payload()) {
			fail("keep_on_error main parse failure should clear pending payload");
		}

		const auto encap_main = build_encap_placeholder_part10("1");
		const std::vector<std::uint8_t> malformed_payload{0xFEu, 0xFFu};
		auto kept_bad_encap = dicom::read_bytes_with_pixel_payload(
		    "bad-encap-keep", encap_main.data(), encap_main.size(),
		    malformed_payload.data(), malformed_payload.size(), keep_on_error);
		if (!kept_bad_encap || !kept_bad_encap->has_error()) {
			fail("keep_on_error encapsulated attach failure should be recorded");
		}
		if (kept_bad_encap->has_attached_pixel_payload()) {
			fail("keep_on_error encapsulated attach failure should detach payload");
		}
	}

	return 0;
}
