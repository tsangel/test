#include "pixel_/encode/core/encode_binding_resolver.hpp"

#include "diagnostics.h"

#include <string>

namespace dicom::pixel::detail {

SelectedEncodePluginBinding select_encode_plugin_binding_or_throw(
    std::string_view function_name, std::string_view file_path,
    uid::WellKnown transfer_syntax) {
	const auto& codec_registry = global_codec_registry();
	const auto* binding = codec_registry.find_binding(transfer_syntax);
	if (!binding || !binding->encode_supported) {
		diag::error_and_throw(
		    "{} file={} ts={} reason=transfer syntax is not supported for encoding by registry binding",
		    function_name, file_path, transfer_syntax.value());
	}
	const auto* encode_plugin = codec_registry.resolve_encoder_plugin(*binding);
	if (!encode_plugin) {
		diag::error_and_throw(
		    "{} file={} ts={} plugin={} reason=registry binding references a missing plugin",
		    function_name, file_path, transfer_syntax.value(), binding->plugin_key);
	}
	return SelectedEncodePluginBinding{binding, encode_plugin};
}

codec_option_pairs build_codec_option_pairs_from_text_or_throw(
    std::string_view function_name, std::string_view file_path,
    uid::WellKnown transfer_syntax, std::string_view plugin_key,
    std::span<const pixel::CodecOptionTextKv> codec_opt,
    std::vector<std::string>* owned_option_keys) {
	codec_option_pairs pairs{};
	pairs.reserve(codec_opt.size());
	if (owned_option_keys) {
		owned_option_keys->clear();
		owned_option_keys->reserve(codec_opt.size());
	}
	for (const auto& option : codec_opt) {
		if (option.key.empty()) {
			diag::error_and_throw(
			    "{} file={} ts={} plugin={} reason=codec option key must not be empty",
			    function_name, file_path, transfer_syntax.value(), plugin_key);
		}
		std::string_view option_key = option.key;
		if (owned_option_keys) {
			owned_option_keys->emplace_back(option.key);
			option_key = owned_option_keys->back();
		}
		pairs.push_back(pixel::CodecOptionKv{
		    .key = option_key,
		    .value = pixel::CodecOptionValue{std::string(option.value)},
		});
	}
	return pairs;
}

} // namespace dicom::pixel::detail
