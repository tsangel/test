#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include <dicom.h>
#include "codec_builtin_flags.hpp"

namespace {
using namespace dicom::literals;

[[noreturn]] void fail(const std::string& message) {
	std::cerr << message << std::endl;
	std::exit(1);
}

[[nodiscard]] dicom::pixel::ConstPixelSpan build_u16_source_span(
    std::span<const std::uint8_t> bytes, std::uint32_t frames,
    std::uint32_t rows, std::uint32_t cols, std::size_t row_stride,
    std::size_t frame_stride) {
	// Keep the smoke test source description aligned with the normalized encode API.
	return dicom::pixel::ConstPixelSpan{
	    .layout = dicom::pixel::PixelLayout{
	        .data_type = dicom::pixel::DataType::u16,
	        .photometric = dicom::pixel::Photometric::monochrome2,
	        .planar = dicom::pixel::Planar::interleaved,
	        .reserved = 0,
	        .rows = rows,
	        .cols = cols,
	        .frames = frames,
	        .samples_per_pixel = 1,
	        .bits_stored = 16,
	        .row_stride = row_stride,
	        .frame_stride = frame_stride,
	    },
	    .bytes = bytes,
	};
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
	file.set_pixel_data("ExplicitVRLittleEndian"_uid,
	    build_u16_source_span(source_bytes, 2, 2, 3, 8, 20));

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
	if (dicom::test::kHtj2kBuiltin && dicom::test::kHasOpenJphBackend) {
		verify_three_cycle_roundtrip("HTJ2KLossless", "HTJ2KLossless"_uid);
	}
	if (dicom::test::kJpeg2kBuiltin) {
		verify_three_cycle_roundtrip("JPEG2000Lossless", "JPEG2000Lossless"_uid);
	}
	if (dicom::test::kJpegLsBuiltin) {
		verify_three_cycle_roundtrip("JPEGLSLossless", "JPEGLSLossless"_uid);
	}
	if (dicom::test::kJpegBuiltin) {
		verify_three_cycle_roundtrip("JPEGLosslessSV1", "JPEGLosslessSV1"_uid);
	}
	if (dicom::test::kJpegXlBuiltin && dicom::test::kHasJpegXlBackend) {
		verify_three_cycle_roundtrip("JPEGXLLossless", "JPEGXLLossless"_uid);
	}
	return 0;
}
