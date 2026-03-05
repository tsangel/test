#pragma once

#include "dicom.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace dicom::pixel::detail {

enum class CodecStatusCode : std::uint8_t {
	ok = 0,
	invalid_argument,
	unsupported,
	backend_error,
	internal_error,
};

struct CodecError {
	CodecStatusCode code{CodecStatusCode::ok};
	std::string stage{};
	std::string detail{};
};

inline void set_codec_error(CodecError& out_error, CodecStatusCode code,
    std::string_view stage, std::string_view detail) {
	out_error.code = code;
	out_error.stage = std::string(stage);
	out_error.detail = std::string(detail);
}

template <typename T>
struct CodecResult {
	bool ok{false};
	std::optional<T> value{};
	CodecError error{};
};

template <>
struct CodecResult<void> {
	bool ok{false};
	CodecError error{};
};

enum class CodecProfile : std::uint8_t {
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

struct ModalityValueTransform {
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
	std::size_t source_frame_size_bytes{0};
	std::size_t destination_frame_payload{0};
	CodecProfile profile{CodecProfile::unknown};
};

struct CodecDecodeFrameInput {
	pixel::PixelDataInfo info{};
	ModalityValueTransform modality_value_transform{};
	std::span<const std::uint8_t> prepared_source{};
	std::span<std::uint8_t> destination{};
	DecodeStrides destination_strides{};
	DecodeOptions options{};
};

struct CodecOptionSchema {
	std::string_view name{};
	std::string_view value_type{};
	std::string_view valid_range{};
	std::string_view recommendation{};
};

using codec_option_value = pixel::CodecOptionValue;
using CodecOptionKv = pixel::CodecOptionKv;
using codec_option_pairs = std::vector<pixel::CodecOptionKv>;

using codec_default_options_fn = std::optional<std::string> (*)(
    uid::WellKnown transfer_syntax, codec_option_pairs& out_pairs);
// Plugin frame encode hook. The caller provides exported option key/value pairs.
using codec_encode_frame_fn = bool (*)(
    const CodecEncodeFrameInput& input, std::span<const CodecOptionKv> encode_options,
    std::vector<std::uint8_t>& out_encoded_frame,
    CodecError& out_error) noexcept;
// Plugin frame decode hook. The plugin must not throw across the boundary.
// On failure, return false and fill out_error.
using codec_decode_frame_fn = bool (*)(
    const CodecDecodeFrameInput& input, CodecError& out_error) noexcept;

struct CodecPlugin {
	std::string_view key{};
	std::string_view display_name{};
	std::span<const CodecOptionSchema> option_schema{};
	codec_default_options_fn default_options{nullptr};
	codec_encode_frame_fn encode_frame{nullptr};
	codec_decode_frame_fn decode_frame{nullptr};
};

struct TransferSyntaxPluginBinding {
	uid::WellKnown transfer_syntax{};
	std::string_view plugin_key{};
	// Cached plugin slot in CodecRegistry::plugins_ for O(1) access after TS binding lookup.
	std::size_t plugin_index{std::numeric_limits<std::size_t>::max()};
	CodecProfile profile{CodecProfile::unknown};
	bool encode_supported{false};
	bool decode_supported{false};
};

[[nodiscard]] std::string_view codec_status_code_name(
    CodecStatusCode code) noexcept;

[[nodiscard]] std::string format_codec_error_context(
    std::string_view function_name, std::string_view file_path,
    uid::WellKnown transfer_syntax, std::string_view plugin_key,
    std::optional<std::size_t> frame_index, const CodecError& error);

[[noreturn]] void throw_codec_error_with_context(
    std::string_view function_name, std::string_view file_path,
    uid::WellKnown transfer_syntax, std::string_view plugin_key,
    std::optional<std::size_t> frame_index, const CodecError& error);

class CodecRegistry {
public:
	using dispatch_read_lock = std::shared_lock<std::shared_mutex>;

	bool register_plugin(const CodecPlugin& plugin);
	bool register_binding(const TransferSyntaxPluginBinding& binding);

	[[nodiscard]] const CodecPlugin* find_plugin(
	    std::string_view plugin_key) const noexcept;
	[[nodiscard]] const TransferSyntaxPluginBinding* find_binding(
	    uid::WellKnown transfer_syntax) const noexcept;
	[[nodiscard]] const CodecPlugin* resolve_encoder_plugin(
	    const TransferSyntaxPluginBinding& binding) const noexcept;
	[[nodiscard]] const CodecPlugin* resolve_decoder_plugin(
	    const TransferSyntaxPluginBinding& binding) const noexcept;
	[[nodiscard]] const CodecPlugin* resolve_encoder_plugin(
	    uid::WellKnown transfer_syntax) const noexcept;
	[[nodiscard]] const CodecPlugin* resolve_decoder_plugin(
	    uid::WellKnown transfer_syntax) const noexcept;
	[[nodiscard]] dispatch_read_lock acquire_dispatch_read_lock() const;
	bool update_plugin_dispatch(
	    std::string_view plugin_key, codec_encode_frame_fn encode_frame,
	    bool update_encode, codec_decode_frame_fn decode_frame,
	    bool update_decode,
	    codec_encode_frame_fn* out_previous_encode_frame = nullptr,
	    codec_decode_frame_fn* out_previous_decode_frame = nullptr) noexcept;

	void clear();

	[[nodiscard]] std::span<const CodecPlugin> plugins() const noexcept {
		return plugins_;
	}
	[[nodiscard]] std::span<const TransferSyntaxPluginBinding> bindings() const noexcept {
		return bindings_;
	}

private:
	static constexpr std::size_t kInvalidPluginIndex =
	    std::numeric_limits<std::size_t>::max();
	[[nodiscard]] std::size_t find_plugin_index(
	    std::string_view plugin_key) const noexcept;
	std::vector<CodecPlugin> plugins_{};
	std::vector<TransferSyntaxPluginBinding> bindings_{};
	std::unordered_map<std::uint16_t, std::size_t> binding_index_by_transfer_syntax_{};
	mutable std::shared_mutex dispatch_mutex_{};
};

[[nodiscard]] CodecRegistry& global_codec_registry();

void register_default_codec_plugins(CodecRegistry& registry);
void register_default_transfer_syntax_bindings(CodecRegistry& registry);

} // namespace dicom::pixel::detail
