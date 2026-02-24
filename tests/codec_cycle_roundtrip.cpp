#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include <dicom.h>

namespace {
using namespace dicom::literals;

[[noreturn]] void fail(const std::string& message) {
	std::cerr << message << std::endl;
	std::exit(1);
}

void assert_bytes_equal(const std::vector<std::uint8_t>& actual,
    const std::vector<std::uint8_t>& expected, std::string_view label) {
	if (actual.size() != expected.size()) {
		fail(std::string(label) + ": byte size mismatch");
	}
	for (std::size_t i = 0; i < expected.size(); ++i) {
		if (actual[i] != expected[i]) {
			fail(std::string(label) + ": byte value mismatch at index " + std::to_string(i));
		}
	}
}

void verify_three_cycle_roundtrip(std::string_view codec_name, dicom::uid::WellKnown transfer_syntax) {
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

	const std::vector<std::uint8_t> expected_frame0{
	    0x01u, 0x00u, 0x02u, 0x00u, 0x03u, 0x00u,
	    0x04u, 0x00u, 0x05u, 0x00u, 0x06u, 0x00u};
	const std::vector<std::uint8_t> expected_frame1{
	    0x11u, 0x00u, 0x12u, 0x00u, 0x13u, 0x00u,
	    0x14u, 0x00u, 0x15u, 0x00u, 0x16u, 0x00u};

	dicom::DicomFile file;
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
	file.set_pixel_data("ExplicitVRLittleEndian"_uid, source, dicom::pixel::NoCompression{});

	for (int cycle = 1; cycle <= 3; ++cycle) {
		file.set_transfer_syntax(transfer_syntax);

		{
			const auto decoded_frame0 = file.pixel_data(0);
			const auto decoded_frame1 = file.pixel_data(1);
			const auto prefix = std::string(codec_name) + " cycle#" + std::to_string(cycle) +
			    " encoded decode";
			assert_bytes_equal(decoded_frame0, expected_frame0, prefix + " frame0");
			assert_bytes_equal(decoded_frame1, expected_frame1, prefix + " frame1");
		}

		file.set_transfer_syntax("ExplicitVRLittleEndian"_uid);

		{
			const auto decoded_frame0 = file.pixel_data(0);
			const auto decoded_frame1 = file.pixel_data(1);
			const auto prefix = std::string(codec_name) + " cycle#" + std::to_string(cycle) +
			    " back-to-native decode";
			assert_bytes_equal(decoded_frame0, expected_frame0, prefix + " frame0");
			assert_bytes_equal(decoded_frame1, expected_frame1, prefix + " frame1");
		}
	}
}

} // namespace

int main() {
	using namespace dicom::literals;
	verify_three_cycle_roundtrip("EncapsulatedUncompressedExplicitVRLittleEndian",
	    "EncapsulatedUncompressedExplicitVRLittleEndian"_uid);
	verify_three_cycle_roundtrip("HTJ2KLossless", "HTJ2KLossless"_uid);
	verify_three_cycle_roundtrip("JPEGLSLossless", "JPEGLSLossless"_uid);
	verify_three_cycle_roundtrip("JPEGLosslessSV1", "JPEGLosslessSV1"_uid);
	return 0;
}
