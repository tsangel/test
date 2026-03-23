#pragma once

#include "dicom.h"

#include <fmt/format.h>

#include <exception>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace dicom::pixel::detail {

enum class CodecStatusCode : std::uint8_t {
	ok = 0,
	invalid_argument,
	unsupported,
	backend_error,
	cancelled,
	internal_error,
};

struct CodecError {
	CodecStatusCode code{CodecStatusCode::ok};
	std::string stage{};
	std::string detail{};
};

inline void set_codec_error(CodecError& out_error, CodecStatusCode code,
    std::string_view stage, std::string_view detail) {
	out_error.code = code;
	out_error.stage = std::string(stage);
	out_error.detail = std::string(detail);
}

[[nodiscard]] std::string_view codec_status_code_name(
    CodecStatusCode code) noexcept;

[[nodiscard]] std::string format_codec_error_suffix(
    std::optional<std::size_t> frame_index, const CodecError& error);

[[nodiscard]] bool codec_exception_needs_boundary_prefix(
    std::string_view message) noexcept;

[[nodiscard]] std::string_view boundary_transfer_syntax_text(
    uid::WellKnown transfer_syntax) noexcept;

[[noreturn]] void throw_codec_exception(std::optional<std::size_t> frame_index,
    const CodecError& error);

[[noreturn]] void rethrow_codec_exception_at_boundary_or_throw(
    std::string_view operation, std::string_view file_path,
    uid::WellKnown transfer_syntax, const std::exception& ex);

[[noreturn]] void rethrow_codec_exception_at_boundary(
    std::string_view operation, std::string_view file_path,
    uid::WellKnown transfer_syntax, const std::exception& ex);

[[noreturn]] inline void rethrow_codec_exception_at_boundary(
    std::string_view operation, const DicomFile& file,
    uid::WellKnown transfer_syntax, const std::exception& ex) {
	rethrow_codec_exception_at_boundary(
	    operation, file.path(), transfer_syntax, ex);
}

[[noreturn]] inline void rethrow_codec_exception_at_boundary(
    std::string_view operation, const DicomFile& file,
    const std::exception& ex) {
	rethrow_codec_exception_at_boundary(
	    operation, file.path(), file.transfer_syntax_uid(), ex);
}

[[noreturn]] inline void rethrow_codec_exception_at_boundary_or_throw(
    std::string_view operation, const DicomFile& file,
    uid::WellKnown transfer_syntax, const std::exception& ex) {
	rethrow_codec_exception_at_boundary_or_throw(
	    operation, file.path(), transfer_syntax, ex);
}

[[noreturn]] inline void rethrow_codec_exception_at_boundary_or_throw(
    std::string_view operation, const DicomFile& file,
    const std::exception& ex) {
	rethrow_codec_exception_at_boundary_or_throw(
	    operation, file.path(), file.transfer_syntax_uid(), ex);
}

template <typename... Args>
[[noreturn]] inline void throw_codec_stage_exception(CodecStatusCode code,
    std::string_view stage, fmt::format_string<Args...> reason_format,
    Args&&... args) {
	throw_codec_exception(std::nullopt,
	    CodecError{
	        .code = code,
	        .stage = std::string(stage),
	        .detail = fmt::format(reason_format, std::forward<Args>(args)...),
	    });
}

template <typename... Args>
[[noreturn]] inline void throw_frame_codec_stage_exception(
    std::size_t frame_index, CodecStatusCode code, std::string_view stage,
    fmt::format_string<Args...> reason_format, Args&&... args) {
	throw_codec_exception(frame_index,
	    CodecError{
	        .code = code,
	        .stage = std::string(stage),
	        .detail = fmt::format(reason_format, std::forward<Args>(args)...),
	    });
}

} // namespace dicom::pixel::detail
