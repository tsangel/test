#pragma once

#include "writing/detail/dataset_write.hpp"

namespace dicom::write_detail {

inline void throw_if_split_pixel_payload_is_detached(const DataElement& element) {
	if (detail::is_detached_pixel_payload_marker(element)) {
		throw_write_stage_error("write_split_pixel_payload",
		    "PixelData payload is detached");
	}
}

inline void reserve_for_split_append(
    std::vector<std::uint8_t>& bytes, std::size_t additional_bytes) {
	if (additional_bytes == 0) {
		return;
	}
	if (bytes.size() >
	    std::numeric_limits<std::size_t>::max() - additional_bytes) {
		throw_write_stage_error("write_split_pixel_payload",
		    "split payload size overflow current={} additional={}",
		    bytes.size(), additional_bytes);
	}
	const auto target_size = bytes.size() + additional_bytes;
	if (target_size > bytes.capacity()) {
		bytes.reserve(target_size);
	}
}

[[nodiscard]] inline std::size_t split_main_dicom_reserve_hint(
    const DataSet& dataset, const WriteOptions& options) {
	constexpr std::size_t kBaseBytes = 4096;
	constexpr std::size_t kPerElementBytes = 128;
	constexpr std::size_t kMaxHintBytes = 8u * 1024u * 1024u;

	std::size_t hint = kBaseBytes + (options.include_preamble ? 132u : 0u);
	const auto element_count = dataset.size();
	if (element_count >
	    (std::numeric_limits<std::size_t>::max() - hint) / kPerElementBytes) {
		return kMaxHintBytes;
	}
	hint += element_count * kPerElementBytes;
	return std::min(hint, kMaxHintBytes);
}

[[nodiscard]] inline std::size_t pixel_sequence_value_reserve_size(
    const DataElement& element) {
	CountingWriter writer;
	write_pixel_sequence_value(element, writer);
	return writer.written;
}

inline void append_split_native_pixel_payload_value(std::vector<std::uint8_t>& payload,
    const DataElement& element, const DatasetWritePlan& write_plan) {
	(void)write_plan;
	throw_if_split_pixel_payload_is_detached(element);
	const auto value = element.value_span();
	reserve_for_split_append(payload, padded_length(value.size()));
	payload.insert(payload.end(), value.begin(), value.end());
	if ((value.size() & 1u) != 0u) {
		const auto normalized_vr = normalize_vr_for_write(element.tag(), element.vr());
		payload.push_back(normalized_vr.padding_byte());
	}
}

inline void append_split_pixel_payload_value(std::vector<std::uint8_t>& payload,
    const DataElement& element, const DatasetWritePlan& write_plan) {
	throw_if_split_pixel_payload_is_detached(element);
	if (element.vr().is_pixel_sequence()) {
		reserve_for_split_append(payload, pixel_sequence_value_reserve_size(element));
		BufferWriter payload_writer(payload);
		write_pixel_sequence_value(element, payload_writer);
		return;
	}
	if (!element.vr().is_binary()) {
		throw_write_stage_error("write_split_pixel_payload",
		    "PixelData must be native binary or encapsulated PX");
	}
	append_split_native_pixel_payload_value(payload, element, write_plan);
}

template <typename FrameProvider>
void append_split_native_pixel_payload_value_from_frame_provider(
    std::vector<std::uint8_t>& payload, const DataElement& element,
    const DatasetWritePlan& write_plan, VR native_pixel_vr,
    std::size_t total_payload_bytes, std::size_t frame_count,
    std::size_t frame_payload_bytes, FrameProvider&& frame_provider) {
	(void)element;
	(void)write_plan;
	reserve_for_split_append(payload, padded_length(total_payload_bytes));
	BufferWriter payload_writer(payload);
	write_native_pixel_data_value_from_frame_provider(payload_writer, native_pixel_vr,
	    total_payload_bytes, frame_count, frame_payload_bytes,
	    std::forward<FrameProvider>(frame_provider));
}

template <typename Writer>
void write_split_pixel_placeholder_element(
    const DataElement& element, Writer& writer, bool explicit_vr) {
	write_non_sequence_element(writer, element.tag(), VR::OB,
	    std::span<const std::uint8_t>(
	        kPixelPayloadPlaceholderMagic.data(), kPixelPayloadPlaceholderMagic.size()),
	    explicit_vr);
}

inline void finalize_split_pixel_payload_result_or_throw(
    const SplitPixelPayloadWriteResult& result) {
	if (result.pixel_payload_bytes.empty()) {
		throw_write_stage_error("write_split_pixel_payload",
		    "PixelData payload is empty");
	}
}

}  // namespace dicom::write_detail
