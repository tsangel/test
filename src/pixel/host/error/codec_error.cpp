#include "pixel/host/error/codec_error.hpp"

#include "diagnostics.h"

#include <fmt/format.h>

namespace dicom::pixel::detail {
namespace diag = dicom::diag;

std::string_view codec_status_code_name(CodecStatusCode code) noexcept {
	switch (code) {
	case CodecStatusCode::ok:
		return "ok";
	case CodecStatusCode::invalid_argument:
		return "invalid_argument";
	case CodecStatusCode::unsupported:
		return "unsupported";
	case CodecStatusCode::backend_error:
		return "backend_error";
	case CodecStatusCode::internal_error:
		return "internal_error";
	}
	return "unknown";
}

std::string format_codec_error_context(std::string_view function_name,
    std::string_view file_path, uid::WellKnown transfer_syntax,
    std::optional<std::size_t> frame_index, const CodecError& error) {
	const auto status = codec_status_code_name(error.code);
	const std::string_view stage =
	    error.stage.empty() ? std::string_view("unknown") : std::string_view(error.stage);
	const std::string_view detail =
	    error.detail.empty() ? std::string_view("unspecified codec error")
	                         : std::string_view(error.detail);

	if (frame_index.has_value()) {
		return fmt::format(
		    "{} file={} ts={} frame={} status={} stage={} reason={}",
		    function_name, file_path, transfer_syntax.value(),
		    *frame_index, status, stage, detail);
	}
	return fmt::format(
	    "{} file={} ts={} status={} stage={} reason={}",
	    function_name, file_path, transfer_syntax.value(), status,
	    stage, detail);
}

[[noreturn]] void throw_codec_error_with_context(
    std::string_view function_name, std::string_view file_path,
    uid::WellKnown transfer_syntax, std::optional<std::size_t> frame_index,
    const CodecError& error) {
	diag::error_and_throw("{}", format_codec_error_context(
	    function_name, file_path, transfer_syntax, frame_index, error));
}

} // namespace dicom::pixel::detail
