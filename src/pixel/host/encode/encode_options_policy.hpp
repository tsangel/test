#pragma once

#include "dicom.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dicom::pixel::detail {

void validate_transfer_syntax_for_encode_or_throw(
    uid::WellKnown transfer_syntax);

[[nodiscard]] std::vector<CodecOptionKv> default_codec_options_for_transfer_syntax_or_throw(
    uid::WellKnown transfer_syntax);

[[nodiscard]] std::vector<CodecOptionKv> build_codec_option_pairs_from_text_or_throw(
    uid::WellKnown transfer_syntax, std::span<const CodecOptionTextKv> codec_opt,
    std::vector<std::string>* owned_option_keys = nullptr);

} // namespace dicom::pixel::detail
