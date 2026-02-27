#include "pixel/decode/core/decode_frame_source_resolver.hpp"

#include "pixel/decode/core/decode_codec_impl_detail.hpp"

#include <exception>
#include <new>

namespace dicom::pixel::detail {

ResolvedDecodeFrameSource resolve_decode_frame_source_or_throw(
    const DicomFile& df, const TransferSyntaxPluginBinding& binding,
    std::size_t frame_index) {
	const auto& info = df.pixeldata_info();
	const auto& ds = df.dataset();

	ResolvedDecodeFrameSource resolved{};
	CodecError decode_error{};
	try {
		switch (binding.profile) {
		case CodecProfile::native_uncompressed: {
			const auto native_source = load_native_frame_source(
			    ds, df.path(), info, frame_index);
			resolved.bytes = native_source.contiguous;
			break;
		}
		case CodecProfile::encapsulated_uncompressed:
		case CodecProfile::rle_lossless:
		case CodecProfile::jpeg_lossless:
		case CodecProfile::jpeg_lossy:
		case CodecProfile::jpegls_lossless:
		case CodecProfile::jpegls_near_lossless:
		case CodecProfile::jpeg2000_lossless:
		case CodecProfile::jpeg2000_lossy:
		case CodecProfile::htj2k_lossless:
		case CodecProfile::htj2k_lossless_rpcl:
		case CodecProfile::htj2k_lossy:
		case CodecProfile::jpegxl_lossless:
		case CodecProfile::jpegxl_lossy:
		case CodecProfile::jpegxl_jpeg_recompression: {
			const auto source = load_encapsulated_frame_source(
			    ds, df.path(), frame_index, binding.plugin_key);
			resolved.bytes = materialize_encapsulated_frame_source(
			    df.path(), frame_index, binding.plugin_key, source,
			    resolved.owned_bytes);
			break;
		}
		case CodecProfile::unknown:
		default:
			break;
		}
	} catch (const std::bad_alloc&) {
		decode_error.code = CodecStatusCode::internal_error;
		decode_error.stage = "allocate";
		decode_error.detail = "memory allocation failed";
		throw_codec_error_with_context("pixel::decode_frame_into",
		    df.path(), info.ts, binding.plugin_key, frame_index, decode_error);
	} catch (const std::exception& e) {
		decode_error.code = CodecStatusCode::invalid_argument;
		decode_error.stage = "load_frame_source";
		decode_error.detail = e.what();
		throw_codec_error_with_context("pixel::decode_frame_into",
		    df.path(), info.ts, binding.plugin_key, frame_index, decode_error);
	} catch (...) {
		decode_error.code = CodecStatusCode::backend_error;
		decode_error.stage = "load_frame_source";
		decode_error.detail = "non-standard exception";
		throw_codec_error_with_context("pixel::decode_frame_into",
		    df.path(), info.ts, binding.plugin_key, frame_index, decode_error);
	}

	return resolved;
}

} // namespace dicom::pixel::detail
