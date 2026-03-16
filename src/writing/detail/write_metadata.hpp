#pragma once

#include "writing/detail/dataset_write.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace dicom::write_detail {

[[nodiscard]] inline std::string normalize_valid_uid_text_or_empty(std::string_view value) {
	auto normalized = uid::normalize_uid_text(value);
	if (!uid::is_valid_uid_text_strict(normalized)) {
		return {};
	}
	return normalized;
}

[[nodiscard]] inline std::string infer_transfer_syntax_uid(
    const DicomFile& file, const DataSet& dataset) {
	if (const auto file_ts = file.transfer_syntax_uid(); file_ts.valid()) {
		if (auto normalized = normalize_valid_uid_text_or_empty(file_ts.value());
		    !normalized.empty()) {
			return normalized;
		}
	}

	if (auto from_meta = dataset.get_value<std::string>("TransferSyntaxUID"_tag);
	    from_meta && !from_meta->empty()) {
		if (auto normalized = normalize_valid_uid_text_or_empty(*from_meta);
		    !normalized.empty()) {
			return normalized;
		}
	}

	const auto fallback = dataset.is_explicit_vr() ? "ExplicitVRLittleEndian"_uid.value()
	                                               : "ImplicitVRLittleEndian"_uid.value();
	return std::string(fallback.data(), fallback.size());
}

// Chooses the effective transfer syntax for plain write_to_stream/write_bytes paths.
[[nodiscard]] inline uid::WellKnown determine_target_transfer_syntax(
    const DicomFile& file, const DataSet& dataset, const WriteOptions& options) {
	if (options.write_file_meta) {
		if (auto from_meta = dataset["TransferSyntaxUID"_tag].to_transfer_syntax_uid()) {
			return *from_meta;
		}
	}

	if (const auto from_file = file.transfer_syntax_uid(); from_file.valid()) {
		return from_file;
	}

	return dataset.is_explicit_vr() ? "ExplicitVRLittleEndian"_uid
	                                : "ImplicitVRLittleEndian"_uid;
}

// Maps the target transfer syntax to body write policy such as explicit VR or deflate.
[[nodiscard]] inline DatasetWritePlan determine_dataset_write_plan(
    uid::WellKnown transfer_syntax, const DataSet& dataset) {
	DatasetWritePlan plan{};
	if (!transfer_syntax.valid()) {
		plan.explicit_vr = dataset.is_explicit_vr();
		return plan;
	}

	plan.explicit_vr = transfer_syntax.uses_explicit_vr();
	if (transfer_syntax == "ExplicitVRBigEndian"_uid) {
		plan.explicit_vr = true;
		plan.convert_body_to_big_endian = true;
	} else if (transfer_syntax == "DeflatedExplicitVRLittleEndian"_uid) {
		plan.explicit_vr = true;
		plan.deflate_body = true;
	}
	return plan;
}

[[nodiscard]] inline std::optional<pixel::Photometric>
parse_photometric_from_text_for_write(std::string_view text) noexcept {
	if (ascii_iequals_keyword(text, "MONOCHROME1")) {
		return pixel::Photometric::monochrome1;
	}
	if (ascii_iequals_keyword(text, "MONOCHROME2")) {
		return pixel::Photometric::monochrome2;
	}
	if (ascii_iequals_keyword(text, "RGB")) {
		return pixel::Photometric::rgb;
	}
	if (ascii_iequals_keyword(text, "YBR_FULL")) {
		return pixel::Photometric::ybr_full;
	}
	if (ascii_iequals_keyword(text, "YBR_FULL_422")) {
		return pixel::Photometric::ybr_full_422;
	}
	if (ascii_iequals_keyword(text, "YBR_RCT")) {
		return pixel::Photometric::ybr_rct;
	}
	if (ascii_iequals_keyword(text, "YBR_ICT")) {
		return pixel::Photometric::ybr_ict;
	}
	return std::nullopt;
}

template <typename Fn>
void for_each_file_meta_element(const DataSet& dataset, Fn&& fn) {
	// File meta must stay in group 0002 and must not contain SQ/PX elements.
	for (const auto& element : dataset) {
		const auto group = element.tag().group();
		if (group < 0x0002u) {
			continue;
		}
		if (group > 0x0002u) {
			break;
		}
		if (element.vr().is_sequence() || element.vr().is_pixel_sequence()) {
			diag::error_and_throw(
			    "write_to_stream reason=file meta element is SQ/PX tag={} vr={}",
			    element.tag().to_string(), element.vr().str());
		}
		fn(element);
	}
}

[[nodiscard]] inline std::uint32_t measure_meta_group_length(const DataSet& dataset) {
	CountingWriter measuring_writer;
	for_each_file_meta_element(dataset, [&](const DataElement& element) {
		if (element.tag() == "FileMetaInformationGroupLength"_tag) {
			return;
		}
		write_non_sequence_element(
		    measuring_writer, element.tag(), element.vr(), element.value_span(), true);
	});
	return checked_u32(measuring_writer.written, "file meta group length");
}

inline void clear_existing_meta_group(DataSet& dataset) {
	std::vector<Tag> tags;
	for (const auto& element : dataset) {
		if (element.tag().group() == 0x0002u) {
			tags.push_back(element.tag());
		}
	}
	for (Tag tag : tags) {
		dataset.remove_dataelement(tag);
	}
}

// Rebuild helpers prefer a normalized TS UID but fall back to Explicit VR Little Endian.
[[nodiscard]] inline std::string determine_transfer_syntax_uid_for_rebuild(
    const DicomFile& file, const DataSet& dataset) {
	if (auto from_meta = dataset.get_value<std::string>("TransferSyntaxUID"_tag);
	    from_meta && !from_meta->empty()) {
		if (auto normalized = normalize_valid_uid_text_or_empty(*from_meta);
		    !normalized.empty()) {
			return normalized;
		}
	}
	return infer_transfer_syntax_uid(file, dataset);
}

template <typename Writer>
void write_file_meta_group(Writer& writer, const DataSet& dataset) {
	// Group length is recomputed from the current meta elements instead of trusting stored bytes.
	const auto meta_group_length = measure_meta_group_length(dataset);
	std::array<std::uint8_t, 4> meta_group_length_bytes{};
	endian::store_le<std::uint32_t>(meta_group_length_bytes.data(), meta_group_length);
	write_non_sequence_element(
	    writer, "FileMetaInformationGroupLength"_tag, VR::UL, meta_group_length_bytes, true);

	for_each_file_meta_element(dataset, [&](const DataElement& element) {
		if (element.tag() == "FileMetaInformationGroupLength"_tag) {
			return;
		}
		write_non_sequence_element(
		    writer, element.tag(), element.vr(), element.value_span(), true);
	});
}

template <typename Writer>
void write_preamble(Writer& writer) {
	static const std::array<std::uint8_t, 128> kPreamble{};
	static const std::array<std::uint8_t, 4> kMagic{
	    static_cast<std::uint8_t>('D'),
	    static_cast<std::uint8_t>('I'),
	    static_cast<std::uint8_t>('C'),
	    static_cast<std::uint8_t>('M')};
	writer.append(kPreamble.data(), kPreamble.size());
	writer.append(kMagic.data(), kMagic.size());
}


}  // namespace dicom::write_detail
