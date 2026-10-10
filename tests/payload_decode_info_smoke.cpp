#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <dicom.h>
#include "codec_builtin_flags.hpp"
#if defined(DICOMSDL_PIXEL_RUNTIME_TEST_HOOKS)
#include "pixel/runtime/runtime_registry.hpp"
#endif

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

#if defined(DICOMSDL_PIXEL_RUNTIME_TEST_HOOKS)
void* unreported_color_create() { return new int(0); }
void unreported_color_destroy(void* ctx) { delete static_cast<int*>(ctx); }
pixel_error_code unreported_color_configure(
    void*, std::uint32_t, const pixel_option_list*) {
	return PIXEL_CODEC_ERR_OK;
}
pixel_error_code unreported_color_decode_frame(
    void*, const pixel_decoder_request* request) {
	if (request == nullptr || request->output.dst == nullptr ||
	    request->source.source_buffer.data == nullptr ||
	    request->source.source_buffer.size > request->output.dst_size) {
		return PIXEL_CODEC_ERR_INVALID_ARGUMENT;
	}
	std::memcpy(request->output.dst, request->source.source_buffer.data,
	    static_cast<std::size_t>(request->source.source_buffer.size));
	// A valid plugin may decode pixels without filling the optional decode_info.
	return PIXEL_CODEC_ERR_OK;
}
std::uint32_t unreported_color_error(const void*, char*, std::uint32_t) { return 0; }

const pixel_decoder_plugin_api& unreported_color_api() {
	static const pixel_decoder_plugin_api api = [] {
		pixel_decoder_plugin_api out{};
		out.struct_size = sizeof(out);
		out.abi_version = PIXEL_DECODER_PLUGIN_ABI;
		out.info.struct_size = sizeof(out.info);
		out.info.abi_version = PIXEL_DECODER_PLUGIN_ABI;
		out.info.display_name = "Unreported Color Test Plugin";
		out.info.supported_profile_flags = PIXEL_CODEC_PROFILE_RLE_LOSSLESS_DEC;
		out.create = &unreported_color_create;
		out.destroy = &unreported_color_destroy;
		out.configure = &unreported_color_configure;
		out.decode_frame = &unreported_color_decode_frame;
		out.copy_last_error_detail = &unreported_color_error;
		return out;
	}();
	return api;
}

void unreported_color_metadata() {
	std::string error;
	require(pixel::runtime::test::register_external_codec_plugin_api_for_test(
	    &unreported_color_api(), nullptr, &error),
	    "could not register unreported color test plugin");
	struct ClearPlugin {
		~ClearPlugin() {
			std::string error;
			(void)pixel::runtime::clear_external_codec_plugins(&error);
		}
	} clear_plugin;

	// The input metadata says YBR, but this test backend emits RGB without
	// reporting its color space. Neither public API can assert that YBR is actual.
	const std::vector<std::uint8_t> rgb{230, 40, 180};
	dp::PixelPayloadDecodeDescriptor desc;
	desc.transfer_syntax_uid = "1.2.840.10008.1.2.5";
	desc.photometric = "YBR_FULL";
	desc.rows = desc.cols = 1;
	desc.samples_per_pixel = 3;
	desc.bits_allocated = desc.bits_stored = 8;
	desc.expected_payload_length = rgb.size();
	desc.frame_fragments = "0:3";
	dp::PixelPayloadDecoder decoder(desc, rgb);
	const auto plan = decoder.create_decode_plan();
	dp::DecodeInfo info;
	const auto pixels = decoder.pixel_buffer(0, plan, info);
	require(pixels.bytes == rgb && !info.photometric.has_value(),
	    "payload decode inferred unreported color space");
	std::vector<std::uint8_t> destination(plan.output_layout.frame_stride);
	dp::DecodeInfo into_info;
	decoder.decode_into(0, destination, plan, into_info);
	require(destination == rgb && !into_info.photometric.has_value(),
	    "payload decode_into inferred unreported color space");

	dicom::DicomFile file;
	require(file.set_value("Rows"_tag, long{1}) &&
	    file.set_value("Columns"_tag, long{1}) &&
	    file.set_value("SamplesPerPixel"_tag, long{3}) &&
	    file.set_value("BitsAllocated"_tag, long{8}) &&
	    file.set_value("BitsStored"_tag, long{8}) &&
	    file.set_value("HighBit"_tag, long{7}) &&
	    file.set_value("PixelRepresentation"_tag, long{0}) &&
	    file.set_value("PlanarConfiguration"_tag, long{0}) &&
	    file.set_value("PhotometricInterpretation"_tag, std::string_view("YBR_FULL")),
	    "could not set DicomFile pixel metadata");
	file.reset_encapsulated_pixel_data(1);
	file.set_transfer_syntax_state_only("RLELossless"_uid);
	file.set_encoded_pixel_frame(0, std::vector<std::uint8_t>(rgb));
	dp::DecodeInfo file_info;
	const auto file_pixels = file.pixel_buffer(0, file.create_decode_plan(), file_info);
	require(file_pixels.bytes == rgb && !file_info.photometric.has_value(),
	    "DicomFile decode inferred unreported color space");
}
#endif
} // namespace

int main() {
	try {
		native_metadata();
		compressed_metadata();
#if defined(DICOMSDL_PIXEL_RUNTIME_TEST_HOOKS)
		unreported_color_metadata();
#endif
		std::cout << "Payload decode metadata tests passed\n";
		return 0;
	} catch (const std::exception& error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
