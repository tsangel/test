#pragma once

#include "dicom.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace dicom::pixel::detail {

enum class codec_direction : std::uint8_t {
	encode = 0,
	decode = 1,
};

enum class codec_status_code : std::uint8_t {
	ok = 0,
	invalid_argument,
	unsupported,
	backend_error,
	internal_error,
};

struct codec_error {
	codec_status_code code{codec_status_code::ok};
	std::string stage{};
	std::string detail{};
};

template <typename T>
struct codec_result {
	bool ok{false};
	std::optional<T> value{};
	codec_error error{};
};

template <>
struct codec_result<void> {
	bool ok{false};
	codec_error error{};
};

enum class codec_profile : std::uint8_t {
	unknown = 0,
	native_uncompressed,
	encapsulated_uncompressed,
	rle_lossless,
	jpeg_lossless,
	jpeg_lossy,
	jpegls_lossless,
	jpegls_near_lossless,
	jpeg2000_lossless,
	jpeg2000_lossy,
	htj2k_lossless,
	htj2k_lossless_rpcl,
	htj2k_lossy,
	jpegxl_lossless,
	jpegxl_lossy,
	jpegxl_jpeg_recompression,
};

struct decode_value_transform {
	bool enabled{false};
	std::optional<pixel::ModalityLut> modality_lut{};
	double rescale_slope{1.0};
	double rescale_intercept{0.0};
};

struct CodecEncodeFrameInput {
	std::span<const std::uint8_t> source_frame{};
	uid::WellKnown transfer_syntax{};
	std::size_t rows{0};
	std::size_t cols{0};
	std::size_t samples_per_pixel{0};
	std::size_t bytes_per_sample{0};
	int bits_allocated{0};
	int bits_stored{0};
	int pixel_representation{0};
	bool use_multicomponent_transform{false};
	Planar source_planar{Planar::interleaved};
	bool planar_source{false};
	std::size_t row_payload_bytes{0};
	std::size_t source_row_stride{0};
	std::size_t source_plane_stride{0};
	std::size_t source_frame_payload{0};
	std::size_t destination_frame_payload{0};
	codec_profile profile{codec_profile::unknown};
};

struct CodecDecodeFrameInput {
	pixel::PixelDataInfo info{};
	decode_value_transform value_transform{};
	std::span<const std::uint8_t> prepared_source{};
	std::span<std::uint8_t> destination{};
	DecodeStrides destination_strides{};
	DecodeOptions options{};
};

struct codec_option_schema {
	std::string_view name{};
	std::string_view value_type{};
	std::string_view valid_range{};
	std::string_view recommendation{};
};

using codec_option_value = std::variant<std::int64_t, double, bool>;

struct codec_option_kv {
	std::string_view key{};
	codec_option_value value{};
};

using codec_option_pairs = std::vector<codec_option_kv>;

using codec_default_options_fn = std::optional<CodecOptions> (*)(
    uid::WellKnown transfer_syntax);
using codec_validate_options_fn = std::optional<std::string> (*)(
    uid::WellKnown transfer_syntax, const CodecOptions& codec_opt);
using codec_export_options_fn = std::optional<std::string> (*)(
    uid::WellKnown transfer_syntax, const CodecOptions& codec_opt,
    codec_option_pairs& out_pairs);
using codec_parse_options_fn = std::optional<std::string> (*)(
    uid::WellKnown transfer_syntax, std::span<const codec_option_kv> in_pairs,
    CodecOptions& out_codec_opt);
// Plugin frame encode hook. The caller parses options once and passes the parsed variant.
using codec_encode_frame_fn = bool (*)(
    const CodecEncodeFrameInput& input, const CodecOptions& parsed_options,
    std::vector<std::uint8_t>& out_encoded_frame,
    codec_error& out_error) noexcept;
// Plugin frame decode hook. The plugin must not throw across the boundary.
// On failure, return false and fill out_error.
using codec_decode_frame_fn = bool (*)(
    const CodecDecodeFrameInput& input, codec_error& out_error) noexcept;

struct codec_plugin {
	std::string_view key{};
	std::string_view display_name{};
	std::span<const codec_option_schema> option_schema{};
	codec_default_options_fn default_options{nullptr};
	codec_validate_options_fn validate_options{nullptr};
	codec_export_options_fn export_options{nullptr};
	codec_parse_options_fn parse_options{nullptr};
	codec_encode_frame_fn encode_frame{nullptr};
	codec_decode_frame_fn decode_frame{nullptr};
};

struct transfer_syntax_plugin_binding {
	uid::WellKnown transfer_syntax{};
	std::string_view plugin_key{};
	// Cached plugin slot in codec_registry::plugins_ for O(1) access after TS binding lookup.
	std::size_t plugin_index{std::numeric_limits<std::size_t>::max()};
	codec_profile profile{codec_profile::unknown};
	bool encode_supported{false};
	bool decode_supported{false};
};

[[nodiscard]] std::string_view codec_status_code_name(
    codec_status_code code) noexcept;

[[nodiscard]] std::string format_codec_error_context(
    std::string_view function_name, std::string_view file_path,
    uid::WellKnown transfer_syntax, std::string_view plugin_key,
    std::optional<std::size_t> frame_index, const codec_error& error);

[[noreturn]] void throw_codec_error_with_context(
    std::string_view function_name, std::string_view file_path,
    uid::WellKnown transfer_syntax, std::string_view plugin_key,
    std::optional<std::size_t> frame_index, const codec_error& error);

class codec_registry {
public:
	bool register_plugin(const codec_plugin& plugin);
	bool register_binding(const transfer_syntax_plugin_binding& binding);

	[[nodiscard]] const codec_plugin* find_plugin(
	    std::string_view plugin_key) const noexcept;
	[[nodiscard]] const transfer_syntax_plugin_binding* find_binding(
	    uid::WellKnown transfer_syntax) const noexcept;
	[[nodiscard]] const codec_plugin* resolve_plugin(
	    uid::WellKnown transfer_syntax, codec_direction direction) const noexcept;

	void clear();

	[[nodiscard]] std::span<const codec_plugin> plugins() const noexcept {
		return plugins_;
	}
	[[nodiscard]] std::span<const transfer_syntax_plugin_binding> bindings() const noexcept {
		return bindings_;
	}

private:
	static constexpr std::size_t kInvalidPluginIndex =
	    std::numeric_limits<std::size_t>::max();
	[[nodiscard]] std::size_t find_plugin_index(
	    std::string_view plugin_key) const noexcept;
	std::vector<codec_plugin> plugins_{};
	std::vector<transfer_syntax_plugin_binding> bindings_{};
};

[[nodiscard]] codec_registry& global_codec_registry();

void register_default_codec_plugins(codec_registry& registry);
void register_default_transfer_syntax_bindings(codec_registry& registry);

} // namespace dicom::pixel::detail
