#include <dicom.h>
#include <dicom_seg.h>

#include <array>
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
		auto roundtrip_file = dicom::read_file(path);
		std::filesystem::remove(path, cleanup_error);
		if (!roundtrip_file || roundtrip_file->has_error()) {
			fail("roundtrip SEG file should be readable");
		}

		auto seg = dicom::seg::from_dicomfile(std::move(roundtrip_file));
		if (seg->segmentation_type() != dicom::seg::SegmentationType::binary ||
		    seg->segments().size() != 2 || seg->frames().size() != 2 ||
		    seg->frames()[1].referenced_segment_number() != 2) {
			fail("roundtrip SEG file metadata mismatch");
		}
		std::vector<std::uint8_t> decoded(16);
		seg->decode_frame_into(1, decoded);
		const std::vector<std::uint8_t> expected{
		    0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 0, 0, 1, 1, 0, 0};
		if (decoded != expected) {
			fail("roundtrip SEG file binary frame unpack mismatch");
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
