#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace pixel::runtime::detail {

inline std::string filesystem_path_to_utf8(const std::filesystem::path& path) {
#if defined(_WIN32)
  const auto utf8 = path.u8string();
  return std::string(reinterpret_cast<const char*>(utf8.c_str()), utf8.size());
#else
  return path.string();
#endif
}

inline std::filesystem::path filesystem_path_from_utf8(std::string_view raw_path) {
#if defined(_WIN32)
  return std::filesystem::u8path(raw_path.begin(), raw_path.end());
#else
  return std::filesystem::path(std::string(raw_path));
#endif
}

inline bool resolve_shared_library_path(const std::filesystem::path& raw_library_path,
    std::filesystem::path* out_library_path, std::string* out_error) {
  auto set_error = [out_error](std::string message) {
    if (out_error != nullptr) {
      *out_error = std::move(message);
    }
  };

  if (raw_library_path.empty()) {
    set_error("library path is empty");
    return false;
  }

  namespace fs = std::filesystem;

  fs::path input_path = raw_library_path.lexically_normal();

  std::error_code ec;
  fs::path absolute_path = input_path;
  if (!absolute_path.is_absolute()) {
    absolute_path = fs::absolute(input_path, ec);
    if (ec) {
      set_error("failed to resolve absolute shared library path: " +
          filesystem_path_to_utf8(input_path));
      return false;
    }
  }

  fs::path resolved_path = fs::weakly_canonical(absolute_path, ec);
  if (ec) {
    resolved_path = absolute_path.lexically_normal();
    ec.clear();
  }

  const bool path_exists = fs::exists(resolved_path, ec);
  if (ec) {
    set_error("failed to stat shared library path: " +
        filesystem_path_to_utf8(resolved_path));
    return false;
  }
  if (!path_exists) {
    set_error("shared library does not exist: " +
        filesystem_path_to_utf8(resolved_path));
    return false;
  }

  if (out_library_path != nullptr) {
    *out_library_path = resolved_path;
  }
  return true;
}

inline bool resolve_shared_library_path(std::string_view raw_library_path,
    std::filesystem::path* out_library_path, std::string* out_error) {
  if (raw_library_path.empty()) {
    if (out_error != nullptr) {
      *out_error = "library path is empty";
    }
    return false;
  }
  return resolve_shared_library_path(
      filesystem_path_from_utf8(raw_library_path), out_library_path, out_error);
}

}  // namespace pixel::runtime::detail

