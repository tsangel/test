#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <optional>
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

void append_u64_le(std::vector<std::uint8_t>& out, std::uint64_t v) {
	for (int shift = 0; shift < 64; shift += 8) {
		out.push_back(static_cast<std::uint8_t>((v >> shift) & 0xFFu));
	}
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

void append_implicit_vr_le(std::vector<std::uint8_t>& out, dicom::Tag tag,
    const std::vector<std::uint8_t>& value) {
	append_u16_le(out, tag.group());
	append_u16_le(out, tag.element());
	append_u32_le(out, static_cast<std::uint32_t>(value.size()));
	append_bytes(out, value);
}

std::vector<std::uint8_t> placeholder_magic() {
	return std::vector<std::uint8_t>(
	    dicom::kPixelDataPayloadPlaceholderMagic.begin(),
	    dicom::kPixelDataPayloadPlaceholderMagic.end());
}

std::vector<std::uint8_t> placeholder_value(
    char vr0, char vr1, std::uint32_t vl, std::uint64_t payload_length) {
	auto value = placeholder_magic();
	value.push_back(static_cast<std::uint8_t>(vr0));
	value.push_back(static_cast<std::uint8_t>(vr1));
	append_u32_le(value, vl);
	append_u64_le(value, payload_length);
	return value;
}

std::vector<std::uint8_t> native_placeholder_value(
    std::uint64_t payload_length) {
	return placeholder_value(
	    'O', 'W', static_cast<std::uint32_t>(payload_length), payload_length);
}

std::vector<std::uint8_t> encap_placeholder_value(
    std::uint64_t payload_length) {
	return placeholder_value('O', 'B', 0xFFFFFFFFu, payload_length);
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
    std::vector<std::uint8_t> placeholder = native_placeholder_value(6)) {
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

std::vector<std::uint8_t> build_native_implicit_full_part10(
    const std::vector<std::uint8_t>& pixel_payload) {
	std::vector<std::uint8_t> body;
	append_implicit_vr_le(body, "SamplesPerPixel"_tag, {0x01u, 0x00u});
	append_implicit_vr_le(body, "PhotometricInterpretation"_tag,
	    {'M', 'O', 'N', 'O', 'C', 'H', 'R', 'O', 'M', 'E', '2', ' '});
	append_implicit_vr_le(body, "NumberOfFrames"_tag, {'1', ' '});
	append_implicit_vr_le(body, "Rows"_tag, {0x01u, 0x00u});
	append_implicit_vr_le(body, "Columns"_tag, {0x03u, 0x00u});
	append_implicit_vr_le(body, "BitsAllocated"_tag, {0x10u, 0x00u});
	append_implicit_vr_le(body, "BitsStored"_tag, {0x10u, 0x00u});
	append_implicit_vr_le(body, "HighBit"_tag, {0x0Fu, 0x00u});
	append_implicit_vr_le(body, "PixelRepresentation"_tag, {0x00u, 0x00u});
	append_implicit_vr_le(body, "PixelData"_tag, pixel_payload);
	return build_part10("1.2.840.10008.1.2", body);
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

std::vector<std::uint8_t> build_encap_placeholder_part10(
    std::string frames_text, std::uint64_t payload_length) {
	std::vector<std::uint8_t> body;
	append_common_pixel_metadata(body, 2, 8, std::move(frames_text));
	append_explicit_vr_le_32(
	    body, "PixelData"_tag, 'O', 'B', encap_placeholder_value(payload_length));
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
	return bytes.size() > dicom::kPixelDataPayloadPlaceholderMagic.size() &&
	    std::equal(dicom::kPixelDataPayloadPlaceholderMagic.begin(),
	        dicom::kPixelDataPayloadPlaceholderMagic.end(), bytes.begin());
}

std::string marker_text(std::span<const std::uint8_t> bytes) {
	if (!starts_with_magic(bytes)) {
		return {};
	}
	const auto offset = dicom::kPixelDataPayloadPlaceholderMagic.size();
	return std::string(
	    reinterpret_cast<const char*>(bytes.data() + offset),
	    bytes.size() - offset);
}

std::vector<std::uint8_t> explicit_pixeldata_element(
    char vr0, char vr1, const std::vector<std::uint8_t>& value,
    bool undefined_length = false) {
	std::vector<std::uint8_t> element;
	append_explicit_vr_le_32(
	    element, "PixelData"_tag, vr0, vr1, value, undefined_length);
	return element;
}

std::vector<std::uint8_t> implicit_pixeldata_element(
    const std::vector<std::uint8_t>& value) {
	std::vector<std::uint8_t> element;
	append_implicit_vr_le(element, "PixelData"_tag, value);
	return element;
}

std::vector<std::uint8_t> replace_first_subspan(
    const std::vector<std::uint8_t>& source,
    const std::vector<std::uint8_t>& needle,
    const std::vector<std::uint8_t>& replacement) {
	const auto pos = std::search(
	    source.begin(), source.end(), needle.begin(), needle.end());
	if (pos == source.end()) {
		fail("replace_first_subspan needle not found");
	}
	std::vector<std::uint8_t> out;
	out.insert(out.end(), source.begin(), pos);
	out.insert(out.end(), replacement.begin(), replacement.end());
	out.insert(out.end(), pos + static_cast<std::ptrdiff_t>(needle.size()),
	    source.end());
	return out;
}

} // namespace

int main() {
	{
		const std::vector<std::uint8_t> pixel_payload{
		    0x34u, 0x12u, 0x56u, 0x78u, 0x9Au, 0xBCu};
		std::vector<std::uint8_t> body;
		append_common_pixel_metadata(body, 3, 16, "1");
		const auto raw_pixel_element =
		    explicit_pixeldata_element('O', 'W', pixel_payload);
		append_bytes(body, raw_pixel_element);
		std::vector<std::uint8_t> trailing;
		append_explicit_vr_le_16(
		    trailing, dicom::Tag(0x7FE1u, 0x0010u), 'L', 'O', {'T', 'R'});
		append_bytes(body, trailing);
		const auto source = build_part10("1.2.840.10008.1.2.1", body);

		const auto split = dicom::split_pixeldata_payload(
		    dicom::DataSetSelection{}, "raw-split-native-explicit",
		    std::span<const std::uint8_t>(source.data(), source.size()));
		if (split.pixel_payload != pixel_payload) {
			fail("split_pixeldata_payload explicit native should preserve PixelData value");
		}
		const auto expected_main = replace_first_subspan(
		    source, raw_pixel_element,
		    explicit_pixeldata_element('O', 'B',
		        placeholder_value('O', 'W',
		            static_cast<std::uint32_t>(pixel_payload.size()),
		            pixel_payload.size())));
		if (split.main_bytes != expected_main) {
			fail("split_pixeldata_payload explicit native main bytes mismatch");
		}
		if (dicom::join_pixeldata_payload(split.main_bytes, split.pixel_payload) != source) {
			fail("join_pixeldata_payload explicit native byte roundtrip mismatch");
		}
		const auto desc = split.decode_descriptor;
		if (desc.expected_payload_length != pixel_payload.size()) {
			fail("split_pixeldata_payload explicit native descriptor length mismatch");
		}
		auto reattached = dicom::read_bytes_with_pixeldata_payload(
		    "raw-split-native-explicit-rt",
		    split.main_bytes.data(), split.main_bytes.size(),
		    split.pixel_payload.data(), split.pixel_payload.size());
		if (reattached->pixel_data(0) != pixel_payload) {
			fail("read_bytes_with_pixeldata_payload explicit native decode mismatch");
		}
		dicom::pixel::PixelPayloadDecoder decoder(
		    desc,
		    std::span<const std::uint8_t>(
		        split.pixel_payload.data(), split.pixel_payload.size()));
		const auto plan = decoder.create_decode_plan();
		if (decoder.pixel_buffer(0, plan).bytes != pixel_payload) {
			fail("PixelPayloadDecoder explicit native value payload mismatch");
		}
	}

	{
		const std::vector<std::uint8_t> pixel_payload{
		    0x34u, 0x12u, 0x56u, 0x78u, 0x9Au, 0xBCu};
		const auto source = build_native_implicit_full_part10(pixel_payload);
		const auto raw_pixel_element = implicit_pixeldata_element(pixel_payload);
		const auto split = dicom::split_pixeldata_payload(
		    dicom::DataSetSelection{}, "raw-split-native-implicit",
		    std::span<const std::uint8_t>(source.data(), source.size()));
		const auto desc = split.decode_descriptor;
		if (split.pixel_payload != pixel_payload) {
			fail("split_pixeldata_payload implicit native payload mismatch");
		}
		const auto expected_main = replace_first_subspan(
		    source, raw_pixel_element,
		    implicit_pixeldata_element(
		        placeholder_value('O', 'W',
		            static_cast<std::uint32_t>(pixel_payload.size()),
		            pixel_payload.size())));
		if (split.main_bytes != expected_main) {
			fail("split_pixeldata_payload implicit native main bytes mismatch");
		}
		if (dicom::join_pixeldata_payload(split.main_bytes, split.pixel_payload) != source) {
			fail("join_pixeldata_payload implicit native byte roundtrip mismatch");
		}
		auto reattached = dicom::read_bytes_with_pixeldata_payload(
		    "raw-split-native-implicit-rt",
		    split.main_bytes.data(), split.main_bytes.size(),
		    split.pixel_payload.data(), split.pixel_payload.size());
		if (reattached->pixel_data(0) != pixel_payload) {
			fail("read_bytes_with_pixeldata_payload implicit native decode mismatch");
		}
	}

	{
		const auto encap_value = build_two_frame_encap_payload();
		const auto source = build_encap_full_part10("2", encap_value);
		const auto raw_pixel_element =
		    explicit_pixeldata_element('O', 'B', encap_value, true);
		const auto split = dicom::split_pixeldata_payload(
		    dicom::DataSetSelection{}, "raw-split-encap",
		    std::span<const std::uint8_t>(source.data(), source.size()));
		const auto desc = split.decode_descriptor;
		if (split.pixel_payload != encap_value ||
		    desc.frame_fragments.empty()) {
			fail("split_pixeldata_payload encapsulated value payload descriptor mismatch");
		}
		if (dicom::join_pixeldata_payload(split.main_bytes, split.pixel_payload) != source) {
			fail("join_pixeldata_payload encapsulated byte roundtrip mismatch");
		}
		auto reattached = dicom::read_bytes_with_pixeldata_payload(
		    "raw-split-encap-rt",
		    split.main_bytes.data(), split.main_bytes.size(),
		    split.pixel_payload.data(), split.pixel_payload.size());
		if (reattached->encoded_pixel_frame_bytes(0) !=
		        std::vector<std::uint8_t>({0x01u, 0x02u}) ||
		    reattached->encoded_pixel_frame_bytes(1) !=
		        std::vector<std::uint8_t>({0x03u, 0x04u})) {
			fail("read_bytes_with_pixeldata_payload encapsulated frame mismatch");
		}
		dicom::pixel::PixelPayloadDecoder decoder(
		    desc,
		    std::span<const std::uint8_t>(
		        split.pixel_payload.data(), split.pixel_payload.size()));
		const auto plan = decoder.create_decode_plan();
		if (decoder.pixel_buffer(0, plan).bytes !=
		        std::vector<std::uint8_t>({0x01u, 0x02u}) ||
		    decoder.pixel_buffer(1, plan).bytes !=
		        std::vector<std::uint8_t>({0x03u, 0x04u})) {
			fail("PixelPayloadDecoder encapsulated value payload mismatch");
		}
	}

	{
		const auto big_endian = build_part10(
		    "1.2.840.10008.1.2.2", std::vector<std::uint8_t>{});
		const auto big_split = dicom::split_pixeldata_payload(
		    dicom::DataSetSelection{}, "raw-split-big-endian",
		    std::span<const std::uint8_t>(big_endian.data(), big_endian.size()));
		if (big_split.ok() ||
		    !big_split.main_bytes.empty() || !big_split.pixel_payload.empty()) {
			fail("split_pixeldata_payload big endian should return empty bytes with error");
		}
		const auto deflated = build_part10(
		    "1.2.840.10008.1.2.1.99", std::vector<std::uint8_t>{});
		const auto deflated_split = dicom::split_pixeldata_payload(
		    dicom::DataSetSelection{}, "raw-split-deflated",
		    std::span<const std::uint8_t>(deflated.data(), deflated.size()));
		if (deflated_split.ok() ||
		    !deflated_split.main_bytes.empty() || !deflated_split.pixel_payload.empty()) {
			fail("split_pixeldata_payload deflated should return empty bytes with error");
		}
		const auto missing = build_native_without_pixeldata_part10();
		const auto missing_split = dicom::split_pixeldata_payload(
		    dicom::DataSetSelection{}, "raw-split-missing-pixeldata",
		    std::span<const std::uint8_t>(missing.data(), missing.size()));
		if (missing_split.ok() ||
		    !missing_split.main_bytes.empty() || !missing_split.pixel_payload.empty()) {
			fail("split_pixeldata_payload missing PixelData should return empty bytes with error");
		}
	}

	{
		const auto main_p10 = build_native_placeholder_part10();
		const std::vector<std::uint8_t> pixel_payload{
		    0x34u, 0x12u, 0x56u, 0x78u, 0x9Au, 0xBCu};
		auto file = dicom::read_bytes_with_pixeldata_payload(
		    "split-native", main_p10.data(), main_p10.size(),
		    pixel_payload.data(), pixel_payload.size());
		if (!file || !file->has_attached_pixeldata_payload()) {
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

		file->detach_pixeldata_payload(true);
		if (file->has_attached_pixeldata_payload()) {
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
		const std::vector<std::uint8_t> pixel_payload{
		    0x34u, 0x12u, 0x56u, 0x78u, 0x9Au, 0xBCu};
		auto file = dicom::read_bytes_with_pixeldata_payload(
		    "split-native-minimal-detach", main_p10.data(), main_p10.size(),
		    pixel_payload.data(), pixel_payload.size());
		file->detach_pixeldata_payload();
		if (file->has_attached_pixeldata_payload()) {
			fail("minimal native detach should report detached");
		}
		auto& detached_pixel_data = file->get_dataelement("PixelData"_tag);
		if (detached_pixel_data.storage_kind() !=
		        dicom::DataElement::StorageKind::owned_bytes ||
		    detached_pixel_data.vr() != dicom::VR::OW ||
		    !bytes_equal(detached_pixel_data.value_span(), placeholder_magic())) {
			fail("minimal native detach should keep only the PIXDATA1 marker");
		}
		const auto minimal_dump = file->dump();
		if (minimal_dump.find("\\x34\\x12\\x56\\x78") != std::string::npos) {
			fail("minimal native detach should not retain PixelData dump text");
		}
		expect_throw("native decode after minimal detach",
		    [&]() { (void)file->pixel_data(0); }, "detached");
	}

	{
		const auto main_p10 = build_native_placeholder_part10();
		auto placeholder_only =
		    dicom::read_bytes("placeholder-only", main_p10.data(), main_p10.size());
		const auto placeholder =
		    placeholder_only->get_dataelement("PixelData"_tag).value_span();
		if (!starts_with_magic(placeholder) ||
		    placeholder.size() != dicom::kPixelDataPayloadPlaceholderMetadataSize) {
			fail("plain read_bytes should preserve placeholder metadata bytes");
		}
		if (placeholder_only->has_attached_pixeldata_payload()) {
			fail("plain read_bytes should not auto attach pixel payload");
		}
		expect_throw("plain placeholder decode",
		    [&]() { (void)placeholder_only->pixel_data(0); },
		    "detached");
	}

	{
		const auto payload = build_single_frame_encap_payload();
		const auto main_p10 = build_encap_placeholder_part10("1", payload.size());
		const std::vector<std::uint8_t> expected{0x34u, 0x12u};
		auto file = dicom::read_bytes_with_pixeldata_payload(
		    "split-encap-single", main_p10.data(), main_p10.size(),
		    payload.data(), payload.size());
		if (!file || !file->has_attached_pixeldata_payload()) {
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
		file->detach_pixeldata_payload(true);
		if (file->has_attached_pixeldata_payload()) {
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
		const auto payload = build_two_frame_encap_payload();
		const auto main_p10 = build_encap_placeholder_part10("2", payload.size());
		const std::vector<std::uint8_t> frame0{0x01u, 0x02u};
		const std::vector<std::uint8_t> frame1{0x03u, 0x04u};
		auto file = dicom::read_bytes_with_pixeldata_payload(
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
		const auto split = dicom::split_pixeldata_payload(
		    dicom::DataSetSelection{}, "split-load-native-source",
		    std::span<const std::uint8_t>(source_p10.data(), source_p10.size()));
		if (split.pixel_payload != pixel_payload) {
			fail("native split load payload mismatch");
		}
		auto placeholder_only = dicom::read_bytes(
		    "split-load-native-main", split.main_bytes.data(),
		    split.main_bytes.size());
		const auto placeholder =
		    placeholder_only->get_dataelement("PixelData"_tag).value_span();
		if (!starts_with_magic(placeholder) ||
		    placeholder.size() != dicom::kPixelDataPayloadPlaceholderMetadataSize) {
			fail("native split load main should contain split placeholder metadata");
		}
		const auto joined =
		    dicom::join_pixeldata_payload(split.main_bytes, split.pixel_payload);
		if (joined != source_p10) {
			fail("native split load join should byte-roundtrip");
		}
		auto roundtrip = dicom::read_bytes_with_pixeldata_payload(
		    "split-load-native-roundtrip", split.main_bytes.data(),
		    split.main_bytes.size(), split.pixel_payload.data(),
		    split.pixel_payload.size());
		if (roundtrip->pixel_data(0) != pixel_payload) {
			fail("native split load roundtrip pixel decode mismatch");
		}

		const auto desc = split.decode_descriptor;
		if (desc.expected_payload_length != split.pixel_payload.size() ||
		    !desc.frame_fragments.empty()) {
			fail("native payload descriptor metadata mismatch");
		}
		dicom::pixel::PixelPayloadDecoder decoder(desc,
		    std::span<const std::uint8_t>(
		        split.pixel_payload.data(), split.pixel_payload.size()));
		const auto plan = decoder.create_decode_plan();
		if (decoder.pixel_buffer(0, plan).bytes != pixel_payload) {
			fail("native payload-only decoder mismatch");
		}
		std::optional<dicom::pixel::PixelPayloadDecoder> transient_decoder;
		{
			std::string ts(desc.transfer_syntax_uid);
			std::string photometric(desc.photometric);
			std::string source_name("transient-native-payload");
			auto transient_desc = desc;
			transient_desc.transfer_syntax_uid = ts;
			transient_desc.photometric = photometric;
			transient_desc.source_name = source_name;
			transient_decoder.emplace(transient_desc,
			    std::span<const std::uint8_t>(
			        split.pixel_payload.data(), split.pixel_payload.size()));
		}
		const auto transient_plan = transient_decoder->create_decode_plan();
		if (transient_decoder->pixel_buffer(0, transient_plan).bytes != pixel_payload) {
			fail("payload decoder should not borrow descriptor strings after construction");
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

		const auto same_ts_bytes =
		    native_source.write_bytes_with_transfer_syntax("ExplicitVRLittleEndian"_uid);
		const auto same_ts_split = dicom::split_pixeldata_payload(
		    dicom::DataSetSelection{}, "split-transcode-same-ts",
		    std::span<const std::uint8_t>(
		        same_ts_bytes.data(), same_ts_bytes.size()));
		if (same_ts_split.pixel_payload != pixel_payload) {
			fail("same transfer syntax transcode+split payload mismatch");
		}

		const auto big_endian_bytes =
		    native_source.write_bytes_with_transfer_syntax("ExplicitVRBigEndian"_uid);
		const auto big_endian_split = dicom::split_pixeldata_payload(
		    dicom::DataSetSelection{}, "split-transcode-big-endian",
		    std::span<const std::uint8_t>(
		        big_endian_bytes.data(), big_endian_bytes.size()));
		if (big_endian_split.ok() ||
		    !big_endian_split.main_bytes.empty() ||
		    !big_endian_split.pixel_payload.empty()) {
			fail("split_pixeldata_payload should reject Big Endian transcoded bytes");
		}

		const auto rle_bytes =
		    native_source.write_bytes_with_transfer_syntax("RLELossless"_uid);
		const auto rle_split = dicom::split_pixeldata_payload(
		    dicom::DataSetSelection{}, "split-transcode-rle",
		    std::span<const std::uint8_t>(rle_bytes.data(), rle_bytes.size()));
		if (rle_split.pixel_payload.empty()) {
			fail("native->RLE transcode+split payload should not be empty");
		}
		auto rle_placeholder_only = dicom::read_bytes(
		    "split-transcode-rle-main", rle_split.main_bytes.data(),
		    rle_split.main_bytes.size());
		const auto rle_placeholder =
		    rle_placeholder_only->get_dataelement("PixelData"_tag).value_span();
		if (!starts_with_magic(rle_placeholder) ||
		    rle_placeholder.size() != dicom::kPixelDataPayloadPlaceholderMetadataSize) {
			fail("native->RLE split main should contain split placeholder metadata");
		}
		auto rle_roundtrip = dicom::read_bytes_with_pixeldata_payload(
		    "split-transcode-rle-roundtrip", rle_split.main_bytes.data(),
		    rle_split.main_bytes.size(), rle_split.pixel_payload.data(),
		    rle_split.pixel_payload.size());
		if (rle_roundtrip->pixel_data(0) != pixel_payload) {
			fail("native->RLE transcode+split roundtrip pixel mismatch");
		}

		const auto native_bytes =
		    rle_roundtrip->write_bytes_with_transfer_syntax(
		        "ExplicitVRLittleEndian"_uid);
		const auto native_split = dicom::split_pixeldata_payload(
		    dicom::DataSetSelection{}, "split-transcode-native",
		    std::span<const std::uint8_t>(
		        native_bytes.data(), native_bytes.size()));
		if (native_split.pixel_payload != pixel_payload) {
			fail("RLE->native transcode+split payload mismatch");
		}
		auto native_roundtrip = dicom::read_bytes_with_pixeldata_payload(
		    "split-transcode-native-roundtrip",
		    native_split.main_bytes.data(), native_split.main_bytes.size(),
		    native_split.pixel_payload.data(),
		    native_split.pixel_payload.size());
		if (native_roundtrip->pixel_data(0) != pixel_payload) {
			fail("RLE->native transcode+split roundtrip pixel mismatch");
		}
	}

	{
		const auto source_payload = build_single_frame_encap_payload();
		const auto source_p10 = build_encap_full_part10("1", source_payload);
		const std::vector<std::uint8_t> expected_frame{0x34u, 0x12u};
		const auto split = dicom::split_pixeldata_payload(
		    dicom::DataSetSelection{}, "split-load-encap-source",
		    std::span<const std::uint8_t>(source_p10.data(), source_p10.size()));
		if (split.pixel_payload.empty()) {
			fail("encapsulated split load payload should not be empty");
		}
		auto placeholder_only = dicom::read_bytes(
		    "split-load-encap-main", split.main_bytes.data(),
		    split.main_bytes.size());
		const auto placeholder =
		    placeholder_only->get_dataelement("PixelData"_tag).value_span();
		if (!starts_with_magic(placeholder) ||
		    placeholder.size() != dicom::kPixelDataPayloadPlaceholderMetadataSize) {
			fail("encapsulated split load main should contain split placeholder metadata");
		}
		const auto joined =
		    dicom::join_pixeldata_payload(split.main_bytes, split.pixel_payload);
		if (joined != source_p10) {
			fail("encapsulated split load join should byte-roundtrip");
		}
		auto roundtrip = dicom::read_bytes_with_pixeldata_payload(
		    "split-load-encap-roundtrip", split.main_bytes.data(),
		    split.main_bytes.size(), split.pixel_payload.data(),
		    split.pixel_payload.size());
		if (!bytes_equal(roundtrip->encoded_pixel_frame_view(0), expected_frame) ||
		    roundtrip->pixel_data(0) != expected_frame) {
			fail("encapsulated split load roundtrip pixel mismatch");
		}

		const auto desc = split.decode_descriptor;
		if (desc.expected_payload_length != split.pixel_payload.size() ||
		    desc.frame_fragments.empty()) {
			fail("encapsulated payload descriptor metadata mismatch");
		}
		dicom::pixel::PixelPayloadDecoder decoder(desc,
		    std::span<const std::uint8_t>(
		        split.pixel_payload.data(), split.pixel_payload.size()));
		const auto plan = decoder.create_decode_plan();
		if (decoder.pixel_buffer(0, plan).bytes != expected_frame) {
			fail("encapsulated payload-only decoder mismatch");
		}

		auto missing_fragments = desc;
		missing_fragments.frame_fragments.clear();
		expect_throw("payload decoder missing frame_fragments",
		    [&]() {
			    (void)dicom::pixel::PixelPayloadDecoder(
			        missing_fragments,
			        std::span<const std::uint8_t>(
			            split.pixel_payload.data(),
			            split.pixel_payload.size()));
		    },
		    "frame_fragments");

		auto malformed_fragments = desc;
		malformed_fragments.frame_fragments = "not-a-fragment";
		expect_throw("payload decoder malformed frame_fragments",
		    [&]() {
			    (void)dicom::pixel::PixelPayloadDecoder(
			        malformed_fragments,
			        std::span<const std::uint8_t>(
			            split.pixel_payload.data(),
			            split.pixel_payload.size()));
		    },
		    "offset:length");

		auto out_of_bounds_fragments = desc;
		out_of_bounds_fragments.frame_fragments = "999999:1";
		expect_throw("payload decoder out-of-bounds frame_fragments",
		    [&]() {
			    (void)dicom::pixel::PixelPayloadDecoder(
			        out_of_bounds_fragments,
			        std::span<const std::uint8_t>(
			            split.pixel_payload.data(),
			            split.pixel_payload.size()));
		    },
		    "outside pixel payload");
	}

	{
		auto bad_magic_value = native_placeholder_value(6);
		bad_magic_value[0] = static_cast<std::uint8_t>('B');
		const auto bad_magic = build_native_placeholder_part10(bad_magic_value);
		const auto bad_length = build_native_placeholder_part10(
		    {'D', 'X', 'P', 'I', 'X'});
		const auto missing_pixel_data = build_native_without_pixeldata_part10();
		const std::vector<std::uint8_t> pixel_payload{
		    0x34u, 0x12u, 0x56u, 0x78u, 0x9Au, 0xBCu};

		expect_throw("bad placeholder magic", [&]() {
			(void)dicom::read_bytes_with_pixeldata_payload(
			    "bad-magic", bad_magic.data(), bad_magic.size(),
			    pixel_payload.data(), pixel_payload.size());
		}, "magic mismatch");

		expect_throw("bad placeholder length", [&]() {
			(void)dicom::read_bytes_with_pixeldata_payload(
			    "bad-length", bad_length.data(), bad_length.size(),
			    pixel_payload.data(), pixel_payload.size());
		}, "exactly 22 bytes");

		expect_throw("missing placeholder", [&]() {
			(void)dicom::read_bytes_with_pixeldata_payload(
			    "missing-pixeldata", missing_pixel_data.data(),
			    missing_pixel_data.size(), pixel_payload.data(),
			    pixel_payload.size());
		}, "placeholder is missing");

		expect_throw("empty external payload", [&]() {
			(void)dicom::read_bytes_with_pixeldata_payload(
			    "empty-payload", bad_magic.data(), bad_magic.size(),
			    pixel_payload.data(), 0);
		}, "empty pixel payload");

		expect_throw("null external payload", [&]() {
			(void)dicom::read_bytes_with_pixeldata_payload(
			    "null-payload", bad_magic.data(), bad_magic.size(),
			    nullptr, pixel_payload.size());
		}, "null pixel payload");
	}

	{
		dicom::ReadOptions keep_on_error;
		keep_on_error.keep_on_error = true;

		auto bad_magic_value = native_placeholder_value(6);
		bad_magic_value[0] = static_cast<std::uint8_t>('B');
		const auto bad_magic = build_native_placeholder_part10(bad_magic_value);
		const std::vector<std::uint8_t> pixel_payload{
		    0x34u, 0x12u, 0x56u, 0x78u, 0x9Au, 0xBCu};
		auto kept_bad_magic = dicom::read_bytes_with_pixeldata_payload(
		    "bad-magic-keep", bad_magic.data(), bad_magic.size(),
		    pixel_payload.data(), pixel_payload.size(), keep_on_error);
		if (!kept_bad_magic || !kept_bad_magic->has_error()) {
			fail("keep_on_error placeholder attach failure should be recorded");
		}
		if (kept_bad_magic->has_attached_pixeldata_payload()) {
			fail("keep_on_error placeholder attach failure should not stay attached");
		}

		const auto truncated_main = build_native_truncated_pixeldata_header_part10();
		auto kept_truncated_main = dicom::read_bytes_with_pixeldata_payload(
		    "truncated-main-keep", truncated_main.data(), truncated_main.size(),
		    pixel_payload.data(), pixel_payload.size(), keep_on_error);
		if (!kept_truncated_main || !kept_truncated_main->has_error()) {
			fail("keep_on_error main parse failure should be recorded");
		}
		if (kept_truncated_main->has_attached_pixeldata_payload()) {
			fail("keep_on_error main parse failure should clear pending payload");
		}

		const std::vector<std::uint8_t> malformed_payload{0xFEu, 0xFFu};
		const auto encap_main =
		    build_encap_placeholder_part10("1", malformed_payload.size());
		auto kept_bad_encap = dicom::read_bytes_with_pixeldata_payload(
		    "bad-encap-keep", encap_main.data(), encap_main.size(),
		    malformed_payload.data(), malformed_payload.size(), keep_on_error);
		if (!kept_bad_encap || !kept_bad_encap->has_error()) {
			fail("keep_on_error encapsulated attach failure should be recorded");
		}
		if (kept_bad_encap->has_attached_pixeldata_payload()) {
			fail("keep_on_error encapsulated attach failure should detach payload");
		}
	}

	return 0;
}
