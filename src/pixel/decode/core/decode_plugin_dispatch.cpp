#include "pixel/decode/core/decode_plugin_dispatch.hpp"

#include "pixel/bridge/codec_plugin_abi_adapter.hpp"

#include <cctype>
#include <string>
#include <string_view>
#include <utility>

namespace dicom::pixel::detail {

namespace {

std::string decorate_decode_detail_with_callsite_context(
    std::string detail, std::string_view file_path, std::size_t frame_index) {
	// Some decoder helpers return fully formatted strings like:
	// "pixel::decode_frame_into file=... frame=... reason=..."
	// Strip only that prefix form; keep codec-native detail intact.
	if (detail.rfind("pixel::decode_frame_into ", 0) == 0) {
		const auto reason_pos = detail.find("reason=");
		if (reason_pos != std::string::npos) {
			detail = detail.substr(reason_pos + 7);
		}
	}
	while (!detail.empty() &&
	       std::isspace(static_cast<unsigned char>(detail.front())) != 0) {
		detail.erase(detail.begin());
	}
	if (detail.empty()) {
		detail = "decoder plugin failed";
	}
	return "file=" + std::string(file_path) + " frame=" +
	    std::to_string(frame_index) + " " + detail;
}

} // namespace

void invoke_decode_plugin_or_throw(const DicomFile& df,
    const TransferSyntaxPluginBinding& binding, const CodecPlugin& plugin,
    const CodecDecodeFrameInput& decode_input, std::size_t frame_index) {
	const auto& info = df.pixeldata_info();

	CodecError decode_error{};
	dicomsdl_decoder_request_v1 abi_request{};
	abi::build_decoder_request_v1(decode_input, abi_request);
	if (abi_request.frame.transfer_syntax_code ==
	    DICOMSDL_TRANSFER_SYNTAX_CODE_INVALID) {
		decode_error.code = CodecStatusCode::invalid_argument;
		decode_error.stage = "validate";
		decode_error.detail = "invalid transfer syntax code for decoder ABI request";
		throw_codec_error_with_context("pixel::decode_frame_into",
		    df.path(), info.ts, binding.plugin_key, frame_index, decode_error);
	}
	if (abi_request.frame.source_dtype == DICOMSDL_DTYPE_UNKNOWN &&
	    info.sv_dtype != DataType::unknown) {
		decode_error.code = CodecStatusCode::invalid_argument;
		decode_error.stage = "validate";
		decode_error.detail = "source dtype is not representable in decoder ABI request";
		throw_codec_error_with_context("pixel::decode_frame_into",
		    df.path(), info.ts, binding.plugin_key, frame_index, decode_error);
	}

	const bool decode_ok = plugin.decode_frame(decode_input, decode_error);
	if (!decode_ok) {
		if (decode_error.code == CodecStatusCode::ok) {
			decode_error.code = CodecStatusCode::backend_error;
		}
		if (decode_error.stage.empty()) {
			decode_error.stage = "decode_frame";
		}
		if (decode_error.detail.empty()) {
			decode_error.detail = "decoder plugin failed";
		}
		decode_error.detail = decorate_decode_detail_with_callsite_context(
		    std::move(decode_error.detail), df.path(), frame_index);
		throw_codec_error_with_context("pixel::decode_frame_into",
		    df.path(), info.ts, binding.plugin_key, frame_index, decode_error);
	}
}

} // namespace dicom::pixel::detail
