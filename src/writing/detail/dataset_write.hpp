#pragma once

#include "dataset_deflate_codec.h"
#include "dataset_endian_converter.h"
#include "writing/detail/write_sinks.hpp"

#include <limits>
#include <string>
#include <utility>

namespace dicom::write_detail {

struct PixelSequenceOffsetTables {
	std::vector<std::uint32_t> basic_offsets{};
	std::vector<std::uint64_t> extended_offsets{};
	std::vector<std::uint64_t> extended_lengths{};
	bool use_extended{false};
};

[[nodiscard]] inline std::span<const std::uint8_t> u64_values_as_bytes(
    const std::vector<std::uint64_t>& values) noexcept {
	if (values.empty()) {
		return {};
	}
	return {
	    reinterpret_cast<const std::uint8_t*>(values.data()),
	    values.size() * sizeof(std::uint64_t),
	};
}

// Computes BOT/EOT state from the currently materialized or stream-backed frame layout.
[[nodiscard]] inline PixelSequenceOffsetTables compute_pixel_sequence_offset_tables(
    const PixelSequence& pixel_sequence, const InStream* seq_stream) {
	constexpr std::size_t kItemHeaderBytes = 8u;
	PixelSequenceOffsetTables tables{};
	const auto frame_count = pixel_sequence.number_of_frames();
	tables.basic_offsets.reserve(frame_count);
	tables.extended_offsets.reserve(frame_count);
	tables.extended_lengths.reserve(frame_count);
	std::size_t next_frame_offset = 0u;
	bool basic_offset_table_overflow = false;
	bool eot_eligible = true;

	// Walk frames once to collect per-frame offsets and decide whether EOT is usable.
	for (std::size_t frame_index = 0; frame_index < frame_count; ++frame_index) {
		const PixelFrame* frame = pixel_sequence.frame(frame_index);
		if (!frame) {
			continue;
		}

		std::size_t frame_fragment_count = 0;
		std::uint64_t frame_offset = 0;
		std::uint64_t frame_length = 0;

		const auto append_fragment = [&](std::size_t fragment_length) {
			if (frame_fragment_count == 0) {
				frame_offset = static_cast<std::uint64_t>(next_frame_offset);
				if (!basic_offset_table_overflow) {
					if (next_frame_offset > std::numeric_limits<std::uint32_t>::max()) {
						basic_offset_table_overflow = true;
						tables.basic_offsets.clear();
					} else {
						tables.basic_offsets.push_back(
						    static_cast<std::uint32_t>(next_frame_offset));
					}
				}
			}
			++frame_fragment_count;
			if (frame_fragment_count > 1) {
				eot_eligible = false;
			}
			if (fragment_length >
			    std::numeric_limits<std::uint64_t>::max() - frame_length) {
				diag::error_and_throw(
				    "write_to_stream reason=encapsulated frame length exceeds uint64 range");
			}
			frame_length += static_cast<std::uint64_t>(fragment_length);

			const auto full_fragment_length = padded_length(fragment_length);
			if (next_frame_offset > std::numeric_limits<std::size_t>::max() - kItemHeaderBytes ||
			    full_fragment_length >
			        std::numeric_limits<std::size_t>::max() - next_frame_offset -
			            kItemHeaderBytes) {
				diag::error_and_throw(
				    "write_to_stream reason=encapsulated pixel frame size exceeds size_t range");
			}
			next_frame_offset += kItemHeaderBytes + full_fragment_length;
		};

		const auto encoded_data = frame->encoded_data_view();
		if (!encoded_data.empty()) {
			append_fragment(encoded_data.size());
		} else {
			const auto& fragments = frame->fragments();
			for (const auto& fragment : fragments) {
				if (!seq_stream) {
					diag::error_and_throw(
					    "write_to_stream reason=pixel fragment references stream but stream is null");
				}
				if (fragment.offset > seq_stream->end_offset() ||
				    fragment.length > seq_stream->end_offset() - fragment.offset) {
					diag::error_and_throw(
					    "write_to_stream reason=pixel fragment out of bounds offset=0x{:X} length={}",
					    fragment.offset, fragment.length);
				}
				append_fragment(fragment.length);
			}
		}

		if (frame_fragment_count == 0) {
			eot_eligible = false;
			continue;
		}
		tables.extended_offsets.push_back(frame_offset);
		tables.extended_lengths.push_back(frame_length);
	}

	// EOT is only emitted when single-fragment frames overflow BOT's 32-bit offsets.
	if (basic_offset_table_overflow && eot_eligible && !tables.extended_offsets.empty() &&
	    tables.extended_offsets.size() == tables.extended_lengths.size()) {
		tables.use_extended = true;
		tables.basic_offsets.clear();
	} else {
		tables.extended_offsets.clear();
		tables.extended_lengths.clear();
	}
	return tables;
}

template <typename Writer>
void write_non_sequence_element(Writer& writer, Tag tag, VR vr,
    std::span<const std::uint8_t> value, bool explicit_vr) {
	const auto normalized_vr = normalize_vr_for_write(tag, vr);
	const auto raw_length = value.size();
	const auto even_value_length = padded_length(raw_length);
	write_element_header(
	    writer, tag, normalized_vr, checked_u32(even_value_length, "element length"), false,
	    explicit_vr);
	if (!value.empty()) {
		writer.append(value.data(), value.size());
	}
	if ((raw_length & 1u) != 0u) {
		writer.append_byte(normalized_vr.padding_byte());
	}
}

template <typename Writer>
void write_nested_dataset(const DataSet& dataset, Writer& writer, bool explicit_vr);

template <typename Writer>
void write_sequence_element(const DataElement& element, Writer& writer, bool explicit_vr) {
	const Sequence* sequence = element.as_sequence();
	if (!sequence) {
		diag::error_and_throw("write_to_stream reason=SQ element has null sequence pointer");
	}

	write_element_header(writer, element.tag(), VR::SQ, 0xFFFFFFFFu, true, explicit_vr);
	for (const auto& item_dataset_ptr : *sequence) {
		if (!item_dataset_ptr) {
			continue;
		}
		write_item_header(writer, kItemTag, 0xFFFFFFFFu);
		write_nested_dataset(*item_dataset_ptr, writer, explicit_vr);
		write_item_header(writer, kItemDelimitationTag, 0u);
	}
	write_item_header(writer, kSequenceDelimitationTag, 0u);
}

template <typename Writer>
void write_pixel_sequence_element(const DataElement& element, Writer& writer,
    bool explicit_vr) {
	const PixelSequence* pixel_sequence = element.as_pixel_sequence();
	if (!pixel_sequence) {
		diag::error_and_throw("write_to_stream reason=PX element has null pixel sequence pointer");
	}

	const InStream* seq_stream = pixel_sequence->stream();
	const auto offset_tables = compute_pixel_sequence_offset_tables(*pixel_sequence, seq_stream);
	const DataSet* owning_dataset = element.parent();
	const bool allow_extended_offset_table = owning_dataset != nullptr &&
	    owning_dataset == owning_dataset->root_dataset();
	// Extended Offset Tables are only synthesized for the instance-level PixelData.
	if (offset_tables.use_extended && allow_extended_offset_table) {
		const auto extended_offsets_bytes =
		    u64_values_as_bytes(offset_tables.extended_offsets);
		const auto extended_lengths_bytes =
		    u64_values_as_bytes(offset_tables.extended_lengths);
		write_non_sequence_element(
		    writer, "ExtendedOffsetTable"_tag, VR::OV, extended_offsets_bytes, explicit_vr);
		write_non_sequence_element(writer, "ExtendedOffsetTableLengths"_tag, VR::OV,
		    extended_lengths_bytes, explicit_vr);
	}

	// Write PixelData itself as an undefined-length item sequence.
	const VR pixel_vr = explicit_vr ? VR::OB : VR::None;
	write_element_header(writer, element.tag(), pixel_vr, 0xFFFFFFFFu, true, explicit_vr);

	const auto basic_offset_table_length =
	    checked_u32(offset_tables.basic_offsets.size() * sizeof(std::uint32_t),
	        "basic offset table length");
	write_item_header(writer, kItemTag, basic_offset_table_length);
	for (const auto offset : offset_tables.basic_offsets) {
		write_u32(writer, offset);
	}

	const auto write_fragment = [&](std::span<const std::uint8_t> fragment) {
		const auto even_value_length = padded_length(fragment.size());
		write_item_header(
		    writer, kItemTag, checked_u32(even_value_length, "pixel fragment length"));
		if (!fragment.empty()) {
			writer.append(fragment.data(), fragment.size());
		}
		if ((fragment.size() & 1u) != 0u) {
			writer.append_byte(0x00u);
		}
	};

	// Emit each stored frame either from materialized bytes or from referenced fragments.
	for (std::size_t frame_index = 0; frame_index < pixel_sequence->number_of_frames();
	     ++frame_index) {
		const PixelFrame* frame = pixel_sequence->frame(frame_index);
		if (!frame) {
			continue;
		}

		const auto encoded_data = frame->encoded_data_view();
		if (!encoded_data.empty()) {
			write_fragment(encoded_data);
			continue;
		}

		const auto& fragments = frame->fragments();
		for (const auto& fragment : fragments) {
			if (!seq_stream) {
				diag::error_and_throw(
				    "write_to_stream reason=pixel fragment references stream but stream is null");
			}
			if (fragment.offset > seq_stream->end_offset() ||
			    fragment.length > seq_stream->end_offset() - fragment.offset) {
				diag::error_and_throw(
				    "write_to_stream reason=pixel fragment out of bounds offset=0x{:X} length={}",
				    fragment.offset, fragment.length);
			}
			write_fragment(seq_stream->get_span(fragment.offset, fragment.length));
		}
	}

	write_item_header(writer, kSequenceDelimitationTag, 0u);
}

template <typename Writer>
void write_data_element(const DataElement& element, Writer& writer, bool explicit_vr) {
	if (element.vr().is_sequence()) {
		write_sequence_element(element, writer, explicit_vr);
		return;
	}
	if (element.vr().is_pixel_sequence()) {
		write_pixel_sequence_element(element, writer, explicit_vr);
		return;
	}

	write_non_sequence_element(
	    writer, element.tag(), element.vr(), element.value_span(), explicit_vr);
}

template <typename Writer>
void write_nested_dataset(const DataSet& dataset, Writer& writer, bool explicit_vr) {
	// Sequence item datasets never carry file meta and don't need root-level EOT filtering.
	for (const auto& element : dataset) {
		write_data_element(element, writer, explicit_vr);
	}
}

template <typename Writer>
void write_dataset(
    const DataSet& dataset, Writer& writer, bool explicit_vr, bool skip_group_0002) {
	const auto& pixel_data_element = dataset.get_dataelement("PixelData"_tag);
	const bool has_encapsulated_pixel_data =
	    !pixel_data_element.is_missing() && pixel_data_element.vr().is_pixel_sequence();
	for (const auto& element : dataset) {
		// Skip file meta when serializing the dataset body and suppress stale EOT tags
		// because encapsulated PixelData writes regenerate them from current frame state.
		if (skip_group_0002 && element.tag().group() == 0x0002u) {
			continue;
		}
		if (has_encapsulated_pixel_data &&
		    (element.tag() == "ExtendedOffsetTable"_tag ||
		        element.tag() == "ExtendedOffsetTableLengths"_tag)) {
			continue;
		}
		write_data_element(element, writer, explicit_vr);
	}
}

template <typename Writer, typename PixelWriter>
void write_dataset_with_pixel_writer(const DataSet& dataset, Writer& writer,
    bool explicit_vr, bool skip_group_0002, PixelWriter&& pixel_writer) {
	// This root-dataset variant lets callers substitute top-level PixelData emission
	// while reusing normal element writes for everything else.
	for (const auto& element : dataset) {
		if (skip_group_0002 && element.tag().group() == 0x0002u) {
			continue;
		}
		if (element.tag() == "ExtendedOffsetTable"_tag ||
		    element.tag() == "ExtendedOffsetTableLengths"_tag) {
			continue;
		}
		if (element.tag() == "PixelData"_tag) {
			pixel_writer(element, writer, explicit_vr);
			continue;
		}
		write_data_element(element, writer, explicit_vr);
	}
}

template <typename Writer>
void write_root_dataset_body(Writer& writer, const DataSet& dataset,
    const DatasetWritePlan& write_plan, const std::string& file_path) {
	std::vector<std::uint8_t> body;
	body.reserve(4096);
	BufferWriter body_writer(body);

	// Serialize into a temporary little-endian body first, then apply TS-specific transforms.
	write_dataset(dataset, body_writer, write_plan.explicit_vr, true);

	if (write_plan.convert_body_to_big_endian && write_plan.deflate_body) {
		diag::error_and_throw(
		    "write_to_stream reason=unsupported encoding pipeline: both big-endian conversion and deflate requested");
	}

	if (write_plan.convert_body_to_big_endian) {
		body = convert_little_endian_dataset_to_big_endian(body, 0, file_path);
	}
	if (write_plan.deflate_body) {
		body = deflate_dataset_body(body, file_path);
	}

	if (!body.empty()) {
		writer.append(body.data(), body.size());
	}
}

template <typename Writer>
void write_pixel_fragment(Writer& writer, std::span<const std::uint8_t> fragment) {
	const auto even_value_length = padded_length(fragment.size());
	write_item_header(
	    writer, kItemTag, checked_u32(even_value_length, "pixel fragment length"));
	if (!fragment.empty()) {
		writer.append(fragment.data(), fragment.size());
	}
	if ((fragment.size() & 1u) != 0u) {
		writer.append_byte(0x00u);
	}
}

template <typename Writer, typename PixelWriter>
void write_root_dataset_body_with_pixel_writer(Writer& writer, const DataSet& dataset,
    const DatasetWritePlan& write_plan, const std::string& file_path,
    PixelWriter&& pixel_writer) {
	if (write_plan.convert_body_to_big_endian && write_plan.deflate_body) {
		diag::error_and_throw(
		    "write_to_stream reason=unsupported encoding pipeline: both big-endian conversion and deflate requested");
	}

	if (!write_plan.convert_body_to_big_endian && !write_plan.deflate_body) {
		// Fast path: stream elements directly when no post-processing is required.
		write_dataset_with_pixel_writer(
		    dataset, writer, write_plan.explicit_vr, true,
		    std::forward<PixelWriter>(pixel_writer));
		return;
	}

	std::vector<std::uint8_t> body;
	body.reserve(4096);
	BufferWriter body_writer(body);
	// Slow path: serialize to a temporary body so endian conversion or deflate can rewrite it.
	write_dataset_with_pixel_writer(dataset, body_writer, write_plan.explicit_vr, true,
	    std::forward<PixelWriter>(pixel_writer));

	if (write_plan.convert_body_to_big_endian) {
		body = convert_little_endian_dataset_to_big_endian(body, 0, file_path);
	}
	if (write_plan.deflate_body) {
		body = deflate_dataset_body(body, file_path);
	}
	if (!body.empty()) {
		writer.append(body.data(), body.size());
	}
}

template <typename Writer, typename FrameProvider>
void write_native_pixel_data_from_frame_provider(Writer& writer,
    const DataElement& element, bool explicit_vr, VR native_pixel_vr,
    std::size_t total_payload_bytes, std::size_t frame_count,
    std::size_t frame_payload_bytes, FrameProvider&& frame_provider) {
	const auto even_value_length = padded_length(total_payload_bytes);
	write_element_header(writer, element.tag(), explicit_vr ? native_pixel_vr : VR::None,
	    checked_u32(even_value_length, "native PixelData length"), false, explicit_vr);

	std::size_t bytes_written = 0;
	// Stream each frame payload and verify the provider matches the precomputed layout.
	for (std::size_t frame_index = 0; frame_index < frame_count; ++frame_index) {
		const auto frame_bytes = frame_provider(frame_index);
		if (frame_bytes.size() != frame_payload_bytes) {
			diag::error_and_throw(
			    "write_with_transfer_syntax reason=native output frame size mismatch frame={} expected={} actual={}",
			    frame_index, frame_payload_bytes, frame_bytes.size());
		}
		writer.append(frame_bytes.data(), frame_bytes.size());
		if (bytes_written > std::numeric_limits<std::size_t>::max() - frame_bytes.size()) {
			diag::error_and_throw(
			    "write_with_transfer_syntax reason=native PixelData byte count overflow");
		}
		bytes_written += frame_bytes.size();
	}

	if (bytes_written != total_payload_bytes) {
		diag::error_and_throw(
		    "write_with_transfer_syntax reason=native PixelData length mismatch expected={} actual={}",
		    total_payload_bytes, bytes_written);
	}
	if ((total_payload_bytes & 1u) != 0u) {
		writer.append_byte(native_pixel_vr.padding_byte());
	}
}


}  // namespace dicom::write_detail
