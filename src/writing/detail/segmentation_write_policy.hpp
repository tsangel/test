#pragma once

#include "dicom.h"
#include "pixel/host/adapter/host_adapter.hpp"
#include "pixel/host/encode/encode_target_policy.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

namespace dicom::write_detail {
using namespace dicom::literals;

enum class SegPixelPayloadKind : std::uint8_t {
	none,
	binary,
	fractional,
	labelmap,
};

struct SegPixelPayloadWritePolicy {
	SegPixelPayloadKind kind{SegPixelPayloadKind::none};

	[[nodiscard]] bool is_segmentation() const noexcept {
		return kind != SegPixelPayloadKind::none;
	}
};

[[nodiscard]] inline std::string_view trim_dicom_text(
    std::string_view value) noexcept {
	while (!value.empty() && value.front() == ' ') {
		value.remove_prefix(1);
	}
	while (!value.empty() && value.back() == ' ') {
		value.remove_suffix(1);
	}
	return value;
}

[[nodiscard]] inline std::optional<std::string_view> text_value(
    const DataSet& dataset, Tag tag) {
	if (const auto value = dataset.get_value<std::string_view>(tag)) {
		return trim_dicom_text(*value);
	}
	return std::nullopt;
}

[[nodiscard]] inline long long_value_or(const DataSet& dataset, Tag tag,
    long fallback) {
	return dataset[tag].to_long().value_or(fallback);
}

[[nodiscard]] inline bool is_segmentation_sop_class_uid(
    std::string_view value) noexcept {
	value = trim_dicom_text(value);
	return value == "SegmentationStorage"_uid.value() ||
	    value == "LabelMapSegmentationStorage"_uid.value();
}

[[nodiscard]] inline bool is_labelmap_segmentation_sop_class_uid(
    std::string_view value) noexcept {
	return trim_dicom_text(value) == "LabelMapSegmentationStorage"_uid.value();
}

[[noreturn]] inline void throw_seg_write_policy_error(const DicomFile& file,
    std::string_view operation, std::string_view reason) {
	diag::throw_exception("{} file={} reason={}", operation, file.path(), reason);
}

inline void require_long_tag_value(const DicomFile& file,
    std::string_view operation, const DataSet& dataset, Tag tag, long expected,
    std::string_view reason) {
	if (long_value_or(dataset, tag, -1) != expected) {
		throw_seg_write_policy_error(file, operation, reason);
	}
}

inline void validate_seg_pixel_metadata_invariants_or_throw(
    const DicomFile& file, std::string_view operation,
    const DataSet& dataset, SegPixelPayloadKind kind) {
	require_long_tag_value(file, operation, dataset, "SamplesPerPixel"_tag, 1,
	    "SEG PixelData write requires SamplesPerPixel=1");
	require_long_tag_value(file, operation, dataset, "PixelRepresentation"_tag, 0,
	    "SEG PixelData write requires PixelRepresentation=0");

	if (kind == SegPixelPayloadKind::binary) {
		require_long_tag_value(file, operation, dataset, "BitsAllocated"_tag, 1,
		    "BINARY SEG PixelData write requires BitsAllocated=1");
		require_long_tag_value(file, operation, dataset, "BitsStored"_tag, 1,
		    "BINARY SEG PixelData write requires BitsStored=1");
		require_long_tag_value(file, operation, dataset, "HighBit"_tag, 0,
		    "BINARY SEG PixelData write requires HighBit=0");
		return;
	}

	if (kind == SegPixelPayloadKind::fractional) {
		require_long_tag_value(file, operation, dataset, "BitsAllocated"_tag, 8,
		    "FRACTIONAL SEG PixelData write requires BitsAllocated=8");
		require_long_tag_value(file, operation, dataset, "BitsStored"_tag, 8,
		    "FRACTIONAL SEG PixelData write requires BitsStored=8");
		require_long_tag_value(file, operation, dataset, "HighBit"_tag, 7,
		    "FRACTIONAL SEG PixelData write requires HighBit=7");
		if (long_value_or(dataset, "MaximumFractionalValue"_tag, 0) <= 0) {
			throw_seg_write_policy_error(file, operation,
			    "FRACTIONAL SEG PixelData write requires MaximumFractionalValue");
		}
		const auto fractional_type =
		    text_value(dataset, "SegmentationFractionalType"_tag).value_or("");
		if (fractional_type != "PROBABILITY" && fractional_type != "OCCUPANCY") {
			throw_seg_write_policy_error(file, operation,
			    "FRACTIONAL SEG PixelData write requires SegmentationFractionalType");
		}
		return;
	}

	if (kind == SegPixelPayloadKind::labelmap) {
		const auto bits_allocated =
		    long_value_or(dataset, "BitsAllocated"_tag, 0);
		if (bits_allocated != 8 && bits_allocated != 16) {
			throw_seg_write_policy_error(file, operation,
			    "LABELMAP SEG PixelData write requires BitsAllocated=8 or 16");
		}
		if (long_value_or(dataset, "BitsStored"_tag, -1) != bits_allocated) {
			throw_seg_write_policy_error(file, operation,
			    "LABELMAP SEG PixelData write requires BitsStored=BitsAllocated");
		}
		if (long_value_or(dataset, "HighBit"_tag, -1) != bits_allocated - 1) {
			throw_seg_write_policy_error(file, operation,
			    "LABELMAP SEG PixelData write requires HighBit=BitsAllocated-1");
		}
		const auto photometric =
		    text_value(dataset, "PhotometricInterpretation"_tag).value_or("");
		if (photometric != "MONOCHROME2" && photometric != "PALETTE COLOR") {
			throw_seg_write_policy_error(file, operation,
			    "LABELMAP SEG PixelData write requires PhotometricInterpretation MONOCHROME2 or PALETTE COLOR");
		}
		if (dataset.get_dataelement("PixelPaddingRangeLimit"_tag).is_present()) {
			throw_seg_write_policy_error(file, operation,
			    "LABELMAP SEG PixelData write does not support PixelPaddingRangeLimit");
		}
		if (const auto& pixel_padding =
		        dataset.get_dataelement("PixelPaddingValue"_tag)) {
			const auto padding_value = pixel_padding.to_long();
			if (!padding_value || *padding_value < 0 ||
			    *padding_value > std::numeric_limits<std::uint16_t>::max()) {
				throw_seg_write_policy_error(file, operation,
				    "LABELMAP SEG PixelData write requires PixelPaddingValue to be a single unsigned label value");
			}
		}
		if (const auto segments_overlap =
		        text_value(dataset, "SegmentsOverlap"_tag);
		    segments_overlap && *segments_overlap != "NO") {
			throw_seg_write_policy_error(file, operation,
			    "LABELMAP SEG PixelData write requires SegmentsOverlap=NO when present");
		}
	}
}

[[nodiscard]] inline SegPixelPayloadWritePolicy
classify_seg_pixel_payload_write_policy_or_throw(const DicomFile& file,
    std::string_view operation) {
	const DataSet& dataset = file.dataset();
	const auto sop_class_uid = text_value(dataset, "SOPClassUID"_tag);
	const auto media_sop_class_uid =
	    text_value(dataset, "MediaStorageSOPClassUID"_tag);

	if (sop_class_uid && media_sop_class_uid &&
	    *sop_class_uid != *media_sop_class_uid &&
	    (is_segmentation_sop_class_uid(*sop_class_uid) ||
	        is_segmentation_sop_class_uid(*media_sop_class_uid))) {
		throw_seg_write_policy_error(file, operation,
		    "SEG SOPClassUID and MediaStorageSOPClassUID disagree");
	}

	const auto declared_sop = sop_class_uid ? *sop_class_uid
	                                        : media_sop_class_uid.value_or("");
	if (!is_segmentation_sop_class_uid(declared_sop)) {
		return {};
	}

	const auto segmentation_type =
	    text_value(dataset, "SegmentationType"_tag).value_or("");
	const bool labelmap_sop =
	    is_labelmap_segmentation_sop_class_uid(declared_sop);

	SegPixelPayloadWritePolicy policy{};
	if (segmentation_type == "BINARY") {
		if (labelmap_sop) {
			throw_seg_write_policy_error(file, operation,
			    "Label Map Segmentation Storage requires SegmentationType=LABELMAP");
		}
		policy.kind = SegPixelPayloadKind::binary;
	} else if (segmentation_type == "FRACTIONAL") {
		if (labelmap_sop) {
			throw_seg_write_policy_error(file, operation,
			    "Label Map Segmentation Storage requires SegmentationType=LABELMAP");
		}
		policy.kind = SegPixelPayloadKind::fractional;
	} else if (segmentation_type == "LABELMAP") {
		if (!labelmap_sop) {
			throw_seg_write_policy_error(file, operation,
			    "LABELMAP SegmentationType requires Label Map Segmentation Storage");
		}
		policy.kind = SegPixelPayloadKind::labelmap;
	} else {
		throw_seg_write_policy_error(file, operation,
		    "SEG PixelData write requires SegmentationType BINARY, FRACTIONAL, or LABELMAP");
	}

	return policy;
}

inline void validate_seg_transfer_syntax_target_or_throw(const DicomFile& file,
    const SegPixelPayloadWritePolicy& policy, uid::WellKnown target_transfer_syntax,
    std::string_view operation) {
	if (!policy.is_segmentation()) {
		return;
	}
	if (target_transfer_syntax.is_lossy()) {
		throw_seg_write_policy_error(file, operation,
		    "SEG PixelData write/transcode requires a lossless target transfer syntax");
	}
}

inline void validate_seg_pixel_transcode_or_throw(const DicomFile& file,
    const SegPixelPayloadWritePolicy& policy, bool needs_pixel_transcode,
    std::string_view operation) {
	if (!policy.is_segmentation() || !needs_pixel_transcode) {
		return;
	}
	if (policy.kind == SegPixelPayloadKind::binary) {
		throw_seg_write_policy_error(file, operation,
		    "BINARY SEG PixelData transcode requires core 1-bit pixel layout/write support");
	}
}

inline void validate_seg_encode_profile_or_throw(const DicomFile& file,
    const SegPixelPayloadWritePolicy& policy, std::uint32_t codec_profile_code,
    std::string_view operation) {
	if (!policy.is_segmentation()) {
		return;
	}
	if (pixel::detail::encode_profile_uses_lossy_compression(codec_profile_code)) {
		throw_seg_write_policy_error(file, operation,
		    "SEG PixelData write/transcode requires a lossless encode profile");
	}
}

inline void validate_seg_encode_profile_from_transfer_syntax_or_throw(
    const DicomFile& file, const SegPixelPayloadWritePolicy& policy,
    uid::WellKnown target_transfer_syntax, std::string_view operation) {
	if (!policy.is_segmentation() || !target_transfer_syntax.is_encapsulated()) {
		return;
	}
	std::uint32_t codec_profile_code = PIXEL_CODEC_PROFILE_UNKNOWN;
	if (!::pixel::runtime::codec_profile_code_from_transfer_syntax(
	        target_transfer_syntax, &codec_profile_code)) {
		return;
	}
	validate_seg_encode_profile_or_throw(
	    file, policy, codec_profile_code, operation);
}

} // namespace dicom::write_detail
