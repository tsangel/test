#include "pixel/decode/core/decode_frame_dispatch.hpp"

#include "pixel/decode/core/decode_frame_source_resolver.hpp"
#include "pixel/decode/core/decode_plugin_dispatch.hpp"

namespace dicom::pixel::detail {

void dispatch_decode_frame_with_resolved_transform(const DicomFile& df,
    const DecodeValueTransform& value_transform, std::size_t frame_index,
    std::span<std::uint8_t> dst, const DecodeStrides& dst_strides,
    const DecodeOptions& effective_opt) {
	const auto& info = df.pixeldata_info();

	const auto& codec_registry = global_codec_registry();
	// Keep dispatch stable for the whole decode call while plugin dispatchers may be swapped.
	[[maybe_unused]] const auto dispatch_lock = codec_registry.acquire_dispatch_read_lock();
	const auto* binding = codec_registry.find_binding(info.ts);
	CodecError decode_error{};
	if (!binding || !binding->decode_supported) {
		decode_error.code = CodecStatusCode::unsupported;
		decode_error.stage = "plugin_lookup";
		decode_error.detail =
		    "transfer syntax is not supported for decode by codec registry binding";
		throw_codec_error_with_context("pixel::decode_frame_into",
		    df.path(), info.ts, "<none>", frame_index, decode_error);
	}
	const auto* plugin = codec_registry.resolve_decoder_plugin(*binding);
	if (!plugin) {
		decode_error.code = CodecStatusCode::internal_error;
		decode_error.stage = "plugin_lookup";
		decode_error.detail = "registry binding references a missing plugin";
		throw_codec_error_with_context("pixel::decode_frame_into",
		    df.path(), info.ts, binding->plugin_key, frame_index, decode_error);
	}
	if (!plugin->decode_frame) {
		decode_error.code = CodecStatusCode::internal_error;
		decode_error.stage = "plugin_lookup";
		decode_error.detail = "registered decode plugin has no dispatcher";
		throw_codec_error_with_context("pixel::decode_frame_into",
		    df.path(), info.ts, binding->plugin_key, frame_index, decode_error);
	}

	const auto resolved_source =
	    resolve_decode_frame_source_or_throw(df, *binding, frame_index);
	const CodecDecodeFrameInput decode_input{
	    .info = info,
	    .value_transform = value_transform,
	    .prepared_source = resolved_source.bytes,
	    .destination = dst,
	    .destination_strides = dst_strides,
	    .options = effective_opt,
	};

	invoke_decode_plugin_or_throw(
	    df, *binding, *plugin, decode_input, frame_index);
}

} // namespace dicom::pixel::detail
