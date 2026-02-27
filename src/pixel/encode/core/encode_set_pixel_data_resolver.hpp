#pragma once

#include "pixel/encode/core/encode_binding_resolver.hpp"

#include <span>
#include <string_view>

namespace dicom::pixel::detail {

struct ResolvedSetPixelDataRequest {
	const TransferSyntaxPluginBinding* binding{nullptr};
	codec_option_pairs codec_options{};
};

[[nodiscard]] ResolvedSetPixelDataRequest
resolve_set_pixel_data_request_with_default_options_or_throw(
    std::string_view file_path, uid::WellKnown transfer_syntax);

[[nodiscard]] ResolvedSetPixelDataRequest
resolve_set_pixel_data_request_with_text_options_or_throw(
    std::string_view file_path, uid::WellKnown transfer_syntax,
    std::span<const pixel::CodecOptionTextKv> codec_opt);

[[nodiscard]] const TransferSyntaxPluginBinding&
resolve_set_pixel_data_binding_for_encoder_context_or_throw(
    std::string_view file_path, uid::WellKnown transfer_syntax,
    std::string_view expected_plugin_key);

void validate_encoder_context_for_set_pixel_data_or_throw(
    std::string_view file_path, uid::WellKnown transfer_syntax,
    bool encoder_context_configured, uid::WellKnown encoder_context_transfer_syntax);

void update_transfer_syntax_uid_element_after_set_pixel_data_or_throw(
    DicomFile& file, uid::WellKnown transfer_syntax);

} // namespace dicom::pixel::detail
