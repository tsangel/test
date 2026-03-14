#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <mutex>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <dicom.h>
#include <instream.h>
#include "codec_builtin_flags.hpp"

int main() {
	using dicom::DataSet;
	using dicom::read_bytes;
	using dicom::read_file;
	using namespace dicom::literals;

	auto fail = [](const std::string& msg) {
		std::cerr << msg << std::endl;
		std::exit(1);
	};

	std::string tmp_dir = ".";
	if (const char* env = std::getenv("TMPDIR"); env && *env) {
		tmp_dir = env;
	} else if (const char* env = std::getenv("TEMP"); env && *env) {
		tmp_dir = env;
	} else if (const char* env = std::getenv("TMP"); env && *env) {
		tmp_dir = env;
	}
	if (!tmp_dir.empty() && tmp_dir.back() != '/' && tmp_dir.back() != '\\') {
		tmp_dir.push_back('/');
	}

	const std::string file_path = tmp_dir + "dicomsdl_pixel_io_smoke.dcm";
	{
		std::ofstream os(file_path, std::ios::binary);
		os << "DICM";
	}
	{
		const auto file = read_file(file_path);
		if (!file) fail("read_file returned null");
		if (file->has_error()) fail("read_file should not record errors for valid input");
		if (!file->error_message().empty()) fail("error_message should be empty when no read error exists");
		if (file->path() != file_path) fail("file path mismatch");
		if (file->stream().attached_size() != 4) fail("file data_size mismatch");
		if (file->size() != file->dataset().size()) fail("DicomFile size forwarding mismatch");

		const auto file_dump = file->dump();
		if (file_dump.find("TAG\tVR\tLEN\tVM\tOFFSET\tVALUE\tKEYWORD") == std::string::npos) {
			fail("DicomFile dump header missing");
		}
		const auto file_dump_no_offset = file->dump(80, false);
		if (file_dump_no_offset.find("TAG\tVR\tLEN\tVM\tVALUE\tKEYWORD") == std::string::npos) {
			fail("DicomFile dump(include_offset=false) header missing");
		}
		if (file_dump_no_offset.find("OFFSET\tVALUE") != std::string::npos) {
			fail("DicomFile dump(include_offset=false) should hide OFFSET column");
		}

		auto& rows = file->add_dataelement("Rows"_tag, dicom::VR::US, 0, 2);
		if (!rows) fail("DicomFile add_dataelement failed");
		if (file->dump().find("'00280010'") == std::string::npos) {
			fail("DicomFile dump should include Rows");
		}
		file->remove_dataelement("Rows"_tag);
		if (file->get_dataelement("Rows"_tag).is_present()) {
			fail("DicomFile remove_dataelement failed");
		}

		DataSet manual;
		manual.attach_to_file(file_path);
		if (manual.path() != file_path) fail("manual path mismatch");
		if (manual.stream().attached_size() != 4) fail("manual data_size mismatch");
	}

	const std::vector<std::uint8_t> buffer{0x01, 0x02, 0x03, 0x04};
	const auto mem = read_bytes("buffer", buffer.data(), buffer.size());
	if (mem->stream().attached_size() != buffer.size()) fail("mem data_size mismatch");

	auto owned_buffer = std::vector<std::uint8_t>{0x0A, 0x0B};
	const auto mem_owned = read_bytes("owned-buffer", std::move(owned_buffer));
	if (mem_owned->stream().attached_size() != 2) fail("mem_owned data_size mismatch");

	const auto malformed = [] {
		std::vector<std::uint8_t> bytes(128, 0);
		bytes.insert(bytes.end(), {'D', 'I', 'C', 'M'});
		bytes.insert(bytes.end(), {0x10, 0x00, 0x10, 0x00, 'P', 'N', 0x08, 0x00});
		bytes.insert(bytes.end(), {'A', 'B'});
		return bytes;
	}();
	bool malformed_threw = false;
	try {
		[[maybe_unused]] auto should_throw =
		    read_bytes("malformed-default", malformed.data(), malformed.size());
	} catch (const std::exception&) {
		malformed_threw = true;
	}
	if (!malformed_threw) fail("malformed input should throw when keep_on_error is false");

	dicom::ReadOptions keep_opts;
	keep_opts.keep_on_error = true;
	const auto malformed_keep =
	    read_bytes("malformed-keep", malformed.data(), malformed.size(), keep_opts);
	if (!malformed_keep) fail("malformed keep_on_error read returned null");
	if (!malformed_keep->has_error()) fail("keep_on_error read should record has_error=true");
	if (malformed_keep->error_message().empty()) fail("keep_on_error read should keep error_message");
	if (malformed_keep->size() == 0) fail("keep_on_error read should preserve partially parsed elements");

	DataSet manual_mem;
	manual_mem.attach_to_memory("manual-buffer", buffer.data(), buffer.size());
	if (manual_mem.stream().attached_size() != buffer.size()) fail("manual_mem data_size mismatch");

	{
		dicom::DicomFile native_pixels;
		auto& bits_allocated = native_pixels.add_dataelement("BitsAllocated"_tag, dicom::VR::US);
		if (!bits_allocated.from_long(16)) {
			fail("failed to set BitsAllocated for native pixel API test");
		}
		native_pixels.set_native_pixel_data(std::vector<std::uint8_t>{0x34, 0x12, 0x78, 0x56});
		const auto& native_pixel_elem = native_pixels.get_dataelement("PixelData"_tag);
		if (native_pixel_elem.is_missing()) fail("set_native_pixel_data should create PixelData");
		if (native_pixel_elem.vr() != dicom::VR::OW) fail("set_native_pixel_data should infer OW");
		const auto span = native_pixel_elem.value_span();
		if (span.size() != 4 || span[0] != 0x34 || span[1] != 0x12 || span[2] != 0x78 || span[3] != 0x56) {
			fail("set_native_pixel_data payload mismatch");
		}
	}

	auto make_u16_source = []() {
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
		return std::pair{std::move(source_bytes), source};
	};

	{
		auto [source_bytes, source] = make_u16_source();
		dicom::DicomFile native_file;
		native_file.set_pixel_data("ExplicitVRLittleEndian"_uid, source);
		const auto& pixel_data = native_file.get_dataelement("PixelData"_tag);
		if (pixel_data.is_missing()) fail("set_pixel_data should create PixelData");
		if (pixel_data.vr() != dicom::VR::OW) fail("set_pixel_data should create OW for 16-bit input");
		const auto pixel_bytes = pixel_data.value_span();
		const std::vector<std::uint8_t> expected{
		    0x01u, 0x00u, 0x02u, 0x00u, 0x03u, 0x00u,
		    0x04u, 0x00u, 0x05u, 0x00u, 0x06u, 0x00u,
		    0x11u, 0x00u, 0x12u, 0x00u, 0x13u, 0x00u,
		    0x14u, 0x00u, 0x15u, 0x00u, 0x16u, 0x00u};
		if (pixel_bytes.size() != expected.size() ||
		    !std::equal(pixel_bytes.begin(), pixel_bytes.end(), expected.begin(), expected.end())) {
			fail("set_pixel_data should strip row/frame padding and keep payload bytes");
		}
		if (native_file["Rows"_tag].to_long().value_or(0) != 2 ||
		    native_file["Columns"_tag].to_long().value_or(0) != 3 ||
		    native_file["NumberOfFrames"_tag].to_long().value_or(0) != 2) {
			fail("set_pixel_data should update core image geometry");
		}
		if (native_file["PhotometricInterpretation"_tag].to_string_view().value_or("") != "MONOCHROME2") {
			fail("set_pixel_data should update PhotometricInterpretation");
		}
		if (native_file["LossyImageCompression"_tag].to_string_view().value_or("") != std::string_view("00")) {
			fail("native uncompressed set_pixel_data should set LossyImageCompression to 00");
		}
	}

	{
		auto [source_bytes, source] = make_u16_source();
		const std::vector<std::uint8_t> expected_frame0{
		    0x01u, 0x00u, 0x02u, 0x00u, 0x03u, 0x00u,
		    0x04u, 0x00u, 0x05u, 0x00u, 0x06u, 0x00u};
		const std::vector<std::uint8_t> expected_frame1{
		    0x11u, 0x00u, 0x12u, 0x00u, 0x13u, 0x00u,
		    0x14u, 0x00u, 0x15u, 0x00u, 0x16u, 0x00u};

		dicom::DicomFile rle_file;
		rle_file.set_pixel_data("RLELossless"_uid, source);
		const auto& pixel_data = rle_file.get_dataelement("PixelData"_tag);
		if (pixel_data.is_missing() || !pixel_data.vr().is_pixel_sequence()) {
			fail("RLE set_pixel_data should create encapsulated PixelData");
		}
		const auto* pixel_sequence = pixel_data.as_pixel_sequence();
		if (!pixel_sequence || pixel_sequence->number_of_frames() != 2) {
			fail("RLE set_pixel_data should create expected frame count");
		}
		if (!rle_file.transfer_syntax_uid().valid() ||
		    rle_file.transfer_syntax_uid().value() != "RLELossless"_uid.value()) {
			fail("RLE set_pixel_data should update runtime transfer syntax");
		}
		if (rle_file.pixel_data(0) != expected_frame0 || rle_file.pixel_data(1) != expected_frame1) {
			fail("RLE set_pixel_data frame roundtrip mismatch");
		}

		const auto rle_encoder_context = dicom::pixel::create_encoder_context("RLELossless"_uid);
		dicom::DicomFile with_context_file;
		with_context_file.set_pixel_data("RLELossless"_uid, source, rle_encoder_context);
		if (with_context_file.pixel_data(0) != expected_frame0 ||
		    with_context_file.pixel_data(1) != expected_frame1) {
			fail("set_pixel_data with reusable encoder context mismatch");
		}

		if (dicom::test::kJpeg2kBuiltin) {
			dicom::DicomFile transcode_chain_file;
			transcode_chain_file.set_pixel_data("RLELossless"_uid, source);
			transcode_chain_file.set_transfer_syntax("JPEG2000Lossless"_uid);
			const auto& transcoded_pixel_data =
			    transcode_chain_file.get_dataelement("PixelData"_tag);
			if (transcoded_pixel_data.is_missing() ||
			    !transcoded_pixel_data.vr().is_pixel_sequence()) {
				fail("multi-frame RLE->JPEG2000 transcode should keep encapsulated PixelData");
			}
			const auto* transcoded_sequence = transcoded_pixel_data.as_pixel_sequence();
			if (!transcoded_sequence || transcoded_sequence->number_of_frames() != 2) {
				fail("multi-frame RLE->JPEG2000 transcode should preserve frame count");
			}
			if (transcode_chain_file.pixel_data(0) != expected_frame0 ||
			    transcode_chain_file.pixel_data(1) != expected_frame1) {
				fail("multi-frame RLE->JPEG2000 transcode frame roundtrip mismatch");
			}
		}
	}

	{
		auto [source_bytes, source] = make_u16_source();
		dicom::DicomFile encap_file;
		encap_file.set_pixel_data("EncapsulatedUncompressedExplicitVRLittleEndian"_uid, source);
		auto& pixel_data = encap_file.get_dataelement("PixelData"_tag);
		if (pixel_data.is_missing() || !pixel_data.vr().is_pixel_sequence()) {
			fail("EncapsulatedUncompressed set_pixel_data should create encapsulated PixelData");
		}
		auto* pixel_sequence = pixel_data.as_pixel_sequence();
		if (!pixel_sequence || pixel_sequence->number_of_frames() != 2) {
			fail("EncapsulatedUncompressed should create expected frame count");
		}
		const auto frame0_encoded = pixel_sequence->frame_encoded_span(0);
		const auto frame1_encoded = pixel_sequence->frame_encoded_span(1);
		if (frame0_encoded.size() != 12 || frame1_encoded.size() != 12) {
			fail("EncapsulatedUncompressed encoded frame size mismatch");
		}
		const std::vector<std::uint8_t> expected_frame0{
		    0x01u, 0x00u, 0x02u, 0x00u, 0x03u, 0x00u,
		    0x04u, 0x00u, 0x05u, 0x00u, 0x06u, 0x00u};
		const std::vector<std::uint8_t> expected_frame1{
		    0x11u, 0x00u, 0x12u, 0x00u, 0x13u, 0x00u,
		    0x14u, 0x00u, 0x15u, 0x00u, 0x16u, 0x00u};
		if (encap_file.pixel_data(0) != expected_frame0 || encap_file.pixel_data(1) != expected_frame1) {
			fail("EncapsulatedUncompressed roundtrip mismatch");
		}
	}

	{
		const std::vector<std::uint8_t> rgb_source{
		    0x10u, 0x20u, 0x30u, 0x40u, 0x50u, 0x60u,
		    0x70u, 0x80u, 0x90u, 0xA0u, 0xB0u, 0xC0u};
		dicom::pixel::PixelSource color_source{};
		color_source.bytes = std::span<const std::uint8_t>(rgb_source.data(), rgb_source.size());
		color_source.data_type = dicom::pixel::DataType::u8;
		color_source.rows = 2;
		color_source.cols = 2;
		color_source.frames = 1;
		color_source.samples_per_pixel = 3;
		color_source.planar = dicom::pixel::Planar::interleaved;
		color_source.photometric = dicom::pixel::Photometric::rgb;

		if (dicom::test::kJpeg2kBuiltin) {
			dicom::DicomFile j2k_file;
			j2k_file.set_pixel_data("JPEG2000Lossless"_uid, color_source);
			if (j2k_file["PhotometricInterpretation"_tag].to_string_view().value_or("") != std::string_view("YBR_RCT")) {
				fail("J2K default color transform should update PhotometricInterpretation to YBR_RCT");
			}

			const std::array<dicom::pixel::CodecOptionTextKv, 1> lossy_options{{{"target_psnr", "45"}}};
			dicom::DicomFile j2k_lossy_file;
			j2k_lossy_file.set_pixel_data(
			    "JPEG2000"_uid, color_source,
			    std::span<const dicom::pixel::CodecOptionTextKv>(lossy_options));
			if (j2k_lossy_file["LossyImageCompression"_tag].to_string_view().value_or("") != std::string_view("01")) {
				fail("J2K lossy set_pixel_data should set LossyImageCompression to 01");
			}
		}

		if (dicom::test::kHtj2kBuiltin) {
			dicom::DicomFile htj2k_file;
			htj2k_file.set_pixel_data("HTJ2KLossless"_uid, color_source);
			if (htj2k_file["PhotometricInterpretation"_tag].to_string_view().value_or("") != std::string_view("YBR_RCT")) {
				fail("HTJ2K default color transform should update PhotometricInterpretation to YBR_RCT");
			}

			const std::array<dicom::pixel::CodecOptionTextKv, 1> lossy_options{{{"target_psnr", "45"}}};
			dicom::DicomFile htj2k_lossy_file;
			htj2k_lossy_file.set_pixel_data(
			    "HTJ2K"_uid, color_source,
			    std::span<const dicom::pixel::CodecOptionTextKv>(lossy_options));
			if (htj2k_lossy_file["LossyImageCompression"_tag].to_string_view().value_or("") != std::string_view("01")) {
				fail("HTJ2K lossy set_pixel_data should set LossyImageCompression to 01");
			}
		}
	}

	if (dicom::test::kJpegLsBuiltin) {
		auto [source_bytes, source] = make_u16_source();
		dicom::DicomFile jpegls_file;
		jpegls_file.set_pixel_data("JPEGLSLossless"_uid, source);
		const auto& pixel_data = jpegls_file.get_dataelement("PixelData"_tag);
		if (pixel_data.is_missing() || !pixel_data.vr().is_pixel_sequence()) {
			fail("JPEG-LS set_pixel_data should create encapsulated PixelData");
		}
		const std::vector<std::uint8_t> expected_frame0{
		    0x01u, 0x00u, 0x02u, 0x00u, 0x03u, 0x00u,
		    0x04u, 0x00u, 0x05u, 0x00u, 0x06u, 0x00u};
		const std::vector<std::uint8_t> expected_frame1{
		    0x11u, 0x00u, 0x12u, 0x00u, 0x13u, 0x00u,
		    0x14u, 0x00u, 0x15u, 0x00u, 0x16u, 0x00u};
		if (jpegls_file.pixel_data(0) != expected_frame0 || jpegls_file.pixel_data(1) != expected_frame1) {
			fail("JPEG-LS set_pixel_data frame roundtrip mismatch");
		}

		const std::vector<std::uint8_t> lossy_source_bytes{0x10u, 0x20u, 0x30u, 0x40u, 0x50u, 0x60u};
		dicom::pixel::PixelSource lossy_source{};
		lossy_source.bytes = std::span<const std::uint8_t>(lossy_source_bytes.data(), lossy_source_bytes.size());
		lossy_source.data_type = dicom::pixel::DataType::u8;
		lossy_source.rows = 2;
		lossy_source.cols = 3;
		lossy_source.frames = 1;
		lossy_source.samples_per_pixel = 1;
		lossy_source.photometric = dicom::pixel::Photometric::monochrome2;
		const std::array<dicom::pixel::CodecOptionTextKv, 1> near_lossless_options{{{"near_lossless_error", "2"}}};
		dicom::DicomFile near_lossless_file;
		near_lossless_file.set_pixel_data(
		    "JPEGLSNearLossless"_uid, lossy_source,
		    std::span<const dicom::pixel::CodecOptionTextKv>(near_lossless_options));
		if (near_lossless_file["LossyImageCompression"_tag].to_string_view().value_or("") != std::string_view("01")) {
			fail("JPEG-LS near-lossless should set LossyImageCompression to 01");
		}
	}

	if (dicom::test::kJpegBuiltin) {
		auto [source_bytes, source] = make_u16_source();
		dicom::DicomFile jpeg_file;
		jpeg_file.set_pixel_data("JPEGLosslessSV1"_uid, source);
		const auto& pixel_data = jpeg_file.get_dataelement("PixelData"_tag);
		if (pixel_data.is_missing() || !pixel_data.vr().is_pixel_sequence()) {
			fail("JPEG set_pixel_data should create encapsulated PixelData");
		}
		const std::vector<std::uint8_t> expected_frame0{
		    0x01u, 0x00u, 0x02u, 0x00u, 0x03u, 0x00u,
		    0x04u, 0x00u, 0x05u, 0x00u, 0x06u, 0x00u};
		const std::vector<std::uint8_t> expected_frame1{
		    0x11u, 0x00u, 0x12u, 0x00u, 0x13u, 0x00u,
		    0x14u, 0x00u, 0x15u, 0x00u, 0x16u, 0x00u};
		if (jpeg_file.pixel_data(0) != expected_frame0 || jpeg_file.pixel_data(1) != expected_frame1) {
			fail("JPEG set_pixel_data frame roundtrip mismatch");
		}

		const std::vector<std::uint8_t> lossy_source_bytes{0x12u, 0x34u, 0x56u, 0x78u, 0x9Au, 0xBCu};
		dicom::pixel::PixelSource lossy_source{};
		lossy_source.bytes = std::span<const std::uint8_t>(lossy_source_bytes.data(), lossy_source_bytes.size());
		lossy_source.data_type = dicom::pixel::DataType::u8;
		lossy_source.rows = 2;
		lossy_source.cols = 3;
		lossy_source.frames = 1;
		lossy_source.samples_per_pixel = 1;
		lossy_source.photometric = dicom::pixel::Photometric::monochrome2;
		const std::array<dicom::pixel::CodecOptionTextKv, 1> jpeg_lossy_options{{{"quality", "90"}}};
		dicom::DicomFile jpeg_lossy_file;
		jpeg_lossy_file.set_pixel_data(
		    "JPEGBaseline8Bit"_uid, lossy_source,
		    std::span<const dicom::pixel::CodecOptionTextKv>(jpeg_lossy_options));
		if (jpeg_lossy_file["LossyImageCompression"_tag].to_string_view().value_or("") != std::string_view("01")) {
			fail("JPEG lossy should set LossyImageCompression to 01");
		}
	}

	{
		auto [source_bytes, source] = make_u16_source();
		const std::vector<std::uint8_t> expected_frame0{
		    0x01u, 0x00u, 0x02u, 0x00u, 0x03u, 0x00u,
		    0x04u, 0x00u, 0x05u, 0x00u, 0x06u, 0x00u};
		const std::vector<std::uint8_t> expected_frame1{
		    0x11u, 0x00u, 0x12u, 0x00u, 0x13u, 0x00u,
		    0x14u, 0x00u, 0x15u, 0x00u, 0x16u, 0x00u};

		dicom::DicomFile native_file;
		native_file.set_pixel_data("ExplicitVRLittleEndian"_uid, source);

		std::ostringstream os(std::ios::binary);
		native_file.write_with_transfer_syntax(os, "RLELossless"_uid);
		const auto encoded = os.str();
		auto roundtrip = read_bytes(
		    "write-with-ts-rle",
		    reinterpret_cast<const std::uint8_t*>(encoded.data()), encoded.size());
		if (!roundtrip) fail("write_with_transfer_syntax native->RLE returned null");
		const auto& pixel_data = roundtrip->get_dataelement("PixelData"_tag);
		if (pixel_data.is_missing() || !pixel_data.vr().is_pixel_sequence()) {
			fail("write_with_transfer_syntax native->RLE should write encapsulated PixelData");
		}
		const auto* pixel_sequence = pixel_data.as_pixel_sequence();
		if (!pixel_sequence || pixel_sequence->extended_offset_table_count() != 2) {
			fail("write_with_transfer_syntax native->RLE should backpatch ExtendedOffsetTable");
		}
		if (roundtrip->pixel_data(0) != expected_frame0 ||
		    roundtrip->pixel_data(1) != expected_frame1) {
			fail("write_with_transfer_syntax native->RLE frame roundtrip mismatch");
		}
		const auto& original_pixel_data = native_file.get_dataelement("PixelData"_tag);
		if (original_pixel_data.is_missing() || !original_pixel_data.vr().is_binary()) {
			fail("write_with_transfer_syntax native->RLE should not mutate source DicomFile");
		}
		if (native_file.transfer_syntax_uid().value() !=
		    "ExplicitVRLittleEndian"_uid.value()) {
			fail("write_with_transfer_syntax native->RLE should preserve source transfer syntax state");
		}
	}

	{
		auto [source_bytes, source] = make_u16_source();
		dicom::DicomFile native_file;
		native_file.set_pixel_data("ExplicitVRLittleEndian"_uid, source);
		static constexpr std::string_view kLowerPhotometric = "monochrome2";
		native_file.add_dataelement("PhotometricInterpretation"_tag, dicom::VR::CS)
		    .set_value_bytes(std::span<const std::uint8_t>(
		        reinterpret_cast<const std::uint8_t*>(kLowerPhotometric.data()),
		        kLowerPhotometric.size()));

		std::ostringstream os(std::ios::binary);
		native_file.write_with_transfer_syntax(os, "RLELossless"_uid);
		const auto encoded = os.str();
		auto roundtrip = read_bytes(
		    "write-with-ts-rle-lowercase-pi",
		    reinterpret_cast<const std::uint8_t*>(encoded.data()), encoded.size());
		if (!roundtrip) fail("write_with_transfer_syntax lowercase PI returned null");
		if (roundtrip->pixel_data(0).empty()) {
			fail("write_with_transfer_syntax should accept lowercase PhotometricInterpretation");
		}
	}

	{
		auto [source_bytes, source] = make_u16_source();
		const std::vector<std::uint8_t> expected_frame0{
		    0x01u, 0x00u, 0x02u, 0x00u, 0x03u, 0x00u,
		    0x04u, 0x00u, 0x05u, 0x00u, 0x06u, 0x00u};
		const std::vector<std::uint8_t> expected_frame1{
		    0x11u, 0x00u, 0x12u, 0x00u, 0x13u, 0x00u,
		    0x14u, 0x00u, 0x15u, 0x00u, 0x16u, 0x00u};

		dicom::DicomFile rle_file;
		rle_file.set_pixel_data("RLELossless"_uid, source);

		std::ostringstream os(std::ios::binary);
		rle_file.write_with_transfer_syntax(os, "ExplicitVRLittleEndian"_uid);
		const auto decoded = os.str();
		auto roundtrip = read_bytes(
		    "write-with-ts-native",
		    reinterpret_cast<const std::uint8_t*>(decoded.data()), decoded.size());
		if (!roundtrip) fail("write_with_transfer_syntax RLE->native returned null");
		const auto& pixel_data = roundtrip->get_dataelement("PixelData"_tag);
		if (pixel_data.is_missing() || pixel_data.vr().is_pixel_sequence()) {
			fail("write_with_transfer_syntax RLE->native should write native PixelData");
		}
		if (roundtrip->pixel_data(0) != expected_frame0 ||
		    roundtrip->pixel_data(1) != expected_frame1) {
			fail("write_with_transfer_syntax RLE->native frame roundtrip mismatch");
		}
		const auto& original_pixel_data = rle_file.get_dataelement("PixelData"_tag);
		if (original_pixel_data.is_missing() || !original_pixel_data.vr().is_pixel_sequence()) {
			fail("write_with_transfer_syntax RLE->native should not mutate source DicomFile");
		}
		if (rle_file.transfer_syntax_uid().value() != "RLELossless"_uid.value()) {
			fail("write_with_transfer_syntax RLE->native should preserve source transfer syntax state");
		}
	}

	if (dicom::test::kJpegLsBuiltin) {
		const std::vector<std::uint8_t> lossy_source_bytes{
		    0x10u, 0x20u, 0x30u, 0x40u, 0x50u, 0x60u};
		dicom::pixel::PixelSource lossy_source{};
		lossy_source.bytes = std::span<const std::uint8_t>(
		    lossy_source_bytes.data(), lossy_source_bytes.size());
		lossy_source.data_type = dicom::pixel::DataType::u8;
		lossy_source.rows = 2;
		lossy_source.cols = 3;
		lossy_source.frames = 1;
		lossy_source.samples_per_pixel = 1;
		lossy_source.photometric = dicom::pixel::Photometric::monochrome2;

		dicom::DicomFile lossy_writer_file;
		lossy_writer_file.set_pixel_data("ExplicitVRLittleEndian"_uid, lossy_source);
		const std::array<dicom::pixel::CodecOptionTextKv, 1> near_lossless_options{{
		    {"near_lossless_error", "2"},
		}};
		std::ostringstream os(std::ios::binary);
		lossy_writer_file.write_with_transfer_syntax(
		    os, "JPEGLSNearLossless"_uid,
		    std::span<const dicom::pixel::CodecOptionTextKv>(near_lossless_options));
		const auto encoded = os.str();
		auto roundtrip = read_bytes(
		    "write-with-ts-jpegls-lossy",
		    reinterpret_cast<const std::uint8_t*>(encoded.data()), encoded.size());
		if (!roundtrip) fail("write_with_transfer_syntax JPEG-LS lossy returned null");
		if (roundtrip->get_dataelement("LossyImageCompression"_tag)
		        .to_string_view().value_or("") != std::string_view("01")) {
			fail("write_with_transfer_syntax JPEG-LS lossy should set LossyImageCompression to 01");
		}
		if (roundtrip->pixel_data(0) != lossy_source_bytes) {
			fail("write_with_transfer_syntax JPEG-LS lossy roundtrip mismatch");
		}
	}

	{
		dicom::DicomFile float_file;
		if (!float_file.add_dataelement("Rows"_tag, dicom::VR::US).from_long(1) ||
		    !float_file.add_dataelement("Columns"_tag, dicom::VR::US).from_long(1) ||
		    !float_file.add_dataelement("SamplesPerPixel"_tag, dicom::VR::US).from_long(1) ||
		    !float_file.add_dataelement("PhotometricInterpretation"_tag, dicom::VR::CS)
		             .from_string_view("MONOCHROME2")) {
			fail("failed to build float pixel source dataset");
		}
		const std::array<float, 1> float_pixels{{1.0f}};
		float_file.add_dataelement("FloatPixelData"_tag, dicom::VR::OF)
		    .set_value_bytes(std::span<const std::uint8_t>(
		        reinterpret_cast<const std::uint8_t*>(float_pixels.data()),
		        sizeof(float_pixels)));

		bool write_threw = false;
		try {
			std::ostringstream os(std::ios::binary);
			float_file.write_with_transfer_syntax(os, "RLELossless"_uid);
		} catch (const std::exception&) {
			write_threw = true;
		}
		if (!write_threw) {
			fail("write_with_transfer_syntax should reject FloatPixelData -> encapsulated transfer syntax");
		}

		bool set_ts_threw = false;
		try {
			float_file.set_transfer_syntax("RLELossless"_uid);
		} catch (const std::exception&) {
			set_ts_threw = true;
		}
		if (!set_ts_threw) {
			fail("set_transfer_syntax should reject FloatPixelData -> encapsulated transfer syntax");
		}
	}

	{
		auto [source_bytes, source] = make_u16_source();
		const std::vector<std::uint8_t> expected_frame0{
		    0x01u, 0x00u, 0x02u, 0x00u, 0x03u, 0x00u,
		    0x04u, 0x00u, 0x05u, 0x00u, 0x06u, 0x00u};
		const std::vector<std::uint8_t> expected_frame1{
		    0x11u, 0x00u, 0x12u, 0x00u, 0x13u, 0x00u,
		    0x14u, 0x00u, 0x15u, 0x00u, 0x16u, 0x00u};

		dicom::DicomFile native_file;
		native_file.set_pixel_data("ExplicitVRLittleEndian"_uid, source);
		dicom::WriteOptions rebuilt_meta_opts;
		rebuilt_meta_opts.keep_existing_meta = false;

		std::ostringstream os(std::ios::binary);
		native_file.write_with_transfer_syntax(
		    os, "DeflatedExplicitVRLittleEndian"_uid, rebuilt_meta_opts);
		const auto deflated = os.str();
		auto roundtrip = read_bytes(
		    "write-with-ts-deflated",
		    reinterpret_cast<const std::uint8_t*>(deflated.data()), deflated.size());
		if (!roundtrip) fail("write_with_transfer_syntax deflated returned null");
		if (roundtrip->pixel_data(0) != expected_frame0 ||
		    roundtrip->pixel_data(1) != expected_frame1) {
			fail("write_with_transfer_syntax deflated frame roundtrip mismatch");
		}
		if (native_file.transfer_syntax_uid().value() !=
		    "ExplicitVRLittleEndian"_uid.value()) {
			fail("write_with_transfer_syntax deflated should preserve source transfer syntax state");
		}
	}

	{
		dicom::DicomFile generated;
		auto add_text_element = [&](dicom::Tag tag, dicom::VR vr, std::string_view value)
		    -> dicom::DataElement& {
			auto& element = generated.add_dataelement(tag, vr, 0, value.size());
			element.set_value_bytes(std::span<const std::uint8_t>(
			    reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
			return element;
		};

		add_text_element("SOPClassUID"_tag, dicom::VR::UI, "1.2.840.10008.5.1.4.1.1.7");
		add_text_element("SOPInstanceUID"_tag, dicom::VR::UI, dicom::uid::generate_sop_instance_uid().value());
		add_text_element("PatientName"_tag, dicom::VR::PN, "WRITE^ROUNDTRIP");

		auto& sequence_element =
		    generated.add_dataelement("ReferencedStudySequence"_tag, dicom::VR::SQ);
		if (!sequence_element) fail("failed to add sequence element");
		auto* sequence = sequence_element.as_sequence();
		if (!sequence) fail("sequence pointer is null");
		auto* sequence_item = sequence->add_dataset();
		if (!sequence_item) fail("failed to append sequence item");
		auto& referenced_uid =
		    sequence_item->add_dataelement("ReferencedSOPInstanceUID"_tag, dicom::VR::UI, 0, 12);
		const std::array<std::uint8_t, 12> uid_value{
		    '1', '.', '2', '.', '3', '.', '4', '.', '5', '.', '6', '\0'};
		referenced_uid.set_value_bytes(uid_value);

		generated.reset_encapsulated_pixel_data(1);
		generated.set_encoded_pixel_frame(0, std::vector<std::uint8_t>{0x01, 0x02, 0x03, 0x04});
		if (generated["NumberOfFrames"_tag].to_long().value_or(0) != 1) {
			fail("reset_encapsulated_pixel_data(1) should set NumberOfFrames");
		}

		dicom::WriteOptions write_opts;
		const auto generated_bytes = generated.write_bytes(write_opts);
		if (generated_bytes.size() < 132) fail("write_bytes should include preamble + DICM");

		std::ostringstream os(std::ios::binary);
		generated.write_to_stream(os, write_opts);
		if (os.str().size() != generated_bytes.size()) fail("write_to_stream size mismatch");

		auto generated_roundtrip =
		    read_bytes("generated-roundtrip", generated_bytes.data(), generated_bytes.size());
		if (!generated_roundtrip) fail("generated read_bytes returned null");
		auto& pix_roundtrip = generated_roundtrip->get_dataelement("PixelData"_tag);
		if (pix_roundtrip.is_missing() || !pix_roundtrip.vr().is_pixel_sequence()) {
			fail("roundtrip pixel data should be pixel sequence");
		}
		auto* pix_value = pix_roundtrip.as_pixel_sequence();
		if (!pix_value || pix_value->number_of_frames() != 1) fail("roundtrip pixel frame count mismatch");
		const auto encoded_span = pix_value->frame_encoded_span(0);
		if (encoded_span.size() != 4 ||
		    encoded_span[0] != 0x01 || encoded_span[1] != 0x02 ||
		    encoded_span[2] != 0x03 || encoded_span[3] != 0x04) {
			fail("roundtrip pixel payload mismatch");
		}

		const std::string roundtrip_path = tmp_dir + "dicomsdl_pixel_io_smoke_roundtrip.dcm";
		generated.write_file(roundtrip_path, write_opts);
		const auto generated_roundtrip_file = read_file(roundtrip_path);
		if (!generated_roundtrip_file) fail("write_file roundtrip read returned null");
		if (generated_roundtrip_file->get_dataelement("PixelData"_tag).is_missing()) {
			fail("write_file roundtrip missing pixel data");
		}
		std::remove(roundtrip_path.c_str());
	}

	std::remove(file_path.c_str());
	return 0;
}
