#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <dicom.h>
#include "codec_builtin_flags.hpp"

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

void expect_true(bool value, std::string_view label) {
	if (!value) {
		fail(std::string(label) + " expected true");
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

void configure_native_file(dicom::DicomFile& df,
    const std::vector<std::uint8_t>& bytes,
    const dicom::pixel::PixelLayout& layout) {
	const dicom::pixel::ConstPixelSpan source{
	    .layout = layout,
	    .bytes = std::span<const std::uint8_t>(bytes.data(), bytes.size()),
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

	{
		const auto plan = df.create_decode_plan(dicom::pixel::DecodeOptions{});
		std::vector<std::uint8_t> decoded(plan.output_layout.single_frame().frame_stride,
		    std::uint8_t{0});
		dicom::pixel::DecodeInfo decode_info{};
		dicom::pixel::decode_frame_into(df, 1, decoded, plan, decode_info);

		std::uint16_t first_sample = 0;
		std::uint16_t last_sample = 0;
		std::memcpy(&first_sample, decoded.data(), sizeof(first_sample));
		std::memcpy(&last_sample,
		    decoded.data() + (15 * sizeof(std::uint16_t)), sizeof(last_sample));
		expect_eq(first_sample, static_cast<std::uint16_t>(101),
		    "free decode_frame_into second-frame first sample");
		expect_eq(last_sample, static_cast<std::uint16_t>(116),
		    "free decode_frame_into second-frame last sample");
		expect_true(decode_info.photometric.has_value(),
		    "free decode_frame_into photometric presence");
		expect_eq(*decode_info.photometric, dicom::pixel::Photometric::monochrome2,
		    "free decode_frame_into photometric");
		expect_eq(decode_info.encoded_lossy_state,
		    dicom::pixel::EncodedLossyState::lossless,
		    "free decode_frame_into encoded lossy state");
		expect_true(decode_info.data_type.has_value(),
		    "free decode_frame_into dtype presence");
		expect_eq(*decode_info.data_type, dicom::pixel::DataType::u16,
		    "free decode_frame_into dtype");
		expect_true(decode_info.planar.has_value(),
		    "free decode_frame_into planar presence");
		expect_eq(*decode_info.planar, dicom::pixel::Planar::interleaved,
		    "free decode_frame_into planar");
		expect_eq(decode_info.bits_per_sample, static_cast<std::uint16_t>(16),
		    "free decode_frame_into bits per sample");
	}

	{
		const auto plan = df.create_decode_plan(dicom::pixel::DecodeOptions{});
		dicom::pixel::DecodeInfo decode_info{};
		const auto decoded = df.pixel_buffer(0, plan, decode_info);
		std::uint16_t first_sample = 0;
		std::uint16_t last_sample = 0;
		std::memcpy(&first_sample, decoded.bytes.data(), sizeof(first_sample));
		std::memcpy(&last_sample,
		    decoded.bytes.data() + (15 * sizeof(std::uint16_t)), sizeof(last_sample));
		expect_eq(first_sample, static_cast<std::uint16_t>(1),
		    "DicomFile::pixel_buffer first-frame first sample");
		expect_eq(last_sample, static_cast<std::uint16_t>(16),
		    "DicomFile::pixel_buffer first-frame last sample");
		expect_true(decode_info.photometric.has_value(),
		    "DicomFile::pixel_buffer photometric presence");
		expect_eq(*decode_info.photometric, dicom::pixel::Photometric::monochrome2,
		    "DicomFile::pixel_buffer photometric");
		expect_eq(decode_info.encoded_lossy_state,
		    dicom::pixel::EncodedLossyState::lossless,
		    "DicomFile::pixel_buffer encoded lossy state");
		expect_true(decode_info.data_type.has_value(),
		    "DicomFile::pixel_buffer dtype presence");
		expect_eq(*decode_info.data_type, dicom::pixel::DataType::u16,
		    "DicomFile::pixel_buffer dtype");
		expect_true(decode_info.planar.has_value(),
		    "DicomFile::pixel_buffer planar presence");
		expect_eq(*decode_info.planar, dicom::pixel::Planar::interleaved,
		    "DicomFile::pixel_buffer planar");
		expect_eq(decode_info.bits_per_sample, static_cast<std::uint16_t>(16),
		    "DicomFile::pixel_buffer bits per sample");
	}

	{
		const auto plan = df.create_decode_plan(dicom::pixel::DecodeOptions{});
		dicom::pixel::DecodeInfo decode_info{};
		const auto decoded = dicom::pixel::decode_all_frames(df, plan, decode_info);
		std::uint16_t first_sample = 0;
		std::uint16_t first_frame_last = 0;
		std::uint16_t second_frame_first = 0;
		std::memcpy(&first_sample, decoded.bytes.data(), sizeof(first_sample));
		std::memcpy(&first_frame_last,
		    decoded.bytes.data() + (15 * sizeof(std::uint16_t)), sizeof(first_frame_last));
		std::memcpy(&second_frame_first,
		    decoded.bytes.data() + plan.output_layout.frame_stride,
		    sizeof(second_frame_first));
		expect_eq(first_sample, static_cast<std::uint16_t>(1),
		    "decode_all_frames first-frame first sample");
		expect_eq(first_frame_last, static_cast<std::uint16_t>(16),
		    "decode_all_frames first-frame last sample");
		expect_eq(second_frame_first, static_cast<std::uint16_t>(101),
		    "decode_all_frames second-frame first sample");
		expect_true(decode_info.photometric.has_value(),
		    "decode_all_frames photometric presence");
		expect_eq(*decode_info.photometric, dicom::pixel::Photometric::monochrome2,
		    "decode_all_frames photometric");
		expect_eq(decode_info.encoded_lossy_state,
		    dicom::pixel::EncodedLossyState::lossless,
		    "decode_all_frames encoded lossy state");
		expect_true(decode_info.data_type.has_value(),
		    "decode_all_frames dtype presence");
		expect_eq(*decode_info.data_type, dicom::pixel::DataType::u16,
		    "decode_all_frames dtype");
		expect_true(decode_info.planar.has_value(),
		    "decode_all_frames planar presence");
		expect_eq(*decode_info.planar, dicom::pixel::Planar::interleaved,
		    "decode_all_frames planar");
		expect_eq(decode_info.bits_per_sample, static_cast<std::uint16_t>(16),
		    "decode_all_frames bits per sample");
	}

	{
		const std::vector<std::uint8_t> cmyk_bytes{
		    0x10u, 0x20u, 0x30u, 0x40u, 0x50u, 0x60u, 0x70u, 0x80u};
		const dicom::pixel::PixelLayout cmyk_layout{
		    .data_type = dicom::pixel::DataType::u8,
		    .photometric = dicom::pixel::Photometric::cmyk,
		    .planar = dicom::pixel::Planar::interleaved,
		    .reserved = 0,
		    .rows = 1,
		    .cols = 2,
		    .frames = 1,
		    .samples_per_pixel = 4,
		    .bits_stored = 8,
		    .row_stride = 8,
		    .frame_stride = 8,
		};
		dicom::DicomFile cmyk_df{};
		configure_native_file(cmyk_df, cmyk_bytes, cmyk_layout);
		expect_true(cmyk_df.set_value("PhotometricInterpretation"_tag, std::string_view("cmyk")),
		    "set lowercase cmyk photometric");
		const auto plan = cmyk_df.create_decode_plan(dicom::pixel::DecodeOptions{});
		expect_eq(plan.output_layout.photometric, dicom::pixel::Photometric::cmyk,
		    "cmyk decode plan photometric");
		dicom::pixel::DecodeInfo decode_info{};
		const auto decoded = cmyk_df.pixel_buffer(0, plan, decode_info);
		expect_true(decoded.bytes.size() == cmyk_bytes.size() &&
		        std::memcmp(decoded.bytes.data(), cmyk_bytes.data(), cmyk_bytes.size()) == 0,
		    "cmyk decode payload");
		expect_true(decode_info.photometric.has_value(), "cmyk decode photometric presence");
		expect_eq(*decode_info.photometric, dicom::pixel::Photometric::cmyk,
		    "cmyk decode photometric");
	}

	{
		const std::vector<std::uint8_t> ybr_bytes{
		    0x11u, 0x22u, 0x33u, 0x44u, 0x55u, 0x66u};
		const dicom::pixel::PixelLayout ybr_layout{
		    .data_type = dicom::pixel::DataType::u8,
		    .photometric = dicom::pixel::Photometric::ybr_partial_422,
		    .planar = dicom::pixel::Planar::interleaved,
		    .reserved = 0,
		    .rows = 1,
		    .cols = 2,
		    .frames = 1,
		    .samples_per_pixel = 3,
		    .bits_stored = 8,
		    .row_stride = 6,
		    .frame_stride = 6,
		};
		dicom::DicomFile ybr_df{};
		configure_native_file(ybr_df, ybr_bytes, ybr_layout);
		expect_true(ybr_df.set_value("PhotometricInterpretation"_tag,
		        std::string_view("ybr_partial_422")),
		    "set lowercase ybr_partial_422 photometric");
		const auto plan = ybr_df.create_decode_plan(dicom::pixel::DecodeOptions{});
		expect_eq(plan.output_layout.photometric,
		    dicom::pixel::Photometric::ybr_partial_422,
		    "ybr_partial_422 decode plan photometric");
		dicom::pixel::DecodeInfo decode_info{};
		const auto decoded = ybr_df.pixel_buffer(0, plan, decode_info);
		expect_true(decoded.bytes.size() == ybr_bytes.size() &&
		        std::memcmp(decoded.bytes.data(), ybr_bytes.data(), ybr_bytes.size()) == 0,
		    "ybr_partial_422 decode payload");
		expect_true(decode_info.photometric.has_value(),
		    "ybr_partial_422 decode photometric presence");
		expect_eq(*decode_info.photometric, dicom::pixel::Photometric::ybr_partial_422,
		    "ybr_partial_422 decode photometric");
	}

	{
		const std::vector<std::uint8_t> ybr_rct_bytes{
		    0x21u, 0x31u, 0x41u, 0x51u, 0x61u, 0x71u};
		const dicom::pixel::PixelLayout ybr_rct_layout{
		    .data_type = dicom::pixel::DataType::u8,
		    .photometric = dicom::pixel::Photometric::ybr_rct,
		    .planar = dicom::pixel::Planar::interleaved,
		    .reserved = 0,
		    .rows = 1,
		    .cols = 2,
		    .frames = 1,
		    .samples_per_pixel = 3,
		    .bits_stored = 8,
		    .row_stride = 6,
		    .frame_stride = 6,
		};
		dicom::DicomFile ybr_rct_df{};
		configure_native_file(ybr_rct_df, ybr_rct_bytes, ybr_rct_layout);
		expect_true(ybr_rct_df.set_value("PhotometricInterpretation"_tag,
		        std::string_view("ybr_rct")),
		    "set lowercase ybr_rct photometric");
		dicom::pixel::DecodeInfo decode_info{};
		const auto decoded = ybr_rct_df.pixel_buffer(0, ybr_rct_df.create_decode_plan(), decode_info);
		expect_true(decoded.bytes.size() == ybr_rct_bytes.size() &&
		        std::memcmp(decoded.bytes.data(), ybr_rct_bytes.data(), ybr_rct_bytes.size()) == 0,
		    "ybr_rct decode payload");
		expect_true(decode_info.photometric.has_value(),
		    "ybr_rct decode photometric presence");
		expect_eq(*decode_info.photometric, dicom::pixel::Photometric::ybr_rct,
		    "ybr_rct decode photometric");
	}

	{
		const std::vector<std::uint8_t> argb_bytes{
		    0x01u, 0x10u, 0x20u, 0x30u, 0x02u, 0x40u, 0x50u, 0x60u};
		const dicom::pixel::PixelLayout argb_layout{
		    .data_type = dicom::pixel::DataType::u8,
		    .photometric = dicom::pixel::Photometric::argb,
		    .planar = dicom::pixel::Planar::interleaved,
		    .reserved = 0,
		    .rows = 1,
		    .cols = 2,
		    .frames = 1,
		    .samples_per_pixel = 4,
		    .bits_stored = 8,
		    .row_stride = 8,
		    .frame_stride = 8,
		};
		dicom::DicomFile argb_df{};
		configure_native_file(argb_df, argb_bytes, argb_layout);
		expect_true(argb_df.set_value("PhotometricInterpretation"_tag,
		        std::string_view("argb")),
		    "set lowercase argb photometric");
		dicom::pixel::DecodeInfo decode_info{};
		const auto decoded = argb_df.pixel_buffer(0, argb_df.create_decode_plan(), decode_info);
		expect_true(decoded.bytes.size() == argb_bytes.size() &&
		        std::memcmp(decoded.bytes.data(), argb_bytes.data(), argb_bytes.size()) == 0,
		    "argb decode payload");
		expect_true(decode_info.photometric.has_value(),
		    "argb decode photometric presence");
		expect_eq(*decode_info.photometric, dicom::pixel::Photometric::argb,
		    "argb decode photometric");
	}

	{
		const std::vector<std::uint8_t> rgb_bytes{
		    0x10u, 0x20u, 0x30u, 0x40u, 0x50u, 0x60u,
		    0x70u, 0x80u, 0x90u, 0xA0u, 0xB0u, 0xC0u};
		const dicom::pixel::PixelLayout rgb_layout{
		    .data_type = dicom::pixel::DataType::u8,
		    .photometric = dicom::pixel::Photometric::rgb,
		    .planar = dicom::pixel::Planar::interleaved,
		    .reserved = 0,
		    .rows = 2,
		    .cols = 2,
		    .frames = 1,
		    .samples_per_pixel = 3,
		    .bits_stored = 8,
		    .row_stride = 6,
		    .frame_stride = 12,
		};
		dicom::DicomFile rgb_df{};
		configure_native_file(rgb_df, rgb_bytes, rgb_layout);
		dicom::pixel::DecodeOptions options{};
		options.planar_out = dicom::pixel::Planar::planar;
		const auto plan = rgb_df.create_decode_plan(options);
		dicom::pixel::DecodeInfo decode_info{};
		const auto decoded = rgb_df.pixel_buffer(0, plan, decode_info);
		expect_true(!decoded.bytes.empty(), "rgb planar decode payload");
		expect_true(decode_info.photometric.has_value(),
		    "rgb planar decode photometric presence");
		expect_eq(*decode_info.photometric, dicom::pixel::Photometric::rgb,
		    "rgb planar decode photometric");
	}

	if (dicom::test::kJpegLsBuiltin || dicom::test::kJpeg2kBuiltin ||
	    dicom::test::kHtj2kBuiltin) {
		const std::vector<std::uint8_t> color_bytes{
		    0x00u, 0x10u, 0x20u, 0x30u, 0x40u, 0x50u,
		    0x60u, 0x70u, 0x80u, 0x90u, 0xA0u, 0xB0u};
		const dicom::pixel::PixelLayout color_layout{
		    .data_type = dicom::pixel::DataType::u8,
		    .photometric = dicom::pixel::Photometric::rgb,
		    .planar = dicom::pixel::Planar::interleaved,
		    .reserved = 0,
		    .rows = 2,
		    .cols = 2,
		    .frames = 1,
		    .samples_per_pixel = 3,
		    .bits_stored = 8,
		    .row_stride = 6,
		    .frame_stride = 12,
		};

		const auto expect_color_codec_photometric =
		    [&](std::string_view label, dicom::uid::WellKnown ts) {
			    dicom::DicomFile color_df{};
			    configure_native_file(color_df, color_bytes, color_layout);
			    color_df.set_transfer_syntax(ts);
			    const auto plan = color_df.create_decode_plan();
			    dicom::pixel::DecodeInfo decode_info{};
			    const auto decoded = color_df.pixel_buffer(0, plan, decode_info);
			    expect_true(!decoded.bytes.empty(), std::string(label) + " decode payload");
			    expect_true(decode_info.photometric.has_value(),
			        std::string(label) + " decode photometric presence");
			    expect_eq(*decode_info.photometric, plan.output_layout.photometric,
			        std::string(label) + " decode photometric");
		    };

		if (dicom::test::kJpegLsBuiltin) {
			expect_color_codec_photometric("jpegls rgb", "JPEGLSLossless"_uid);
		}
		if (dicom::test::kJpeg2kBuiltin) {
			expect_color_codec_photometric("jpeg2000 rgb", "JPEG2000Lossless"_uid);
		}
		if (dicom::test::kHtj2kBuiltin) {
			expect_color_codec_photometric("htj2k rgb", "HTJ2KLossless"_uid);
		}
	}

	return 0;
}
