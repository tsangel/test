#include <cstdlib>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <dicom.h>

#include "codec_builtin_flags.hpp"
#include "../src/pixel/host/error/codec_error.hpp"

namespace {
using namespace dicom::literals;

[[noreturn]] void fail(const std::string& message) {
	std::cerr << message << std::endl;
	std::exit(1);
}

void expect_contains(std::string_view haystack, std::string_view needle,
    std::string_view label) {
	if (haystack.find(needle) == std::string_view::npos) {
		fail(std::string(label) + " missing token: " + std::string(needle));
	}
}

void expect_not_contains(std::string_view haystack, std::string_view needle,
    std::string_view label) {
	if (haystack.find(needle) != std::string_view::npos) {
		fail(std::string(label) + " contains forbidden token: " + std::string(needle));
	}
}

void set_long_element(dicom::DicomFile& df, dicom::Tag tag, dicom::VR vr, long value,
    std::string_view label) {
	auto& elem = df.add_dataelement(tag, vr);
	if (!elem.from_long(value)) {
		fail(std::string("failed to set ") + std::string(label));
	}
}

void configure_minimal_integral_pixel_metadata(dicom::DicomFile& df) {
	set_long_element(df, "Rows"_tag, dicom::VR::US, 1, "Rows");
	set_long_element(df, "Columns"_tag, dicom::VR::US, 1, "Columns");
	set_long_element(df, "SamplesPerPixel"_tag, dicom::VR::US, 1, "SamplesPerPixel");
	set_long_element(df, "BitsAllocated"_tag, dicom::VR::US, 16, "BitsAllocated");
	set_long_element(df, "BitsStored"_tag, dicom::VR::US, 16, "BitsStored");
	set_long_element(df, "PixelRepresentation"_tag, dicom::VR::US, 0, "PixelRepresentation");
	set_long_element(df, "NumberOfFrames"_tag, dicom::VR::IS, 1, "NumberOfFrames");
	if (!df.set_value("PhotometricInterpretation"_tag, std::string_view("MONOCHROME2"))) {
		fail("failed to set PhotometricInterpretation");
	}
}

void expect_decode_throw(std::string_view label, dicom::DicomFile& df,
    std::size_t frame_index, std::span<std::uint8_t> dst,
    const dicom::pixel::DecodePlan& plan,
    std::string_view expected_ts, std::string_view expected_status,
    std::string_view expected_stage) {
	try {
		dicom::pixel::decode_frame_into(df, frame_index, dst, plan);
		fail(std::string(label) + " should throw");
	} catch (const std::exception& e) {
		const std::string what = e.what();
		expect_contains(what, "pixel::decode_frame_into", label);
		expect_contains(what, std::string("ts=") + std::string(expected_ts), label);
		expect_contains(what, "status=" + std::string(expected_status), label);
		expect_contains(what, "stage=" + std::string(expected_stage), label);
	}
}

void expect_create_decode_plan_throw(
    std::string_view label, dicom::DicomFile& df, std::string_view expected_reason) {
	try {
		(void)df.create_decode_plan(dicom::pixel::DecodeOptions{});
		fail(std::string(label) + " should throw");
	} catch (const std::exception& e) {
		const std::string what = e.what();
		expect_contains(what, "DicomFile::calc_decode_strides", label);
		expect_contains(what, "reason=" + std::string(expected_reason), label);
	}
}

} // namespace

int main() {
	using dicom::pixel::detail::CodecError;
	using dicom::pixel::detail::CodecStatusCode;
	using dicom::pixel::detail::format_codec_error_context;
	using dicom::pixel::detail::throw_codec_error_with_context;

	if (!(dicom::test::kJpegBuiltin &&
	        dicom::test::kJpegLsBuiltin &&
	        dicom::test::kJpeg2kBuiltin &&
	        dicom::test::kHtj2kBuiltin &&
	        dicom::test::kJpegXlBuiltin)) {
		return 0;
	}

	const CodecError frame_error{
	    .code = CodecStatusCode::backend_error,
	    .stage = "encode_frame",
	    .detail = "CharLS encode failed (simulated)",
	};
	const auto frame_message = format_codec_error_context(
	    "DicomFile::set_pixel_data", "/tmp/codec_error_test.dcm",
	    "JPEGLSLossless"_uid, std::size_t{2}, frame_error);
	expect_contains(frame_message, "DicomFile::set_pixel_data", "frame message");
	expect_contains(frame_message, "file=/tmp/codec_error_test.dcm", "frame message");
	expect_contains(
	    frame_message, std::string("ts=") + std::string("JPEGLSLossless"_uid.value()),
	    "frame message");
	expect_contains(frame_message, "frame=2", "frame message");
	expect_contains(frame_message, "status=backend_error", "frame message");
	expect_contains(frame_message, "stage=encode_frame", "frame message");
	expect_contains(
	    frame_message, "reason=CharLS encode failed (simulated)", "frame message");

	try {
		throw_codec_error_with_context("DicomFile::set_pixel_data",
		    "/tmp/codec_error_test.dcm", "JPEGXLLossless"_uid, std::nullopt,
		    CodecError{
		        .code = CodecStatusCode::unsupported,
		        .stage = "plugin_lookup",
		        .detail = "plugin is not registered in runtime registry",
		    });
		fail("throw_codec_error_with_context should throw");
	} catch (const std::exception& e) {
		const std::string what = e.what();
		expect_contains(what, "DicomFile::set_pixel_data", "throw message");
		expect_contains(what, "file=/tmp/codec_error_test.dcm", "throw message");
		expect_contains(
		    what, std::string("ts=") + std::string("JPEGXLLossless"_uid.value()),
		    "throw message");
		expect_contains(what, "status=unsupported", "throw message");
		expect_contains(what, "stage=plugin_lookup", "throw message");
		expect_contains(
		    what, "reason=plugin is not registered in runtime registry", "throw message");
	}

	{
		dicom::DicomFile df{};
		df.set_transfer_syntax("ExplicitVRLittleEndian"_uid);
		expect_decode_throw("uninitialized decode plan throw message", df, 0,
		    std::span<std::uint8_t>{}, dicom::pixel::DecodePlan{},
		    "ExplicitVRLittleEndian"_uid.value(), "invalid_argument", "validate_plan");
	}

	for (const auto [label, ts] : {
	         std::pair<std::string_view, dicom::uid::WellKnown>{
	             "jpeg uninitialized decode plan throw message", "JPEGBaseline8Bit"_uid},
	         {"jpegls uninitialized decode plan throw message", "JPEGLSLossless"_uid},
	         {"jpegxl uninitialized decode plan throw message", "JPEGXLLossless"_uid},
	         {"jpeg2k uninitialized decode plan throw message", "JPEG2000Lossless"_uid},
	         {"htj2k uninitialized decode plan throw message", "HTJ2KLossless"_uid},
	         {"rle uninitialized decode plan throw message", "RLELossless"_uid},
	     }) {
		dicom::DicomFile df{};
		df.set_transfer_syntax(ts);
		expect_decode_throw(label, df, 0, std::span<std::uint8_t>{},
		    dicom::pixel::DecodePlan{}, ts.value(),
		    "invalid_argument", "validate_plan");
	}

	{
		dicom::DicomFile df{};
		df.set_transfer_syntax("ExplicitVRLittleEndian"_uid);
		configure_minimal_integral_pixel_metadata(df);
		const auto plan = df.create_decode_plan(dicom::pixel::DecodeOptions{});
		std::vector<std::uint8_t> dst(plan.strides.frame, std::uint8_t{0});
		expect_decode_throw("native load_frame_source throw message", df, 0,
		    std::span<std::uint8_t>(dst.data(), dst.size()), plan,
		    "ExplicitVRLittleEndian"_uid.value(),
		    "invalid_argument", "load_frame_source");
	}

	{
		dicom::DicomFile df{};
		df.set_transfer_syntax("ExplicitVRLittleEndian"_uid);
		configure_minimal_integral_pixel_metadata(df);
		df.set_native_pixel_data(std::vector<std::uint8_t>{0x00, 0x00});

		const auto plan = df.create_decode_plan(dicom::pixel::DecodeOptions{});
		std::vector<std::uint8_t> dst(plan.strides.frame, std::uint8_t{0});
		try {
			dicom::pixel::decode_frame_into(df, 1,
			    std::span<std::uint8_t>(dst.data(), dst.size()), plan);
			fail("native frame index throw message should throw");
			} catch (const std::exception& e) {
				const std::string what = e.what();
				expect_contains(what, "frame=1", "native frame index throw message");
				expect_contains(what, "status=invalid_argument",
			    "native frame index throw message");
			expect_contains(what, "stage=load_frame_source",
			    "native frame index throw message");
			expect_contains(what, "raw frame index out of range",
			    "native frame index throw message");
		}
	}

	{
		dicom::DicomFile df{};
		df.set_transfer_syntax("ExplicitVRLittleEndian"_uid);
		configure_minimal_integral_pixel_metadata(df);
		set_long_element(df, "Rows"_tag, dicom::VR::US, 0, "Rows");
		df.set_native_pixel_data(std::vector<std::uint8_t>{0x00, 0x00});
		expect_create_decode_plan_throw("native metadata create_decode_plan throw message", df,
		    "invalid Rows/Columns/SamplesPerPixel");
	}

	for (const auto [label, ts] : {
	         std::pair<std::string_view, dicom::uid::WellKnown>{
	             "jpeg load_frame_source throw message", "JPEGBaseline8Bit"_uid},
	         {"jpegls load_frame_source throw message", "JPEGLSLossless"_uid},
	         {"jpegxl load_frame_source throw message", "JPEGXLLossless"_uid},
	         {"jpeg2k load_frame_source throw message", "JPEG2000Lossless"_uid},
	         {"htj2k load_frame_source throw message", "HTJ2KLossless"_uid},
	         {"rle load_frame_source throw message", "RLELossless"_uid},
	     }) {
		dicom::DicomFile df{};
		df.set_transfer_syntax(ts);
		configure_minimal_integral_pixel_metadata(df);
		const auto plan = df.create_decode_plan(dicom::pixel::DecodeOptions{});
		std::vector<std::uint8_t> dst(plan.strides.frame, std::uint8_t{0});
		expect_decode_throw(label, df, 0,
		    std::span<std::uint8_t>(dst.data(), dst.size()), plan, ts.value(), "invalid_argument",
		    "load_frame_source");
	}

	{
		dicom::DicomFile df{};
		df.set_transfer_syntax("JPEG2000Lossless"_uid);
		configure_minimal_integral_pixel_metadata(df);
		df.reset_encapsulated_pixel_data(1);
		df.set_encoded_pixel_frame(0, std::vector<std::uint8_t>{0x00, 0x01, 0x02, 0x03});

		const auto plan = df.create_decode_plan(dicom::pixel::DecodeOptions{});
		std::vector<std::uint8_t> dst(plan.strides.frame, std::uint8_t{0});
		try {
			dicom::pixel::decode_frame_into(df, 0,
			    std::span<std::uint8_t>(dst.data(), dst.size()), plan);
			fail("jpeg2k backend decode throw message should throw");
			} catch (const std::exception& e) {
				const std::string what = e.what();
				expect_contains(what, "status=backend_error",
				    "jpeg2k backend decode throw message");
			expect_contains(what, "stage=decode_frame",
			    "jpeg2k backend decode throw message");
			expect_contains(what, "reason=", "jpeg2k backend decode throw message");
			expect_not_contains(what, "reason=pixel::decode_frame_into ",
			    "jpeg2k backend decode throw message");
		}
	}

	{
		dicom::DicomFile df{};
		df.set_transfer_syntax("HTJ2KLossless"_uid);
		configure_minimal_integral_pixel_metadata(df);
		df.reset_encapsulated_pixel_data(1);
		df.set_encoded_pixel_frame(0, std::vector<std::uint8_t>{0x00, 0x01, 0x02, 0x03});

		const auto plan = df.create_decode_plan(dicom::pixel::DecodeOptions{});
		std::vector<std::uint8_t> dst(plan.strides.frame, std::uint8_t{0});
		try {
			dicom::pixel::decode_frame_into(df, 0,
			    std::span<std::uint8_t>(dst.data(), dst.size()), plan);
			fail("htj2k backend decode throw message should throw");
			} catch (const std::exception& e) {
				const std::string what = e.what();
				expect_contains(what, "status=backend_error",
				    "htj2k backend decode throw message");
			expect_contains(what, "stage=decode_frame",
			    "htj2k backend decode throw message");
			expect_contains(what, "reason=", "htj2k backend decode throw message");
			expect_not_contains(what, "reason=pixel::decode_frame_into ",
			    "htj2k backend decode throw message");
		}
	}

	{
		dicom::DicomFile df{};
		df.set_transfer_syntax("JPEG2000Lossless"_uid);
		configure_minimal_integral_pixel_metadata(df);
		df.reset_encapsulated_pixel_data(1);
		df.set_encoded_pixel_frame(0, std::vector<std::uint8_t>{0x00, 0x01, 0x02, 0x03});

		auto decode_opt = dicom::pixel::DecodeOptions{};
		decode_opt.codec_threads = -2;
		const auto plan = df.create_decode_plan(decode_opt);
		std::vector<std::uint8_t> dst(plan.strides.frame, std::uint8_t{0});
		try {
			dicom::pixel::decode_frame_into(df, 0,
			    std::span<std::uint8_t>(dst.data(), dst.size()), plan);
			fail("jpeg2k option throw message should throw");
			} catch (const std::exception& e) {
				const std::string what = e.what();
				expect_contains(what, "reason=", "jpeg2k option throw message");
			expect_not_contains(what, "reason=pixel::decode_frame_into ",
			    "jpeg2k option throw message");
		}
	}

	{
		dicom::DicomFile df{};
		df.set_transfer_syntax("MPEG2MPML"_uid);
		configure_minimal_integral_pixel_metadata(df);
		// Provide an encapsulated PixelData sequence so the decode path reaches codec lookup.
		df.reset_encapsulated_pixel_data(1);
		df.set_encoded_pixel_frame(0, std::vector<std::uint8_t>{0x00});
		const auto plan = df.create_decode_plan(dicom::pixel::DecodeOptions{});
		expect_decode_throw("unsupported ts decode throw message", df, 0,
		    std::span<std::uint8_t>{}, plan,
		    "MPEG2MPML"_uid.value(), "unsupported",
		    "plugin_lookup");
	}

	{
		dicom::DicomFile df{};
		df.set_transfer_syntax("ExplicitVRLittleEndian"_uid);
		configure_minimal_integral_pixel_metadata(df);
		df.set_native_pixel_data(std::vector<std::uint8_t>{0x34, 0x12});
		df.set_transfer_syntax("RLELossless"_uid);

		const auto baseline_frame = df.pixel_data(0);
		if (baseline_frame != std::vector<std::uint8_t>{0x34, 0x12}) {
			fail("encapsulated baseline decode mismatch");
		}

		std::string long_option_key(129, 'k');
		const std::array<dicom::pixel::CodecOptionTextKv, 1> invalid_options{{
		    {long_option_key, "1"},
		}};
		try {
			df.set_transfer_syntax(
			    "JPEG2000Lossless"_uid,
			    std::span<const dicom::pixel::CodecOptionTextKv>(invalid_options));
			fail("encapsulated transcode invalid option should throw");
		} catch (const std::exception& e) {
			const std::string what = e.what();
			expect_contains(what, "max_option_key_bytes",
			    "encapsulated transcode invalid option should throw");
		}

		if (df.transfer_syntax_uid() != "RLELossless"_uid) {
			fail("encapsulated transcode failure should preserve source transfer syntax");
		}
		if (df.pixel_data(0) != baseline_frame) {
			fail("encapsulated transcode failure should preserve source pixel data");
		}
	}

	return 0;
}
