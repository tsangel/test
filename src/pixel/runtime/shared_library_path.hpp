#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace pixel::runtime::detail {

inline bool resolve_shared_library_path(std::string_view raw_library_path,
    std::string* out_library_path, std::string* out_error) {
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

  fs::path input_path{std::string(raw_library_path)};
  input_path = input_path.lexically_normal();

  std::error_code ec;
  fs::path absolute_path = input_path;
  if (!absolute_path.is_absolute()) {
    absolute_path = fs::absolute(input_path, ec);
    if (ec) {
      set_error("failed to resolve absolute shared library path: " +
          input_path.string());
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
    set_error("failed to stat shared library path: " + resolved_path.string());
    return false;
  }
  if (!path_exists) {
    set_error("shared library does not exist: " + resolved_path.string());
    return false;
  }

  if (out_library_path != nullptr) {
    *out_library_path = resolved_path.string();
  }
  return true;
}

}  // namespace pixel::runtime::detail

