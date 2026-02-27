#pragma once

#include "pixel/registry/codec_registry.hpp"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dicom::pixel::detail {

struct SelectedEncodePluginBinding {
	const TransferSyntaxPluginBinding* binding{nullptr};
	const CodecPlugin* plugin{nullptr};
};

[[nodiscard]] SelectedEncodePluginBinding select_encode_plugin_binding_or_throw(
    std::string_view function_name, std::string_view file_path,
    uid::WellKnown transfer_syntax);

[[nodiscard]] codec_option_pairs build_codec_option_pairs_from_text_or_throw(
    std::string_view function_name, std::string_view file_path,
    uid::WellKnown transfer_syntax, std::string_view plugin_key,
    std::span<const pixel::CodecOptionTextKv> codec_opt,
    std::vector<std::string>* owned_option_keys = nullptr);

} // namespace dicom::pixel::detail
