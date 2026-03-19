#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <vector>

#include <dicom.h>

#include "codec_builtin_flags.hpp"

namespace {

[[noreturn]] void fail(const std::string& message) {
	std::cerr << message << '\n';
	std::exit(1);
}

void fill_htj2k_test_file(dicom::DicomFile& df) {
	std::vector<std::uint8_t> source_bytes(16, 0xEEu);
	const auto write_row = [&](std::size_t row, std::array<std::uint8_t, 6> payload) {
		const std::size_t base = row * 8;
		for (std::size_t i = 0; i < payload.size(); ++i) {
			source_bytes[base + i] = payload[i];
		}
	};
	write_row(0, {0x01u, 0x00u, 0x02u, 0x00u, 0x03u, 0x00u});
	write_row(1, {0x04u, 0x00u, 0x05u, 0x00u, 0x06u, 0x00u});

	// Keep the padded source layout explicit so the test still exercises
	// row/frame stride handling during HTJ2K encode.
	const dicom::pixel::ConstPixelSpan source{
	    .layout = dicom::pixel::PixelLayout{
	        .data_type = dicom::pixel::DataType::u16,
	        .photometric = dicom::pixel::Photometric::monochrome2,
	        .planar = dicom::pixel::Planar::interleaved,
	        .reserved = 0,
	        .rows = 2,
	        .cols = 3,
	        .frames = 1,
	        .samples_per_pixel = 1,
	        .bits_stored = 16,
	        .row_stride = 8,
	        .frame_stride = 16,
	    },
	    .bytes = std::span<const std::uint8_t>(source_bytes.data(), source_bytes.size()),
	};
	using namespace dicom::literals;
	df.set_pixel_data("HTJ2KLossless"_uid, source);
}

void expect_decode_with_threads_hint(
    const dicom::DicomFile& df, int codec_threads,
    const std::vector<std::uint8_t>& expected) {
	dicom::pixel::DecodeOptions opt{};
	opt.codec_threads = codec_threads;
	const auto plan = df.create_decode_plan(opt);
	std::vector<std::uint8_t> dst(plan.output_layout.frame_stride, std::uint8_t{0});
	dicom::pixel::decode_frame_into(
	    df, 0, std::span<std::uint8_t>(dst.data(), dst.size()), plan);
	if (dst != expected) {
		fail("HTJ2K decode mismatch when using codec_threads=" +
		    std::to_string(codec_threads));
	}
}

}  // namespace

int main() {
	if (!dicom::test::kHtj2kBuiltin || !dicom::test::kHasOpenJphBackend) {
		return 0;
	}

	std::string error{};
	if (!dicom::pixel::use_openjph_for_htj2k_decoding(&error)) {
		fail("use_openjph_for_htj2k_decoding should succeed before runtime initialization: " +
		    error);
	}

	dicom::DicomFile df;
	fill_htj2k_test_file(df);
	const std::vector<std::uint8_t> expected{
	    0x01u, 0x00u, 0x02u, 0x00u, 0x03u, 0x00u,
	    0x04u, 0x00u, 0x05u, 0x00u, 0x06u, 0x00u};

	expect_decode_with_threads_hint(df, -1, expected);
	expect_decode_with_threads_hint(df, 4, expected);
	return 0;
}
