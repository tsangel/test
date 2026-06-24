#include <dicom.h>
#include <dicom_seg.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

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
	set_text(*file, "SegmentsOverlap", "NO");
	populate_segment(*file, 0, 1, "One");
	populate_segment(*file, 1, 2, "Two");
	populate_segment(*file, 2, 7, "Absent");
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
	populate_segment(*file, 0, 1, "One");
	populate_segment(*file, 1, 300, "Three Hundred");
	populate_labelmap_frame(*file, 0, 10.0);
	file->set_native_pixel_data(
	    std::vector<std::uint8_t>{0, 0, 1, 0, 0x2C, 0x01, 0x2C, 0x01},
	    dicom::VR::OW);
	return file;
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
	}

	{
		auto compressed_file = make_binary_seg_file();
		compressed_file->set_transfer_syntax_state_only("RLELossless"_uid);
		auto seg = dicom::seg::from_dicomfile(std::move(compressed_file));
		std::vector<std::uint8_t> decoded(16);
		expect_throw_contains("compressed binary SEG decode",
		    [&] { seg->decode_frame_into(0, decoded); },
		    "compressed/encapsulated BINARY SEG");

		auto deflated_frame_file = make_binary_seg_file();
		deflated_frame_file->set_transfer_syntax_state_only(
		    "DeflatedImageFrameCompression"_uid);
		auto deflated_frame_seg =
		    dicom::seg::from_dicomfile(std::move(deflated_frame_file));
		expect_throw_contains("Deflated Image Frame binary SEG decode",
		    [&] { deflated_frame_seg->decode_frame_into(0, decoded); },
		    "compressed/encapsulated BINARY SEG");
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
		    seg->segment_frame_count(7) != 0 ||
		    seg->frames_for_segment(99).size() != 0) {
			fail("labelmap frames_for_segment mismatch");
		}
		seg->validate_label_values();
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
