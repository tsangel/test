#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace dicom::detail {

inline std::string filesystem_path_to_utf8(const std::filesystem::path& path) {
#if defined(_WIN32)
	const auto utf8 = path.u8string();
	return std::string(reinterpret_cast<const char*>(utf8.c_str()), utf8.size());
#else
	return path.string();
#endif
}

inline std::string normalize_stream_identifier_path(std::string_view raw_path) {
	if (raw_path.empty()) {
		return {};
	}

	const auto normalized_path = std::filesystem::path(raw_path).lexically_normal();
	const auto normalized_text = normalized_path.string();
	return normalized_text.empty() ? std::string(raw_path) : normalized_text;
}

inline std::string normalize_stream_identifier_path(const std::filesystem::path& raw_path) {
	if (raw_path.empty()) {
		return {};
	}

	const auto normalized_path = raw_path.lexically_normal();
	const auto normalized_text = filesystem_path_to_utf8(normalized_path);
	return normalized_text.empty() ? filesystem_path_to_utf8(raw_path) : normalized_text;
}

}  // namespace dicom::detail
