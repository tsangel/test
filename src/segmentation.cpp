#include "dicom_seg.h"

#include "diagnostics.h"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

namespace dicom::seg {
using namespace dicom::literals;

namespace {

// Small typed readers used by the SEG adapter. They intentionally keep the
// source DataSet as the single source of truth and avoid copying text values.
[[nodiscard]] bool text_equals(std::string_view lhs, std::string_view rhs) noexcept {
	return lhs == rhs;
}

[[nodiscard]] std::optional<std::string_view> string_value(
    const DataSet& dataset, Tag tag) {
	return dataset.get_dataelement(tag).to_string_view();
}

[[nodiscard]] std::string_view string_value_or_empty(
    const DataSet& dataset, Tag tag) {
	return string_value(dataset, tag).value_or(std::string_view{});
}

[[nodiscard]] const Sequence* sequence_value(
    const DataSet& dataset, Tag tag) {
	const auto& element = dataset.get_dataelement(tag);
	return element ? element.as_sequence() : nullptr;
}

[[nodiscard]] const DataSet* sequence_item(
    const DataSet& dataset, Tag sequence_tag, std::size_t index) {
	const auto* sequence = sequence_value(dataset, sequence_tag);
	if (!sequence || index >= static_cast<std::size_t>(sequence->size())) {
		return nullptr;
	}
	return sequence->get_dataset(index);
}

[[nodiscard]] const Sequence* functional_group_sequence(
    const DataSet& frame_item, const DataSet* shared_item, Tag sequence_tag) {
	// Functional group macros may be repeated per-frame or placed once in
	// SharedFunctionalGroupsSequence. Per-frame values override shared values.
	if (const auto* sequence = sequence_value(frame_item, sequence_tag)) {
		if (sequence->size() > 0) {
			return sequence;
		}
	}
	if (shared_item) {
		if (const auto* sequence = sequence_value(*shared_item, sequence_tag)) {
			if (sequence->size() > 0) {
				return sequence;
			}
		}
	}
	return nullptr;
}

[[nodiscard]] const DataSet* functional_group_first_item(
    const DataSet& frame_item, const DataSet* shared_item, Tag sequence_tag) {
	const auto* sequence = functional_group_sequence(frame_item, shared_item, sequence_tag);
	return sequence ? sequence->get_dataset(0) : nullptr;
}

[[nodiscard]] std::optional<std::uint16_t> uint16_value(
    const DataElement& element) {
	auto values = element.to_long_vector();
	if (values && !values->empty()) {
		if (values->size() != 1) {
			return std::nullopt;
		}
		const auto value = values->front();
		if (value < 0 || value > std::numeric_limits<std::uint16_t>::max()) {
			return std::nullopt;
		}
		return static_cast<std::uint16_t>(value);
	}
	if (const auto value = element.to_long()) {
		if (*value < 0 || *value > std::numeric_limits<std::uint16_t>::max()) {
			return std::nullopt;
		}
		return static_cast<std::uint16_t>(*value);
	}
	return std::nullopt;
}

[[nodiscard]] std::optional<std::uint16_t> uint16_value(
    const DataSet& dataset, Tag tag) {
	return uint16_value(dataset.get_dataelement(tag));
}

[[nodiscard]] std::optional<std::size_t> size_value(
    const DataSet& dataset, Tag tag) {
	if (const auto value = dataset.get_dataelement(tag).to_long()) {
		if (*value < 0) {
			return std::nullopt;
		}
		return static_cast<std::size_t>(*value);
	}
	return std::nullopt;
}

[[nodiscard]] std::optional<std::uint16_t> referenced_segment_number(
    const DataSet& frame_item, const DataSet* shared_item) {
	const auto* segment_id_item = functional_group_first_item(
	    frame_item, shared_item, "SegmentIdentificationSequence"_tag);
	if (!segment_id_item) {
		return std::nullopt;
	}
	return uint16_value(*segment_id_item, "ReferencedSegmentNumber"_tag);
}

[[nodiscard]] std::vector<std::uint32_t> uint32_values(
    const DataElement& element) {
	std::vector<std::uint32_t> result;
	if (auto values = element.to_long_vector()) {
		result.reserve(values->size());
		for (const auto value : *values) {
			if (value >= 0 &&
			    static_cast<unsigned long long>(value) <=
			        std::numeric_limits<std::uint32_t>::max()) {
				result.push_back(static_cast<std::uint32_t>(value));
			}
		}
		return result;
	}
	if (const auto value = element.to_long();
	    value && *value >= 0 &&
	    static_cast<unsigned long long>(*value) <=
	        std::numeric_limits<std::uint32_t>::max()) {
		result.push_back(static_cast<std::uint32_t>(*value));
	}
	return result;
}

template <std::size_t N>
[[nodiscard]] std::optional<std::array<double, N>> double_array_value(
    const DataSet& dataset, Tag tag) {
	const auto values = dataset.get_dataelement(tag).to_double_vector();
	if (!values || values->size() < N) {
		return std::nullopt;
	}
	std::array<double, N> out{};
	std::copy_n(values->begin(), N, out.begin());
	return out;
}

[[nodiscard]] std::optional<CodeView> code_view_from_sequence(
    const DataSet& dataset, Tag sequence_tag) {
	// MVP accessors expose the first code item. Multi-item code semantics can be
	// added later without changing the raw dataset escape hatch.
	const auto* item = sequence_item(dataset, sequence_tag, 0);
	if (!item) {
		return std::nullopt;
	}
	return CodeView{
	    .value = string_value_or_empty(*item, "CodeValue"_tag),
	    .scheme_designator =
	        string_value_or_empty(*item, "CodingSchemeDesignator"_tag),
	    .scheme_version =
	        string_value_or_empty(*item, "CodingSchemeVersion"_tag),
	    .meaning = string_value_or_empty(*item, "CodeMeaning"_tag),
	};
}

[[nodiscard]] SegmentationType parse_segmentation_type(
    std::string_view value) noexcept {
	if (text_equals(value, "BINARY")) {
		return SegmentationType::binary;
	}
	if (text_equals(value, "FRACTIONAL")) {
		return SegmentationType::fractional;
	}
	if (text_equals(value, "LABELMAP")) {
		return SegmentationType::labelmap;
	}
	return SegmentationType::unknown;
}

[[nodiscard]] SegmentationFractionalType parse_fractional_type(
    std::string_view value) noexcept {
	if (text_equals(value, "PROBABILITY")) {
		return SegmentationFractionalType::probability;
	}
	if (text_equals(value, "OCCUPANCY")) {
		return SegmentationFractionalType::occupancy;
	}
	if (value.empty()) {
		return SegmentationFractionalType::none;
	}
	return SegmentationFractionalType::unknown;
}

[[nodiscard]] SegmentAlgorithmType parse_algorithm_type(
    std::string_view value) noexcept {
	if (text_equals(value, "AUTOMATIC")) {
		return SegmentAlgorithmType::automatic_;
	}
	if (text_equals(value, "SEMIAUTOMATIC")) {
		return SegmentAlgorithmType::semiautomatic;
	}
	if (text_equals(value, "MANUAL")) {
		return SegmentAlgorithmType::manual;
	}
	return SegmentAlgorithmType::unknown;
}

[[nodiscard]] bool dataset_has_segmentation_sop_class(const DataSet& dataset) {
	const auto segmentation_storage = "SegmentationStorage"_uid.value();
	if (const auto sop_class_uid = string_value(dataset, "SOPClassUID"_tag);
	    sop_class_uid && *sop_class_uid == segmentation_storage) {
		return true;
	}
	if (const auto media_sop_class_uid =
	        string_value(dataset, "MediaStorageSOPClassUID"_tag);
	    media_sop_class_uid &&
	    *media_sop_class_uid == segmentation_storage) {
		return true;
	}
	return false;
}

[[nodiscard]] bool dataset_has_labelmap_segmentation_sop_class(
    const DataSet& dataset) {
	const auto labelmap_segmentation_storage =
	    "LabelMapSegmentationStorage"_uid.value();
	if (const auto sop_class_uid = string_value(dataset, "SOPClassUID"_tag);
	    sop_class_uid && *sop_class_uid == labelmap_segmentation_storage) {
		return true;
	}
	if (const auto media_sop_class_uid =
	        string_value(dataset, "MediaStorageSOPClassUID"_tag);
	    media_sop_class_uid &&
	    *media_sop_class_uid == labelmap_segmentation_storage) {
		return true;
	}
	return false;
}

[[noreturn]] void throw_seg(std::string_view reason) {
	diag::error_and_throw("seg::from_dicomfile reason={}", reason);
}

[[noreturn]] void throw_decode(std::string_view reason) {
	diag::error_and_throw("seg::Segmentation::decode_frame_into reason={}", reason);
}

void collect_source_image_refs(
    const DataSet& frame_item, const DataSet* shared_item,
    detail::SegmentFrameRecord& frame) {
	// SourceImageSequence is nested under DerivationImageSequence in SEG. These
	// references are provenance metadata, so they are indexed but not used to
	// decide spatial overlay compatibility.
	const auto* derivation_sequence = functional_group_sequence(
	    frame_item, shared_item, "DerivationImageSequence"_tag);
	if (!derivation_sequence) {
		return;
	}
	for (std::size_t derivation_index = 0;
	     derivation_index < static_cast<std::size_t>(derivation_sequence->size());
	     ++derivation_index) {
		const auto* derivation_item = derivation_sequence->get_dataset(derivation_index);
		if (!derivation_item) {
			continue;
		}
		const auto* source_sequence =
		    sequence_value(*derivation_item, "SourceImageSequence"_tag);
		if (!source_sequence) {
			continue;
		}
		for (std::size_t source_index = 0;
		     source_index < static_cast<std::size_t>(source_sequence->size());
		     ++source_index) {
			const auto* source_item = source_sequence->get_dataset(source_index);
			if (!source_item) {
				continue;
			}
			frame.source_images.push_back(detail::SourceImageRefRecord{
			    .item = source_item,
			    .referenced_frame_numbers = uint32_values(
			        source_item->get_dataelement("ReferencedFrameNumber"_tag)),
			});
		}
	}
}

} // namespace

void Segmentation::index_segment_sequence_items(const Options& options) {
	// Build the SegmentNumber catalog first; frame indexing depends on this map
	// to validate ReferencedSegmentNumber values and to group frames by segment.
	const auto& dataset = file_->dataset();
	const auto* segment_sequence = sequence_value(dataset, "SegmentSequence"_tag);
	if (!segment_sequence || segment_sequence->size() <= 0) {
		if (options.validate_required_modules) {
			throw_seg("SegmentSequence is missing or empty");
		}
		return;
	}

	index_.segments.reserve(static_cast<std::size_t>(segment_sequence->size()));
	for (std::size_t index = 0;
	     index < static_cast<std::size_t>(segment_sequence->size()); ++index) {
		const auto* item = segment_sequence->get_dataset(index);
		if (!item) {
			if (options.validate_required_modules) {
				throw_seg("SegmentSequence contains a missing item");
			}
			continue;
		}
		const auto number = uint16_value(*item, "SegmentNumber"_tag);
		if (!number || *number == 0) {
			if (options.validate_required_modules) {
				throw_seg("SegmentSequence item is missing SegmentNumber");
			}
			continue;
		}
		if (index_.segment_index_by_number.contains(*number)) {
			if (options.validate_required_modules) {
				throw_seg("SegmentSequence contains duplicate SegmentNumber");
			}
			continue;
		}
		index_.segment_index_by_number.emplace(*number, index_.segments.size());
		index_.segments.push_back(detail::SegmentRecord{.item = item, .number = *number});
	}
}

void Segmentation::index_per_frame_functional_group_items(const Options& options) {
	// Keep frame order identical to PixelData/PerFrameFunctionalGroupsSequence.
	// Segment-specific views store only indices into this canonical frame list.
	const auto& dataset = file_->dataset();
	const auto* per_frame_sequence =
	    sequence_value(dataset, "PerFrameFunctionalGroupsSequence"_tag);
	const auto per_frame_count =
	    per_frame_sequence ? static_cast<std::size_t>(per_frame_sequence->size()) : 0u;
	const auto declared_frame_count = size_value(dataset, "NumberOfFrames"_tag);
	const std::size_t frame_count = declared_frame_count.value_or(per_frame_count);

	if (frame_count == 0) {
		if (options.validate_required_modules) {
			throw_seg("NumberOfFrames or PerFrameFunctionalGroupsSequence is missing");
		}
		return;
	}
	if (!per_frame_sequence || per_frame_count < frame_count) {
		if (options.validate_required_modules) {
			throw_seg("PerFrameFunctionalGroupsSequence has fewer items than frames");
		}
	}
	if (declared_frame_count && per_frame_sequence && per_frame_count != *declared_frame_count &&
	    options.validate_required_modules) {
		throw_seg("NumberOfFrames does not match PerFrameFunctionalGroupsSequence item count");
	}

	index_.frames.reserve(frame_count);
	for (std::size_t frame_index = 0; frame_index < frame_count; ++frame_index) {
		const auto* frame_item =
		    per_frame_sequence && frame_index < per_frame_count
		        ? per_frame_sequence->get_dataset(frame_index)
		        : nullptr;
		if (!frame_item) {
			if (options.validate_required_modules) {
				throw_seg("PerFrameFunctionalGroupsSequence contains a missing item");
			}
			index_.frames.push_back(detail::SegmentFrameRecord{});
			continue;
		}

		const auto segment_number =
		    referenced_segment_number(*frame_item, shared_functional_groups_item_);
		if (!segment_number || *segment_number == 0) {
			if (options.validate_required_modules) {
				throw_seg("frame is missing a single ReferencedSegmentNumber");
			}
			index_.frames.push_back(
			    detail::SegmentFrameRecord{.functional_group_item = frame_item});
			continue;
		}
		if (!index_.segment_index_by_number.contains(*segment_number) &&
		    options.validate_required_modules) {
			throw_seg("frame references an undefined segment number");
		}

		detail::SegmentFrameRecord frame{
		    .functional_group_item = frame_item,
		    .referenced_segment_number = *segment_number,
		};
		collect_source_image_refs(*frame_item, shared_functional_groups_item_, frame);
		index_.frame_indices_by_segment[*segment_number].push_back(frame_index);
		index_.frames.push_back(std::move(frame));
	}
}

void Segmentation::extract_instance_metadata(const Options& options) {
	// Cache instance-level values that are cheap and central to the API. Segment
	// and frame attributes remain lazy through their borrowed DataSet views.
	auto& dataset = file_->dataset();

	segmentation_type_ = parse_segmentation_type(
	    string_value_or_empty(dataset, "SegmentationType"_tag));
	fractional_type_ = parse_fractional_type(
	    string_value_or_empty(dataset, "SegmentationFractionalType"_tag));
	maximum_fractional_value_ = uint16_value(dataset, "MaximumFractionalValue"_tag);
	frame_of_reference_uid_ = string_value(dataset, "FrameOfReferenceUID"_tag);
	rows_ = size_value(dataset, "Rows"_tag).value_or(0);
	columns_ = size_value(dataset, "Columns"_tag).value_or(0);

	if (segmentation_type_ == SegmentationType::unknown &&
	    options.validate_required_modules) {
		throw_seg("SegmentationType is missing or unsupported");
	}
	if (segmentation_type_ == SegmentationType::labelmap &&
	    options.validate_required_modules) {
		throw_seg("LABELMAP SEG is outside the SEG MVP scope");
	}
	if (!frame_of_reference_uid_ && options.validate_required_modules) {
		throw_seg("FrameOfReferenceUID is missing");
	}
	if ((rows_ == 0 || columns_ == 0) && options.validate_required_modules) {
		throw_seg("Rows or Columns is missing");
	}
	if (segmentation_type_ == SegmentationType::fractional &&
	    options.validate_required_modules) {
		if (fractional_type_ == SegmentationFractionalType::none ||
		    fractional_type_ == SegmentationFractionalType::unknown) {
			throw_seg("FRACTIONAL SEG requires SegmentationFractionalType");
		}
		if (!maximum_fractional_value_ || *maximum_fractional_value_ == 0) {
			throw_seg("FRACTIONAL SEG requires MaximumFractionalValue");
		}
	}

	const auto* shared_sequence =
	    sequence_value(dataset, "SharedFunctionalGroupsSequence"_tag);
	if (!shared_sequence || shared_sequence->size() != 1) {
		if (options.validate_required_modules) {
			throw_seg("SharedFunctionalGroupsSequence must contain exactly one item");
		}
	} else {
		shared_functional_groups_item_ = shared_sequence->get_dataset(0);
	}
}

Segmentation::~Segmentation() = default;

Segmentation::Segmentation(std::unique_ptr<DicomFile> file) noexcept
    : file_(std::move(file)) {}

bool Segmentation::is_valid() const noexcept {
	return static_cast<bool>(file_);
}

SegmentationType Segmentation::segmentation_type() const noexcept {
	return segmentation_type_;
}

SegmentationFractionalType Segmentation::fractional_type() const noexcept {
	return fractional_type_;
}

std::optional<std::uint16_t> Segmentation::maximum_fractional_value() const noexcept {
	return maximum_fractional_value_;
}

std::optional<std::string_view> Segmentation::frame_of_reference_uid() const noexcept {
	return frame_of_reference_uid_;
}

const DataSet& Segmentation::shared_functional_groups_item() const {
	if (!shared_functional_groups_item_) {
		diag::throw_exception("seg::Segmentation::shared_functional_groups_item missing");
	}
	return *shared_functional_groups_item_;
}

std::size_t Segmentation::rows() const noexcept {
	return rows_;
}

std::size_t Segmentation::columns() const noexcept {
	return columns_;
}

std::size_t Segmentation::segment_count() const noexcept {
	return index_.segments.size();
}

std::size_t Segmentation::frame_count() const noexcept {
	return index_.frames.size();
}

SegmentListView Segmentation::segments() const noexcept {
	return SegmentListView(this);
}

SegmentFrameListView Segmentation::frames() const noexcept {
	return SegmentFrameListView(this, nullptr);
}

std::optional<SegmentView> Segmentation::segment_by_number(
    std::uint16_t segment_number) const noexcept {
	const auto found = index_.segment_index_by_number.find(segment_number);
	if (found == index_.segment_index_by_number.end()) {
		return std::nullopt;
	}
	const auto& segment = index_.segments[found->second];
	return SegmentView(segment.item, segment.number);
}

SegmentFrameListView Segmentation::frames_for_segment(
    std::uint16_t segment_number) const noexcept {
	const auto found = index_.frame_indices_by_segment.find(segment_number);
	if (found == index_.frame_indices_by_segment.end()) {
		return SegmentFrameListView(this, &index_.empty_frame_indices);
	}
	return SegmentFrameListView(this, &found->second);
}

std::size_t Segmentation::segment_frame_count(
    std::uint16_t segment_number) const noexcept {
	const auto found = index_.frame_indices_by_segment.find(segment_number);
	return found == index_.frame_indices_by_segment.end() ? 0u : found->second.size();
}

void Segmentation::decode_frame_into(
    std::size_t frame_index, std::span<std::uint8_t> out) const {
	if (frame_index >= index_.frames.size()) {
		throw_decode("frame index out of range");
	}

	file_->ensure_loaded("PixelData"_tag);

	if (segmentation_type_ == SegmentationType::binary) {
		// DICOM BINARY SEG stores one bit per pixel across the multi-frame
		// native PixelData stream. Expose it as one byte per pixel for callers.
		const auto transfer_syntax = file_->transfer_syntax_uid();
		if (transfer_syntax &&
		    (!transfer_syntax.is_uncompressed() ||
		        transfer_syntax.is_encapsulated())) {
			throw_decode(
			    "compressed/encapsulated BINARY SEG PixelData is not supported");
		}
		const auto bits_allocated =
		    file_->get_dataelement("BitsAllocated"_tag).to_long().value_or(0);
		const auto bits_stored =
		    file_->get_dataelement("BitsStored"_tag).to_long().value_or(0);
		if (bits_allocated != 1 || bits_stored != 1) {
			throw_decode("BINARY SEG requires BitsAllocated=1 and BitsStored=1");
		}
		if (rows_ == 0 || columns_ == 0 ||
		    columns_ > std::numeric_limits<std::size_t>::max() / rows_) {
			throw_decode("invalid Rows or Columns");
		}
		const auto pixels_per_frame = rows_ * columns_;
		if (out.size() < pixels_per_frame) {
			throw_decode("destination buffer is smaller than decoded frame");
		}
		if (frame_index >
		    std::numeric_limits<std::size_t>::max() / pixels_per_frame) {
			throw_decode("frame bit offset overflows size_t");
		}
		const auto frame_bit_offset = frame_index * pixels_per_frame;
		const auto total_bits = index_.frames.size() * pixels_per_frame;
		const auto& pixel_data = file_->get_dataelement("PixelData"_tag);
		if (!pixel_data) {
			throw_decode("BINARY SEG PixelData is missing");
		}
		if (pixel_data.as_pixel_sequence()) {
			throw_decode(
			    "compressed/encapsulated BINARY SEG PixelData is not supported");
		}
		if (dicom::detail::is_detached_pixel_payload_marker(pixel_data)) {
			throw_decode("BINARY SEG PixelData payload is detached");
		}
		const auto bytes = pixel_data.value_span();
		if (bytes.size() < (total_bits + 7u) / 8u) {
			throw_decode("BINARY SEG PixelData size mismatch");
		}
		for (std::size_t pixel_index = 0; pixel_index < pixels_per_frame; ++pixel_index) {
			const auto bit_index = frame_bit_offset + pixel_index;
			out[pixel_index] = static_cast<std::uint8_t>(
			    (bytes[bit_index / 8u] >> (bit_index % 8u)) & 0x01u);
		}
		return;
	}

	if (segmentation_type_ == SegmentationType::fractional) {
		// FRACTIONAL SEG commonly stores 8-bit samples with
		// MaximumFractionalValue describing the scale. The MVP keeps this frame
		// by frame and delegates normal native decoding to DicomFile.
		const auto bits_allocated =
		    file_->get_dataelement("BitsAllocated"_tag).to_long().value_or(0);
		if (bits_allocated != 8) {
			throw_decode("FRACTIONAL SEG MVP requires BitsAllocated=8");
		}
		const auto plan = file_->create_decode_plan();
		file_->decode_into(frame_index, out, plan);
		return;
	}

	throw_decode("unsupported SegmentationType for frame decode");
}

SegmentListView::SegmentListView(const Segmentation* segmentation) noexcept
    : segmentation_(segmentation) {}

std::size_t SegmentListView::size() const noexcept {
	return segmentation_ ? segmentation_->index_.segments.size() : 0u;
}

bool SegmentListView::empty() const noexcept {
	return size() == 0;
}

SegmentView SegmentListView::operator[](std::size_t index) const {
	if (!segmentation_ || index >= segmentation_->index_.segments.size()) {
		diag::throw_exception("seg::SegmentListView::operator[] index out of range");
	}
	const auto& segment = segmentation_->index_.segments[index];
	return SegmentView(segment.item, segment.number);
}

SegmentListView::iterator SegmentListView::begin() const noexcept {
	return iterator(segmentation_, 0);
}

SegmentListView::iterator SegmentListView::end() const noexcept {
	return iterator(segmentation_, size());
}

SegmentListView::iterator::iterator(
    const Segmentation* segmentation, std::size_t index) noexcept
    : segmentation_(segmentation), index_(index) {}

SegmentView SegmentListView::iterator::operator*() const {
	return SegmentListView(segmentation_)[index_];
}

SegmentListView::iterator& SegmentListView::iterator::operator++() noexcept {
	++index_;
	return *this;
}

SegmentListView::iterator SegmentListView::iterator::operator++(int) noexcept {
	auto copy = *this;
	++(*this);
	return copy;
}

bool SegmentListView::iterator::operator==(const iterator& rhs) const noexcept {
	return segmentation_ == rhs.segmentation_ && index_ == rhs.index_;
}

SegmentFrameListView::SegmentFrameListView(const Segmentation* segmentation,
    const std::vector<std::size_t>* frame_indices) noexcept
    : segmentation_(segmentation), frame_indices_(frame_indices) {}

std::size_t SegmentFrameListView::size() const noexcept {
	if (frame_indices_) {
		return frame_indices_->size();
	}
	return segmentation_ ? segmentation_->index_.frames.size() : 0u;
}

bool SegmentFrameListView::empty() const noexcept {
	return size() == 0;
}

SegmentFrameView SegmentFrameListView::operator[](std::size_t ordinal) const {
	return SegmentFrameView(segmentation_, frame_index_from_ordinal(ordinal));
}

std::size_t SegmentFrameListView::frame_index_from_ordinal(
    std::size_t ordinal) const {
	if (frame_indices_) {
		if (ordinal >= frame_indices_->size()) {
			diag::throw_exception("seg::SegmentFrameListView::operator[] index out of range");
		}
		return (*frame_indices_)[ordinal];
	}
	if (!segmentation_) {
		diag::throw_exception("seg::SegmentFrameListView::operator[] invalid view");
	}
	if (ordinal >= segmentation_->index_.frames.size()) {
		diag::throw_exception("seg::SegmentFrameListView::operator[] index out of range");
	}
	return ordinal;
}

SegmentFrameListView::iterator SegmentFrameListView::begin() const noexcept {
	return iterator(segmentation_, frame_indices_, 0);
}

SegmentFrameListView::iterator SegmentFrameListView::end() const noexcept {
	return iterator(segmentation_, frame_indices_, size());
}

SegmentFrameListView::iterator::iterator(const Segmentation* segmentation,
    const std::vector<std::size_t>* frame_indices, std::size_t ordinal) noexcept
    : segmentation_(segmentation),
      frame_indices_(frame_indices),
      ordinal_(ordinal) {}

SegmentFrameView SegmentFrameListView::iterator::operator*() const {
	return SegmentFrameListView(segmentation_, frame_indices_)[ordinal_];
}

SegmentFrameListView::iterator& SegmentFrameListView::iterator::operator++() noexcept {
	++ordinal_;
	return *this;
}

SegmentFrameListView::iterator SegmentFrameListView::iterator::operator++(int) noexcept {
	auto copy = *this;
	++(*this);
	return copy;
}

bool SegmentFrameListView::iterator::operator==(const iterator& rhs) const noexcept {
	return segmentation_ == rhs.segmentation_ &&
	    frame_indices_ == rhs.frame_indices_ && ordinal_ == rhs.ordinal_;
}

SegmentView::SegmentView(const DataSet* item, std::uint16_t number) noexcept
    : item_(item), number_(number) {}

std::uint16_t SegmentView::number() const noexcept {
	return number_;
}

std::string_view SegmentView::label() const {
	return item_ ? string_value_or_empty(*item_, "SegmentLabel"_tag) : std::string_view{};
}

std::string_view SegmentView::description() const {
	return item_ ? string_value_or_empty(*item_, "SegmentDescription"_tag)
	             : std::string_view{};
}

std::string_view SegmentView::algorithm_name() const {
	return item_ ? string_value_or_empty(*item_, "SegmentAlgorithmName"_tag)
	             : std::string_view{};
}

SegmentAlgorithmType SegmentView::algorithm_type() const {
	return item_ ? parse_algorithm_type(
	                   string_value_or_empty(*item_, "SegmentAlgorithmType"_tag))
	             : SegmentAlgorithmType::unknown;
}

std::optional<CodeView> SegmentView::property_category() const {
	return item_ ? code_view_from_sequence(
	                   *item_, "SegmentedPropertyCategoryCodeSequence"_tag)
	             : std::nullopt;
}

std::optional<CodeView> SegmentView::property_type() const {
	return item_ ? code_view_from_sequence(*item_, "SegmentedPropertyTypeCodeSequence"_tag)
	             : std::nullopt;
}

std::optional<CodeView> SegmentView::anatomic_region() const {
	return item_ ? code_view_from_sequence(*item_, "AnatomicRegionSequence"_tag)
	             : std::nullopt;
}

std::optional<std::array<std::uint16_t, 3>>
SegmentView::recommended_display_cielab() const {
	if (!item_) {
		return std::nullopt;
	}
	const auto values =
	    item_->get_dataelement("RecommendedDisplayCIELabValue"_tag).to_long_vector();
	if (!values || values->size() < 3) {
		return std::nullopt;
	}
	std::array<std::uint16_t, 3> out{};
	for (std::size_t index = 0; index < out.size(); ++index) {
		const auto value = (*values)[index];
		if (value < 0 || value > std::numeric_limits<std::uint16_t>::max()) {
			return std::nullopt;
		}
		out[index] = static_cast<std::uint16_t>(value);
	}
	return out;
}

const DataSet& SegmentView::dataset() const noexcept {
	return *item_;
}

SegmentFrameView::SegmentFrameView(
    const Segmentation* segmentation, std::size_t frame_index) noexcept
    : segmentation_(segmentation), frame_index_(frame_index) {}

std::size_t SegmentFrameView::index() const noexcept {
	return frame_index_;
}

std::uint16_t SegmentFrameView::referenced_segment_number() const {
	if (!segmentation_ || frame_index_ >= segmentation_->index_.frames.size()) {
		diag::throw_exception("seg::SegmentFrameView::referenced_segment_number invalid frame");
	}
	return segmentation_->index_.frames[frame_index_].referenced_segment_number;
}

std::optional<std::array<double, 3>>
SegmentFrameView::image_position_patient() const {
	const auto& frame_item = per_frame_functional_groups_item();
	const auto* plane_position_item = functional_group_first_item(
	    frame_item, segmentation_->shared_functional_groups_item_,
	    "PlanePositionSequence"_tag);
	return plane_position_item
	           ? double_array_value<3>(*plane_position_item, "ImagePositionPatient"_tag)
	           : std::nullopt;
}

std::optional<std::array<double, 6>>
SegmentFrameView::image_orientation_patient() const {
	const auto& frame_item = per_frame_functional_groups_item();
	const auto* plane_orientation_item = functional_group_first_item(
	    frame_item, segmentation_->shared_functional_groups_item_,
	    "PlaneOrientationSequence"_tag);
	return plane_orientation_item
	           ? double_array_value<6>(*plane_orientation_item, "ImageOrientationPatient"_tag)
	           : std::nullopt;
}

std::optional<std::array<double, 2>> SegmentFrameView::pixel_spacing() const {
	const auto& frame_item = per_frame_functional_groups_item();
	const auto* pixel_measures_item = functional_group_first_item(
	    frame_item, segmentation_->shared_functional_groups_item_,
	    "PixelMeasuresSequence"_tag);
	return pixel_measures_item
	           ? double_array_value<2>(*pixel_measures_item, "PixelSpacing"_tag)
	           : std::nullopt;
}

std::optional<double> SegmentFrameView::slice_thickness() const {
	const auto& frame_item = per_frame_functional_groups_item();
	const auto* pixel_measures_item = functional_group_first_item(
	    frame_item, segmentation_->shared_functional_groups_item_,
	    "PixelMeasuresSequence"_tag);
	return pixel_measures_item
	           ? pixel_measures_item->get_dataelement("SliceThickness"_tag).to_double()
	           : std::nullopt;
}

SourceImageRefListView SegmentFrameView::source_images() const {
	return SourceImageRefListView(segmentation_, frame_index_);
}

const DataSet& SegmentFrameView::per_frame_functional_groups_item() const {
	if (!segmentation_ || frame_index_ >= segmentation_->index_.frames.size() ||
	    !segmentation_->index_.frames[frame_index_].functional_group_item) {
		diag::throw_exception("seg::SegmentFrameView::per_frame_functional_groups_item missing");
	}
	return *segmentation_->index_.frames[frame_index_].functional_group_item;
}

SourceImageRefListView::SourceImageRefListView(
    const Segmentation* segmentation, std::size_t frame_index) noexcept
    : segmentation_(segmentation), frame_index_(frame_index) {}

std::size_t SourceImageRefListView::size() const noexcept {
	if (!segmentation_ || frame_index_ >= segmentation_->index_.frames.size()) {
		return 0;
	}
	return segmentation_->index_.frames[frame_index_].source_images.size();
}

bool SourceImageRefListView::empty() const noexcept {
	return size() == 0;
}

SourceImageRefView SourceImageRefListView::operator[](std::size_t index) const {
	if (!segmentation_ || frame_index_ >= segmentation_->index_.frames.size()) {
		diag::throw_exception("seg::SourceImageRefListView::operator[] invalid frame");
	}
	const auto& refs = segmentation_->index_.frames[frame_index_].source_images;
	if (index >= refs.size()) {
		diag::throw_exception("seg::SourceImageRefListView::operator[] index out of range");
	}
	const auto& ref = refs[index];
	return SourceImageRefView(ref.item,
	    std::span<const std::uint32_t>(
	        ref.referenced_frame_numbers.data(), ref.referenced_frame_numbers.size()));
}

SourceImageRefView::SourceImageRefView(const DataSet* item,
    std::span<const std::uint32_t> referenced_frame_numbers) noexcept
    : item_(item), referenced_frame_numbers_(referenced_frame_numbers) {}

std::string_view SourceImageRefView::sop_class_uid() const {
	return item_ ? string_value_or_empty(*item_, "ReferencedSOPClassUID"_tag)
	             : std::string_view{};
}

std::string_view SourceImageRefView::sop_instance_uid() const {
	return item_ ? string_value_or_empty(*item_, "ReferencedSOPInstanceUID"_tag)
	             : std::string_view{};
}

std::span<const std::uint32_t>
SourceImageRefView::referenced_frame_numbers() const noexcept {
	return referenced_frame_numbers_;
}

const DataSet& SourceImageRefView::dataset() const noexcept {
	return *item_;
}

bool is_segmentation_storage(const DataSet& ds) noexcept {
	try {
		return dataset_has_segmentation_sop_class(ds);
	} catch (...) {
		return false;
	}
}

bool is_segmentation_storage(const DicomFile& file) noexcept {
	return is_segmentation_storage(file.dataset());
}

std::unique_ptr<Segmentation> from_dicomfile(
    std::unique_ptr<DicomFile> file, const Options& options) {
	// Higher-level modules adapt an already parsed DicomFile instead of owning a
	// separate read path. That keeps file/byte error policy in the core reader.
	if (!file) {
		throw_seg("input DicomFile is null");
	}
	if (file->has_error() && !options.allow_partial_source) {
		throw_seg("input DicomFile has a read error");
	}
	if (dataset_has_labelmap_segmentation_sop_class(file->dataset())) {
		throw_seg("LABELMAP SEG is outside the SEG MVP scope");
	}
	if (!is_segmentation_storage(*file)) {
		throw_seg("input DicomFile is not Segmentation Storage");
	}

	auto segmentation =
	    std::unique_ptr<Segmentation>(new Segmentation(std::move(file)));
	// Build only metadata indexes here. Pixel data is decoded lazily through
	// decode_frame_into(), preserving the cost model of DicomFile.
	segmentation->extract_instance_metadata(options);
	segmentation->index_segment_sequence_items(options);
	segmentation->index_per_frame_functional_group_items(options);
	return segmentation;
}

} // namespace dicom::seg
