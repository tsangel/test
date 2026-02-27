#include "dicom.h"

#include "pixel/bridge/codec_plugin_abi_adapter.hpp"
#include "pixel/plugin_abi/external/plugin_loader.hpp"
#include "pixel/bridge/codec_option_bridge.hpp"
#include "pixel/registry/codec_registry.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fmt/format.h>

namespace dicom::pixel::detail {

namespace {

using abi::ExternalDecoderPlugin;
using abi::ExternalEncoderPlugin;

struct ExternalPluginBridgeEntry {
  std::string plugin_key{};
  codec_encode_frame_fn fallback_encode{nullptr};
  codec_decode_frame_fn fallback_decode{nullptr};
  bool fallback_encode_captured{false};
  bool fallback_decode_captured{false};
  ExternalDecoderPlugin decoder{};
  ExternalEncoderPlugin encoder{};
  bool decoder_active{false};
  bool encoder_active{false};
  std::mutex mutex{};
};

std::unordered_map<std::string, std::shared_ptr<ExternalPluginBridgeEntry>>
    g_external_plugin_entries{};
std::mutex g_external_plugin_entries_mutex{};

constexpr std::size_t kMaxSourceFrameBytes = std::size_t{2} * 1024u * 1024u * 1024u;
constexpr std::size_t kMaxCoalesceBytesPerFrame =
    std::size_t{2} * 1024u * 1024u * 1024u;
constexpr std::int32_t kMaxRows = 65535;
constexpr std::int32_t kMaxCols = 65535;

bool decode_frame_plugin_external_bridge(
    const CodecDecodeFrameInput& input, CodecError& out_error) noexcept;
bool encode_frame_plugin_external_bridge(
    const CodecEncodeFrameInput& input, std::span<const CodecOptionKv> encode_options,
    std::vector<std::uint8_t>& out_encoded_frame,
    CodecError& out_error) noexcept;

void set_optional_error(std::string* out_error, std::string message) {
  if (out_error) {
    *out_error = std::move(message);
  }
}

[[nodiscard]] bool validate_decoder_bridge_input(
    const CodecDecodeFrameInput& input, CodecError& out_error) {
  if (input.info.rows <= 0 || input.info.cols <= 0) {
    set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
        fmt::format("invalid frame dimensions (rows={}, cols={})",
            input.info.rows, input.info.cols));
    return false;
  }
  if (input.info.rows > kMaxRows || input.info.cols > kMaxCols) {
    set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
        fmt::format(
            "frame dimensions exceed configured limits "
            "(rows={}, cols={}, max_rows={}, max_cols={})",
            input.info.rows, input.info.cols, kMaxRows, kMaxCols));
    return false;
  }
  if (input.prepared_source.size() > kMaxCoalesceBytesPerFrame) {
    set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
        fmt::format(
            "prepared source exceeds max_coalesce_bytes_per_frame "
            "(size={}, max={})",
            input.prepared_source.size(), kMaxCoalesceBytesPerFrame));
    return false;
  }
  if (input.destination.size() > kMaxSourceFrameBytes) {
    set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
        fmt::format("destination buffer exceeds max_frame_bytes (size={}, max={})",
            input.destination.size(), kMaxSourceFrameBytes));
    return false;
  }
  return true;
}

[[nodiscard]] bool validate_encoder_bridge_input(
    const CodecEncodeFrameInput& input, CodecError& out_error) {
  if (input.rows == 0 || input.cols == 0) {
    set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
        fmt::format("invalid frame dimensions (rows={}, cols={})",
            input.rows, input.cols));
    return false;
  }
  if (input.rows > static_cast<std::size_t>(kMaxRows) ||
      input.cols > static_cast<std::size_t>(kMaxCols)) {
    set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
        fmt::format(
            "frame dimensions exceed configured limits "
            "(rows={}, cols={}, max_rows={}, max_cols={})",
            input.rows, input.cols, kMaxRows, kMaxCols));
    return false;
  }
  if (input.source_frame.size() > kMaxSourceFrameBytes ||
      input.source_frame_size_bytes > kMaxSourceFrameBytes) {
    set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
        fmt::format(
            "source frame exceeds max_source_frame_bytes "
            "(span_size={}, frame_size={}, max={})",
            input.source_frame.size(), input.source_frame_size_bytes,
            kMaxSourceFrameBytes));
    return false;
  }
  if (input.destination_frame_payload > kMaxSourceFrameBytes) {
    set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
        fmt::format(
            "destination frame payload exceeds max_source_frame_bytes "
            "(payload={}, max={})",
            input.destination_frame_payload, kMaxSourceFrameBytes));
    return false;
  }
  return true;
}

[[nodiscard]] std::shared_ptr<ExternalPluginBridgeEntry>
find_external_entry_by_plugin_key(std::string_view plugin_key) {
  std::lock_guard<std::mutex> lock(g_external_plugin_entries_mutex);
  const auto it = g_external_plugin_entries.find(std::string(plugin_key));
  if (it == g_external_plugin_entries.end()) {
    return {};
  }
  return it->second;
}

[[nodiscard]] std::shared_ptr<ExternalPluginBridgeEntry>
find_external_entry_for_transfer_syntax(uid::WellKnown transfer_syntax) {
  const auto* binding = global_codec_registry().find_binding(transfer_syntax);
  if (!binding) {
    return {};
  }
  return find_external_entry_by_plugin_key(binding->plugin_key);
}

bool apply_external_bridge_dispatch(
    const std::shared_ptr<ExternalPluginBridgeEntry>& entry,
    std::string& out_error) {
  auto& registry = global_codec_registry();
  const auto encode_dispatch = entry->encoder_active
                                   ? &encode_frame_plugin_external_bridge
                                   : entry->fallback_encode;
  const auto decode_dispatch = entry->decoder_active
                                   ? &decode_frame_plugin_external_bridge
                                   : entry->fallback_decode;
  const bool update_encode = entry->encoder_active || entry->fallback_encode != nullptr;
  const bool update_decode = entry->decoder_active || entry->fallback_decode != nullptr;
  codec_encode_frame_fn previous_encode = nullptr;
  codec_decode_frame_fn previous_decode = nullptr;
  if (!registry.update_plugin_dispatch(entry->plugin_key, encode_dispatch, update_encode,
          decode_dispatch, update_decode, &previous_encode, &previous_decode)) {
    out_error = fmt::format("failed to update dispatch for plugin '{}'",
        entry->plugin_key);
    return false;
  }
  if (update_encode && !entry->fallback_encode_captured) {
    entry->fallback_encode = previous_encode;
    entry->fallback_encode_captured = true;
  }
  if (update_decode && !entry->fallback_decode_captured) {
    entry->fallback_decode = previous_decode;
    entry->fallback_decode_captured = true;
  }
  return true;
}

bool decode_frame_plugin_external_bridge(
    const CodecDecodeFrameInput& input, CodecError& out_error) noexcept {
  out_error = {};
  try {
    if (!validate_decoder_bridge_input(input, out_error)) {
      return false;
    }

    auto entry = find_external_entry_for_transfer_syntax(input.info.ts);
    if (!entry) {
      set_codec_error(out_error, CodecStatusCode::unsupported, "plugin_lookup",
          "no external decoder bridge is registered for transfer syntax");
      return false;
    }

    std::lock_guard<std::mutex> lock(entry->mutex);
    if (!entry->decoder_active) {
      set_codec_error(out_error, CodecStatusCode::unsupported, "plugin_lookup",
          "external decoder bridge is not active");
      return false;
    }

    dicomsdl_decoder_request_v1 request{};
    abi::build_decoder_request_v1(input, request);

    char detail_buffer[1024];
    dicomsdl_codec_error_v1 plugin_error{};
    abi::initialize_codec_error_buffer(plugin_error, detail_buffer,
        static_cast<std::uint32_t>(sizeof(detail_buffer)));
    std::string loader_error{};

    if (!abi::configure_external_decoder_plugin(entry->decoder,
            request.frame.transfer_syntax_code, nullptr, &plugin_error,
            loader_error)) {
      if (!loader_error.empty()) {
        set_codec_error(out_error, CodecStatusCode::internal_error,
            "plugin_lookup", loader_error);
        return false;
      }
      out_error = abi::decode_plugin_error_v1(
          plugin_error, "parse_options", "external decoder configure failed");
      if (out_error.code == CodecStatusCode::ok) {
        out_error.code = CodecStatusCode::backend_error;
      }
      return false;
    }

    abi::initialize_codec_error_buffer(plugin_error, detail_buffer,
        static_cast<std::uint32_t>(sizeof(detail_buffer)));
    if (!abi::decode_external_frame(
            entry->decoder, &request, &plugin_error, loader_error)) {
      if (!loader_error.empty()) {
        set_codec_error(out_error, CodecStatusCode::internal_error,
            "decode_frame", loader_error);
        return false;
      }
      out_error = abi::decode_plugin_error_v1(
          plugin_error, "decode_frame", "external decoder failed");
      if (out_error.code == CodecStatusCode::ok) {
        out_error.code = CodecStatusCode::backend_error;
      }
      return false;
    }
    return true;
  } catch (const std::exception& e) {
    set_codec_error(out_error, CodecStatusCode::backend_error, "decode_frame",
        e.what());
    return false;
  } catch (...) {
    set_codec_error(out_error, CodecStatusCode::backend_error, "decode_frame",
        "non-standard exception");
    return false;
  }
}

bool encode_frame_plugin_external_bridge(
    const CodecEncodeFrameInput& input, std::span<const CodecOptionKv> encode_options,
    std::vector<std::uint8_t>& out_encoded_frame,
    CodecError& out_error) noexcept {
  out_encoded_frame.clear();
  out_error = {};
  try {
    if (!validate_encoder_bridge_input(input, out_error)) {
      return false;
    }

    auto entry = find_external_entry_for_transfer_syntax(input.transfer_syntax);
    if (!entry) {
      set_codec_error(out_error, CodecStatusCode::unsupported, "plugin_lookup",
          "no external encoder bridge is registered for transfer syntax");
      return false;
    }

    std::lock_guard<std::mutex> lock(entry->mutex);
    if (!entry->encoder_active) {
      set_codec_error(out_error, CodecStatusCode::unsupported, "plugin_lookup",
          "external encoder bridge is not active");
      return false;
    }

    AbiOptionStorage option_storage{};
    if (!build_abi_option_storage_from_pairs(
            encode_options, option_storage, out_error)) {
      return false;
    }
    const dicomsdl_codec_option_list_v1* option_list_ptr =
        abi_option_list_ptr(option_storage);

    char detail_buffer[1024];
    dicomsdl_codec_error_v1 plugin_error{};
    abi::initialize_codec_error_buffer(plugin_error, detail_buffer,
        static_cast<std::uint32_t>(sizeof(detail_buffer)));
    std::string loader_error{};

    if (!abi::configure_external_encoder_plugin(entry->encoder,
            abi::to_transfer_syntax_code(input.transfer_syntax), option_list_ptr,
            &plugin_error, loader_error)) {
      if (!loader_error.empty()) {
        set_codec_error(out_error, CodecStatusCode::internal_error,
            "plugin_lookup", loader_error);
        return false;
      }
      out_error = abi::decode_plugin_error_v1(
          plugin_error, "parse_options", "external encoder configure failed");
      if (out_error.code == CodecStatusCode::ok) {
        out_error.code = CodecStatusCode::backend_error;
      }
      return false;
    }

    const std::size_t minimum_capacity = std::max<std::size_t>(
        input.destination_frame_payload, std::size_t{4096});
    if (minimum_capacity > kMaxSourceFrameBytes) {
      set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
          fmt::format(
              "initial output capacity exceeds max_source_frame_bytes "
              "(capacity={}, max={})",
              minimum_capacity, kMaxSourceFrameBytes));
      return false;
    }
    std::vector<std::uint8_t> encoded_buffer(minimum_capacity, 0);

    for (int attempt = 0; attempt < 4; ++attempt) {
      dicomsdl_encoder_request_v1 request{};
      abi::build_encoder_request_v1(input, std::span<std::uint8_t>(encoded_buffer),
          static_cast<std::uint64_t>(encoded_buffer.size()), request);

      abi::initialize_codec_error_buffer(plugin_error, detail_buffer,
          static_cast<std::uint32_t>(sizeof(detail_buffer)));
      if (abi::encode_external_frame(
              entry->encoder, &request, &plugin_error, loader_error)) {
        if (request.output.encoded_size >
            static_cast<std::uint64_t>(encoded_buffer.size())) {
          set_codec_error(out_error, CodecStatusCode::internal_error,
              "encode_frame",
              "external encoder returned encoded_size larger than output buffer");
          return false;
        }
        out_encoded_frame.assign(encoded_buffer.begin(),
            encoded_buffer.begin() +
                static_cast<std::ptrdiff_t>(request.output.encoded_size));
        out_error = {};
        return true;
      }

      if (!loader_error.empty()) {
        set_codec_error(out_error, CodecStatusCode::internal_error,
            "encode_frame", loader_error);
        return false;
      }
      if (plugin_error.status_code != DICOMSDL_CODEC_OUTPUT_TOO_SMALL) {
        out_error = abi::decode_plugin_error_v1(
            plugin_error, "encode_frame", "external encoder failed");
        if (out_error.code == CodecStatusCode::ok) {
          out_error.code = CodecStatusCode::backend_error;
        }
        return false;
      }

      std::size_t required_size =
          static_cast<std::size_t>(request.output.encoded_size);
      if (required_size <= encoded_buffer.size()) {
        if (encoded_buffer.size() > (std::numeric_limits<std::size_t>::max)() / 2) {
          required_size = (std::numeric_limits<std::size_t>::max)();
        } else {
          required_size = encoded_buffer.size() * 2;
        }
      }
      if (required_size == 0) {
        required_size = minimum_capacity;
      }
      if (required_size > kMaxSourceFrameBytes) {
        set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
            fmt::format(
                "requested encoded buffer size exceeds max_source_frame_bytes "
                "(required={}, max={})",
                required_size, kMaxSourceFrameBytes));
        return false;
      }
      encoded_buffer.resize(required_size);
    }

    set_codec_error(out_error, CodecStatusCode::backend_error, "encode_frame",
        "external encoder repeatedly returned OUTPUT_TOO_SMALL");
    return false;
  } catch (const std::exception& e) {
    set_codec_error(out_error, CodecStatusCode::backend_error, "encode_frame",
        e.what());
    return false;
  } catch (...) {
    set_codec_error(out_error, CodecStatusCode::backend_error, "encode_frame",
        "non-standard exception");
    return false;
  }
}

bool attach_external_decoder_plugin(
    const std::shared_ptr<ExternalPluginBridgeEntry>& entry,
    ExternalDecoderPlugin&& decoder_plugin, std::string& out_error) {
  out_error.clear();
  ExternalDecoderPlugin previous{};
  const bool had_previous = entry->decoder_active;
  if (had_previous) {
    previous = std::move(entry->decoder);
  }

  entry->decoder = std::move(decoder_plugin);
  entry->decoder_active = true;
  if (!apply_external_bridge_dispatch(entry, out_error)) {
    std::string release_error{};
    (void)abi::release_external_decoder_plugin(entry->decoder, release_error);
    entry->decoder = {};
    entry->decoder_active = false;
    if (had_previous) {
      entry->decoder = std::move(previous);
      entry->decoder_active = true;
      std::string restore_error{};
      (void)apply_external_bridge_dispatch(entry, restore_error);
    }
    return false;
  }

  if (had_previous) {
    std::string release_error{};
    if (!abi::release_external_decoder_plugin(previous, release_error)) {
      out_error = release_error;
      return false;
    }
  }
  return true;
}

bool attach_external_encoder_plugin(
    const std::shared_ptr<ExternalPluginBridgeEntry>& entry,
    ExternalEncoderPlugin&& encoder_plugin, std::string& out_error) {
  out_error.clear();
  ExternalEncoderPlugin previous{};
  const bool had_previous = entry->encoder_active;
  if (had_previous) {
    previous = std::move(entry->encoder);
  }

  entry->encoder = std::move(encoder_plugin);
  entry->encoder_active = true;
  if (!apply_external_bridge_dispatch(entry, out_error)) {
    std::string release_error{};
    (void)abi::release_external_encoder_plugin(entry->encoder, release_error);
    entry->encoder = {};
    entry->encoder_active = false;
    if (had_previous) {
      entry->encoder = std::move(previous);
      entry->encoder_active = true;
      std::string restore_error{};
      (void)apply_external_bridge_dispatch(entry, restore_error);
    }
    return false;
  }

  if (had_previous) {
    std::string release_error{};
    if (!abi::release_external_encoder_plugin(previous, release_error)) {
      out_error = release_error;
      return false;
    }
  }
  return true;
}

bool register_external_decoder_impl(std::string_view plugin_key,
    ExternalDecoderPlugin&& decoder_plugin, std::string& out_error) {
  out_error.clear();
  std::shared_ptr<ExternalPluginBridgeEntry> entry{};
  {
    std::lock_guard<std::mutex> lock(g_external_plugin_entries_mutex);
    auto& slot = g_external_plugin_entries[std::string(plugin_key)];
    if (!slot) {
      slot = std::make_shared<ExternalPluginBridgeEntry>();
      slot->plugin_key = std::string(plugin_key);
    }
    entry = slot;
  }
  std::lock_guard<std::mutex> lock(entry->mutex);
  return attach_external_decoder_plugin(
      entry, std::move(decoder_plugin), out_error);
}

bool register_external_encoder_impl(std::string_view plugin_key,
    ExternalEncoderPlugin&& encoder_plugin, std::string& out_error) {
  out_error.clear();
  std::shared_ptr<ExternalPluginBridgeEntry> entry{};
  {
    std::lock_guard<std::mutex> lock(g_external_plugin_entries_mutex);
    auto& slot = g_external_plugin_entries[std::string(plugin_key)];
    if (!slot) {
      slot = std::make_shared<ExternalPluginBridgeEntry>();
      slot->plugin_key = std::string(plugin_key);
    }
    entry = slot;
  }
  std::lock_guard<std::mutex> lock(entry->mutex);
  return attach_external_encoder_plugin(
      entry, std::move(encoder_plugin), out_error);
}

bool unregister_external_plugin_impl(
    std::string_view plugin_key, std::string& out_error) {
  out_error.clear();
  auto entry = find_external_entry_by_plugin_key(plugin_key);
  if (!entry) {
    out_error = fmt::format(
        "external plugin bridge '{}' is not registered",
        plugin_key);
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(entry->mutex);
    auto& registry = global_codec_registry();
    const bool restore_encode = entry->fallback_encode_captured;
    const bool restore_decode = entry->fallback_decode_captured;
    if (!registry.update_plugin_dispatch(entry->plugin_key, entry->fallback_encode,
            restore_encode, entry->fallback_decode, restore_decode)) {
      out_error = fmt::format(
          "failed to restore dispatch for plugin '{}'", entry->plugin_key);
      return false;
    }

    if (entry->decoder_active) {
      std::string release_error{};
      if (!abi::release_external_decoder_plugin(entry->decoder, release_error)) {
        out_error = release_error;
        return false;
      }
      entry->decoder_active = false;
      entry->decoder = {};
    }
    if (entry->encoder_active) {
      std::string release_error{};
      if (!abi::release_external_encoder_plugin(entry->encoder, release_error)) {
        out_error = release_error;
        return false;
      }
      entry->encoder_active = false;
      entry->encoder = {};
    }
  }

  std::lock_guard<std::mutex> lock(g_external_plugin_entries_mutex);
  g_external_plugin_entries.erase(std::string(plugin_key));
  return true;
}

}  // namespace

}  // namespace dicom::pixel::detail

namespace dicom::pixel {

bool register_external_codec_plugin_from_library(
    std::string_view library_path, std::string* out_plugin_key,
    std::string* out_error) {
  std::string error{};
  detail::abi::ExternalDecoderPlugin decoder_plugin{};
  detail::abi::ExternalEncoderPlugin encoder_plugin{};
  std::string decoder_error{};
  std::string encoder_error{};

  const bool decoder_loaded = detail::abi::load_external_decoder_plugin(
      library_path, decoder_plugin, decoder_error);
  const bool encoder_loaded = detail::abi::load_external_encoder_plugin(
      library_path, encoder_plugin, encoder_error);

  if (!decoder_loaded && !encoder_loaded) {
    error = fmt::format(
        "failed to load decoder ({}) and encoder ({}) from '{}'",
        decoder_error.empty() ? "no detail" : decoder_error,
        encoder_error.empty() ? "no detail" : encoder_error, library_path);
    detail::set_optional_error(out_error, std::move(error));
    return false;
  }

  std::string plugin_key{};
  if (decoder_loaded) {
    if (!decoder_plugin.api.info.plugin_key ||
        std::string_view(decoder_plugin.api.info.plugin_key).empty()) {
      std::string release_error{};
      (void)detail::abi::release_external_decoder_plugin(
          decoder_plugin, release_error);
      if (encoder_loaded) {
        (void)detail::abi::release_external_encoder_plugin(
            encoder_plugin, release_error);
      }
      detail::set_optional_error(
          out_error, "decoder plugin has empty plugin_key");
      return false;
    }
    plugin_key = decoder_plugin.api.info.plugin_key;
  }
  if (encoder_loaded) {
    if (!encoder_plugin.api.info.plugin_key ||
        std::string_view(encoder_plugin.api.info.plugin_key).empty()) {
      std::string release_error{};
      if (decoder_loaded) {
        (void)detail::abi::release_external_decoder_plugin(
            decoder_plugin, release_error);
      }
      (void)detail::abi::release_external_encoder_plugin(
          encoder_plugin, release_error);
      detail::set_optional_error(
          out_error, "encoder plugin has empty plugin_key");
      return false;
    }
    if (plugin_key.empty()) {
      plugin_key = encoder_plugin.api.info.plugin_key;
    } else if (plugin_key != encoder_plugin.api.info.plugin_key) {
      std::string release_error{};
      if (decoder_loaded) {
        (void)detail::abi::release_external_decoder_plugin(
            decoder_plugin, release_error);
      }
      (void)detail::abi::release_external_encoder_plugin(
          encoder_plugin, release_error);
      detail::set_optional_error(out_error,
          "decoder and encoder plugin_key mismatch in same library");
      return false;
    }
  }

  if (decoder_loaded) {
    if (!detail::register_external_decoder_impl(
            plugin_key, std::move(decoder_plugin), error)) {
      std::string release_error{};
      (void)detail::abi::release_external_decoder_plugin(
          decoder_plugin, release_error);
      if (encoder_loaded) {
        (void)detail::abi::release_external_encoder_plugin(
            encoder_plugin, release_error);
      }
      detail::set_optional_error(out_error, std::move(error));
      return false;
    }
  }
  if (encoder_loaded) {
    if (!detail::register_external_encoder_impl(
            plugin_key, std::move(encoder_plugin), error)) {
      std::string rollback_error{};
      if (decoder_loaded) {
        (void)detail::unregister_external_plugin_impl(
            plugin_key, rollback_error);
      }
      std::string release_error{};
      (void)detail::abi::release_external_encoder_plugin(
          encoder_plugin, release_error);
      detail::set_optional_error(out_error, std::move(error));
      return false;
    }
  }

  if (out_plugin_key) {
    *out_plugin_key = plugin_key;
  }
  detail::set_optional_error(out_error, {});
  return true;
}

bool register_external_decoder_plugin_static(
    const dicomsdl_decoder_plugin_api_v1* api, std::string* out_error) {
  if (api == nullptr) {
    detail::set_optional_error(out_error, "decoder api pointer is null");
    return false;
  }
  if (!api->info.plugin_key ||
      std::string_view(api->info.plugin_key).empty()) {
    detail::set_optional_error(out_error, "decoder api plugin_key is empty");
    return false;
  }

  detail::abi::ExternalDecoderPlugin decoder_plugin{};
  std::string error{};
  if (!detail::abi::init_external_decoder_plugin_from_api(
          *api, decoder_plugin, error)) {
    detail::set_optional_error(out_error, std::move(error));
    return false;
  }
  if (!detail::register_external_decoder_impl(
          api->info.plugin_key, std::move(decoder_plugin), error)) {
    std::string release_error{};
    (void)detail::abi::release_external_decoder_plugin(
        decoder_plugin, release_error);
    detail::set_optional_error(out_error, std::move(error));
    return false;
  }
  detail::set_optional_error(out_error, {});
  return true;
}

bool register_external_encoder_plugin_static(
    const dicomsdl_encoder_plugin_api_v1* api, std::string* out_error) {
  if (api == nullptr) {
    detail::set_optional_error(out_error, "encoder api pointer is null");
    return false;
  }
  if (!api->info.plugin_key ||
      std::string_view(api->info.plugin_key).empty()) {
    detail::set_optional_error(out_error, "encoder api plugin_key is empty");
    return false;
  }

  detail::abi::ExternalEncoderPlugin encoder_plugin{};
  std::string error{};
  if (!detail::abi::init_external_encoder_plugin_from_api(
          *api, encoder_plugin, error)) {
    detail::set_optional_error(out_error, std::move(error));
    return false;
  }
  if (!detail::register_external_encoder_impl(
          api->info.plugin_key, std::move(encoder_plugin), error)) {
    std::string release_error{};
    (void)detail::abi::release_external_encoder_plugin(
        encoder_plugin, release_error);
    detail::set_optional_error(out_error, std::move(error));
    return false;
  }
  detail::set_optional_error(out_error, {});
  return true;
}

bool unregister_external_codec_plugin(
    std::string_view plugin_key, std::string* out_error) {
  std::string error{};
  if (!detail::unregister_external_plugin_impl(plugin_key, error)) {
    detail::set_optional_error(out_error, std::move(error));
    return false;
  }
  detail::set_optional_error(out_error, {});
  return true;
}

}  // namespace dicom::pixel
