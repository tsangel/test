#pragma once

#include "serialization/detail/write_core.hpp"
#include "pixel/host/encode/encode_target_policy.hpp"

#include <cmath>
#include <string>
#include <string_view>
#include <vector>

namespace dicom::write_detail {

enum class OverlayDisposition : std::uint8_t {
	present = 0,
	deleted,
};

struct OverlayEntry {
	Tag tag{};
	OverlayDisposition disposition{OverlayDisposition::present};
	VR vr{VR::None};
	std::vector<std::uint8_t> value_bytes{};
};

class TransientWriteOverlay {
public:
	void upsert(Tag tag, VR vr, std::span<const std::uint8_t> value_bytes) {
		OverlayEntry entry{};
		entry.tag = tag;
		entry.disposition = OverlayDisposition::present;
		entry.vr = vr;
		entry.value_bytes.assign(value_bytes.begin(), value_bytes.end());
		entries_.push_back(std::move(entry));
	}

	void erase(Tag tag) {
		OverlayEntry entry{};
		entry.tag = tag;
		entry.disposition = OverlayDisposition::deleted;
		entries_.push_back(std::move(entry));
	}

	void replace_file_meta_group(bool replace = true) noexcept {
		replace_file_meta_group_ = replace;
	}

	[[nodiscard]] bool replaces_file_meta_group() const noexcept {
		return replace_file_meta_group_;
	}

	void finalize() {
		std::stable_sort(entries_.begin(), entries_.end(),
		    [](const OverlayEntry& lhs, const OverlayEntry& rhs) {
			    return lhs.tag < rhs.tag;
		    });
		std::vector<OverlayEntry> deduped{};
		deduped.reserve(entries_.size());
		for (std::size_t index = 0; index < entries_.size();) {
			std::size_t next = index + 1;
			while (next < entries_.size() && entries_[next].tag == entries_[index].tag) {
				++next;
			}
			deduped.push_back(std::move(entries_[next - 1]));
			index = next;
		}
		entries_ = std::move(deduped);
	}

	[[nodiscard]] const std::vector<OverlayEntry>& entries() const noexcept {
		return entries_;
	}

private:
	std::vector<OverlayEntry> entries_{};
	bool replace_file_meta_group_{false};
};

struct MergedElementRef {
	const DataElement* source{nullptr};
	const OverlayEntry* overlay{nullptr};

	[[nodiscard]] Tag tag() const noexcept {
		return source != nullptr ? source->tag() : overlay->tag;
	}

	[[nodiscard]] VR vr() const noexcept {
		return source != nullptr ? source->vr() : overlay->vr;
	}

	[[nodiscard]] std::span<const std::uint8_t> value_span() const noexcept {
		if (source != nullptr) {
			return source->value_span();
		}
		return std::span<const std::uint8_t>(
		    overlay->value_bytes.data(), overlay->value_bytes.size());
	}

	[[nodiscard]] bool from_overlay() const noexcept { return overlay != nullptr; }
};

struct LossyRatioBackpatchState {
	std::size_t token_offset_in_value{0};
	std::size_t token_width{0};
	std::size_t absolute_token_offset{0};
};

[[nodiscard]] inline std::string_view to_photometric_text_for_streaming_write(
    pixel::Photometric photometric) noexcept {
	switch (photometric) {
	case pixel::Photometric::monochrome1:
		return "MONOCHROME1";
	case pixel::Photometric::monochrome2:
		return "MONOCHROME2";
	case pixel::Photometric::rgb:
		return "RGB";
	case pixel::Photometric::ybr_full:
		return "YBR_FULL";
	case pixel::Photometric::ybr_full_422:
		return "YBR_FULL_422";
	case pixel::Photometric::ybr_rct:
		return "YBR_RCT";
	case pixel::Photometric::ybr_ict:
		return "YBR_ICT";
	default:
		return "MONOCHROME2";
	}
}

template <typename Builder>
void overlay_build_and_upsert_or_throw(TransientWriteOverlay& overlay,
    std::string_view error_context, std::string_view file_path, Tag tag, VR vr,
    Builder&& builder) {
	DataElement element(tag, vr, 0, 0, nullptr);
	if (!builder(element)) {
		diag::error_and_throw(
		    "{} file={} reason=failed to build overlay element tag={} vr={}",
		    error_context, file_path, tag.to_string(), vr.str());
	}
	overlay.upsert(tag, vr, element.value_span());
}

inline void overlay_upsert_long_or_throw(TransientWriteOverlay& overlay,
    std::string_view error_context, std::string_view file_path, Tag tag, VR vr,
    long value) {
	overlay_build_and_upsert_or_throw(overlay, error_context, file_path, tag, vr,
	    [&](DataElement& element) { return element.from_long(value); });
}

inline void overlay_upsert_string_view_or_throw(TransientWriteOverlay& overlay,
    std::string_view error_context, std::string_view file_path, Tag tag, VR vr,
    std::string_view value) {
	overlay_build_and_upsert_or_throw(overlay, error_context, file_path, tag, vr,
	    [&](DataElement& element) { return element.from_string_view(value); });
}

inline void overlay_upsert_string_views_or_throw(TransientWriteOverlay& overlay,
    std::string_view error_context, std::string_view file_path, Tag tag, VR vr,
    std::span<const std::string_view> values) {
	overlay_build_and_upsert_or_throw(overlay, error_context, file_path, tag, vr,
	    [&](DataElement& element) { return element.from_string_views(values); });
}

inline void overlay_upsert_double_vector_or_throw(TransientWriteOverlay& overlay,
    std::string_view error_context, std::string_view file_path, Tag tag, VR vr,
    std::span<const double> values) {
	overlay_build_and_upsert_or_throw(overlay, error_context, file_path, tag, vr,
	    [&](DataElement& element) { return element.from_double_vector(values); });
}

inline void overlay_upsert_transfer_syntax_uid_or_throw(TransientWriteOverlay& overlay,
    std::string_view error_context, std::string_view file_path,
    uid::WellKnown transfer_syntax) {
	overlay_build_and_upsert_or_throw(overlay, error_context, file_path,
	    kTransferSyntaxUidTag, VR::UI, [&](DataElement& element) {
		    return element.from_transfer_syntax_uid(transfer_syntax);
	    });
}

inline void overlay_upsert_uid_or_throw(TransientWriteOverlay& overlay,
    std::string_view error_context, std::string_view file_path, Tag tag,
    std::string_view uid_value) {
	overlay_build_and_upsert_or_throw(overlay, error_context, file_path, tag, VR::UI,
	    [&](DataElement& element) { return element.from_uid_string(uid_value); });
}

inline void overlay_upsert_value_bytes(TransientWriteOverlay& overlay, Tag tag, VR vr,
    std::span<const std::uint8_t> value_bytes) {
	overlay.upsert(tag, vr, value_bytes);
}

[[nodiscard]] inline bool is_file_meta_tag(Tag tag) noexcept {
	return tag.group() == 0x0002u;
}

[[nodiscard]] inline bool is_body_overlay_tag(Tag tag) noexcept {
	return tag.group() != 0x0002u && tag != kExtendedOffsetTableTag &&
	    tag != kExtendedOffsetTableLengthsTag;
}

template <typename Fn>
void for_each_merged_file_meta_element(const DataSet& dataset,
    const TransientWriteOverlay& overlay, Fn&& fn) {
	const auto& entries = overlay.entries();
	auto overlay_it = entries.begin();
	const auto overlay_end = entries.end();

	const auto emit_overlay = [&](const OverlayEntry& entry) {
		if (entry.disposition == OverlayDisposition::present) {
			fn(MergedElementRef{nullptr, &entry});
		}
	};

	if (overlay.replaces_file_meta_group()) {
		for (; overlay_it != overlay_end && is_file_meta_tag(overlay_it->tag); ++overlay_it) {
			emit_overlay(*overlay_it);
		}
		return;
	}

	for (const auto& element : dataset) {
		const auto group = element.tag().group();
		if (group < 0x0002u) {
			continue;
		}
		if (group > 0x0002u) {
			break;
		}
		while (overlay_it != overlay_end && is_file_meta_tag(overlay_it->tag) &&
		       overlay_it->tag < element.tag()) {
			emit_overlay(*overlay_it);
			++overlay_it;
		}
		if (overlay_it != overlay_end && is_file_meta_tag(overlay_it->tag) &&
		    overlay_it->tag == element.tag()) {
			emit_overlay(*overlay_it);
			++overlay_it;
			continue;
		}
		fn(MergedElementRef{&element, nullptr});
	}

	while (overlay_it != overlay_end && is_file_meta_tag(overlay_it->tag)) {
		emit_overlay(*overlay_it);
		++overlay_it;
	}
}

template <typename Fn>
void for_each_merged_body_element(const DataSet& dataset,
    const TransientWriteOverlay& overlay, Fn&& fn) {
	const auto& entries = overlay.entries();
	auto overlay_it = entries.begin();
	const auto overlay_end = entries.end();
	while (overlay_it != overlay_end && is_file_meta_tag(overlay_it->tag)) {
		++overlay_it;
	}

	const auto emit_overlay = [&](const OverlayEntry& entry) {
		if (entry.disposition == OverlayDisposition::present && is_body_overlay_tag(entry.tag)) {
			fn(MergedElementRef{nullptr, &entry});
		}
	};

	for (const auto& element : dataset) {
		if (element.tag().group() == 0x0002u) {
			continue;
		}
		while (overlay_it != overlay_end && overlay_it->tag < element.tag()) {
			emit_overlay(*overlay_it);
			++overlay_it;
		}
		if (overlay_it != overlay_end && overlay_it->tag == element.tag()) {
			emit_overlay(*overlay_it);
			++overlay_it;
			continue;
		}
		if (!is_body_overlay_tag(element.tag())) {
			continue;
		}
		fn(MergedElementRef{&element, nullptr});
	}

	while (overlay_it != overlay_end) {
		emit_overlay(*overlay_it);
		++overlay_it;
	}
}

[[nodiscard]] inline bool has_prior_lossy_history_for_streaming_write(
    const DataSet& dataset) {
	const auto lossy_value = dataset["LossyImageCompression"_tag].to_string_view();
	return lossy_value && *lossy_value == "01";
}

[[nodiscard]] inline std::vector<std::string> read_lossy_method_values_for_streaming_write(
    const DataSet& dataset) {
	std::vector<std::string> methods;
	const auto parsed = dataset["LossyImageCompressionMethod"_tag].to_string_views();
	if (!parsed) {
		return methods;
	}
	methods.reserve(parsed->size());
	for (const auto value : *parsed) {
		methods.emplace_back(value);
	}
	return methods;
}

[[nodiscard]] inline std::vector<double> read_lossy_ratio_values_for_streaming_write(
    const DataSet& dataset) {
	const auto parsed = dataset["LossyImageCompressionRatio"_tag].to_double_vector();
	return parsed.value_or(std::vector<double>{});
}

[[nodiscard]] inline std::string format_lossy_ratio_ds_value_or_throw(
    std::string_view file_path, uid::WellKnown transfer_syntax, double value) {
	if (!std::isfinite(value) || value <= 0.0) {
		diag::error_and_throw(
		    "write_with_transfer_syntax file={} ts={} reason=invalid lossy compression ratio {}",
		    file_path, transfer_syntax.value(), value);
	}

	for (int precision = 17; precision >= 1; --precision) {
		auto text = fmt::format("{:.{}g}", value, precision);
		if (text.size() <= 16u) {
			return text;
		}
	}

	diag::error_and_throw(
	    "write_with_transfer_syntax file={} ts={} reason=lossy compression ratio {} does not fit DS VM item width",
	    file_path, transfer_syntax.value(), value);
}

inline void update_pixel_metadata_for_streaming_write_overlay(
    TransientWriteOverlay& overlay, std::string_view file_path, uid::WellKnown,
    const pixel::PixelSource& source, bool target_is_rle,
    pixel::Photometric output_photometric, int bits_allocated, int bits_stored,
    int high_bit, int pixel_representation) {
	overlay_upsert_long_or_throw(overlay, "write_with_transfer_syntax", file_path,
	    "Rows"_tag, VR::US, static_cast<long>(source.rows));
	overlay_upsert_long_or_throw(overlay, "write_with_transfer_syntax", file_path,
	    "Columns"_tag, VR::US, static_cast<long>(source.cols));
	overlay_upsert_long_or_throw(overlay, "write_with_transfer_syntax", file_path,
	    "SamplesPerPixel"_tag, VR::US, static_cast<long>(source.samples_per_pixel));
	overlay_upsert_long_or_throw(overlay, "write_with_transfer_syntax", file_path,
	    "BitsAllocated"_tag, VR::US, static_cast<long>(bits_allocated));
	overlay_upsert_long_or_throw(overlay, "write_with_transfer_syntax", file_path,
	    "BitsStored"_tag, VR::US, static_cast<long>(bits_stored));
	overlay_upsert_long_or_throw(overlay, "write_with_transfer_syntax", file_path,
	    "HighBit"_tag, VR::US, static_cast<long>(high_bit));
	overlay_upsert_long_or_throw(overlay, "write_with_transfer_syntax", file_path,
	    "PixelRepresentation"_tag, VR::US, static_cast<long>(pixel_representation));
	overlay_upsert_string_view_or_throw(overlay, "write_with_transfer_syntax",
	    file_path, "PhotometricInterpretation"_tag, VR::CS,
	    to_photometric_text_for_streaming_write(output_photometric));

	if (source.frames > 1) {
		overlay_upsert_long_or_throw(overlay, "write_with_transfer_syntax", file_path,
		    "NumberOfFrames"_tag, VR::IS, static_cast<long>(source.frames));
	} else {
		overlay.erase("NumberOfFrames"_tag);
	}

	if (source.samples_per_pixel > 1) {
		const long planar_configuration = target_is_rle
		                                      ? 1L
		                                      : (source.planar == pixel::Planar::planar ? 1L : 0L);
		overlay_upsert_long_or_throw(overlay, "write_with_transfer_syntax", file_path,
		    "PlanarConfiguration"_tag, VR::US, planar_configuration);
	} else {
		overlay.erase("PlanarConfiguration"_tag);
	}
}

inline void update_lossy_metadata_for_streaming_write_overlay(
    const DataSet& source_dataset, TransientWriteOverlay& overlay,
    std::string_view file_path, uid::WellKnown transfer_syntax,
    uint32_t codec_profile_code, std::size_t uncompressed_payload_bytes,
    std::size_t encoded_payload_bytes) {
	const bool current_encode_is_lossy =
	    pixel::detail::encode_profile_uses_lossy_compression(codec_profile_code);
	const bool had_prior_lossy =
	    has_prior_lossy_history_for_streaming_write(source_dataset);

	if (!current_encode_is_lossy) {
		overlay_upsert_string_view_or_throw(overlay, "write_with_transfer_syntax",
		    file_path, "LossyImageCompression"_tag, VR::CS,
		    had_prior_lossy ? "01" : "00");
		if (!had_prior_lossy) {
			overlay.erase("LossyImageCompressionRatio"_tag);
			overlay.erase("LossyImageCompressionMethod"_tag);
		}
		return;
	}

	if (encoded_payload_bytes == 0 || uncompressed_payload_bytes == 0) {
		diag::error_and_throw(
		    "write_with_transfer_syntax file={} ts={} reason=cannot compute lossy compression ratio from zero payload size (uncompressed={} encoded={})",
		    file_path, transfer_syntax.value(), uncompressed_payload_bytes,
		    encoded_payload_bytes);
	}

	const auto method =
	    pixel::detail::lossy_method_for_encode_profile(codec_profile_code);
	if (!method) {
		diag::error_and_throw(
		    "write_with_transfer_syntax file={} ts={} reason=missing lossy compression method mapping for transfer syntax",
		    file_path, transfer_syntax.value());
	}

	const double ratio = static_cast<double>(uncompressed_payload_bytes) /
	    static_cast<double>(encoded_payload_bytes);
	if (!std::isfinite(ratio) || ratio <= 0.0) {
		diag::error_and_throw(
		    "write_with_transfer_syntax file={} ts={} reason=invalid lossy compression ratio computed from uncompressed={} encoded={}",
		    file_path, transfer_syntax.value(), uncompressed_payload_bytes,
		    encoded_payload_bytes);
	}

	std::vector<std::string> methods;
	std::vector<double> ratios;
	if (had_prior_lossy) {
		methods = read_lossy_method_values_for_streaming_write(source_dataset);
		ratios = read_lossy_ratio_values_for_streaming_write(source_dataset);
		const auto paired_count = std::min(methods.size(), ratios.size());
		methods.resize(paired_count);
		ratios.resize(paired_count);
	}
	methods.emplace_back(*method);
	ratios.push_back(ratio);

	std::vector<std::string_view> method_views;
	method_views.reserve(methods.size());
	for (const auto& value : methods) {
		method_views.emplace_back(value);
	}

	overlay_upsert_string_view_or_throw(overlay, "write_with_transfer_syntax",
	    file_path, "LossyImageCompression"_tag, VR::CS, "01");
	overlay_upsert_double_vector_or_throw(overlay, "write_with_transfer_syntax",
	    file_path, "LossyImageCompressionRatio"_tag, VR::DS,
	    std::span<const double>(ratios.data(), ratios.size()));
	overlay_upsert_string_views_or_throw(overlay, "write_with_transfer_syntax",
	    file_path, "LossyImageCompressionMethod"_tag, VR::CS,
	    std::span<const std::string_view>(method_views.data(), method_views.size()));
}

[[nodiscard]] inline LossyRatioBackpatchState
prepare_lossy_metadata_placeholder_for_streaming_write_overlay(
    const DataSet& source_dataset, TransientWriteOverlay& overlay,
    std::string_view file_path, uid::WellKnown transfer_syntax,
    uint32_t codec_profile_code) {
	const auto method =
	    pixel::detail::lossy_method_for_encode_profile(codec_profile_code);
	if (!method) {
		diag::error_and_throw(
		    "write_with_transfer_syntax file={} ts={} reason=missing lossy compression method mapping for transfer syntax",
		    file_path, transfer_syntax.value());
	}

	const bool had_prior_lossy =
	    has_prior_lossy_history_for_streaming_write(source_dataset);
	std::vector<std::string> methods;
	std::vector<double> ratios;
	if (had_prior_lossy) {
		methods = read_lossy_method_values_for_streaming_write(source_dataset);
		ratios = read_lossy_ratio_values_for_streaming_write(source_dataset);
		const auto paired_count = std::min(methods.size(), ratios.size());
		methods.resize(paired_count);
		ratios.resize(paired_count);
	}
	methods.emplace_back(*method);

	std::vector<std::string_view> method_views;
	method_views.reserve(methods.size());
	for (const auto& value : methods) {
		method_views.emplace_back(value);
	}

	std::string ratio_text;
	ratio_text.reserve((ratios.size() * 18u) + 16u);
	for (std::size_t index = 0; index < ratios.size(); ++index) {
		if (index != 0) {
			ratio_text.push_back('\\');
		}
		ratio_text += format_lossy_ratio_ds_value_or_throw(
		    file_path, transfer_syntax, ratios[index]);
	}
	if (!ratio_text.empty()) {
		ratio_text.push_back('\\');
	}

	const auto token_offset_in_value = ratio_text.size();
	static constexpr std::string_view kRatioPlaceholder = "0               ";
	ratio_text.append(kRatioPlaceholder);

	overlay_upsert_string_view_or_throw(overlay, "write_with_transfer_syntax",
	    file_path, "LossyImageCompression"_tag, VR::CS, "01");
	overlay_upsert_string_views_or_throw(overlay, "write_with_transfer_syntax",
	    file_path, "LossyImageCompressionMethod"_tag, VR::CS,
	    std::span<const std::string_view>(method_views.data(), method_views.size()));
	overlay_upsert_value_bytes(overlay, "LossyImageCompressionRatio"_tag, VR::DS,
	    std::span<const std::uint8_t>(
	        reinterpret_cast<const std::uint8_t*>(ratio_text.data()), ratio_text.size()));

	return LossyRatioBackpatchState{
	    token_offset_in_value,
	    kRatioPlaceholder.size(),
	    0u,
	};
}

inline void build_rebuilt_file_meta_overlay_or_throw(const DicomFile& file,
    const DataSet& source_dataset, uid::WellKnown target_transfer_syntax,
    TransientWriteOverlay& overlay) {
	overlay.replace_file_meta_group(true);

	std::string sop_class_uid;
	if (auto value = source_dataset[kSopClassUidTag].to_uid_string();
	    value && !value->empty()) {
		sop_class_uid = uid::normalize_uid_text(*value);
	} else if (auto value = source_dataset[kMediaStorageSopClassUidTag].to_uid_string();
	           value && !value->empty()) {
		sop_class_uid = uid::normalize_uid_text(*value);
	} else {
		sop_class_uid = std::string("SecondaryCaptureImageStorage"_uid.value());
	}
	if (!uid::is_valid_uid_text_strict(sop_class_uid)) {
		sop_class_uid = std::string("SecondaryCaptureImageStorage"_uid.value());
	}

	std::string sop_instance_uid;
	if (auto value = source_dataset[kSopInstanceUidTag].to_uid_string();
	    value && !value->empty()) {
		sop_instance_uid = uid::normalize_uid_text(*value);
	} else if (auto value = source_dataset[kMediaStorageSopInstanceUidTag].to_uid_string();
	           value && !value->empty()) {
		sop_instance_uid = uid::normalize_uid_text(*value);
	} else {
		const auto generated = uid::generate_sop_instance_uid();
		const auto generated_value = generated.value();
		sop_instance_uid.assign(generated_value.data(), generated_value.size());
	}
	if (!uid::is_valid_uid_text_strict(sop_instance_uid)) {
		const auto generated = uid::generate_sop_instance_uid();
		const auto generated_value = generated.value();
		sop_instance_uid.assign(generated_value.data(), generated_value.size());
	}

	const std::array<std::uint8_t, 2> meta_version{{0x00u, 0x01u}};
	overlay_upsert_value_bytes(
	    overlay, kFileMetaInformationVersionTag, VR::OB, meta_version);
	overlay_upsert_uid_or_throw(
	    overlay, "write_with_transfer_syntax", file.path(),
	    kMediaStorageSopClassUidTag, sop_class_uid);
	overlay_upsert_uid_or_throw(
	    overlay, "write_with_transfer_syntax", file.path(),
	    kMediaStorageSopInstanceUidTag, sop_instance_uid);
	overlay_upsert_transfer_syntax_uid_or_throw(
	    overlay, "write_with_transfer_syntax", file.path(), target_transfer_syntax);
	overlay_upsert_uid_or_throw(
	    overlay, "write_with_transfer_syntax", file.path(),
	    kImplementationClassUidTag, uid::implementation_class_uid());
	overlay_upsert_string_view_or_throw(
	    overlay, "write_with_transfer_syntax", file.path(),
	    kImplementationVersionNameTag, VR::SH, uid::implementation_version_name());
}

[[nodiscard]] inline std::size_t measure_dataset_value_offset_or_throw_with_overlay(
    const DataSet& dataset, const TransientWriteOverlay& overlay, Tag target_tag,
    bool explicit_vr) {
	CountingWriter measuring_writer;
	bool found = false;
	for_each_merged_body_element(dataset, overlay, [&](const MergedElementRef& element) {
		if (found) {
			return;
		}
		if (element.tag() == target_tag) {
			const auto normalized_vr = normalize_vr_for_write(element.tag(), element.vr());
			const auto raw_length = element.value_span().size();
			const auto full_length = padded_length(raw_length);
			write_element_header(measuring_writer, element.tag(), normalized_vr,
			    checked_u32(full_length, "element length"), false, explicit_vr);
			found = true;
			return;
		}
		if (element.from_overlay()) {
			write_non_sequence_element(measuring_writer, element.tag(), element.vr(),
			    element.value_span(), explicit_vr);
		} else {
			write_data_element(*element.source, measuring_writer, explicit_vr);
		}
	});

	if (!found) {
		diag::error_and_throw(
		    "write_with_transfer_syntax reason=expected metadata tag {} not found while preparing backpatch",
		    target_tag.to_string());
	}
	return measuring_writer.written;
}

template <typename Writer>
void backpatch_lossy_ratio_or_throw(Writer& writer, std::string_view file_path,
    uid::WellKnown transfer_syntax, std::size_t uncompressed_payload_bytes,
    std::size_t encoded_payload_bytes,
    const LossyRatioBackpatchState& backpatch_state) {
	if (encoded_payload_bytes == 0 || uncompressed_payload_bytes == 0) {
		diag::error_and_throw(
		    "write_with_transfer_syntax file={} ts={} reason=cannot compute lossy compression ratio from zero payload size (uncompressed={} encoded={})",
		    file_path, transfer_syntax.value(), uncompressed_payload_bytes,
		    encoded_payload_bytes);
	}

	const double ratio = static_cast<double>(uncompressed_payload_bytes) /
	    static_cast<double>(encoded_payload_bytes);
	auto ratio_text =
	    format_lossy_ratio_ds_value_or_throw(file_path, transfer_syntax, ratio);
	if (ratio_text.size() > backpatch_state.token_width) {
		diag::error_and_throw(
		    "write_with_transfer_syntax file={} ts={} reason=lossy compression ratio {} exceeds placeholder width {}",
		    file_path, transfer_syntax.value(), ratio, backpatch_state.token_width);
	}
	ratio_text.resize(backpatch_state.token_width, ' ');

	writer.overwrite(backpatch_state.absolute_token_offset,
	    std::span<const std::uint8_t>(
	        reinterpret_cast<const std::uint8_t*>(ratio_text.data()), ratio_text.size()));
}

[[nodiscard]] inline std::uint32_t measure_meta_group_length_with_overlay(
    const DataSet& dataset, const TransientWriteOverlay& overlay) {
	CountingWriter measuring_writer;
	for_each_merged_file_meta_element(dataset, overlay, [&](const MergedElementRef& element) {
		if (element.tag() == kFileMetaGroupLengthTag) {
			return;
		}
		write_non_sequence_element(measuring_writer, element.tag(), element.vr(),
		    element.value_span(), true);
	});
	return checked_u32(measuring_writer.written, "file meta group length");
}

template <typename Writer>
void write_file_meta_group_with_overlay(Writer& writer, const DataSet& dataset,
    const TransientWriteOverlay& overlay) {
	const auto meta_group_length =
	    measure_meta_group_length_with_overlay(dataset, overlay);
	std::array<std::uint8_t, 4> meta_group_length_bytes{};
	endian::store_le<std::uint32_t>(meta_group_length_bytes.data(), meta_group_length);
	write_non_sequence_element(
	    writer, kFileMetaGroupLengthTag, VR::UL, meta_group_length_bytes, true);

	for_each_merged_file_meta_element(dataset, overlay, [&](const MergedElementRef& element) {
		if (element.tag() == kFileMetaGroupLengthTag) {
			return;
		}
		write_non_sequence_element(writer, element.tag(), element.vr(),
		    element.value_span(), true);
	});
}

template <typename Writer, typename PixelWriter>
void write_dataset_with_overlay_and_pixel_writer(const DataSet& dataset,
    const TransientWriteOverlay& overlay, Writer& writer, bool explicit_vr,
    PixelWriter&& pixel_writer) {
	for_each_merged_body_element(dataset, overlay, [&](const MergedElementRef& element) {
		if (element.tag() == kPixelDataTag) {
			if (element.source == nullptr) {
				diag::error_and_throw(
				    "write_with_transfer_syntax reason=PixelData cannot be supplied from overlay");
			}
			pixel_writer(*element.source, writer, explicit_vr);
			return;
		}
		if (element.from_overlay()) {
			write_non_sequence_element(writer, element.tag(), element.vr(),
			    element.value_span(), explicit_vr);
		} else {
			write_data_element(*element.source, writer, explicit_vr);
		}
	});
}

template <typename Writer, typename PixelWriter>
void write_dataset_body_with_overlay_and_pixel_writer(Writer& writer,
    const DataSet& dataset, const TransientWriteOverlay& overlay,
    const DatasetEncoding& encoding, const std::string& file_path,
    PixelWriter&& pixel_writer) {
	if (encoding.convert_body_to_big_endian && encoding.deflate_body) {
		diag::error_and_throw(
		    "write_to_stream reason=unsupported encoding pipeline: both big-endian conversion and deflate requested");
	}

	if (!encoding.convert_body_to_big_endian && !encoding.deflate_body) {
		write_dataset_with_overlay_and_pixel_writer(dataset, overlay, writer,
		    encoding.explicit_vr, std::forward<PixelWriter>(pixel_writer));
		return;
	}

	std::vector<std::uint8_t> body;
	body.reserve(4096);
	VectorWriter body_writer(body);
	write_dataset_with_overlay_and_pixel_writer(dataset, overlay, body_writer,
	    encoding.explicit_vr, std::forward<PixelWriter>(pixel_writer));

	if (encoding.convert_body_to_big_endian) {
		body = convert_little_endian_dataset_to_big_endian(body, 0, file_path);
	}
	if (encoding.deflate_body) {
		body = deflate_dataset_body(body, file_path);
	}
	if (!body.empty()) {
		writer.append(body.data(), body.size());
	}
}

template <typename Writer>
void write_current_dataset_direct_with_overlay(const DicomFile& file,
    const TransientWriteOverlay& overlay, Writer& writer,
    uid::WellKnown transfer_syntax, const WriteOptions& options) {
	const auto& dataset = file.dataset();
	const auto encoding = determine_dataset_encoding(transfer_syntax, dataset);

	if (options.include_preamble) {
		write_preamble(writer);
	}
	if (options.write_file_meta) {
		write_file_meta_group_with_overlay(writer, dataset, overlay);
	}
	write_dataset_body_with_overlay_and_pixel_writer(writer, dataset, overlay,
	    encoding, file.path(),
	    [&](const DataElement& element, auto& direct_writer, bool explicit_vr) {
		    write_data_element(element, direct_writer, explicit_vr);
	    });
}

}  // namespace dicom::write_detail
