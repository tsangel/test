#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
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
	auto make_layout = [](dicom::pixel::DataType data_type, std::uint32_t frames,
	                      std::uint32_t rows, std::uint32_t cols,
	                      std::uint16_t samples_per_pixel,
	                      dicom::pixel::Photometric photometric,
	                      dicom::pixel::Planar planar = dicom::pixel::Planar::interleaved,
	                      std::size_t row_stride = 0,
	                      std::size_t frame_stride = 0,
	                      std::uint16_t bits_stored = 0) {
		// Keep test sources on the same normalized layout contract used by production code.
		dicom::pixel::PixelLayout layout{
		    .data_type = data_type,
		    .photometric = photometric,
		    .planar = planar,
		    .reserved = 0,
		    .rows = rows,
		    .cols = cols,
		    .frames = frames,
		    .samples_per_pixel = samples_per_pixel,
		    .bits_stored = bits_stored == 0
		                       ? dicom::pixel::normalized_bits_stored_of(data_type)
		                       : bits_stored,
		    .row_stride = row_stride,
		    .frame_stride = frame_stride,
		};
		if (layout.row_stride == 0 || layout.frame_stride == 0) {
			const auto packed = layout.packed();
			if (layout.row_stride == 0) {
				layout.row_stride = packed.row_stride;
			}
			if (layout.frame_stride == 0) {
				layout.frame_stride = packed.frame_stride;
			}
		}
		return layout;
	};
	auto make_source_span =
	    [](std::span<const std::uint8_t> bytes, const dicom::pixel::PixelLayout& layout) {
		    return dicom::pixel::ConstPixelSpan{
		        .layout = layout,
		        .bytes = bytes,
		    };
	    };

	namespace fs = std::filesystem;
	std::error_code path_ec;
	fs::path tmp_dir = fs::temp_directory_path(path_ec);
	if (path_ec || tmp_dir.empty()) {
		path_ec.clear();
		tmp_dir = fs::current_path(path_ec);
	}
	if (path_ec || tmp_dir.empty()) {
		fail("failed to resolve temp directory");
	}

	auto remove_file_or_fail = [&](const fs::path& path, const char* label) {
		std::error_code remove_ec;
		const bool removed = fs::remove(path, remove_ec);
		if (remove_ec) {
			fail(std::string(label) + " cleanup failed: " + remove_ec.message());
		}
		if (!removed) {
			fail(std::string(label) + " cleanup failed: file was not removed");
		}
	};

	const fs::path file_path = (tmp_dir / "dicomsdl_pixel_io_smoke.dcm").lexically_normal();
	const std::string file_path_text = file_path.string();
	{
		std::ofstream os(file_path, std::ios::binary);
		os << "DICM";
	}
	{
		const auto file = read_file(file_path_text);
		if (!file) fail("read_file returned null");
		if (file->has_error()) fail("read_file should not record errors for valid input");
		if (!file->error_message().empty()) fail("error_message should be empty when no read error exists");
		if (file->path() != file_path_text) fail("file path mismatch");
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

		auto& rows = file->add_dataelement("Rows"_tag, dicom::VR::US);
		if (!rows) fail("DicomFile add_dataelement failed");
		if (file->dump().find("'00280010'") == std::string::npos) {
			fail("DicomFile dump should include Rows");
		}
		file->remove_dataelement("Rows"_tag);
		if (file->get_dataelement("Rows"_tag).is_present()) {
			fail("DicomFile remove_dataelement failed");
		}

		DataSet manual;
		manual.attach_to_file(file_path_text);
		if (manual.path() != file_path_text) fail("manual path mismatch");
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

	auto make_u16_source = [&]() {
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

		return std::pair{
		    std::move(source_bytes),
		    make_layout(dicom::pixel::DataType::u16, 2, 2, 3, 1,
		        dicom::pixel::Photometric::monochrome2,
		        dicom::pixel::Planar::interleaved, 8, 20, 16),
		};
	};

	auto append_u16_le = [](std::vector<std::uint8_t>& out, std::uint16_t v) {
		out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
		out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
	};
	auto append_u32_le = [](std::vector<std::uint8_t>& out, std::uint32_t v) {
		out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
		out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
		out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFu));
		out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFFu));
	};
	auto append_bytes = [](std::vector<std::uint8_t>& out,
	                        const std::vector<std::uint8_t>& value) {
		out.insert(out.end(), value.begin(), value.end());
	};
	auto append_explicit_vr_le_16 =
	    [&](std::vector<std::uint8_t>& out, dicom::Tag tag,
	        char vr0, char vr1, const std::vector<std::uint8_t>& value) {
		    if (value.size() > 0xFFFFu) {
			    fail("append_explicit_vr_le_16 value too large");
		    }
		    append_u16_le(out, tag.group());
		    append_u16_le(out, tag.element());
		    out.push_back(static_cast<std::uint8_t>(vr0));
		    out.push_back(static_cast<std::uint8_t>(vr1));
		    append_u16_le(out, static_cast<std::uint16_t>(value.size()));
		    append_bytes(out, value);
	    };
	auto append_explicit_vr_le_32 =
	    [&](std::vector<std::uint8_t>& out, dicom::Tag tag,
	        char vr0, char vr1, const std::vector<std::uint8_t>& value,
	        bool undefined_length = false) {
		    append_u16_le(out, tag.group());
		    append_u16_le(out, tag.element());
		    out.push_back(static_cast<std::uint8_t>(vr0));
		    out.push_back(static_cast<std::uint8_t>(vr1));
		    append_u16_le(out, 0u);
		    append_u32_le(out, undefined_length
		                           ? 0xFFFFFFFFu
		                           : static_cast<std::uint32_t>(value.size()));
		    append_bytes(out, value);
	    };
	auto ui_value = [](std::string uid) {
		if (uid.empty() || uid.back() != '\0') {
			uid.push_back('\0');
		}
		if ((uid.size() & 1u) != 0u) {
			uid.push_back('\0');
		}
		return std::vector<std::uint8_t>(uid.begin(), uid.end());
	};
	auto build_part10 = [&](std::string transfer_syntax_uid,
	                         const std::vector<std::uint8_t>& body) {
		std::vector<std::uint8_t> meta_ts;
		append_explicit_vr_le_16(
		    meta_ts, dicom::Tag(0x0002u, 0x0010u), 'U', 'I',
		    ui_value(std::move(transfer_syntax_uid)));

		std::vector<std::uint8_t> meta_gl_value;
		append_u32_le(meta_gl_value, static_cast<std::uint32_t>(meta_ts.size()));

		std::vector<std::uint8_t> out(128, 0);
		out.insert(out.end(), {'D', 'I', 'C', 'M'});
		append_explicit_vr_le_16(
		    out, dicom::Tag(0x0002u, 0x0000u), 'U', 'L', meta_gl_value);
		append_bytes(out, meta_ts);
		append_bytes(out, body);
		return out;
	};
	auto build_multifragment_encapsulated_uncompressed_body = [&]() {
		std::vector<std::uint8_t> body;
		append_explicit_vr_le_16(
		    body, dicom::Tag(0x0028u, 0x0002u), 'U', 'S',
		    std::vector<std::uint8_t>{0x01u, 0x00u});
		append_explicit_vr_le_16(
		    body, dicom::Tag(0x0028u, 0x0004u), 'C', 'S',
		    std::vector<std::uint8_t>{
		        'M', 'O', 'N', 'O', 'C', 'H', 'R', 'O', 'M', 'E', '2', ' '});
		append_explicit_vr_le_16(
		    body, dicom::Tag(0x0028u, 0x0010u), 'U', 'S',
		    std::vector<std::uint8_t>{0x01u, 0x00u});
		append_explicit_vr_le_16(
		    body, dicom::Tag(0x0028u, 0x0011u), 'U', 'S',
		    std::vector<std::uint8_t>{0x02u, 0x00u});
		append_explicit_vr_le_16(
		    body, dicom::Tag(0x0028u, 0x0100u), 'U', 'S',
		    std::vector<std::uint8_t>{0x10u, 0x00u});
		append_explicit_vr_le_16(
		    body, dicom::Tag(0x0028u, 0x0101u), 'U', 'S',
		    std::vector<std::uint8_t>{0x10u, 0x00u});
		append_explicit_vr_le_16(
		    body, dicom::Tag(0x0028u, 0x0102u), 'U', 'S',
		    std::vector<std::uint8_t>{0x0Fu, 0x00u});
		append_explicit_vr_le_16(
		    body, dicom::Tag(0x0028u, 0x0103u), 'U', 'S',
		    std::vector<std::uint8_t>{0x00u, 0x00u});

		std::vector<std::uint8_t> encapsulated_pixel_value;
		append_u16_le(encapsulated_pixel_value, 0xFFFEu);
		append_u16_le(encapsulated_pixel_value, 0xE000u);
		append_u32_le(encapsulated_pixel_value, 0u);

		append_u16_le(encapsulated_pixel_value, 0xFFFEu);
		append_u16_le(encapsulated_pixel_value, 0xE000u);
		append_u32_le(encapsulated_pixel_value, 2u);
		encapsulated_pixel_value.push_back(0x34u);
		encapsulated_pixel_value.push_back(0x12u);

		append_u16_le(encapsulated_pixel_value, 0xFFFEu);
		append_u16_le(encapsulated_pixel_value, 0xE000u);
		append_u32_le(encapsulated_pixel_value, 2u);
		encapsulated_pixel_value.push_back(0x78u);
		encapsulated_pixel_value.push_back(0x56u);

		append_u16_le(encapsulated_pixel_value, 0xFFFEu);
		append_u16_le(encapsulated_pixel_value, 0xE0DDu);
		append_u32_le(encapsulated_pixel_value, 0u);

		append_explicit_vr_le_32(
		    body, dicom::Tag(0x7FE0u, 0x0010u), 'O', 'B',
		    encapsulated_pixel_value, true);
		return body;
	};

	{
		auto [source_bytes, source_layout] = make_u16_source();
		dicom::DicomFile native_file;
		native_file.set_pixel_data("ExplicitVRLittleEndian"_uid,
		    make_source_span(source_bytes, source_layout));
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
		auto [source_bytes, source_layout] = make_u16_source();
		const std::vector<std::uint8_t> expected_frame0{
		    0x01u, 0x00u, 0x02u, 0x00u, 0x03u, 0x00u,
		    0x04u, 0x00u, 0x05u, 0x00u, 0x06u, 0x00u};
		const std::vector<std::uint8_t> expected_frame1{
		    0x11u, 0x00u, 0x12u, 0x00u, 0x13u, 0x00u,
		    0x14u, 0x00u, 0x15u, 0x00u, 0x16u, 0x00u};

		dicom::DicomFile rle_file;
		rle_file.set_pixel_data("RLELossless"_uid,
		    make_source_span(source_bytes, source_layout));
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
		with_context_file.set_pixel_data("RLELossless"_uid,
		    make_source_span(source_bytes, source_layout), rle_encoder_context);
		if (with_context_file.pixel_data(0) != expected_frame0 ||
		    with_context_file.pixel_data(1) != expected_frame1) {
			fail("set_pixel_data with reusable encoder context mismatch");
		}

		if (dicom::test::kJpeg2kBuiltin) {
			dicom::DicomFile transcode_chain_file;
			transcode_chain_file.set_pixel_data("RLELossless"_uid,
			    make_source_span(source_bytes, source_layout));
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
		auto [source_bytes, source_layout] = make_u16_source();
		dicom::DicomFile encap_file;
		encap_file.set_pixel_data("EncapsulatedUncompressedExplicitVRLittleEndian"_uid,
		    make_source_span(source_bytes, source_layout));
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
		const auto color_layout = make_layout(dicom::pixel::DataType::u8, 1, 2, 2, 3,
		    dicom::pixel::Photometric::rgb);
		const auto color_source = make_source_span(rgb_source, color_layout);

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
		auto [source_bytes, source_layout] = make_u16_source();
		dicom::DicomFile jpegls_file;
		jpegls_file.set_pixel_data("JPEGLSLossless"_uid,
		    make_source_span(source_bytes, source_layout));
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
		const auto lossy_source = make_source_span(lossy_source_bytes,
		    make_layout(dicom::pixel::DataType::u8, 1, 2, 3, 1,
		        dicom::pixel::Photometric::monochrome2));
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
		auto [source_bytes, source_layout] = make_u16_source();
		dicom::DicomFile jpeg_file;
		jpeg_file.set_pixel_data("JPEGLosslessSV1"_uid,
		    make_source_span(source_bytes, source_layout));
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
		const auto lossy_source = make_source_span(lossy_source_bytes,
		    make_layout(dicom::pixel::DataType::u8, 1, 2, 3, 1,
		        dicom::pixel::Photometric::monochrome2));
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
		auto [source_bytes, source_layout] = make_u16_source();
		const std::vector<std::uint8_t> expected_frame0{
		    0x01u, 0x00u, 0x02u, 0x00u, 0x03u, 0x00u,
		    0x04u, 0x00u, 0x05u, 0x00u, 0x06u, 0x00u};
		const std::vector<std::uint8_t> expected_frame1{
		    0x11u, 0x00u, 0x12u, 0x00u, 0x13u, 0x00u,
		    0x14u, 0x00u, 0x15u, 0x00u, 0x16u, 0x00u};

		dicom::DicomFile native_file;
		native_file.set_pixel_data("ExplicitVRLittleEndian"_uid,
		    make_source_span(source_bytes, source_layout));

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
		auto [source_bytes, source_layout] = make_u16_source();
		dicom::DicomFile native_file;
		native_file.set_pixel_data("ExplicitVRLittleEndian"_uid,
		    make_source_span(source_bytes, source_layout));
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
		auto [source_bytes, source_layout] = make_u16_source();
		const std::vector<std::uint8_t> expected_frame0{
		    0x01u, 0x00u, 0x02u, 0x00u, 0x03u, 0x00u,
		    0x04u, 0x00u, 0x05u, 0x00u, 0x06u, 0x00u};
		const std::vector<std::uint8_t> expected_frame1{
		    0x11u, 0x00u, 0x12u, 0x00u, 0x13u, 0x00u,
		    0x14u, 0x00u, 0x15u, 0x00u, 0x16u, 0x00u};

		dicom::DicomFile rle_file;
		rle_file.set_pixel_data("RLELossless"_uid,
		    make_source_span(source_bytes, source_layout));

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

	{
		const auto source_bytes = build_part10(
		    "1.2.840.10008.1.2.1.98",
		    build_multifragment_encapsulated_uncompressed_body());
		auto source = read_bytes(
		    "write-with-ts-no-cache-source", source_bytes.data(), source_bytes.size());
		if (!source) fail("write_with_transfer_syntax no-cache source returned null");
		auto& pixel_data = source->get_dataelement("PixelData"_tag);
		if (pixel_data.is_missing() || !pixel_data.vr().is_pixel_sequence()) {
			fail("write_with_transfer_syntax no-cache source should be encapsulated");
		}
		auto* pixel_sequence = pixel_data.as_pixel_sequence();
		if (!pixel_sequence || pixel_sequence->number_of_frames() != 1) {
			fail("write_with_transfer_syntax no-cache source frame count mismatch");
		}
		const auto* frame0_before = pixel_sequence->frame(0);
		if (!frame0_before) {
			fail("write_with_transfer_syntax no-cache source frame 0 missing");
		}
		if (frame0_before->encoded_data_size() != 0) {
			fail("write_with_transfer_syntax no-cache source should start without materialized frame cache");
		}

		std::ostringstream os(std::ios::binary);
		source->write_with_transfer_syntax(os, "ExplicitVRLittleEndian"_uid);
		const auto output = os.str();
		auto roundtrip = read_bytes(
		    "write-with-ts-no-cache-roundtrip",
		    reinterpret_cast<const std::uint8_t*>(output.data()), output.size());
		if (!roundtrip) fail("write_with_transfer_syntax no-cache roundtrip returned null");
		const std::vector<std::uint8_t> expected_frame{
		    0x34u, 0x12u, 0x78u, 0x56u};
		if (roundtrip->pixel_data(0) != expected_frame) {
			fail("write_with_transfer_syntax no-cache roundtrip mismatch");
		}
		const auto* frame0_after = pixel_sequence->frame(0);
		if (!frame0_after || frame0_after->encoded_data_size() != 0) {
			fail("write_with_transfer_syntax should not materialize source frame cache");
		}
	}

	if (dicom::test::kJpegLsBuiltin) {
		const std::vector<std::uint8_t> lossy_source_bytes{
		    0x10u, 0x20u, 0x30u, 0x40u, 0x50u, 0x60u};
		const int near_lossless_error = 2;
		const auto lossy_source = make_source_span(lossy_source_bytes,
		    make_layout(dicom::pixel::DataType::u8, 1, 2, 3, 1,
		        dicom::pixel::Photometric::monochrome2));

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
		const auto lossy_ratio =
		    roundtrip->get_dataelement("LossyImageCompressionRatio"_tag)
		        .to_double_vector();
		if (!lossy_ratio || lossy_ratio->empty() || (*lossy_ratio)[0] <= 0.0) {
			fail("write_with_transfer_syntax JPEG-LS lossy should backpatch LossyImageCompressionRatio");
		}
		const auto roundtrip_frame = roundtrip->pixel_data(0);
		if (roundtrip_frame.size() != lossy_source_bytes.size()) {
			fail("write_with_transfer_syntax JPEG-LS lossy frame size mismatch");
		}
		for (std::size_t i = 0; i < roundtrip_frame.size(); ++i) {
			int diff = static_cast<int>(roundtrip_frame[i]) -
			    static_cast<int>(lossy_source_bytes[i]);
			if (diff < 0) {
				diff = -diff;
			}
			if (diff > near_lossless_error) {
				fail("write_with_transfer_syntax JPEG-LS lossy max abs error exceeded");
			}
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
		auto [source_bytes, source_layout] = make_u16_source();
		const std::vector<std::uint8_t> expected_frame0{
		    0x01u, 0x00u, 0x02u, 0x00u, 0x03u, 0x00u,
		    0x04u, 0x00u, 0x05u, 0x00u, 0x06u, 0x00u};
		const std::vector<std::uint8_t> expected_frame1{
		    0x11u, 0x00u, 0x12u, 0x00u, 0x13u, 0x00u,
		    0x14u, 0x00u, 0x15u, 0x00u, 0x16u, 0x00u};

		dicom::DicomFile native_file;
		native_file.set_pixel_data("ExplicitVRLittleEndian"_uid,
		    make_source_span(source_bytes, source_layout));
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
			auto& element = generated.add_dataelement(tag, vr);
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
		    sequence_item->add_dataelement("ReferencedSOPInstanceUID"_tag, dicom::VR::UI);
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

		const fs::path roundtrip_path =
		    (tmp_dir / "dicomsdl_pixel_io_smoke_roundtrip.dcm").lexically_normal();
		const std::string roundtrip_path_text = roundtrip_path.string();
		generated.write_file(roundtrip_path_text, write_opts);
		const auto generated_roundtrip_file = read_file(roundtrip_path_text);
		if (!generated_roundtrip_file) fail("write_file roundtrip read returned null");
		if (generated_roundtrip_file->get_dataelement("PixelData"_tag).is_missing()) {
			fail("write_file roundtrip missing pixel data");
		}
		remove_file_or_fail(roundtrip_path, "roundtrip file");
	}

	remove_file_or_fail(file_path, "input file");
	return 0;
}
