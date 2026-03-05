#include <cstdlib>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <dicom.h>
#include "codec_builtin_flags.hpp"
#include "../src/pixel/registry/codec_registry.hpp"
#include "../src/pixel/encode/core/encode_codec_impl_detail.hpp"

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

void expect_decode_plugin_or_runtime(std::string_view haystack,
    std::string_view plugin_key, std::string_view label) {
	const bool has_runtime =
	    haystack.find("plugin=runtime") != std::string_view::npos;
	if (has_runtime) {
		expect_contains(haystack, "plugin=runtime", label);
		return;
	}
	const std::string expected = std::string("plugin=") + std::string(plugin_key);
	expect_contains(haystack, expected, label);
}

void expect_native_or_runtime_plugin(std::string_view haystack,
    std::string_view label) {
	expect_decode_plugin_or_runtime(haystack, "native", label);
}

void expect_none_or_runtime_plugin(std::string_view haystack,
    std::string_view label) {
	if (haystack.find("plugin=runtime") != std::string_view::npos) {
		expect_contains(haystack, "plugin=runtime", label);
		return;
	}
	expect_contains(haystack, "plugin=<none>", label);
}

void expect_decode_load_frame_source_or_runtime_lookup_error(
    std::string_view what, std::string_view label) {
	const bool has_runtime_plugin =
	    what.find("plugin=runtime") != std::string_view::npos;
	if (has_runtime_plugin) {
		const bool has_load_frame_source =
		    what.find("status=invalid_argument") != std::string_view::npos &&
		    what.find("stage=load_frame_source") != std::string_view::npos;
		const bool has_runtime_lookup =
		    what.find("status=unsupported") != std::string_view::npos &&
		    what.find("stage=plugin_lookup") != std::string_view::npos;
		if (!has_load_frame_source && !has_runtime_lookup) {
			fail(std::string(label) +
			    " missing expected runtime status/stage pair");
		}
		return;
	}
	expect_contains(what, "status=invalid_argument", label);
	expect_contains(what, "stage=load_frame_source", label);
}

void expect_missing_decode_plugin_or_runtime_native_error(
    std::string_view what, std::string_view label) {
	if (what.find("plugin=<none>") != std::string_view::npos) {
		expect_contains(what, "plugin=<none>", label);
		expect_contains(what, "status=unsupported", label);
		expect_contains(what, "stage=plugin_lookup", label);
		expect_contains(what,
		    "reason=transfer syntax is not supported for decode by codec registry binding",
		    label);
		return;
	}
	expect_native_or_runtime_plugin(what, label);
	expect_contains(what, "status=invalid_argument", label);
	expect_contains(what, "stage=load_frame_source", label);
	expect_contains(what, "missing PixelData", label);
}

void set_long_element(dicom::DicomFile& df, dicom::Tag tag, dicom::VR vr, long value,
    std::string_view label) {
	auto* elem = df.add_dataelement(tag, vr);
	if (!elem || !elem->from_long(value)) {
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
}

struct RegistrySnapshot {
	std::vector<dicom::pixel::detail::CodecPlugin> plugins{};
	std::vector<dicom::pixel::detail::TransferSyntaxPluginBinding> bindings{};
};

RegistrySnapshot snapshot_registry(
    const dicom::pixel::detail::CodecRegistry& registry) {
	RegistrySnapshot snapshot{};
	const auto plugins = registry.plugins();
	snapshot.plugins.assign(plugins.begin(), plugins.end());
	const auto bindings = registry.bindings();
	snapshot.bindings.assign(bindings.begin(), bindings.end());
	return snapshot;
}

void restore_registry(dicom::pixel::detail::CodecRegistry& registry,
    const RegistrySnapshot& snapshot) {
	registry.clear();
	for (const auto& plugin : snapshot.plugins) {
		if (!registry.register_plugin(plugin)) {
			fail("failed to restore codec plugin snapshot");
		}
	}
	for (const auto& binding : snapshot.bindings) {
		if (!registry.register_binding(binding)) {
			fail("failed to restore transfer syntax binding snapshot");
		}
	}
}

bool always_fail_encode_dispatch(
    const dicom::pixel::detail::CodecEncodeFrameInput&,
    std::span<const dicom::pixel::detail::CodecOptionKv>,
    std::vector<std::uint8_t>&, dicom::pixel::detail::CodecError& out_error) noexcept {
	out_error = {};
	return false;
}

void encode_with_plugin_or_throw(std::string_view plugin_key) {
	std::vector<std::uint8_t> source_frame{0x00u, 0x00u};
	dicom::DicomFile df{};
	const dicom::pixel::detail::EncapsulatedEncodeInput encode_input{
	    .source_base = source_frame.data(),
	    .frame_count = 1,
	    .source_frame_stride = 2,
	    .source_frame_size_bytes = 2,
	    .source_aliases_current_native_pixel_data = false,
	};
	const dicom::pixel::detail::CodecEncodeFnInput encode_fn_input{
	    .file = df,
	    .transfer_syntax = "JPEG2000Lossless"_uid,
	    .encode_input = encode_input,
	    .codec_options = std::span<const dicom::pixel::detail::CodecOptionKv>{},
	    .rows = 1,
	    .cols = 1,
	    .samples_per_pixel = 1,
	    .bytes_per_sample = 2,
	    .bits_allocated = 16,
	    .bits_stored = 16,
	    .pixel_representation = 0,
	    .use_multicomponent_transform = false,
	    .source_planar = dicom::pixel::Planar::interleaved,
	    .planar_source = false,
	    .row_payload_bytes = 2,
	    .source_row_stride = 2,
	    .source_plane_stride = 2,
	    .source_frame_size_bytes = 2,
	    .destination_frame_payload = 2,
	    .profile = dicom::pixel::detail::CodecProfile::jpeg2000_lossless,
	    .plugin_key = plugin_key,
	};
	dicom::pixel::detail::encode_encapsulated_pixel_data(encode_fn_input);
}

} // namespace

int main() {
	using dicom::pixel::detail::CodecError;
	using dicom::pixel::detail::CodecDecodeFrameInput;
	using dicom::pixel::detail::CodecStatusCode;
	using dicom::pixel::detail::format_codec_error_context;
	using dicom::pixel::detail::global_codec_registry;
	using dicom::pixel::detail::throw_codec_error_with_context;
	const dicom::pixel::detail::DecodeValueTransform decode_transform{};

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
	    "JPEGLSLossless"_uid, "jpegls", std::size_t{2}, frame_error);
	const auto expected_jpegls_ts =
	    std::string("ts=") + std::string("JPEGLSLossless"_uid.value());
	expect_contains(frame_message, "DicomFile::set_pixel_data", "frame message");
	expect_contains(frame_message, "file=/tmp/codec_error_test.dcm", "frame message");
	expect_contains(frame_message, expected_jpegls_ts, "frame message");
	expect_contains(frame_message, "plugin=jpegls", "frame message");
	expect_contains(frame_message, "frame=2", "frame message");
	expect_contains(frame_message, "status=backend_error", "frame message");
	expect_contains(frame_message, "stage=encode_frame", "frame message");
	expect_contains(frame_message, "reason=CharLS encode failed (simulated)", "frame message");

	try {
		const CodecError lookup_error{
		    .code = CodecStatusCode::unsupported,
		    .stage = "plugin_lookup",
		    .detail = "plugin is not registered in codec registry",
		};
		throw_codec_error_with_context("DicomFile::set_pixel_data",
		    "/tmp/codec_error_test.dcm", "JPEGXLLossless"_uid, "jpegxl",
		    std::nullopt, lookup_error);
		fail("throw_codec_error_with_context should throw");
	} catch (const std::exception& e) {
		const std::string what = e.what();
		const auto expected_jpegxl_ts =
		    std::string("ts=") + std::string("JPEGXLLossless"_uid.value());
		expect_contains(what, "DicomFile::set_pixel_data", "throw message");
		expect_contains(what, "file=/tmp/codec_error_test.dcm", "throw message");
		expect_contains(what, expected_jpegxl_ts, "throw message");
		expect_contains(what, "plugin=jpegxl", "throw message");
		expect_contains(what, "status=unsupported", "throw message");
		expect_contains(what, "stage=plugin_lookup", "throw message");
		expect_contains(
		    what, "reason=plugin is not registered in codec registry", "throw message");
	}

	{
		dicom::DicomFile df{};
		const auto& registry = global_codec_registry();
		const auto* native_plugin = registry.find_plugin("native");
		if (native_plugin && native_plugin->decode_frame) {
			const auto& info = df.pixeldata_info();
			CodecDecodeFrameInput decode_input{
			    .info = info,
			    .value_transform = decode_transform,
			    .destination = std::span<std::uint8_t>{},
			    .destination_strides = dicom::pixel::DecodeStrides{},
			    .options = dicom::pixel::DecodeOptions{},
			};
			CodecError decode_error{};
			const bool ok = native_plugin->decode_frame(decode_input, decode_error);
			if (ok) {
				fail("native decode_frame plugin should fail for empty DicomFile");
			}
			if (decode_error.code != CodecStatusCode::invalid_argument) {
				fail("native decode_frame plugin should report invalid_argument");
			}
			if (decode_error.stage != "validate") {
				fail("native decode_frame plugin should report validate stage");
			}
			expect_contains(decode_error.detail, "sv_dtype is unknown",
			    "native decode_frame plugin error");
		}
	}

	{
		const auto& registry = global_codec_registry();
		const auto* native_plugin = registry.find_plugin("native");
		if (native_plugin && native_plugin->decode_frame) {
			CodecDecodeFrameInput decode_input{};
			CodecError decode_error{};
			const bool ok = native_plugin->decode_frame(decode_input, decode_error);
			if (ok) {
				fail("native decode_frame plugin should fail for invalid input");
			}
			if (decode_error.code != CodecStatusCode::invalid_argument) {
				fail("native decode_frame plugin should report invalid_argument on bad input");
			}
			if (decode_error.stage != "validate") {
				fail("native decode_frame plugin should report validate stage on bad input");
			}
			expect_contains(decode_error.detail, "sv_dtype is unknown",
			    "native decode_frame bad input error");
		}
	}

	{
		dicom::DicomFile df{};
		const auto& registry = global_codec_registry();
		const auto* native_plugin = registry.find_plugin("native");
		if (native_plugin && native_plugin->decode_frame) {
			const auto& info = df.pixeldata_info();
			CodecDecodeFrameInput decode_input{
			    .info = info,
			    .value_transform = decode_transform,
			    .destination = std::span<std::uint8_t>{},
			    .destination_strides = dicom::pixel::DecodeStrides{},
			    .options = dicom::pixel::DecodeOptions{},
			};
			CodecError decode_error{};
			const bool ok = native_plugin->decode_frame(decode_input, decode_error);
			if (ok) {
				fail("native decode_frame plugin should fail for empty DicomFile");
			}
			if (decode_error.code != CodecStatusCode::invalid_argument) {
				fail("native decode_frame plugin should report invalid_argument");
			}
			if (decode_error.stage != "validate") {
				fail("native decode_frame plugin should report validate stage");
			}
			expect_contains(decode_error.detail, "sv_dtype is unknown",
			    "native decode_frame plugin error");
		}
	}

	{
		dicom::DicomFile df{};
		const auto& registry = global_codec_registry();
		const auto* rle_plugin = registry.find_plugin("rle");
		if (rle_plugin && rle_plugin->decode_frame) {
			const auto& info = df.pixeldata_info();
			CodecDecodeFrameInput decode_input{
			    .info = info,
			    .value_transform = decode_transform,
			    .destination = std::span<std::uint8_t>{},
			    .destination_strides = dicom::pixel::DecodeStrides{},
			    .options = dicom::pixel::DecodeOptions{},
			};
			CodecError decode_error{};
			const bool ok = rle_plugin->decode_frame(decode_input, decode_error);
			if (ok) {
				fail("rle decode_frame plugin should fail for empty DicomFile");
			}
			if (decode_error.code != CodecStatusCode::invalid_argument) {
				fail("rle decode_frame plugin should report invalid_argument");
			}
			if (decode_error.stage != "validate") {
				fail("rle decode_frame plugin should report validate stage");
			}
			expect_contains(decode_error.detail, "sv_dtype is unknown",
			    "rle decode_frame plugin error");
		}
	}

	{
		dicom::DicomFile df{};
		const auto& registry = global_codec_registry();
		const auto* jpeg_plugin = registry.find_plugin("jpeg");
		if (jpeg_plugin && jpeg_plugin->decode_frame) {
			const auto& info = df.pixeldata_info();
			CodecDecodeFrameInput decode_input{
			    .info = info,
			    .value_transform = decode_transform,
			    .destination = std::span<std::uint8_t>{},
			    .destination_strides = dicom::pixel::DecodeStrides{},
			    .options = dicom::pixel::DecodeOptions{},
			};
			CodecError decode_error{};
			const bool ok = jpeg_plugin->decode_frame(decode_input, decode_error);
			if (ok) {
				fail("jpeg decode_frame plugin should fail for empty DicomFile");
			}
			if (decode_error.code != CodecStatusCode::invalid_argument) {
				fail("jpeg decode_frame plugin should report invalid_argument");
			}
			if (decode_error.stage != "validate") {
				fail("jpeg decode_frame plugin should report validate stage");
			}
			expect_contains(decode_error.detail, "sv_dtype is unknown",
			    "jpeg decode_frame plugin error");
		}
	}

	{
		dicom::DicomFile df{};
		const auto& registry = global_codec_registry();
		const auto* jpegls_plugin = registry.find_plugin("jpegls");
		if (jpegls_plugin && jpegls_plugin->decode_frame) {
			const auto& info = df.pixeldata_info();
			CodecDecodeFrameInput decode_input{
			    .info = info,
			    .value_transform = decode_transform,
			    .destination = std::span<std::uint8_t>{},
			    .destination_strides = dicom::pixel::DecodeStrides{},
			    .options = dicom::pixel::DecodeOptions{},
			};
			CodecError decode_error{};
			const bool ok = jpegls_plugin->decode_frame(decode_input, decode_error);
			if (ok) {
				fail("jpegls decode_frame plugin should fail for empty DicomFile");
			}
			if (decode_error.code != CodecStatusCode::invalid_argument) {
				fail("jpegls decode_frame plugin should report invalid_argument");
			}
			if (decode_error.stage != "validate") {
				fail("jpegls decode_frame plugin should report validate stage");
			}
			expect_contains(decode_error.detail, "sv_dtype is unknown",
			    "jpegls decode_frame plugin error");
		}
	}

	{
		dicom::DicomFile df{};
		const auto& registry = global_codec_registry();
		const auto* jpegxl_plugin = registry.find_plugin("jpegxl");
		if (jpegxl_plugin && jpegxl_plugin->decode_frame) {
			const auto& info = df.pixeldata_info();
			CodecDecodeFrameInput decode_input{
			    .info = info,
			    .value_transform = decode_transform,
			    .destination = std::span<std::uint8_t>{},
			    .destination_strides = dicom::pixel::DecodeStrides{},
			    .options = dicom::pixel::DecodeOptions{},
			};
			CodecError decode_error{};
			const bool ok = jpegxl_plugin->decode_frame(decode_input, decode_error);
			if (ok) {
				fail("jpegxl decode_frame plugin should fail for empty DicomFile");
			}
			if (decode_error.code != CodecStatusCode::invalid_argument) {
				fail("jpegxl decode_frame plugin should report invalid_argument");
			}
			if (decode_error.stage != "validate") {
				fail("jpegxl decode_frame plugin should report validate stage");
			}
			expect_contains(decode_error.detail, "sv_dtype is unknown",
			    "jpegxl decode_frame plugin error");
		}
	}

	{
		dicom::DicomFile df{};
		const auto& registry = global_codec_registry();
		const auto* jpeg2k_plugin = registry.find_plugin("jpeg2k");
		if (jpeg2k_plugin && jpeg2k_plugin->decode_frame) {
			const auto& info = df.pixeldata_info();
			CodecDecodeFrameInput decode_input{
			    .info = info,
			    .value_transform = decode_transform,
			    .destination = std::span<std::uint8_t>{},
			    .destination_strides = dicom::pixel::DecodeStrides{},
			    .options = dicom::pixel::DecodeOptions{},
			};
			CodecError decode_error{};
			const bool ok = jpeg2k_plugin->decode_frame(decode_input, decode_error);
			if (ok) {
				fail("jpeg2k decode_frame plugin should fail for empty DicomFile");
			}
			if (decode_error.code != CodecStatusCode::invalid_argument) {
				fail("jpeg2k decode_frame plugin should report invalid_argument");
			}
			if (decode_error.stage != "validate") {
				fail("jpeg2k decode_frame plugin should report validate stage");
			}
			expect_contains(decode_error.detail, "sv_dtype is unknown",
			    "jpeg2k decode_frame plugin error");
		}
	}

	{
		dicom::DicomFile df{};
		const auto& registry = global_codec_registry();
		const auto* htj2k_plugin = registry.find_plugin("htj2k");
		if (htj2k_plugin && htj2k_plugin->decode_frame) {
			const auto& info = df.pixeldata_info();
			CodecDecodeFrameInput decode_input{
			    .info = info,
			    .value_transform = decode_transform,
			    .destination = std::span<std::uint8_t>{},
			    .destination_strides = dicom::pixel::DecodeStrides{},
			    .options = dicom::pixel::DecodeOptions{},
			};
			CodecError decode_error{};
			const bool ok = htj2k_plugin->decode_frame(decode_input, decode_error);
			if (ok) {
				fail("htj2k decode_frame plugin should fail for empty DicomFile");
			}
			if (decode_error.code != CodecStatusCode::invalid_argument) {
				fail("htj2k decode_frame plugin should report invalid_argument");
			}
			if (decode_error.stage != "validate") {
				fail("htj2k decode_frame plugin should report validate stage");
			}
			expect_contains(decode_error.detail, "sv_dtype is unknown",
			    "htj2k decode_frame plugin error");
		}
	}

	{
		dicom::DicomFile df{};
		df.set_transfer_syntax("ExplicitVRLittleEndian"_uid);
		try {
			dicom::pixel::decode_frame_into(df, 0, std::span<std::uint8_t>{},
			    dicom::pixel::DecodeStrides{}, dicom::pixel::DecodeOptions{});
			fail("decode_frame_into should throw for empty native pixel source");
		} catch (const std::exception& e) {
			const std::string what = e.what();
			const auto expected_ts =
			    std::string("ts=") + std::string("ExplicitVRLittleEndian"_uid.value());
			expect_contains(what, "pixel::decode_frame_into", "decode throw message");
			expect_contains(what, expected_ts, "decode throw message");
			expect_native_or_runtime_plugin(what, "decode throw message");
			expect_contains(what, "frame=0", "decode throw message");
			expect_contains(what, "status=invalid_argument", "decode throw message");
			expect_contains(what, "stage=load_frame_source", "decode throw message");
		}
	}

	{
		dicom::DicomFile df{};
		df.set_transfer_syntax("JPEGBaseline8Bit"_uid);
		try {
			dicom::pixel::decode_frame_into(df, 0, std::span<std::uint8_t>{},
			    dicom::pixel::DecodeStrides{}, dicom::pixel::DecodeOptions{});
			fail("decode_frame_into should throw for empty jpeg pixel source");
		} catch (const std::exception& e) {
			const std::string what = e.what();
			const auto expected_ts =
			    std::string("ts=") + std::string("JPEGBaseline8Bit"_uid.value());
			expect_contains(what, "pixel::decode_frame_into", "jpeg decode throw message");
			expect_contains(what, expected_ts, "jpeg decode throw message");
			expect_decode_plugin_or_runtime(what, "jpeg", "jpeg decode throw message");
			expect_contains(what, "frame=0", "jpeg decode throw message");
			expect_decode_load_frame_source_or_runtime_lookup_error(
			    what, "jpeg decode throw message");
		}
	}

	{
		dicom::DicomFile df{};
		df.set_transfer_syntax("JPEGLSLossless"_uid);
		try {
			dicom::pixel::decode_frame_into(df, 0, std::span<std::uint8_t>{},
			    dicom::pixel::DecodeStrides{}, dicom::pixel::DecodeOptions{});
			fail("decode_frame_into should throw for empty jpegls pixel source");
		} catch (const std::exception& e) {
			const std::string what = e.what();
			const auto expected_ts =
			    std::string("ts=") + std::string("JPEGLSLossless"_uid.value());
			expect_contains(what, "pixel::decode_frame_into", "jpegls decode throw message");
			expect_contains(what, expected_ts, "jpegls decode throw message");
			expect_decode_plugin_or_runtime(what, "jpegls", "jpegls decode throw message");
			expect_contains(what, "frame=0", "jpegls decode throw message");
			expect_decode_load_frame_source_or_runtime_lookup_error(
			    what, "jpegls decode throw message");
		}
	}

	{
		dicom::DicomFile df{};
		df.set_transfer_syntax("JPEGXLLossless"_uid);
		try {
			dicom::pixel::decode_frame_into(df, 0, std::span<std::uint8_t>{},
			    dicom::pixel::DecodeStrides{}, dicom::pixel::DecodeOptions{});
			fail("decode_frame_into should throw for empty jpegxl pixel source");
		} catch (const std::exception& e) {
			const std::string what = e.what();
			const auto expected_ts =
			    std::string("ts=") + std::string("JPEGXLLossless"_uid.value());
			expect_contains(what, "pixel::decode_frame_into", "jpegxl decode throw message");
			expect_contains(what, expected_ts, "jpegxl decode throw message");
			expect_decode_plugin_or_runtime(what, "jpegxl", "jpegxl decode throw message");
			expect_contains(what, "frame=0", "jpegxl decode throw message");
			expect_decode_load_frame_source_or_runtime_lookup_error(
			    what, "jpegxl decode throw message");
		}
	}

	{
		dicom::DicomFile df{};
		df.set_transfer_syntax("JPEG2000Lossless"_uid);
		try {
			dicom::pixel::decode_frame_into(df, 0, std::span<std::uint8_t>{},
			    dicom::pixel::DecodeStrides{}, dicom::pixel::DecodeOptions{});
			fail("decode_frame_into should throw for empty jpeg2k pixel source");
		} catch (const std::exception& e) {
			const std::string what = e.what();
			const auto expected_ts =
			    std::string("ts=") + std::string("JPEG2000Lossless"_uid.value());
			expect_contains(what, "pixel::decode_frame_into", "jpeg2k decode throw message");
			expect_contains(what, expected_ts, "jpeg2k decode throw message");
			expect_decode_plugin_or_runtime(what, "jpeg2k", "jpeg2k decode throw message");
			expect_contains(what, "frame=0", "jpeg2k decode throw message");
			expect_decode_load_frame_source_or_runtime_lookup_error(
			    what, "jpeg2k decode throw message");
		}
	}

	{
		dicom::DicomFile df{};
		df.set_transfer_syntax("HTJ2KLossless"_uid);
		try {
			dicom::pixel::decode_frame_into(df, 0, std::span<std::uint8_t>{},
			    dicom::pixel::DecodeStrides{}, dicom::pixel::DecodeOptions{});
			fail("decode_frame_into should throw for empty htj2k pixel source");
		} catch (const std::exception& e) {
			const std::string what = e.what();
			const auto expected_ts =
			    std::string("ts=") + std::string("HTJ2KLossless"_uid.value());
			expect_contains(what, "pixel::decode_frame_into", "htj2k decode throw message");
			expect_contains(what, expected_ts, "htj2k decode throw message");
			expect_decode_plugin_or_runtime(what, "htj2k", "htj2k decode throw message");
			expect_contains(what, "frame=0", "htj2k decode throw message");
			expect_decode_load_frame_source_or_runtime_lookup_error(
			    what, "htj2k decode throw message");
		}
	}

	{
		dicom::DicomFile df{};
		df.set_transfer_syntax("RLELossless"_uid);
		try {
			dicom::pixel::decode_frame_into(df, 0, std::span<std::uint8_t>{},
			    dicom::pixel::DecodeStrides{}, dicom::pixel::DecodeOptions{});
			fail("decode_frame_into should throw for empty rle pixel source");
		} catch (const std::exception& e) {
			const std::string what = e.what();
			const auto expected_ts =
			    std::string("ts=") + std::string("RLELossless"_uid.value());
			expect_contains(what, "pixel::decode_frame_into", "rle decode throw message");
			expect_contains(what, expected_ts, "rle decode throw message");
			expect_decode_plugin_or_runtime(what, "rle", "rle decode throw message");
			expect_contains(what, "frame=0", "rle decode throw message");
			expect_decode_load_frame_source_or_runtime_lookup_error(
			    what, "rle decode throw message");
		}
	}

	{
		dicom::DicomFile df{};
		df.set_transfer_syntax("JPEG2000Lossless"_uid);
		configure_minimal_integral_pixel_metadata(df);

		const auto strides = df.calc_decode_strides(dicom::pixel::DecodeOptions{});
		std::vector<std::uint8_t> dst(strides.frame, std::uint8_t{0});
		try {
			dicom::pixel::decode_frame_into(df, 0,
			    std::span<std::uint8_t>(dst.data(), dst.size()), strides,
			    dicom::pixel::DecodeOptions{});
			fail("decode_frame_into should throw when jpeg2k frame source is missing");
		} catch (const std::exception& e) {
			const std::string what = e.what();
			const auto expected_ts =
			    std::string("ts=") + std::string("JPEG2000Lossless"_uid.value());
			expect_contains(what, "pixel::decode_frame_into",
			    "jpeg2k load_frame_source throw message");
			expect_contains(what, expected_ts, "jpeg2k load_frame_source throw message");
			expect_decode_plugin_or_runtime(
			    what, "jpeg2k", "jpeg2k load_frame_source throw message");
			expect_decode_load_frame_source_or_runtime_lookup_error(
			    what, "jpeg2k load_frame_source throw message");
		}
	}

	{
		dicom::DicomFile df{};
		df.set_transfer_syntax("HTJ2KLossless"_uid);
		configure_minimal_integral_pixel_metadata(df);

		const auto strides = df.calc_decode_strides(dicom::pixel::DecodeOptions{});
		std::vector<std::uint8_t> dst(strides.frame, std::uint8_t{0});
		try {
			dicom::pixel::decode_frame_into(df, 0,
			    std::span<std::uint8_t>(dst.data(), dst.size()), strides,
			    dicom::pixel::DecodeOptions{});
			fail("decode_frame_into should throw when htj2k frame source is missing");
		} catch (const std::exception& e) {
			const std::string what = e.what();
			const auto expected_ts =
			    std::string("ts=") + std::string("HTJ2KLossless"_uid.value());
			expect_contains(what, "pixel::decode_frame_into",
			    "htj2k load_frame_source throw message");
			expect_contains(what, expected_ts, "htj2k load_frame_source throw message");
			expect_decode_plugin_or_runtime(
			    what, "htj2k", "htj2k load_frame_source throw message");
			expect_decode_load_frame_source_or_runtime_lookup_error(
			    what, "htj2k load_frame_source throw message");
		}
	}

		{
			dicom::DicomFile df{};
			df.set_transfer_syntax("ExplicitVRLittleEndian"_uid);
			configure_minimal_integral_pixel_metadata(df);

		const auto strides = df.calc_decode_strides(dicom::pixel::DecodeOptions{});
		std::vector<std::uint8_t> dst(strides.frame, std::uint8_t{0});
		try {
			dicom::pixel::decode_frame_into(df, 0,
			    std::span<std::uint8_t>(dst.data(), dst.size()), strides,
			    dicom::pixel::DecodeOptions{});
			fail("decode_frame_into should throw when native frame source is missing");
		} catch (const std::exception& e) {
			const std::string what = e.what();
			const auto expected_ts =
			    std::string("ts=") + std::string("ExplicitVRLittleEndian"_uid.value());
			expect_contains(
			    what, "pixel::decode_frame_into", "native load_frame_source throw message");
			expect_contains(what, expected_ts, "native load_frame_source throw message");
			expect_native_or_runtime_plugin(
			    what, "native load_frame_source throw message");
			expect_contains(what, "status=invalid_argument",
			    "native load_frame_source throw message");
			expect_contains(what, "stage=load_frame_source",
			    "native load_frame_source throw message");
			}
		}

		{
			dicom::DicomFile df{};
			df.set_transfer_syntax("ExplicitVRLittleEndian"_uid);
			configure_minimal_integral_pixel_metadata(df);
			df.set_native_pixel_data(std::vector<std::uint8_t>{0x00, 0x00});

			const auto strides = df.calc_decode_strides(dicom::pixel::DecodeOptions{});
			std::vector<std::uint8_t> dst(strides.frame, std::uint8_t{0});
			try {
				dicom::pixel::decode_frame_into(df, 1,
				    std::span<std::uint8_t>(dst.data(), dst.size()), strides,
				    dicom::pixel::DecodeOptions{});
				fail("decode_frame_into should throw when frame index is out of range");
			} catch (const std::exception& e) {
				const std::string what = e.what();
				const auto expected_ts =
				    std::string("ts=") + std::string("ExplicitVRLittleEndian"_uid.value());
				expect_contains(what, "pixel::decode_frame_into",
				    "native frame index throw message");
				expect_contains(what, expected_ts, "native frame index throw message");
				expect_native_or_runtime_plugin(
				    what, "native frame index throw message");
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

			try {
				dicom::pixel::decode_frame_into(df, 0, std::span<std::uint8_t>{},
				    dicom::pixel::DecodeStrides{}, dicom::pixel::DecodeOptions{});
				fail("decode_frame_into should throw for invalid native metadata");
			} catch (const std::exception& e) {
				const std::string what = e.what();
				const auto expected_ts =
				    std::string("ts=") + std::string("ExplicitVRLittleEndian"_uid.value());
				expect_contains(what, "pixel::decode_frame_into",
				    "native metadata throw message");
				expect_contains(what, expected_ts, "native metadata throw message");
				expect_native_or_runtime_plugin(
				    what, "native metadata throw message");
				expect_contains(what, "status=invalid_argument",
				    "native metadata throw message");
				expect_contains(what, "stage=load_frame_source",
				    "native metadata throw message");
				expect_contains(what, "invalid raw pixel metadata",
				    "native metadata throw message");
			}
		}

		{
			dicom::DicomFile df{};
			df.set_transfer_syntax("JPEGBaseline8Bit"_uid);
			configure_minimal_integral_pixel_metadata(df);

		const auto strides = df.calc_decode_strides(dicom::pixel::DecodeOptions{});
		std::vector<std::uint8_t> dst(strides.frame, std::uint8_t{0});
		try {
			dicom::pixel::decode_frame_into(df, 0,
			    std::span<std::uint8_t>(dst.data(), dst.size()), strides,
			    dicom::pixel::DecodeOptions{});
			fail("decode_frame_into should throw when jpeg frame source is missing");
		} catch (const std::exception& e) {
			const std::string what = e.what();
			const auto expected_ts =
			    std::string("ts=") + std::string("JPEGBaseline8Bit"_uid.value());
			expect_contains(
			    what, "pixel::decode_frame_into", "jpeg load_frame_source throw message");
			expect_contains(what, expected_ts, "jpeg load_frame_source throw message");
			expect_decode_plugin_or_runtime(
			    what, "jpeg", "jpeg load_frame_source throw message");
			expect_decode_load_frame_source_or_runtime_lookup_error(
			    what, "jpeg load_frame_source throw message");
		}
	}

	{
		dicom::DicomFile df{};
		df.set_transfer_syntax("JPEGLSLossless"_uid);
		configure_minimal_integral_pixel_metadata(df);

		const auto strides = df.calc_decode_strides(dicom::pixel::DecodeOptions{});
		std::vector<std::uint8_t> dst(strides.frame, std::uint8_t{0});
		try {
			dicom::pixel::decode_frame_into(df, 0,
			    std::span<std::uint8_t>(dst.data(), dst.size()), strides,
			    dicom::pixel::DecodeOptions{});
			fail("decode_frame_into should throw when jpegls frame source is missing");
		} catch (const std::exception& e) {
			const std::string what = e.what();
			const auto expected_ts =
			    std::string("ts=") + std::string("JPEGLSLossless"_uid.value());
			expect_contains(
			    what, "pixel::decode_frame_into", "jpegls load_frame_source throw message");
			expect_contains(what, expected_ts, "jpegls load_frame_source throw message");
			expect_decode_plugin_or_runtime(
			    what, "jpegls", "jpegls load_frame_source throw message");
			expect_decode_load_frame_source_or_runtime_lookup_error(
			    what, "jpegls load_frame_source throw message");
		}
	}

	{
		dicom::DicomFile df{};
		df.set_transfer_syntax("JPEGXLLossless"_uid);
		configure_minimal_integral_pixel_metadata(df);

		const auto strides = df.calc_decode_strides(dicom::pixel::DecodeOptions{});
		std::vector<std::uint8_t> dst(strides.frame, std::uint8_t{0});
		try {
			dicom::pixel::decode_frame_into(df, 0,
			    std::span<std::uint8_t>(dst.data(), dst.size()), strides,
			    dicom::pixel::DecodeOptions{});
			fail("decode_frame_into should throw when jpegxl frame source is missing");
		} catch (const std::exception& e) {
			const std::string what = e.what();
			const auto expected_ts =
			    std::string("ts=") + std::string("JPEGXLLossless"_uid.value());
			expect_contains(
			    what, "pixel::decode_frame_into", "jpegxl load_frame_source throw message");
			expect_contains(what, expected_ts, "jpegxl load_frame_source throw message");
			expect_decode_plugin_or_runtime(
			    what, "jpegxl", "jpegxl load_frame_source throw message");
			expect_decode_load_frame_source_or_runtime_lookup_error(
			    what, "jpegxl load_frame_source throw message");
		}
	}

	{
		dicom::DicomFile df{};
		df.set_transfer_syntax("RLELossless"_uid);
		configure_minimal_integral_pixel_metadata(df);

		const auto strides = df.calc_decode_strides(dicom::pixel::DecodeOptions{});
		std::vector<std::uint8_t> dst(strides.frame, std::uint8_t{0});
		try {
			dicom::pixel::decode_frame_into(df, 0,
			    std::span<std::uint8_t>(dst.data(), dst.size()), strides,
			    dicom::pixel::DecodeOptions{});
			fail("decode_frame_into should throw when rle frame source is missing");
		} catch (const std::exception& e) {
			const std::string what = e.what();
			const auto expected_ts =
			    std::string("ts=") + std::string("RLELossless"_uid.value());
			expect_contains(
			    what, "pixel::decode_frame_into", "rle load_frame_source throw message");
			expect_contains(what, expected_ts, "rle load_frame_source throw message");
			expect_decode_plugin_or_runtime(
			    what, "rle", "rle load_frame_source throw message");
			expect_decode_load_frame_source_or_runtime_lookup_error(
			    what, "rle load_frame_source throw message");
		}
	}

	{
		dicom::DicomFile df{};
		df.set_transfer_syntax("JPEG2000Lossless"_uid);
		configure_minimal_integral_pixel_metadata(df);
		df.reset_encapsulated_pixel_data(1);
		df.set_encoded_pixel_frame(0,
		    std::vector<std::uint8_t>{0x00, 0x01, 0x02, 0x03});

		const auto strides = df.calc_decode_strides(dicom::pixel::DecodeOptions{});
		std::vector<std::uint8_t> dst(strides.frame, std::uint8_t{0});
		try {
			dicom::pixel::decode_frame_into(df, 0,
			    std::span<std::uint8_t>(dst.data(), dst.size()), strides,
			    dicom::pixel::DecodeOptions{});
			fail("decode_frame_into should throw for invalid jpeg2k codestream");
		} catch (const std::exception& e) {
			const std::string what = e.what();
			const auto expected_ts =
			    std::string("ts=") + std::string("JPEG2000Lossless"_uid.value());
			expect_contains(
			    what, "pixel::decode_frame_into", "jpeg2k backend decode throw message");
			expect_contains(what, expected_ts, "jpeg2k backend decode throw message");
				expect_decode_plugin_or_runtime(
				    what, "jpeg2k", "jpeg2k backend decode throw message");
				expect_contains(
				    what, "status=backend_error", "jpeg2k backend decode throw message");
				expect_contains(
				    what, "stage=decode_frame", "jpeg2k backend decode throw message");
				expect_contains(what, "reason=file=", "jpeg2k backend decode throw message");
				expect_not_contains(what, "reason=pixel::decode_frame_into ",
				    "jpeg2k backend decode throw message");
			}
		}

		{
		dicom::DicomFile df{};
		df.set_transfer_syntax("HTJ2KLossless"_uid);
		configure_minimal_integral_pixel_metadata(df);
		df.reset_encapsulated_pixel_data(1);
		df.set_encoded_pixel_frame(0,
		    std::vector<std::uint8_t>{0x00, 0x01, 0x02, 0x03});

		const auto strides = df.calc_decode_strides(dicom::pixel::DecodeOptions{});
		std::vector<std::uint8_t> dst(strides.frame, std::uint8_t{0});
		try {
			dicom::pixel::decode_frame_into(df, 0,
			    std::span<std::uint8_t>(dst.data(), dst.size()), strides,
			    dicom::pixel::DecodeOptions{});
			fail("decode_frame_into should throw for invalid htj2k codestream");
		} catch (const std::exception& e) {
			const std::string what = e.what();
			const auto expected_ts =
			    std::string("ts=") + std::string("HTJ2KLossless"_uid.value());
			expect_contains(
			    what, "pixel::decode_frame_into", "htj2k backend decode throw message");
			expect_contains(what, expected_ts, "htj2k backend decode throw message");
				expect_decode_plugin_or_runtime(
				    what, "htj2k", "htj2k backend decode throw message");
				expect_contains(
				    what, "status=backend_error", "htj2k backend decode throw message");
				expect_contains(
				    what, "stage=decode_frame", "htj2k backend decode throw message");
				expect_contains(what, "reason=file=", "htj2k backend decode throw message");
				expect_not_contains(what, "reason=pixel::decode_frame_into ",
				    "htj2k backend decode throw message");
				const bool has_openjph_token =
				    what.find("HTJ2K decode failed") != std::string::npos;
				const bool has_openjpeg_token =
				    what.find("OpenJPEG decode failed") != std::string::npos;
				if (!has_openjph_token && !has_openjpeg_token) {
					fail("htj2k backend decode throw message missing backend token");
				}
			}
		}

		{
			dicom::DicomFile df{};
			df.set_transfer_syntax("JPEG2000Lossless"_uid);
			configure_minimal_integral_pixel_metadata(df);
			df.reset_encapsulated_pixel_data(1);
			df.set_encoded_pixel_frame(0,
			    std::vector<std::uint8_t>{0x00, 0x01, 0x02, 0x03});

			const auto strides = df.calc_decode_strides(dicom::pixel::DecodeOptions{});
			std::vector<std::uint8_t> dst(strides.frame, std::uint8_t{0});
			auto decode_opt = dicom::pixel::DecodeOptions{};
			decode_opt.decoder_threads = -2;
			try {
				dicom::pixel::decode_frame_into(df, 0,
				    std::span<std::uint8_t>(dst.data(), dst.size()), strides, decode_opt);
				fail("decode_frame_into should throw for invalid decoder_threads option");
			} catch (const std::exception& e) {
				const std::string what = e.what();
				const auto expected_ts =
				    std::string("ts=") + std::string("JPEG2000Lossless"_uid.value());
				expect_contains(what, "pixel::decode_frame_into",
				    "jpeg2k option throw message");
				expect_contains(what, expected_ts, "jpeg2k option throw message");
				expect_decode_plugin_or_runtime(
				    what, "jpeg2k", "jpeg2k option throw message");
				expect_contains(
				    what, "status=invalid_argument", "jpeg2k option throw message");
				expect_contains(what, "stage=parse_options", "jpeg2k option throw message");
				const bool has_decodeopt_token =
				    what.find("DecodeOptions.decoder_threads must be -1, 0, or positive") !=
				    std::string::npos;
				const bool has_threads_token =
				    what.find("threads must be -1, 0, or positive") != std::string::npos;
				if (!has_decodeopt_token && !has_threads_token) {
					fail("jpeg2k option throw message missing threads validation token");
				}
				expect_contains(what, "reason=file=", "jpeg2k option throw message");
				expect_not_contains(what, "reason=pixel::decode_frame_into ",
				    "jpeg2k option throw message");
			}
		}

	{
		dicom::DicomFile df{};
		df.set_transfer_syntax("MPEG2MPML"_uid);
		try {
			dicom::pixel::decode_frame_into(df, 0, std::span<std::uint8_t>{},
			    dicom::pixel::DecodeStrides{}, dicom::pixel::DecodeOptions{});
			fail("decode_frame_into should throw for unsupported transfer syntax");
		} catch (const std::exception& e) {
			const std::string what = e.what();
			const auto expected_ts =
			    std::string("ts=") + std::string("MPEG2MPML"_uid.value());
			expect_contains(what, "pixel::decode_frame_into",
			    "unsupported ts decode throw message");
			expect_contains(what, expected_ts, "unsupported ts decode throw message");
			expect_none_or_runtime_plugin(what, "unsupported ts decode throw message");
			expect_contains(
			    what, "status=unsupported", "unsupported ts decode throw message");
			expect_contains(
			    what, "stage=plugin_lookup", "unsupported ts decode throw message");
		}
	}

		{
			auto& registry = global_codec_registry();
			const auto snapshot = snapshot_registry(registry);
			registry.clear();
		const bool binding_registered =
		    registry.register_binding(dicom::pixel::detail::TransferSyntaxPluginBinding{
		        .transfer_syntax = "ExplicitVRLittleEndian"_uid,
		        .plugin_key = "missing-decode-plugin",
		        .profile = dicom::pixel::detail::CodecProfile::native_uncompressed,
		        .encode_supported = false,
		        .decode_supported = true,
		    });
		if (!binding_registered) {
			fail("failed to register missing plugin binding test fixture");
		}
		try {
			dicom::DicomFile df{};
			df.set_transfer_syntax("ExplicitVRLittleEndian"_uid);
			dicom::pixel::decode_frame_into(df, 0, std::span<std::uint8_t>{},
			    dicom::pixel::DecodeStrides{}, dicom::pixel::DecodeOptions{});
			fail("decode_frame_into should throw when binding plugin is missing");
		} catch (const std::exception& e) {
			const std::string what = e.what();
				const auto expected_ts =
				    std::string("ts=") + std::string("ExplicitVRLittleEndian"_uid.value());
				expect_contains(what, "pixel::decode_frame_into",
				    "missing plugin decode throw message");
				expect_contains(what, expected_ts, "missing plugin decode throw message");
				expect_missing_decode_plugin_or_runtime_native_error(
				    what, "missing plugin decode throw message");
				}
				restore_registry(registry, snapshot);
			}

		{
		auto& registry = global_codec_registry();
		const auto snapshot = snapshot_registry(registry);
		registry.clear();
		const bool plugin_registered =
		    registry.register_plugin(dicom::pixel::detail::CodecPlugin{
		        .key = "dummy-no-decode",
		        .display_name = "Dummy no decode",
		    });
		if (!plugin_registered) {
			fail("failed to register no-dispatch decode plugin fixture");
		}
		const bool binding_registered =
		    registry.register_binding(dicom::pixel::detail::TransferSyntaxPluginBinding{
		        .transfer_syntax = "ExplicitVRLittleEndian"_uid,
		        .plugin_key = "dummy-no-decode",
		        .profile = dicom::pixel::detail::CodecProfile::native_uncompressed,
		        .encode_supported = false,
		        .decode_supported = true,
		    });
		if (!binding_registered) {
			fail("failed to register no-dispatch decode binding fixture");
		}
		try {
			dicom::DicomFile df{};
			df.set_transfer_syntax("ExplicitVRLittleEndian"_uid);
			dicom::pixel::decode_frame_into(df, 0, std::span<std::uint8_t>{},
			    dicom::pixel::DecodeStrides{}, dicom::pixel::DecodeOptions{});
			fail("decode_frame_into should throw when decode dispatcher is missing");
		} catch (const std::exception& e) {
			const std::string what = e.what();
				const auto expected_ts =
				    std::string("ts=") + std::string("ExplicitVRLittleEndian"_uid.value());
				expect_contains(what, "pixel::decode_frame_into",
				    "no-dispatch decode throw message");
				expect_contains(what, expected_ts, "no-dispatch decode throw message");
				expect_missing_decode_plugin_or_runtime_native_error(
				    what, "no-dispatch decode throw message");
			}
			restore_registry(registry, snapshot);
		}

		{
			try {
			encode_with_plugin_or_throw("missing-encode-plugin");
			fail("encode_encapsulated_pixel_data should throw when plugin is missing");
		} catch (const std::exception& e) {
			const std::string what = e.what();
			const auto expected_ts =
			    std::string("ts=") + std::string("JPEG2000Lossless"_uid.value());
			expect_contains(what, "DicomFile::set_pixel_data",
			    "missing plugin encode throw message");
			expect_contains(what, expected_ts, "missing plugin encode throw message");
			expect_contains(what, "plugin=missing-encode-plugin",
			    "missing plugin encode throw message");
			expect_contains(
			    what, "status=unsupported", "missing plugin encode throw message");
			expect_contains(
			    what, "stage=plugin_lookup", "missing plugin encode throw message");
			expect_contains(what, "reason=plugin is not registered in codec registry",
			    "missing plugin encode throw message");
		}
	}

	{
		auto& registry = global_codec_registry();
		const auto snapshot = snapshot_registry(registry);
		registry.clear();
		const bool plugin_registered =
		    registry.register_plugin(dicom::pixel::detail::CodecPlugin{
		        .key = "dummy-no-encode",
		        .display_name = "Dummy no encode",
		    });
		if (!plugin_registered) {
			fail("failed to register no-dispatch encode plugin fixture");
		}
		try {
			encode_with_plugin_or_throw("dummy-no-encode");
			fail("encode_encapsulated_pixel_data should throw when encode dispatch is missing");
		} catch (const std::exception& e) {
			const std::string what = e.what();
			const auto expected_ts =
			    std::string("ts=") + std::string("JPEG2000Lossless"_uid.value());
			expect_contains(what, "DicomFile::set_pixel_data",
			    "no-dispatch encode throw message");
			expect_contains(what, expected_ts, "no-dispatch encode throw message");
			expect_contains(what, "plugin=dummy-no-encode",
			    "no-dispatch encode throw message");
			expect_contains(
			    what, "status=unsupported", "no-dispatch encode throw message");
			expect_contains(
			    what, "stage=plugin_lookup", "no-dispatch encode throw message");
			expect_contains(what, "reason=plugin does not provide encode_frame dispatch",
			    "no-dispatch encode throw message");
		}
		restore_registry(registry, snapshot);
	}

	{
		auto& registry = global_codec_registry();
		const auto snapshot = snapshot_registry(registry);
		registry.clear();
		const bool plugin_registered =
		    registry.register_plugin(dicom::pixel::detail::CodecPlugin{
		        .key = "dummy-fail-encode",
		        .display_name = "Dummy fail encode",
		        .encode_frame = &always_fail_encode_dispatch,
		    });
		if (!plugin_registered) {
			fail("failed to register failing encode plugin fixture");
		}
		try {
			encode_with_plugin_or_throw("dummy-fail-encode");
			fail("encode_encapsulated_pixel_data should throw for failed encode dispatch");
		} catch (const std::exception& e) {
			const std::string what = e.what();
			const auto expected_ts =
			    std::string("ts=") + std::string("JPEG2000Lossless"_uid.value());
			expect_contains(
			    what, "DicomFile::set_pixel_data", "backend encode throw message");
			expect_contains(what, expected_ts, "backend encode throw message");
			expect_contains(what, "plugin=dummy-fail-encode",
			    "backend encode throw message");
			expect_contains(what, "frame=0", "backend encode throw message");
			expect_contains(what, "status=backend_error",
			    "backend encode throw message");
			expect_contains(
			    what, "stage=encode_frame", "backend encode throw message");
			expect_contains(
			    what, "reason=encoder plugin failed", "backend encode throw message");
		}
		restore_registry(registry, snapshot);
	}

	{
		const auto& registry = global_codec_registry();
		const auto* rle_plugin = registry.find_plugin("rle");
		if (rle_plugin && rle_plugin->encode_frame) {
			dicom::pixel::detail::CodecEncodeFrameInput encode_input{};
			encode_input.transfer_syntax = "RLELossless"_uid;
			encode_input.source_planar = dicom::pixel::Planar::interleaved;
			encode_input.bytes_per_sample = 1;
			std::vector<std::uint8_t> encoded{};
			CodecError encode_error{};
			const std::span<const dicom::pixel::detail::CodecOptionKv> encode_options{};
			const bool ok = rle_plugin->encode_frame(
			    encode_input, encode_options, encoded, encode_error);
			if (ok) {
				fail("rle encode_frame plugin should fail for invalid arguments");
			}
			if (encode_error.code != CodecStatusCode::invalid_argument) {
				fail("rle encode_frame plugin should report invalid_argument");
			}
			if (encode_error.stage != "validate") {
				fail("rle encode_frame plugin should report validate stage");
			}
			expect_contains(encode_error.detail, "request numeric field is invalid",
			    "rle encode_frame plugin error");
		}
	}

	{
		std::vector<std::uint8_t> frame(2, std::uint8_t{0});
		std::vector<std::uint8_t> encoded{};
		CodecError encode_error{};
		dicom::pixel::J2kOptions options{};
		const bool ok = dicom::pixel::detail::try_encode_jpeg2k_frame(
		    std::span<const std::uint8_t>(frame.data(), frame.size()),
		    1, 1, 1, 2, 16, 12, 0, dicom::pixel::Planar::interleaved, 2, false,
		    false, options, encoded, encode_error);
		if (ok) {
			fail("try_encode_jpeg2k_frame should fail without lossy target");
		}
		if (encode_error.code != CodecStatusCode::invalid_argument) {
			fail("try_encode_jpeg2k_frame should report invalid_argument");
		}
		if (encode_error.stage != "validate") {
			fail("try_encode_jpeg2k_frame should report validate stage");
		}
		expect_contains(encode_error.detail, "lossy JPEG2000 requires target_psnr or target_bpp",
		    "try_encode_jpeg2k_frame error");
	}

	{
		std::vector<std::uint8_t> frame(2, std::uint8_t{0});
		std::vector<std::uint8_t> encoded{};
		CodecError encode_error{};
		dicom::pixel::Htj2kOptions options{};
		const bool ok = dicom::pixel::detail::try_encode_htj2k_frame(
		    std::span<const std::uint8_t>(frame.data(), frame.size()),
		    1, 1, 1, 2, 12, 12, 0, dicom::pixel::Planar::interleaved, 2, false,
		    true, false, options, encoded, encode_error);
		if (ok) {
			fail("try_encode_htj2k_frame should fail for invalid bits_allocated");
		}
		if (encode_error.code != CodecStatusCode::invalid_argument) {
			fail("try_encode_htj2k_frame should report invalid_argument");
		}
		if (encode_error.stage != "validate") {
			fail("try_encode_htj2k_frame should report validate stage");
		}
		expect_contains(encode_error.detail, "bits_allocated must be 8 or 16",
		    "try_encode_htj2k_frame error");
	}

	{
		std::vector<std::uint8_t> frame(2, std::uint8_t{0});
		std::vector<std::uint8_t> encoded{};
		CodecError encode_error{};
		dicom::pixel::JpegOptions options{};
		const bool ok = dicom::pixel::detail::try_encode_jpeg_frame(
		    std::span<const std::uint8_t>(frame.data(), frame.size()),
		    1, 1, 1, 2, 12, 12, dicom::pixel::Planar::interleaved, 2, true, options,
		    encoded, encode_error);
		if (ok) {
			fail("try_encode_jpeg_frame should fail for invalid bits_allocated");
		}
		if (encode_error.code != CodecStatusCode::invalid_argument) {
			fail("try_encode_jpeg_frame should report invalid_argument");
		}
		if (encode_error.stage != "validate") {
			fail("try_encode_jpeg_frame should report validate stage");
		}
		expect_contains(encode_error.detail, "bits_allocated must be 8 or 16",
		    "try_encode_jpeg_frame error");
	}

	{
		std::vector<std::uint8_t> frame(2, std::uint8_t{0});
		std::vector<std::uint8_t> encoded{};
		CodecError encode_error{};
		dicom::pixel::JpegXlOptions options{};
		const bool ok = dicom::pixel::detail::try_encode_jpegxl_frame(
		    std::span<const std::uint8_t>(frame.data(), frame.size()),
		    1, 1, 1, 2, 16, 12, 0, dicom::pixel::Planar::interleaved, 2, true,
		    options, encoded, encode_error);
		if (ok) {
			fail("try_encode_jpegxl_frame should fail for invalid lossless distance");
		}
		if (encode_error.code != CodecStatusCode::invalid_argument) {
			fail("try_encode_jpegxl_frame should report invalid_argument");
		}
		if (encode_error.stage != "validate") {
			fail("try_encode_jpegxl_frame should report validate stage");
		}
		expect_contains(encode_error.detail, "lossless JPEG-XL requires distance=0",
		    "try_encode_jpegxl_frame error");
	}

	return 0;
}
