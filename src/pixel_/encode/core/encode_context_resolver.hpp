#pragma once

#include "pixel_/registry/codec_registry.hpp"

#include <span>
#include <string>
#include <vector>

namespace dicom::pixel::detail {

struct ResolvedEncoderContext {
	uid::WellKnown transfer_syntax{};
	std::string plugin_key{};
	std::vector<std::string> option_keys{};
	codec_option_pairs codec_options{};
};

[[nodiscard]] ResolvedEncoderContext
resolve_encoder_context_with_default_options_or_throw(
    uid::WellKnown transfer_syntax);

[[nodiscard]] ResolvedEncoderContext
resolve_encoder_context_with_text_options_or_throw(
    uid::WellKnown transfer_syntax,
    std::span<const pixel::CodecOptionTextKv> codec_opt);

} // namespace dicom::pixel::detail
