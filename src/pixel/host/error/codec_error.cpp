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
	case CodecStatusCode::cancelled:
		return "cancelled";
	case CodecStatusCode::internal_error:
		return "internal_error";
	}
	return "unknown";
}

std::string format_codec_error_suffix(std::optional<std::size_t> frame_index,
    const CodecError& error) {
	const auto status = codec_status_code_name(error.code);
	const std::string_view stage =
	    error.stage.empty() ? std::string_view("unknown") : std::string_view(error.stage);
	const std::string_view detail =
	    error.detail.empty() ? std::string_view("unspecified codec error")
	                         : std::string_view(error.detail);

	if (frame_index.has_value()) {
		return fmt::format("frame={} status={} stage={} reason={}",
		    *frame_index, status, stage, detail);
	}
	return fmt::format("status={} stage={} reason={}",
	    status, stage, detail);
}

bool codec_exception_needs_boundary_prefix(std::string_view message) noexcept {
	return message.starts_with("status=") || message.starts_with("frame=") ||
	    message.starts_with("stage=");
}

[[nodiscard]] bool codec_exception_has_boundary_prefix(
    std::string_view operation, std::string_view message) noexcept {
	return message.starts_with(operation);
}

std::string_view boundary_transfer_syntax_text(
    uid::WellKnown transfer_syntax) noexcept {
	return transfer_syntax.valid() ? transfer_syntax.value() : std::string_view("<invalid>");
}

[[nodiscard]] std::string format_codec_boundary_message(
    std::string_view operation, std::string_view file_path,
    uid::WellKnown transfer_syntax, std::string_view message) {
	return fmt::format("{} file={} ts={} {}",
	    operation, file_path,
	    boundary_transfer_syntax_text(transfer_syntax), message);
}

[[noreturn]] void throw_codec_exception(std::optional<std::size_t> frame_index,
    const CodecError& error) {
	throw diag::DicomException(format_codec_error_suffix(frame_index, error));
}

[[noreturn]] void rethrow_codec_exception_at_boundary(
    std::string_view operation, std::string_view file_path,
    uid::WellKnown transfer_syntax, const std::exception& ex) {
	const std::string_view message = ex.what();
	if (codec_exception_needs_boundary_prefix(message)) {
		diag::throw_exception("{}",
		    format_codec_boundary_message(
		        operation, file_path, transfer_syntax, message));
	}
	throw;
}

[[noreturn]] void rethrow_codec_exception_at_boundary_or_throw(
    std::string_view operation, std::string_view file_path,
    uid::WellKnown transfer_syntax, const std::exception& ex) {
	const std::string_view message = ex.what();
	if (codec_exception_needs_boundary_prefix(message)) {
		diag::error_and_throw("{}",
		    format_codec_boundary_message(
		        operation, file_path, transfer_syntax, message));
	}
	if (codec_exception_has_boundary_prefix(operation, message)) {
		diag::error_and_throw("{}", message);
	}
	throw;
}

} // namespace dicom::pixel::detail
