#pragma once

#include "dicom.h"

#include <array>
#include <cstdint>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dicom::seg {
class Segmentation;
}

namespace dicom::geometry {

struct Vec3d {
	double x{0.0};
	double y{0.0};
	double z{0.0};
};

struct Point3d {
	double x{0.0};
	double y{0.0};
	double z{0.0};
};

struct ImagePoint2D {
	double i{0.0};
	double j{0.0};
};

struct ImagePoint3D {
	double i{0.0};
	double j{0.0};
	double k{0.0};
};

struct PlaneProjection2D {
	ImagePoint2D index{};
	double signed_normal_distance_mm{0.0};
};

struct ImageSize2D {
	std::size_t i{0}; // columns
	std::size_t j{0}; // rows
};

struct ImageSize3D {
	std::size_t i{0}; // columns
	std::size_t j{0}; // rows
	std::size_t k{0}; // slices/frames
};

struct ImageSpacing2D {
	double i{0.0}; // column spacing in mm
	double j{0.0}; // row spacing in mm
};

struct ImageSpacing3D {
	double i{0.0};
	double j{0.0};
	double k{0.0};
};

struct GeometryTolerance {
	double orientation_tolerance{1e-4};
	double spacing_tolerance_mm{1e-3};
	double position_tolerance_mm{1e-3};
	double normal_distance_tolerance_mm{1e-3};
};

/// Row-major 4x4 matrix. Multiplication uses column vectors:
/// `out_h = matrix * in_h`.
class Matrix4x4d {
public:
	static Matrix4x4d identity() noexcept;

	[[nodiscard]] double operator()(std::size_t row, std::size_t column) const noexcept {
		return values_[row * 4 + column];
	}
	double& operator()(std::size_t row, std::size_t column) noexcept {
		return values_[row * 4 + column];
	}

	[[nodiscard]] std::array<double, 4> multiply(
	    const std::array<double, 4>& vector) const noexcept;

private:
	std::array<double, 16> values_{};
};

enum class GeometryBuildStatus {
	ok,
	missing_required_tag,
	invalid_value,
	invalid_size,
	invalid_spacing,
	invalid_orientation,
	singular_matrix,
	invalid_frame_index,
	missing_volumetric_properties,
	mixed_volumetric_properties,
	unknown_volumetric_properties,
	sampled_frame_geometry,
	distorted_frame_geometry,
	unsupported_frame_geometry,
};

template <typename T>
class GeometryBuildResult {
public:
	static GeometryBuildResult success(T value, ElementPath source = {}) {
		return GeometryBuildResult(
		    GeometryBuildStatus::ok, std::move(value), Tag{}, std::string{},
		    std::move(source));
	}

	static GeometryBuildResult failure(
	    GeometryBuildStatus status, Tag tag = Tag{}, std::string message = {},
	    ElementPath source = {}) {
		if (status == GeometryBuildStatus::ok) {
			status = GeometryBuildStatus::invalid_value;
		}
		return GeometryBuildResult(
		    status, std::nullopt, tag, std::move(message), std::move(source));
	}

	[[nodiscard]] bool ok() const noexcept {
		return status_ == GeometryBuildStatus::ok && value_.has_value();
	}
	[[nodiscard]] GeometryBuildStatus status() const noexcept { return status_; }
	[[nodiscard]] Tag tag() const noexcept { return tag_; }
	[[nodiscard]] const std::string& message() const noexcept { return message_; }
	[[nodiscard]] const ElementPath& source() const noexcept { return source_; }
	[[nodiscard]] const T& value() const& { return *value_; }
	[[nodiscard]] T& value() & { return *value_; }
	[[nodiscard]] T&& value() && { return std::move(*value_); }
	[[nodiscard]] const std::optional<T>& maybe_value() const noexcept {
		return value_;
	}

private:
	GeometryBuildResult(GeometryBuildStatus status, std::optional<T> value, Tag tag,
	    std::string message, ElementPath source)
	    : status_(status), value_(std::move(value)), tag_(tag),
	      message_(std::move(message)), source_(std::move(source)) {}

	GeometryBuildStatus status_{GeometryBuildStatus::invalid_value};
	std::optional<T> value_{};
	Tag tag_{};
	std::string message_{};
	ElementPath source_{};
};

struct ImagePlaneGeometryParams {
	Point3d origin{};
	Vec3d direction_i{};
	Vec3d direction_j{};
	ImageSpacing2D spacing{};
	ImageSize2D size{};
};

struct ImageVolumeGeometryParams {
	Point3d origin{};
	Vec3d direction_i{};
	Vec3d direction_j{};
	Vec3d direction_k{};
	ImageSpacing3D spacing{};
	ImageSize3D size{};
};

class ImagePlaneGeometry {
public:
	ImagePlaneGeometry(const ImagePlaneGeometry&) = default;
	ImagePlaneGeometry(ImagePlaneGeometry&&) noexcept = default;
	ImagePlaneGeometry& operator=(const ImagePlaneGeometry&) = default;
	ImagePlaneGeometry& operator=(ImagePlaneGeometry&&) noexcept = default;

	[[nodiscard]] Point3d origin() const noexcept { return origin_; }
	[[nodiscard]] Vec3d direction_i() const noexcept { return direction_i_; }
	[[nodiscard]] Vec3d direction_j() const noexcept { return direction_j_; }
	[[nodiscard]] Vec3d normal() const noexcept { return normal_; }
	[[nodiscard]] ImageSpacing2D spacing() const noexcept { return spacing_; }
	[[nodiscard]] double spacing_i() const noexcept { return spacing_.i; }
	[[nodiscard]] double spacing_j() const noexcept { return spacing_.j; }
	[[nodiscard]] ImageSize2D size() const noexcept { return size_; }
	[[nodiscard]] std::size_t columns() const noexcept { return size_.i; }
	[[nodiscard]] std::size_t rows() const noexcept { return size_.j; }
	/// Embeds plane index `(i, j, normal_mm, 1)` in patient/world space.
	[[nodiscard]] const Matrix4x4d& index_to_world_matrix() const noexcept {
		return index_to_world_;
	}
	/// Returns `(i, j, signed_normal_distance_mm, 1)` for a world point.
	[[nodiscard]] const Matrix4x4d& world_to_index_matrix() const noexcept {
		return world_to_index_;
	}

	[[nodiscard]] Point3d world_from_index(ImagePoint2D index) const noexcept;
	[[nodiscard]] ImagePoint2D index_from_world(Point3d world) const noexcept;
	[[nodiscard]] double normal_distance_from_world(Point3d world) const noexcept;
	[[nodiscard]] bool contains_index(ImagePoint2D index) const noexcept;
	[[nodiscard]] bool contains_world(
	    Point3d world, double normal_distance_tolerance_mm = 1e-3) const noexcept;

private:
	friend GeometryBuildResult<ImagePlaneGeometry> make_image_plane_geometry(
	    const ImagePlaneGeometryParams&, GeometryTolerance);

	ImagePlaneGeometry() = default;

	Point3d origin_{};
	Vec3d direction_i_{};
	Vec3d direction_j_{};
	Vec3d normal_{};
	ImageSpacing2D spacing_{};
	ImageSize2D size_{};
	Matrix4x4d index_to_world_{};
	Matrix4x4d world_to_index_{};
};

/// Rectilinear 3D image grid with one affine transform and one positive spacing
/// per axis. Non-uniform slice stacks must stay in slice-stack analysis until a
/// caller chooses a policy for resampling or grid expansion.
class ImageVolumeGeometry {
public:
	ImageVolumeGeometry(const ImageVolumeGeometry&) = default;
	ImageVolumeGeometry(ImageVolumeGeometry&&) noexcept = default;
	ImageVolumeGeometry& operator=(const ImageVolumeGeometry&) = default;
	ImageVolumeGeometry& operator=(ImageVolumeGeometry&&) noexcept = default;

	[[nodiscard]] Point3d origin() const noexcept { return origin_; }
	[[nodiscard]] Vec3d direction_i() const noexcept { return direction_i_; }
	[[nodiscard]] Vec3d direction_j() const noexcept { return direction_j_; }
	[[nodiscard]] Vec3d direction_k() const noexcept { return direction_k_; }
	[[nodiscard]] ImageSpacing3D spacing() const noexcept { return spacing_; }
	[[nodiscard]] double spacing_i() const noexcept { return spacing_.i; }
	[[nodiscard]] double spacing_j() const noexcept { return spacing_.j; }
	[[nodiscard]] double spacing_k() const noexcept { return spacing_.k; }
	[[nodiscard]] ImageSize3D size() const noexcept { return size_; }
	[[nodiscard]] std::size_t columns() const noexcept { return size_.i; }
	[[nodiscard]] std::size_t rows() const noexcept { return size_.j; }
	[[nodiscard]] std::size_t slices() const noexcept { return size_.k; }
	[[nodiscard]] const Matrix4x4d& index_to_world_matrix() const noexcept {
		return index_to_world_;
	}
	[[nodiscard]] const Matrix4x4d& world_to_index_matrix() const noexcept {
		return world_to_index_;
	}

	[[nodiscard]] Point3d world_from_index(ImagePoint3D index) const noexcept;
	[[nodiscard]] ImagePoint3D index_from_world(Point3d world) const noexcept;
	[[nodiscard]] bool contains_index(ImagePoint3D index) const noexcept;
	[[nodiscard]] bool contains_world(
	    Point3d world, GeometryTolerance tolerance = {}) const noexcept;

private:
	friend GeometryBuildResult<ImageVolumeGeometry> make_image_volume_geometry(
	    const ImageVolumeGeometryParams&, GeometryTolerance);

	ImageVolumeGeometry() = default;

	Point3d origin_{};
	Vec3d direction_i_{};
	Vec3d direction_j_{};
	Vec3d direction_k_{};
	ImageSpacing3D spacing_{};
	ImageSize3D size_{};
	Matrix4x4d index_to_world_{};
	Matrix4x4d world_to_index_{};
};

enum class VolumetricPropertiesValue {
	volume,
	sampled,
	distorted,
};

enum class ImageFrameGeometryKind {
	regular_plane,
	sampled_projection,
	distorted,
};

struct VolumetricPropertiesInfo {
	VolumetricPropertiesValue value{VolumetricPropertiesValue::volume};
	ElementPath source{};
};

struct ImageFrameGeometry {
	ImageFrameGeometry(ImagePlaneGeometry plane_value,
	    ImageFrameGeometryKind kind_value = ImageFrameGeometryKind::regular_plane)
	    : plane(std::move(plane_value)), kind(kind_value) {}

	ImagePlaneGeometry plane;
	ImageFrameGeometryKind kind{ImageFrameGeometryKind::regular_plane};
};

enum class SliceStackStatus {
	ok,
	empty,
	missing_geometry,
	missing_frame_content,
	missing_dimension_module,
	unsupported_tiled_image,
	multiple_frame_stacks,
	geometry_parse_failure,
	missing_frame_of_reference,
	mixed_frame_of_reference,
	inconsistent_rows_columns,
	inconsistent_orientation,
	inconsistent_pixel_spacing,
	inconsistent_slice_origin,
	duplicate_slice_position,
	non_uniform_spacing,
};

struct SliceStackOptions {
	GeometryTolerance tolerance{};
	double slice_position_tolerance_mm{1e-3};
	double origin_residual_tolerance_mm{1e-3};
	bool allow_duplicate_positions{false};
};

struct ImageFrameStackOptions {
	SliceStackOptions slice_stack{};
	bool allow_geometry_grouping_fallback{false};
};

struct SliceStackInput {
	std::size_t source_index{0};
	std::size_t frame_index{0};
	ImagePlaneGeometry plane;
	std::string_view frame_of_reference_uid;
};

struct SliceStackSlice {
	std::size_t input_index{0};
	std::size_t source_index{0};
	std::size_t frame_index{0};
	ImagePlaneGeometry plane;
	double position_along_normal_mm{0.0};
	double in_plane_residual_mm{0.0};
};

struct SliceStackGap {
	std::size_t lower_sorted_index{0};
	std::size_t upper_sorted_index{0};
	double spacing_mm{0.0};
};

struct SliceStackRun {
	std::size_t begin_sorted_index{0};
	std::size_t end_sorted_index{0};
	double spacing_mm{0.0};
};

/// Placement record for a decoded source slice. Use `source_index` and
/// `frame_index` to find/decode the input frame, then place it at `target_k`
/// in the output volume buffer.
struct SliceStackItem {
	std::size_t source_index{0};
	std::size_t frame_index{0};
	std::size_t target_k{0};
	double position_along_normal_mm{0.0};
	double in_plane_residual_mm{0.0};
};

struct SliceStackIssue {
	SliceStackStatus status{SliceStackStatus::ok};
	std::size_t input_index{0};
	std::size_t source_index{0};
	std::size_t frame_index{0};
	Tag tag{};
	std::string message;
	ElementPath source{};
};

struct DimensionIndexDescriptor {
	Tag dimension_index_pointer{};
	Tag functional_group_pointer{};
	std::string dimension_organization_uid;
	std::string label;
	std::string private_creator;
};

struct DimensionIndexValue {
	DimensionIndexDescriptor descriptor;
	std::int64_t value{0};
};

struct ImageFrameStackKey {
	std::string stack_id;
	std::vector<DimensionIndexValue> dimension_values;
};

class ImageFrameStackAnalysis;

class SliceStackAnalysis {
public:
	[[nodiscard]] SliceStackStatus status() const noexcept { return status_; }
	[[nodiscard]] bool ok() const noexcept { return status_ == SliceStackStatus::ok; }
	[[nodiscard]] const std::string& frame_of_reference_uid() const noexcept {
		return frame_of_reference_uid_;
	}
	[[nodiscard]] const std::vector<SliceStackSlice>& slices() const noexcept {
		return slices_;
	}
	[[nodiscard]] const std::vector<SliceStackGap>& gaps() const noexcept {
		return gaps_;
	}
	[[nodiscard]] const std::vector<SliceStackRun>& uniform_runs() const noexcept {
		return uniform_runs_;
	}
	[[nodiscard]] const std::vector<SliceStackIssue>& issues() const noexcept {
		return issues_;
	}
	[[nodiscard]] std::optional<double> uniform_spacing_k() const noexcept {
		return uniform_spacing_k_;
	}
	[[nodiscard]] double max_in_plane_residual_mm() const noexcept {
		return max_in_plane_residual_mm_;
	}

private:
	friend SliceStackAnalysis analyze_slice_stack(
	    std::span<const SliceStackInput>, SliceStackOptions);
	friend SliceStackAnalysis analyze_slice_stack(
	    std::span<const DataSet* const>, SliceStackOptions);
	friend SliceStackAnalysis analyze_image_frame_stack(
	    const DicomFile&, std::span<const std::size_t>, ImageFrameStackOptions);
	friend SliceStackAnalysis analyze_image_frame_stack(
	    const DicomFile&, ImageFrameStackOptions);
	friend SliceStackAnalysis analyze_nm_frame_stack(
	    const DicomFile&, SliceStackOptions);

	SliceStackStatus status_{SliceStackStatus::empty};
	std::string frame_of_reference_uid_;
	std::vector<SliceStackSlice> slices_;
	std::vector<SliceStackGap> gaps_;
	std::vector<SliceStackRun> uniform_runs_;
	std::vector<SliceStackIssue> issues_;
	std::optional<double> uniform_spacing_k_{};
	double max_in_plane_residual_mm_{0.0};
};

struct ImageFrameStackGroup {
	ImageFrameStackKey key;
	std::vector<std::size_t> frame_indices;
	SliceStackAnalysis analysis;
};

class SliceStackPlan {
public:
	[[nodiscard]] SliceStackStatus status() const noexcept { return status_; }
	[[nodiscard]] bool ok() const noexcept { return status_ == SliceStackStatus::ok; }
	[[nodiscard]] const std::string& frame_of_reference_uid() const noexcept {
		return frame_of_reference_uid_;
	}
	[[nodiscard]] const std::optional<ImageVolumeGeometry>& volume_geometry()
	    const noexcept {
		return volume_geometry_;
	}
	[[nodiscard]] const std::vector<SliceStackItem>& placements() const noexcept {
		return placements_;
	}
	[[nodiscard]] const std::vector<SliceStackIssue>& issues() const noexcept {
		return issues_;
	}

private:
	friend SliceStackPlan plan_slice_stack(
	    std::span<const SliceStackInput>, SliceStackOptions);
	friend SliceStackPlan plan_slice_stack(
	    std::span<const DataSet* const>, SliceStackOptions);
	friend SliceStackPlan plan_image_frame_stack(
	    const DicomFile&, std::span<const std::size_t>, ImageFrameStackOptions);
	friend SliceStackPlan plan_image_frame_stack(
	    const DicomFile&, ImageFrameStackOptions);
	friend SliceStackPlan plan_nm_frame_stack(
	    const DicomFile&, SliceStackOptions);

	SliceStackStatus status_{SliceStackStatus::empty};
	std::string frame_of_reference_uid_;
	std::optional<ImageVolumeGeometry> volume_geometry_{};
	std::vector<SliceStackItem> placements_;
	std::vector<SliceStackIssue> issues_;
};

class ImageFrameStackAnalysis {
public:
	[[nodiscard]] SliceStackStatus status() const noexcept { return status_; }
	[[nodiscard]] bool ok() const noexcept { return status_ == SliceStackStatus::ok; }
	[[nodiscard]] const std::vector<ImageFrameStackGroup>& groups() const noexcept {
		return groups_;
	}
	[[nodiscard]] const std::vector<SliceStackIssue>& issues() const noexcept {
		return issues_;
	}

private:
	friend ImageFrameStackAnalysis analyze_image_frame_stacks(
	    const DicomFile&, ImageFrameStackOptions);

	SliceStackStatus status_{SliceStackStatus::empty};
	std::vector<ImageFrameStackGroup> groups_;
	std::vector<SliceStackIssue> issues_;
};

[[nodiscard]] double dot(Vec3d a, Vec3d b) noexcept;
[[nodiscard]] Vec3d cross(Vec3d a, Vec3d b) noexcept;
[[nodiscard]] double norm(Vec3d v) noexcept;
[[nodiscard]] Vec3d normalize(Vec3d v) noexcept;

[[nodiscard]] GeometryBuildResult<ImagePlaneGeometry> make_image_plane_geometry(
    const ImagePlaneGeometryParams& params,
    GeometryTolerance tolerance = {});
[[nodiscard]] GeometryBuildResult<ImageVolumeGeometry> make_image_volume_geometry(
    const ImageVolumeGeometryParams& params,
    GeometryTolerance tolerance = {});

[[nodiscard]] GeometryBuildResult<ImagePlaneGeometry> plane_from_single_frame_image(
    const DataSet& dataset);
[[nodiscard]] GeometryBuildResult<ImagePlaneGeometry> plane_from_single_frame_image(
    const DicomFile& file);

class FrameGeometryReader {
public:
	explicit FrameGeometryReader(const DataSet& dataset) noexcept;
	explicit FrameGeometryReader(const DicomFile& file) noexcept;

	/// Resolve one regular overlay frame plane using the enhanced image order:
	/// PerFrameFunctionalGroupsSequence -> SharedFunctionalGroupsSequence -> root dataset.
	/// Frames whose VolumetricProperties resolve to SAMPLED or DISTORTED fail with
	/// sampled_frame_geometry/distorted_frame_geometry; use image_frame_geometry()
	/// when those non-regular frame semantics need to be inspected.
	/// For many frames, construct one reader and reuse it instead of repeatedly
	/// calling `plane_from_multiframe_image()`.
	[[nodiscard]] GeometryBuildResult<ImagePlaneGeometry> plane(
	    std::size_t frame_index) const;

	/// Resolve plane geometry together with SOP-class volumetric semantics.
	/// `sampled_projection` and `distorted` are returned here for inspection,
	/// but `plane_from_multiframe_image()` rejects them for direct overlay use.
	[[nodiscard]] GeometryBuildResult<ImageFrameGeometry> image_frame_geometry(
	    std::size_t frame_index) const;

	[[nodiscard]] GeometryBuildResult<VolumetricPropertiesInfo> volumetric_properties(
	    std::size_t frame_index) const;

	[[nodiscard]] GeometryBuildResult<std::string> frame_of_reference() const;

private:
	[[nodiscard]] GeometryBuildResult<ImagePlaneGeometry> raw_plane(
	    std::size_t frame_index) const;

	const DataSet* root_{nullptr};
	const Sequence* per_frame_functional_groups_sequence_{nullptr};
	GeometryBuildStatus per_frame_functional_groups_status_{GeometryBuildStatus::ok};
	Tag per_frame_functional_groups_tag_{};
	const DataSet* shared_functional_groups_item_{nullptr};
	GeometryBuildStatus shared_functional_groups_status_{GeometryBuildStatus::ok};
	Tag shared_functional_groups_tag_{};
	Tag frame_type_sequence_tag_{};
	bool has_sop_class_uid_{false};
	bool unsupported_sop_class_{false};
	bool nm_image_storage_{false};
};

[[nodiscard]] GeometryBuildResult<ImagePlaneGeometry> plane_from_multiframe_image(
    const DataSet& dataset, std::size_t frame_index);
[[nodiscard]] GeometryBuildResult<ImagePlaneGeometry> plane_from_multiframe_image(
    const DicomFile& file, std::size_t frame_index);
[[nodiscard]] GeometryBuildResult<ImageFrameGeometry> frame_geometry_from_multiframe_image(
    const DataSet& dataset, std::size_t frame_index);
[[nodiscard]] GeometryBuildResult<ImageFrameGeometry> frame_geometry_from_multiframe_image(
    const DicomFile& file, std::size_t frame_index);
[[nodiscard]] GeometryBuildResult<VolumetricPropertiesInfo>
volumetric_properties_from_multiframe_image(
    const DataSet& dataset, std::size_t frame_index);
[[nodiscard]] GeometryBuildResult<VolumetricPropertiesInfo>
volumetric_properties_from_multiframe_image(
    const DicomFile& file, std::size_t frame_index);
[[nodiscard]] GeometryBuildResult<ImagePlaneGeometry> plane_from_seg_frame(
    const seg::Segmentation& segmentation, std::size_t frame_index);

[[nodiscard]] GeometryBuildResult<std::string> frame_of_reference_from_dataset(
    const DataSet& dataset);
[[nodiscard]] GeometryBuildResult<std::string> frame_of_reference_from_dataset(
    const DicomFile& file);
[[nodiscard]] GeometryBuildResult<std::string> frame_of_reference_from_segmentation(
    const seg::Segmentation& segmentation);

[[nodiscard]] SliceStackAnalysis analyze_slice_stack(
    std::span<const SliceStackInput> inputs,
    SliceStackOptions options = {});
[[nodiscard]] SliceStackPlan plan_slice_stack(
    std::span<const SliceStackInput> inputs,
    SliceStackOptions options = {});
[[nodiscard]] SliceStackAnalysis analyze_slice_stack(
    std::span<const DataSet* const> datasets,
    SliceStackOptions options = {});
[[nodiscard]] SliceStackPlan plan_slice_stack(
    std::span<const DataSet* const> datasets,
    SliceStackOptions options = {});
[[nodiscard]] SliceStackAnalysis analyze_image_frame_stack(
    const DicomFile& file,
    std::span<const std::size_t> frame_indices,
    ImageFrameStackOptions options = {});
[[nodiscard]] SliceStackPlan plan_image_frame_stack(
    const DicomFile& file,
    std::span<const std::size_t> frame_indices,
    ImageFrameStackOptions options = {});
[[nodiscard]] ImageFrameStackAnalysis analyze_image_frame_stacks(
    const DicomFile& file,
    ImageFrameStackOptions options = {});
[[nodiscard]] SliceStackAnalysis analyze_image_frame_stack(
    const DicomFile& file,
    ImageFrameStackOptions options = {});
[[nodiscard]] SliceStackPlan plan_image_frame_stack(
    const DicomFile& file,
    ImageFrameStackOptions options = {});
/// Analyze a Nuclear Medicine reconstructed TOMO stack using FrameIncrementPointer
/// with exactly one SliceVector. Projection TOMO/GATED TOMO and multi-vector NM
/// frame organizations are rejected because they are not regular reconstructed
/// slice planes in this MVP adapter.
[[nodiscard]] SliceStackAnalysis analyze_nm_frame_stack(
    const DicomFile& file,
    SliceStackOptions options = {});
[[nodiscard]] SliceStackPlan plan_nm_frame_stack(
    const DicomFile& file,
    SliceStackOptions options = {});

enum class OverlayCompatibility {
	compatible,
	missing_frame_of_reference,
	different_frame_of_reference,
	non_parallel_planes,
	opposite_orientation,
	out_of_plane,
	different_spacing,
	different_extent,
	requires_resampling,
};

struct OverlayCheckOptions {
	double frame_position_tolerance_mm{1e-3};
	double normal_distance_tolerance_mm{1e-3};
	double orientation_tolerance{1e-4};
	double spacing_tolerance_mm{1e-3};
	bool require_same_grid{false};
};

struct IndexRange1D {
	std::size_t begin{0};
	std::size_t end{0};

	[[nodiscard]] bool empty() const noexcept { return begin >= end; }
};

struct OverlayCheck {
	OverlayCompatibility status{OverlayCompatibility::different_frame_of_reference};
	bool same_frame_of_reference{false};
	bool can_transform{false};
	bool can_direct_overlay{false};
	bool same_grid{false};
	bool same_spacing{false};
	bool same_extent{false};
	bool overlaps_extent{false};
	bool source_inside_target_extent{false};
	bool requires_resampling{false};
	std::optional<IndexRange1D> target_k_range{};
	double max_position_error_mm{0.0};
	double max_normal_distance_mm{0.0};
	double max_orientation_error{0.0};
	double max_spacing_error_mm{0.0};

	[[nodiscard]] bool ok() const noexcept { return can_transform; }
};

/// O(1) preflight check over already-built geometry objects. This does not walk
/// DICOM datasets, allocate buffers, or build transform matrices.
[[nodiscard]] OverlayCheck check_overlay_compatibility(
    std::string_view source_frame_of_reference_uid,
    const ImagePlaneGeometry& source,
    std::string_view target_frame_of_reference_uid,
    const ImagePlaneGeometry& target,
    OverlayCheckOptions options = {});
[[nodiscard]] OverlayCheck check_overlay_compatibility(
    std::string_view source_frame_of_reference_uid,
    const ImagePlaneGeometry& source,
    std::string_view target_frame_of_reference_uid,
    const ImageVolumeGeometry& target,
    OverlayCheckOptions options = {});
[[nodiscard]] OverlayCheck check_overlay_compatibility(
    std::string_view source_frame_of_reference_uid,
    const ImageVolumeGeometry& source,
    std::string_view target_frame_of_reference_uid,
    const ImagePlaneGeometry& target,
    OverlayCheckOptions options = {});
[[nodiscard]] OverlayCheck check_overlay_compatibility(
    std::string_view source_frame_of_reference_uid,
    const ImageVolumeGeometry& source,
    std::string_view target_frame_of_reference_uid,
    const ImageVolumeGeometry& target,
    OverlayCheckOptions options = {});

class PlaneToPlaneTransform {
public:
	[[nodiscard]] ImagePoint2D target_index_from_source_index(
	    ImagePoint2D source) const noexcept;
	[[nodiscard]] ImagePoint2D source_index_from_target_index(
	    ImagePoint2D target) const noexcept;

private:
	friend PlaneToPlaneTransform make_plane_to_plane_transform(
	    const ImagePlaneGeometry&, const ImagePlaneGeometry&) noexcept;

	Matrix4x4d target_from_source_{};
	Matrix4x4d source_from_target_{};
};

class PlaneToVolumeTransform {
public:
	[[nodiscard]] ImagePoint3D target_index_from_source_index(
	    ImagePoint2D source) const noexcept;
	[[nodiscard]] ImagePoint2D source_index_from_target_index(
	    ImagePoint3D target) const noexcept;
	[[nodiscard]] PlaneProjection2D source_projection_from_target_index(
	    ImagePoint3D target) const noexcept;

private:
	friend PlaneToVolumeTransform make_plane_to_volume_transform(
	    const ImagePlaneGeometry&, const ImageVolumeGeometry&) noexcept;

	Matrix4x4d target_from_source_{};
	Matrix4x4d source_from_target_{};
};

class VolumeToPlaneTransform {
public:
	[[nodiscard]] ImagePoint2D target_index_from_source_index(
	    ImagePoint3D source) const noexcept;
	[[nodiscard]] PlaneProjection2D target_projection_from_source_index(
	    ImagePoint3D source) const noexcept;
	[[nodiscard]] ImagePoint3D source_index_from_target_index(
	    ImagePoint2D target) const noexcept;

private:
	friend VolumeToPlaneTransform make_volume_to_plane_transform(
	    const ImageVolumeGeometry&, const ImagePlaneGeometry&) noexcept;

	Matrix4x4d target_from_source_{};
	Matrix4x4d source_from_target_{};
};

class VolumeToVolumeTransform {
public:
	[[nodiscard]] ImagePoint3D target_index_from_source_index(
	    ImagePoint3D source) const noexcept;
	[[nodiscard]] ImagePoint3D source_index_from_target_index(
	    ImagePoint3D target) const noexcept;

private:
	friend VolumeToVolumeTransform make_volume_to_volume_transform(
	    const ImageVolumeGeometry&, const ImageVolumeGeometry&) noexcept;

	Matrix4x4d target_from_source_{};
	Matrix4x4d source_from_target_{};
};

[[nodiscard]] PlaneToPlaneTransform make_plane_to_plane_transform(
    const ImagePlaneGeometry& source,
    const ImagePlaneGeometry& target) noexcept;
[[nodiscard]] PlaneToVolumeTransform make_plane_to_volume_transform(
    const ImagePlaneGeometry& source,
    const ImageVolumeGeometry& target) noexcept;
[[nodiscard]] VolumeToPlaneTransform make_volume_to_plane_transform(
    const ImageVolumeGeometry& source,
    const ImagePlaneGeometry& target) noexcept;
[[nodiscard]] VolumeToVolumeTransform make_volume_to_volume_transform(
    const ImageVolumeGeometry& source,
    const ImageVolumeGeometry& target) noexcept;

} // namespace dicom::geometry
