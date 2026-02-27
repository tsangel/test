#include "pixel/plugin_abi/external/plugin_loader.hpp"

#include <utility>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace dicom::pixel::detail::abi {

namespace {

using get_decoder_api_fn = int (*)(dicomsdl_decoder_plugin_api_v1* out_api);
using get_encoder_api_fn = int (*)(dicomsdl_encoder_plugin_api_v1* out_api);

void reset_decoder_plugin(ExternalDecoderPlugin& plugin) noexcept {
  plugin = {};
}

void reset_encoder_plugin(ExternalEncoderPlugin& plugin) noexcept {
  plugin = {};
}

bool open_shared_library(std::string_view library_path,
    ExternalSharedLibrary& out_library, std::string& out_error) {
  out_library = {};
  if (library_path.empty()) {
    out_error = "library path is empty";
    return false;
  }

#if defined(_WIN32)
  HMODULE handle = LoadLibraryA(std::string(library_path).c_str());
  if (handle == nullptr) {
    out_error = "LoadLibraryA failed";
    return false;
  }
  out_library.native_handle = reinterpret_cast<void*>(handle);
#else
  void* handle = dlopen(std::string(library_path).c_str(), RTLD_NOW | RTLD_LOCAL);
  if (handle == nullptr) {
    const char* detail = dlerror();
    out_error = detail != nullptr ? std::string(detail) : "dlopen failed";
    return false;
  }
  out_library.native_handle = handle;
#endif
  out_library.path = std::string(library_path);
  return true;
}

void close_shared_library(ExternalSharedLibrary& library) noexcept {
  if (library.native_handle == nullptr) {
    library.path.clear();
    return;
  }

#if defined(_WIN32)
  auto* handle = reinterpret_cast<HMODULE>(library.native_handle);
  FreeLibrary(handle);
#else
  dlclose(library.native_handle);
#endif

  library.native_handle = nullptr;
  library.path.clear();
}

void* find_symbol(const ExternalSharedLibrary& library,
    const char* symbol_name) noexcept {
  if (library.native_handle == nullptr || symbol_name == nullptr) {
    return nullptr;
  }

#if defined(_WIN32)
  auto* handle = reinterpret_cast<HMODULE>(library.native_handle);
  return reinterpret_cast<void*>(GetProcAddress(handle, symbol_name));
#else
  return dlsym(library.native_handle, symbol_name);
#endif
}

bool begin_inflight(std::size_t retain_count, std::size_t& in_flight_calls,
    std::string& out_error) {
  if (retain_count == 0) {
    out_error = "plugin is not loaded";
    return false;
  }
  ++in_flight_calls;
  return true;
}

void end_inflight(std::size_t& in_flight_calls) noexcept {
  if (in_flight_calls > 0) {
    --in_flight_calls;
  }
}

bool validate_decoder_api(const dicomsdl_decoder_plugin_api_v1& api,
    std::string& out_error) {
  if (api.abi_version != DICOMSDL_DECODER_PLUGIN_ABI_V1) {
    out_error = "decoder plugin ABI version mismatch";
    return false;
  }
  if (api.create == nullptr || api.destroy == nullptr ||
      api.configure == nullptr || api.decode_frame == nullptr) {
    out_error = "decoder plugin API is missing required function pointers";
    return false;
  }
  return true;
}

bool validate_encoder_api(const dicomsdl_encoder_plugin_api_v1& api,
    std::string& out_error) {
  if (api.abi_version != DICOMSDL_ENCODER_PLUGIN_ABI_V1) {
    out_error = "encoder plugin ABI version mismatch";
    return false;
  }
  if (api.create == nullptr || api.destroy == nullptr ||
      api.configure == nullptr || api.encode_frame == nullptr) {
    out_error = "encoder plugin API is missing required function pointers";
    return false;
  }
  return true;
}

}  // namespace

bool load_external_decoder_plugin(std::string_view library_path,
    ExternalDecoderPlugin& out_plugin, std::string& out_error) {
  out_error.clear();
  reset_decoder_plugin(out_plugin);

  if (!open_shared_library(library_path, out_plugin.library, out_error)) {
    return false;
  }

  auto* get_api = reinterpret_cast<get_decoder_api_fn>(find_symbol(
      out_plugin.library, "dicomsdl_get_decoder_plugin_api_v1"));
  if (get_api == nullptr) {
    out_error = "decoder plugin symbol not found: dicomsdl_get_decoder_plugin_api_v1";
    close_shared_library(out_plugin.library);
    reset_decoder_plugin(out_plugin);
    return false;
  }

  out_plugin.api = {};
  out_plugin.api.struct_size = sizeof(dicomsdl_decoder_plugin_api_v1);
  out_plugin.api.abi_version = DICOMSDL_DECODER_PLUGIN_ABI_V1;
  if (get_api(&out_plugin.api) == 0) {
    out_error = "decoder plugin API handshake failed";
    close_shared_library(out_plugin.library);
    reset_decoder_plugin(out_plugin);
    return false;
  }
  if (!validate_decoder_api(out_plugin.api, out_error)) {
    close_shared_library(out_plugin.library);
    reset_decoder_plugin(out_plugin);
    return false;
  }

  out_plugin.context = out_plugin.api.create();
  if (out_plugin.context == nullptr) {
    out_error = "decoder plugin create() returned null context";
    close_shared_library(out_plugin.library);
    reset_decoder_plugin(out_plugin);
    return false;
  }

  out_plugin.retain_count = 1;
  out_plugin.in_flight_calls = 0;
  return true;
}

bool load_external_encoder_plugin(std::string_view library_path,
    ExternalEncoderPlugin& out_plugin, std::string& out_error) {
  out_error.clear();
  reset_encoder_plugin(out_plugin);

  if (!open_shared_library(library_path, out_plugin.library, out_error)) {
    return false;
  }

  auto* get_api = reinterpret_cast<get_encoder_api_fn>(find_symbol(
      out_plugin.library, "dicomsdl_get_encoder_plugin_api_v1"));
  if (get_api == nullptr) {
    out_error = "encoder plugin symbol not found: dicomsdl_get_encoder_plugin_api_v1";
    close_shared_library(out_plugin.library);
    reset_encoder_plugin(out_plugin);
    return false;
  }

  out_plugin.api = {};
  out_plugin.api.struct_size = sizeof(dicomsdl_encoder_plugin_api_v1);
  out_plugin.api.abi_version = DICOMSDL_ENCODER_PLUGIN_ABI_V1;
  if (get_api(&out_plugin.api) == 0) {
    out_error = "encoder plugin API handshake failed";
    close_shared_library(out_plugin.library);
    reset_encoder_plugin(out_plugin);
    return false;
  }
  if (!validate_encoder_api(out_plugin.api, out_error)) {
    close_shared_library(out_plugin.library);
    reset_encoder_plugin(out_plugin);
    return false;
  }

  out_plugin.context = out_plugin.api.create();
  if (out_plugin.context == nullptr) {
    out_error = "encoder plugin create() returned null context";
    close_shared_library(out_plugin.library);
    reset_encoder_plugin(out_plugin);
    return false;
  }

  out_plugin.retain_count = 1;
  out_plugin.in_flight_calls = 0;
  return true;
}

bool init_external_decoder_plugin_from_api(
    const dicomsdl_decoder_plugin_api_v1& api,
    ExternalDecoderPlugin& out_plugin, std::string& out_error) {
  out_error.clear();
  reset_decoder_plugin(out_plugin);

  out_plugin.api = api;
  if (!validate_decoder_api(out_plugin.api, out_error)) {
    reset_decoder_plugin(out_plugin);
    return false;
  }
  out_plugin.context = out_plugin.api.create();
  if (out_plugin.context == nullptr) {
    out_error = "decoder plugin create() returned null context";
    reset_decoder_plugin(out_plugin);
    return false;
  }
  out_plugin.retain_count = 1;
  out_plugin.in_flight_calls = 0;
  return true;
}

bool init_external_encoder_plugin_from_api(
    const dicomsdl_encoder_plugin_api_v1& api,
    ExternalEncoderPlugin& out_plugin, std::string& out_error) {
  out_error.clear();
  reset_encoder_plugin(out_plugin);

  out_plugin.api = api;
  if (!validate_encoder_api(out_plugin.api, out_error)) {
    reset_encoder_plugin(out_plugin);
    return false;
  }
  out_plugin.context = out_plugin.api.create();
  if (out_plugin.context == nullptr) {
    out_error = "encoder plugin create() returned null context";
    reset_encoder_plugin(out_plugin);
    return false;
  }
  out_plugin.retain_count = 1;
  out_plugin.in_flight_calls = 0;
  return true;
}

void retain_external_decoder_plugin(ExternalDecoderPlugin& plugin) noexcept {
  if (plugin.retain_count > 0) {
    ++plugin.retain_count;
  }
}

void retain_external_encoder_plugin(ExternalEncoderPlugin& plugin) noexcept {
  if (plugin.retain_count > 0) {
    ++plugin.retain_count;
  }
}

bool release_external_decoder_plugin(ExternalDecoderPlugin& plugin,
    std::string& out_error) {
  out_error.clear();
  if (plugin.retain_count == 0) {
    out_error = "decoder plugin is not loaded";
    return false;
  }

  --plugin.retain_count;
  if (plugin.retain_count > 0) {
    return true;
  }
  if (plugin.in_flight_calls != 0) {
    ++plugin.retain_count;
    out_error = "decoder plugin has in-flight calls";
    return false;
  }

  if (plugin.api.destroy != nullptr && plugin.context != nullptr) {
    plugin.api.destroy(plugin.context);
  }
  close_shared_library(plugin.library);
  reset_decoder_plugin(plugin);
  return true;
}

bool release_external_encoder_plugin(ExternalEncoderPlugin& plugin,
    std::string& out_error) {
  out_error.clear();
  if (plugin.retain_count == 0) {
    out_error = "encoder plugin is not loaded";
    return false;
  }

  --plugin.retain_count;
  if (plugin.retain_count > 0) {
    return true;
  }
  if (plugin.in_flight_calls != 0) {
    ++plugin.retain_count;
    out_error = "encoder plugin has in-flight calls";
    return false;
  }

  if (plugin.api.destroy != nullptr && plugin.context != nullptr) {
    plugin.api.destroy(plugin.context);
  }
  close_shared_library(plugin.library);
  reset_encoder_plugin(plugin);
  return true;
}

bool configure_external_decoder_plugin(ExternalDecoderPlugin& plugin,
    std::uint16_t transfer_syntax_code,
    const dicomsdl_codec_option_list_v1* options, dicomsdl_codec_error_v1* error,
    std::string& out_error) {
  out_error.clear();
  if (plugin.api.configure == nullptr || plugin.context == nullptr) {
    out_error = "decoder plugin is not ready";
    return false;
  }
  if (!begin_inflight(plugin.retain_count, plugin.in_flight_calls, out_error)) {
    return false;
  }
  const int ok =
      plugin.api.configure(plugin.context, transfer_syntax_code, options, error);
  end_inflight(plugin.in_flight_calls);
  return ok != 0;
}

bool configure_external_encoder_plugin(ExternalEncoderPlugin& plugin,
    std::uint16_t transfer_syntax_code,
    const dicomsdl_codec_option_list_v1* options, dicomsdl_codec_error_v1* error,
    std::string& out_error) {
  out_error.clear();
  if (plugin.api.configure == nullptr || plugin.context == nullptr) {
    out_error = "encoder plugin is not ready";
    return false;
  }
  if (!begin_inflight(plugin.retain_count, plugin.in_flight_calls, out_error)) {
    return false;
  }
  const int ok =
      plugin.api.configure(plugin.context, transfer_syntax_code, options, error);
  end_inflight(plugin.in_flight_calls);
  return ok != 0;
}

bool decode_external_frame(ExternalDecoderPlugin& plugin,
    const dicomsdl_decoder_request_v1* request, dicomsdl_codec_error_v1* error,
    std::string& out_error) {
  out_error.clear();
  if (plugin.api.decode_frame == nullptr || plugin.context == nullptr) {
    out_error = "decoder plugin is not ready";
    return false;
  }
  if (!begin_inflight(plugin.retain_count, plugin.in_flight_calls, out_error)) {
    return false;
  }
  const int ok = plugin.api.decode_frame(plugin.context, request, error);
  end_inflight(plugin.in_flight_calls);
  return ok != 0;
}

bool encode_external_frame(ExternalEncoderPlugin& plugin,
    const dicomsdl_encoder_request_v1* request, dicomsdl_codec_error_v1* error,
    std::string& out_error) {
  out_error.clear();
  if (plugin.api.encode_frame == nullptr || plugin.context == nullptr) {
    out_error = "encoder plugin is not ready";
    return false;
  }
  if (!begin_inflight(plugin.retain_count, plugin.in_flight_calls, out_error)) {
    return false;
  }
  const int ok = plugin.api.encode_frame(plugin.context, request, error);
  end_inflight(plugin.in_flight_calls);
  return ok != 0;
}

}  // namespace dicom::pixel::detail::abi
