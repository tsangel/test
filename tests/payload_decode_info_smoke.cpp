#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <dicom.h>

using namespace dicom::literals;
namespace dp = dicom::pixel;

namespace {
void require(bool condition, const char* message) {
	if (!condition) throw std::runtime_error(message);
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
		require(result.bytes == bytes, "native pixels changed");
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
	require(decoder.pixel_buffer(0, plan, info).bytes ==
	    std::vector<std::uint8_t>({1, 0, 1}), "1-bit unpack changed");
	require(info.photometric == dp::Photometric::monochrome2 &&
	    info.data_type == dp::DataType::u8 && info.bits_per_sample == 1 &&
	    info.planar == dp::Planar::interleaved &&
	    info.encoded_lossy_state == dp::EncodedLossyState::lossless,
	    "1-bit decode metadata incorrect");
}

void compressed_metadata() {
	for (const auto photo : {"YBR_FULL_422", "YBR_RCT", "YBR_ICT"}) {
		const bool jpeg = std::string_view(photo) == "YBR_FULL_422";
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
