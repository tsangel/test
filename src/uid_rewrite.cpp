#include <dicom.h>
#include <diagnostics.h>

#include <string>

using namespace dicom::literals;
namespace diag = dicom::diag;

namespace dicom {
namespace {

// Include both object-level and special-context SOP Instance UID tags that may
// need remapping during file rewrite flows. This intentionally covers File Meta
// (`MediaStorageSOPInstanceUID`), RTV metadata
// (`RTVCommunicationSOPInstanceUID`), DICOMDIR/File-set references
// (`ReferencedSOPInstanceUIDInFile`), and derived-instance links such as
// `MultiFrameSourceSOPInstanceUID`.
//
// `FailedSOPInstanceUIDList` is intentionally excluded for now. It is mainly
// used in DIMSE C-GET/C-MOVE failure or warning responses, and it is VM 1-n, so
// it needs multi-value UI rewrite support rather than the current single-value
// `to_uid_string()` / `from_uid_string()` path.
[[nodiscard]] bool is_rewrite_sop_instance_uid_tag(Tag tag) noexcept {
	return tag == "MediaStorageSOPInstanceUID"_tag ||
	    tag == "RTVCommunicationSOPInstanceUID"_tag ||
	    tag == "SOPInstanceUID"_tag ||
	    tag == "ReferencedSOPInstanceUID"_tag ||
	    tag == "ReferencedSOPInstanceUIDInFile"_tag ||
	    tag == "MultiFrameSourceSOPInstanceUID"_tag ||
	    tag == "SOPInstanceUIDOfConcatenationSource"_tag;
}

[[nodiscard]] bool is_rewrite_frame_of_reference_uid_tag(Tag tag) noexcept {
	return tag == "FrameOfReferenceUID"_tag ||
	    tag == "SynchronizationFrameOfReferenceUID"_tag ||
	    tag == "TargetFrameOfReferenceUID"_tag ||
	    tag == "VolumeFrameOfReferenceUID"_tag ||
	    tag == "TableFrameOfReferenceUID"_tag ||
	    tag == "SourceFrameOfReferenceUID"_tag ||
	    tag == "ReferencedFrameOfReferenceUID"_tag ||
	    tag == "RelatedFrameOfReferenceUID"_tag ||
	    tag == "EquipmentFrameOfReferenceUID"_tag;
}

[[nodiscard]] bool should_rewrite_uid_tag(
    Tag tag, const RewriteUidOptions& options) noexcept {
	if (tag == "StudyInstanceUID"_tag) {
		return options.rewrite_study_instance_uid;
	}
	if (tag == "SeriesInstanceUID"_tag) {
		return options.rewrite_series_instance_uid;
	}
	if (is_rewrite_sop_instance_uid_tag(tag)) {
		return options.rewrite_sop_instance_uids;
	}
	if (is_rewrite_frame_of_reference_uid_tag(tag)) {
		return options.rewrite_frame_of_reference_uids;
	}
	return false;
}

[[nodiscard]] bool rewrite_uid_element(
    DataElement& element,
    UidRemapper& remapper,
    const RewriteUidOptions& options) {
	if (element.vr() != VR::UI || !should_rewrite_uid_tag(element.tag(), options)) {
		return false;
	}
	auto uid = element.to_uid_string();
	if (!uid || uid->empty()) {
		return false;
	}
	const auto mapped = remapper.map_uid(*uid);
	if (mapped == *uid) {
		return false;
	}
	if (!element.from_uid_string(mapped)) {
		const auto* parent = element.parent();
		const std::string file_path =
		    (parent != nullptr && parent->root_dataset() != nullptr)
		        ? parent->root_dataset()->path()
		        : std::string{};
		diag::error_and_throw(
		    "rewrite_uids file={} tag={} reason=failed to assign mapped UID value={}",
		    file_path, element.tag().to_string(), mapped);
	}
	return true;
}

} // namespace

std::size_t rewrite_uids(
    DataSet& dataset,
    UidRemapper& remapper,
    const RewriteUidOptions& options) {
	std::size_t rewritten = 0;
	dataset.visit([&](auto, auto& element) -> DataSetVisitControl {
		if (rewrite_uid_element(element, remapper, options)) {
			++rewritten;
		}
		return DataSetVisitControl::continue_;
	});
	return rewritten;
}

std::size_t rewrite_uids(
    DicomFile& file,
    UidRemapper& remapper,
    const RewriteUidOptions& options) {
	return rewrite_uids(file.dataset(), remapper, options);
}

} // namespace dicom
