#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace dicom::detail {

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
	const auto normalized_text = normalized_path.string();
	return normalized_text.empty() ? raw_path.string() : normalized_text;
}

}  // namespace dicom::detail
