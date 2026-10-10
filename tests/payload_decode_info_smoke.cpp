#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <dicom.h>
#include "codec_builtin_flags.hpp"

using namespace dicom::literals;
namespace dp = dicom::pixel;

namespace {
void require(bool condition, const char* message) {
	if (!condition) throw std::runtime_error(message);
}

void require_same_info(const dp::DecodeInfo& lhs, const dp::DecodeInfo& rhs) {
	require(lhs.photometric == rhs.photometric &&
	    lhs.encoded_lossy_state == rhs.encoded_lossy_state &&
	    lhs.data_type == rhs.data_type && lhs.planar == rhs.planar &&
	    lhs.bits_per_sample == rhs.bits_per_sample,
	    "pixel_buffer and decode_into metadata differ");
}

void native_metadata() {
	for (const auto photo : {"MONOCHROME1", "MONOCHROME2", "PALETTE COLOR"}) {
		const std::vector<std::uint8_t> bytes{0, 64, 128, 255};
		dp::PixelPayloadDecodeDescriptor desc;
		desc.transfer_syntax_uid = "1.2.840.10008.1.2.1";
		desc.photometric = photo;
		desc.rows = 1;
		desc.cols = 4;
		desc.bits_allocated = desc.bits_stored = 8;
		desc.expected_payload_length = bytes.size();
		dp::PixelPayloadDecoder decoder(desc, bytes);
		const auto plan = decoder.create_decode_plan();
		dp::DecodeInfo info;
		const auto result = decoder.pixel_buffer(0, plan, info);
		std::vector<std::uint8_t> destination(plan.output_layout.frame_stride);
		dp::DecodeInfo into_info;
		decoder.decode_into(0, destination, plan, into_info);
		require(result.bytes == bytes, "native pixels changed");
		require(destination == result.bytes, "native decode_into pixels differ");
		require_same_info(info, into_info);
		require(result.bytes == decoder.pixel_buffer(0, plan).bytes,
		    "legacy overload changed");
		require(info.photometric == dp::parse_photometric(photo),
		    "native photometric semantics lost");
		require(info.data_type == dp::DataType::u8 &&
		    info.planar == dp::Planar::interleaved && info.bits_per_sample == 8 &&
		    info.encoded_lossy_state == dp::EncodedLossyState::lossless,
		    "native decode metadata incorrect");
	}

	const std::vector<std::uint8_t> bits{0x05};
	dp::PixelPayloadDecodeDescriptor desc;
	desc.transfer_syntax_uid = "1.2.840.10008.1.2.1";
	desc.photometric = "MONOCHROME2";
	desc.rows = 1;
	desc.cols = 3;
	desc.bits_allocated = desc.bits_stored = 1;
	desc.expected_payload_length = bits.size();
	dp::PixelPayloadDecoder decoder(desc, bits);
	const auto plan = decoder.create_decode_plan();
	dp::DecodeInfo info;
	const auto result = decoder.pixel_buffer(0, plan, info);
	std::vector<std::uint8_t> destination(plan.output_layout.frame_stride);
	dp::DecodeInfo into_info;
	decoder.decode_into(0, destination, plan, into_info);
	require(result.bytes == destination, "1-bit decode_into pixels differ");
	require_same_info(info, into_info);
	require(result.bytes ==
	    std::vector<std::uint8_t>({1, 0, 1}), "1-bit unpack changed");
	require(info.photometric == dp::Photometric::monochrome2 &&
	    info.data_type == dp::DataType::u8 && info.bits_per_sample == 1 &&
	    info.planar == dp::Planar::interleaved &&
	    info.encoded_lossy_state == dp::EncodedLossyState::lossless,
	    "1-bit decode metadata incorrect");

	const auto previous_info = info;
	bool threw = false;
	try {
		decoder.decode_into(1, destination, plan, info);
	} catch (const std::exception&) {
		threw = true;
	}
	require(threw, "invalid frame decode should throw");
	require_same_info(previous_info, info);
}

void compressed_metadata() {
	for (const auto photo : {"YBR_FULL_422", "YBR_RCT", "YBR_ICT"}) {
		const bool jpeg = std::string_view(photo) == "YBR_FULL_422";
		if ((jpeg && !dicom::test::kJpegBuiltin) ||
		    (!jpeg && !dicom::test::kJpeg2kBuiltin)) {
			continue;
		}
		const auto ts = jpeg ? "JPEGBaseline8Bit"_uid :
		    std::string_view(photo) == "YBR_RCT" ? "JPEG2000Lossless"_uid : "JPEG2000"_uid;
		std::vector<std::uint8_t> bytes(8 * 8 * 3);
		for (std::size_t i = 0; i < bytes.size(); i += 3) {
			bytes[i] = 230;
			bytes[i + 1] = 40;
			bytes[i + 2] = 180;
		}
		dp::PixelLayout layout;
		layout.data_type = dp::DataType::u8;
		layout.photometric = dp::Photometric::rgb;
		layout.planar = dp::Planar::interleaved;
		layout.rows = layout.cols = 8;
		layout.frames = 1;
		layout.samples_per_pixel = 3;
		layout.bits_stored = 8;
		dicom::DicomFile file;
		file.set_pixel_data(ts, {layout.packed(), bytes});
		require(file.set_value("PhotometricInterpretation"_tag, std::string_view(photo)),
		    "could not set compressed photometric");
		const auto encoded = file.write_bytes_with_transfer_syntax(ts);
		const auto split = dicom::split_pixeldata_payload(
		    dicom::DataSetSelection{}, "payload-decode-info", encoded);
		require(split.ok(), "could not split compressed payload");
		dp::PixelPayloadDecoder decoder(split.decode_descriptor, split.pixel_payload);
		const auto plan = decoder.create_decode_plan();
		dp::DecodeInfo info, file_info;
		const auto result = decoder.pixel_buffer(0, plan, info);
		std::vector<std::uint8_t> destination(plan.output_layout.frame_stride);
		dp::DecodeInfo into_info;
		decoder.decode_into(0, destination, plan, into_info);
		require(destination == result.bytes,
		    "compressed decode_into pixels differ");
		require_same_info(info, into_info);
		const auto expected = file.pixel_buffer(0, file.create_decode_plan(), file_info);
		require(result.bytes == expected.bytes &&
		    result.bytes == decoder.pixel_buffer(0, plan).bytes,
		    "metadata overload changed compressed pixels");
		require(info.photometric == dp::Photometric::rgb,
		    "compressed backend RGB output not reported");
		require(result.layout.photometric == plan.output_layout.photometric &&
		    result.layout.photometric == dp::parse_photometric(photo),
		    "planned layout unexpectedly changed");
		require(info.data_type == file_info.data_type &&
		    info.planar == file_info.planar && info.bits_per_sample == file_info.bits_per_sample &&
		    info.encoded_lossy_state == file_info.encoded_lossy_state,
		    "payload and DicomFile decode metadata differ");
		require(info.encoded_lossy_state ==
		    (std::string_view(photo) == "YBR_RCT"
		            ? dp::EncodedLossyState::lossless
		            : dp::EncodedLossyState::lossy),
		    "compressed lossiness not reported");
	}
}
} // namespace

int main() {
	try {
		native_metadata();
		compressed_metadata();
		std::cout << "Payload decode metadata tests passed\n";
		return 0;
	} catch (const std::exception& error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
