#include "pixel/registry/codec_registry.hpp"

#include "diagnostics.h"

#include <limits>
#include <optional>
#include <shared_mutex>
#include <string>

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
    std::string_view plugin_key, std::optional<std::size_t> frame_index,
    const CodecError& error) {
  const auto status = codec_status_code_name(error.code);
  const std::string_view stage =
      error.stage.empty() ? std::string_view("unknown") : std::string_view(error.stage);
  const std::string_view detail =
      error.detail.empty() ? std::string_view("unspecified codec error")
                           : std::string_view(error.detail);

  if (frame_index.has_value()) {
    return fmt::format(
        "{} file={} ts={} plugin={} frame={} status={} stage={} reason={}",
        function_name, file_path, transfer_syntax.value(), plugin_key,
        *frame_index, status, stage, detail);
  }
  return fmt::format(
      "{} file={} ts={} plugin={} status={} stage={} reason={}",
      function_name, file_path, transfer_syntax.value(), plugin_key, status,
      stage, detail);
}

[[noreturn]] void throw_codec_error_with_context(
    std::string_view function_name, std::string_view file_path,
    uid::WellKnown transfer_syntax, std::string_view plugin_key,
    std::optional<std::size_t> frame_index, const CodecError& error) {
  diag::error_and_throw("{}", format_codec_error_context(
      function_name, file_path, transfer_syntax, plugin_key, frame_index, error));
}

bool CodecRegistry::register_plugin(const CodecPlugin& plugin) {
  std::unique_lock<std::shared_mutex> lock(dispatch_mutex_);
  if (plugin.key.empty() || find_plugin(plugin.key) != nullptr) {
    return false;
  }
  plugins_.push_back(plugin);
  const auto plugin_index = plugins_.size() - 1;
  for (auto& binding : bindings_) {
    if (binding.plugin_key == plugin.key) {
      binding.plugin_index = plugin_index;
    }
  }
  return true;
}

bool CodecRegistry::register_binding(const TransferSyntaxPluginBinding& binding) {
  std::unique_lock<std::shared_mutex> lock(dispatch_mutex_);
  const auto transfer_syntax_key = binding.transfer_syntax.raw_index();
  if (!binding.transfer_syntax.valid() ||
      binding.transfer_syntax.uid_type() != UidType::TransferSyntax ||
      binding.plugin_key.empty() ||
      binding_index_by_transfer_syntax_.find(transfer_syntax_key) !=
          binding_index_by_transfer_syntax_.end()) {
    return false;
  }

  auto normalized = binding;
  normalized.plugin_index = find_plugin_index(binding.plugin_key);
  bindings_.push_back(normalized);
  binding_index_by_transfer_syntax_[transfer_syntax_key] = bindings_.size() - 1;
  return true;
}

std::size_t CodecRegistry::find_plugin_index(std::string_view plugin_key) const noexcept {
  for (std::size_t i = 0; i < plugins_.size(); ++i) {
    if (plugins_[i].key == plugin_key) {
      return i;
    }
  }
  return kInvalidPluginIndex;
}

const CodecPlugin* CodecRegistry::find_plugin(std::string_view plugin_key) const noexcept {
  const auto plugin_index = find_plugin_index(plugin_key);
  if (plugin_index == kInvalidPluginIndex) {
    return nullptr;
  }
  return &plugins_[plugin_index];
}

const TransferSyntaxPluginBinding* CodecRegistry::find_binding(
    uid::WellKnown transfer_syntax) const noexcept {
  const auto it = binding_index_by_transfer_syntax_.find(transfer_syntax.raw_index());
  if (it == binding_index_by_transfer_syntax_.end()) {
    return nullptr;
  }
  const auto index = it->second;
  if (index >= bindings_.size()) {
    return nullptr;
  }
  return &bindings_[index];
}

const CodecPlugin* CodecRegistry::resolve_encoder_plugin(
    const TransferSyntaxPluginBinding& binding) const noexcept {
  if (binding.plugin_index < plugins_.size()) {
    const auto& plugin = plugins_[binding.plugin_index];
    if (plugin.key == binding.plugin_key) {
      return &plugin;
    }
  }
  return find_plugin(binding.plugin_key);
}

const CodecPlugin* CodecRegistry::resolve_decoder_plugin(
    const TransferSyntaxPluginBinding& binding) const noexcept {
  if (binding.plugin_index < plugins_.size()) {
    const auto& plugin = plugins_[binding.plugin_index];
    if (plugin.key == binding.plugin_key) {
      return &plugin;
    }
  }
  return find_plugin(binding.plugin_key);
}

const CodecPlugin* CodecRegistry::resolve_encoder_plugin(
    uid::WellKnown transfer_syntax) const noexcept {
  const auto* binding = find_binding(transfer_syntax);
  if (binding == nullptr) {
    return nullptr;
  }
  return resolve_encoder_plugin(*binding);
}

const CodecPlugin* CodecRegistry::resolve_decoder_plugin(
    uid::WellKnown transfer_syntax) const noexcept {
  const auto* binding = find_binding(transfer_syntax);
  if (binding == nullptr) {
    return nullptr;
  }
  return resolve_decoder_plugin(*binding);
}

CodecRegistry::dispatch_read_lock CodecRegistry::acquire_dispatch_read_lock() const {
  return dispatch_read_lock(dispatch_mutex_);
}

bool CodecRegistry::update_plugin_dispatch(
    std::string_view plugin_key, codec_encode_frame_fn encode_frame,
    bool update_encode, codec_decode_frame_fn decode_frame,
    bool update_decode, codec_encode_frame_fn* out_previous_encode_frame,
    codec_decode_frame_fn* out_previous_decode_frame) noexcept {
  std::unique_lock<std::shared_mutex> lock(dispatch_mutex_);
  const auto plugin_index = find_plugin_index(plugin_key);
  if (plugin_index == kInvalidPluginIndex) {
    return false;
  }

  auto& plugin = plugins_[plugin_index];
  if (out_previous_encode_frame != nullptr) {
    *out_previous_encode_frame = plugin.encode_frame;
  }
  if (out_previous_decode_frame != nullptr) {
    *out_previous_decode_frame = plugin.decode_frame;
  }
  if (update_encode) {
    plugin.encode_frame = encode_frame;
  }
  if (update_decode) {
    plugin.decode_frame = decode_frame;
  }
  return true;
}

void CodecRegistry::clear() {
  std::unique_lock<std::shared_mutex> lock(dispatch_mutex_);
  plugins_.clear();
  bindings_.clear();
  binding_index_by_transfer_syntax_.clear();
}

CodecRegistry& global_codec_registry() {
  static CodecRegistry registry{};
  return registry;
}

void register_default_codec_plugins(CodecRegistry& registry) {
  (void)registry;
}

void register_default_transfer_syntax_bindings(CodecRegistry& registry) {
  (void)registry;
}

}  // namespace dicom::pixel::detail

namespace dicom::pixel {

namespace {

void set_optional_error(std::string* out_error, std::string message) {
  if (out_error != nullptr) {
    *out_error = std::move(message);
  }
}

Htj2kDecoder& selected_htj2k_backend() {
  static Htj2kDecoder backend = Htj2kDecoder::auto_select;
  return backend;
}

}  // namespace

bool set_htj2k_decoder_backend(Htj2kDecoder backend, std::string* out_error) {
  selected_htj2k_backend() = backend;
  set_optional_error(out_error, {});
  return true;
}

Htj2kDecoder get_htj2k_decoder_backend() noexcept {
  return selected_htj2k_backend();
}

bool use_openjph_for_htj2k_decode(std::string* out_error) {
  return set_htj2k_decoder_backend(Htj2kDecoder::openjph, out_error);
}

bool use_openjpeg_for_htj2k_decode(std::string* out_error) {
  return set_htj2k_decoder_backend(Htj2kDecoder::openjpeg, out_error);
}

}  // namespace dicom::pixel
