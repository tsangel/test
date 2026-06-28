#include <dicom.h>
#include <dicom_seg.h>

#include "codec_builtin_flags.hpp"
#include "pixel/runtime/runtime_registry.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace dicom::literals;

void* unknown_lossy_decoder_create() {
	return new int(0);
}

void unknown_lossy_decoder_destroy(void* ctx) {
	delete static_cast<int*>(ctx);
}

pixel_error_code unknown_lossy_decoder_configure(
    void*, std::uint32_t, const pixel_option_list*) {
	return PIXEL_CODEC_ERR_OK;
}

pixel_error_code unknown_lossy_decoder_decode_frame(
    void*, const pixel_decoder_request* request) {
	if (request == nullptr || request->source.source_buffer.data == nullptr ||
	    request->output.dst == nullptr) {
		return PIXEL_CODEC_ERR_INVALID_ARGUMENT;
	}
	const auto source_size = request->source.source_buffer.size;
	if (request->output.dst_size < source_size) {
		return PIXEL_CODEC_ERR_OUTPUT_TOO_SMALL;
	}
	std::memcpy(request->output.dst, request->source.source_buffer.data,
	    static_cast<std::size_t>(source_size));
	if (request->decode_info != nullptr) {
		request->decode_info->struct_size = sizeof(pixel_decoder_info);
		request->decode_info->abi_version = PIXEL_DECODER_PLUGIN_ABI;
		request->decode_info->actual_color_space =
		    PIXEL_DECODED_COLOR_SPACE_MONOCHROME;
		request->decode_info->encoded_lossy_state =
		    PIXEL_ENCODED_LOSSY_STATE_UNKNOWN;
		request->decode_info->actual_dtype = request->output.dst_dtype;
		request->decode_info->actual_planar = request->output.dst_planar;
		request->decode_info->bits_per_sample =
		    request->output.dst_dtype == PIXEL_DTYPE_U16 ? 16 : 8;
	}
	return PIXEL_CODEC_ERR_OK;
}

std::uint32_t unknown_lossy_decoder_copy_last_error_detail(
    const void*, char*, std::uint32_t) {
	return 0;
}

const pixel_decoder_plugin_api& unknown_lossy_decoder_api() {
	static const pixel_decoder_plugin_api api = [] {
		pixel_decoder_plugin_api out{};
		out.struct_size = sizeof(pixel_decoder_plugin_api);
		out.abi_version = PIXEL_DECODER_PLUGIN_ABI;
		out.info.struct_size = sizeof(pixel_decoder_plugin_info);
		out.info.abi_version = PIXEL_DECODER_PLUGIN_ABI;
		out.info.display_name = "Unknown Lossy State Decoder Test Plugin";
		out.info.supported_profile_flags = PIXEL_CODEC_PROFILE_RLE_LOSSLESS_DEC;
		out.create = &unknown_lossy_decoder_create;
		out.destroy = &unknown_lossy_decoder_destroy;
		out.configure = &unknown_lossy_decoder_configure;
		out.decode_frame = &unknown_lossy_decoder_decode_frame;
		out.copy_last_error_detail =
		    &unknown_lossy_decoder_copy_last_error_detail;
		return out;
	}();
	return api;
}

struct ExternalPluginClearGuard {
	~ExternalPluginClearGuard() {
		std::string error;
		(void)pixel::runtime::clear_external_codec_plugins(&error);
	}
};

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

template <typename Fn>
void expect_throw_contains(
    std::string_view label, Fn&& fn, std::string_view token) {
	try {
		fn();
		fail(std::string(label) + " should throw");
	} catch (const std::exception& e) {
		expect_contains(e.what(), token, label);
	}
}

void set_long(dicom::DicomFile& file, std::string_view key, long value) {
	if (!file.set_value(key, value)) {
		fail("failed to set " + std::string(key));
	}
}

void set_text(dicom::DicomFile& file, std::string_view key, std::string_view value) {
	if (!file.set_value(key, value)) {
		fail("failed to set " + std::string(key));
	}
}

template <std::size_t N>
void set_doubles(dicom::DicomFile& file, std::string_view key,
    const std::array<double, N>& values) {
	if (!file.set_value(
	        key, std::span<const double>(values.data(), values.size()))) {
		fail("failed to set " + std::string(key));
	}
}

template <std::size_t N>
void set_longs(dicom::DicomFile& file, std::string_view key,
    const std::array<long, N>& values) {
	if (!file.set_value(key, std::span<const long>(values.data(), values.size()))) {
		fail("failed to set " + std::string(key));
	}
}

template <std::size_t N>
void set_ints(dicom::DicomFile& file, std::string_view key,
    const std::array<int, N>& values) {
	if (!file.set_value(key, std::span<const int>(values.data(), values.size()))) {
		fail("failed to set " + std::string(key));
	}
}

void append_u32_le(std::vector<std::uint8_t>& out, std::uint32_t value) {
	out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
	out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
	out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFu));
	out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFFu));
}

void append_u64_le(std::vector<std::uint8_t>& out, std::uint64_t value) {
	for (int shift = 0; shift < 64; shift += 8) {
		out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
	}
}

std::vector<std::uint8_t> detached_pixel_payload_marker(
    char vr0, char vr1, std::uint32_t value_length,
    std::uint64_t payload_length) {
	std::vector<std::uint8_t> marker{
	    dicom::kPixelDataPayloadPlaceholderMagic.begin(),
	    dicom::kPixelDataPayloadPlaceholderMagic.end()};
	marker.push_back(static_cast<std::uint8_t>(vr0));
	marker.push_back(static_cast<std::uint8_t>(vr1));
	append_u32_le(marker, value_length);
	append_u64_le(marker, payload_length);
	return marker;
}

void populate_common_seg_metadata(dicom::DicomFile& file, std::string_view type,
    long frame_count, long rows, long columns) {
	set_text(file, "StudyInstanceUID", "1.2.826.0.1.3680043.10.543.1");
	set_text(file, "SeriesInstanceUID",
	    type == "BINARY" ? "1.2.826.0.1.3680043.10.543.2"
	                     : "1.2.826.0.1.3680043.10.543.3");
	const std::string_view sop_instance_uid =
	    type == "BINARY" ? "1.2.826.0.1.3680043.10.543.4"
	                     : "1.2.826.0.1.3680043.10.543.5";
	set_text(file, "SOPClassUID", "SegmentationStorage"_uid.value());
	set_text(file, "SOPInstanceUID", sop_instance_uid);
	set_text(file, "MediaStorageSOPClassUID",
	    "SegmentationStorage"_uid.value());
	set_text(file, "MediaStorageSOPInstanceUID", sop_instance_uid);
	set_text(file, "Modality", "SEG");
	set_text(file, "FrameOfReferenceUID", "1.2.826.0.1.3680043.10.543.42");
	set_text(file, "SegmentationType", type);
	set_long(file, "Rows", rows);
	set_long(file, "Columns", columns);
	set_long(file, "SamplesPerPixel", 1);
	set_text(file, "PhotometricInterpretation", "MONOCHROME2");
	set_long(file, "PixelRepresentation", 0);
	set_long(file, "NumberOfFrames", frame_count);

	const std::array<double, 2> spacing{1.5, 2.5};
	const std::array<double, 6> orientation{1.0, 0.0, 0.0, 0.0, 1.0, 0.0};
	set_doubles(file,
	    "SharedFunctionalGroupsSequence.0.PixelMeasuresSequence.0.PixelSpacing",
	    spacing);
	set_long(file,
	    "SharedFunctionalGroupsSequence.0.PixelMeasuresSequence.0.SliceThickness",
	    3);
	set_doubles(file,
	    "SharedFunctionalGroupsSequence.0.PlaneOrientationSequence.0."
	    "ImageOrientationPatient",
	    orientation);
}

void populate_segment(dicom::DicomFile& file, int ordinal, long number,
    std::string_view label) {
	const auto base = std::string("SegmentSequence.") + std::to_string(ordinal) + ".";
	set_long(file, base + "SegmentNumber", number);
	set_text(file, base + "SegmentLabel", label);
	set_text(file, base + "SegmentDescription", std::string(label) + " description");
	set_text(file, base + "SegmentAlgorithmType", "AUTOMATIC");
	set_text(file, base + "SegmentAlgorithmName", "unit-test");
	const std::array<int, 3> cielab{1000 + ordinal, 2000 + ordinal, 3000 + ordinal};
	set_ints(file, base + "RecommendedDisplayCIELabValue", cielab);
	set_text(file,
	    base + "SegmentedPropertyCategoryCodeSequence.0.CodeValue", "T-D0050");
	set_text(file,
	    base + "SegmentedPropertyCategoryCodeSequence.0.CodingSchemeDesignator",
	    "SRT");
	set_text(file,
	    base + "SegmentedPropertyCategoryCodeSequence.0.CodeMeaning", "Tissue");
}

void populate_frame(dicom::DicomFile& file, int frame_index, long segment_number,
    double z_position) {
	const auto base = std::string("PerFrameFunctionalGroupsSequence.") +
	    std::to_string(frame_index) + ".";
	set_long(file,
	    base + "SegmentIdentificationSequence.0.ReferencedSegmentNumber",
	    segment_number);
	const std::array<double, 3> position{0.0, 0.0, z_position};
	set_doubles(file, base + "PlanePositionSequence.0.ImagePositionPatient",
	    position);
	set_text(file,
	    base + "DerivationImageSequence.0.SourceImageSequence.0."
	    "ReferencedSOPClassUID",
	    "1.2.840.10008.5.1.4.1.1.2");
	set_text(file,
	    base + "DerivationImageSequence.0.SourceImageSequence.0."
	    "ReferencedSOPInstanceUID",
	    frame_index == 0 ? "1.2.826.0.1.3680043.10.543.100"
	                     : "1.2.826.0.1.3680043.10.543.101");
	const std::array<long, 2> source_frames{frame_index + 1L, frame_index + 2L};
	set_longs(file,
	    base + "DerivationImageSequence.0.SourceImageSequence.0."
	    "ReferencedFrameNumber",
	    source_frames);
}

void populate_labelmap_frame(dicom::DicomFile& file, int frame_index,
    double z_position) {
	const auto base = std::string("PerFrameFunctionalGroupsSequence.") +
	    std::to_string(frame_index) + ".";
	const std::array<double, 3> position{0.0, 0.0, z_position};
	set_doubles(file, base + "PlanePositionSequence.0.ImagePositionPatient",
	    position);
	set_text(file,
	    base + "DerivationImageSequence.0.SourceImageSequence.0."
	    "ReferencedSOPClassUID",
	    "1.2.840.10008.5.1.4.1.1.2");
	set_text(file,
	    base + "DerivationImageSequence.0.SourceImageSequence.0."
	    "ReferencedSOPInstanceUID",
	    frame_index == 0 ? "1.2.826.0.1.3680043.10.543.500"
	                     : "1.2.826.0.1.3680043.10.543.501");
}

std::unique_ptr<dicom::DicomFile> make_binary_seg_file() {
	auto file = std::make_unique<dicom::DicomFile>();
	populate_common_seg_metadata(*file, "BINARY", 2, 2, 8);
	set_long(*file, "BitsAllocated", 1);
	set_long(*file, "BitsStored", 1);
	set_long(*file, "HighBit", 0);
	populate_segment(*file, 0, 1, "First");
	populate_segment(*file, 1, 2, "Second");
	populate_frame(*file, 0, 1, 10.0);
	populate_frame(*file, 1, 2, 20.0);
	file->set_native_pixel_data(
	    std::vector<std::uint8_t>{0x55, 0x0F, 0x80, 0x33}, dicom::VR::OB);
	return file;
}

std::unique_ptr<dicom::DicomFile> make_unaligned_binary_seg_file() {
	auto file = std::make_unique<dicom::DicomFile>();
	populate_common_seg_metadata(*file, "BINARY", 2, 2, 3);
	set_long(*file, "BitsAllocated", 1);
	set_long(*file, "BitsStored", 1);
	set_long(*file, "HighBit", 0);
	populate_segment(*file, 0, 1, "First");
	populate_segment(*file, 1, 2, "Second");
	populate_frame(*file, 0, 1, 10.0);
	populate_frame(*file, 1, 2, 20.0);
	// Frame 0 has local bits 0,2,5 set. Frame 1 starts at absolute bit 6
	// and has local bits 0,1,4 set.
	file->set_native_pixel_data(
	    std::vector<std::uint8_t>{0xE5, 0x04}, dicom::VR::OB);
	return file;
}

std::unique_ptr<dicom::DicomFile> make_single_pixel_binary_seg_file(
    std::vector<std::uint8_t> pixel_data) {
	auto file = std::make_unique<dicom::DicomFile>();
	populate_common_seg_metadata(*file, "BINARY", 1, 1, 1);
	set_long(*file, "BitsAllocated", 1);
	set_long(*file, "BitsStored", 1);
	set_long(*file, "HighBit", 0);
	populate_segment(*file, 0, 1, "Single");
	populate_frame(*file, 0, 1, 10.0);
	file->set_native_pixel_data(std::move(pixel_data), dicom::VR::OB);
	return file;
}

std::unique_ptr<dicom::DicomFile> make_fractional_seg_file() {
	auto file = std::make_unique<dicom::DicomFile>();
	populate_common_seg_metadata(*file, "FRACTIONAL", 1, 2, 2);
	set_text(*file, "SegmentationFractionalType", "PROBABILITY");
	set_long(*file, "MaximumFractionalValue", 255);
	set_long(*file, "BitsAllocated", 8);
	set_long(*file, "BitsStored", 8);
	set_long(*file, "HighBit", 7);
	populate_segment(*file, 0, 1, "Fractional");
	populate_frame(*file, 0, 1, 1.0);
	file->set_native_pixel_data(
	    std::vector<std::uint8_t>{0, 128, 255, 64}, dicom::VR::OB);
	return file;
}

std::unique_ptr<dicom::DicomFile> make_non_seg_rgb_file() {
	auto file = std::make_unique<dicom::DicomFile>();
	file->set_transfer_syntax_state_only("ExplicitVRLittleEndian"_uid);
	set_long(*file, "Rows", 1);
	set_long(*file, "Columns", 2);
	set_long(*file, "SamplesPerPixel", 3);
	set_text(*file, "PhotometricInterpretation", "RGB");
	set_long(*file, "BitsAllocated", 8);
	set_long(*file, "BitsStored", 8);
	set_long(*file, "HighBit", 7);
	set_long(*file, "PixelRepresentation", 0);
	file->set_native_pixel_data(
	    std::vector<std::uint8_t>{1, 2, 3, 4, 5, 6}, dicom::VR::OB);
	return file;
}

std::unique_ptr<dicom::DicomFile> make_non_seg_signed_file() {
	auto file = std::make_unique<dicom::DicomFile>();
	file->set_transfer_syntax_state_only("ExplicitVRLittleEndian"_uid);
	set_long(*file, "Rows", 1);
	set_long(*file, "Columns", 2);
	set_long(*file, "SamplesPerPixel", 1);
	set_text(*file, "PhotometricInterpretation", "MONOCHROME2");
	set_long(*file, "BitsAllocated", 16);
	set_long(*file, "BitsStored", 16);
	set_long(*file, "HighBit", 15);
	set_long(*file, "PixelRepresentation", 1);
	file->set_native_pixel_data(
	    std::vector<std::uint8_t>{0xFF, 0xFF, 0x00, 0x00}, dicom::VR::OW);
	return file;
}

std::unique_ptr<dicom::DicomFile> make_labelmap_seg8_file(
    std::vector<std::uint8_t> pixel_data =
        std::vector<std::uint8_t>{0, 1, 2, 2, 0, 1, 0, 0, 2, 0, 0, 0}) {
	auto file = std::make_unique<dicom::DicomFile>();
	populate_common_seg_metadata(*file, "LABELMAP", 2, 2, 3);
	set_text(*file, "SOPClassUID", "LabelMapSegmentationStorage"_uid.value());
	set_text(*file, "MediaStorageSOPClassUID",
	    "LabelMapSegmentationStorage"_uid.value());
	set_long(*file, "BitsAllocated", 8);
	set_long(*file, "BitsStored", 8);
	set_long(*file, "HighBit", 7);
	set_long(*file, "PixelPaddingValue", 0);
	set_text(*file, "SegmentsOverlap", "NO");
	populate_segment(*file, 0, 0, "Background");
	populate_segment(*file, 1, 1, "One");
	populate_segment(*file, 2, 2, "Two");
	populate_segment(*file, 3, 7, "Absent");
	populate_labelmap_frame(*file, 0, 10.0);
	populate_labelmap_frame(*file, 1, 20.0);
	file->set_native_pixel_data(std::move(pixel_data), dicom::VR::OB);
	return file;
}

std::unique_ptr<dicom::DicomFile> make_labelmap_seg16_file() {
	auto file = std::make_unique<dicom::DicomFile>();
	populate_common_seg_metadata(*file, "LABELMAP", 1, 2, 2);
	set_text(*file, "SOPClassUID", "LabelMapSegmentationStorage"_uid.value());
	set_text(*file, "MediaStorageSOPClassUID",
	    "LabelMapSegmentationStorage"_uid.value());
	set_long(*file, "BitsAllocated", 16);
	set_long(*file, "BitsStored", 16);
	set_long(*file, "HighBit", 15);
	set_long(*file, "PixelPaddingValue", 0);
	populate_segment(*file, 0, 0, "Background");
	populate_segment(*file, 1, 1, "One");
	populate_segment(*file, 2, 300, "Three Hundred");
	populate_labelmap_frame(*file, 0, 10.0);
	file->set_native_pixel_data(
	    std::vector<std::uint8_t>{0, 0, 1, 0, 0x2C, 0x01, 0x2C, 0x01},
	    dicom::VR::OW);
	return file;
}

dicom::PixelFrame& require_pixel_frame(
    dicom::DicomFile& file, std::size_t frame_index) {
	auto* pixel_sequence =
	    file.dataset()["PixelData"_tag].as_pixel_sequence();
	if (!pixel_sequence) {
		fail("PixelData is not a PixelSequence");
	}
	auto* frame = pixel_sequence->frame(frame_index);
	if (!frame) {
		fail("PixelSequence frame is missing");
	}
	return *frame;
}

void set_labelmap_seg_sop_class(dicom::DicomFile& file) {
	set_text(file, "SOPClassUID", "LabelMapSegmentationStorage"_uid.value());
	set_text(file, "MediaStorageSOPClassUID",
	    "LabelMapSegmentationStorage"_uid.value());
	set_text(file, "Modality", "SEG");
}

void set_multiframe_grayscale_sop_class(dicom::DicomFile& file) {
	set_text(file, "SOPClassUID",
	    "MultiFrameGrayscaleByteSecondaryCaptureImageStorage"_uid.value());
	set_text(file, "MediaStorageSOPClassUID",
	    "MultiFrameGrayscaleByteSecondaryCaptureImageStorage"_uid.value());
	set_text(file, "Modality", "OT");
}

std::unique_ptr<dicom::seg::Segmentation> roundtrip_seg_with_transfer_syntax(
    std::unique_ptr<dicom::DicomFile> file,
    dicom::uid::WellKnown transfer_syntax,
    std::string name) {
	auto bytes = file->write_bytes_with_transfer_syntax(transfer_syntax);
	auto reread = dicom::read_bytes(std::move(name), std::move(bytes));
	return dicom::seg::from_dicomfile(std::move(reread));
}

void verify_seg_extended_offset_table_written(
    dicom::uid::WellKnown transfer_syntax) {
	auto bytes = make_labelmap_seg8_file()->write_bytes_with_transfer_syntax(
	    transfer_syntax);
	auto reread = dicom::read_bytes(
	    "labelmap8-seg-eot", bytes.data(), bytes.size());
	if (!reread) {
		fail("labelmap8 EOT reread returned null");
	}
	const auto& pixel_data = reread->get_dataelement("PixelData"_tag);
	if (pixel_data.is_missing() || !pixel_data.vr().is_pixel_sequence()) {
		fail("labelmap8 EOT roundtrip should write encapsulated PixelData");
	}
	const auto* pixel_sequence = pixel_data.as_pixel_sequence();
	if (!pixel_sequence || pixel_sequence->extended_offset_table_count() != 2) {
		fail("labelmap8 EOT roundtrip should backpatch ExtendedOffsetTable");
	}
	auto seg = dicom::seg::from_dicomfile(std::move(reread));
	std::vector<std::uint8_t> decoded0(6);
	std::vector<std::uint8_t> decoded1(6);
	seg->decode_frame_into(0, decoded0);
	seg->decode_frame_into(1, decoded1);
	if (decoded0 != std::vector<std::uint8_t>{0, 1, 2, 2, 0, 1} ||
	    decoded1 != std::vector<std::uint8_t>{0, 0, 2, 0, 0, 0}) {
		fail("labelmap8 EOT roundtrip decode mismatch");
	}
}

void verify_fractional_roundtrip(dicom::uid::WellKnown transfer_syntax) {
	auto seg = roundtrip_seg_with_transfer_syntax(
	    make_fractional_seg_file(), transfer_syntax, "fractional-seg-roundtrip");
	if (seg->segmentation_type() != dicom::seg::SegmentationType::fractional) {
		fail("fractional roundtrip segmentation type mismatch");
	}
	std::vector<std::uint8_t> decoded(4);
	seg->decode_frame_into(0, decoded);
	if (decoded != std::vector<std::uint8_t>{0, 128, 255, 64}) {
		fail("fractional roundtrip decode mismatch");
	}
	const auto mask = seg->mask_for_segment(0, 1);
	if (mask != std::vector<std::uint8_t>{0, 1, 1, 1}) {
		fail("fractional roundtrip mask mismatch");
	}
	seg->validate_label_values();
}

void verify_labelmap8_roundtrip(dicom::uid::WellKnown transfer_syntax) {
	auto seg = roundtrip_seg_with_transfer_syntax(
	    make_labelmap_seg8_file(), transfer_syntax, "labelmap8-seg-roundtrip");
	if (seg->segmentation_type() != dicom::seg::SegmentationType::labelmap ||
	    seg->labelmap_bits_allocated().value_or(0) != 8) {
		fail("labelmap8 roundtrip metadata mismatch");
	}
	if (seg->segments_overlap() != dicom::seg::SegmentsOverlap::no) {
		fail("labelmap8 roundtrip SegmentsOverlap mismatch");
	}
	std::vector<std::uint8_t> decoded0(6);
	std::vector<std::uint8_t> decoded1(6);
	seg->decode_frame_into(0, decoded0);
	seg->decode_frame_into(1, decoded1);
	if (decoded0 != std::vector<std::uint8_t>{0, 1, 2, 2, 0, 1} ||
	    decoded1 != std::vector<std::uint8_t>{0, 0, 2, 0, 0, 0}) {
		fail("labelmap8 roundtrip decode mismatch");
	}
	const auto present0 = seg->present_segment_numbers(0);
	const auto present1 = seg->present_segment_numbers(1);
	if (present0.size() != 2 || present0[0] != 1 || present0[1] != 2 ||
	    present1.size() != 1 || present1[0] != 2) {
		fail("labelmap8 roundtrip presence mismatch");
	}
	const auto mask1 = seg->mask_for_segment(0, 1);
	if (mask1 != std::vector<std::uint8_t>{0, 1, 0, 0, 0, 1}) {
		fail("labelmap8 roundtrip mask mismatch");
	}
	if (seg->frames_for_segment(2).size() != 2 ||
	    seg->frames_for_segment(7).size() != 0) {
		fail("labelmap8 roundtrip frames_for_segment mismatch");
	}
	seg->validate_label_values();
}

void verify_labelmap16_roundtrip(dicom::uid::WellKnown transfer_syntax) {
	auto seg = roundtrip_seg_with_transfer_syntax(
	    make_labelmap_seg16_file(), transfer_syntax, "labelmap16-seg-roundtrip");
	if (seg->segmentation_type() != dicom::seg::SegmentationType::labelmap ||
	    seg->labelmap_bits_allocated().value_or(0) != 16) {
		fail("labelmap16 roundtrip metadata mismatch");
	}
	std::vector<std::uint16_t> decoded(4);
	seg->decode_labelmap_frame_into(0, decoded);
	if (decoded != std::vector<std::uint16_t>{0, 1, 300, 300}) {
		fail("labelmap16 roundtrip decode mismatch");
	}
	const auto present = seg->present_segment_numbers(0);
	if (present.size() != 2 || present[0] != 1 || present[1] != 300) {
		fail("labelmap16 roundtrip presence mismatch");
	}
	const auto mask300 = seg->mask_for_segment(0, 300);
	if (mask300 != std::vector<std::uint8_t>{0, 0, 1, 1}) {
		fail("labelmap16 roundtrip mask mismatch");
	}
	seg->validate_label_values();
}

std::filesystem::path unique_temp_seg_path() {
	const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
	return std::filesystem::temp_directory_path() /
	    ("dicomsdl-segmentation-mvp-" + std::to_string(tick) + ".dcm");
}

} // namespace

int main() {
	{
		auto file = make_binary_seg_file();
		if (!dicom::seg::is_segmentation_storage(*file)) {
			fail("synthetic binary SEG should be recognized");
		}

		auto seg = dicom::seg::from_dicomfile(std::move(file));
		if (!seg || !seg->is_valid()) {
			fail("from_dicomfile should return a valid Segmentation");
		}
		if (seg->segmentation_type() != dicom::seg::SegmentationType::binary) {
			fail("binary segmentation type mismatch");
		}
		if (seg->segments_overlap() != dicom::seg::SegmentsOverlap::undefined) {
			fail("missing SegmentsOverlap should be undefined");
		}
		if (!seg->frame_of_reference_uid() ||
		    *seg->frame_of_reference_uid() !=
		        "1.2.826.0.1.3680043.10.543.42") {
			fail("FrameOfReferenceUID mismatch");
		}
		if (seg->rows() != 2 || seg->columns() != 8 ||
		    seg->segment_count() != 2 || seg->frame_count() != 2) {
			fail("basic SEG accessor mismatch");
		}
		if (seg->segments().size() != 2) {
			fail("segment count mismatch");
		}
		const auto first_segment = seg->segments()[0];
		if (first_segment.number() != 1 || first_segment.label() != "First" ||
		    first_segment.description() != "First description" ||
		    first_segment.algorithm_type() !=
		        dicom::seg::SegmentAlgorithmType::automatic_ ||
		    first_segment.algorithm_name() != "unit-test") {
			fail("first segment metadata mismatch");
		}
		const auto category = first_segment.property_category();
		if (!category || category->value != "T-D0050" ||
		    category->scheme_designator != "SRT" ||
		    category->meaning != "Tissue") {
			fail("segment code metadata mismatch");
		}
		const auto display = first_segment.recommended_display_cielab();
		if (!display || (*display)[0] != 1000 || (*display)[1] != 2000 ||
		    (*display)[2] != 3000) {
			fail("recommended display CIELab mismatch");
		}
		const auto second_segment = seg->segment_by_number(2);
		if (!second_segment || second_segment->label() != "Second") {
			fail("segment_by_number mismatch");
		}

		if (seg->frames().size() != 2 || seg->segment_frame_count(1) != 1 ||
		    seg->segment_frame_count(2) != 1) {
			fail("frame index counts mismatch");
		}
		const auto frame0 = seg->frames()[0];
		if (frame0.index() != 0 || frame0.referenced_segment_number() != 1) {
			fail("frame0 segment reference mismatch");
		}
		const auto segment2_frames = seg->frames_for_segment(2);
		if (segment2_frames.size() != 1 || segment2_frames[0].index() != 1 ||
		    segment2_frames[0].referenced_segment_number() != 2) {
			fail("frames_for_segment mismatch");
		}
		const auto position = frame0.image_position_patient();
		const auto orientation = frame0.image_orientation_patient();
		const auto spacing = frame0.pixel_spacing();
		const auto thickness = frame0.slice_thickness();
		if (!position || (*position)[2] != 10.0 || !orientation ||
		    (*orientation)[0] != 1.0 || !spacing || (*spacing)[0] != 1.5 ||
		    (*spacing)[1] != 2.5 || !thickness || *thickness != 3.0) {
			fail("frame geometry metadata mismatch");
		}
		if (!frame0.per_frame_functional_groups_item()
		         .get_dataelement("SegmentIdentificationSequence"_tag)
		         .as_sequence()) {
			fail("per_frame_functional_groups_item should expose raw item");
		}
		if (!seg->shared_functional_groups_item()
		         .get_dataelement("PixelMeasuresSequence"_tag)
		         .as_sequence()) {
			fail("shared_functional_groups_item should expose raw item");
		}
		const auto refs = frame0.source_images();
		if (refs.size() != 1 || refs[0].sop_class_uid() !=
		        "1.2.840.10008.5.1.4.1.1.2" ||
		    refs[0].sop_instance_uid() !=
		        "1.2.826.0.1.3680043.10.543.100") {
			fail("source image ref metadata mismatch");
		}
		const auto ref_frames = refs[0].referenced_frame_numbers();
		if (ref_frames.size() != 2 || ref_frames[0] != 1 || ref_frames[1] != 2) {
			fail("source image referenced frame numbers mismatch");
		}
		std::vector<std::thread> source_ref_threads;
		source_ref_threads.reserve(8);
		for (std::size_t thread_index = 0; thread_index < 8; ++thread_index) {
			source_ref_threads.emplace_back([&seg] {
				for (std::size_t iteration = 0; iteration < 500; ++iteration) {
					const auto threaded_refs = seg->frames()[0].source_images();
					if (threaded_refs.size() != 1 || threaded_refs.empty()) {
						fail("threaded source image ref size mismatch");
					}
					const auto threaded_ref = threaded_refs[0];
					if (threaded_ref.sop_class_uid() !=
					        "1.2.840.10008.5.1.4.1.1.2" ||
					    threaded_ref.sop_instance_uid() !=
					        "1.2.826.0.1.3680043.10.543.100") {
						fail("threaded source image ref metadata mismatch");
					}
					const auto threaded_ref_frames =
					    threaded_ref.referenced_frame_numbers();
					if (threaded_ref_frames.size() != 2 ||
					    threaded_ref_frames[0] != 1 ||
					    threaded_ref_frames[1] != 2) {
						fail("threaded source image referenced frame numbers mismatch");
					}
				}
			});
		}
		for (auto& thread : source_ref_threads) {
			thread.join();
		}

		std::vector<std::uint8_t> decoded0(16);
		seg->decode_frame_into(0, decoded0);
		const std::vector<std::uint8_t> expected0{
		    1, 0, 1, 0, 1, 0, 1, 0, 1, 1, 1, 1, 0, 0, 0, 0};
		if (decoded0 != expected0) {
			fail("binary frame 0 unpack mismatch");
		}
		std::vector<std::uint8_t> decoded1(16);
		seg->decode_frame_into(1, decoded1);
		const std::vector<std::uint8_t> expected1{
		    0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 0, 0, 1, 1, 0, 0};
		if (decoded1 != expected1) {
			fail("binary frame 1 unpack mismatch");
		}
		const auto bits0 = seg->binary_frame_bits(0);
		if (bits0.bytes.size() != 2 || bits0.first_bit_offset != 0 ||
		    bits0.bit_count != 16 || bits0.rows != 2 || bits0.columns != 8) {
			fail("binary frame bits view metadata mismatch");
		}
		std::vector<std::size_t> set_bits0;
		seg->for_each_binary_frame_set_bit(
		    0, [&](std::size_t pixel_index) { set_bits0.push_back(pixel_index); });
		if (set_bits0 != std::vector<std::size_t>{0, 2, 4, 6, 8, 9, 10, 11}) {
			fail("binary member set-bit iterator mismatch");
		}
		std::vector<std::size_t> free_set_bits0;
		dicom::seg::for_each_binary_frame_set_bit(
		    bits0, [&](std::size_t pixel_index) {
			    free_set_bits0.push_back(pixel_index);
		    });
		if (free_set_bits0 != set_bits0) {
			fail("binary free set-bit iterator mismatch");
		}
		expect_throw_contains("binary set-bit visitor exception",
		    [&] {
			    seg->for_each_binary_frame_set_bit(0, [](std::size_t) {
				    throw std::runtime_error("visitor stop");
			    });
		    },
		    "visitor stop");
	}

	{
		auto seg =
		    dicom::seg::from_dicomfile(make_unaligned_binary_seg_file());
		const auto bits0 = seg->binary_frame_bits(0);
		if (bits0.bytes.size() != 1 || bits0.first_bit_offset != 0 ||
		    bits0.bit_count != 6 || bits0.rows != 2 || bits0.columns != 3) {
			fail("unaligned binary frame 0 bits view metadata mismatch");
		}
		const auto bits1 = seg->binary_frame_bits(1);
		if (bits1.bytes.size() != 2 || bits1.first_bit_offset != 6 ||
		    bits1.bit_count != 6 || bits1.rows != 2 || bits1.columns != 3) {
			fail("unaligned binary frame 1 bits view metadata mismatch");
		}
		std::vector<std::size_t> set_bits0;
		std::vector<std::size_t> set_bits1;
		seg->for_each_binary_frame_set_bit(
		    0, [&](std::size_t pixel_index) { set_bits0.push_back(pixel_index); });
		seg->for_each_binary_frame_set_bit(
		    1, [&](std::size_t pixel_index) { set_bits1.push_back(pixel_index); });
		if (set_bits0 != std::vector<std::size_t>{0, 2, 5}) {
			fail("unaligned binary frame 0 set-bit iterator mismatch");
		}
		if (set_bits1 != std::vector<std::size_t>{0, 1, 4}) {
			fail("unaligned binary frame 1 set-bit iterator mismatch");
		}
		std::vector<std::uint8_t> decoded0(6);
		std::vector<std::uint8_t> decoded1(6);
		seg->decode_frame_into(0, decoded0);
		seg->decode_frame_into(1, decoded1);
		if (decoded0 != std::vector<std::uint8_t>{1, 0, 1, 0, 0, 1} ||
		    decoded1 != std::vector<std::uint8_t>{1, 1, 0, 0, 1, 0}) {
			fail("unaligned binary frame unpack mismatch");
		}
	}

	{
		auto file = make_binary_seg_file();
		file->reset_encapsulated_pixel_data(2);
		file->set_transfer_syntax_state_only(
		    "EncapsulatedUncompressedExplicitVRLittleEndian"_uid);
		file->set_encoded_pixel_frame(
		    0, std::vector<std::uint8_t>{0x55, 0x0F});
		file->set_encoded_pixel_frame(
		    1, std::vector<std::uint8_t>{0x80, 0x33});
		auto seg = dicom::seg::from_dicomfile(std::move(file));

		const auto bits0 = seg->binary_frame_bits(0);
		if (bits0.bytes.size() != 2 || bits0.first_bit_offset != 0 ||
		    bits0.bit_count != 16 || bits0.rows != 2 || bits0.columns != 8) {
			fail("encapsulated uncompressed binary frame bits view metadata mismatch");
		}
		const auto bits1 = seg->binary_frame_bits(1);
		if (bits1.bytes.size() != 2 || bits1.first_bit_offset != 0 ||
		    bits1.bit_count != 16 || bits1.rows != 2 || bits1.columns != 8) {
			fail("encapsulated uncompressed binary frame 1 bits metadata mismatch");
		}

		std::vector<std::size_t> set_bits0;
		std::vector<std::size_t> set_bits1;
		seg->for_each_binary_frame_set_bit(
		    0, [&](std::size_t pixel_index) { set_bits0.push_back(pixel_index); });
		seg->for_each_binary_frame_set_bit(
		    1, [&](std::size_t pixel_index) { set_bits1.push_back(pixel_index); });
		if (set_bits0 != std::vector<std::size_t>{0, 2, 4, 6, 8, 9, 10, 11}) {
			fail("encapsulated uncompressed binary frame 0 set-bit mismatch");
		}
		if (set_bits1 != std::vector<std::size_t>{7, 8, 9, 12, 13}) {
			fail("encapsulated uncompressed binary frame 1 set-bit mismatch");
		}

		std::vector<std::uint8_t> decoded(16);
		seg->decode_frame_into(1, decoded);
		const std::vector<std::uint8_t> expected{
		    0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 0, 0, 1, 1, 0, 0};
		if (decoded != expected) {
			fail("encapsulated uncompressed binary frame unpack mismatch");
		}
	}

	{
		auto yes_file = make_binary_seg_file();
		set_text(*yes_file, "SegmentsOverlap", "YES");
		auto yes_seg = dicom::seg::from_dicomfile(std::move(yes_file));
		if (yes_seg->segments_overlap() != dicom::seg::SegmentsOverlap::yes) {
			fail("SegmentsOverlap=YES metadata mismatch");
		}

		auto no_file = make_binary_seg_file();
		set_text(*no_file, "SegmentsOverlap", "NO");
		auto no_seg = dicom::seg::from_dicomfile(std::move(no_file));
		if (no_seg->segments_overlap() != dicom::seg::SegmentsOverlap::no) {
			fail("SegmentsOverlap=NO metadata mismatch");
		}

		auto unknown_file = make_binary_seg_file();
		set_text(*unknown_file, "SegmentsOverlap", "MAYBE");
		auto unknown_seg = dicom::seg::from_dicomfile(std::move(unknown_file));
		if (unknown_seg->segments_overlap() !=
		    dicom::seg::SegmentsOverlap::unknown) {
			fail("unknown SegmentsOverlap metadata mismatch");
		}
	}

	{
		auto file = make_binary_seg_file();
		const auto path = unique_temp_seg_path();
		std::error_code cleanup_error;
		std::filesystem::remove(path, cleanup_error);

		file->write_file(path);
		auto seg = dicom::seg::read_file(path);
		std::filesystem::remove(path, cleanup_error);
		if (seg->segmentation_type() != dicom::seg::SegmentationType::binary ||
		    seg->segments().size() != 2 || seg->frames().size() != 2 ||
		    seg->frames()[1].referenced_segment_number() != 2) {
			fail("seg::read_file SEG metadata mismatch");
		}
		std::vector<std::uint8_t> decoded(16);
		seg->decode_frame_into(1, decoded);
		const std::vector<std::uint8_t> expected{
		    0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 0, 0, 1, 1, 0, 0};
		if (decoded != expected) {
			fail("seg::read_file binary frame unpack mismatch");
		}
	}

	{
		auto file = make_binary_seg_file();
		auto bytes = file->write_bytes();

		auto seg = dicom::seg::read_bytes(
		    "synthetic-seg", bytes.data(), bytes.size());
		if (seg->frame_count() != 2 ||
		    seg->frames()[0].referenced_segment_number() != 1) {
			fail("seg::read_bytes pointer path metadata mismatch");
		}

		auto owned_seg = dicom::seg::read_bytes(
		    "synthetic-seg-owned", std::move(bytes));
		std::vector<std::uint8_t> decoded(16);
		owned_seg->decode_frame_into(0, decoded);
		const std::vector<std::uint8_t> expected{
		    1, 0, 1, 0, 1, 0, 1, 0, 1, 1, 1, 1, 0, 0, 0, 0};
		if (decoded != expected) {
			fail("seg::read_bytes owned-buffer frame unpack mismatch");
		}
	}

	{
		auto file = make_binary_seg_file();
		const auto path = unique_temp_seg_path();
		std::error_code cleanup_error;
		std::filesystem::remove(path, cleanup_error);

		file->write_file(path);
		dicom::ReadOptions partial_options;
		partial_options.load_until = "DoubleFloatPixelData"_tag;
		auto partial_file = dicom::read_file(path, partial_options);
		if (!partial_file || partial_file->has_error()) {
			fail("partial SEG file should be readable");
		}
		if (partial_file->get_dataelement("PixelData"_tag)) {
			fail("partial read should not load PixelData before decode");
		}

		auto seg = dicom::seg::from_dicomfile(std::move(partial_file));
		std::vector<std::uint8_t> decoded(16);
		seg->decode_frame_into(0, decoded);
		std::filesystem::remove(path, cleanup_error);
		const std::vector<std::uint8_t> expected{
		    1, 0, 1, 0, 1, 0, 1, 0, 1, 1, 1, 1, 0, 0, 0, 0};
		if (decoded != expected) {
			fail("partial read SEG binary frame unpack mismatch");
		}
	}

	{
		auto detached_file = make_binary_seg_file();
		detached_file->set_native_pixel_data(
		    detached_pixel_payload_marker('O', 'B', 4, 4), dicom::VR::OB);
		auto seg = dicom::seg::from_dicomfile(std::move(detached_file));
		std::vector<std::uint8_t> decoded(16);
		expect_throw_contains("detached binary SEG decode",
		    [&] { seg->decode_frame_into(0, decoded); }, "detached");
		expect_throw_contains("detached binary SEG bits",
		    [&] { (void)seg->binary_frame_bits(0); }, "detached");
	}

	{
		auto compressed_file = make_binary_seg_file();
		compressed_file->set_transfer_syntax_state_only("RLELossless"_uid);
		auto seg = dicom::seg::from_dicomfile(std::move(compressed_file));
		std::vector<std::uint8_t> decoded(16);
		expect_throw_contains("compressed binary SEG decode",
		    [&] { seg->decode_frame_into(0, decoded); },
		    "compressed BINARY SEG");
		expect_throw_contains("compressed binary SEG bits",
		    [&] { (void)seg->binary_frame_bits(0); },
		    "compressed BINARY SEG");

		auto deflated_frame_file = make_binary_seg_file();
		deflated_frame_file->set_transfer_syntax_state_only(
		    "DeflatedImageFrameCompression"_uid);
		auto deflated_frame_seg =
		    dicom::seg::from_dicomfile(std::move(deflated_frame_file));
		expect_throw_contains("Deflated Image Frame binary SEG decode",
		    [&] { deflated_frame_seg->decode_frame_into(0, decoded); },
		    "compressed BINARY SEG");
	}

	{
		expect_throw_contains("BINARY SEG write RLE preflight",
		    [&] {
			    auto file = make_binary_seg_file();
			    (void)file->write_bytes_with_transfer_syntax("RLELossless"_uid);
		    },
		    "BINARY SEG PixelData transcode");
		expect_throw_contains("BINARY SEG set_transfer_syntax RLE preflight",
		    [&] {
			    auto file = make_binary_seg_file();
			    file->set_transfer_syntax("RLELossless"_uid);
		    },
		    "BINARY SEG PixelData transcode");
		if constexpr (!dicom::test::kRleBuiltin) {
			expect_throw_contains("LABELMAP SEG RLE encoder missing write",
			    [&] {
				    auto file = make_labelmap_seg8_file();
				    (void)file->write_bytes_with_transfer_syntax(
				        "RLELossless"_uid);
			    },
			    "encoder binding");
			expect_throw_contains("LABELMAP SEG RLE encoder missing set",
			    [&] {
				    auto file = make_labelmap_seg8_file();
				    file->set_transfer_syntax("RLELossless"_uid);
			    },
			    "encoder binding");
		}
		expect_throw_contains("FRACTIONAL SEG lossy write preflight",
		    [&] {
			    auto file = make_fractional_seg_file();
			    (void)file->write_bytes_with_transfer_syntax(
			        "JPEGBaseline8Bit"_uid);
		    },
		    "lossless target transfer syntax");
		expect_throw_contains("BINARY SEG photometric write preflight",
		    [&] {
			    auto file = make_binary_seg_file();
			    set_text(*file, "PhotometricInterpretation", "RGB");
			    (void)file->write_bytes_with_transfer_syntax("RLELossless"_uid);
		    },
		    "PhotometricInterpretation=MONOCHROME2");
		expect_throw_contains("FRACTIONAL SEG photometric write preflight",
		    [&] {
			    auto file = make_fractional_seg_file();
			    set_text(*file, "PhotometricInterpretation", "PALETTE COLOR");
			    (void)file->write_bytes_with_transfer_syntax(
			        "RLELossless"_uid);
		    },
		    "PhotometricInterpretation=MONOCHROME2");
		expect_throw_contains("LABELMAP SEG metadata write preflight",
		    [&] {
			    auto file = make_labelmap_seg8_file();
			    set_long(*file, "HighBit", 6);
			    (void)file->write_bytes_with_transfer_syntax(
			        "RLELossless"_uid);
		    },
		    "HighBit=BitsAllocated-1");
		expect_throw_contains("FRACTIONAL SEG fractional type write preflight",
		    [&] {
			    auto file = make_fractional_seg_file();
			    file->remove_dataelement("SegmentationFractionalType"_tag);
			    (void)file->write_bytes_with_transfer_syntax(
			        "RLELossless"_uid);
		    },
		    "SegmentationFractionalType");
		expect_throw_contains("LABELMAP SEG overlap write preflight",
		    [&] {
			    auto file = make_labelmap_seg8_file();
			    set_text(*file, "SegmentsOverlap", "YES");
			    (void)file->write_bytes_with_transfer_syntax(
			        "RLELossless"_uid);
		    },
		    "SegmentsOverlap=NO");
		expect_throw_contains("LABELMAP SEG padding write preflight",
		    [&] {
			    auto file = make_labelmap_seg8_file();
			    auto& padding =
			        file->add_dataelement("PixelPaddingValue"_tag, dicom::VR::SS);
			    if (!padding.from_long(-1)) {
				    fail("failed to set invalid PixelPaddingValue");
			    }
			    (void)file->write_bytes_with_transfer_syntax(
			        "RLELossless"_uid);
		    },
		    "PixelPaddingValue");
		expect_throw_contains("LABELMAP SEG padding VM write preflight",
		    [&] {
			    auto file = make_labelmap_seg8_file();
			    set_longs(*file, "PixelPaddingValue", std::array<long, 2>{0, 1});
			    (void)file->write_bytes_with_transfer_syntax(
			        "RLELossless"_uid);
		    },
		    "PixelPaddingValue");
		{
			auto file = make_fractional_seg_file();
			file->set_transfer_syntax_state_only("ExplicitVRLittleEndian"_uid);
			file->remove_dataelement("SegmentationFractionalType"_tag);
			(void)file->write_bytes_with_transfer_syntax(
			    "ExplicitVRLittleEndian"_uid);
		}
		{
			dicom::WriteOptions rebuilt_meta_options{};
			rebuilt_meta_options.keep_existing_meta = false;
			(void)make_non_seg_rgb_file()->write_bytes_with_transfer_syntax(
			    "ExplicitVRLittleEndian"_uid, rebuilt_meta_options);
			(void)make_non_seg_signed_file()->write_bytes_with_transfer_syntax(
			    "ExplicitVRLittleEndian"_uid, rebuilt_meta_options);
		}
		{
			auto file = make_non_seg_rgb_file();
			file->set_transfer_syntax(
			    "EncapsulatedUncompressedExplicitVRLittleEndian"_uid);
		}
		{
			auto file = make_binary_seg_file();
			file->remove_dataelement("PixelData"_tag);
			file->set_transfer_syntax("JPEGBaseline8Bit"_uid);
		}
	}

	{
		std::vector<dicom::uid::WellKnown> transfer_syntaxes{
		    "EncapsulatedUncompressedExplicitVRLittleEndian"_uid,
		};
		if constexpr (dicom::test::kRleBuiltin) {
			transfer_syntaxes.push_back("RLELossless"_uid);
		}
		for (const auto transfer_syntax : transfer_syntaxes) {
			verify_fractional_roundtrip(transfer_syntax);
			verify_labelmap8_roundtrip(transfer_syntax);
			verify_labelmap16_roundtrip(transfer_syntax);
			verify_seg_extended_offset_table_written(transfer_syntax);
		}
	}

	{
		auto invalid_fractional_read = make_fractional_seg_file();
		set_long(*invalid_fractional_read, "MaximumFractionalValue", 128);
		auto invalid_fractional_seg =
		    dicom::seg::from_dicomfile(std::move(invalid_fractional_read));
		std::vector<std::uint8_t> decoded(4);
		expect_throw_contains("fractional read sample validation",
		    [&] { invalid_fractional_seg->decode_frame_into(0, decoded); },
		    "MaximumFractionalValue");
		expect_throw_contains("fractional mask sample validation",
		    [&] { (void)invalid_fractional_seg->mask_for_segment(0, 1); },
		    "MaximumFractionalValue");
		expect_throw_contains("fractional validate sample validation",
		    [&] { invalid_fractional_seg->validate_label_values(); },
		    "MaximumFractionalValue");

		auto invalid_fractional = make_fractional_seg_file();
		set_long(*invalid_fractional, "MaximumFractionalValue", 128);
		expect_throw_contains("fractional transcode sample validation",
		    [&] {
			    (void)invalid_fractional->write_bytes_with_transfer_syntax(
			        "EncapsulatedUncompressedExplicitVRLittleEndian"_uid);
		    },
		    "MaximumFractionalValue");
		auto invalid_fractional_set = make_fractional_seg_file();
		set_long(*invalid_fractional_set, "MaximumFractionalValue", 128);
		expect_throw_contains("fractional set_transfer sample validation",
		    [&] {
			    invalid_fractional_set->set_transfer_syntax(
			        "EncapsulatedUncompressedExplicitVRLittleEndian"_uid);
		    },
		    "MaximumFractionalValue");

		auto invalid_labelmap = make_labelmap_seg8_file(
		    std::vector<std::uint8_t>{0, 1, 9, 2, 0, 1, 0, 0, 2, 0, 0, 0});
		expect_throw_contains("labelmap native transcode label validation",
		    [&] {
			    (void)invalid_labelmap->write_bytes_with_transfer_syntax(
			        "EncapsulatedUncompressedExplicitVRLittleEndian"_uid);
		    },
		    "undefined segment");
		auto invalid_labelmap_set = make_labelmap_seg8_file(
		    std::vector<std::uint8_t>{0, 1, 9, 2, 0, 1, 0, 0, 2, 0, 0, 0});
		expect_throw_contains("labelmap native set_transfer label validation",
		    [&] {
			    invalid_labelmap_set->set_transfer_syntax(
			        "EncapsulatedUncompressedExplicitVRLittleEndian"_uid);
		    },
		    "undefined segment");

		auto encapsulated_bytes =
		    make_labelmap_seg8_file()->write_bytes_with_transfer_syntax(
		        "EncapsulatedUncompressedExplicitVRLittleEndian"_uid);
		auto encapsulated_set_bytes = encapsulated_bytes;
		auto invalid_encapsulated = dicom::read_bytes(
		    "labelmap-encapsulated-invalid-segment", std::move(encapsulated_bytes));
		set_long(*invalid_encapsulated, "SegmentSequence.2.SegmentNumber", 9);
		expect_throw_contains("labelmap encapsulated transcode label validation",
		    [&] {
			    (void)invalid_encapsulated->write_bytes_with_transfer_syntax(
			        "ExplicitVRLittleEndian"_uid);
		    },
		    "undefined segment");
		auto invalid_encapsulated_set = dicom::read_bytes(
		    "labelmap-encapsulated-invalid-segment-set",
		    std::move(encapsulated_set_bytes));
		set_long(*invalid_encapsulated_set, "SegmentSequence.2.SegmentNumber", 9);
		expect_throw_contains("labelmap encapsulated set_transfer label validation",
		    [&] {
			    invalid_encapsulated_set->set_transfer_syntax(
			        "ExplicitVRLittleEndian"_uid);
		    },
		    "undefined segment");
	}

	{
		auto encapsulated_bytes =
		    make_labelmap_seg8_file()->write_bytes_with_transfer_syntax(
		        "EncapsulatedUncompressedExplicitVRLittleEndian"_uid);
		auto frame_count_mismatch = dicom::read_bytes(
		    "labelmap-encapsulated-frame-count-mismatch",
		    std::move(encapsulated_bytes));
		set_long(*frame_count_mismatch, "NumberOfFrames", 3);
		expect_throw_contains("labelmap encapsulated frame count mismatch",
		    [&] {
			    (void)frame_count_mismatch->write_bytes_with_transfer_syntax(
			        "ExplicitVRLittleEndian"_uid);
		    },
		    "frame index out of range");

		auto missing_fragments = make_labelmap_seg8_file();
		missing_fragments->reset_encapsulated_pixel_data(2);
		missing_fragments->set_transfer_syntax_state_only(
		    "EncapsulatedUncompressedExplicitVRLittleEndian"_uid);
		expect_throw_contains("labelmap encapsulated missing fragments",
		    [&] {
			    (void)missing_fragments->write_bytes_with_transfer_syntax(
			        "ExplicitVRLittleEndian"_uid);
		    },
		    "no fragments");

		auto zero_length_fragment = make_labelmap_seg8_file();
		zero_length_fragment->reset_encapsulated_pixel_data(2);
		zero_length_fragment->set_transfer_syntax_state_only(
		    "EncapsulatedUncompressedExplicitVRLittleEndian"_uid);
		require_pixel_frame(*zero_length_fragment, 0)
		    .set_fragments(std::vector<dicom::PixelFragment>{{0, 0}});
		expect_throw_contains("labelmap encapsulated zero-length fragment",
		    [&] {
			    (void)zero_length_fragment->write_bytes_with_transfer_syntax(
			        "ExplicitVRLittleEndian"_uid);
		    },
		    "zero-length fragment");

		auto short_frame = make_labelmap_seg8_file();
		short_frame->reset_encapsulated_pixel_data(2);
		short_frame->set_transfer_syntax_state_only(
		    "EncapsulatedUncompressedExplicitVRLittleEndian"_uid);
		short_frame->set_encoded_pixel_frame(
		    0, std::vector<std::uint8_t>{0, 1, 2});
		short_frame->set_encoded_pixel_frame(
		    1, std::vector<std::uint8_t>{0, 0, 2, 0, 0, 0});
		expect_throw_contains("labelmap encapsulated decoded length mismatch",
		    [&] {
			    (void)short_frame->write_bytes_with_transfer_syntax(
			        "ExplicitVRLittleEndian"_uid);
		    },
		    "shorter");
	}

	{
		auto unsupported_decode_file = make_labelmap_seg8_file();
		unsupported_decode_file->reset_encapsulated_pixel_data(2);
		unsupported_decode_file->set_transfer_syntax_state_only("MPEG2MPML"_uid);
		unsupported_decode_file->set_encoded_pixel_frame(
		    0, std::vector<std::uint8_t>{0x00});
		unsupported_decode_file->set_encoded_pixel_frame(
		    1, std::vector<std::uint8_t>{0x00});
		auto unsupported_seg =
		    dicom::seg::from_dicomfile(std::move(unsupported_decode_file));
		std::vector<std::uint8_t> decoded(6);
		expect_throw_contains("labelmap unsupported source codec decode reject",
		    [&] { unsupported_seg->decode_frame_into(0, decoded); },
		    "decoder binding");

		auto unsupported_transcode_file = make_labelmap_seg8_file();
		unsupported_transcode_file->reset_encapsulated_pixel_data(2);
		unsupported_transcode_file->set_transfer_syntax_state_only(
		    "MPEG2MPML"_uid);
		unsupported_transcode_file->set_encoded_pixel_frame(
		    0, std::vector<std::uint8_t>{0x00});
		unsupported_transcode_file->set_encoded_pixel_frame(
		    1, std::vector<std::uint8_t>{0x00});
		expect_throw_contains(
		    "labelmap unsupported source codec transcode reject",
		    [&] {
			    (void)unsupported_transcode_file->write_bytes_with_transfer_syntax(
			        "ExplicitVRLittleEndian"_uid);
		    },
		    "decoder binding");
	}

	if constexpr (dicom::test::kJpegBuiltin) {
		auto lossy_source = make_labelmap_seg8_file();
		set_multiframe_grayscale_sop_class(*lossy_source);
		auto lossy_bytes = lossy_source->write_bytes_with_transfer_syntax(
		    "JPEGBaseline8Bit"_uid);

		auto lossy_decode_bytes = lossy_bytes;
		auto lossy_set_bytes = lossy_bytes;
		auto lossy_decode_file = dicom::read_bytes(
		    "labelmap-lossy-jpeg-source-decode", std::move(lossy_decode_bytes));
		set_labelmap_seg_sop_class(*lossy_decode_file);
		auto lossy_seg = dicom::seg::from_dicomfile(std::move(lossy_decode_file));
		std::vector<std::uint8_t> decoded(6);
		expect_throw_contains("labelmap lossy source decode reject",
		    [&] { lossy_seg->decode_frame_into(0, decoded); },
		    "lossless source");

		auto lossy_transcode_file = dicom::read_bytes(
		    "labelmap-lossy-jpeg-source-transcode", std::move(lossy_bytes));
		set_labelmap_seg_sop_class(*lossy_transcode_file);
		expect_throw_contains("labelmap lossy source transcode reject",
		    [&] {
			    (void)lossy_transcode_file->write_bytes_with_transfer_syntax(
			        "ExplicitVRLittleEndian"_uid);
		    },
		    "lossless source");
		auto lossy_set_file = dicom::read_bytes(
		    "labelmap-lossy-jpeg-source-set", std::move(lossy_set_bytes));
		set_labelmap_seg_sop_class(*lossy_set_file);
		expect_throw_contains("labelmap lossy source set_transfer reject",
		    [&] {
			    lossy_set_file->set_transfer_syntax(
			        "ExplicitVRLittleEndian"_uid);
		    },
		    "lossless source");
	}

	if constexpr (dicom::test::kJpegLsBuiltin) {
		auto near_lossless_source = make_labelmap_seg8_file();
		set_multiframe_grayscale_sop_class(*near_lossless_source);
		auto near_lossless_bytes =
		    near_lossless_source->write_bytes_with_transfer_syntax(
		        "JPEGLSNearLossless"_uid);

		auto near_lossless_decode_bytes = near_lossless_bytes;
		auto near_lossless_decode_file = dicom::read_bytes(
		    "labelmap-near-lossless-jpegls-source-decode",
		    std::move(near_lossless_decode_bytes));
		set_labelmap_seg_sop_class(*near_lossless_decode_file);
		auto near_lossless_seg =
		    dicom::seg::from_dicomfile(std::move(near_lossless_decode_file));
		std::vector<std::uint8_t> decoded(6);
		expect_throw_contains("labelmap near-lossless source decode reject",
		    [&] { near_lossless_seg->decode_frame_into(0, decoded); },
		    "lossless source");

		auto near_lossless_transcode_file = dicom::read_bytes(
		    "labelmap-near-lossless-jpegls-source-transcode",
		    std::move(near_lossless_bytes));
		set_labelmap_seg_sop_class(*near_lossless_transcode_file);
		expect_throw_contains("labelmap near-lossless source transcode reject",
		    [&] {
			    (void)near_lossless_transcode_file
			        ->write_bytes_with_transfer_syntax(
			            "ExplicitVRLittleEndian"_uid);
		    },
		    "lossless source");
	}

	{
		ExternalPluginClearGuard plugin_guard{};
		std::string plugin_error;
		if (!pixel::runtime::test::register_external_codec_plugin_api_for_test(
		        &unknown_lossy_decoder_api(), nullptr, &plugin_error)) {
			fail("failed to register unknown lossy-state decoder test plugin: " +
			    plugin_error);
		}

		auto unknown_lossy_decode_file = make_labelmap_seg8_file();
		unknown_lossy_decode_file->reset_encapsulated_pixel_data(2);
		unknown_lossy_decode_file->set_transfer_syntax_state_only("RLELossless"_uid);
		unknown_lossy_decode_file->set_encoded_pixel_frame(
		    0, std::vector<std::uint8_t>{0, 1, 2, 2, 0, 1});
		unknown_lossy_decode_file->set_encoded_pixel_frame(
		    1, std::vector<std::uint8_t>{0, 0, 2, 0, 0, 0});
		auto unknown_lossy_seg =
		    dicom::seg::from_dicomfile(std::move(unknown_lossy_decode_file));
		std::vector<std::uint8_t> decoded(6);
		expect_throw_contains("labelmap unknown lossy-state source decode reject",
		    [&] { unknown_lossy_seg->decode_frame_into(0, decoded); },
		    "lossless source");

		auto unknown_lossy_transcode_file = make_labelmap_seg8_file();
		unknown_lossy_transcode_file->reset_encapsulated_pixel_data(2);
		unknown_lossy_transcode_file->set_transfer_syntax_state_only(
		    "RLELossless"_uid);
		unknown_lossy_transcode_file->set_encoded_pixel_frame(
		    0, std::vector<std::uint8_t>{0, 1, 2, 2, 0, 1});
		unknown_lossy_transcode_file->set_encoded_pixel_frame(
		    1, std::vector<std::uint8_t>{0, 0, 2, 0, 0, 0});
		expect_throw_contains("labelmap unknown lossy-state source transcode reject",
		    [&] {
			    (void)unknown_lossy_transcode_file
			        ->write_bytes_with_transfer_syntax("ExplicitVRLittleEndian"_uid);
		    },
		    "lossless source");
	}

	{
		auto short_pixel_data = make_binary_seg_file();
		short_pixel_data->set_native_pixel_data(
		    std::vector<std::uint8_t>{0x00}, dicom::VR::OB);
		auto seg = dicom::seg::from_dicomfile(std::move(short_pixel_data));
		std::vector<std::uint8_t> decoded(16);
		expect_throw_contains("short binary SEG PixelData",
		    [&] { seg->decode_frame_into(0, decoded); },
		    "PixelData size mismatch");
		expect_throw_contains("short binary SEG bits",
		    [&] { (void)seg->binary_frame_bits(0); },
		    "PixelData size mismatch");
	}

	{
		auto missing_pixel_data = make_binary_seg_file();
		missing_pixel_data->remove_dataelement("PixelData"_tag);
		auto seg = dicom::seg::from_dicomfile(std::move(missing_pixel_data));
		std::vector<std::uint8_t> decoded(16);
		expect_throw_contains("missing binary SEG decode",
		    [&] { seg->decode_frame_into(0, decoded); },
		    "PixelData is missing");
		expect_throw_contains("missing binary SEG bits",
		    [&] { (void)seg->binary_frame_bits(0); },
		    "PixelData is missing");
	}

	{
		auto seg = dicom::seg::from_dicomfile(make_fractional_seg_file());
		expect_throw_contains("fractional binary frame bits",
		    [&] { (void)seg->binary_frame_bits(0); },
		    "BINARY SEG");
		auto labelmap_seg = dicom::seg::from_dicomfile(make_labelmap_seg8_file());
		expect_throw_contains("labelmap binary frame bits",
		    [&] { (void)labelmap_seg->binary_frame_bits(0); },
		    "BINARY SEG");
	}

	{
		auto padded_pixel_data =
		    dicom::seg::from_dicomfile(make_single_pixel_binary_seg_file(
		        std::vector<std::uint8_t>{0x01, 0x00}));
		std::vector<std::uint8_t> decoded(1);
		padded_pixel_data->decode_frame_into(0, decoded);
		if (decoded != std::vector<std::uint8_t>{1}) {
			fail("binary SEG zero padding byte decode mismatch");
		}

		auto nonzero_padding =
		    dicom::seg::from_dicomfile(make_single_pixel_binary_seg_file(
		        std::vector<std::uint8_t>{0x01, 0x7F}));
		expect_throw_contains("non-zero binary SEG PixelData padding byte",
		    [&] { nonzero_padding->decode_frame_into(0, decoded); },
		    "padding byte");

		auto extra_even_length_padding = make_unaligned_binary_seg_file();
		extra_even_length_padding->set_native_pixel_data(
		    std::vector<std::uint8_t>{0xE5, 0x04, 0x00}, dicom::VR::OB);
		auto extra_even_seg =
		    dicom::seg::from_dicomfile(std::move(extra_even_length_padding));
		std::vector<std::uint8_t> even_decoded(6);
		expect_throw_contains("extra binary SEG PixelData byte",
		    [&] { extra_even_seg->decode_frame_into(0, even_decoded); },
		    "PixelData size mismatch");

		auto encapsulated_padded_file = make_single_pixel_binary_seg_file({});
		encapsulated_padded_file->reset_encapsulated_pixel_data(1);
		encapsulated_padded_file->set_transfer_syntax_state_only(
		    "EncapsulatedUncompressedExplicitVRLittleEndian"_uid);
		encapsulated_padded_file->set_encoded_pixel_frame(
		    0, std::vector<std::uint8_t>{0x01, 0x00});
		auto encapsulated_padded =
		    dicom::seg::from_dicomfile(std::move(encapsulated_padded_file));
		encapsulated_padded->decode_frame_into(0, decoded);
		if (decoded != std::vector<std::uint8_t>{1}) {
			fail("encapsulated binary SEG zero padding byte decode mismatch");
		}

		auto encapsulated_nonzero_padding_file =
		    make_single_pixel_binary_seg_file({});
		encapsulated_nonzero_padding_file->reset_encapsulated_pixel_data(1);
		encapsulated_nonzero_padding_file->set_transfer_syntax_state_only(
		    "EncapsulatedUncompressedExplicitVRLittleEndian"_uid);
		encapsulated_nonzero_padding_file->set_encoded_pixel_frame(
		    0, std::vector<std::uint8_t>{0x01, 0x7F});
		auto encapsulated_nonzero_padding = dicom::seg::from_dicomfile(
		    std::move(encapsulated_nonzero_padding_file));
		expect_throw_contains(
		    "encapsulated non-zero binary SEG PixelData padding byte",
		    [&] { encapsulated_nonzero_padding->decode_frame_into(0, decoded); },
		    "padding byte");

		auto encapsulated_missing_frame = make_binary_seg_file();
		encapsulated_missing_frame->reset_encapsulated_pixel_data(2);
		encapsulated_missing_frame->set_transfer_syntax_state_only(
		    "EncapsulatedUncompressedExplicitVRLittleEndian"_uid);
		auto encapsulated_missing_seg =
		    dicom::seg::from_dicomfile(std::move(encapsulated_missing_frame));
		expect_throw_contains("encapsulated binary SEG missing frame payload",
		    [&] { (void)encapsulated_missing_seg->binary_frame_bits(0); },
		    "PixelData size mismatch");
	}

	{
		auto seg = dicom::seg::from_dicomfile(make_fractional_seg_file());
		if (seg->segmentation_type() != dicom::seg::SegmentationType::fractional ||
		    seg->fractional_type() !=
		        dicom::seg::SegmentationFractionalType::probability ||
		    !seg->maximum_fractional_value() ||
		    *seg->maximum_fractional_value() != 255) {
			fail("fractional segmentation metadata mismatch");
		}
		std::vector<std::uint8_t> decoded(4);
		seg->decode_frame_into(0, decoded);
		if (decoded != std::vector<std::uint8_t>{0, 128, 255, 64}) {
			fail("fractional frame decode mismatch");
		}
		const auto scaled_second =
		    static_cast<double>(decoded[1]) / *seg->maximum_fractional_value();
		if (scaled_second < 0.501 || scaled_second > 0.503) {
			fail("fractional MaximumFractionalValue scaling mismatch");
		}
	}

	{
		auto missing_fractional_type = make_fractional_seg_file();
		missing_fractional_type->remove_dataelement("SegmentationFractionalType"_tag);
		expect_throw_contains("missing SegmentationFractionalType",
		    [&] {
			    (void)dicom::seg::from_dicomfile(
			        std::move(missing_fractional_type));
		    },
		    "SegmentationFractionalType");

		auto missing_maximum = make_fractional_seg_file();
		missing_maximum->remove_dataelement("MaximumFractionalValue"_tag);
		expect_throw_contains("missing MaximumFractionalValue",
		    [&] {
			    (void)dicom::seg::from_dicomfile(std::move(missing_maximum));
		    },
		    "MaximumFractionalValue");
	}

	{
		auto labelmap_type = make_binary_seg_file();
		set_text(*labelmap_type, "SegmentationType", "LABELMAP");
		expect_throw_contains("LABELMAP SegmentationType with legacy SOP",
		    [&] { (void)dicom::seg::from_dicomfile(std::move(labelmap_type)); },
		    "Label Map");

		auto labelmap_sop_class = make_binary_seg_file();
		set_text(*labelmap_sop_class, "SOPClassUID",
		    "LabelMapSegmentationStorage"_uid.value());
		set_text(*labelmap_sop_class, "MediaStorageSOPClassUID",
		    "LabelMapSegmentationStorage"_uid.value());
		expect_throw_contains("LABELMAP SOP Class",
		    [&] {
			    (void)dicom::seg::from_dicomfile(std::move(labelmap_sop_class));
		    },
		    "SegmentationType=LABELMAP");
	}

	{
		auto file = make_labelmap_seg8_file();
		if (dicom::seg::is_segmentation_storage(*file) ||
		    !dicom::seg::is_labelmap_segmentation_storage(*file) ||
		    !dicom::seg::is_any_segmentation_storage(*file)) {
			fail("labelmap SOP classification mismatch");
		}
		auto seg = dicom::seg::from_dicomfile(std::move(file));
		if (seg->segmentation_type() != dicom::seg::SegmentationType::labelmap ||
		    !seg->labelmap_bits_allocated() || *seg->labelmap_bits_allocated() != 8) {
			fail("labelmap metadata mismatch");
		}
		const auto frame0 = seg->frames()[0];
		expect_throw_contains("labelmap referenced_segment_number",
		    [&] { (void)frame0.referenced_segment_number(); },
		    "LABELMAP");
		const auto present0 = frame0.present_segment_numbers();
		if (present0.size() != 2 || present0[0] != 1 || present0[1] != 2) {
			fail("labelmap frame presence mismatch");
		}
		std::vector<std::uint8_t> decoded0(6);
		seg->decode_frame_into(0, decoded0);
		if (decoded0 != std::vector<std::uint8_t>{0, 1, 2, 2, 0, 1}) {
			fail("labelmap 8-bit decode mismatch");
		}
		const auto mask1 = seg->mask_for_segment(0, 1);
		if (mask1 != std::vector<std::uint8_t>{0, 1, 0, 0, 0, 1}) {
			fail("labelmap segment mask mismatch");
		}
		if (seg->frames_for_segment(2).size() != 2 ||
		    seg->segment_frame_count(0) != 0 ||
		    seg->segment_frame_count(7) != 0 ||
		    seg->frames_for_segment(99).size() != 0) {
			fail("labelmap frames_for_segment mismatch");
		}
		const auto background_mask = seg->mask_for_segment(0, 0);
		if (background_mask != std::vector<std::uint8_t>{1, 0, 0, 0, 1, 0}) {
			fail("labelmap background mask mismatch");
		}
		seg->validate_label_values();
	}

	{
		auto range_file = make_labelmap_seg8_file();
		set_long(*range_file, "PixelPaddingRangeLimit", 1);
		expect_throw_contains("labelmap PixelPaddingRangeLimit",
		    [&] { (void)dicom::seg::from_dicomfile(std::move(range_file)); },
		    "PixelPaddingRangeLimit");

		auto missing_background = make_labelmap_seg8_file();
		set_long(*missing_background, "PixelPaddingValue", 255);
		expect_throw_contains("labelmap PixelPaddingValue missing segment",
		    [&] { (void)dicom::seg::from_dicomfile(std::move(missing_background)); },
		    "PixelPaddingValue");

		auto binary_zero_segment = make_binary_seg_file();
		set_long(*binary_zero_segment, "SegmentSequence.0.SegmentNumber", 0);
		expect_throw_contains("binary SegmentNumber zero",
		    [&] { (void)dicom::seg::from_dicomfile(std::move(binary_zero_segment)); },
		    "SegmentNumber 0");
	}

	{
		auto bad_label = make_labelmap_seg8_file(
		    std::vector<std::uint8_t>{0, 1, 9, 0, 0, 0, 0, 0, 0, 0, 0, 0});
		auto seg = dicom::seg::from_dicomfile(std::move(bad_label));
		expect_throw_contains("labelmap unknown label validation",
		    [&] { (void)seg->present_segment_numbers(0); },
		    "undefined segment");
	}

	{
		auto missing_pixel_data = make_labelmap_seg8_file();
		missing_pixel_data->remove_dataelement("PixelData"_tag);
		auto seg = dicom::seg::from_dicomfile(std::move(missing_pixel_data));
		std::vector<std::uint8_t> decoded(6);
		expect_throw_contains("missing labelmap PixelData",
		    [&] { seg->decode_frame_into(0, decoded); },
		    "PixelData is missing");

		auto detached_file = make_labelmap_seg8_file();
		detached_file->set_native_pixel_data(
		    detached_pixel_payload_marker('O', 'B', 12, 12), dicom::VR::OB);
		auto detached_seg = dicom::seg::from_dicomfile(std::move(detached_file));
		expect_throw_contains("detached labelmap PixelData",
		    [&] { detached_seg->decode_frame_into(0, decoded); },
		    "detached");

		auto pixel_sequence_file = make_labelmap_seg8_file();
		pixel_sequence_file->reset_encapsulated_pixel_data(2);
		pixel_sequence_file->set_transfer_syntax_state_only(
		    "ExplicitVRLittleEndian"_uid);
		auto pixel_sequence_seg =
		    dicom::seg::from_dicomfile(std::move(pixel_sequence_file));
		expect_throw_contains("labelmap PixelSequence",
		    [&] { pixel_sequence_seg->decode_frame_into(0, decoded); },
		    "PixelSequence");
	}

	{
		auto seg = dicom::seg::from_dicomfile(make_labelmap_seg16_file());
		std::vector<std::uint16_t> decoded(4);
		seg->decode_labelmap_frame_into(0, decoded);
		if (decoded != std::vector<std::uint16_t>{0, 1, 300, 300}) {
			fail("labelmap 16-bit decode mismatch");
		}
		const auto bytes = seg->decode_labelmap_frame_bytes(0);
		if (bytes != std::vector<std::uint8_t>{0, 0, 1, 0, 0x2C, 0x01, 0x2C, 0x01}) {
			fail("labelmap 16-bit native bytes mismatch");
		}
		const auto present = seg->present_segment_numbers(0);
		if (present.size() != 2 || present[0] != 1 || present[1] != 300) {
			fail("labelmap 16-bit presence mismatch");
		}
	}

#ifdef DICOMSDL_SEGMENTATION_TEST_HOOKS
	{
		auto seg = dicom::seg::from_dicomfile(make_labelmap_seg8_file());
		dicom::seg::detail::reset_labelmap_frame_scan_count();

		std::vector<std::uint8_t> decoded0(6);
		seg->decode_frame_into(0, decoded0);
		if (dicom::seg::detail::labelmap_frame_scan_count() != 1) {
			fail("labelmap decode should scan frame 0 once");
		}

		const auto present0 = seg->present_segment_numbers(0);
		if (present0.size() != 2 || present0[0] != 1 || present0[1] != 2 ||
		    dicom::seg::detail::labelmap_frame_scan_count() != 1) {
			fail("present_segment_numbers should reuse decoded frame cache");
		}

		const auto segment2_frames = seg->frames_for_segment(2);
		if (segment2_frames.size() != 2 ||
		    dicom::seg::detail::labelmap_frame_scan_count() != 2) {
			fail("frames_for_segment should scan only uncached labelmap frames");
		}

		seg->validate_label_values();
		if (dicom::seg::detail::labelmap_frame_scan_count() != 2) {
			fail("validate_label_values should reuse published all-frame index");
		}

		if (seg->segment_frame_count(7) != 0 ||
		    dicom::seg::detail::labelmap_frame_scan_count() != 2) {
			fail("known absent label should use published empty index result");
		}
		const auto absent_mask = seg->mask_for_segment(0, 7);
		if (absent_mask != std::vector<std::uint8_t>(6, 0) ||
		    dicom::seg::detail::labelmap_frame_scan_count() != 2) {
			fail("known absent label mask should reuse presence cache");
		}
	}

	{
		auto seg = dicom::seg::from_dicomfile(make_labelmap_seg8_file());
		dicom::seg::detail::reset_labelmap_frame_scan_count();
		std::atomic<int> failures{0};
		std::vector<std::thread> workers;
		workers.reserve(8);
		for (int worker_index = 0; worker_index < 8; ++worker_index) {
			workers.emplace_back([&] {
				try {
					if (seg->frames_for_segment(2).size() != 2) {
						++failures;
					}
				} catch (...) {
					++failures;
				}
			});
		}
		for (auto& worker : workers) {
			worker.join();
		}
		if (failures.load() != 0) {
			fail("concurrent labelmap frame indexing returned inconsistent results");
		}
		if (dicom::seg::detail::labelmap_frame_scan_count() != 2) {
			fail("concurrent labelmap frame indexing should build once");
		}
	}

	{
		auto seg = dicom::seg::from_dicomfile(make_labelmap_seg8_file());
		dicom::seg::detail::reset_labelmap_frame_scan_count();
		std::atomic<int> failures{0};
		std::vector<std::thread> workers;
		workers.reserve(8);
		for (int worker_index = 0; worker_index < 8; ++worker_index) {
			workers.emplace_back([&] {
				try {
					for (int iteration = 0; iteration < 40; ++iteration) {
						const auto frame_index =
						    static_cast<std::size_t>(iteration % 2);
						const auto present =
						    seg->present_segment_numbers(frame_index);
						if (frame_index == 0) {
							if (present.size() != 2 || present[0] != 1 ||
							    present[1] != 2) {
								++failures;
							}
						} else if (present.size() != 1 || present[0] != 2) {
							++failures;
						}
						if (seg->frames_for_segment(2).size() != 2) {
							++failures;
						}
						seg->validate_label_values();
					}
				} catch (...) {
					++failures;
				}
			});
		}
		for (auto& worker : workers) {
			worker.join();
		}
		if (failures.load() != 0) {
			fail("labelmap concurrent lazy scan returned inconsistent results");
		}
		const auto scan_count =
		    dicom::seg::detail::labelmap_frame_scan_count();
		if (scan_count == 0) {
			fail("labelmap concurrent lazy scan should scan at least once");
		}
		seg->validate_label_values();
		if (dicom::seg::detail::labelmap_frame_scan_count() != scan_count) {
			fail("labelmap concurrent lazy scan should publish reusable index");
		}
	}
#endif

	{
		auto missing_segment_sequence = make_binary_seg_file();
		missing_segment_sequence->remove_dataelement("SegmentSequence"_tag);
		expect_throw_contains("missing SegmentSequence",
		    [&] {
			    (void)dicom::seg::from_dicomfile(
			        std::move(missing_segment_sequence));
		    },
		    "SegmentSequence");

		auto missing_per_frame_sequence = make_binary_seg_file();
		missing_per_frame_sequence->remove_dataelement(
		    "PerFrameFunctionalGroupsSequence"_tag);
		expect_throw_contains("missing PerFrameFunctionalGroupsSequence",
		    [&] {
			    (void)dicom::seg::from_dicomfile(
			        std::move(missing_per_frame_sequence));
		    },
		    "PerFrameFunctionalGroupsSequence");

		auto missing_shared_geometry = make_binary_seg_file();
		missing_shared_geometry->remove_dataelement(
		    "SharedFunctionalGroupsSequence"_tag);
		expect_throw_contains("missing SharedFunctionalGroupsSequence",
		    [&] {
			    (void)dicom::seg::from_dicomfile(
			        std::move(missing_shared_geometry));
		    },
		    "SharedFunctionalGroupsSequence");
	}

	{
		auto file = std::make_unique<dicom::DicomFile>();
		set_text(*file, "SOPClassUID", "1.2.840.10008.5.1.4.1.1.2");
		if (dicom::seg::is_segmentation_storage(*file)) {
			fail("CT Image Storage should not be recognized as SEG");
		}
		bool threw = false;
		try {
			(void)dicom::seg::from_dicomfile(std::move(file));
		} catch (const std::exception&) {
			threw = true;
		}
		if (!threw) {
			fail("from_dicomfile should reject non-SEG input");
		}
	}

	return 0;
}
