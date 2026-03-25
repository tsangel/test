#include <cstdlib>
#include <exception>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <dicom.h>

namespace {
using namespace dicom::literals;

[[noreturn]] void fail(const std::string& message) {
	std::cerr << message << '\n';
	std::exit(1);
}

template <typename T>
void expect_eq(const T& actual, const T& expected, std::string_view label) {
	if (!(actual == expected)) {
		fail(std::string(label) + " mismatch");
	}
}

void expect_contains(std::string_view haystack, std::string_view needle,
    std::string_view label) {
	if (haystack.find(needle) == std::string_view::npos) {
		fail(std::string(label) + " missing token: " + std::string(needle));
	}
}

void configure_native_multiframe_file(dicom::DicomFile& df,
    const std::vector<std::uint16_t>& pixels, int frames, int rows, int cols) {
	const dicom::pixel::ConstPixelSpan source{
	    .layout = dicom::pixel::PixelLayout{
	        .data_type = dicom::pixel::DataType::u16,
	        .photometric = dicom::pixel::Photometric::monochrome2,
	        .planar = dicom::pixel::Planar::interleaved,
	        .reserved = 0,
	        .rows = static_cast<std::uint32_t>(rows),
	        .cols = static_cast<std::uint32_t>(cols),
	        .frames = static_cast<std::uint32_t>(frames),
	        .samples_per_pixel = 1,
	        .bits_stored = 16,
	        .row_stride = static_cast<std::size_t>(cols) * sizeof(std::uint16_t),
	        .frame_stride = static_cast<std::size_t>(rows * cols) * sizeof(std::uint16_t),
	    },
	    .bytes = std::span<const std::uint8_t>(
	        reinterpret_cast<const std::uint8_t*>(pixels.data()),
	        pixels.size() * sizeof(std::uint16_t)),
	};
	df.set_pixel_data("ExplicitVRLittleEndian"_uid, source);
}

} // namespace

int main() {
	constexpr int kFrames = 2;
	constexpr int kRows = 4;
	constexpr int kCols = 4;
	const std::vector<std::uint16_t> pixels{
	    1, 2, 3, 4, 5, 6, 7, 8,
	    9, 10, 11, 12, 13, 14, 15, 16,
	    101, 102, 103, 104, 105, 106, 107, 108,
	    109, 110, 111, 112, 113, 114, 115, 116,
	};

	dicom::DicomFile df{};
	configure_native_multiframe_file(df, pixels, kFrames, kRows, kCols);

	{
		dicom::pixel::DecodeOptions options{};
		options.alignment = 3;
		options.row_stride = 16;
		const auto plan = df.create_decode_plan(options);
		expect_eq(plan.output_layout.row_stride, std::size_t{16},
		    "explicit row_stride");
		expect_eq(plan.output_layout.frame_stride, std::size_t{64},
		    "auto frame_stride from explicit row_stride");
	}

	{
		dicom::pixel::DecodeOptions options{};
		options.alignment = 3;
		options.frame_stride = 48;
		const auto plan = df.create_decode_plan(options);
		expect_eq(plan.output_layout.row_stride, std::size_t{8},
		    "packed row_stride when only frame_stride is explicit");
		expect_eq(plan.output_layout.frame_stride, std::size_t{48},
		    "explicit frame_stride");
	}

	{
		dicom::pixel::DecodeOptions options{};
		options.row_stride = 6;
		try {
			(void)df.create_decode_plan(options);
			fail("too-small row_stride should throw");
		} catch (const std::exception& e) {
			expect_contains(e.what(), "row_stride is smaller than row payload",
			    "row_stride error");
		}
	}

	{
		dicom::pixel::DecodeOptions options{};
		options.frame_stride = 16;
		try {
			(void)df.create_decode_plan(options);
			fail("too-small frame_stride should throw");
		} catch (const std::exception& e) {
			expect_contains(e.what(), "frame_stride is smaller than frame payload",
			    "frame_stride error");
		}
	}

	return 0;
}
