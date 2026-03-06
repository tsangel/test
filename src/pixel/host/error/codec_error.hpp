#pragma once

#include "dicom.h"

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

[[nodiscard]] std::string format_codec_error_context(
    std::string_view function_name, std::string_view file_path,
    uid::WellKnown transfer_syntax, std::optional<std::size_t> frame_index,
    const CodecError& error);

[[noreturn]] void throw_codec_error_with_context(
    std::string_view function_name, std::string_view file_path,
    uid::WellKnown transfer_syntax, std::optional<std::size_t> frame_index,
    const CodecError& error);

} // namespace dicom::pixel::detail
