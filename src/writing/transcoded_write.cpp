#include "writing/transcoded_write.hpp"

#include "writing/detail/overlay_write.hpp"
#include "pixel/host/adapter/host_adapter.hpp"
#include "pixel/host/decode/decode_frame_dispatch.hpp"
#include "pixel/host/encode/encode_set_pixel_data_runner.hpp"
#include "pixel/host/encode/encode_target_policy.hpp"
#include "pixel/host/encode/multicomponent_transform_policy.hpp"
#include "pixel/host/support/dicom_pixel_support.hpp"

#include <exception>
#include <limits>
#include <optional>
#include <string_view>
#include <vector>

namespace dicom::write_detail {
namespace {

// Builds the normalized native frame layout used by streaming re-encode.
[[nodiscard]] pixel::PixelLayout resolve_decoded_source_layout_for_write_or_throw(
    DicomFile& file, uid::WellKnown target_ts, const pixel::DecodePlan& decode_plan) {
	if (decode_plan.output_layout.empty()) {
		diag::error_and_throw(
		    "write_with_transfer_syntax file={} target_ts={} reason=decoded frame layout is empty",
		    file.path(), target_ts.value());
	}

	// The decode plan already carries the exact normalized frame layout used for re-encode.
	return decode_plan.output_layout;
}

// Builds the normalized native source span directly from on-disk PixelData.
[[nodiscard]] pixel::ConstPixelSpan build_native_source_span_for_write(
    DicomFile& file, uid::WellKnown target_ts) {
	DataSet& dataset = file.dataset();
	const auto& pixel_data = dataset["PixelData"_tag];
	if (!pixel_data || pixel_data.vr().is_pixel_sequence() || !pixel_data.vr().is_binary()) {
		diag::error_and_throw(
		    "write_with_transfer_syntax file={} target_ts={} reason=PixelData must be native binary for native source write path",
		    file.path(), target_ts.value());
	}

	const auto layout = file.native_pixel_layout();
	if (!layout.has_value()) {
		diag::error_and_throw(
		    "write_with_transfer_syntax file={} target_ts={} reason=invalid native pixel metadata for normalized layout reconstruction",
		    file.path(), target_ts.value());
	}

	const auto source_bytes = pixel_data.value_span();

	std::size_t required_bytes = 0;
	if (!pixel::try_pixel_storage_size(*layout, required_bytes) ||
	    source_bytes.size() < required_bytes) {
		diag::error_and_throw(
		    "write_with_transfer_syntax file={} target_ts={} reason=PixelData bytes({}) are shorter than required native frame payload({})",
		    file.path(), target_ts.value(), source_bytes.size(), required_bytes);
	}

	return pixel::ConstPixelSpan{
	    .layout = *layout,
	    .bytes = source_bytes.first(required_bytes),
	};
}

template <typename FrameProvider>
std::size_t measure_encoded_payload_bytes_from_frame_provider(DicomFile& file,
    uid::WellKnown transfer_syntax, const pixel::PixelLayout& source_layout,
    uint32_t codec_profile_code, std::span<const pixel::CodecOptionKv> codec_options,
    bool use_multicomponent_transform, std::size_t frame_count,
    FrameProvider&& frame_provider) {
	// Used only for non-seekable lossy outputs where the final ratio cannot be backpatched.
	std::size_t encoded_payload_bytes = 0;
	pixel::detail::encode_frames_from_frame_provider_with_runtime_or_throw(
	    file, transfer_syntax, source_layout, codec_profile_code, codec_options,
	    use_multicomponent_transform, frame_count,
	    std::forward<FrameProvider>(frame_provider),
	    [&](std::size_t, std::vector<std::uint8_t>&& encoded_frame) {
		    if (encoded_payload_bytes >
		        std::numeric_limits<std::size_t>::max() - encoded_frame.size()) {
			    diag::error_and_throw(
			        "write_with_transfer_syntax file={} target_ts={} reason=encoded payload size overflow during lossy prepass",
			        file.path(), transfer_syntax.value());
		    }
		    encoded_payload_bytes += encoded_frame.size();
	    });
	return encoded_payload_bytes;
}

template <typename Writer>
void write_current_dataset_as_is(DicomFile& file, Writer& writer,
    uid::WellKnown transfer_syntax, const WriteOptions& options) {
	const auto& dataset = file.dataset();
	const auto write_plan = determine_dataset_write_plan(transfer_syntax, dataset);
	if (options.include_preamble) {
		write_preamble(writer);
	}
	if (options.write_file_meta) {
		write_file_meta_group(writer, dataset);
	}
	write_root_dataset_body_with_pixel_writer(
	    writer, dataset, write_plan, file.path(),
	    [](const DataElement& element, auto& direct_writer, bool explicit_vr) {
 		    write_data_element(element, direct_writer, explicit_vr);
 	    });
}

// Captures whether this write is a plain serialization, native/encapsulated conversion, or transcode.
struct TransferSyntaxWriteDecision {
	uid::WellKnown source_transfer_syntax{};
	bool has_float_pixel_data{false};
	bool same_transfer_syntax{false};
	bool has_native_pixel_data{false};
	bool has_encapsulated_pixel_data{false};
	bool target_is_encapsulated{false};
	bool needs_native_to_encapsulated{false};
	bool needs_encapsulated_to_native{false};
	bool needs_encapsulated_transcode{false};
	bool needs_pixel_transcode{false};
};

// Classifies the write path before any expensive decode/encode setup happens.
[[nodiscard]] TransferSyntaxWriteDecision classify_transfer_syntax_write(
    const DicomFile& file, const DataSet& dataset,
    uid::WellKnown target_transfer_syntax) {
	TransferSyntaxWriteDecision decision{};
	decision.source_transfer_syntax = file.transfer_syntax_uid();
	const auto& pixel_data = dataset["PixelData"_tag];
	decision.has_float_pixel_data =
	    dataset["FloatPixelData"_tag].is_present() ||
	    dataset["DoubleFloatPixelData"_tag].is_present();
	decision.same_transfer_syntax =
	    decision.source_transfer_syntax.valid() &&
	    decision.source_transfer_syntax == target_transfer_syntax;
	decision.has_native_pixel_data =
	    pixel_data && pixel_data.vr().is_binary() && !pixel_data.vr().is_pixel_sequence();
	decision.has_encapsulated_pixel_data =
	    pixel_data && pixel_data.vr().is_pixel_sequence();
	decision.target_is_encapsulated = target_transfer_syntax.is_encapsulated();
	decision.needs_native_to_encapsulated =
	    decision.has_native_pixel_data && decision.target_is_encapsulated;
	decision.needs_encapsulated_to_native =
	    decision.has_encapsulated_pixel_data && !decision.target_is_encapsulated;
	decision.needs_encapsulated_transcode =
	    decision.has_encapsulated_pixel_data && decision.target_is_encapsulated &&
	    !decision.same_transfer_syntax;
	decision.needs_pixel_transcode =
	    decision.needs_native_to_encapsulated ||
	    decision.needs_encapsulated_to_native ||
	    decision.needs_encapsulated_transcode;
	return decision;
}

// Carries all state shared between prepare and emit steps for streaming writes.
struct PreparedStreamingTranscodeState {
	pixel::PixelLayout source_decode_layout{};
	pixel::PixelLayout source_layout{};
	std::span<const std::uint8_t> source_bytes{};
	std::optional<pixel::DecodePlan> decode_plan{};
	pixel::EncoderContext staged_encoder_ctx{};
	const pixel::EncoderContext* active_encoder_ctx{nullptr};
	uint32_t codec_profile_code{PIXEL_CODEC_PROFILE_UNKNOWN};
	pixel::support_detail::ComputedEncodeSourceLayout encode_source_layout{};
	bool use_multicomponent_transform{false};
	pixel::Photometric output_photometric{pixel::Photometric::monochrome2};
	bool backpatch_lossy_ratio{false};
	std::size_t encoded_payload_bytes{0};
	std::vector<std::uint8_t> decoded_frame{};
	TransientWriteOverlay overlay{};
	std::optional<LossyRatioBackpatchState> lossy_ratio_backpatch{};
	DatasetWritePlan write_plan{};
};

// Resolves the normalized source layout used by downstream encode/write loops.
void prepare_streaming_source_layout_or_throw(
    PreparedStreamingTranscodeState& state, DicomFile& file,
    uid::WellKnown target_transfer_syntax,
    const TransferSyntaxWriteDecision& decision) {
	if (decision.needs_native_to_encapsulated) {
		// Native PixelData can be described directly without a decode plan.
		const auto source = build_native_source_span_for_write(file, target_transfer_syntax);
		state.source_layout = source.layout;
		state.source_bytes = source.bytes;
		return;
	}

	// Encapsulated sources need a decode plan so the write path knows the exact
	// native frame layout it will materialize on demand.
	pixel::DecodeOptions decode_options{};
	decode_options.alignment = 1;
	decode_options.planar_out = state.source_decode_layout.planar;
	decode_options.decode_mct = false;
	state.decode_plan = file.create_decode_plan(decode_options);
	if (state.decode_plan->output_layout.empty()) {
		diag::error_and_throw(
		    "write_with_transfer_syntax file={} target_ts={} reason=calculated native frame size is zero for streaming write",
		    file.path(), target_transfer_syntax.value());
	}
	state.source_layout =
	    resolve_decoded_source_layout_for_write_or_throw(
	        file, target_transfer_syntax, *state.decode_plan);
	state.source_bytes = {};
}

// Chooses which encoder context supplies codec options for this write.
void prepare_streaming_encoder_context_or_throw(
    PreparedStreamingTranscodeState& state, DicomFile& file,
    uid::WellKnown target_transfer_syntax, WriteEncoderConfigSource encode_mode,
    std::span<const pixel::CodecOptionTextKv> codec_opt_override,
    const pixel::EncoderContext* encoder_ctx,
    const TransferSyntaxWriteDecision& decision) {
	state.active_encoder_ctx = encoder_ctx;
	if (!decision.target_is_encapsulated) {
		return;
	}

	if (encode_mode == WriteEncoderConfigSource::use_plugin_defaults) {
		state.staged_encoder_ctx.configure(target_transfer_syntax);
		state.active_encoder_ctx = &state.staged_encoder_ctx;
	} else if (encode_mode == WriteEncoderConfigSource::use_explicit_options) {
		state.staged_encoder_ctx.configure(target_transfer_syntax, codec_opt_override);
		state.active_encoder_ctx = &state.staged_encoder_ctx;
	}
	if (state.active_encoder_ctx == nullptr || !state.active_encoder_ctx->configured()) {
		diag::error_and_throw(
		    "write_with_transfer_syntax file={} target_ts={} reason=encoder context is not configured",
		    file.path(), target_transfer_syntax.value());
	}
	if (state.active_encoder_ctx->transfer_syntax_uid() != target_transfer_syntax) {
		diag::error_and_throw(
		    "write_with_transfer_syntax file={} target_ts={} ctx_ts={} reason=encoder context transfer syntax mismatch",
		    file.path(), target_transfer_syntax.value(),
		    state.active_encoder_ctx->transfer_syntax_uid().value());
	}
}

// Computes encode policy such as profile, output photometric, and post-write ratio strategy.
void prepare_streaming_encode_policy_or_throw(
    PreparedStreamingTranscodeState& state, DicomFile& file,
    uid::WellKnown target_transfer_syntax,
    const TransferSyntaxWriteDecision& decision, bool writer_can_overwrite) {
	state.codec_profile_code = PIXEL_CODEC_PROFILE_UNKNOWN;
	if (!::pixel::runtime::codec_profile_code_from_transfer_syntax(
	        target_transfer_syntax, &state.codec_profile_code)) {
		diag::error_and_throw(
		    "write_with_transfer_syntax file={} ts={} reason=transfer syntax is not mapped to a runtime codec profile",
		    file.path(), target_transfer_syntax.value());
	}
	state.encode_source_layout =
	    pixel::support_detail::compute_encode_source_layout_without_source_bytes_or_throw(
	        state.source_layout, file.path());

	state.output_photometric = state.source_layout.photometric;
	state.backpatch_lossy_ratio =
	    decision.target_is_encapsulated &&
	    pixel::detail::encode_profile_uses_lossy_compression(state.codec_profile_code) &&
	    writer_can_overwrite;
	if (!decision.target_is_encapsulated) {
		return;
	}

	state.use_multicomponent_transform =
	    pixel::detail::should_use_multicomponent_transform(target_transfer_syntax,
	        state.codec_profile_code, state.active_encoder_ctx->codec_options(),
	        state.encode_source_layout.samples_per_pixel, file.path());
	state.output_photometric =
	    pixel::detail::compute_output_photometric_for_encode_profile(
	        state.codec_profile_code, state.use_multicomponent_transform,
	        state.source_layout.photometric);
}

// Runs the lossy size prepass only when backpatching is unavailable.
void measure_streaming_lossy_payload_prepass_if_needed_or_throw(
    PreparedStreamingTranscodeState& state, DicomFile& file,
    uid::WellKnown target_transfer_syntax,
    const TransferSyntaxWriteDecision& decision) {
	if (!decision.target_is_encapsulated ||
	    !pixel::detail::encode_profile_uses_lossy_compression(state.codec_profile_code) ||
	    state.backpatch_lossy_ratio) {
		return;
	}

	if (decision.needs_native_to_encapsulated) {
		// Native sources can hand frame spans directly to the encoder prepass.
		state.encoded_payload_bytes =
		    measure_encoded_payload_bytes_from_frame_provider(
		        file, target_transfer_syntax, state.source_layout,
		        state.codec_profile_code, state.active_encoder_ctx->codec_options(),
		        state.use_multicomponent_transform, state.encode_source_layout.frames,
		        [&](std::size_t frame_index) -> std::span<const std::uint8_t> {
			        const auto frame_offset =
			            frame_index * state.encode_source_layout.source_frame_stride;
			        return state.source_bytes.subspan(
			            frame_offset,
			            state.encode_source_layout.source_frame_size_bytes);
		        });
		return;
	}

	state.decoded_frame.resize(state.decode_plan->output_layout.frame_stride);
	// Encapsulated sources reuse one decode buffer while measuring the lossy payload.
	state.encoded_payload_bytes =
	    measure_encoded_payload_bytes_from_frame_provider(
	        file, target_transfer_syntax, state.source_layout,
	        state.codec_profile_code, state.active_encoder_ctx->codec_options(),
	        state.use_multicomponent_transform, state.encode_source_layout.frames,
	        [&](std::size_t frame_index) -> std::span<const std::uint8_t> {
		        auto frame_span = std::span<std::uint8_t>(
		            state.decoded_frame.data(), state.decoded_frame.size());
		        const auto prepared_source =
		            pixel::support_detail::prepare_decode_frame_source_without_cache_or_throw(
		                file, state.source_decode_layout, frame_index);
		        pixel::detail::dispatch_decode_prepared_frame(file.path(),
		            file.transfer_syntax_uid(), state.source_decode_layout, frame_index,
		            prepared_source.bytes, frame_span, *state.decode_plan);
		        return std::span<const std::uint8_t>(
		            frame_span.data(),
		            state.encode_source_layout.source_frame_size_bytes);
	        });
}

// Populates the transient metadata overlay for the write target.
void prepare_streaming_overlay_or_throw(PreparedStreamingTranscodeState& state,
    DicomFile& file, DataSet& dataset, uid::WellKnown target_transfer_syntax,
    const WriteOptions& options) {
	update_pixel_metadata_for_streaming_write_overlay(state.overlay, file.path(),
	    target_transfer_syntax, state.source_layout,
	    pixel::detail::is_rle_encode_profile(state.codec_profile_code),
	    state.output_photometric, state.encode_source_layout.bits_allocated,
	    state.encode_source_layout.bits_stored, state.encode_source_layout.high_bit,
	    state.encode_source_layout.pixel_representation);
	if (state.backpatch_lossy_ratio) {
		state.lossy_ratio_backpatch =
		    prepare_lossy_metadata_placeholder_for_streaming_write_overlay(
		        dataset, state.overlay, file.path(), target_transfer_syntax,
		        state.codec_profile_code);
	} else {
		update_lossy_metadata_for_streaming_write_overlay(dataset, state.overlay,
		    file.path(), target_transfer_syntax, state.codec_profile_code,
		    state.encode_source_layout.destination_total_bytes,
		    state.encoded_payload_bytes);
	}
	if (options.write_file_meta) {
		if (!options.keep_existing_meta) {
			build_rebuilt_file_meta_overlay_or_throw(
			    file, dataset, target_transfer_syntax, state.overlay);
		} else {
			overlay_upsert_transfer_syntax_uid_or_throw(
			    state.overlay, "write_with_transfer_syntax", file.path(),
			    target_transfer_syntax);
		}
	}
	state.overlay.finalize();
	state.write_plan = determine_dataset_write_plan(target_transfer_syntax, dataset);
}

[[nodiscard]] PreparedStreamingTranscodeState
prepare_streaming_transcode_state_or_throw(DicomFile& file, DataSet& dataset,
    uid::WellKnown target_transfer_syntax, WriteEncoderConfigSource encode_mode,
    std::span<const pixel::CodecOptionTextKv> codec_opt_override,
    const pixel::EncoderContext* encoder_ctx, const WriteOptions& options,
    const TransferSyntaxWriteDecision& decision, bool writer_can_overwrite) {
	PreparedStreamingTranscodeState state{};
	// Build state in the same order the write pipeline will consume it.
	state.source_decode_layout =
	    pixel::support_detail::compute_decode_source_layout(file);
	if (state.source_decode_layout.empty()) {
		diag::error_and_throw(
		    "write_with_transfer_syntax file={} target_ts={} reason=PixelData metadata is not decodable for the requested transfer syntax conversion",
		    file.path(), target_transfer_syntax.value());
	}
	prepare_streaming_source_layout_or_throw(
	    state, file, target_transfer_syntax, decision);
	prepare_streaming_encoder_context_or_throw(state, file, target_transfer_syntax,
	    encode_mode, codec_opt_override, encoder_ctx, decision);
	prepare_streaming_encode_policy_or_throw(
	    state, file, target_transfer_syntax, decision, writer_can_overwrite);
	measure_streaming_lossy_payload_prepass_if_needed_or_throw(
	    state, file, target_transfer_syntax, decision);
	prepare_streaming_overlay_or_throw(
	    state, file, dataset, target_transfer_syntax, options);
	return state;
}

template <typename Writer>
void write_encapsulated_transcoded_pixel_data_or_throw(DicomFile& file, Writer& writer,
    uid::WellKnown target_transfer_syntax,
    const TransferSyntaxWriteDecision& decision,
    PreparedStreamingTranscodeState& state) {
	write_dataset_body_with_overlay_and_pixel_writer(writer, file.dataset(), state.overlay,
	    state.write_plan, file.path(),
	    [&](const DataElement& element, auto& direct_writer, bool explicit_vr) {
		    std::size_t extended_offset_table_value_offset = 0;
		    std::size_t extended_offset_table_lengths_value_offset = 0;
		    std::vector<std::uint64_t> extended_offsets{};
		    std::vector<std::uint64_t> extended_lengths{};
		    const bool write_extended_offset_table =
		        direct_writer.can_overwrite() &&
		        state.encode_source_layout.frames != 0;
		    // Reserve EOT space up front so offsets can be backpatched after frame emission.
		    if (write_extended_offset_table) {
			    if (state.encode_source_layout.frames >
			        std::numeric_limits<std::size_t>::max() /
			            sizeof(std::uint64_t)) {
				    diag::error_and_throw(
				        "write_with_transfer_syntax file={} target_ts={} reason=ExtendedOffsetTable size overflow",
				        file.path(), target_transfer_syntax.value());
			    }
			    const auto extended_value_length =
			        state.encode_source_layout.frames * sizeof(std::uint64_t);
			    write_element_header(direct_writer, "ExtendedOffsetTable"_tag,
			        explicit_vr ? VR::OV : VR::None,
			        checked_u32(extended_value_length, "ExtendedOffsetTable length"),
			        false, explicit_vr);
			    extended_offset_table_value_offset = direct_writer.position();
			    append_zero_filled_bytes(direct_writer, extended_value_length);

			    write_element_header(direct_writer, "ExtendedOffsetTableLengths"_tag,
			        explicit_vr ? VR::OV : VR::None,
			        checked_u32(extended_value_length,
			            "ExtendedOffsetTableLengths length"),
			        false, explicit_vr);
			    extended_offset_table_lengths_value_offset =
			        direct_writer.position();
			    append_zero_filled_bytes(direct_writer, extended_value_length);

			    extended_offsets.reserve(state.encode_source_layout.frames);
			    extended_lengths.reserve(state.encode_source_layout.frames);
		    }

		    // PixelData itself is streamed as an encapsulated undefined-length sequence.
		    const VR pixel_vr = explicit_vr ? VR::OB : VR::None;
		    write_element_header(direct_writer, element.tag(), pixel_vr, 0xFFFFFFFFu,
		        true, explicit_vr);
		    write_item_header(direct_writer, kItemTag, 0u);
		    std::uint64_t next_frame_offset = 0;

		    const auto commit_encoded_frame =
		        [&](std::vector<std::uint8_t>&& encoded_frame) {
			        if (state.lossy_ratio_backpatch) {
				        if (state.encoded_payload_bytes >
				            std::numeric_limits<std::size_t>::max() -
				                encoded_frame.size()) {
					        diag::error_and_throw(
					            "write_with_transfer_syntax file={} target_ts={} reason=encoded payload size overflow during streamed write",
					            file.path(), target_transfer_syntax.value());
				        }
				        state.encoded_payload_bytes += encoded_frame.size();
			        }
			        if (write_extended_offset_table) {
				        extended_offsets.push_back(next_frame_offset);
				        extended_lengths.push_back(encoded_frame.size());
			        }
			        write_pixel_fragment(direct_writer,
			            std::span<const std::uint8_t>(
			                encoded_frame.data(), encoded_frame.size()));
			        next_frame_offset += 8u + static_cast<std::uint64_t>(
			            padded_length(encoded_frame.size()));
		        };

		    // Source frames come either directly from native PixelData or from decode-on-demand.
		    if (decision.needs_native_to_encapsulated) {
			    pixel::detail::encode_frames_from_frame_provider_with_runtime_or_throw(
			        file, target_transfer_syntax, state.source_layout,
			        state.codec_profile_code, state.active_encoder_ctx->codec_options(),
			        state.use_multicomponent_transform, state.encode_source_layout.frames,
			        [&](std::size_t frame_index) -> std::span<const std::uint8_t> {
				        const auto frame_offset =
				            frame_index *
				            state.encode_source_layout.source_frame_stride;
				        return state.source_bytes.subspan(
				            frame_offset,
				            state.encode_source_layout.source_frame_size_bytes);
			        },
			        [&](std::size_t, std::vector<std::uint8_t>&& encoded_frame) {
				        commit_encoded_frame(std::move(encoded_frame));
			        });
		    } else {
			    if (state.decoded_frame.size() !=
			        state.decode_plan->output_layout.frame_stride) {
				    state.decoded_frame.resize(
				        state.decode_plan->output_layout.frame_stride);
			    }
			    pixel::detail::encode_frames_from_frame_provider_with_runtime_or_throw(
			        file, target_transfer_syntax, state.source_layout,
			        state.codec_profile_code, state.active_encoder_ctx->codec_options(),
			        state.use_multicomponent_transform, state.encode_source_layout.frames,
			        [&](std::size_t frame_index) -> std::span<const std::uint8_t> {
				        auto frame_span = std::span<std::uint8_t>(
				            state.decoded_frame.data(), state.decoded_frame.size());
				        const auto prepared_source =
				            pixel::support_detail::prepare_decode_frame_source_without_cache_or_throw(
				                file, state.source_decode_layout, frame_index);
				        pixel::detail::dispatch_decode_prepared_frame(file.path(),
				            file.transfer_syntax_uid(), state.source_decode_layout,
				            frame_index, prepared_source.bytes, frame_span,
				            *state.decode_plan);
				        return std::span<const std::uint8_t>(
				            frame_span.data(),
				            state.encode_source_layout.source_frame_size_bytes);
			        },
			        [&](std::size_t, std::vector<std::uint8_t>&& encoded_frame) {
				        commit_encoded_frame(std::move(encoded_frame));
			        });
		    }

		    write_item_header(direct_writer, kSequenceDelimitationTag, 0u);
		    // Backpatch EOT after the final fragment offsets and lengths are known.
		    if (write_extended_offset_table) {
			    const auto extended_offsets_bytes = u64_values_as_bytes(extended_offsets);
			    const auto extended_lengths_bytes = u64_values_as_bytes(extended_lengths);
			    direct_writer.overwrite(extended_offset_table_value_offset,
			        extended_offsets_bytes);
			    direct_writer.overwrite(extended_offset_table_lengths_value_offset,
			        extended_lengths_bytes);
		    }
	    });

	if (state.lossy_ratio_backpatch) {
		// Seekable outputs patch the final lossy ratio once the full payload size is known.
		backpatch_lossy_ratio_or_throw(writer, file.path(), target_transfer_syntax,
		    state.encode_source_layout.destination_total_bytes,
		    state.encoded_payload_bytes,
		    *state.lossy_ratio_backpatch);
	}
}

template <typename Writer>
void write_native_transcoded_pixel_data_or_throw(DicomFile& file, Writer& writer,
    PreparedStreamingTranscodeState& state) {
	if (state.decoded_frame.size() != state.decode_plan->output_layout.frame_stride) {
		state.decoded_frame.resize(state.decode_plan->output_layout.frame_stride);
	}
	const auto native_pixel_vr =
	    native_pixel_vr_from_bits_allocated_for_write(
	        state.encode_source_layout.bits_allocated);
	write_dataset_body_with_overlay_and_pixel_writer(writer, file.dataset(), state.overlay,
	    state.write_plan, file.path(),
	    [&](const DataElement& element, auto& direct_writer, bool explicit_vr) {
		    write_native_pixel_data_from_frame_provider(direct_writer, element, explicit_vr,
		        native_pixel_vr, state.encode_source_layout.destination_total_bytes,
		        state.encode_source_layout.frames,
		        state.encode_source_layout.destination_frame_payload,
		        [&](std::size_t frame_index) -> std::span<const std::uint8_t> {
			        auto frame_span = std::span<std::uint8_t>(
			            state.decoded_frame.data(), state.decoded_frame.size());
			        const auto prepared_source =
			            pixel::support_detail::prepare_decode_frame_source_without_cache_or_throw(
			                file, state.source_decode_layout, frame_index);
			        pixel::detail::dispatch_decode_prepared_frame(file.path(),
			            file.transfer_syntax_uid(), state.source_decode_layout, frame_index,
			            prepared_source.bytes, frame_span, *state.decode_plan);
			        return std::span<const std::uint8_t>(
			            frame_span.data(),
			            state.encode_source_layout.destination_frame_payload);
		        });
	    });
}

template <typename Writer>
void write_with_transfer_syntax_impl(DicomFile& file, Writer& writer,
    uid::WellKnown target_transfer_syntax, WriteEncoderConfigSource encode_mode,
    std::span<const pixel::CodecOptionTextKv> codec_opt_override,
    const pixel::EncoderContext* encoder_ctx, const WriteOptions& options) {
	if (!target_transfer_syntax.valid() ||
	    target_transfer_syntax.uid_type() != UidType::TransferSyntax) {
		diag::error_and_throw(
		    "write_with_transfer_syntax reason=uid must be a valid Transfer Syntax UID");
	}

	DataSet& dataset = file.dataset();
	dataset.ensure_loaded(Tag(0xFFFFu, 0xFFFFu));
	// First decide whether this is a plain write, a metadata overlay, or a pixel transcode.
	const auto decision =
	    classify_transfer_syntax_write(file, dataset, target_transfer_syntax);
	if (!decision.needs_pixel_transcode && decision.same_transfer_syntax &&
	    options.keep_existing_meta) {
		write_current_dataset_as_is(file, writer, target_transfer_syntax, options);
		return;
	}
	if (decision.has_float_pixel_data && decision.target_is_encapsulated) {
		diag::error_and_throw(
		    "write_with_transfer_syntax file={} target_ts={} reason=FloatPixelData/DoubleFloatPixelData cannot be written with encapsulated transfer syntaxes",
		    file.path(), target_transfer_syntax.value());
	}
	if ((decision.needs_native_to_encapsulated ||
	        decision.needs_encapsulated_transcode) &&
	    !target_transfer_syntax.supports_pixel_encode()) {
		diag::error_and_throw(
		    "write_with_transfer_syntax file={} source_ts={} target_ts={} reason=target transfer syntax does not support pixel encode",
		    file.path(), decision.source_transfer_syntax.value(),
		    target_transfer_syntax.value());
	}

	if (!decision.needs_pixel_transcode) {
		// Metadata-only writes still use an overlay so the source DataSet stays untouched.
		TransientWriteOverlay overlay{};
		if (options.write_file_meta) {
			if (!options.keep_existing_meta) {
				build_rebuilt_file_meta_overlay_or_throw(
				    file, dataset, target_transfer_syntax, overlay);
			} else {
				overlay_upsert_transfer_syntax_uid_or_throw(overlay,
				    "write_with_transfer_syntax", file.path(),
				    target_transfer_syntax);
			}
		}
		overlay.finalize();
		write_current_dataset_with_overlay(
		    file, overlay, writer, target_transfer_syntax, options);
		return;
	}

	// For streaming transcodes, prepare shared state once and then hand off to the emit path.
	auto state = prepare_streaming_transcode_state_or_throw(file, dataset,
	    target_transfer_syntax, encode_mode, codec_opt_override, encoder_ctx, options,
	    decision, writer.can_overwrite());
	// State is moved out of prepare(), so restore the pointer to whichever encoder context survived.
	state.active_encoder_ctx = state.staged_encoder_ctx.configured()
	    ? &state.staged_encoder_ctx
	    : encoder_ctx;
	if (options.include_preamble) {
		write_preamble(writer);
	}
	if (options.write_file_meta) {
		write_file_meta_group_with_overlay(writer, dataset, state.overlay);
	}
	if (state.lossy_ratio_backpatch) {
		state.lossy_ratio_backpatch->absolute_token_offset =
		    writer.position() +
		    measure_dataset_value_offset_or_throw_with_overlay(
		        dataset, state.overlay, "LossyImageCompressionRatio"_tag,
		        state.write_plan.explicit_vr) +
		    state.lossy_ratio_backpatch->token_offset_in_value;
	}

	if (decision.target_is_encapsulated) {
		write_encapsulated_transcoded_pixel_data_or_throw(
		    file, writer, target_transfer_syntax, decision, state);
	} else {
		write_native_transcoded_pixel_data_or_throw(file, writer, state);
	}
}

}  // namespace

void write_with_transfer_syntax_to_stream_writer(DicomFile& file, StreamWriter& writer,
    uid::WellKnown target_transfer_syntax, WriteEncoderConfigSource encode_mode,
    std::span<const pixel::CodecOptionTextKv> codec_opt_override,
    const pixel::EncoderContext* encoder_ctx, const WriteOptions& options) {
	write_with_transfer_syntax_impl(file, writer, target_transfer_syntax, encode_mode,
	    codec_opt_override, encoder_ctx, options);
}

void write_with_transfer_syntax_to_buffer_writer(DicomFile& file, BufferWriter& writer,
    uid::WellKnown target_transfer_syntax, WriteEncoderConfigSource encode_mode,
    std::span<const pixel::CodecOptionTextKv> codec_opt_override,
    const pixel::EncoderContext* encoder_ctx, const WriteOptions& options) {
	write_with_transfer_syntax_impl(file, writer, target_transfer_syntax, encode_mode,
	    codec_opt_override, encoder_ctx, options);
}

}  // namespace dicom::write_detail

