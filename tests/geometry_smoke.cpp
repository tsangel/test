#include <dicom.h>
#include <dicom_geometry.h>
#include <dicom_seg.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <string_view>

namespace {
std::atomic<bool> g_count_allocations{false};
std::atomic<std::size_t> g_allocation_count{0};
} // namespace

void* operator new(std::size_t size) {
	if (g_count_allocations.load(std::memory_order_relaxed)) {
		g_allocation_count.fetch_add(1, std::memory_order_relaxed);
	}
	if (void* ptr = std::malloc(size == 0 ? 1 : size)) {
		return ptr;
	}
	throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
	return ::operator new(size);
}

void operator delete(void* ptr) noexcept {
	std::free(ptr);
}

void operator delete[](void* ptr) noexcept {
	::operator delete(ptr);
}

void operator delete(void* ptr, std::size_t) noexcept {
	::operator delete(ptr);
}

void operator delete[](void* ptr, std::size_t) noexcept {
	::operator delete[](ptr);
}

namespace {
using namespace dicom::literals;

[[noreturn]] void fail(const std::string& message) {
	std::cerr << message << std::endl;
	std::exit(1);
}

void expect_near(double actual, double expected, std::string_view label,
    double tolerance = 1e-9) {
	if (std::abs(actual - expected) > tolerance) {
		fail(std::string(label) + " mismatch");
	}
}

template <typename Fn>
void expect_no_allocations(Fn&& fn, std::string_view label) {
	g_allocation_count.store(0, std::memory_order_relaxed);
	g_count_allocations.store(true, std::memory_order_relaxed);
	fn();
	g_count_allocations.store(false, std::memory_order_relaxed);
	if (g_allocation_count.load(std::memory_order_relaxed) != 0) {
		fail(std::string(label) + " allocated memory");
	}
}

template <std::size_t N>
void set_doubles(dicom::DicomFile& file, dicom::Tag tag,
    const std::array<double, N>& values) {
	if (!file.set_value(tag, std::span<const double>(values.data(), values.size()))) {
		fail("failed to set " + tag.to_string());
	}
}

template <std::size_t N>
void set_doubles(dicom::DicomFile& file, std::string_view key,
    const std::array<double, N>& values) {
	if (!file.set_value(key, std::span<const double>(values.data(), values.size()))) {
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

void set_int(dicom::DicomFile& file, dicom::Tag tag, int value) {
	if (!file.set_value(tag, value)) {
		fail("failed to set " + tag.to_string());
	}
}

void set_int(dicom::DicomFile& file, std::string_view key, int value) {
	if (!file.set_value(key, value)) {
		fail("failed to set " + std::string(key));
	}
}

void set_tag(dicom::DicomFile& file, std::string_view key, dicom::Tag value) {
	if (!file.set_value(key, value)) {
		fail("failed to set " + std::string(key));
	}
}

template <std::size_t N>
void set_tags(dicom::DicomFile& file, dicom::Tag tag,
    const std::array<dicom::Tag, N>& values) {
	auto& element = file.add_dataelement(tag, dicom::VR::AT);
	if (!element.from_tag_vector(
	        std::span<const dicom::Tag>(values.data(), values.size()))) {
		fail("failed to set " + tag.to_string());
	}
}

void set_text(dicom::DicomFile& file, dicom::Tag tag, std::string_view value) {
	if (!file.set_value(tag, value)) {
		fail("failed to set " + tag.to_string());
	}
}

void set_text(dicom::DicomFile& file, std::string_view key, std::string_view value) {
	if (!file.set_value(key, value)) {
		fail("failed to set " + std::string(key));
	}
}

dicom::geometry::ImagePlaneGeometry make_test_plane(
    dicom::geometry::ImageSpacing2D spacing = {2.5, 1.5},
    dicom::geometry::ImageSize2D size = {256, 128},
    dicom::geometry::Point3d origin = {10.0, 20.0, 30.0},
    dicom::geometry::Vec3d direction_i = {1.0, 0.0, 0.0},
    dicom::geometry::Vec3d direction_j = {0.0, 1.0, 0.0}) {
	dicom::geometry::ImagePlaneGeometryParams params;
	params.origin = origin;
	params.direction_i = direction_i;
	params.direction_j = direction_j;
	params.spacing = spacing;
	params.size = size;
	auto result = dicom::geometry::make_image_plane_geometry(params);
	if (!result.ok()) {
		fail("make_image_plane_geometry should succeed");
	}
	return std::move(result).value();
}

dicom::geometry::ImageVolumeGeometry make_test_volume(
    dicom::geometry::ImageSpacing3D spacing = {2.5, 1.5, 4.0},
    dicom::geometry::ImageSize3D size = {256, 128, 32},
    dicom::geometry::Point3d origin = {10.0, 20.0, 30.0}) {
	dicom::geometry::ImageVolumeGeometryParams params;
	params.origin = origin;
	params.direction_i = {1.0, 0.0, 0.0};
	params.direction_j = {0.0, 1.0, 0.0};
	params.direction_k = {0.0, 0.0, 1.0};
	params.spacing = spacing;
	params.size = size;
	auto result = dicom::geometry::make_image_volume_geometry(params);
	if (!result.ok()) {
		fail("make_image_volume_geometry should succeed");
	}
	return std::move(result).value();
}

std::unique_ptr<dicom::DicomFile> make_single_frame_file() {
	auto file = std::make_unique<dicom::DicomFile>();
	set_int(*file, "Rows"_tag, 128);
	set_int(*file, "Columns"_tag, 256);
	const std::array<double, 3> position{10.0, 20.0, 30.0};
	const std::array<double, 6> orientation{1.0, 0.0, 0.0, 0.0, 1.0, 0.0};
	const std::array<double, 2> spacing{1.5, 2.5};
	set_doubles(*file, "ImagePositionPatient"_tag, position);
	set_doubles(*file, "ImageOrientationPatient"_tag, orientation);
	set_doubles(*file, "PixelSpacing"_tag, spacing);
	set_text(*file, "FrameOfReferenceUID"_tag, "1.2.826.0.1.3680043.10.543.1");
	return file;
}

std::unique_ptr<dicom::DicomFile> make_single_frame_file_at_z(double z) {
	auto file = make_single_frame_file();
	const std::array<double, 3> position{10.0, 20.0, z};
	set_doubles(*file, "ImagePositionPatient"_tag, position);
	return file;
}

std::unique_ptr<dicom::DicomFile> make_seg_geometry_file() {
	auto file = std::make_unique<dicom::DicomFile>();
	set_text(*file, "SOPClassUID"_tag, "SegmentationStorage"_uid.value());
	set_text(*file, "MediaStorageSOPClassUID"_tag,
	    "SegmentationStorage"_uid.value());
	set_text(*file, "FrameOfReferenceUID"_tag,
	    "1.2.826.0.1.3680043.10.543.42");
	set_text(*file, "SegmentationType"_tag, "BINARY");
	set_int(*file, "Rows"_tag, 4);
	set_int(*file, "Columns"_tag, 5);
	set_int(*file, "NumberOfFrames"_tag, 2);
	set_int(*file, "SegmentSequence.0.SegmentNumber", 1);
	set_text(*file, "SegmentSequence.0.SegmentLabel", "First");
	const std::array<double, 2> spacing{1.5, 2.5};
	const std::array<double, 6> orientation{
	    1.0, 0.0, 0.0, 0.0, 1.0, 0.0};
	set_doubles(*file,
	    "SharedFunctionalGroupsSequence.0.PixelMeasuresSequence.0.PixelSpacing",
	    spacing);
	set_doubles(*file,
	    "SharedFunctionalGroupsSequence.0.PlaneOrientationSequence.0."
	    "ImageOrientationPatient",
	    orientation);
	for (int frame_index = 0; frame_index < 2; ++frame_index) {
		const auto base = std::string("PerFrameFunctionalGroupsSequence.") +
		    std::to_string(frame_index) + ".";
		set_int(*file,
		    base + "SegmentIdentificationSequence.0.ReferencedSegmentNumber",
		    1);
		const std::array<double, 3> position{
		    0.0, 0.0, frame_index == 0 ? 10.0 : 20.0};
		set_doubles(*file,
		    base + "PlanePositionSequence.0.ImagePositionPatient",
		    position);
	}
	return file;
}

std::unique_ptr<dicom::DicomFile> make_labelmap_seg_geometry_file() {
	auto file = make_seg_geometry_file();
	set_text(*file, "SOPClassUID"_tag,
	    "LabelMapSegmentationStorage"_uid.value());
	set_text(*file, "MediaStorageSOPClassUID"_tag,
	    "LabelMapSegmentationStorage"_uid.value());
	set_text(*file, "SegmentationType"_tag, "LABELMAP");
	set_int(*file, "SamplesPerPixel"_tag, 1);
	set_int(*file, "BitsAllocated"_tag, 8);
	set_int(*file, "BitsStored"_tag, 8);
	set_int(*file, "HighBit"_tag, 7);
	set_int(*file, "PixelRepresentation"_tag, 0);
	set_text(*file, "PhotometricInterpretation"_tag, "MONOCHROME2");
	set_text(*file, "SegmentsOverlap"_tag, "NO");
	return file;
}

std::unique_ptr<dicom::DicomFile> make_enhanced_ct_stack_file() {
	auto file = std::make_unique<dicom::DicomFile>();
	set_int(*file, "Rows"_tag, 4);
	set_int(*file, "Columns"_tag, 5);
	set_int(*file, "NumberOfFrames"_tag, 3);
	set_text(*file, "SOPClassUID"_tag, "EnhancedCTImageStorage"_uid.value());
	set_text(*file, "FrameOfReferenceUID"_tag, "1.2.826.0.1.3680043.10.543.77");
	const std::array<double, 2> spacing{2.0, 3.0};
	const std::array<double, 6> orientation{
	    1.0, 0.0, 0.0, 0.0, 1.0, 0.0};
	set_doubles(*file,
	    "SharedFunctionalGroupsSequence.0.PixelMeasuresSequence.0.PixelSpacing",
	    spacing);
	set_doubles(*file,
	    "SharedFunctionalGroupsSequence.0.PlaneOrientationSequence.0."
	    "ImageOrientationPatient",
	    orientation);
	set_text(*file,
	    "SharedFunctionalGroupsSequence.0.CTImageFrameTypeSequence.0."
	    "VolumetricProperties",
	    "VOLUME");
	set_tag(*file,
	    "DimensionIndexSequence.0.DimensionIndexPointer",
	    "InStackPositionNumber"_tag);
	set_tag(*file,
	    "DimensionIndexSequence.0.FunctionalGroupPointer",
	    "FrameContentSequence"_tag);
	set_text(*file,
	    "DimensionIndexSequence.0.DimensionDescriptionLabel",
	    "Stack position");
	for (int frame_index = 0; frame_index < 3; ++frame_index) {
		const auto base = std::string("PerFrameFunctionalGroupsSequence.") +
		    std::to_string(frame_index) + ".";
		const std::array<double, 3> position{
		    0.0, 0.0, frame_index == 0 ? 10.0 :
		                  frame_index == 1 ? 20.0 :
		                                     30.0};
		set_doubles(*file,
		    base + "PlanePositionSequence.0.ImagePositionPatient",
		    position);
		set_text(*file, base + "FrameContentSequence.0.StackID", "STACK_A");
		set_int(*file,
		    base + "FrameContentSequence.0.InStackPositionNumber",
		    frame_index + 1);
		const std::array<long, 1> dimension_values{{
		    static_cast<long>(frame_index + 1),
		}};
		set_longs(*file,
		    base + "FrameContentSequence.0.DimensionIndexValues",
		    dimension_values);
	}
	return file;
}

std::unique_ptr<dicom::DicomFile> make_nm_recon_tomo_stack_file() {
	auto file = std::make_unique<dicom::DicomFile>();
	set_int(*file, "Rows"_tag, 4);
	set_int(*file, "Columns"_tag, 5);
	set_int(*file, "NumberOfFrames"_tag, 3);
	set_text(*file, "SOPClassUID"_tag,
	    "NuclearMedicineImageStorage"_uid.value());
	set_text(*file, "FrameOfReferenceUID"_tag,
	    "1.2.826.0.1.3680043.10.543.88");
	set_text(*file, "ImageType"_tag, "ORIGINAL\\PRIMARY\\RECON TOMO");
	const std::array<double, 3> position{0.0, 0.0, 100.0};
	const std::array<double, 6> orientation{
	    1.0, 0.0, 0.0, 0.0, 1.0, 0.0};
	const std::array<double, 2> spacing{2.0, 3.0};
	const std::array<double, 1> slice_spacing{5.0};
	const std::array<dicom::Tag, 1> frame_increment_pointers{{
	    "SliceVector"_tag,
	}};
	const std::array<long, 3> slice_vector{{3, 1, 2}};
	set_doubles(*file, "ImagePositionPatient"_tag, position);
	set_doubles(*file, "ImageOrientationPatient"_tag, orientation);
	set_doubles(*file, "PixelSpacing"_tag, spacing);
	set_doubles(*file, "SpacingBetweenSlices"_tag, slice_spacing);
	set_tags(*file, "FrameIncrementPointer"_tag, frame_increment_pointers);
	set_longs(*file, "SliceVector", slice_vector);
	return file;
}

} // namespace

int main() {
	{
		auto plane = make_test_plane();
		const auto world = plane.world_from_index({2.0, 3.0});
		expect_near(world.x, 15.0, "world.x");
		expect_near(world.y, 24.5, "world.y");
		expect_near(world.z, 30.0, "world.z");

		const auto index = plane.index_from_world(world);
		expect_near(index.i, 2.0, "index.i");
		expect_near(index.j, 3.0, "index.j");
		expect_near(plane.index_to_world_matrix()(0, 0), 2.5,
		    "index_to_world matrix column i");
		expect_near(plane.index_to_world_matrix()(1, 1), 1.5,
		    "index_to_world matrix column j");
		expect_near(plane.index_to_world_matrix()(0, 3), 10.0,
		    "index_to_world matrix origin x");
		expect_near(plane.world_to_index_matrix()(2, 2), 1.0,
		    "world_to_index matrix normal z");

		const auto elevated = dicom::geometry::Point3d{15.0, 24.5, 37.0};
		expect_near(plane.normal_distance_from_world(elevated), 7.0,
		    "normal distance");
		if (!plane.contains_index({0.0, 0.0}) ||
		    !plane.contains_world({10.0, 20.0, 30.0}) ||
		    plane.contains_world(elevated, 1e-3)) {
			fail("contains helpers should respect bounds and normal distance");
		}
	}

	{
		constexpr double s = 0.7071067811865476;
		auto plane = make_test_plane({2.0, 3.0}, {16, 12}, {1.0, 2.0, 3.0},
		    {s, s, 0.0}, {0.0, 0.0, 1.0});
		const auto world = plane.world_from_index({2.0, 3.0});
		expect_near(world.x, 1.0 + s * 4.0, "oblique world.x");
		expect_near(world.y, 2.0 + s * 4.0, "oblique world.y");
		expect_near(world.z, 12.0, "oblique world.z");
		const auto index = plane.index_from_world(world);
		expect_near(index.i, 2.0, "oblique index.i");
		expect_near(index.j, 3.0, "oblique index.j");
		expect_near(plane.normal().x, s, "oblique normal.x");
		expect_near(plane.normal().y, -s, "oblique normal.y");
		expect_near(plane.normal().z, 0.0, "oblique normal.z");
	}

	{
		dicom::geometry::ImagePlaneGeometryParams params;
		params.origin = {0.0, 0.0, 0.0};
		params.direction_i = {1.0, 0.0, 0.0};
		params.direction_j = {1.0, 0.0, 0.0};
		params.spacing = {1.0, 1.0};
		params.size = {2, 2};
		const auto result = dicom::geometry::make_image_plane_geometry(params);
		if (result.ok() ||
		    result.status() != dicom::geometry::GeometryBuildStatus::invalid_orientation) {
			fail("non-orthogonal directions should fail");
		}
		params.direction_j = {0.0, 1.0, 0.0};
		params.spacing = {0.0, 1.0};
		const auto bad_spacing = dicom::geometry::make_image_plane_geometry(params);
		if (bad_spacing.ok() ||
		    bad_spacing.status() !=
		        dicom::geometry::GeometryBuildStatus::invalid_spacing ||
		    bad_spacing.maybe_value().has_value()) {
			fail("invalid spacing should fail without a value");
		}
		params.spacing = {1.0, 1.0};
		params.size = {0, 2};
		const auto bad_size = dicom::geometry::make_image_plane_geometry(params);
		if (bad_size.ok() ||
		    bad_size.status() != dicom::geometry::GeometryBuildStatus::invalid_size ||
		    bad_size.maybe_value().has_value()) {
			fail("invalid size should fail without a value");
		}
	}

	{
		auto volume = make_test_volume();
		const auto world = volume.world_from_index({2.0, 3.0, 4.0});
		expect_near(world.x, 15.0, "volume world.x");
		expect_near(world.y, 24.5, "volume world.y");
		expect_near(world.z, 46.0, "volume world.z");

		const auto index = volume.index_from_world(world);
		expect_near(index.i, 2.0, "volume index.i");
		expect_near(index.j, 3.0, "volume index.j");
		expect_near(index.k, 4.0, "volume index.k");
		expect_near(volume.index_to_world_matrix()(0, 0), 2.5,
		    "volume index_to_world matrix column i");
		expect_near(volume.index_to_world_matrix()(1, 1), 1.5,
		    "volume index_to_world matrix column j");
		expect_near(volume.index_to_world_matrix()(2, 2), 4.0,
		    "volume index_to_world matrix column k");
		expect_near(volume.index_to_world_matrix()(0, 3), 10.0,
		    "volume index_to_world matrix origin x");

		if (!volume.contains_index({0.0, 0.0, 0.0}) ||
		    !volume.contains_world({10.0, 20.0, 30.0}) ||
		    volume.contains_index({255.6, 0.0, 0.0})) {
			fail("volume contains helpers should use sample-centered bounds");
		}

		dicom::geometry::GeometryTolerance tolerance;
		tolerance.position_tolerance_mm = 0.01;
		if (!volume.contains_world({8.75 - 0.001, 20.0, 30.0}, tolerance) ||
		    volume.contains_world({8.0, 20.0, 30.0}, tolerance)) {
			fail("volume contains_world should apply boundary tolerance");
		}
	}

	{
		dicom::geometry::ImageVolumeGeometryParams params;
		params.origin = {0.0, 0.0, 0.0};
		params.direction_i = {1.0, 0.0, 0.0};
		params.direction_j = {0.0, 1.0, 0.0};
		params.direction_k = {0.0, 0.0, 1.0};
		params.spacing = {1.0, 1.0, 1.0};
		params.size = {2, 2, 2};

		params.spacing.k = 0.0;
		auto bad_spacing = dicom::geometry::make_image_volume_geometry(params);
		if (bad_spacing.ok() ||
		    bad_spacing.status() !=
		        dicom::geometry::GeometryBuildStatus::invalid_spacing ||
		    bad_spacing.maybe_value().has_value()) {
			fail("invalid volume spacing should fail without a value");
		}

		params.spacing.k = 1.0;
		params.size.k = 0;
		auto bad_size = dicom::geometry::make_image_volume_geometry(params);
		if (bad_size.ok() ||
		    bad_size.status() != dicom::geometry::GeometryBuildStatus::invalid_size ||
		    bad_size.maybe_value().has_value()) {
			fail("invalid volume size should fail without a value");
		}

		params.size.k = 2;
		params.direction_k = {0.0, 1.0, 1.0};
		auto bad_orientation = dicom::geometry::make_image_volume_geometry(params);
		if (bad_orientation.ok() ||
		    bad_orientation.status() !=
		        dicom::geometry::GeometryBuildStatus::invalid_orientation) {
			fail("non-orthogonal volume direction should fail");
		}

		params.direction_k = {0.0, 0.0, -1.0};
		auto left_handed = dicom::geometry::make_image_volume_geometry(params);
		if (left_handed.ok() ||
		    left_handed.status() !=
		        dicom::geometry::GeometryBuildStatus::invalid_orientation) {
			fail("left-handed volume basis should fail");
		}
	}

	{
		auto file = make_single_frame_file();
		auto result = dicom::geometry::plane_from_single_frame_image(*file);
		if (!result.ok()) {
			fail("single-frame plane should parse");
		}
		const auto& plane = result.value();
		if (plane.columns() != 256 || plane.rows() != 128) {
			fail("Rows/Columns should map to j/i size");
		}
		expect_near(plane.spacing_i(), 2.5, "spacing_i");
		expect_near(plane.spacing_j(), 1.5, "spacing_j");
		expect_near(plane.direction_i().x, 1.0, "direction_i.x");
		expect_near(plane.direction_j().y, 1.0, "direction_j.y");

		auto frame_of_reference = dicom::geometry::frame_of_reference_from_dataset(*file);
		if (!frame_of_reference.ok() ||
		    frame_of_reference.value() != "1.2.826.0.1.3680043.10.543.1") {
			fail("FrameOfReferenceUID should parse");
		}
	}

	{
		dicom::DicomFile file;
		set_int(file, "Rows"_tag, 2);
		set_int(file, "Columns"_tag, 2);
		const auto result = dicom::geometry::plane_from_single_frame_image(file);
		if (result.ok() ||
		    result.status() !=
		        dicom::geometry::GeometryBuildStatus::missing_required_tag ||
		    result.tag() != "ImagePositionPatient"_tag ||
		    result.source().leaf_tag() != "ImagePositionPatient"_tag ||
		    result.source().depth() != 0) {
			fail("missing geometry tag should be reported without throwing");
		}
	}
	{
		auto file = make_single_frame_file();
		const std::array<double, 2> bad_spacing{1.5, 0.0};
		set_doubles(*file, "PixelSpacing"_tag, bad_spacing);
		const auto result = dicom::geometry::plane_from_single_frame_image(*file);
		if (result.ok() ||
		    result.status() != dicom::geometry::GeometryBuildStatus::invalid_spacing ||
		    result.tag() != "PixelSpacing"_tag ||
		    result.source().leaf_tag() != "PixelSpacing"_tag) {
			fail("invalid DICOM PixelSpacing should keep the source tag");
		}
	}
	{
		dicom::DicomFile file;
		set_int(file, "Rows"_tag, 4);
		set_int(file, "Columns"_tag, 5);
		set_text(file, "SOPClassUID"_tag, "EnhancedCTImageStorage"_uid.value());
		set_text(file, "VolumetricProperties"_tag, "SAMPLED");
		const std::array<double, 2> root_spacing{9.0, 9.0};
		const std::array<double, 2> shared_spacing{2.0, 3.0};
		const std::array<double, 6> shared_orientation{
		    1.0, 0.0, 0.0, 0.0, 1.0, 0.0};
		const std::array<double, 3> frame0_position{0.0, 0.0, 10.0};
		const std::array<double, 3> frame1_position{0.0, 0.0, 20.0};
		set_doubles(file, "PixelSpacing"_tag, root_spacing);
		set_doubles(file,
		    "SharedFunctionalGroupsSequence.0.PixelMeasuresSequence.0.PixelSpacing",
		    shared_spacing);
		set_doubles(file,
		    "SharedFunctionalGroupsSequence.0.PlaneOrientationSequence.0."
		    "ImageOrientationPatient",
		    shared_orientation);
		set_text(file,
		    "SharedFunctionalGroupsSequence.0.CTImageFrameTypeSequence.0."
		    "VolumetricProperties",
		    "VOLUME");
		set_doubles(file,
		    "PerFrameFunctionalGroupsSequence.0.PlanePositionSequence.0."
		    "ImagePositionPatient",
		    frame0_position);
		set_doubles(file,
		    "PerFrameFunctionalGroupsSequence.1.PlanePositionSequence.0."
		    "ImagePositionPatient",
		    frame1_position);

		auto result = dicom::geometry::plane_from_multiframe_image(file, 1);
		if (!result.ok()) {
			fail("multiframe plane should parse from per-frame/shared metadata");
		}
		const auto& plane = result.value();
		expect_near(plane.origin().z, 20.0, "multiframe per-frame position");
		expect_near(plane.spacing_i(), 3.0, "multiframe shared spacing_i");
		expect_near(plane.spacing_j(), 2.0, "multiframe shared spacing_j");
		if (plane.columns() != 5 || plane.rows() != 4) {
			fail("multiframe Rows/Columns should map to j/i size");
		}

		const std::array<double, 2> bad_shared_spacing{2.0, -3.0};
		set_doubles(file,
		    "SharedFunctionalGroupsSequence.0.PixelMeasuresSequence.0.PixelSpacing",
		    bad_shared_spacing);
		auto bad_shared_spacing_result =
		    dicom::geometry::plane_from_multiframe_image(file, 0);
		if (bad_shared_spacing_result.ok() ||
		    bad_shared_spacing_result.status() !=
		        dicom::geometry::GeometryBuildStatus::invalid_spacing ||
		    bad_shared_spacing_result.tag() != "PixelSpacing"_tag ||
		    bad_shared_spacing_result.source().leaf_tag() != "PixelSpacing"_tag ||
		    bad_shared_spacing_result.source().depth() == 0) {
			fail("invalid shared PixelSpacing should keep functional group source path");
		}
		set_doubles(file,
		    "SharedFunctionalGroupsSequence.0.PixelMeasuresSequence.0.PixelSpacing",
		    shared_spacing);

		dicom::geometry::FrameGeometryReader reader(file);
		auto reader_result = reader.plane(0);
		if (!reader_result.ok() || reader_result.value().origin().z != 10.0) {
			fail("FrameGeometryReader should resolve frame zero");
		}
		auto missing = reader.plane(2);
		if (missing.ok() ||
		    missing.status() !=
		        dicom::geometry::GeometryBuildStatus::invalid_frame_index ||
		    missing.tag() != "PerFrameFunctionalGroupsSequence"_tag) {
			fail("out-of-range frame should not fall back to root metadata");
		}
	}
	{
		dicom::DicomFile file;
		set_int(file, "Rows"_tag, 2);
		set_int(file, "Columns"_tag, 3);
		set_text(file, "VolumetricProperties"_tag, "VOLUME");
		const std::array<double, 3> position{1.0, 2.0, 3.0};
		const std::array<double, 6> orientation{1.0, 0.0, 0.0, 0.0, 1.0, 0.0};
		const std::array<double, 2> spacing{4.0, 5.0};
		set_doubles(file, "ImagePositionPatient"_tag, position);
		set_doubles(file, "ImageOrientationPatient"_tag, orientation);
		set_doubles(file, "PixelSpacing"_tag, spacing);

		auto result = dicom::geometry::plane_from_multiframe_image(file, 0);
		if (!result.ok()) {
			fail("legacy root fallback should parse frame zero without PerFrame FG");
		}
		expect_near(result.value().origin().z, 3.0, "legacy root fallback position");
		expect_near(result.value().spacing_i(), 5.0, "legacy root fallback spacing_i");

		result = dicom::geometry::plane_from_multiframe_image(file, 1);
		if (result.ok() ||
		    result.status() !=
		        dicom::geometry::GeometryBuildStatus::invalid_frame_index ||
		    result.tag() != "NumberOfFrames"_tag) {
			fail("root fallback without NumberOfFrames should only allow frame zero");
		}

		set_int(file, "NumberOfFrames"_tag, 2);
		result = dicom::geometry::plane_from_multiframe_image(file, 1);
		if (!result.ok()) {
			fail("root fallback should allow frame index inside NumberOfFrames");
		}
		result = dicom::geometry::plane_from_multiframe_image(file, 2);
		if (result.ok() ||
		    result.status() !=
		        dicom::geometry::GeometryBuildStatus::invalid_frame_index ||
		    result.tag() != "NumberOfFrames"_tag) {
			fail("root fallback should reject frame index outside NumberOfFrames");
		}
	}
	{
		dicom::DicomFile file;
		set_int(file, "Rows"_tag, 2);
		set_int(file, "Columns"_tag, 3);
		const std::array<double, 3> position{1.0, 2.0, 3.0};
		const std::array<double, 6> orientation{1.0, 0.0, 0.0, 0.0, 1.0, 0.0};
		const std::array<double, 2> spacing{4.0, 5.0};
		set_doubles(file, "ImagePositionPatient"_tag, position);
		set_doubles(file, "ImageOrientationPatient"_tag, orientation);
		set_doubles(file, "PixelSpacing"_tag, spacing);
		auto& bad_per_frame =
		    file.add_dataelement("PerFrameFunctionalGroupsSequence"_tag, dicom::VR::LO);
		if (!bad_per_frame.from_string_view("not-a-sequence")) {
			fail("failed to create invalid PerFrameFunctionalGroupsSequence");
		}

		dicom::geometry::FrameGeometryReader reader(file);
		const auto result = reader.plane(0);
		if (result.ok() ||
		    result.status() != dicom::geometry::GeometryBuildStatus::invalid_value ||
		    result.tag() != "PerFrameFunctionalGroupsSequence"_tag) {
			fail("invalid PerFrameFunctionalGroupsSequence should not fall back to root");
		}
	}
	{
		auto file = make_seg_geometry_file();
		auto seg = dicom::seg::from_dicomfile(std::move(file));
		auto frame_of_reference =
		    dicom::geometry::frame_of_reference_from_segmentation(*seg);
		if (!frame_of_reference.ok() ||
		    frame_of_reference.value() != "1.2.826.0.1.3680043.10.543.42") {
			fail("SEG FrameOfReferenceUID should parse");
		}

		auto plane = dicom::geometry::plane_from_seg_frame(*seg, 1);
		if (!plane.ok()) {
			fail("SEG frame plane should parse from PerFrame/Shared FG");
		}
		expect_near(plane.value().origin().z, 20.0, "SEG frame position");
		expect_near(plane.value().spacing_i(), 2.5, "SEG spacing_i");
		expect_near(plane.value().spacing_j(), 1.5, "SEG spacing_j");
		if (plane.value().columns() != 5 || plane.value().rows() != 4) {
			fail("SEG Rows/Columns should map to j/i size");
		}

		const auto out_of_range = dicom::geometry::plane_from_seg_frame(*seg, 9);
		if (out_of_range.ok() ||
		    out_of_range.status() !=
		        dicom::geometry::GeometryBuildStatus::invalid_frame_index) {
			fail("SEG out-of-range frame should return invalid_frame_index");
		}
	}
	{
		auto file = make_labelmap_seg_geometry_file();
		auto seg = dicom::seg::from_dicomfile(std::move(file));
		auto plane = dicom::geometry::plane_from_seg_frame(*seg, 1);
		if (!plane.ok()) {
			fail("LABELMAP SEG frame plane should parse from PerFrame/Shared FG");
		}
		expect_near(plane.value().origin().z, 20.0,
		    "LABELMAP SEG frame position");
		expect_near(plane.value().spacing_i(), 2.5,
		    "LABELMAP SEG spacing_i");
		expect_near(plane.value().spacing_j(), 1.5,
		    "LABELMAP SEG spacing_j");
		if (plane.value().columns() != 5 || plane.value().rows() != 4) {
			fail("LABELMAP SEG Rows/Columns should map to j/i size");
		}
	}
	{
		auto file = make_seg_geometry_file();
		const std::array<double, 6> root_orientation{
		    1.0, 0.0, 0.0, 0.0, 1.0, 0.0};
		set_doubles(*file, "ImageOrientationPatient"_tag, root_orientation);
		file->remove_dataelement("SharedFunctionalGroupsSequence"_tag);
		set_text(*file, "SharedFunctionalGroupsSequence.0.PixelMeasuresSequence.0."
		                "PixelSpacing",
		    "1.5\\2.5");
		auto seg = dicom::seg::from_dicomfile(std::move(file),
		    dicom::seg::Options{.validate_required_modules = false});
		auto plane = dicom::geometry::plane_from_seg_frame(*seg, 0);
		if (plane.ok() ||
		    plane.status() !=
		        dicom::geometry::GeometryBuildStatus::missing_required_tag ||
		    plane.tag() != "ImageOrientationPatient"_tag) {
			fail("SEG geometry should not fall back to root ImageOrientationPatient");
		}
	}
	{
		dicom::DicomFile file;
		set_int(file, "Rows"_tag, 4);
		set_int(file, "Columns"_tag, 5);
		set_text(file, "SOPClassUID"_tag, "EnhancedPETImageStorage"_uid.value());
		set_text(file, "VolumetricProperties"_tag, "DISTORTED");
		const std::array<double, 3> position{0.0, 0.0, 0.0};
		const std::array<double, 6> orientation{1.0, 0.0, 0.0, 0.0, 1.0, 0.0};
		const std::array<double, 2> spacing{1.0, 1.0};
		set_doubles(file, "ImagePositionPatient"_tag, position);
		set_doubles(file, "ImageOrientationPatient"_tag, orientation);
		set_doubles(file, "PixelSpacing"_tag, spacing);
		set_text(file,
		    "PerFrameFunctionalGroupsSequence.0.PETFrameTypeSequence.0."
		    "VolumetricProperties",
		    "VOLUME");

		auto info =
		    dicom::geometry::volumetric_properties_from_multiframe_image(file, 0);
		if (!info.ok() ||
		    info.value().value !=
		        dicom::geometry::VolumetricPropertiesValue::volume ||
		    info.value().source.leaf_tag() != "VolumetricProperties"_tag ||
		    info.value().source.parents().size() != 2 ||
		    info.value().source.parents()[0].sequence_tag !=
		        "PerFrameFunctionalGroupsSequence"_tag ||
		    info.value().source.parents()[1].sequence_tag !=
		        "PETFrameTypeSequence"_tag) {
			fail("PET frame VolumetricProperties should prefer per-frame source");
		}
		auto plane = dicom::geometry::plane_from_multiframe_image(file, 0);
		if (!plane.ok()) {
			fail("per-frame PET VOLUME should be accepted as a regular plane");
		}
	}
	{
		dicom::DicomFile file;
		set_int(file, "Rows"_tag, 2);
		set_int(file, "Columns"_tag, 2);
		set_text(file, "SOPClassUID"_tag, "EnhancedMRImageStorage"_uid.value());
		set_text(file, "VolumetricProperties"_tag, "VOLUME");
		const std::array<double, 3> position{0.0, 0.0, 0.0};
		const std::array<double, 6> orientation{1.0, 0.0, 0.0, 0.0, 1.0, 0.0};
		const std::array<double, 2> spacing{1.0, 1.0};
		set_doubles(file, "ImagePositionPatient"_tag, position);
		set_doubles(file, "ImageOrientationPatient"_tag, orientation);
		set_doubles(file, "PixelSpacing"_tag, spacing);
		set_text(file,
		    "SharedFunctionalGroupsSequence.0.MRImageFrameTypeSequence.0."
		    "VolumetricProperties",
		    "SAMPLED");

		auto frame_geometry =
		    dicom::geometry::frame_geometry_from_multiframe_image(file, 0);
		if (!frame_geometry.ok() ||
		    frame_geometry.value().kind !=
		        dicom::geometry::ImageFrameGeometryKind::sampled_projection) {
			fail("SAMPLED frame should be preserved by frame_geometry API");
		}
		auto plane = dicom::geometry::plane_from_multiframe_image(file, 0);
		if (plane.ok() ||
		    plane.status() !=
		        dicom::geometry::GeometryBuildStatus::sampled_frame_geometry) {
			fail("SAMPLED frame should be rejected by overlay plane API");
		}

		set_text(file,
		    "SharedFunctionalGroupsSequence.0.MRImageFrameTypeSequence.0."
		    "VolumetricProperties",
		    "DISTORTED");
		frame_geometry =
		    dicom::geometry::frame_geometry_from_multiframe_image(file, 0);
		if (!frame_geometry.ok() ||
		    frame_geometry.value().kind !=
		        dicom::geometry::ImageFrameGeometryKind::distorted) {
			fail("DISTORTED frame should be preserved by frame_geometry API");
		}
		plane = dicom::geometry::plane_from_multiframe_image(file, 0);
		if (plane.ok() ||
		    plane.status() !=
		        dicom::geometry::GeometryBuildStatus::distorted_frame_geometry) {
			fail("DISTORTED frame should be rejected by overlay plane API");
		}
	}
	{
		dicom::DicomFile file;
		set_text(file, "SOPClassUID"_tag, "EnhancedCTImageStorage"_uid.value());
		set_text(file, "VolumetricProperties"_tag, "MIXED");
		auto mixed =
		    dicom::geometry::volumetric_properties_from_multiframe_image(file, 0);
		if (mixed.ok() ||
		    mixed.status() !=
		        dicom::geometry::GeometryBuildStatus::mixed_volumetric_properties) {
			fail("root MIXED without frame-level value should fail");
		}
		set_text(file, "VolumetricProperties"_tag, "WOBBLY");
		auto unknown =
		    dicom::geometry::volumetric_properties_from_multiframe_image(file, 0);
		if (unknown.ok() ||
		    unknown.status() !=
		        dicom::geometry::GeometryBuildStatus::unknown_volumetric_properties) {
			fail("unknown VolumetricProperties should fail");
		}
		file.remove_dataelement("VolumetricProperties"_tag);
		auto missing =
		    dicom::geometry::volumetric_properties_from_multiframe_image(file, 0);
		if (missing.ok() ||
		    missing.status() !=
		        dicom::geometry::GeometryBuildStatus::missing_volumetric_properties) {
			fail("missing VolumetricProperties should fail");
		}
	}
	{
		dicom::DicomFile file;
		set_text(file, "SOPClassUID"_tag, "NuclearMedicineImageStorage"_uid.value());
		set_text(file, "VolumetricProperties"_tag, "VOLUME");
		auto result =
		    dicom::geometry::volumetric_properties_from_multiframe_image(file, 0);
		if (result.ok() ||
		    result.status() !=
		        dicom::geometry::GeometryBuildStatus::unsupported_frame_geometry) {
			fail("NM Image Storage should not use enhanced volumetric properties");
		}
	}

	{
		auto file = make_nm_recon_tomo_stack_file();
		auto analysis = dicom::geometry::analyze_nm_frame_stack(*file);
		if (!analysis.ok() || analysis.slices().size() != 3 ||
		    !analysis.uniform_spacing_k() ||
		    analysis.frame_of_reference_uid() !=
		        "1.2.826.0.1.3680043.10.543.88") {
			fail("NM reconstructed TOMO stack should analyze successfully");
		}
		expect_near(*analysis.uniform_spacing_k(), 5.0,
		    "NM reconstructed spacing");
		if (analysis.slices()[0].frame_index != 1 ||
		    analysis.slices()[1].frame_index != 2 ||
		    analysis.slices()[2].frame_index != 0) {
			fail("NM SliceVector should sort frames by reconstructed slice position");
		}
		expect_near(analysis.slices()[0].plane.origin().z, 90.0,
		    "NM first sorted origin");
		expect_near(analysis.slices()[2].plane.origin().z, 100.0,
		    "NM last sorted origin");

		auto plan = dicom::geometry::plan_nm_frame_stack(*file);
		if (!plan.ok() || !plan.volume_geometry() ||
		    plan.placements().size() != 3 ||
		    plan.placements()[0].frame_index != 1 ||
		    plan.placements()[0].target_k != 0 ||
		    plan.placements()[2].frame_index != 0 ||
		    plan.placements()[2].target_k != 2) {
			fail("NM reconstructed TOMO plan should map frames to sorted target_k");
		}
		expect_near(plan.volume_geometry()->origin().z, 90.0,
		    "NM volume origin");
		expect_near(plan.volume_geometry()->spacing_k(), 5.0,
		    "NM volume spacing_k");
	}

	{
		auto file = make_nm_recon_tomo_stack_file();
		file->remove_dataelement("NumberOfFrames"_tag);
		set_text(*file,
		    "PerFrameFunctionalGroupsSequence.0.FrameContentSequence.0.StackID",
		    "NM_FALLBACK_SHOULD_NOT_BE_USED");
		auto analysis = dicom::geometry::analyze_nm_frame_stack(*file);
		if (analysis.ok() ||
		    analysis.status() !=
		        dicom::geometry::SliceStackStatus::missing_frame_content ||
		    analysis.issues().empty() ||
		    analysis.issues().front().tag != "NumberOfFrames"_tag) {
			fail("NM adapter should require root NumberOfFrames");
		}
	}

	{
		auto file = make_nm_recon_tomo_stack_file();
		set_text(*file, "ImageType"_tag, "ORIGINAL\\PRIMARY\\TOMO");
		auto analysis = dicom::geometry::analyze_nm_frame_stack(*file);
		if (analysis.ok() ||
		    analysis.status() !=
		        dicom::geometry::SliceStackStatus::geometry_parse_failure ||
		    analysis.issues().empty() ||
		    analysis.issues().front().tag != "ImageType"_tag) {
			fail("NM projection TOMO should not be treated as a reconstructed stack");
		}
	}

	{
		auto file = make_nm_recon_tomo_stack_file();
		const std::array<dicom::Tag, 2> frame_increment_pointers{{
		    "SliceVector"_tag,
		    "TimeSlotVector"_tag,
		}};
		set_tags(*file, "FrameIncrementPointer"_tag, frame_increment_pointers);
		auto analysis = dicom::geometry::analyze_nm_frame_stack(*file);
		if (analysis.ok() ||
		    analysis.status() !=
		        dicom::geometry::SliceStackStatus::geometry_parse_failure ||
		    analysis.issues().empty() ||
		    analysis.issues().front().tag != "FrameIncrementPointer"_tag) {
			fail("NM adapter should reject multi-vector frame organization");
		}
	}

	{
		auto file = make_nm_recon_tomo_stack_file();
		file->remove_dataelement("FrameIncrementPointer"_tag);
		auto analysis = dicom::geometry::analyze_nm_frame_stack(*file);
		if (analysis.ok() ||
		    analysis.status() !=
		        dicom::geometry::SliceStackStatus::missing_frame_content ||
		    analysis.issues().empty() ||
		    analysis.issues().front().tag != "FrameIncrementPointer"_tag) {
			fail("NM adapter should require FrameIncrementPointer -> SliceVector");
		}
	}

	{
		const auto z20 = make_test_plane({2.5, 1.5}, {256, 128}, {0.0, 0.0, 20.0});
		const auto z10 = make_test_plane({2.5, 1.5}, {256, 128}, {0.0, 0.0, 10.0});
		const auto z30 = make_test_plane({2.5, 1.5}, {256, 128}, {0.0, 0.0, 30.0});
		const std::array<dicom::geometry::SliceStackInput, 3> inputs{{
		    {0, 0, z20, "1.2.3"},
		    {1, 0, z10, "1.2.3"},
		    {2, 0, z30, "1.2.3"},
		}};
		auto analysis = dicom::geometry::analyze_slice_stack(
		    std::span<const dicom::geometry::SliceStackInput>(
		        inputs.data(), inputs.size()));
		if (!analysis.ok() || analysis.slices().size() != 3 ||
		    analysis.gaps().size() != 2 || !analysis.uniform_spacing_k()) {
			fail("uniform slice stack should analyze successfully");
		}
		if (analysis.slices()[0].source_index != 1 ||
		    analysis.slices()[1].source_index != 0 ||
		    analysis.slices()[2].source_index != 2) {
			fail("slice stack should sort by position along normal");
		}
		expect_near(*analysis.uniform_spacing_k(), 10.0,
		    "slice stack uniform spacing");
		expect_near(analysis.gaps()[0].spacing_mm, 10.0,
		    "slice stack first gap");
		if (analysis.uniform_runs().size() != 1 ||
		    analysis.uniform_runs()[0].begin_sorted_index != 0 ||
		    analysis.uniform_runs()[0].end_sorted_index != 3) {
			fail("uniform slice stack should expose one full uniform run");
		}
		expect_near(analysis.uniform_runs()[0].spacing_mm, 10.0,
		    "slice stack uniform run spacing");
		if (analysis.frame_of_reference_uid() != "1.2.3") {
			fail("slice stack analysis should own FrameOfReferenceUID");
		}

		auto plan = dicom::geometry::plan_slice_stack(
		    std::span<const dicom::geometry::SliceStackInput>(
		        inputs.data(), inputs.size()));
		if (!plan.ok() || !plan.volume_geometry() ||
		    plan.placements().size() != 3) {
			fail("uniform slice stack should produce a volume plan");
		}
		const auto& volume = *plan.volume_geometry();
		if (volume.slices() != 3 || volume.rows() != 128 ||
		    volume.columns() != 256) {
			fail("slice stack volume geometry should use sorted slice count and size");
		}
		expect_near(volume.origin().z, 10.0, "slice stack volume origin");
		expect_near(volume.spacing_k(), 10.0, "slice stack volume spacing_k");
		if (plan.placements()[0].source_index != 1 ||
		    plan.placements()[0].target_k != 0 ||
		    plan.placements()[1].source_index != 0 ||
		    plan.placements()[1].target_k != 1 ||
		    plan.placements()[2].source_index != 2 ||
		    plan.placements()[2].target_k != 2) {
			fail("slice stack plan should map source slices to sorted target_k");
		}
	}

	{
		const auto z10 = make_test_plane({2.5, 1.5}, {256, 128}, {0.0, 0.0, 10.0});
		const auto z20 = make_test_plane({2.5, 1.5}, {256, 128}, {0.0, 0.0, 20.0});
		const std::array<dicom::geometry::SliceStackInput, 2> inputs{{
		    {0, 0, z10, "1.2.3"},
		    {1, 0, z20, "1.2.4"},
		}};
		auto analysis = dicom::geometry::analyze_slice_stack(
		    std::span<const dicom::geometry::SliceStackInput>(
		        inputs.data(), inputs.size()));
		if (analysis.ok() ||
		    analysis.status() !=
		        dicom::geometry::SliceStackStatus::mixed_frame_of_reference ||
		    analysis.issues().empty()) {
			fail("mixed FrameOfReferenceUID should be reported");
		}
	}

	{
		const auto z10 = make_test_plane({2.5, 1.5}, {256, 128}, {0.0, 0.0, 10.0});
		const auto z10_duplicate =
		    make_test_plane({2.5, 1.5}, {256, 128}, {0.0, 0.0, 10.0});
		const auto z30 = make_test_plane({2.5, 1.5}, {256, 128}, {0.0, 0.0, 30.0});
		const std::array<dicom::geometry::SliceStackInput, 3> inputs{{
		    {0, 0, z10, "1.2.3"},
		    {1, 0, z10_duplicate, "1.2.3"},
		    {2, 0, z30, "1.2.3"},
		}};
		auto analysis = dicom::geometry::analyze_slice_stack(
		    std::span<const dicom::geometry::SliceStackInput>(
		        inputs.data(), inputs.size()));
		if (analysis.ok() ||
		    analysis.status() !=
		        dicom::geometry::SliceStackStatus::duplicate_slice_position) {
			fail("duplicate slice position should be reported before non-uniform gap");
		}

		dicom::geometry::SliceStackOptions allow_duplicate_options;
		allow_duplicate_options.allow_duplicate_positions = true;
		analysis = dicom::geometry::analyze_slice_stack(
		    std::span<const dicom::geometry::SliceStackInput>(
		        inputs.data(), inputs.size()),
		    allow_duplicate_options);
		if (!analysis.ok() || analysis.uniform_spacing_k()) {
			fail("allowed duplicate positions should not become a uniform volume");
		}
		auto plan = dicom::geometry::plan_slice_stack(
		    std::span<const dicom::geometry::SliceStackInput>(
		        inputs.data(), inputs.size()),
		    allow_duplicate_options);
		if (plan.ok() || plan.volume_geometry() ||
		    plan.status() != dicom::geometry::SliceStackStatus::non_uniform_spacing ||
		    plan.issues().empty() ||
		    plan.issues().front().tag != "ImagePositionPatient"_tag ||
		    plan.issues().front().source_index != 1) {
			fail("allowed duplicate positions should leave a plan diagnostic");
		}
	}

	{
		const auto z10 = make_test_plane({2.5, 1.5}, {256, 128}, {0.0, 0.0, 10.0});
		const auto close_z10 =
		    make_test_plane({2.5, 1.5}, {256, 128}, {0.0, 0.0, 10.0005});
		const std::array<dicom::geometry::SliceStackInput, 2> inputs{{
		    {0, 0, z10, "1.2.3"},
		    {1, 0, close_z10, "1.2.3"},
		}};
		auto analysis = dicom::geometry::analyze_slice_stack(
		    std::span<const dicom::geometry::SliceStackInput>(
		        inputs.data(), inputs.size()));
		if (analysis.ok() ||
		    analysis.status() !=
		        dicom::geometry::SliceStackStatus::duplicate_slice_position) {
			fail("default slice position tolerance should detect close duplicates");
		}

		dicom::geometry::SliceStackOptions tight_position_options;
		tight_position_options.slice_position_tolerance_mm = 1e-4;
		analysis = dicom::geometry::analyze_slice_stack(
		    std::span<const dicom::geometry::SliceStackInput>(
		        inputs.data(), inputs.size()),
		    tight_position_options);
		if (!analysis.ok() || !analysis.uniform_spacing_k()) {
			fail("slice_position_tolerance_mm should control duplicate detection");
		}
	}

	{
		const auto z00 =
		    make_test_plane({2.5, 1.5}, {256, 128}, {0.0, 0.0, 0.0});
		const auto z10 =
		    make_test_plane({2.5, 1.5}, {256, 128}, {0.0, 0.0, 10.0});
		const auto z20 =
		    make_test_plane({2.5, 1.5}, {256, 128}, {0.0, 0.0, 20.0});
		const auto z50 =
		    make_test_plane({2.5, 1.5}, {256, 128}, {0.0, 0.0, 50.0});
		const auto z70 =
		    make_test_plane({2.5, 1.5}, {256, 128}, {0.0, 0.0, 70.0});
		const auto z90 =
		    make_test_plane({2.5, 1.5}, {256, 128}, {0.0, 0.0, 90.0});
		const std::array<dicom::geometry::SliceStackInput, 6> inputs{{
		    {0, 0, z00, "1.2.3"},
		    {1, 0, z10, "1.2.3"},
		    {2, 0, z20, "1.2.3"},
		    {3, 0, z50, "1.2.3"},
		    {4, 0, z70, "1.2.3"},
		    {5, 0, z90, "1.2.3"},
		}};
		auto analysis = dicom::geometry::analyze_slice_stack(
		    std::span<const dicom::geometry::SliceStackInput>(
		        inputs.data(), inputs.size()));
		if (analysis.ok() ||
		    analysis.status() !=
		        dicom::geometry::SliceStackStatus::non_uniform_spacing ||
		    analysis.uniform_runs().size() != 2) {
			fail("non-uniform stack should expose uniform runs");
		}
		if (analysis.uniform_runs()[0].begin_sorted_index != 0 ||
		    analysis.uniform_runs()[0].end_sorted_index != 3 ||
		    analysis.uniform_runs()[1].begin_sorted_index != 3 ||
		    analysis.uniform_runs()[1].end_sorted_index != 6) {
			fail("uniform runs should describe sorted half-open ranges");
		}
		expect_near(analysis.uniform_runs()[0].spacing_mm, 10.0,
		    "first uniform run spacing");
		expect_near(analysis.uniform_runs()[1].spacing_mm, 20.0,
		    "second uniform run spacing");
	}

	{
		const auto z10 = make_test_plane({2.5, 1.5}, {256, 128}, {0.0, 0.0, 10.0});
		const auto z25 = make_test_plane({2.5, 1.5}, {256, 128}, {0.0, 0.0, 25.0});
		const auto z30 = make_test_plane({2.5, 1.5}, {256, 128}, {0.0, 0.0, 30.0});
		const std::array<dicom::geometry::SliceStackInput, 3> inputs{{
		    {0, 0, z10, "1.2.3"},
		    {1, 0, z25, "1.2.3"},
		    {2, 0, z30, "1.2.3"},
		}};
		auto analysis = dicom::geometry::analyze_slice_stack(
		    std::span<const dicom::geometry::SliceStackInput>(
		        inputs.data(), inputs.size()));
		if (analysis.ok() ||
		    analysis.status() !=
		        dicom::geometry::SliceStackStatus::non_uniform_spacing ||
		    analysis.uniform_spacing_k()) {
			fail("non-uniform slice spacing should keep analysis diagnostic-only");
		}
		auto plan = dicom::geometry::plan_slice_stack(
		    std::span<const dicom::geometry::SliceStackInput>(
		        inputs.data(), inputs.size()));
		if (plan.ok() || plan.volume_geometry() ||
		    plan.status() != dicom::geometry::SliceStackStatus::non_uniform_spacing) {
			fail("non-uniform slice spacing should not produce a volume plan");
		}
	}

	{
		const auto z10 = make_test_plane({2.5, 1.5}, {256, 128}, {0.0, 0.0, 10.0});
		const auto drift =
		    make_test_plane({2.5, 1.5}, {256, 128}, {0.1, 0.0, 20.0});
		const std::array<dicom::geometry::SliceStackInput, 2> inputs{{
		    {0, 0, z10, "1.2.3"},
		    {1, 0, drift, "1.2.3"},
		}};
		auto analysis = dicom::geometry::analyze_slice_stack(
		    std::span<const dicom::geometry::SliceStackInput>(
		        inputs.data(), inputs.size()));
		if (analysis.ok() ||
		    analysis.status() !=
		        dicom::geometry::SliceStackStatus::inconsistent_slice_origin) {
			fail("slice stack should reject in-plane origin residual");
		}

		dicom::geometry::SliceStackOptions relaxed_residual_options;
		relaxed_residual_options.origin_residual_tolerance_mm = 0.2;
		analysis = dicom::geometry::analyze_slice_stack(
		    std::span<const dicom::geometry::SliceStackInput>(
		        inputs.data(), inputs.size()),
		    relaxed_residual_options);
		if (!analysis.ok() || !analysis.uniform_spacing_k() ||
		    analysis.max_in_plane_residual_mm() < 0.099 ||
		    analysis.slices()[1].in_plane_residual_mm < 0.099) {
			fail("origin_residual_tolerance_mm should control in-plane residual");
		}
		auto plan = dicom::geometry::plan_slice_stack(
		    std::span<const dicom::geometry::SliceStackInput>(
		        inputs.data(), inputs.size()),
		    relaxed_residual_options);
		if (!plan.ok() || plan.placements().size() != 2 ||
		    plan.placements()[1].in_plane_residual_mm < 0.099) {
			fail("slice stack plan should preserve in-plane residual diagnostics");
		}
	}

	{
		auto file_z20 = make_single_frame_file_at_z(20.0);
		auto file_z10 = make_single_frame_file_at_z(10.0);
		auto file_z30 = make_single_frame_file_at_z(30.0);
		const std::array<const dicom::DataSet*, 3> datasets{{
		    &file_z20->dataset(),
		    &file_z10->dataset(),
		    &file_z30->dataset(),
		}};
		auto analysis = dicom::geometry::analyze_slice_stack(
		    std::span<const dicom::DataSet* const>(
		        datasets.data(), datasets.size()));
		if (!analysis.ok() || analysis.slices().size() != 3 ||
		    !analysis.uniform_spacing_k()) {
			fail("classic DataSet stack analysis should match direct inputs");
		}
		if (analysis.slices()[0].source_index != 1 ||
		    analysis.slices()[1].source_index != 0 ||
		    analysis.slices()[2].source_index != 2) {
			fail("classic DataSet stack should preserve source_index mapping");
		}

		auto plan = dicom::geometry::plan_slice_stack(
		    std::span<const dicom::DataSet* const>(
		        datasets.data(), datasets.size()));
		if (!plan.ok() || !plan.volume_geometry() ||
		    plan.placements().size() != 3 ||
		    plan.placements()[0].source_index != 1 ||
		    plan.placements()[0].target_k != 0) {
			fail("classic DataSet stack should produce sorted placements");
		}
		expect_near(plan.volume_geometry()->origin().z, 10.0,
		    "classic DataSet stack volume origin");
		expect_near(plan.volume_geometry()->spacing_k(), 10.0,
		    "classic DataSet stack volume spacing_k");
	}

	{
		auto good = make_single_frame_file_at_z(10.0);
		dicom::DicomFile bad;
		set_int(bad, "Rows"_tag, 128);
		set_int(bad, "Columns"_tag, 256);
		set_text(bad, "FrameOfReferenceUID"_tag, "1.2.826.0.1.3680043.10.543.1");
		const std::array<const dicom::DataSet*, 2> datasets{{
		    &good->dataset(),
		    &bad.dataset(),
		}};
		auto analysis = dicom::geometry::analyze_slice_stack(
		    std::span<const dicom::DataSet* const>(
		        datasets.data(), datasets.size()));
		if (analysis.ok() ||
		    analysis.status() != dicom::geometry::SliceStackStatus::missing_geometry ||
		    analysis.issues().empty() ||
		    analysis.issues().back().source_index != 1 ||
		    analysis.issues().back().source.leaf_tag() !=
		        "ImagePositionPatient"_tag) {
			fail("classic DataSet stack should report missing geometry by source index");
		}
		auto plan = dicom::geometry::plan_slice_stack(
		    std::span<const dicom::DataSet* const>(
		        datasets.data(), datasets.size()));
		if (plan.ok() || plan.volume_geometry() ||
		    plan.status() != dicom::geometry::SliceStackStatus::missing_geometry) {
			fail("classic DataSet stack with missing geometry should not plan volume");
		}
	}

	{
		auto file = make_enhanced_ct_stack_file();
		const std::array<std::size_t, 3> frame_indices{{1, 0, 2}};
		auto analysis = dicom::geometry::analyze_image_frame_stack(
		    *file, std::span<const std::size_t>(
		               frame_indices.data(), frame_indices.size()));
		if (!analysis.ok() || analysis.slices().size() != 3 ||
		    analysis.slices()[0].frame_index != 0 ||
		    analysis.slices()[1].frame_index != 1 ||
		    analysis.slices()[2].frame_index != 2 ||
		    analysis.frame_of_reference_uid() !=
		        "1.2.826.0.1.3680043.10.543.77") {
			fail("enhanced frame stack analysis should preserve sorted frame indices");
		}

		auto plan = dicom::geometry::plan_image_frame_stack(
		    *file, std::span<const std::size_t>(
		               frame_indices.data(), frame_indices.size()));
		if (!plan.ok() || !plan.volume_geometry() ||
		    plan.placements().size() != 3 ||
		    plan.placements()[0].source_index != 0 ||
		    plan.placements()[0].frame_index != 0 ||
		    plan.placements()[0].target_k != 0 ||
		    plan.placements()[1].frame_index != 1 ||
		    plan.placements()[2].frame_index != 2) {
			fail("enhanced frame stack plan should map target_k to frame_index");
		}
		expect_near(plan.volume_geometry()->origin().z, 10.0,
		    "enhanced frame stack volume origin");
		expect_near(plan.volume_geometry()->spacing_k(), 10.0,
		    "enhanced frame stack spacing_k");
	}

	{
		auto file = make_enhanced_ct_stack_file();
		file->dataset().remove_dataelement("DimensionIndexSequence"_tag);
		for (int frame_index = 0; frame_index < 3; ++frame_index) {
			auto* frame_item = file->dataset().sequence_item(
			    "PerFrameFunctionalGroupsSequence"_tag,
			    static_cast<std::uint32_t>(frame_index));
			if (!frame_item) {
				fail("test setup should have PerFrameFunctionalGroupsSequence item");
			}
			frame_item->remove_dataelement("FrameContentSequence"_tag);
		}
		auto stacks = dicom::geometry::analyze_image_frame_stacks(*file);
		if (stacks.ok() ||
		    stacks.status() !=
		        dicom::geometry::SliceStackStatus::missing_dimension_module ||
		    stacks.issues().empty() ||
		    stacks.issues().front().tag != "DimensionIndexSequence"_tag) {
			fail("enhanced whole-file grouping should require DimensionIndexSequence");
		}
		dicom::geometry::ImageFrameStackOptions options;
		options.allow_geometry_grouping_fallback = true;
		stacks = dicom::geometry::analyze_image_frame_stacks(*file, options);
		if (!stacks.ok() || stacks.groups().size() != 1 ||
		    !stacks.groups()[0].analysis.ok()) {
			fail("explicit geometry grouping fallback should use frame geometry only");
		}
	}

	{
		auto file = make_enhanced_ct_stack_file();
		set_text(*file, "DimensionOrganizationType"_tag, "TILED_FULL");
		auto stacks = dicom::geometry::analyze_image_frame_stacks(*file);
		if (stacks.ok() ||
		    stacks.status() !=
		        dicom::geometry::SliceStackStatus::unsupported_tiled_image ||
		    stacks.issues().empty() ||
		    stacks.issues().front().tag != "DimensionOrganizationType"_tag) {
			fail("tiled multi-frame images should be rejected explicitly");
		}
	}

	{
		auto file = make_enhanced_ct_stack_file();
		auto stacks = dicom::geometry::analyze_image_frame_stacks(*file);
		if (!stacks.ok() || stacks.groups().size() != 1 ||
		    stacks.groups()[0].key.stack_id != "STACK_A" ||
		    stacks.groups()[0].frame_indices.size() != 3 ||
		    stacks.groups()[0].frame_indices[0] != 0 ||
		    stacks.groups()[0].frame_indices[1] != 1 ||
		    stacks.groups()[0].frame_indices[2] != 2 ||
		    !stacks.groups()[0].analysis.ok()) {
			fail("enhanced image frame stack analysis should group one StackID");
		}

		auto analysis = dicom::geometry::analyze_image_frame_stack(*file);
		if (!analysis.ok() || analysis.slices().size() != 3 ||
		    analysis.slices()[0].frame_index != 0 ||
		    analysis.slices()[2].frame_index != 2) {
			fail("single-stack enhanced convenience analysis should succeed");
		}

		auto plan = dicom::geometry::plan_image_frame_stack(*file);
		if (!plan.ok() || !plan.volume_geometry() ||
		    plan.placements().size() != 3 ||
		    plan.placements()[0].frame_index != 0 ||
		    plan.placements()[2].target_k != 2) {
			fail("single-stack enhanced convenience plan should succeed");
		}
	}

	{
		auto file = make_enhanced_ct_stack_file();
		set_text(*file,
		    "PerFrameFunctionalGroupsSequence.2.FrameContentSequence.0.StackID",
		    "STACK_B");
		set_int(*file,
		    "PerFrameFunctionalGroupsSequence.2.FrameContentSequence.0."
		    "InStackPositionNumber",
		    1);
		auto stacks = dicom::geometry::analyze_image_frame_stacks(*file);
		if (!stacks.ok() || stacks.groups().size() != 2 ||
		    stacks.groups()[0].key.stack_id != "STACK_A" ||
		    stacks.groups()[0].frame_indices.size() != 2 ||
		    stacks.groups()[1].key.stack_id != "STACK_B" ||
		    stacks.groups()[1].frame_indices.size() != 1) {
			fail("enhanced image frame stack analysis should split StackID groups");
		}
		auto plan = dicom::geometry::plan_image_frame_stack(*file);
		if (plan.ok() || plan.volume_geometry() ||
		    plan.status() !=
		        dicom::geometry::SliceStackStatus::multiple_frame_stacks ||
		    plan.issues().empty()) {
			fail("whole-file enhanced plan should reject multiple frame stacks");
		}
	}

	{
		auto file = make_enhanced_ct_stack_file();
		set_int(*file, "NumberOfFrames"_tag, 6);
		set_tag(*file,
		    "DimensionIndexSequence.0.DimensionIndexPointer",
		    "InStackPositionNumber"_tag);
		set_tag(*file,
		    "DimensionIndexSequence.0.FunctionalGroupPointer",
		    "FrameContentSequence"_tag);
		set_text(*file,
		    "DimensionIndexSequence.0.DimensionDescriptionLabel",
		    "Stack position");
		set_tag(*file,
		    "DimensionIndexSequence.1.DimensionIndexPointer",
		    "TemporalPositionIndex"_tag);
		set_tag(*file,
		    "DimensionIndexSequence.1.FunctionalGroupPointer",
		    "FrameContentSequence"_tag);
		set_text(*file,
		    "DimensionIndexSequence.1.DimensionDescriptionLabel",
		    "Temporal position");
		for (int frame_index = 0; frame_index < 6; ++frame_index) {
			const auto base = std::string("PerFrameFunctionalGroupsSequence.") +
			    std::to_string(frame_index) + ".";
			const int in_stack_position = frame_index % 3 + 1;
			const int temporal_position = frame_index < 3 ? 1 : 2;
			const std::array<double, 3> position{
			    0.0,
			    0.0,
			    in_stack_position == 1 ? 10.0 :
			        in_stack_position == 2 ? 20.0 :
			                                 30.0,
			};
			const std::array<long, 2> dimension_values{{
			    static_cast<long>(in_stack_position),
			    static_cast<long>(temporal_position),
			}};
			set_doubles(*file,
			    base + "PlanePositionSequence.0.ImagePositionPatient",
			    position);
			set_text(*file, base + "FrameContentSequence.0.StackID", "STACK_A");
			set_int(*file,
			    base + "FrameContentSequence.0.InStackPositionNumber",
			    in_stack_position);
			set_longs(*file,
			    base + "FrameContentSequence.0.DimensionIndexValues",
			    dimension_values);
		}

		auto stacks = dicom::geometry::analyze_image_frame_stacks(*file);
		if (!stacks.ok() || stacks.groups().size() != 2 ||
		    stacks.groups()[0].key.dimension_values.size() != 1 ||
		    stacks.groups()[0].key.dimension_values[0].descriptor
		            .dimension_index_pointer != "TemporalPositionIndex"_tag ||
		    stacks.groups()[0].key.dimension_values[0].value != 1 ||
		    stacks.groups()[1].key.dimension_values.size() != 1 ||
		    stacks.groups()[1].key.dimension_values[0].value != 2 ||
		    stacks.groups()[0].frame_indices[0] != 0 ||
		    stacks.groups()[0].frame_indices[2] != 2 ||
		    stacks.groups()[1].frame_indices[0] != 3 ||
		    stacks.groups()[1].frame_indices[2] != 5 ||
		    !stacks.groups()[0].analysis.ok() ||
		    !stacks.groups()[1].analysis.ok()) {
			fail("DimensionIndexValues should split non-spatial frame stacks");
		}

		auto plan = dicom::geometry::plan_image_frame_stack(*file);
		if (plan.ok() ||
		    plan.status() !=
		        dicom::geometry::SliceStackStatus::multiple_frame_stacks) {
			fail("whole-file enhanced plan should reject temporal stacks");
		}

		set_doubles(*file,
		    "SharedFunctionalGroupsSequence.0.PixelMeasuresSequence.0.PixelSpacing",
		    std::array<double, 2>{2.0, -3.0});
		auto analysis = dicom::geometry::analyze_image_frame_stack(*file);
		if (analysis.ok() ||
		    analysis.status() !=
		        dicom::geometry::SliceStackStatus::multiple_frame_stacks) {
			fail("whole-file enhanced analysis should report multiple stacks first");
		}
		plan = dicom::geometry::plan_image_frame_stack(*file);
		if (plan.ok() ||
		    plan.status() !=
		        dicom::geometry::SliceStackStatus::multiple_frame_stacks) {
			fail("whole-file enhanced plan should report multiple stacks first");
		}
	}

	{
		auto file = make_enhanced_ct_stack_file();
		const std::array<long, 2> too_many_values{{1, 99}};
		set_longs(*file,
		    "PerFrameFunctionalGroupsSequence.0.FrameContentSequence.0."
		    "DimensionIndexValues",
		    too_many_values);
		auto stacks = dicom::geometry::analyze_image_frame_stacks(*file);
		if (stacks.ok() ||
		    stacks.status() !=
		        dicom::geometry::SliceStackStatus::geometry_parse_failure ||
		    stacks.issues().empty() ||
		    stacks.issues().front().tag != "DimensionIndexValues"_tag ||
		    stacks.issues().front().frame_index != 0 ||
		    stacks.issues().front().source.leaf_tag() !=
		        "DimensionIndexValues"_tag ||
		    stacks.issues().front().source.depth() == 0) {
			fail("extra DimensionIndexValues should be rejected");
		}
	}

	{
		auto file = make_enhanced_ct_stack_file();
		set_tag(*file,
		    "DimensionIndexSequence.0.DimensionIndexPointer",
		    "TemporalPositionIndex"_tag);
		set_tag(*file,
		    "DimensionIndexSequence.0.FunctionalGroupPointer",
		    "FrameContentSequence"_tag);
		for (int frame_index = 0; frame_index < 3; ++frame_index) {
			auto* frame_content = file->dataset().sequence_item(
			    "PerFrameFunctionalGroupsSequence"_tag,
			    static_cast<std::uint32_t>(frame_index));
			if (!frame_content) {
				fail("test setup should have PerFrameFunctionalGroupsSequence item");
			}
			frame_content = frame_content->sequence_item(
			    "FrameContentSequence"_tag, 0);
			if (!frame_content) {
				fail("test setup should have FrameContentSequence item");
			}
			frame_content->remove_dataelement("DimensionIndexValues"_tag);
		}
		auto stacks = dicom::geometry::analyze_image_frame_stacks(*file);
		if (stacks.ok() ||
		    stacks.status() !=
		        dicom::geometry::SliceStackStatus::geometry_parse_failure ||
		    stacks.issues().empty() ||
		    stacks.issues()[0].tag != "DimensionIndexValues"_tag ||
		    stacks.issues()[0].frame_index != 0 ||
		    stacks.issues()[0].source.leaf_tag() !=
		        "DimensionIndexValues"_tag ||
		    stacks.issues()[0].source.depth() == 0) {
			fail("missing DimensionIndexValues should be reported by frame");
		}
	}

	{
		auto file = make_enhanced_ct_stack_file();
		auto* frame_item =
		    file->dataset().sequence_item("PerFrameFunctionalGroupsSequence"_tag, 1);
		if (!frame_item) {
			fail("test setup should have PerFrameFunctionalGroupsSequence item");
		}
		auto* frame_content =
		    frame_item->sequence_item("FrameContentSequence"_tag, 0);
		if (!frame_content) {
			fail("test setup should have FrameContentSequence item");
		}
		frame_content->remove_dataelement("StackID"_tag);
		auto stacks = dicom::geometry::analyze_image_frame_stacks(*file);
		if (stacks.ok() ||
		    stacks.status() !=
		        dicom::geometry::SliceStackStatus::missing_frame_content ||
		    stacks.issues().empty() ||
		    stacks.issues()[0].tag != "StackID"_tag ||
		    stacks.issues()[0].source.leaf_tag() != "StackID"_tag ||
		    stacks.issues()[0].source.depth() == 0) {
			fail("missing StackID should preserve source path");
		}
	}

	{
		auto file = make_enhanced_ct_stack_file();
		auto* frame_item =
		    file->dataset().sequence_item("PerFrameFunctionalGroupsSequence"_tag, 1);
		if (!frame_item) {
			fail("test setup should have PerFrameFunctionalGroupsSequence item");
		}
		frame_item->remove_dataelement("FrameContentSequence"_tag);
		auto stacks = dicom::geometry::analyze_image_frame_stacks(*file);
		if (stacks.ok() ||
		    stacks.status() !=
		        dicom::geometry::SliceStackStatus::missing_frame_content ||
		    stacks.issues().empty() || stacks.issues()[0].frame_index != 1 ||
		    stacks.issues()[0].source.leaf_tag() !=
		        "FrameContentSequence"_tag ||
		    stacks.issues()[0].source.depth() == 0) {
			fail("missing FrameContentSequence should be reported by frame");
		}
	}

	{
		auto file = make_enhanced_ct_stack_file();
		auto* frame_item =
		    file->dataset().sequence_item("PerFrameFunctionalGroupsSequence"_tag, 1);
		if (!frame_item) {
			fail("test setup should have PerFrameFunctionalGroupsSequence item");
		}
		frame_item->remove_dataelement("FrameContentSequence"_tag);
		auto& bad_frame_content =
		    frame_item->add_dataelement("FrameContentSequence"_tag, dicom::VR::LO);
		if (!bad_frame_content.from_string_view("bad")) {
			fail("failed to create malformed FrameContentSequence");
		}
		auto stacks = dicom::geometry::analyze_image_frame_stacks(*file);
		if (stacks.ok() ||
		    stacks.status() !=
		        dicom::geometry::SliceStackStatus::geometry_parse_failure ||
		    stacks.issues().empty() || stacks.issues()[0].frame_index != 1 ||
		    stacks.issues()[0].tag != "FrameContentSequence"_tag ||
		    stacks.issues()[0].source.leaf_tag() !=
		        "FrameContentSequence"_tag ||
		    stacks.issues()[0].source.depth() == 0 ||
		    stacks.issues()[0].message.find("malformed FrameContentSequence") ==
		        std::string::npos) {
			fail("malformed FrameContentSequence should be reported distinctly");
		}
	}

	{
		auto file = make_enhanced_ct_stack_file();
		set_text(*file,
		    "PerFrameFunctionalGroupsSequence.1.CTImageFrameTypeSequence.0."
		    "VolumetricProperties",
		    "SAMPLED");
		dicom::geometry::FrameGeometryReader reader(*file);
		auto plane = reader.plane(1);
		if (plane.ok() ||
		    plane.status() !=
		        dicom::geometry::GeometryBuildStatus::sampled_frame_geometry) {
			fail("FrameGeometryReader::plane should reject sampled frames");
		}
		auto frame_geometry = reader.image_frame_geometry(1);
		if (!frame_geometry.ok() ||
		    frame_geometry.value().kind !=
		        dicom::geometry::ImageFrameGeometryKind::sampled_projection) {
			fail("image_frame_geometry should preserve sampled frame kind");
		}
	}

	{
		auto file = make_enhanced_ct_stack_file();
		auto* shared_item =
		    file->dataset().sequence_item("SharedFunctionalGroupsSequence"_tag, 0);
		if (!shared_item) {
			fail("test setup should have SharedFunctionalGroupsSequence item");
		}
		shared_item->remove_dataelement("PixelMeasuresSequence"_tag);
		auto& bad_pixel_measures =
		    shared_item->add_dataelement("PixelMeasuresSequence"_tag, dicom::VR::LO);
		if (!bad_pixel_measures.from_string_view("bad")) {
			fail("failed to create malformed PixelMeasuresSequence");
		}
		set_doubles(*file, "PixelSpacing"_tag, std::array<double, 2>{2.0, 3.0});
		auto plane = dicom::geometry::plane_from_multiframe_image(*file, 0);
		if (plane.ok() ||
		    plane.status() != dicom::geometry::GeometryBuildStatus::invalid_value ||
		    plane.tag() != "PixelMeasuresSequence"_tag ||
		    plane.source().depth() != 1 ||
		    plane.source().parents()[0].sequence_tag !=
		        "SharedFunctionalGroupsSequence"_tag ||
		    plane.source().leaf_tag() != "PixelMeasuresSequence"_tag) {
			fail("malformed shared macro sequence should not fall back to root");
		}
	}

	{
		auto file = make_enhanced_ct_stack_file();
		set_text(*file,
		    "PerFrameFunctionalGroupsSequence.1.CTImageFrameTypeSequence.0."
		    "VolumetricProperties",
		    "SAMPLED");
		const std::array<std::size_t, 3> frame_indices{{0, 1, 2}};
		auto plan = dicom::geometry::plan_image_frame_stack(
		    *file, std::span<const std::size_t>(
		               frame_indices.data(), frame_indices.size()));
		if (plan.ok() || plan.volume_geometry() ||
		    plan.status() != dicom::geometry::SliceStackStatus::geometry_parse_failure ||
		    plan.issues().empty() || plan.issues().front().frame_index != 1) {
			fail("sampled enhanced frame should block image frame stack planning");
		}
	}

	{
		const auto source = make_test_plane();
		const auto target = make_test_plane();
		auto check = dicom::geometry::check_overlay_compatibility(
		    "1.2.3", source, "1.2.3", target);
		if (!check.ok() || !check.can_direct_overlay ||
		    check.status != dicom::geometry::OverlayCompatibility::compatible) {
			fail("identical planes should be directly compatible");
		}

		const auto smaller_extent =
		    make_test_plane({2.5, 1.5}, {128, 64}, {10.0, 20.0, 30.0});
		check = dicom::geometry::check_overlay_compatibility(
		    "1.2.3", source, "1.2.3", smaller_extent);
		if (!check.ok() || !check.overlaps_extent ||
		    check.requires_resampling ||
		    check.status != dicom::geometry::OverlayCompatibility::different_extent) {
			fail("extent-only plane mismatch should not require resampling");
		}

		check = dicom::geometry::check_overlay_compatibility(
		    "1.2.3", source, "1.2.4", target);
		if (check.ok() ||
		    check.status !=
		        dicom::geometry::OverlayCompatibility::different_frame_of_reference) {
			fail("different FoR should block transform");
		}

		const auto resampled_target =
		    make_test_plane(dicom::geometry::ImageSpacing2D{3.0, 1.5});
		check = dicom::geometry::check_overlay_compatibility(
		    "1.2.3", source, "1.2.3", resampled_target);
		if (!check.ok() || !check.requires_resampling ||
		    check.status != dicom::geometry::OverlayCompatibility::different_spacing) {
			fail("different spacing should require resampling");
		}
		dicom::geometry::OverlayCheckOptions strict_options;
		strict_options.require_same_grid = true;
		check = dicom::geometry::check_overlay_compatibility(
		    "1.2.3", source, "1.2.3", resampled_target, strict_options);
		if (check.ok() || check.can_transform || check.can_direct_overlay ||
		    !check.requires_resampling ||
		    check.status != dicom::geometry::OverlayCompatibility::different_spacing) {
			fail("require_same_grid should reject spacing resampling as ok");
		}

		const auto rotated_target = make_test_plane({2.5, 1.5}, {256, 128},
		    {10.0, 20.0, 30.0}, {0.0, 1.0, 0.0}, {-1.0, 0.0, 0.0});
		check = dicom::geometry::check_overlay_compatibility(
		    "1.2.3", source, "1.2.3", rotated_target);
		if (!check.ok() || check.can_direct_overlay ||
		    !check.requires_resampling ||
		    check.status != dicom::geometry::OverlayCompatibility::requires_resampling) {
			fail("rotated in-plane axes should require resampling");
		}

		dicom::geometry::ImagePlaneGeometryParams opposite_params;
		opposite_params.origin = {10.0, 20.0, 30.0};
		opposite_params.direction_i = {1.0, 0.0, 0.0};
		opposite_params.direction_j = {0.0, -1.0, 0.0};
		opposite_params.spacing = {3.0, 1.5};
		opposite_params.size = {256, 128};
		auto opposite_result =
		    dicom::geometry::make_image_plane_geometry(opposite_params);
		if (!opposite_result.ok()) {
			fail("opposite plane should still be valid geometry");
		}
		check = dicom::geometry::check_overlay_compatibility(
		    "1.2.3", source, "1.2.3", opposite_result.value());
		if (!check.ok() ||
		    check.status != dicom::geometry::OverlayCompatibility::opposite_orientation) {
			fail("opposite orientation should win over spacing in status priority");
		}
	}

	{
		const auto plane = make_test_plane();
		const auto volume = make_test_volume();
		auto check = dicom::geometry::check_overlay_compatibility(
		    "1.2.3", plane, "1.2.3", volume);
		if (!check.ok() || !check.can_direct_overlay ||
		    !check.source_inside_target_extent || !check.overlaps_extent ||
		    !check.target_k_range || check.target_k_range->begin != 0 ||
		    check.target_k_range->end != 1 ||
		    check.status != dicom::geometry::OverlayCompatibility::compatible) {
			fail("plane-volume check should allow direct overlay on matching slice");
		}

		const auto slice_four =
		    make_test_plane({2.5, 1.5}, {256, 128}, {10.0, 20.0, 46.0});
		check = dicom::geometry::check_overlay_compatibility(
		    "1.2.3", slice_four, "1.2.3", volume);
		if (!check.ok() || !check.can_direct_overlay ||
		    !check.target_k_range || check.target_k_range->begin != 4 ||
		    check.target_k_range->end != 5) {
			fail("plane-volume check should report target k range");
		}

		const auto outside =
		    make_test_plane({2.5, 1.5}, {256, 128}, {10.0, 20.0, 500.0});
		check = dicom::geometry::check_overlay_compatibility(
		    "1.2.3", outside, "1.2.3", volume);
		if (!check.ok() || check.overlaps_extent || check.target_k_range ||
		    check.status != dicom::geometry::OverlayCompatibility::different_extent) {
			fail("plane-volume check should report no extent overlap");
		}

		const auto out_of_plane =
		    make_test_plane({2.5, 1.5}, {256, 128}, {10.0, 20.0, 34.0});
		check = dicom::geometry::check_overlay_compatibility(
		    "1.2.3", plane, "1.2.3", out_of_plane);
		if (!check.ok() || check.overlaps_extent || check.requires_resampling ||
		    check.status != dicom::geometry::OverlayCompatibility::out_of_plane) {
			fail("plane-plane out-of-plane should not report extent overlap");
		}

		const auto resampled =
		    make_test_plane({3.0, 1.5}, {256, 128}, {10.0, 20.0, 30.0});
		check = dicom::geometry::check_overlay_compatibility(
		    "1.2.3", resampled, "1.2.3", volume);
		if (!check.ok() || !check.requires_resampling ||
		    check.status != dicom::geometry::OverlayCompatibility::different_spacing) {
			fail("plane-volume spacing mismatch should require resampling");
		}
	}

	{
		const auto source = make_test_volume();
		const auto target = make_test_volume();
		auto check = dicom::geometry::check_overlay_compatibility(
		    "1.2.3", source, "1.2.3", target);
		if (!check.ok() || !check.can_direct_overlay ||
		    !check.source_inside_target_extent || !check.overlaps_extent ||
		    !check.target_k_range || check.target_k_range->begin != 0 ||
		    check.target_k_range->end != 32 ||
		    check.status != dicom::geometry::OverlayCompatibility::compatible) {
			fail("volume-volume check should allow direct overlay on matching grid");
		}

		const auto outside =
		    make_test_volume({2.5, 1.5, 4.0}, {256, 128, 32},
		        {10.0, 20.0, 500.0});
		check = dicom::geometry::check_overlay_compatibility(
		    "1.2.3", outside, "1.2.3", target);
		if (!check.ok() || check.overlaps_extent || check.target_k_range ||
		    check.status != dicom::geometry::OverlayCompatibility::different_extent) {
			fail("volume-volume check should report no extent overlap");
		}
	}

	{
		const auto volume = make_test_volume();
		const auto plane = make_test_plane();
		auto check = dicom::geometry::check_overlay_compatibility(
		    "1.2.3", volume, "1.2.3", plane);
		if (!check.ok() || !check.can_direct_overlay ||
		    !check.source_inside_target_extent || !check.overlaps_extent ||
		    check.target_k_range ||
		    check.status != dicom::geometry::OverlayCompatibility::compatible) {
			fail("volume-plane check should allow direct overlay on matching slice");
		}

		const auto slice_four =
		    make_test_plane({2.5, 1.5}, {256, 128}, {10.0, 20.0, 46.0});
		check = dicom::geometry::check_overlay_compatibility(
		    "1.2.3", volume, "1.2.3", slice_four);
		if (!check.ok() || !check.can_direct_overlay || check.target_k_range) {
			fail("volume-plane check should match a later source slice");
		}

		const auto outside =
		    make_test_plane({2.5, 1.5}, {256, 128}, {10.0, 20.0, 500.0});
		check = dicom::geometry::check_overlay_compatibility(
		    "1.2.3", volume, "1.2.3", outside);
		if (!check.ok() || check.overlaps_extent ||
		    check.status != dicom::geometry::OverlayCompatibility::different_extent) {
			fail("volume-plane check should report no extent overlap");
		}

		const auto small_plane =
		    make_test_plane({2.5, 1.5}, {64, 64}, {10.0, 20.0, 30.0});
		check = dicom::geometry::check_overlay_compatibility(
		    "1.2.3", volume, "1.2.3", small_plane);
		if (!check.ok() || !check.overlaps_extent ||
		    check.source_inside_target_extent ||
		    check.status != dicom::geometry::OverlayCompatibility::different_extent) {
			fail("volume-plane check should report projected extent mismatch");
		}
	}

	{
		const auto source = make_test_plane();
		const auto target =
		    make_test_plane({2.5, 1.5}, {256, 128}, {15.0, 24.5, 30.0});
		auto transform =
		    dicom::geometry::make_plane_to_plane_transform(source, target);
		const auto target_index =
		    transform.target_index_from_source_index({2.0, 3.0});
		expect_near(target_index.i, 0.0, "plane-plane target.i");
		expect_near(target_index.j, 0.0, "plane-plane target.j");
		const auto source_index =
		    transform.source_index_from_target_index(target_index);
		expect_near(source_index.i, 2.0, "plane-plane source.i");
		expect_near(source_index.j, 3.0, "plane-plane source.j");
	}

	{
		const auto source =
		    make_test_plane({2.5, 1.5}, {256, 128}, {10.0, 20.0, 46.0});
		const auto target = make_test_volume();
		auto transform =
		    dicom::geometry::make_plane_to_volume_transform(source, target);
		const auto target_index =
		    transform.target_index_from_source_index({2.0, 3.0});
		expect_near(target_index.i, 2.0, "plane-volume target.i");
		expect_near(target_index.j, 3.0, "plane-volume target.j");
		expect_near(target_index.k, 4.0, "plane-volume target.k");
		const auto source_index =
		    transform.source_index_from_target_index(target_index);
		expect_near(source_index.i, 2.0, "plane-volume source.i");
		expect_near(source_index.j, 3.0, "plane-volume source.j");
		const auto projection =
		    transform.source_projection_from_target_index({2.0, 3.0, 5.0});
		expect_near(projection.index.i, 2.0, "plane-volume projection.i");
		expect_near(projection.index.j, 3.0, "plane-volume projection.j");
		expect_near(projection.signed_normal_distance_mm, 4.0,
		    "plane-volume signed normal distance");
	}

	{
		const auto source = make_test_volume();
		const auto target =
		    make_test_plane({2.5, 1.5}, {256, 128}, {10.0, 20.0, 46.0});
		auto transform =
		    dicom::geometry::make_volume_to_plane_transform(source, target);
		const auto target_index =
		    transform.target_index_from_source_index({2.0, 3.0, 4.0});
		expect_near(target_index.i, 2.0, "volume-plane target.i");
		expect_near(target_index.j, 3.0, "volume-plane target.j");
		const auto projection =
		    transform.target_projection_from_source_index({2.0, 3.0, 5.0});
		expect_near(projection.index.i, 2.0, "volume-plane projection.i");
		expect_near(projection.index.j, 3.0, "volume-plane projection.j");
		expect_near(projection.signed_normal_distance_mm, 4.0,
		    "volume-plane signed normal distance");
		const auto source_index =
		    transform.source_index_from_target_index({2.0, 3.0});
		expect_near(source_index.i, 2.0, "volume-plane source.i");
		expect_near(source_index.j, 3.0, "volume-plane source.j");
		expect_near(source_index.k, 4.0, "volume-plane source.k");
	}

	{
		const auto source = make_test_volume();
		const auto target = make_test_volume(
		    {2.5, 1.5, 4.0}, {256, 128, 32}, {15.0, 24.5, 46.0});
		auto transform =
		    dicom::geometry::make_volume_to_volume_transform(source, target);
		const auto target_index =
		    transform.target_index_from_source_index({2.0, 3.0, 4.0});
		expect_near(target_index.i, 0.0, "volume-volume target.i");
		expect_near(target_index.j, 0.0, "volume-volume target.j");
		expect_near(target_index.k, 0.0, "volume-volume target.k");
		const auto source_index =
		    transform.source_index_from_target_index(target_index);
		expect_near(source_index.i, 2.0, "volume-volume source.i");
		expect_near(source_index.j, 3.0, "volume-volume source.j");
		expect_near(source_index.k, 4.0, "volume-volume source.k");
	}

	{
		const auto plane = make_test_plane();
		const auto volume = make_test_volume();
		const auto plane_transform =
		    dicom::geometry::make_plane_to_volume_transform(plane, volume);
		const auto volume_transform =
		    dicom::geometry::make_volume_to_volume_transform(volume, volume);
		double sink = 0.0;
		expect_no_allocations(
		    [&]() {
			    for (int index = 0; index < 1000; ++index) {
				    const auto check = dicom::geometry::check_overlay_compatibility(
				        "1.2.3", plane, "1.2.3", volume);
				    sink += check.can_transform ? 1.0 : 0.0;
				    const auto point = plane_transform.target_index_from_source_index(
				        {static_cast<double>(index % 7),
				            static_cast<double>(index % 11)});
				    sink += point.i + point.j + point.k;
				    const auto volume_point =
				        volume_transform.target_index_from_source_index(
				            {1.0, 2.0, 3.0});
				    sink += volume_point.i;
				    const auto path = dicom::ElementPath{}
				        .item("PerFrameFunctionalGroupsSequence"_tag, 0)
				        .item("PixelMeasuresSequence"_tag, 0)
				        .element("PixelSpacing"_tag);
				    sink += path.ok() ? static_cast<double>(path.depth()) : 0.0;
			    }
		    },
		    "overlay/transform/ElementPath hot path");
		if (sink <= 0.0) {
			fail("no-allocation hot path test should execute work");
		}
	}

	return 0;
}
