#include "dicom_geometry.h"

#include "dicom_seg.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace dicom::geometry {
namespace {
using namespace dicom::literals;

constexpr double kEpsilon = 1e-12;

struct ResolvedCodeString {
	std::string value;
	ElementPath source;
};

[[nodiscard]] bool finite(double value) noexcept {
	return std::isfinite(value);
}

[[nodiscard]] bool finite(Vec3d value) noexcept {
	return finite(value.x) && finite(value.y) && finite(value.z);
}

[[nodiscard]] bool finite(Point3d value) noexcept {
	return finite(value.x) && finite(value.y) && finite(value.z);
}

[[nodiscard]] bool finite(const ImagePlaneGeometry& plane) noexcept {
	return finite(plane.origin()) && finite(plane.direction_i()) &&
	       finite(plane.direction_j()) && finite(plane.normal()) &&
	       finite(plane.spacing_i()) && finite(plane.spacing_j());
}

[[nodiscard]] Vec3d operator-(Point3d a, Point3d b) noexcept {
	return Vec3d{a.x - b.x, a.y - b.y, a.z - b.z};
}

[[nodiscard]] Point3d operator+(Point3d point, Vec3d vector) noexcept {
	return Point3d{point.x + vector.x, point.y + vector.y, point.z + vector.z};
}

[[nodiscard]] Vec3d operator*(Vec3d vector, double scale) noexcept {
	return Vec3d{vector.x * scale, vector.y * scale, vector.z * scale};
}

[[nodiscard]] ElementPath root_path(Tag tag) noexcept {
	return ElementPath{}.element(tag);
}

[[nodiscard]] std::string_view trim_ascii_spaces(std::string_view value) noexcept {
	while (!value.empty() && (value.front() == ' ' || value.front() == '\0')) {
		value.remove_prefix(1);
	}
	while (!value.empty() && (value.back() == ' ' || value.back() == '\0')) {
		value.remove_suffix(1);
	}
	return value;
}

[[nodiscard]] std::optional<std::string_view> root_string_view(
    const DataSet& dataset, Tag tag) {
	const auto& element = dataset.get_dataelement(root_path(tag));
	if (element.is_missing()) {
		return std::nullopt;
	}
	auto value = element.to_string_view();
	if (!value) {
		return std::nullopt;
	}
	return trim_ascii_spaces(*value);
}

[[nodiscard]] GeometryBuildResult<const DataSet*> resolve_sequence_item_if_present(
    const DataSet& dataset, Tag sequence_tag, std::uint32_t item_index) {
	const auto& element = dataset.get_dataelement(root_path(sequence_tag));
	if (element.is_missing()) {
		return GeometryBuildResult<const DataSet*>::success(nullptr);
	}
	if (!element.vr().is_sequence()) {
		return GeometryBuildResult<const DataSet*>::failure(
		    GeometryBuildStatus::invalid_value, sequence_tag,
		    "element is not a sequence");
	}
	const auto* sequence = element.as_sequence();
	if (!sequence) {
		return GeometryBuildResult<const DataSet*>::failure(
		    GeometryBuildStatus::invalid_value, sequence_tag,
		    "invalid sequence value");
	}
	const DataSet* item = sequence->get_dataset(item_index);
	if (!item) {
		return GeometryBuildResult<const DataSet*>::failure(
		    GeometryBuildStatus::invalid_value, sequence_tag,
		    "sequence item is missing");
	}
	return GeometryBuildResult<const DataSet*>::success(item);
}

[[nodiscard]] std::optional<GeometryBuildResult<const DataSet*>>
try_resolve_functional_group_macro_item(
    const DataSet* functional_group_item, Tag macro_sequence_tag) {
	if (!functional_group_item) {
		return std::nullopt;
	}
	return resolve_sequence_item_if_present(
	    *functional_group_item, macro_sequence_tag, 0);
}

[[nodiscard]] Tag frame_type_sequence_tag_for_sop_class(
    std::string_view sop_class_uid) noexcept {
	if (sop_class_uid == "EnhancedMRImageStorage"_uid.value() ||
	    sop_class_uid == "LegacyConvertedEnhancedMRImageStorage"_uid.value()) {
		return "MRImageFrameTypeSequence"_tag;
	}
	if (sop_class_uid == "EnhancedCTImageStorage"_uid.value() ||
	    sop_class_uid == "LegacyConvertedEnhancedCTImageStorage"_uid.value()) {
		return "CTImageFrameTypeSequence"_tag;
	}
	if (sop_class_uid == "EnhancedPETImageStorage"_uid.value() ||
	    sop_class_uid == "LegacyConvertedEnhancedPETImageStorage"_uid.value()) {
		return "PETFrameTypeSequence"_tag;
	}
	return Tag{};
}

[[nodiscard]] bool is_nm_image_storage(std::string_view sop_class_uid) noexcept {
	return sop_class_uid == "NuclearMedicineImageStorage"_uid.value();
}

[[nodiscard]] std::optional<GeometryBuildResult<ResolvedCodeString>>
try_read_code_string_from_functional_group(const DataSet* functional_group_item,
    Tag functional_group_sequence_tag, std::uint32_t functional_group_item_index,
    Tag macro_sequence_tag, Tag leaf_tag) {
	if (!functional_group_item) {
		return std::nullopt;
	}
	auto macro_item_result =
	    try_resolve_functional_group_macro_item(
	        functional_group_item, macro_sequence_tag);
	if (!macro_item_result) {
		return std::nullopt;
	}
	if (!macro_item_result->ok()) {
		return GeometryBuildResult<ResolvedCodeString>::failure(
		    macro_item_result->status(), macro_item_result->tag(),
		    macro_item_result->message());
	}
	const DataSet* macro_item = macro_item_result->value();
	if (!macro_item) {
		return std::nullopt;
	}
	const auto& element = macro_item->get_dataelement(root_path(leaf_tag));
	if (element.is_missing()) {
		return std::nullopt;
	}
	auto value = element.to_string_view();
	if (!value) {
		return GeometryBuildResult<ResolvedCodeString>::failure(
		    GeometryBuildStatus::invalid_value, leaf_tag, "invalid code string");
	}
	ElementPath source;
	source.item(functional_group_sequence_tag, functional_group_item_index)
	    .item(macro_sequence_tag, 0)
	    .element(leaf_tag);
	return GeometryBuildResult<ResolvedCodeString>::success(
	    ResolvedCodeString{std::string(trim_ascii_spaces(*value)), source});
}

[[nodiscard]] std::optional<GeometryBuildResult<ResolvedCodeString>>
try_read_root_code_string(const DataSet& root, Tag leaf_tag) {
	const auto source = root_path(leaf_tag);
	const auto& element = root.get_dataelement(source);
	if (element.is_missing()) {
		return std::nullopt;
	}
	auto value = element.to_string_view();
	if (!value) {
		return GeometryBuildResult<ResolvedCodeString>::failure(
		    GeometryBuildStatus::invalid_value, leaf_tag, "invalid code string");
	}
	return GeometryBuildResult<ResolvedCodeString>::success(
	    ResolvedCodeString{std::string(trim_ascii_spaces(*value)), source});
}

[[nodiscard]] GeometryBuildResult<VolumetricPropertiesInfo>
volumetric_properties_from_code_string(ResolvedCodeString resolved) {
	const auto value = std::string_view(resolved.value);
	if (value == "VOLUME") {
		return GeometryBuildResult<VolumetricPropertiesInfo>::success(
		    VolumetricPropertiesInfo{VolumetricPropertiesValue::volume,
		        std::move(resolved.source)});
	}
	if (value == "SAMPLED") {
		return GeometryBuildResult<VolumetricPropertiesInfo>::success(
		    VolumetricPropertiesInfo{VolumetricPropertiesValue::sampled,
		        std::move(resolved.source)});
	}
	if (value == "DISTORTED") {
		return GeometryBuildResult<VolumetricPropertiesInfo>::success(
		    VolumetricPropertiesInfo{VolumetricPropertiesValue::distorted,
		        std::move(resolved.source)});
	}
	if (value == "MIXED") {
		return GeometryBuildResult<VolumetricPropertiesInfo>::failure(
		    GeometryBuildStatus::mixed_volumetric_properties,
		    "VolumetricProperties"_tag,
		    "VolumetricProperties is MIXED without a frame-level value");
	}
	return GeometryBuildResult<VolumetricPropertiesInfo>::failure(
	    GeometryBuildStatus::unknown_volumetric_properties,
	    "VolumetricProperties"_tag,
	    "unknown VolumetricProperties value");
}

[[nodiscard]] ImageFrameGeometryKind kind_from_volumetric_properties(
    VolumetricPropertiesValue value) noexcept {
	switch (value) {
	case VolumetricPropertiesValue::volume:
		return ImageFrameGeometryKind::regular_plane;
	case VolumetricPropertiesValue::sampled:
		return ImageFrameGeometryKind::sampled_projection;
	case VolumetricPropertiesValue::distorted:
		return ImageFrameGeometryKind::distorted;
	}
	return ImageFrameGeometryKind::distorted;
}

[[nodiscard]] GeometryBuildResult<std::vector<double>> read_double_vector(
    const DataSet& dataset, Tag tag, std::size_t expected_count) {
	const auto& element = dataset.get_dataelement(root_path(tag));
	if (element.is_missing()) {
		return GeometryBuildResult<std::vector<double>>::failure(
		    GeometryBuildStatus::missing_required_tag, tag, "missing required tag");
	}
	auto values = element.to_double_vector();
	if (!values || values->size() != expected_count) {
		return GeometryBuildResult<std::vector<double>>::failure(
		    GeometryBuildStatus::invalid_value, tag, "invalid value count");
	}
	return GeometryBuildResult<std::vector<double>>::success(std::move(*values));
}

[[nodiscard]] GeometryBuildResult<int> read_positive_int(
    const DataSet& dataset, Tag tag) {
	const auto& element = dataset.get_dataelement(root_path(tag));
	if (element.is_missing()) {
		return GeometryBuildResult<int>::failure(
		    GeometryBuildStatus::missing_required_tag, tag, "missing required tag");
	}
	auto value = element.to_int();
	if (!value || *value <= 0) {
		return GeometryBuildResult<int>::failure(
		    GeometryBuildStatus::invalid_size, tag, "value must be positive");
	}
	return GeometryBuildResult<int>::success(*value);
}

[[nodiscard]] std::string make_tag_message(std::string_view prefix, Tag tag) {
	std::string message(prefix);
	message += " ";
	message += tag.to_string();
	return message;
}

[[nodiscard]] int slice_stack_status_priority(SliceStackStatus status) noexcept {
	switch (status) {
	case SliceStackStatus::ok:
		return 100;
	case SliceStackStatus::empty:
		return 0;
	case SliceStackStatus::missing_geometry:
		return 1;
	case SliceStackStatus::missing_frame_content:
		return 2;
	case SliceStackStatus::missing_dimension_module:
		return 3;
	case SliceStackStatus::unsupported_tiled_image:
		return 4;
	case SliceStackStatus::geometry_parse_failure:
		return 5;
	case SliceStackStatus::missing_frame_of_reference:
		return 6;
	case SliceStackStatus::mixed_frame_of_reference:
		return 7;
	case SliceStackStatus::inconsistent_rows_columns:
		return 8;
	case SliceStackStatus::inconsistent_orientation:
		return 9;
	case SliceStackStatus::inconsistent_pixel_spacing:
		return 10;
	case SliceStackStatus::inconsistent_slice_origin:
		return 11;
	case SliceStackStatus::duplicate_slice_position:
		return 12;
	case SliceStackStatus::non_uniform_spacing:
		return 13;
	case SliceStackStatus::multiple_frame_stacks:
		return 14;
	}
	return 100;
}

[[nodiscard]] SliceStackStatus slice_stack_status_from_geometry_build(
    GeometryBuildStatus status) noexcept {
	switch (status) {
	case GeometryBuildStatus::missing_required_tag:
		return SliceStackStatus::missing_geometry;
	case GeometryBuildStatus::invalid_size:
		return SliceStackStatus::inconsistent_rows_columns;
	case GeometryBuildStatus::invalid_spacing:
		return SliceStackStatus::inconsistent_pixel_spacing;
	case GeometryBuildStatus::invalid_orientation:
		return SliceStackStatus::inconsistent_orientation;
	default:
		return SliceStackStatus::geometry_parse_failure;
	}
}

struct SliceStackInputStore {
	std::vector<SliceStackInput> inputs;
	std::vector<std::string> frame_of_reference_uids;
	std::vector<SliceStackIssue> issues;
	SliceStackStatus status{SliceStackStatus::ok};
};

struct FrameStackMember {
	std::size_t frame_index{0};
	int in_stack_position{0};
};

struct FrameStackBucket {
	ImageFrameStackKey key;
	std::vector<FrameStackMember> members;
};

[[nodiscard]] bool same_descriptor(
    const DimensionIndexDescriptor& a,
    const DimensionIndexDescriptor& b) noexcept {
	return a.dimension_index_pointer == b.dimension_index_pointer &&
	       a.functional_group_pointer == b.functional_group_pointer &&
	       a.dimension_organization_uid == b.dimension_organization_uid &&
	       a.label == b.label && a.private_creator == b.private_creator;
}

[[nodiscard]] bool same_dimension_values(
    const std::vector<DimensionIndexValue>& a,
    const std::vector<DimensionIndexValue>& b) noexcept {
	if (a.size() != b.size()) {
		return false;
	}
	for (std::size_t index = 0; index < a.size(); ++index) {
		if (a[index].value != b[index].value ||
		    !same_descriptor(a[index].descriptor, b[index].descriptor)) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] bool same_stack_key(
    const ImageFrameStackKey& a, const ImageFrameStackKey& b) noexcept {
	return a.stack_id == b.stack_id &&
	       same_dimension_values(a.dimension_values, b.dimension_values);
}

void hash_combine(std::size_t& seed, std::size_t value) noexcept {
	seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
}

struct ImageFrameStackKeyHash {
	[[nodiscard]] std::size_t operator()(const ImageFrameStackKey& key) const noexcept {
		std::size_t seed = std::hash<std::string>{}(key.stack_id);
		for (const auto& value : key.dimension_values) {
			hash_combine(seed,
			    std::hash<std::int64_t>{}(value.value));
			hash_combine(seed,
			    std::hash<std::uint32_t>{}(
			        value.descriptor.dimension_index_pointer.value()));
			hash_combine(seed,
			    std::hash<std::uint32_t>{}(
			        value.descriptor.functional_group_pointer.value()));
			hash_combine(seed,
			    std::hash<std::string>{}(
			        value.descriptor.dimension_organization_uid));
			hash_combine(seed,
			    std::hash<std::string>{}(value.descriptor.label));
			hash_combine(seed,
			    std::hash<std::string>{}(value.descriptor.private_creator));
		}
		return seed;
	}
};

struct ImageFrameStackKeyEqual {
	[[nodiscard]] bool operator()(const ImageFrameStackKey& a,
	    const ImageFrameStackKey& b) const noexcept {
		return same_stack_key(a, b);
	}
};

[[nodiscard]] bool is_spatial_dimension(
    const DimensionIndexDescriptor& descriptor) noexcept {
	return descriptor.dimension_index_pointer == "InStackPositionNumber"_tag ||
	       descriptor.dimension_index_pointer == "ImagePositionPatient"_tag ||
	       descriptor.functional_group_pointer == "PlanePositionSequence"_tag;
}

[[nodiscard]] GeometryBuildResult<std::vector<DimensionIndexDescriptor>>
read_dimension_index_descriptors(const DataSet& root) {
	const auto tag = "DimensionIndexSequence"_tag;
	const auto& element = root.get_dataelement(root_path(tag));
	if (element.is_missing()) {
		return GeometryBuildResult<std::vector<DimensionIndexDescriptor>>::success(
		    {});
	}
	if (!element.vr().is_sequence()) {
		return GeometryBuildResult<std::vector<DimensionIndexDescriptor>>::failure(
		    GeometryBuildStatus::invalid_value, tag,
		    "DimensionIndexSequence is not a sequence");
	}
	const auto* sequence = element.as_sequence();
	if (!sequence) {
		return GeometryBuildResult<std::vector<DimensionIndexDescriptor>>::failure(
		    GeometryBuildStatus::invalid_value, tag,
		    "invalid DimensionIndexSequence");
	}

	std::vector<DimensionIndexDescriptor> descriptors;
	descriptors.reserve(static_cast<std::size_t>(std::max(0, sequence->size())));
	for (const auto& item : *sequence) {
		if (!item) {
			return GeometryBuildResult<std::vector<DimensionIndexDescriptor>>::failure(
			    GeometryBuildStatus::invalid_value, tag,
			    "DimensionIndexSequence contains a null item");
		}
		const auto pointer =
		    item->get_dataelement(root_path("DimensionIndexPointer"_tag)).to_tag();
		if (!pointer || !*pointer) {
			return GeometryBuildResult<std::vector<DimensionIndexDescriptor>>::failure(
			    GeometryBuildStatus::invalid_value, "DimensionIndexPointer"_tag,
			    "missing or invalid DimensionIndexPointer");
		}
		DimensionIndexDescriptor descriptor;
		descriptor.dimension_index_pointer = *pointer;
		descriptor.functional_group_pointer =
		    item->get_dataelement(root_path("FunctionalGroupPointer"_tag))
		        .to_tag(Tag{});
		if (auto value =
		        root_string_view(*item, "DimensionOrganizationUID"_tag)) {
			descriptor.dimension_organization_uid = std::string(*value);
		}
		if (auto value =
		        root_string_view(*item, "DimensionDescriptionLabel"_tag)) {
			descriptor.label = std::string(*value);
		}
		if (auto value =
		        root_string_view(*item, "DimensionIndexPrivateCreator"_tag)) {
			descriptor.private_creator = std::string(*value);
		}
		descriptors.push_back(std::move(descriptor));
	}
	return GeometryBuildResult<std::vector<DimensionIndexDescriptor>>::success(
	    std::move(descriptors));
}

[[nodiscard]] GeometryBuildResult<std::vector<DimensionIndexValue>>
read_non_spatial_dimension_values(const DataSet& frame_content,
    const std::vector<DimensionIndexDescriptor>& descriptors) {
	if (descriptors.empty()) {
		return GeometryBuildResult<std::vector<DimensionIndexValue>>::success({});
	}
	const auto tag = "DimensionIndexValues"_tag;
	const auto& element = frame_content.get_dataelement(root_path(tag));
	if (element.is_missing()) {
		return GeometryBuildResult<std::vector<DimensionIndexValue>>::failure(
		    GeometryBuildStatus::missing_required_tag, tag,
		    "missing DimensionIndexValues");
	}
	auto values = element.to_longlong_vector();
	if (!values || values->size() != descriptors.size()) {
		return GeometryBuildResult<std::vector<DimensionIndexValue>>::failure(
		    GeometryBuildStatus::invalid_value, tag,
		    "DimensionIndexValues count does not match DimensionIndexSequence");
	}

	std::vector<DimensionIndexValue> dimension_values;
	dimension_values.reserve(descriptors.size());
	for (std::size_t index = 0; index < descriptors.size(); ++index) {
		const auto& descriptor = descriptors[index];
		if (is_spatial_dimension(descriptor)) {
			continue;
		}
		dimension_values.push_back(DimensionIndexValue{
		    descriptor,
		    static_cast<std::int64_t>((*values)[index]),
		});
	}
	return GeometryBuildResult<std::vector<DimensionIndexValue>>::success(
	    std::move(dimension_values));
}

void add_slice_stack_store_issue(SliceStackInputStore& store,
    SliceStackStatus status, std::size_t input_index, std::size_t source_index,
    std::size_t frame_index, Tag tag, std::string message) {
	store.issues.push_back(SliceStackIssue{
	    status,
	    input_index,
	    source_index,
	    frame_index,
	    tag,
	    std::move(message),
	});
	if (slice_stack_status_priority(status) <
	    slice_stack_status_priority(store.status)) {
		store.status = status;
	}
}

[[nodiscard]] SliceStackInputStore make_slice_stack_inputs_from_datasets(
    std::span<const DataSet* const> datasets) {
	SliceStackInputStore store;
	store.inputs.reserve(datasets.size());
	store.frame_of_reference_uids.reserve(datasets.size());
	for (std::size_t index = 0; index < datasets.size(); ++index) {
		const DataSet* dataset = datasets[index];
		if (!dataset) {
			add_slice_stack_store_issue(store, SliceStackStatus::missing_geometry,
			    index, index, 0, Tag{}, "missing DataSet pointer");
			continue;
		}

		auto plane = plane_from_single_frame_image(*dataset);
		if (!plane.ok()) {
			add_slice_stack_store_issue(store,
			    slice_stack_status_from_geometry_build(plane.status()), index,
			    index, 0, plane.tag(), plane.message());
			continue;
		}

		std::string_view frame_of_reference_uid;
		auto frame_of_reference = frame_of_reference_from_dataset(*dataset);
		if (frame_of_reference.ok()) {
			store.frame_of_reference_uids.push_back(
			    std::move(frame_of_reference).value());
			frame_of_reference_uid = store.frame_of_reference_uids.back();
		}

		store.inputs.push_back(SliceStackInput{
		    index,
		    0,
		    std::move(plane).value(),
		    frame_of_reference_uid,
		});
	}
	return store;
}

[[nodiscard]] SliceStackInputStore make_slice_stack_inputs_from_image_frames(
    const DicomFile& file, std::span<const std::size_t> frame_indices) {
	SliceStackInputStore store;
	store.inputs.reserve(frame_indices.size());
	store.frame_of_reference_uids.reserve(1);

	FrameGeometryReader reader(file);
	std::string_view frame_of_reference_uid;
	auto frame_of_reference = reader.frame_of_reference();
	if (frame_of_reference.ok()) {
		store.frame_of_reference_uids.push_back(
		    std::move(frame_of_reference).value());
		frame_of_reference_uid = store.frame_of_reference_uids.back();
	}

	for (std::size_t input_index = 0; input_index < frame_indices.size();
	     ++input_index) {
		const auto frame_index = frame_indices[input_index];
		auto frame_geometry = reader.image_frame_geometry(frame_index);
		if (!frame_geometry.ok()) {
			add_slice_stack_store_issue(store,
			    slice_stack_status_from_geometry_build(frame_geometry.status()),
			    input_index, 0, frame_index, frame_geometry.tag(),
			    frame_geometry.message());
			continue;
		}
		if (frame_geometry.value().kind != ImageFrameGeometryKind::regular_plane) {
			add_slice_stack_store_issue(store,
			    SliceStackStatus::geometry_parse_failure, input_index, 0,
			    frame_index, "VolumetricProperties"_tag,
			    "image frame is not a regular plane");
			continue;
		}

		auto frame = std::move(frame_geometry).value();
		store.inputs.push_back(SliceStackInput{
		    0,
		    frame_index,
		    std::move(frame.plane),
		    frame_of_reference_uid,
		});
	}
	return store;
}

[[nodiscard]] std::vector<SliceStackInput> make_slice_stack_inputs_from_analysis(
    const SliceStackAnalysis& analysis) {
	std::vector<SliceStackInput> inputs;
	inputs.reserve(analysis.slices().size());
	const auto frame_of_reference_uid =
	    std::string_view(analysis.frame_of_reference_uid());
	for (const auto& slice : analysis.slices()) {
		inputs.push_back(SliceStackInput{
		    slice.source_index,
		    slice.frame_index,
		    slice.plane,
		    frame_of_reference_uid,
		});
	}
	return inputs;
}

[[nodiscard]] GeometryBuildResult<ImagePlaneGeometry> forward_plane_failure(
    GeometryBuildStatus status, Tag tag, std::string_view message) {
	return GeometryBuildResult<ImagePlaneGeometry>::failure(
	    status, tag, std::string(message));
}

[[nodiscard]] GeometryBuildResult<ImagePlaneGeometry> forward_vector_failure(
    const GeometryBuildResult<std::vector<double>>& result) {
	return forward_plane_failure(result.status(), result.tag(), result.message());
}

[[nodiscard]] GeometryBuildResult<ImagePlaneGeometry> forward_int_failure(
    const GeometryBuildResult<int>& result) {
	return forward_plane_failure(result.status(), result.tag(), result.message());
}

[[nodiscard]] GeometryBuildResult<ImagePlaneGeometry> plane_from_dicom_values(
    std::size_t rows, std::size_t columns, const std::vector<double>& position,
    const std::vector<double>& orientation, const std::vector<double>& spacing) {
	ImagePlaneGeometryParams params;
	params.origin = Point3d{position[0], position[1], position[2]};
	// DICOM calls orientation[0..2] the row direction cosine. With DicomSDL's
	// index convention, that is the direction of increasing column index i.
	params.direction_i = Vec3d{orientation[0], orientation[1], orientation[2]};
	// DICOM calls orientation[3..5] the column direction cosine; here it is the
	// direction of increasing row index j.
	params.direction_j = Vec3d{orientation[3], orientation[4], orientation[5]};
	// DICOM PixelSpacing is row spacing then column spacing.
	params.spacing = ImageSpacing2D{spacing[1], spacing[0]};
	params.size = ImageSize2D{
	    columns,
	    rows,
	};

	auto result = make_image_plane_geometry(params);
	if (!result.ok()) {
		Tag failure_tag{};
		switch (result.status()) {
		case GeometryBuildStatus::invalid_spacing:
			failure_tag = "PixelSpacing"_tag;
			break;
		case GeometryBuildStatus::invalid_orientation:
			failure_tag = "ImageOrientationPatient"_tag;
			break;
		case GeometryBuildStatus::invalid_size:
			failure_tag = "Rows"_tag;
			break;
		default:
			break;
		}
		return GeometryBuildResult<ImagePlaneGeometry>::failure(
		    result.status(), failure_tag, result.message());
	}
	return result;
}

[[nodiscard]] std::optional<GeometryBuildResult<std::vector<double>>>
try_read_double_vector_from_functional_group(const DataSet* functional_group_item,
    Tag macro_sequence_tag, Tag leaf_tag, std::size_t expected_count) {
	if (!functional_group_item) {
		return std::nullopt;
	}
	auto macro_item_result =
	    try_resolve_functional_group_macro_item(
	        functional_group_item, macro_sequence_tag);
	if (!macro_item_result) {
		return std::nullopt;
	}
	if (!macro_item_result->ok()) {
		return GeometryBuildResult<std::vector<double>>::failure(
		    macro_item_result->status(), macro_item_result->tag(),
		    macro_item_result->message());
	}
	const DataSet* macro_item = macro_item_result->value();
	if (!macro_item) {
		return std::nullopt;
	}
	const auto& element = macro_item->get_dataelement(root_path(leaf_tag));
	if (element.is_missing()) {
		return std::nullopt;
	}
	auto values = element.to_double_vector();
	if (!values || values->size() != expected_count) {
		return GeometryBuildResult<std::vector<double>>::failure(
		    GeometryBuildStatus::invalid_value, leaf_tag, "invalid value count");
	}
	return GeometryBuildResult<std::vector<double>>::success(std::move(*values));
}

[[nodiscard]] GeometryBuildResult<std::vector<double>> resolve_frame_double_vector(
    const DataSet& root, const DataSet* per_frame_functional_group_item,
    const DataSet* shared_functional_group_item, Tag macro_sequence_tag,
    Tag leaf_tag, Tag root_tag, std::size_t expected_count) {
	if (auto result = try_read_double_vector_from_functional_group(
	        per_frame_functional_group_item, macro_sequence_tag, leaf_tag,
	        expected_count)) {
		return std::move(*result);
	}
	if (auto result = try_read_double_vector_from_functional_group(
	        shared_functional_group_item, macro_sequence_tag, leaf_tag,
	        expected_count)) {
		return std::move(*result);
	}
	return read_double_vector(root, root_tag, expected_count);
}

[[nodiscard]] GeometryBuildResult<std::vector<double>>
resolve_strict_frame_double_vector(
    const DataSet* per_frame_functional_group_item,
    const DataSet* shared_functional_group_item, Tag macro_sequence_tag,
    Tag leaf_tag, std::size_t expected_count) {
	if (auto result = try_read_double_vector_from_functional_group(
	        per_frame_functional_group_item, macro_sequence_tag, leaf_tag,
	        expected_count)) {
		return std::move(*result);
	}
	if (auto result = try_read_double_vector_from_functional_group(
	        shared_functional_group_item, macro_sequence_tag, leaf_tag,
	        expected_count)) {
		return std::move(*result);
	}
	return GeometryBuildResult<std::vector<double>>::failure(
	    GeometryBuildStatus::missing_required_tag, leaf_tag, "missing required tag");
}

[[nodiscard]] GeometryBuildResult<const Sequence*> resolve_per_frame_sequence(
    const DataSet& root) {
	const auto tag = "PerFrameFunctionalGroupsSequence"_tag;
	const auto& element = root.get_dataelement(root_path(tag));
	if (element.is_missing()) {
		return GeometryBuildResult<const Sequence*>::success(nullptr);
	}
	if (!element.vr().is_sequence()) {
		return GeometryBuildResult<const Sequence*>::failure(
		    GeometryBuildStatus::invalid_value, tag);
	}
	const auto* sequence = element.as_sequence();
	if (!sequence) {
		return GeometryBuildResult<const Sequence*>::failure(
		    GeometryBuildStatus::invalid_value, tag);
	}
	return GeometryBuildResult<const Sequence*>::success(sequence);
}

[[nodiscard]] GeometryBuildResult<const DataSet*> resolve_per_frame_item(
    const Sequence* per_frame_sequence, std::size_t frame_index) {
	if (!per_frame_sequence) {
		return GeometryBuildResult<const DataSet*>::success(nullptr);
	}
	const auto tag = "PerFrameFunctionalGroupsSequence"_tag;
	const DataSet* item = per_frame_sequence->get_dataset(frame_index);
	if (!item) {
		return GeometryBuildResult<const DataSet*>::failure(
		    GeometryBuildStatus::invalid_frame_index, tag,
		    "frame index is outside PerFrameFunctionalGroupsSequence");
	}
	return GeometryBuildResult<const DataSet*>::success(item);
}

[[nodiscard]] std::size_t per_frame_sequence_size(const DataSet& root) {
	auto sequence = resolve_per_frame_sequence(root);
	if (!sequence.ok() || !sequence.value()) {
		return 0;
	}
	return static_cast<std::size_t>(std::max(0, sequence.value()->size()));
}

[[nodiscard]] std::optional<std::size_t> number_of_frames(const DataSet& root) {
	const auto& element =
	    root.get_dataelement(root_path("NumberOfFrames"_tag));
	if (!element.is_missing()) {
		auto value = element.to_int();
		if (value && *value > 0) {
			return static_cast<std::size_t>(*value);
		}
		return std::nullopt;
	}
	const auto fallback = per_frame_sequence_size(root);
	if (fallback > 0) {
		return fallback;
	}
	return std::nullopt;
}

[[nodiscard]] std::optional<Tag> tiled_multiframe_tag(const DataSet& root) {
	if (auto value = root_string_view(root, "DimensionOrganizationType"_tag)) {
		if (*value == "TILED_FULL" || *value == "TILED_SPARSE") {
			return "DimensionOrganizationType"_tag;
		}
	}
	for (const Tag tag : {"TotalPixelMatrixRows"_tag, "TotalPixelMatrixColumns"_tag,
	         "TotalPixelMatrixOriginSequence"_tag, "TotalPixelMatrixFocalPlanes"_tag}) {
		if (!root.get_dataelement(root_path(tag)).is_missing()) {
			return tag;
		}
	}
	return std::nullopt;
}

[[nodiscard]] GeometryBuildResult<int> validate_root_only_frame_index(
    const DataSet& root, std::size_t frame_index) {
	const auto tag = "NumberOfFrames"_tag;
	const auto& element = root.get_dataelement(root_path(tag));
	if (element.is_missing()) {
		if (frame_index == 0) {
			return GeometryBuildResult<int>::success(1);
		}
		return GeometryBuildResult<int>::failure(
		    GeometryBuildStatus::invalid_frame_index, tag,
		    "frame index is outside root dataset frame metadata");
	}
	auto value = element.to_int();
	if (!value || *value <= 0) {
		return GeometryBuildResult<int>::failure(
		    GeometryBuildStatus::invalid_value, tag, "invalid NumberOfFrames");
	}
	if (frame_index >= static_cast<std::size_t>(*value)) {
		return GeometryBuildResult<int>::failure(
		    GeometryBuildStatus::invalid_frame_index, tag,
		    "frame index is outside NumberOfFrames");
	}
	return GeometryBuildResult<int>::success(*value);
}

[[nodiscard]] Matrix4x4d make_plane_index_to_world(
    Point3d origin, Vec3d direction_i, Vec3d direction_j, Vec3d normal,
    ImageSpacing2D spacing) noexcept {
	auto matrix = Matrix4x4d::identity();
	matrix(0, 0) = direction_i.x * spacing.i;
	matrix(1, 0) = direction_i.y * spacing.i;
	matrix(2, 0) = direction_i.z * spacing.i;
	matrix(0, 1) = direction_j.x * spacing.j;
	matrix(1, 1) = direction_j.y * spacing.j;
	matrix(2, 1) = direction_j.z * spacing.j;
	matrix(0, 2) = normal.x;
	matrix(1, 2) = normal.y;
	matrix(2, 2) = normal.z;
	matrix(0, 3) = origin.x;
	matrix(1, 3) = origin.y;
	matrix(2, 3) = origin.z;
	return matrix;
}

[[nodiscard]] Matrix4x4d make_plane_world_to_index(
    Point3d origin, Vec3d direction_i, Vec3d direction_j, Vec3d normal,
    ImageSpacing2D spacing) noexcept {
	auto matrix = Matrix4x4d::identity();
	matrix(0, 0) = direction_i.x / spacing.i;
	matrix(0, 1) = direction_i.y / spacing.i;
	matrix(0, 2) = direction_i.z / spacing.i;
	matrix(0, 3) = -dot(Vec3d{origin.x, origin.y, origin.z}, direction_i) / spacing.i;
	matrix(1, 0) = direction_j.x / spacing.j;
	matrix(1, 1) = direction_j.y / spacing.j;
	matrix(1, 2) = direction_j.z / spacing.j;
	matrix(1, 3) = -dot(Vec3d{origin.x, origin.y, origin.z}, direction_j) / spacing.j;
	matrix(2, 0) = normal.x;
	matrix(2, 1) = normal.y;
	matrix(2, 2) = normal.z;
	matrix(2, 3) = -dot(Vec3d{origin.x, origin.y, origin.z}, normal);
	return matrix;
}

[[nodiscard]] Matrix4x4d make_volume_index_to_world(
    Point3d origin, Vec3d direction_i, Vec3d direction_j, Vec3d direction_k,
    ImageSpacing3D spacing) noexcept {
	auto matrix = Matrix4x4d::identity();
	matrix(0, 0) = direction_i.x * spacing.i;
	matrix(1, 0) = direction_i.y * spacing.i;
	matrix(2, 0) = direction_i.z * spacing.i;
	matrix(0, 1) = direction_j.x * spacing.j;
	matrix(1, 1) = direction_j.y * spacing.j;
	matrix(2, 1) = direction_j.z * spacing.j;
	matrix(0, 2) = direction_k.x * spacing.k;
	matrix(1, 2) = direction_k.y * spacing.k;
	matrix(2, 2) = direction_k.z * spacing.k;
	matrix(0, 3) = origin.x;
	matrix(1, 3) = origin.y;
	matrix(2, 3) = origin.z;
	return matrix;
}

[[nodiscard]] Matrix4x4d make_volume_world_to_index(
    Point3d origin, Vec3d direction_i, Vec3d direction_j, Vec3d direction_k,
    ImageSpacing3D spacing) noexcept {
	auto matrix = Matrix4x4d::identity();
	const auto origin_vector = Vec3d{origin.x, origin.y, origin.z};
	matrix(0, 0) = direction_i.x / spacing.i;
	matrix(0, 1) = direction_i.y / spacing.i;
	matrix(0, 2) = direction_i.z / spacing.i;
	matrix(0, 3) = -dot(origin_vector, direction_i) / spacing.i;
	matrix(1, 0) = direction_j.x / spacing.j;
	matrix(1, 1) = direction_j.y / spacing.j;
	matrix(1, 2) = direction_j.z / spacing.j;
	matrix(1, 3) = -dot(origin_vector, direction_j) / spacing.j;
	matrix(2, 0) = direction_k.x / spacing.k;
	matrix(2, 1) = direction_k.y / spacing.k;
	matrix(2, 2) = direction_k.z / spacing.k;
	matrix(2, 3) = -dot(origin_vector, direction_k) / spacing.k;
	return matrix;
}

[[nodiscard]] Matrix4x4d multiply(
    const Matrix4x4d& left, const Matrix4x4d& right) noexcept {
	Matrix4x4d out;
	for (std::size_t row = 0; row < 4; ++row) {
		for (std::size_t col = 0; col < 4; ++col) {
			double value = 0.0;
			for (std::size_t k = 0; k < 4; ++k) {
				value += left(row, k) * right(k, col);
			}
			out(row, col) = value;
		}
	}
	return out;
}

} // namespace

Matrix4x4d Matrix4x4d::identity() noexcept {
	Matrix4x4d matrix;
	matrix(0, 0) = 1.0;
	matrix(1, 1) = 1.0;
	matrix(2, 2) = 1.0;
	matrix(3, 3) = 1.0;
	return matrix;
}

std::array<double, 4> Matrix4x4d::multiply(
    const std::array<double, 4>& vector) const noexcept {
	std::array<double, 4> out{};
	for (std::size_t row = 0; row < 4; ++row) {
		for (std::size_t col = 0; col < 4; ++col) {
			out[row] += (*this)(row, col) * vector[col];
		}
	}
	return out;
}

double dot(Vec3d a, Vec3d b) noexcept {
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3d cross(Vec3d a, Vec3d b) noexcept {
	return Vec3d{
	    a.y * b.z - a.z * b.y,
	    a.z * b.x - a.x * b.z,
	    a.x * b.y - a.y * b.x,
	};
}

double norm(Vec3d v) noexcept {
	return std::sqrt(dot(v, v));
}

Vec3d normalize(Vec3d v) noexcept {
	const double length = norm(v);
	if (length <= kEpsilon || !finite(length)) {
		return Vec3d{};
	}
	return Vec3d{v.x / length, v.y / length, v.z / length};
}

Point3d ImagePlaneGeometry::world_from_index(ImagePoint2D index) const noexcept {
	const auto out = index_to_world_.multiply({index.i, index.j, 0.0, 1.0});
	return Point3d{out[0], out[1], out[2]};
}

ImagePoint2D ImagePlaneGeometry::index_from_world(Point3d world) const noexcept {
	const auto out = world_to_index_.multiply({world.x, world.y, world.z, 1.0});
	return ImagePoint2D{out[0], out[1]};
}

double ImagePlaneGeometry::normal_distance_from_world(Point3d world) const noexcept {
	const auto out = world_to_index_.multiply({world.x, world.y, world.z, 1.0});
	return out[2];
}

bool ImagePlaneGeometry::contains_index(ImagePoint2D index) const noexcept {
	return index.i >= -0.5 && index.j >= -0.5 &&
	       index.i < static_cast<double>(size_.i) - 0.5 &&
	       index.j < static_cast<double>(size_.j) - 0.5;
}

bool ImagePlaneGeometry::contains_world(
    Point3d world, double normal_distance_tolerance_mm) const noexcept {
	if (std::abs(normal_distance_from_world(world)) > normal_distance_tolerance_mm) {
		return false;
	}
	return contains_index(index_from_world(world));
}

GeometryBuildResult<ImagePlaneGeometry> make_image_plane_geometry(
    const ImagePlaneGeometryParams& params, GeometryTolerance tolerance) {
	if (params.size.i == 0 || params.size.j == 0) {
		return GeometryBuildResult<ImagePlaneGeometry>::failure(
		    GeometryBuildStatus::invalid_size, Tag{}, "image size must be positive");
	}
	if (!finite(params.origin)) {
		return GeometryBuildResult<ImagePlaneGeometry>::failure(
		    GeometryBuildStatus::invalid_value, Tag{}, "origin must be finite");
	}
	if (!finite(params.direction_i) || !finite(params.direction_j)) {
		return GeometryBuildResult<ImagePlaneGeometry>::failure(
		    GeometryBuildStatus::invalid_orientation, Tag{}, "direction must be finite");
	}
	if (!finite(params.spacing.i) || !finite(params.spacing.j) ||
	    params.spacing.i <= 0.0 || params.spacing.j <= 0.0) {
		return GeometryBuildResult<ImagePlaneGeometry>::failure(
		    GeometryBuildStatus::invalid_spacing, Tag{}, "spacing must be positive");
	}

	const auto direction_i = normalize(params.direction_i);
	const auto direction_j = normalize(params.direction_j);
	if (norm(direction_i) <= kEpsilon || norm(direction_j) <= kEpsilon) {
		return GeometryBuildResult<ImagePlaneGeometry>::failure(
		    GeometryBuildStatus::invalid_orientation, Tag{}, "direction is zero length");
	}
	const double direction_dot = dot(direction_i, direction_j);
	if (std::abs(direction_dot) > tolerance.orientation_tolerance) {
		return GeometryBuildResult<ImagePlaneGeometry>::failure(
		    GeometryBuildStatus::invalid_orientation, Tag{}, "directions are not orthogonal");
	}
	const auto normal = normalize(cross(direction_i, direction_j));
	if (norm(normal) <= kEpsilon) {
		return GeometryBuildResult<ImagePlaneGeometry>::failure(
		    GeometryBuildStatus::invalid_orientation, Tag{}, "normal is zero length");
	}

	ImagePlaneGeometry geometry;
	geometry.origin_ = params.origin;
	geometry.direction_i_ = direction_i;
	geometry.direction_j_ = direction_j;
	geometry.normal_ = normal;
	geometry.spacing_ = params.spacing;
	geometry.size_ = params.size;
	geometry.index_to_world_ = make_plane_index_to_world(
	    params.origin, direction_i, direction_j, normal, params.spacing);
	geometry.world_to_index_ = make_plane_world_to_index(
	    params.origin, direction_i, direction_j, normal, params.spacing);
	return GeometryBuildResult<ImagePlaneGeometry>::success(std::move(geometry));
}

Point3d ImageVolumeGeometry::world_from_index(ImagePoint3D index) const noexcept {
	const auto out = index_to_world_.multiply({index.i, index.j, index.k, 1.0});
	return Point3d{out[0], out[1], out[2]};
}

ImagePoint3D ImageVolumeGeometry::index_from_world(Point3d world) const noexcept {
	const auto out = world_to_index_.multiply({world.x, world.y, world.z, 1.0});
	return ImagePoint3D{out[0], out[1], out[2]};
}

bool ImageVolumeGeometry::contains_index(ImagePoint3D index) const noexcept {
	return index.i >= -0.5 && index.j >= -0.5 && index.k >= -0.5 &&
	       index.i < static_cast<double>(size_.i) - 0.5 &&
	       index.j < static_cast<double>(size_.j) - 0.5 &&
	       index.k < static_cast<double>(size_.k) - 0.5;
}

bool ImageVolumeGeometry::contains_world(
    Point3d world, GeometryTolerance tolerance) const noexcept {
	auto index = index_from_world(world);
	auto index_tolerance = [](double position_tolerance_mm, double spacing_mm) {
		if (!finite(position_tolerance_mm) || position_tolerance_mm < 0.0) {
			return 0.0;
		}
		return position_tolerance_mm / spacing_mm;
	};
	const double tolerance_i =
	    index_tolerance(tolerance.position_tolerance_mm, spacing_.i);
	const double tolerance_j =
	    index_tolerance(tolerance.position_tolerance_mm, spacing_.j);
	const double tolerance_k =
	    index_tolerance(tolerance.position_tolerance_mm, spacing_.k);
	return index.i >= -0.5 - tolerance_i &&
	       index.i < static_cast<double>(size_.i) - 0.5 + tolerance_i &&
	       index.j >= -0.5 - tolerance_j &&
	       index.j < static_cast<double>(size_.j) - 0.5 + tolerance_j &&
	       index.k >= -0.5 - tolerance_k &&
	       index.k < static_cast<double>(size_.k) - 0.5 + tolerance_k;
}

ImagePoint2D PlaneToPlaneTransform::target_index_from_source_index(
    ImagePoint2D source) const noexcept {
	const auto out =
	    target_from_source_.multiply({source.i, source.j, 0.0, 1.0});
	return ImagePoint2D{out[0], out[1]};
}

ImagePoint2D PlaneToPlaneTransform::source_index_from_target_index(
    ImagePoint2D target) const noexcept {
	const auto out =
	    source_from_target_.multiply({target.i, target.j, 0.0, 1.0});
	return ImagePoint2D{out[0], out[1]};
}

ImagePoint3D PlaneToVolumeTransform::target_index_from_source_index(
    ImagePoint2D source) const noexcept {
	const auto out =
	    target_from_source_.multiply({source.i, source.j, 0.0, 1.0});
	return ImagePoint3D{out[0], out[1], out[2]};
}

ImagePoint2D PlaneToVolumeTransform::source_index_from_target_index(
    ImagePoint3D target) const noexcept {
	const auto projection = source_projection_from_target_index(target);
	return projection.index;
}

PlaneProjection2D PlaneToVolumeTransform::source_projection_from_target_index(
    ImagePoint3D target) const noexcept {
	const auto out =
	    source_from_target_.multiply({target.i, target.j, target.k, 1.0});
	return PlaneProjection2D{ImagePoint2D{out[0], out[1]}, out[2]};
}

ImagePoint2D VolumeToPlaneTransform::target_index_from_source_index(
    ImagePoint3D source) const noexcept {
	const auto projection = target_projection_from_source_index(source);
	return projection.index;
}

PlaneProjection2D VolumeToPlaneTransform::target_projection_from_source_index(
    ImagePoint3D source) const noexcept {
	const auto out =
	    target_from_source_.multiply({source.i, source.j, source.k, 1.0});
	return PlaneProjection2D{ImagePoint2D{out[0], out[1]}, out[2]};
}

ImagePoint3D VolumeToPlaneTransform::source_index_from_target_index(
    ImagePoint2D target) const noexcept {
	const auto out =
	    source_from_target_.multiply({target.i, target.j, 0.0, 1.0});
	return ImagePoint3D{out[0], out[1], out[2]};
}

ImagePoint3D VolumeToVolumeTransform::target_index_from_source_index(
    ImagePoint3D source) const noexcept {
	const auto out =
	    target_from_source_.multiply({source.i, source.j, source.k, 1.0});
	return ImagePoint3D{out[0], out[1], out[2]};
}

ImagePoint3D VolumeToVolumeTransform::source_index_from_target_index(
    ImagePoint3D target) const noexcept {
	const auto out =
	    source_from_target_.multiply({target.i, target.j, target.k, 1.0});
	return ImagePoint3D{out[0], out[1], out[2]};
}

PlaneToPlaneTransform make_plane_to_plane_transform(
    const ImagePlaneGeometry& source,
    const ImagePlaneGeometry& target) noexcept {
	PlaneToPlaneTransform transform;
	transform.target_from_source_ =
	    multiply(target.world_to_index_matrix(), source.index_to_world_matrix());
	transform.source_from_target_ =
	    multiply(source.world_to_index_matrix(), target.index_to_world_matrix());
	return transform;
}

PlaneToVolumeTransform make_plane_to_volume_transform(
    const ImagePlaneGeometry& source,
    const ImageVolumeGeometry& target) noexcept {
	PlaneToVolumeTransform transform;
	transform.target_from_source_ =
	    multiply(target.world_to_index_matrix(), source.index_to_world_matrix());
	transform.source_from_target_ =
	    multiply(source.world_to_index_matrix(), target.index_to_world_matrix());
	return transform;
}

VolumeToPlaneTransform make_volume_to_plane_transform(
    const ImageVolumeGeometry& source,
    const ImagePlaneGeometry& target) noexcept {
	VolumeToPlaneTransform transform;
	transform.target_from_source_ =
	    multiply(target.world_to_index_matrix(), source.index_to_world_matrix());
	transform.source_from_target_ =
	    multiply(source.world_to_index_matrix(), target.index_to_world_matrix());
	return transform;
}

VolumeToVolumeTransform make_volume_to_volume_transform(
    const ImageVolumeGeometry& source,
    const ImageVolumeGeometry& target) noexcept {
	VolumeToVolumeTransform transform;
	transform.target_from_source_ =
	    multiply(target.world_to_index_matrix(), source.index_to_world_matrix());
	transform.source_from_target_ =
	    multiply(source.world_to_index_matrix(), target.index_to_world_matrix());
	return transform;
}

GeometryBuildResult<ImageVolumeGeometry> make_image_volume_geometry(
    const ImageVolumeGeometryParams& params, GeometryTolerance tolerance) {
	if (params.size.i == 0 || params.size.j == 0 || params.size.k == 0) {
		return GeometryBuildResult<ImageVolumeGeometry>::failure(
		    GeometryBuildStatus::invalid_size, Tag{}, "volume size must be positive");
	}
	if (!finite(params.origin)) {
		return GeometryBuildResult<ImageVolumeGeometry>::failure(
		    GeometryBuildStatus::invalid_value, Tag{}, "origin must be finite");
	}
	if (!finite(params.direction_i) || !finite(params.direction_j) ||
	    !finite(params.direction_k)) {
		return GeometryBuildResult<ImageVolumeGeometry>::failure(
		    GeometryBuildStatus::invalid_orientation, Tag{}, "direction must be finite");
	}
	if (!finite(params.spacing.i) || !finite(params.spacing.j) ||
	    !finite(params.spacing.k) || params.spacing.i <= 0.0 ||
	    params.spacing.j <= 0.0 || params.spacing.k <= 0.0) {
		return GeometryBuildResult<ImageVolumeGeometry>::failure(
		    GeometryBuildStatus::invalid_spacing, Tag{}, "spacing must be positive");
	}

	const auto direction_i = normalize(params.direction_i);
	const auto direction_j = normalize(params.direction_j);
	const auto direction_k = normalize(params.direction_k);
	if (norm(direction_i) <= kEpsilon || norm(direction_j) <= kEpsilon ||
	    norm(direction_k) <= kEpsilon) {
		return GeometryBuildResult<ImageVolumeGeometry>::failure(
		    GeometryBuildStatus::invalid_orientation, Tag{}, "direction is zero length");
	}
	if (std::abs(dot(direction_i, direction_j)) > tolerance.orientation_tolerance ||
	    std::abs(dot(direction_i, direction_k)) > tolerance.orientation_tolerance ||
	    std::abs(dot(direction_j, direction_k)) > tolerance.orientation_tolerance) {
		return GeometryBuildResult<ImageVolumeGeometry>::failure(
		    GeometryBuildStatus::invalid_orientation,
		    Tag{}, "directions are not orthogonal");
	}
	const double handedness = dot(cross(direction_i, direction_j), direction_k);
	if (std::abs(handedness - 1.0) > tolerance.orientation_tolerance) {
		return GeometryBuildResult<ImageVolumeGeometry>::failure(
		    GeometryBuildStatus::invalid_orientation,
		    Tag{}, "directions must form a right-handed basis");
	}

	ImageVolumeGeometry geometry;
	geometry.origin_ = params.origin;
	geometry.direction_i_ = direction_i;
	geometry.direction_j_ = direction_j;
	geometry.direction_k_ = direction_k;
	geometry.spacing_ = params.spacing;
	geometry.size_ = params.size;
	geometry.index_to_world_ = make_volume_index_to_world(
	    params.origin, direction_i, direction_j, direction_k, params.spacing);
	geometry.world_to_index_ = make_volume_world_to_index(
	    params.origin, direction_i, direction_j, direction_k, params.spacing);
	return GeometryBuildResult<ImageVolumeGeometry>::success(std::move(geometry));
}

GeometryBuildResult<ImagePlaneGeometry> plane_from_single_frame_image(
    const DataSet& dataset) {
	const auto rows = read_positive_int(dataset, "Rows"_tag);
	if (!rows.ok()) {
		return forward_int_failure(rows);
	}
	const auto columns = read_positive_int(dataset, "Columns"_tag);
	if (!columns.ok()) {
		return forward_int_failure(columns);
	}
	const auto image_position =
	    read_double_vector(dataset, "ImagePositionPatient"_tag, 3);
	if (!image_position.ok()) {
		return forward_vector_failure(image_position);
	}
	const auto image_orientation =
	    read_double_vector(dataset, "ImageOrientationPatient"_tag, 6);
	if (!image_orientation.ok()) {
		return forward_vector_failure(image_orientation);
	}
	const auto pixel_spacing = read_double_vector(dataset, "PixelSpacing"_tag, 2);
	if (!pixel_spacing.ok()) {
		return forward_vector_failure(pixel_spacing);
	}

	return plane_from_dicom_values(rows.value(), columns.value(),
	    image_position.value(), image_orientation.value(), pixel_spacing.value());
}

GeometryBuildResult<ImagePlaneGeometry> plane_from_single_frame_image(
    const DicomFile& file) {
	return plane_from_single_frame_image(file.dataset());
}

FrameGeometryReader::FrameGeometryReader(const DataSet& dataset) noexcept
    : root_(&dataset) {
	const auto per_frame_sequence = resolve_per_frame_sequence(dataset);
	if (per_frame_sequence.ok()) {
		per_frame_functional_groups_sequence_ = per_frame_sequence.value();
	} else {
		per_frame_functional_groups_status_ = per_frame_sequence.status();
		per_frame_functional_groups_tag_ = per_frame_sequence.tag();
	}
	const auto shared_item =
	    resolve_sequence_item_if_present(dataset, "SharedFunctionalGroupsSequence"_tag, 0);
	if (shared_item.ok()) {
		shared_functional_groups_item_ = shared_item.value();
	} else {
		shared_functional_groups_status_ = shared_item.status();
		shared_functional_groups_tag_ = shared_item.tag();
	}
	if (auto sop_class_uid = root_string_view(dataset, "SOPClassUID"_tag)) {
		has_sop_class_uid_ = true;
		nm_image_storage_ = is_nm_image_storage(*sop_class_uid);
		frame_type_sequence_tag_ =
		    frame_type_sequence_tag_for_sop_class(*sop_class_uid);
		unsupported_sop_class_ =
		    !nm_image_storage_ && !static_cast<bool>(frame_type_sequence_tag_);
	}
}

FrameGeometryReader::FrameGeometryReader(const DicomFile& file) noexcept
    : FrameGeometryReader(file.dataset()) {}

GeometryBuildResult<ImagePlaneGeometry> FrameGeometryReader::raw_plane(
    std::size_t frame_index) const {
	if (!root_) {
		return GeometryBuildResult<ImagePlaneGeometry>::failure(
		    GeometryBuildStatus::invalid_value, Tag{}, "missing root dataset");
	}
	const auto rows = read_positive_int(*root_, "Rows"_tag);
	if (!rows.ok()) {
		return forward_int_failure(rows);
	}
	const auto columns = read_positive_int(*root_, "Columns"_tag);
	if (!columns.ok()) {
		return forward_int_failure(columns);
	}
	if (per_frame_functional_groups_status_ != GeometryBuildStatus::ok) {
		return GeometryBuildResult<ImagePlaneGeometry>::failure(
		    per_frame_functional_groups_status_, per_frame_functional_groups_tag_,
		    "invalid PerFrameFunctionalGroupsSequence");
	}
	if (shared_functional_groups_status_ != GeometryBuildStatus::ok) {
		return GeometryBuildResult<ImagePlaneGeometry>::failure(
		    shared_functional_groups_status_, shared_functional_groups_tag_,
		    "invalid SharedFunctionalGroupsSequence");
	}
	if (!per_frame_functional_groups_sequence_) {
		const auto frame_index_result =
		    validate_root_only_frame_index(*root_, frame_index);
		if (!frame_index_result.ok()) {
			return forward_int_failure(frame_index_result);
		}
	}
	const auto per_frame =
	    resolve_per_frame_item(per_frame_functional_groups_sequence_, frame_index);
	if (!per_frame.ok()) {
		return GeometryBuildResult<ImagePlaneGeometry>::failure(
		    per_frame.status(), per_frame.tag(), per_frame.message());
	}
	const DataSet* per_frame_item = per_frame.value();
	const auto image_position = resolve_frame_double_vector(*root_, per_frame_item,
	    shared_functional_groups_item_, "PlanePositionSequence"_tag,
	    "ImagePositionPatient"_tag, "ImagePositionPatient"_tag, 3);
	if (!image_position.ok()) {
		return forward_vector_failure(image_position);
	}
	const auto image_orientation = resolve_frame_double_vector(*root_, per_frame_item,
	    shared_functional_groups_item_, "PlaneOrientationSequence"_tag,
	    "ImageOrientationPatient"_tag, "ImageOrientationPatient"_tag, 6);
	if (!image_orientation.ok()) {
		return forward_vector_failure(image_orientation);
	}
	const auto pixel_spacing = resolve_frame_double_vector(*root_, per_frame_item,
	    shared_functional_groups_item_, "PixelMeasuresSequence"_tag,
	    "PixelSpacing"_tag, "PixelSpacing"_tag, 2);
	if (!pixel_spacing.ok()) {
		return forward_vector_failure(pixel_spacing);
	}

	return plane_from_dicom_values(rows.value(), columns.value(),
	    image_position.value(), image_orientation.value(), pixel_spacing.value());
}

GeometryBuildResult<ImagePlaneGeometry> FrameGeometryReader::plane(
    std::size_t frame_index) const {
	auto frame_geometry = image_frame_geometry(frame_index);
	if (!frame_geometry.ok()) {
		return GeometryBuildResult<ImagePlaneGeometry>::failure(
		    frame_geometry.status(), frame_geometry.tag(), frame_geometry.message());
	}
	if (frame_geometry.value().kind == ImageFrameGeometryKind::sampled_projection) {
		return GeometryBuildResult<ImagePlaneGeometry>::failure(
		    GeometryBuildStatus::sampled_frame_geometry,
		    "VolumetricProperties"_tag,
		    "SAMPLED frame is not a regular overlay plane");
	}
	if (frame_geometry.value().kind == ImageFrameGeometryKind::distorted) {
		return GeometryBuildResult<ImagePlaneGeometry>::failure(
		    GeometryBuildStatus::distorted_frame_geometry,
		    "VolumetricProperties"_tag,
		    "DISTORTED frame is not a regular overlay plane");
	}
	auto frame = std::move(frame_geometry).value();
	return GeometryBuildResult<ImagePlaneGeometry>::success(std::move(frame.plane));
}

GeometryBuildResult<ImageFrameGeometry> FrameGeometryReader::image_frame_geometry(
    std::size_t frame_index) const {
	auto plane_result = raw_plane(frame_index);
	if (!plane_result.ok()) {
		return GeometryBuildResult<ImageFrameGeometry>::failure(
		    plane_result.status(), plane_result.tag(), plane_result.message());
	}
	auto volumetric_properties_result = volumetric_properties(frame_index);
	if (!volumetric_properties_result.ok()) {
		return GeometryBuildResult<ImageFrameGeometry>::failure(
		    volumetric_properties_result.status(), volumetric_properties_result.tag(),
		    volumetric_properties_result.message());
	}

	ImageFrameGeometry frame_geometry{
	    std::move(plane_result).value(),
	    kind_from_volumetric_properties(volumetric_properties_result.value().value)};
	return GeometryBuildResult<ImageFrameGeometry>::success(
	    std::move(frame_geometry));
}

GeometryBuildResult<VolumetricPropertiesInfo>
FrameGeometryReader::volumetric_properties(std::size_t frame_index) const {
	if (!root_) {
		return GeometryBuildResult<VolumetricPropertiesInfo>::failure(
		    GeometryBuildStatus::invalid_value, Tag{}, "missing root dataset");
	}
	if (nm_image_storage_) {
		return GeometryBuildResult<VolumetricPropertiesInfo>::failure(
		    GeometryBuildStatus::unsupported_frame_geometry, "SOPClassUID"_tag,
		    "NM Image Storage uses NM-specific frame organization");
	}
	if (unsupported_sop_class_) {
		return GeometryBuildResult<VolumetricPropertiesInfo>::failure(
		    GeometryBuildStatus::unsupported_frame_geometry, "SOPClassUID"_tag,
		    "SOP Class has no supported frame type sequence");
	}
	if (frame_index >
	    static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
		return GeometryBuildResult<VolumetricPropertiesInfo>::failure(
		    GeometryBuildStatus::invalid_frame_index,
		    "PerFrameFunctionalGroupsSequence"_tag,
		    "frame index is too large for ElementPath diagnostics");
	}
	if (per_frame_functional_groups_status_ != GeometryBuildStatus::ok) {
		return GeometryBuildResult<VolumetricPropertiesInfo>::failure(
		    per_frame_functional_groups_status_, per_frame_functional_groups_tag_,
		    "invalid PerFrameFunctionalGroupsSequence");
	}
	if (shared_functional_groups_status_ != GeometryBuildStatus::ok) {
		return GeometryBuildResult<VolumetricPropertiesInfo>::failure(
		    shared_functional_groups_status_, shared_functional_groups_tag_,
		    "invalid SharedFunctionalGroupsSequence");
	}
	if (!per_frame_functional_groups_sequence_) {
		const auto frame_index_result =
		    validate_root_only_frame_index(*root_, frame_index);
		if (!frame_index_result.ok()) {
			return GeometryBuildResult<VolumetricPropertiesInfo>::failure(
			    frame_index_result.status(), frame_index_result.tag(),
			    frame_index_result.message());
		}
	}

	const auto frame_item_index = static_cast<std::uint32_t>(frame_index);
	const auto per_frame =
	    resolve_per_frame_item(per_frame_functional_groups_sequence_, frame_index);
	if (!per_frame.ok()) {
		return GeometryBuildResult<VolumetricPropertiesInfo>::failure(
		    per_frame.status(), per_frame.tag(), per_frame.message());
	}

	if (frame_type_sequence_tag_) {
		if (auto result = try_read_code_string_from_functional_group(
		        per_frame.value(), "PerFrameFunctionalGroupsSequence"_tag,
		        frame_item_index, frame_type_sequence_tag_,
		        "VolumetricProperties"_tag)) {
			if (!result->ok()) {
				return GeometryBuildResult<VolumetricPropertiesInfo>::failure(
				    result->status(), result->tag(), result->message());
			}
			return volumetric_properties_from_code_string(std::move(*result).value());
		}
		if (auto result = try_read_code_string_from_functional_group(
		        shared_functional_groups_item_,
		        "SharedFunctionalGroupsSequence"_tag, 0,
		        frame_type_sequence_tag_, "VolumetricProperties"_tag)) {
			if (!result->ok()) {
				return GeometryBuildResult<VolumetricPropertiesInfo>::failure(
				    result->status(), result->tag(), result->message());
			}
			return volumetric_properties_from_code_string(std::move(*result).value());
		}
	} else if (has_sop_class_uid_) {
		return GeometryBuildResult<VolumetricPropertiesInfo>::failure(
		    GeometryBuildStatus::unsupported_frame_geometry, "SOPClassUID"_tag,
		    "SOP Class has no supported frame type sequence");
	}

	if (auto result =
	        try_read_root_code_string(*root_, "VolumetricProperties"_tag)) {
		if (!result->ok()) {
			return GeometryBuildResult<VolumetricPropertiesInfo>::failure(
			    result->status(), result->tag(), result->message());
		}
		return volumetric_properties_from_code_string(std::move(*result).value());
	}
	return GeometryBuildResult<VolumetricPropertiesInfo>::failure(
	    GeometryBuildStatus::missing_volumetric_properties,
	    "VolumetricProperties"_tag,
	    "missing VolumetricProperties");
}

GeometryBuildResult<std::string> FrameGeometryReader::frame_of_reference() const {
	if (!root_) {
		return GeometryBuildResult<std::string>::failure(
		    GeometryBuildStatus::invalid_value, Tag{}, "missing root dataset");
	}
	return frame_of_reference_from_dataset(*root_);
}

GeometryBuildResult<ImagePlaneGeometry> plane_from_multiframe_image(
    const DataSet& dataset, std::size_t frame_index) {
	auto result = FrameGeometryReader(dataset).image_frame_geometry(frame_index);
	if (!result.ok()) {
		return GeometryBuildResult<ImagePlaneGeometry>::failure(
		    result.status(), result.tag(), result.message());
	}
	if (result.value().kind == ImageFrameGeometryKind::sampled_projection) {
		return GeometryBuildResult<ImagePlaneGeometry>::failure(
		    GeometryBuildStatus::sampled_frame_geometry,
		    "VolumetricProperties"_tag,
		    "SAMPLED frame is not a regular overlay plane");
	}
	if (result.value().kind == ImageFrameGeometryKind::distorted) {
		return GeometryBuildResult<ImagePlaneGeometry>::failure(
		    GeometryBuildStatus::distorted_frame_geometry,
		    "VolumetricProperties"_tag,
		    "DISTORTED frame is not a regular overlay plane");
	}
	auto frame_geometry = std::move(result).value();
	return GeometryBuildResult<ImagePlaneGeometry>::success(
	    std::move(frame_geometry.plane));
}

GeometryBuildResult<ImagePlaneGeometry> plane_from_multiframe_image(
    const DicomFile& file, std::size_t frame_index) {
	return plane_from_multiframe_image(file.dataset(), frame_index);
}

GeometryBuildResult<ImageFrameGeometry> frame_geometry_from_multiframe_image(
    const DataSet& dataset, std::size_t frame_index) {
	return FrameGeometryReader(dataset).image_frame_geometry(frame_index);
}

GeometryBuildResult<ImageFrameGeometry> frame_geometry_from_multiframe_image(
    const DicomFile& file, std::size_t frame_index) {
	return FrameGeometryReader(file).image_frame_geometry(frame_index);
}

GeometryBuildResult<VolumetricPropertiesInfo>
volumetric_properties_from_multiframe_image(
    const DataSet& dataset, std::size_t frame_index) {
	return FrameGeometryReader(dataset).volumetric_properties(frame_index);
}

GeometryBuildResult<VolumetricPropertiesInfo>
volumetric_properties_from_multiframe_image(
    const DicomFile& file, std::size_t frame_index) {
	return FrameGeometryReader(file).volumetric_properties(frame_index);
}

GeometryBuildResult<ImagePlaneGeometry> plane_from_seg_frame(
    const seg::Segmentation& segmentation, std::size_t frame_index) {
	if (frame_index >= segmentation.frame_count()) {
		return GeometryBuildResult<ImagePlaneGeometry>::failure(
		    GeometryBuildStatus::invalid_frame_index,
		    "PerFrameFunctionalGroupsSequence"_tag,
		    "frame index is outside SEG frame range");
	}
	if (segmentation.rows() == 0 || segmentation.columns() == 0) {
		return GeometryBuildResult<ImagePlaneGeometry>::failure(
		    GeometryBuildStatus::invalid_size, "Rows"_tag,
		    "SEG Rows/Columns must be positive");
	}

	const auto* per_frame_item =
	    segmentation.try_per_frame_functional_groups_item(frame_index);
	if (!per_frame_item) {
		return GeometryBuildResult<ImagePlaneGeometry>::failure(
		    GeometryBuildStatus::missing_required_tag,
		    "PerFrameFunctionalGroupsSequence"_tag,
		    "missing SEG PerFrameFunctionalGroupsSequence item");
	}
	const auto* shared_item = segmentation.try_shared_functional_groups_item();
	if (!shared_item) {
		return GeometryBuildResult<ImagePlaneGeometry>::failure(
		    GeometryBuildStatus::missing_required_tag,
		    "SharedFunctionalGroupsSequence"_tag,
		    "missing SEG SharedFunctionalGroupsSequence item");
	}

	const auto image_position = resolve_strict_frame_double_vector(
	    per_frame_item, shared_item, "PlanePositionSequence"_tag,
	    "ImagePositionPatient"_tag, 3);
	if (!image_position.ok()) {
		return forward_vector_failure(image_position);
	}
	const auto image_orientation = resolve_strict_frame_double_vector(
	    per_frame_item, shared_item, "PlaneOrientationSequence"_tag,
	    "ImageOrientationPatient"_tag, 6);
	if (!image_orientation.ok()) {
		return forward_vector_failure(image_orientation);
	}
	const auto pixel_spacing = resolve_strict_frame_double_vector(
	    per_frame_item, shared_item, "PixelMeasuresSequence"_tag,
	    "PixelSpacing"_tag, 2);
	if (!pixel_spacing.ok()) {
		return forward_vector_failure(pixel_spacing);
	}

	return plane_from_dicom_values(segmentation.rows(), segmentation.columns(),
	    image_position.value(), image_orientation.value(), pixel_spacing.value());
}

GeometryBuildResult<std::string> frame_of_reference_from_dataset(
    const DataSet& dataset) {
	const auto tag = "FrameOfReferenceUID"_tag;
	const auto& element = dataset.get_dataelement(root_path(tag));
	if (element.is_missing()) {
		return GeometryBuildResult<std::string>::failure(
		    GeometryBuildStatus::missing_required_tag, tag,
		    make_tag_message("missing required tag", tag));
	}
	auto value = element.to_uid_string();
	if (!value || value->empty()) {
		return GeometryBuildResult<std::string>::failure(
		    GeometryBuildStatus::invalid_value, tag,
		    make_tag_message("invalid UID value", tag));
	}
	return GeometryBuildResult<std::string>::success(std::move(*value));
}

GeometryBuildResult<std::string> frame_of_reference_from_dataset(
    const DicomFile& file) {
	return frame_of_reference_from_dataset(file.dataset());
}

GeometryBuildResult<std::string> frame_of_reference_from_segmentation(
    const seg::Segmentation& segmentation) {
	const auto tag = "FrameOfReferenceUID"_tag;
	const auto value = segmentation.frame_of_reference_uid();
	if (!value || value->empty()) {
		return GeometryBuildResult<std::string>::failure(
		    GeometryBuildStatus::missing_required_tag, tag,
		    make_tag_message("missing required tag", tag));
	}
	return GeometryBuildResult<std::string>::success(std::string(*value));
}

SliceStackAnalysis analyze_slice_stack(
    std::span<const SliceStackInput> inputs, SliceStackOptions options) {
	SliceStackAnalysis analysis;
	if (inputs.empty()) {
		analysis.status_ = SliceStackStatus::empty;
		analysis.issues_.push_back(SliceStackIssue{
		    SliceStackStatus::empty,
		    0,
		    0,
		    0,
		    Tag{},
		    "slice stack input is empty",
		});
		return analysis;
	}

	analysis.status_ = SliceStackStatus::ok;
	const auto orientation_tolerance =
	    std::max(0.0, options.tolerance.orientation_tolerance);
	const auto spacing_tolerance =
	    std::max(0.0, options.tolerance.spacing_tolerance_mm);
	const auto slice_position_tolerance =
	    std::max(0.0, options.slice_position_tolerance_mm);
	const auto origin_residual_tolerance =
	    std::max(0.0, options.origin_residual_tolerance_mm);
	const auto& reference_input = inputs.front();
	const auto& reference_plane = reference_input.plane;
	const auto reference_size = reference_plane.size();
	const auto reference_spacing = reference_plane.spacing();
	const auto reference_origin = reference_plane.origin();
	const auto reference_direction_i = reference_plane.direction_i();
	const auto reference_direction_j = reference_plane.direction_j();
	const auto reference_normal = reference_plane.normal();

	auto add_issue = [&](SliceStackStatus status, const SliceStackInput& input,
	                     std::size_t input_index, Tag tag,
	                     std::string message) {
		analysis.issues_.push_back(SliceStackIssue{
		    status,
		    input_index,
		    input.source_index,
		    input.frame_index,
		    tag,
		    std::move(message),
		});
		if (slice_stack_status_priority(status) <
		    slice_stack_status_priority(analysis.status_)) {
			analysis.status_ = status;
		}
	};

	for (std::size_t input_index = 0; input_index < inputs.size(); ++input_index) {
		const auto& input = inputs[input_index];
		if (input.frame_of_reference_uid.empty()) {
			add_issue(SliceStackStatus::missing_frame_of_reference, input,
			    input_index, "FrameOfReferenceUID"_tag,
			    "missing FrameOfReferenceUID");
		} else if (analysis.frame_of_reference_uid_.empty()) {
			analysis.frame_of_reference_uid_ =
			    std::string(input.frame_of_reference_uid);
		} else if (input.frame_of_reference_uid !=
		           analysis.frame_of_reference_uid_) {
			add_issue(SliceStackStatus::mixed_frame_of_reference, input,
			    input_index, "FrameOfReferenceUID"_tag,
			    "mixed FrameOfReferenceUID values");
		}

		const auto& plane = input.plane;
		if (!finite(plane)) {
			add_issue(SliceStackStatus::inconsistent_orientation, input,
			    input_index, Tag{}, "plane geometry has non-finite values");
		}
		if (plane.size().i != reference_size.i ||
		    plane.size().j != reference_size.j) {
			add_issue(SliceStackStatus::inconsistent_rows_columns, input,
			    input_index, Tag{}, "slice Rows/Columns differ from reference");
		}
		const auto spacing = plane.spacing();
		if (std::abs(spacing.i - reference_spacing.i) > spacing_tolerance ||
		    std::abs(spacing.j - reference_spacing.j) > spacing_tolerance) {
			add_issue(SliceStackStatus::inconsistent_pixel_spacing, input,
			    input_index, "PixelSpacing"_tag,
			    "slice PixelSpacing differs from reference");
		}
		if (1.0 - dot(plane.direction_i(), reference_direction_i) >
		        orientation_tolerance ||
		    1.0 - dot(plane.direction_j(), reference_direction_j) >
		        orientation_tolerance ||
		    1.0 - dot(plane.normal(), reference_normal) >
		        orientation_tolerance) {
			add_issue(SliceStackStatus::inconsistent_orientation, input,
			    input_index, "ImageOrientationPatient"_tag,
			    "slice orientation differs from reference");
		}

		const auto origin_delta = plane.origin() - reference_origin;
		const double position = dot(origin_delta, reference_normal);
		const auto projected_origin =
		    reference_origin + reference_normal * position;
		const auto residual = plane.origin() - projected_origin;
		const double residual_i = dot(residual, reference_direction_i);
		const double residual_j = dot(residual, reference_direction_j);
		const double residual_mm =
		    std::sqrt(residual_i * residual_i + residual_j * residual_j);
		if (residual_mm > origin_residual_tolerance) {
			add_issue(SliceStackStatus::inconsistent_slice_origin, input,
			    input_index, "ImagePositionPatient"_tag,
			    "slice origin has in-plane residual");
		}

		analysis.slices_.push_back(SliceStackSlice{
		    input_index,
		    input.source_index,
		    input.frame_index,
		    plane,
		    position,
		});
	}

	std::sort(analysis.slices_.begin(), analysis.slices_.end(),
	    [](const SliceStackSlice& a, const SliceStackSlice& b) {
		    return a.position_along_normal_mm < b.position_along_normal_mm;
	    });

	for (std::size_t sorted_index = 1; sorted_index < analysis.slices_.size();
	     ++sorted_index) {
		const auto& lower = analysis.slices_[sorted_index - 1];
		const auto& upper = analysis.slices_[sorted_index];
		const double spacing =
		    upper.position_along_normal_mm - lower.position_along_normal_mm;
		analysis.gaps_.push_back(SliceStackGap{
		    sorted_index - 1,
		    sorted_index,
		    spacing,
		});
		if (spacing <= slice_position_tolerance) {
			const auto& input = inputs[upper.input_index];
			if (!options.allow_duplicate_positions) {
				add_issue(SliceStackStatus::duplicate_slice_position, input,
				    upper.input_index, "ImagePositionPatient"_tag,
				    "duplicate slice position");
			}
		}
	}

	if (!analysis.gaps_.empty()) {
		std::optional<double> reference_gap;
		bool has_duplicate_gap = false;
		bool uniform = true;
		for (const auto& gap : analysis.gaps_) {
			if (gap.spacing_mm <= slice_position_tolerance) {
				has_duplicate_gap = true;
				continue;
			}
			if (!reference_gap) {
				reference_gap = gap.spacing_mm;
				continue;
			}
			if (std::abs(gap.spacing_mm - *reference_gap) > spacing_tolerance) {
				uniform = false;
				const auto& upper = analysis.slices_[gap.upper_sorted_index];
				const auto& input = inputs[upper.input_index];
				add_issue(SliceStackStatus::non_uniform_spacing, input,
				    upper.input_index, "ImagePositionPatient"_tag,
				    "slice spacing is not uniform");
			}
		}
		if (uniform && !has_duplicate_gap && reference_gap &&
		    analysis.status_ == SliceStackStatus::ok) {
			analysis.uniform_spacing_k_ = *reference_gap;
		}
	}

	return analysis;
}

SliceStackAnalysis analyze_slice_stack(
    std::span<const DataSet* const> datasets, SliceStackOptions options) {
	auto store = make_slice_stack_inputs_from_datasets(datasets);
	if (store.inputs.empty()) {
		if (store.issues.empty()) {
			return analyze_slice_stack(std::span<const SliceStackInput>{}, options);
		}
		SliceStackAnalysis analysis;
		analysis.status_ = store.status;
		analysis.issues_ = std::move(store.issues);
		return analysis;
	}

	auto analysis = analyze_slice_stack(
	    std::span<const SliceStackInput>(store.inputs.data(), store.inputs.size()),
	    options);
	for (auto& issue : store.issues) {
		if (slice_stack_status_priority(issue.status) <
		    slice_stack_status_priority(analysis.status_)) {
			analysis.status_ = issue.status;
		}
		analysis.issues_.push_back(std::move(issue));
	}
	return analysis;
}

SliceStackPlan plan_slice_stack(
    std::span<const SliceStackInput> inputs, SliceStackOptions options) {
	auto analysis = analyze_slice_stack(inputs, options);
	SliceStackPlan plan;
	plan.status_ = analysis.status();
	plan.frame_of_reference_uid_ = analysis.frame_of_reference_uid();
	plan.issues_ = analysis.issues();
	if (!analysis.ok()) {
		return plan;
	}
	if (!analysis.uniform_spacing_k()) {
		plan.status_ = SliceStackStatus::non_uniform_spacing;
		return plan;
	}

	const auto& slices = analysis.slices();
	if (slices.empty()) {
		plan.status_ = SliceStackStatus::empty;
		return plan;
	}

	const auto& first_plane = slices.front().plane;
	ImageVolumeGeometryParams params;
	params.origin = first_plane.origin();
	params.direction_i = first_plane.direction_i();
	params.direction_j = first_plane.direction_j();
	params.direction_k = first_plane.normal();
	params.spacing = ImageSpacing3D{
	    first_plane.spacing_i(),
	    first_plane.spacing_j(),
	    *analysis.uniform_spacing_k(),
	};
	params.size = ImageSize3D{
	    first_plane.columns(),
	    first_plane.rows(),
	    slices.size(),
	};

	auto volume = make_image_volume_geometry(params, options.tolerance);
	if (!volume.ok()) {
		switch (volume.status()) {
		case GeometryBuildStatus::invalid_size:
			plan.status_ = SliceStackStatus::inconsistent_rows_columns;
			break;
		case GeometryBuildStatus::invalid_spacing:
			plan.status_ = SliceStackStatus::non_uniform_spacing;
			break;
		case GeometryBuildStatus::invalid_orientation:
			plan.status_ = SliceStackStatus::inconsistent_orientation;
			break;
		default:
			plan.status_ = SliceStackStatus::inconsistent_slice_origin;
			break;
		}
		plan.issues_.push_back(SliceStackIssue{
		    plan.status_,
		    slices.front().input_index,
		    slices.front().source_index,
		    slices.front().frame_index,
		    volume.tag(),
		    volume.message(),
		});
		return plan;
	}

	plan.volume_geometry_ = std::move(volume).value();
	plan.placements_.reserve(slices.size());
	for (std::size_t sorted_index = 0; sorted_index < slices.size();
	     ++sorted_index) {
		const auto& slice = slices[sorted_index];
		plan.placements_.push_back(SliceStackItem{
		    slice.source_index,
		    slice.frame_index,
		    sorted_index,
		    slice.position_along_normal_mm,
		});
	}
	return plan;
}

SliceStackPlan plan_slice_stack(
    std::span<const DataSet* const> datasets, SliceStackOptions options) {
	auto store = make_slice_stack_inputs_from_datasets(datasets);
	if (!store.issues.empty()) {
		SliceStackPlan plan;
		auto analysis = store.inputs.empty()
		    ? SliceStackAnalysis{}
		    : analyze_slice_stack(
		          std::span<const SliceStackInput>(
		              store.inputs.data(), store.inputs.size()),
		          options);
		plan.status_ = analysis.status();
		plan.frame_of_reference_uid_ = analysis.frame_of_reference_uid();
		plan.issues_ = analysis.issues();
		for (auto& issue : store.issues) {
			if (slice_stack_status_priority(issue.status) <
			    slice_stack_status_priority(plan.status_)) {
				plan.status_ = issue.status;
			}
			plan.issues_.push_back(std::move(issue));
		}
		if (store.inputs.empty()) {
			plan.status_ = store.status;
		}
		return plan;
	}

	return plan_slice_stack(
	    std::span<const SliceStackInput>(store.inputs.data(), store.inputs.size()),
	    options);
}

SliceStackAnalysis analyze_image_frame_stack(const DicomFile& file,
    std::span<const std::size_t> frame_indices, ImageFrameStackOptions options) {
	auto store = make_slice_stack_inputs_from_image_frames(file, frame_indices);
	if (store.inputs.empty()) {
		if (store.issues.empty()) {
			return analyze_slice_stack(
			    std::span<const SliceStackInput>{}, options.slice_stack);
		}
		SliceStackAnalysis analysis;
		analysis.status_ = store.status;
		analysis.issues_ = std::move(store.issues);
		return analysis;
	}

	auto analysis = analyze_slice_stack(
	    std::span<const SliceStackInput>(store.inputs.data(), store.inputs.size()),
	    options.slice_stack);
	for (auto& issue : store.issues) {
		if (slice_stack_status_priority(issue.status) <
		    slice_stack_status_priority(analysis.status_)) {
			analysis.status_ = issue.status;
		}
		analysis.issues_.push_back(std::move(issue));
	}
	return analysis;
}

SliceStackPlan plan_image_frame_stack(const DicomFile& file,
    std::span<const std::size_t> frame_indices, ImageFrameStackOptions options) {
	auto store = make_slice_stack_inputs_from_image_frames(file, frame_indices);
	if (!store.issues.empty()) {
		SliceStackPlan plan;
		auto analysis = store.inputs.empty()
		    ? SliceStackAnalysis{}
		    : analyze_slice_stack(
		          std::span<const SliceStackInput>(
		              store.inputs.data(), store.inputs.size()),
		          options.slice_stack);
		plan.status_ = analysis.status();
		plan.frame_of_reference_uid_ = analysis.frame_of_reference_uid();
		plan.issues_ = analysis.issues();
		for (auto& issue : store.issues) {
			if (slice_stack_status_priority(issue.status) <
			    slice_stack_status_priority(plan.status_)) {
				plan.status_ = issue.status;
			}
			plan.issues_.push_back(std::move(issue));
		}
		if (store.inputs.empty()) {
			plan.status_ = store.status;
		}
		return plan;
	}

	return plan_slice_stack(
	    std::span<const SliceStackInput>(store.inputs.data(), store.inputs.size()),
	    options.slice_stack);
}

ImageFrameStackAnalysis analyze_image_frame_stacks(
    const DicomFile& file, ImageFrameStackOptions options) {
	ImageFrameStackAnalysis stack_analysis;
	stack_analysis.status_ = SliceStackStatus::ok;
	const auto& root = file.dataset();
	if (auto tiled_tag = tiled_multiframe_tag(root)) {
		stack_analysis.status_ = SliceStackStatus::unsupported_tiled_image;
		stack_analysis.issues_.push_back(SliceStackIssue{
		    SliceStackStatus::unsupported_tiled_image,
		    0,
		    0,
		    0,
		    *tiled_tag,
		    "tiled multi-frame images are not supported by slice stack planning",
		});
		return stack_analysis;
	}
	auto frame_count = number_of_frames(root);
	if (!frame_count || *frame_count == 0) {
		stack_analysis.status_ = SliceStackStatus::empty;
		stack_analysis.issues_.push_back(SliceStackIssue{
		    SliceStackStatus::empty,
		    0,
		    0,
		    0,
		    "NumberOfFrames"_tag,
		    "NumberOfFrames or PerFrameFunctionalGroupsSequence is missing",
		});
		return stack_analysis;
	}

	auto per_frame_sequence = resolve_per_frame_sequence(root);
	if (!per_frame_sequence.ok()) {
		stack_analysis.status_ = SliceStackStatus::missing_frame_content;
		stack_analysis.issues_.push_back(SliceStackIssue{
		    SliceStackStatus::missing_frame_content,
		    0,
		    0,
		    0,
		    "PerFrameFunctionalGroupsSequence"_tag,
		    per_frame_sequence.message(),
		});
		return stack_analysis;
	}

	auto dimension_descriptors = read_dimension_index_descriptors(root);
	if (!dimension_descriptors.ok()) {
		stack_analysis.status_ = SliceStackStatus::geometry_parse_failure;
		stack_analysis.issues_.push_back(SliceStackIssue{
		    SliceStackStatus::geometry_parse_failure,
		    0,
		    0,
		    0,
		    dimension_descriptors.tag(),
		    dimension_descriptors.message(),
		});
		return stack_analysis;
	}
	const bool geometry_grouping_fallback =
	    dimension_descriptors.value().empty() &&
	    options.allow_geometry_grouping_fallback;
	if (dimension_descriptors.value().empty() &&
	    !geometry_grouping_fallback) {
		stack_analysis.status_ = SliceStackStatus::missing_dimension_module;
		stack_analysis.issues_.push_back(SliceStackIssue{
		    SliceStackStatus::missing_dimension_module,
		    0,
		    0,
		    0,
		    "DimensionIndexSequence"_tag,
		    "DimensionIndexSequence is required for enhanced frame grouping",
		});
		return stack_analysis;
	}
	if (!per_frame_sequence.value() && !geometry_grouping_fallback) {
		stack_analysis.status_ = SliceStackStatus::missing_frame_content;
		stack_analysis.issues_.push_back(SliceStackIssue{
		    SliceStackStatus::missing_frame_content,
		    0,
		    0,
		    0,
		    "PerFrameFunctionalGroupsSequence"_tag,
		    "PerFrameFunctionalGroupsSequence is missing",
		});
		return stack_analysis;
	}

	std::vector<FrameStackBucket> buckets;
	buckets.reserve(1);
	std::unordered_map<ImageFrameStackKey, std::size_t, ImageFrameStackKeyHash,
	    ImageFrameStackKeyEqual>
	    bucket_indices;
	bucket_indices.reserve(1);

	auto add_issue = [&](SliceStackStatus status, std::size_t frame_index,
	                     Tag tag, std::string message) {
		stack_analysis.issues_.push_back(SliceStackIssue{
		    status,
		    frame_index,
		    0,
		    frame_index,
		    tag,
		    std::move(message),
		});
		if (slice_stack_status_priority(status) <
		    slice_stack_status_priority(stack_analysis.status_)) {
			stack_analysis.status_ = status;
		}
	};

	if (geometry_grouping_fallback) {
		ImageFrameStackKey key;
		key.stack_id = "geometry";
		FrameStackBucket bucket{std::move(key), {}};
		bucket.members.reserve(*frame_count);
		for (std::size_t frame_index = 0; frame_index < *frame_count; ++frame_index) {
			bucket.members.push_back(FrameStackMember{
			    frame_index,
			    static_cast<int>(frame_index + 1),
			});
		}
		buckets.push_back(std::move(bucket));
	} else {
		for (std::size_t frame_index = 0; frame_index < *frame_count; ++frame_index) {
			const DataSet* per_frame_item =
			    per_frame_sequence.value()->get_dataset(frame_index);
			if (!per_frame_item) {
				add_issue(SliceStackStatus::missing_frame_content, frame_index,
				    "PerFrameFunctionalGroupsSequence"_tag,
				    "frame index is outside PerFrameFunctionalGroupsSequence");
				continue;
			}
			auto frame_content_result = resolve_sequence_item_if_present(
			    *per_frame_item, "FrameContentSequence"_tag, 0);
			if (!frame_content_result.ok()) {
				add_issue(SliceStackStatus::geometry_parse_failure, frame_index,
				    frame_content_result.tag(),
				    std::string("malformed FrameContentSequence: ") +
				        frame_content_result.message());
				continue;
			}
			const DataSet* frame_content = frame_content_result.value();
			if (!frame_content) {
				add_issue(SliceStackStatus::missing_frame_content, frame_index,
				    "FrameContentSequence"_tag,
				    "missing FrameContentSequence item");
				continue;
			}

			auto stack_id = root_string_view(*frame_content, "StackID"_tag);
			if (!stack_id || stack_id->empty()) {
				add_issue(SliceStackStatus::missing_frame_content, frame_index,
				    "StackID"_tag, "missing StackID");
				continue;
			}
			const auto& position_element =
			    frame_content->get_dataelement(root_path("InStackPositionNumber"_tag));
			auto in_stack_position = position_element.to_int();
			if (!in_stack_position || *in_stack_position <= 0) {
				add_issue(SliceStackStatus::missing_frame_content, frame_index,
				    "InStackPositionNumber"_tag,
				    "missing or invalid InStackPositionNumber");
				continue;
			}
			auto dimension_values = read_non_spatial_dimension_values(
			    *frame_content, dimension_descriptors.value());
			if (!dimension_values.ok()) {
				add_issue(SliceStackStatus::geometry_parse_failure, frame_index,
				    dimension_values.tag(), dimension_values.message());
				continue;
			}

			ImageFrameStackKey key;
			key.stack_id = std::string(*stack_id);
			key.dimension_values = std::move(dimension_values).value();

			auto [bucket_position, inserted] =
			    bucket_indices.emplace(key, buckets.size());
			if (inserted) {
				buckets.push_back(FrameStackBucket{std::move(key), {}});
			}
			buckets[bucket_position->second].members.push_back(FrameStackMember{
			    frame_index,
			    *in_stack_position,
			});
		}
	}

	if (buckets.empty()) {
		if (stack_analysis.issues_.empty()) {
			stack_analysis.status_ = SliceStackStatus::empty;
			stack_analysis.issues_.push_back(SliceStackIssue{
			    SliceStackStatus::empty,
			    0,
			    0,
			    0,
			    "FrameContentSequence"_tag,
			    "no frame stack groups were found",
			});
		}
		return stack_analysis;
	}
	if (!stack_analysis.ok()) {
		return stack_analysis;
	}

	stack_analysis.groups_.reserve(buckets.size());
	for (auto& bucket : buckets) {
		std::sort(bucket.members.begin(), bucket.members.end(),
		    [](const FrameStackMember& a, const FrameStackMember& b) {
			    if (a.in_stack_position != b.in_stack_position) {
				    return a.in_stack_position < b.in_stack_position;
			    }
			    return a.frame_index < b.frame_index;
		    });
		ImageFrameStackGroup group;
		group.key = std::move(bucket.key);
		group.frame_indices.reserve(bucket.members.size());
		for (const auto& member : bucket.members) {
			group.frame_indices.push_back(member.frame_index);
		}
		group.analysis = analyze_image_frame_stack(
		    file,
		    std::span<const std::size_t>(
		        group.frame_indices.data(), group.frame_indices.size()),
		    options);
		for (const auto& issue : group.analysis.issues()) {
			if (slice_stack_status_priority(issue.status) <
			    slice_stack_status_priority(stack_analysis.status_)) {
				stack_analysis.status_ = issue.status;
			}
			stack_analysis.issues_.push_back(issue);
		}
		if (slice_stack_status_priority(group.analysis.status()) <
		    slice_stack_status_priority(stack_analysis.status_)) {
			stack_analysis.status_ = group.analysis.status();
		}
		stack_analysis.groups_.push_back(std::move(group));
	}
	return stack_analysis;
}

SliceStackAnalysis analyze_image_frame_stack(
    const DicomFile& file, ImageFrameStackOptions options) {
	auto stacks = analyze_image_frame_stacks(file, options);
	if (!stacks.ok()) {
		SliceStackAnalysis analysis;
		analysis.status_ = stacks.status();
		analysis.issues_ = stacks.issues();
		return analysis;
	}
	if (stacks.groups().empty()) {
		SliceStackAnalysis analysis;
		analysis.status_ = SliceStackStatus::empty;
		analysis.issues_ = stacks.issues();
		return analysis;
	}
	if (stacks.groups().size() != 1) {
		SliceStackAnalysis analysis;
		analysis.status_ = SliceStackStatus::multiple_frame_stacks;
		analysis.issues_ = stacks.issues();
		analysis.issues_.push_back(SliceStackIssue{
		    SliceStackStatus::multiple_frame_stacks,
		    0,
		    0,
		    0,
		    "StackID"_tag,
		    "image contains multiple frame stacks",
		});
		return analysis;
	}
	return stacks.groups().front().analysis;
}

SliceStackPlan plan_image_frame_stack(
    const DicomFile& file, ImageFrameStackOptions options) {
	auto stacks = analyze_image_frame_stacks(file, options);
	SliceStackPlan plan;
	if (!stacks.ok()) {
		plan.status_ = stacks.status();
		plan.issues_ = stacks.issues();
		return plan;
	}
	if (stacks.groups().empty()) {
		plan.status_ = SliceStackStatus::empty;
		plan.issues_ = stacks.issues();
		return plan;
	}
	if (stacks.groups().size() != 1) {
		plan.status_ = SliceStackStatus::multiple_frame_stacks;
		plan.issues_ = stacks.issues();
		plan.issues_.push_back(SliceStackIssue{
		    SliceStackStatus::multiple_frame_stacks,
		    0,
		    0,
		    0,
		    "StackID"_tag,
		    "image contains multiple frame stacks",
		});
		return plan;
	}

	const auto inputs =
	    make_slice_stack_inputs_from_analysis(stacks.groups().front().analysis);
	return plan_slice_stack(
	    std::span<const SliceStackInput>(inputs.data(), inputs.size()),
	    options.slice_stack);
}

struct Bounds2D {
	double min_i{std::numeric_limits<double>::infinity()};
	double max_i{-std::numeric_limits<double>::infinity()};
	double min_j{std::numeric_limits<double>::infinity()};
	double max_j{-std::numeric_limits<double>::infinity()};
};

struct Bounds3D {
	double min_i{std::numeric_limits<double>::infinity()};
	double max_i{-std::numeric_limits<double>::infinity()};
	double min_j{std::numeric_limits<double>::infinity()};
	double max_j{-std::numeric_limits<double>::infinity()};
	double min_k{std::numeric_limits<double>::infinity()};
	double max_k{-std::numeric_limits<double>::infinity()};
};

[[nodiscard]] double positive_tolerance(double value) noexcept {
	return finite(value) && value > 0.0 ? value : 0.0;
}

void include(Bounds2D& bounds, ImagePoint2D point) noexcept {
	bounds.min_i = std::min(bounds.min_i, point.i);
	bounds.max_i = std::max(bounds.max_i, point.i);
	bounds.min_j = std::min(bounds.min_j, point.j);
	bounds.max_j = std::max(bounds.max_j, point.j);
}

void include(Bounds3D& bounds, ImagePoint3D point) noexcept {
	bounds.min_i = std::min(bounds.min_i, point.i);
	bounds.max_i = std::max(bounds.max_i, point.i);
	bounds.min_j = std::min(bounds.min_j, point.j);
	bounds.max_j = std::max(bounds.max_j, point.j);
	bounds.min_k = std::min(bounds.min_k, point.k);
	bounds.max_k = std::max(bounds.max_k, point.k);
}

[[nodiscard]] bool range_overlaps_size(
    double min_value, double max_value, std::size_t size,
    double index_tolerance) noexcept {
	return max_value >= -0.5 - index_tolerance &&
	       min_value < static_cast<double>(size) - 0.5 + index_tolerance;
}

[[nodiscard]] bool range_inside_size(
    double min_value, double max_value, std::size_t size,
    double index_tolerance) noexcept {
	return min_value >= -0.5 - index_tolerance &&
	       max_value <= static_cast<double>(size) - 0.5 + index_tolerance;
}

[[nodiscard]] bool bounds_overlap(
    const Bounds2D& bounds, ImageSize2D size, double tolerance_i,
    double tolerance_j) noexcept {
	return range_overlaps_size(bounds.min_i, bounds.max_i, size.i, tolerance_i) &&
	       range_overlaps_size(bounds.min_j, bounds.max_j, size.j, tolerance_j);
}

[[nodiscard]] bool bounds_inside(
    const Bounds2D& bounds, ImageSize2D size, double tolerance_i,
    double tolerance_j) noexcept {
	return range_inside_size(bounds.min_i, bounds.max_i, size.i, tolerance_i) &&
	       range_inside_size(bounds.min_j, bounds.max_j, size.j, tolerance_j);
}

[[nodiscard]] bool bounds_overlap(
    const Bounds3D& bounds, ImageSize3D size, double tolerance_i,
    double tolerance_j, double tolerance_k) noexcept {
	return range_overlaps_size(bounds.min_i, bounds.max_i, size.i, tolerance_i) &&
	       range_overlaps_size(bounds.min_j, bounds.max_j, size.j, tolerance_j) &&
	       range_overlaps_size(bounds.min_k, bounds.max_k, size.k, tolerance_k);
}

[[nodiscard]] bool bounds_inside(
    const Bounds3D& bounds, ImageSize3D size, double tolerance_i,
    double tolerance_j, double tolerance_k) noexcept {
	return range_inside_size(bounds.min_i, bounds.max_i, size.i, tolerance_i) &&
	       range_inside_size(bounds.min_j, bounds.max_j, size.j, tolerance_j) &&
	       range_inside_size(bounds.min_k, bounds.max_k, size.k, tolerance_k);
}

[[nodiscard]] std::optional<IndexRange1D> index_range_for_bounds(
    double min_value, double max_value, std::size_t size,
    double index_tolerance) noexcept {
	const double expanded_min = min_value - index_tolerance;
	const double expanded_max = max_value + index_tolerance;
	const auto raw_begin =
	    static_cast<long long>(std::ceil(expanded_min - 0.5));
	const auto raw_end =
	    static_cast<long long>(std::ceil(expanded_max + 0.5));
	const auto clamped_begin =
	    std::clamp<long long>(raw_begin, 0, static_cast<long long>(size));
	const auto clamped_end =
	    std::clamp<long long>(raw_end, 0, static_cast<long long>(size));
	if (clamped_begin >= clamped_end) {
		return std::nullopt;
	}
	return IndexRange1D{
	    static_cast<std::size_t>(clamped_begin),
	    static_cast<std::size_t>(clamped_end),
	};
}

[[nodiscard]] std::array<Point3d, 4> plane_extent_corners(
    const ImagePlaneGeometry& plane) noexcept {
	const double max_i = static_cast<double>(plane.columns()) - 0.5;
	const double max_j = static_cast<double>(plane.rows()) - 0.5;
	return {{
	    plane.world_from_index({-0.5, -0.5}),
	    plane.world_from_index({max_i, -0.5}),
	    plane.world_from_index({-0.5, max_j}),
	    plane.world_from_index({max_i, max_j}),
	}};
}

[[nodiscard]] std::array<Point3d, 8> volume_extent_corners(
    const ImageVolumeGeometry& volume) noexcept {
	const double max_i = static_cast<double>(volume.columns()) - 0.5;
	const double max_j = static_cast<double>(volume.rows()) - 0.5;
	const double max_k = static_cast<double>(volume.slices()) - 0.5;
	return {{
	    volume.world_from_index({-0.5, -0.5, -0.5}),
	    volume.world_from_index({max_i, -0.5, -0.5}),
	    volume.world_from_index({-0.5, max_j, -0.5}),
	    volume.world_from_index({max_i, max_j, -0.5}),
	    volume.world_from_index({-0.5, -0.5, max_k}),
	    volume.world_from_index({max_i, -0.5, max_k}),
	    volume.world_from_index({-0.5, max_j, max_k}),
	    volume.world_from_index({max_i, max_j, max_k}),
	}};
}

[[nodiscard]] GeometryTolerance extent_tolerance(
    OverlayCheckOptions options) noexcept {
	GeometryTolerance tolerance;
	tolerance.position_tolerance_mm =
	    positive_tolerance(options.frame_position_tolerance_mm);
	tolerance.normal_distance_tolerance_mm =
	    positive_tolerance(options.normal_distance_tolerance_mm);
	tolerance.orientation_tolerance =
	    positive_tolerance(options.orientation_tolerance);
	tolerance.spacing_tolerance_mm =
	    positive_tolerance(options.spacing_tolerance_mm);
	return tolerance;
}

[[nodiscard]] bool same_frame_of_reference(
    OverlayCheck& check, std::string_view source_frame_of_reference_uid,
    std::string_view target_frame_of_reference_uid) noexcept {
	if (source_frame_of_reference_uid.empty() ||
	    target_frame_of_reference_uid.empty()) {
		check.status = OverlayCompatibility::missing_frame_of_reference;
		return false;
	}
	if (source_frame_of_reference_uid != target_frame_of_reference_uid) {
		check.status = OverlayCompatibility::different_frame_of_reference;
		return false;
	}
	check.same_frame_of_reference = true;
	check.can_transform = true;
	return true;
}

[[nodiscard]] OverlayCheck finalize_overlay_check(
    OverlayCheck check, OverlayCheckOptions options) noexcept {
	if (options.require_same_grid && !check.same_grid) {
		check.can_transform = false;
		check.can_direct_overlay = false;
	}
	return check;
}

OverlayCheck check_overlay_compatibility(
    std::string_view source_frame_of_reference_uid,
    const ImagePlaneGeometry& source,
    std::string_view target_frame_of_reference_uid,
    const ImagePlaneGeometry& target,
    OverlayCheckOptions options) {
	OverlayCheck check;
	if (!same_frame_of_reference(
	        check, source_frame_of_reference_uid, target_frame_of_reference_uid)) {
		return check;
	}

	const double normal_dot = dot(source.normal(), target.normal());
	const double axis_i_dot = dot(source.direction_i(), target.direction_i());
	const double axis_j_dot = dot(source.direction_j(), target.direction_j());
	const double normal_error = 1.0 - std::abs(normal_dot);
	const double axis_error = std::max(
	    1.0 - std::abs(axis_i_dot), 1.0 - std::abs(axis_j_dot));
	check.max_orientation_error = std::max(normal_error, axis_error);
	const double tolerance_i =
	    positive_tolerance(options.frame_position_tolerance_mm) /
	    target.spacing_i();
	const double tolerance_j =
	    positive_tolerance(options.frame_position_tolerance_mm) /
	    target.spacing_j();
	Bounds2D target_bounds;
	for (const auto corner : plane_extent_corners(source)) {
		include(target_bounds, target.index_from_world(corner));
	}
	check.overlaps_extent =
	    bounds_overlap(target_bounds, target.size(), tolerance_i, tolerance_j);
	check.source_inside_target_extent =
	    bounds_inside(target_bounds, target.size(), tolerance_i, tolerance_j);

	if (normal_error > options.orientation_tolerance) {
		check.status = OverlayCompatibility::non_parallel_planes;
		check.requires_resampling = check.overlaps_extent;
		return finalize_overlay_check(check, options);
	}
	if (normal_dot < 0.0 || axis_i_dot < 0.0 || axis_j_dot < 0.0) {
		check.status = OverlayCompatibility::opposite_orientation;
		check.requires_resampling = check.overlaps_extent;
		return finalize_overlay_check(check, options);
	}

	check.max_normal_distance_mm =
	    std::abs(dot(target.origin() - source.origin(), source.normal()));
	if (check.max_normal_distance_mm > options.normal_distance_tolerance_mm) {
		check.status = OverlayCompatibility::out_of_plane;
		check.overlaps_extent = false;
		check.source_inside_target_extent = false;
		return finalize_overlay_check(check, options);
	}
	if (!check.overlaps_extent) {
		check.status = OverlayCompatibility::different_extent;
		return finalize_overlay_check(check, options);
	}

	const auto source_spacing = source.spacing();
	const auto target_spacing = target.spacing();
	check.max_spacing_error_mm = std::max(
	    std::abs(source_spacing.i - target_spacing.i),
	    std::abs(source_spacing.j - target_spacing.j));
	check.same_spacing =
	    check.max_spacing_error_mm <= options.spacing_tolerance_mm;
	check.same_extent = source.size().i == target.size().i &&
	                    source.size().j == target.size().j;

	const auto origin_delta = target.origin() - source.origin();
	const double in_plane_i_mm =
	    std::abs(dot(origin_delta, source.direction_i()));
	const double in_plane_j_mm =
	    std::abs(dot(origin_delta, source.direction_j()));
	check.max_position_error_mm = std::max(in_plane_i_mm, in_plane_j_mm);
	const bool same_origin =
	    check.max_position_error_mm <= options.frame_position_tolerance_mm;
	const bool same_positive_orientation =
	    normal_dot >= 0.0 && axis_i_dot >= 0.0 && axis_j_dot >= 0.0 &&
	    check.max_orientation_error <= options.orientation_tolerance;
	check.same_grid = same_positive_orientation && check.same_spacing &&
	                  check.same_extent && same_origin;
	check.can_direct_overlay = check.same_grid;

	if (check.can_direct_overlay) {
		check.status = OverlayCompatibility::compatible;
		return finalize_overlay_check(check, options);
	}
	if (!check.same_spacing) {
		check.status = OverlayCompatibility::different_spacing;
		check.requires_resampling = true;
		return finalize_overlay_check(check, options);
	}
	if (!check.same_extent) {
		check.status = OverlayCompatibility::different_extent;
		return finalize_overlay_check(check, options);
	}
	if (axis_error > options.orientation_tolerance) {
		check.status = OverlayCompatibility::requires_resampling;
		check.requires_resampling = true;
		return finalize_overlay_check(check, options);
	}
	check.status = OverlayCompatibility::requires_resampling;
	check.requires_resampling = true;
	return finalize_overlay_check(check, options);
}

OverlayCheck check_overlay_compatibility(
    std::string_view source_frame_of_reference_uid,
    const ImagePlaneGeometry& source,
    std::string_view target_frame_of_reference_uid,
    const ImageVolumeGeometry& target,
    OverlayCheckOptions options) {
	OverlayCheck check;
	if (!same_frame_of_reference(
	        check, source_frame_of_reference_uid, target_frame_of_reference_uid)) {
		return check;
	}

	const double normal_dot = dot(source.normal(), target.direction_k());
	const double axis_i_dot = dot(source.direction_i(), target.direction_i());
	const double axis_j_dot = dot(source.direction_j(), target.direction_j());
	const double normal_error = 1.0 - std::abs(normal_dot);
	const double axis_error = std::max(
	    1.0 - std::abs(axis_i_dot), 1.0 - std::abs(axis_j_dot));
	check.max_orientation_error = std::max(normal_error, axis_error);

	const double tolerance_i =
	    positive_tolerance(options.frame_position_tolerance_mm) /
	    target.spacing_i();
	const double tolerance_j =
	    positive_tolerance(options.frame_position_tolerance_mm) /
	    target.spacing_j();
	const double tolerance_k =
	    positive_tolerance(options.normal_distance_tolerance_mm) /
	    target.spacing_k();
	Bounds3D target_bounds;
	for (const auto corner : plane_extent_corners(source)) {
		include(target_bounds, target.index_from_world(corner));
	}
	check.overlaps_extent = bounds_overlap(
	    target_bounds, target.size(), tolerance_i, tolerance_j, tolerance_k);
	check.source_inside_target_extent = bounds_inside(
	    target_bounds, target.size(), tolerance_i, tolerance_j, tolerance_k);
	if (check.overlaps_extent) {
		check.target_k_range = index_range_for_bounds(
		    target_bounds.min_k, target_bounds.max_k, target.slices(),
		    tolerance_k);
	}

	const auto source_spacing = source.spacing();
	const auto target_spacing = target.spacing();
	check.max_spacing_error_mm = std::max(
	    std::abs(source_spacing.i - target_spacing.i),
	    std::abs(source_spacing.j - target_spacing.j));
	check.same_spacing =
	    check.max_spacing_error_mm <= options.spacing_tolerance_mm;
	check.same_extent = source.columns() == target.columns() &&
	                    source.rows() == target.rows();

	const auto origin_index = target.index_from_world(source.origin());
	const double nearest_k = std::round(origin_index.k);
	const double origin_i_mm = std::abs(origin_index.i * target.spacing_i());
	const double origin_j_mm = std::abs(origin_index.j * target.spacing_j());
	check.max_position_error_mm = std::max(origin_i_mm, origin_j_mm);
	check.max_normal_distance_mm =
	    std::abs((origin_index.k - nearest_k) * target.spacing_k());
	const bool same_origin =
	    check.max_position_error_mm <= options.frame_position_tolerance_mm &&
	    check.max_normal_distance_mm <= options.normal_distance_tolerance_mm;
	const bool same_positive_orientation =
	    normal_dot >= 0.0 && axis_i_dot >= 0.0 && axis_j_dot >= 0.0 &&
	    check.max_orientation_error <= options.orientation_tolerance;
	check.same_grid = same_positive_orientation && check.same_spacing &&
	                  check.same_extent && same_origin && check.target_k_range &&
	                  check.target_k_range->end == check.target_k_range->begin + 1;
	check.can_direct_overlay = check.same_grid && check.overlaps_extent;

	if (normal_error > options.orientation_tolerance) {
		check.status = OverlayCompatibility::non_parallel_planes;
		check.requires_resampling = check.overlaps_extent;
		return finalize_overlay_check(check, options);
	}
	if (normal_dot < 0.0 || axis_i_dot < 0.0 || axis_j_dot < 0.0) {
		check.status = OverlayCompatibility::opposite_orientation;
		check.requires_resampling = check.overlaps_extent;
		return finalize_overlay_check(check, options);
	}
	if (!check.overlaps_extent) {
		check.status = OverlayCompatibility::different_extent;
		return finalize_overlay_check(check, options);
	}
	if (!check.same_spacing) {
		check.status = OverlayCompatibility::different_spacing;
		check.requires_resampling = true;
		return finalize_overlay_check(check, options);
	}
	if (!check.same_extent) {
		check.status = OverlayCompatibility::different_extent;
		return finalize_overlay_check(check, options);
	}
	if (axis_error > options.orientation_tolerance) {
		check.status = OverlayCompatibility::requires_resampling;
		check.requires_resampling = true;
		return finalize_overlay_check(check, options);
	}
	if (check.can_direct_overlay) {
		check.status = OverlayCompatibility::compatible;
		return finalize_overlay_check(check, options);
	}
	check.status = OverlayCompatibility::requires_resampling;
	check.requires_resampling = true;
	return finalize_overlay_check(check, options);
}

OverlayCheck check_overlay_compatibility(
    std::string_view source_frame_of_reference_uid,
    const ImageVolumeGeometry& source,
    std::string_view target_frame_of_reference_uid,
    const ImagePlaneGeometry& target,
    OverlayCheckOptions options) {
	OverlayCheck check;
	if (!same_frame_of_reference(
	        check, source_frame_of_reference_uid, target_frame_of_reference_uid)) {
		return check;
	}

	const double normal_dot = dot(source.direction_k(), target.normal());
	const double axis_i_dot = dot(source.direction_i(), target.direction_i());
	const double axis_j_dot = dot(source.direction_j(), target.direction_j());
	const double normal_error = 1.0 - std::abs(normal_dot);
	const double axis_error = std::max(
	    1.0 - std::abs(axis_i_dot), 1.0 - std::abs(axis_j_dot));
	check.max_orientation_error = std::max(normal_error, axis_error);

	const double source_tolerance_i =
	    positive_tolerance(options.frame_position_tolerance_mm) /
	    source.spacing_i();
	const double source_tolerance_j =
	    positive_tolerance(options.frame_position_tolerance_mm) /
	    source.spacing_j();
	const double source_tolerance_k =
	    positive_tolerance(options.normal_distance_tolerance_mm) /
	    source.spacing_k();
	Bounds3D source_bounds;
	for (const auto corner : plane_extent_corners(target)) {
		include(source_bounds, source.index_from_world(corner));
	}
	check.overlaps_extent = bounds_overlap(source_bounds, source.size(),
	    source_tolerance_i, source_tolerance_j, source_tolerance_k);

	const double target_tolerance_i =
	    positive_tolerance(options.frame_position_tolerance_mm) /
	    target.spacing_i();
	const double target_tolerance_j =
	    positive_tolerance(options.frame_position_tolerance_mm) /
	    target.spacing_j();
	Bounds2D target_bounds;
	for (const auto corner : volume_extent_corners(source)) {
		include(target_bounds, target.index_from_world(corner));
	}
	check.source_inside_target_extent = bounds_inside(
	    target_bounds, target.size(), target_tolerance_i, target_tolerance_j);

	const auto source_spacing = source.spacing();
	const auto target_spacing = target.spacing();
	check.max_spacing_error_mm = std::max(
	    std::abs(source_spacing.i - target_spacing.i),
	    std::abs(source_spacing.j - target_spacing.j));
	check.same_spacing =
	    check.max_spacing_error_mm <= options.spacing_tolerance_mm;
	check.same_extent = source.columns() == target.columns() &&
	                    source.rows() == target.rows();

	const auto origin_index = source.index_from_world(target.origin());
	const double nearest_k = std::round(origin_index.k);
	const double origin_i_mm = std::abs(origin_index.i * source.spacing_i());
	const double origin_j_mm = std::abs(origin_index.j * source.spacing_j());
	check.max_position_error_mm = std::max(origin_i_mm, origin_j_mm);
	check.max_normal_distance_mm =
	    std::abs((origin_index.k - nearest_k) * source.spacing_k());
	const bool same_origin =
	    check.max_position_error_mm <= options.frame_position_tolerance_mm &&
	    check.max_normal_distance_mm <= options.normal_distance_tolerance_mm;
	const auto source_k_range = index_range_for_bounds(
	    source_bounds.min_k, source_bounds.max_k, source.slices(),
	    source_tolerance_k);
	const bool same_positive_orientation =
	    normal_dot >= 0.0 && axis_i_dot >= 0.0 && axis_j_dot >= 0.0 &&
	    check.max_orientation_error <= options.orientation_tolerance;
	check.same_grid = same_positive_orientation && check.same_spacing &&
	                  check.same_extent && same_origin && source_k_range &&
	                  source_k_range->end == source_k_range->begin + 1;
	check.can_direct_overlay = check.same_grid && check.overlaps_extent;

	if (normal_error > options.orientation_tolerance) {
		check.status = OverlayCompatibility::non_parallel_planes;
		check.requires_resampling = check.overlaps_extent;
		return finalize_overlay_check(check, options);
	}
	if (normal_dot < 0.0 || axis_i_dot < 0.0 || axis_j_dot < 0.0) {
		check.status = OverlayCompatibility::opposite_orientation;
		check.requires_resampling = check.overlaps_extent;
		return finalize_overlay_check(check, options);
	}
	if (!check.overlaps_extent) {
		check.status = OverlayCompatibility::different_extent;
		return finalize_overlay_check(check, options);
	}
	if (!check.same_spacing) {
		check.status = OverlayCompatibility::different_spacing;
		check.requires_resampling = true;
		return finalize_overlay_check(check, options);
	}
	if (!check.same_extent) {
		check.status = OverlayCompatibility::different_extent;
		return finalize_overlay_check(check, options);
	}
	if (axis_error > options.orientation_tolerance) {
		check.status = OverlayCompatibility::requires_resampling;
		check.requires_resampling = true;
		return finalize_overlay_check(check, options);
	}
	if (check.can_direct_overlay) {
		check.status = OverlayCompatibility::compatible;
		return finalize_overlay_check(check, options);
	}
	check.status = OverlayCompatibility::requires_resampling;
	check.requires_resampling = true;
	return finalize_overlay_check(check, options);
}

OverlayCheck check_overlay_compatibility(
    std::string_view source_frame_of_reference_uid,
    const ImageVolumeGeometry& source,
    std::string_view target_frame_of_reference_uid,
    const ImageVolumeGeometry& target,
    OverlayCheckOptions options) {
	OverlayCheck check;
	if (!same_frame_of_reference(
	        check, source_frame_of_reference_uid, target_frame_of_reference_uid)) {
		return check;
	}

	const double axis_i_dot = dot(source.direction_i(), target.direction_i());
	const double axis_j_dot = dot(source.direction_j(), target.direction_j());
	const double axis_k_dot = dot(source.direction_k(), target.direction_k());
	check.max_orientation_error = std::max({
	    1.0 - std::abs(axis_i_dot),
	    1.0 - std::abs(axis_j_dot),
	    1.0 - std::abs(axis_k_dot),
	});

	const double tolerance_i =
	    positive_tolerance(options.frame_position_tolerance_mm) /
	    target.spacing_i();
	const double tolerance_j =
	    positive_tolerance(options.frame_position_tolerance_mm) /
	    target.spacing_j();
	const double tolerance_k =
	    positive_tolerance(options.normal_distance_tolerance_mm) /
	    target.spacing_k();
	Bounds3D target_bounds;
	for (const auto corner : volume_extent_corners(source)) {
		include(target_bounds, target.index_from_world(corner));
	}
	check.overlaps_extent = bounds_overlap(
	    target_bounds, target.size(), tolerance_i, tolerance_j, tolerance_k);
	check.source_inside_target_extent = bounds_inside(
	    target_bounds, target.size(), tolerance_i, tolerance_j, tolerance_k);
	if (check.overlaps_extent) {
		check.target_k_range = index_range_for_bounds(
		    target_bounds.min_k, target_bounds.max_k, target.slices(),
		    tolerance_k);
	}

	const auto source_spacing = source.spacing();
	const auto target_spacing = target.spacing();
	check.max_spacing_error_mm = std::max({
	    std::abs(source_spacing.i - target_spacing.i),
	    std::abs(source_spacing.j - target_spacing.j),
	    std::abs(source_spacing.k - target_spacing.k),
	});
	check.same_spacing =
	    check.max_spacing_error_mm <= options.spacing_tolerance_mm;
	check.same_extent = source.size().i == target.size().i &&
	                    source.size().j == target.size().j &&
	                    source.size().k == target.size().k;

	const auto origin_index = target.index_from_world(source.origin());
	const double origin_i_mm = std::abs(origin_index.i * target.spacing_i());
	const double origin_j_mm = std::abs(origin_index.j * target.spacing_j());
	const double origin_k_mm = std::abs(origin_index.k * target.spacing_k());
	check.max_position_error_mm =
	    std::max({origin_i_mm, origin_j_mm, origin_k_mm});
	check.max_normal_distance_mm = origin_k_mm;
	const bool same_origin =
	    check.max_position_error_mm <= options.frame_position_tolerance_mm;
	const bool same_positive_orientation =
	    axis_i_dot >= 0.0 && axis_j_dot >= 0.0 && axis_k_dot >= 0.0 &&
	    check.max_orientation_error <= options.orientation_tolerance;
	check.same_grid = same_positive_orientation && check.same_spacing &&
	                  check.same_extent && same_origin;
	check.can_direct_overlay = check.same_grid && check.overlaps_extent;

	if (check.max_orientation_error > options.orientation_tolerance) {
		check.status = OverlayCompatibility::non_parallel_planes;
		check.requires_resampling = check.overlaps_extent;
		return finalize_overlay_check(check, options);
	}
	if (axis_i_dot < 0.0 || axis_j_dot < 0.0 || axis_k_dot < 0.0) {
		check.status = OverlayCompatibility::opposite_orientation;
		check.requires_resampling = check.overlaps_extent;
		return finalize_overlay_check(check, options);
	}
	if (!check.overlaps_extent) {
		check.status = OverlayCompatibility::different_extent;
		return finalize_overlay_check(check, options);
	}
	if (!check.same_spacing) {
		check.status = OverlayCompatibility::different_spacing;
		check.requires_resampling = true;
		return finalize_overlay_check(check, options);
	}
	if (!check.same_extent) {
		check.status = OverlayCompatibility::different_extent;
		return finalize_overlay_check(check, options);
	}
	if (check.can_direct_overlay) {
		check.status = OverlayCompatibility::compatible;
		return finalize_overlay_check(check, options);
	}
	check.status = OverlayCompatibility::requires_resampling;
	check.requires_resampling = true;
	return finalize_overlay_check(check, options);
}

} // namespace dicom::geometry
