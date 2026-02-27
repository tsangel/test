#include "pixel/registry/codec_registry.hpp"

#include "diagnostics.h"
#if DICOMSDL_CODEC_HTJ2K_BUILTIN
#include "pixel/plugin_abi/builtin/htj2k_builtin_plugin.hpp"
#endif
#if DICOMSDL_CODEC_JPEG2K_BUILTIN
#include "pixel/plugin_abi/builtin/jpeg2k_builtin_plugin.hpp"
#endif
#if DICOMSDL_CODEC_JPEG_BUILTIN
#include "pixel/plugin_abi/builtin/jpeg_builtin_plugin.hpp"
#endif
#if DICOMSDL_CODEC_JPEGLS_BUILTIN
#include "pixel/plugin_abi/builtin/jpegls_builtin_plugin.hpp"
#endif
#if DICOMSDL_CODEC_JPEGXL_BUILTIN
#include "pixel/plugin_abi/builtin/jpegxl_builtin_plugin.hpp"
#endif
#include "pixel/plugin_abi/builtin/rle_builtin_plugin.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace dicom::pixel::detail {
using namespace dicom::literals;
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

bool encode_frame_plugin_encapsulated_uncompressed(
    const CodecEncodeFrameInput& input, std::span<const CodecOptionKv> encode_options,
    std::vector<std::uint8_t>& out_encoded_frame,
    CodecError& out_error) noexcept;

bool decode_frame_plugin_native(
    const CodecDecodeFrameInput& input, CodecError& out_error) noexcept;
bool decode_frame_plugin_encapsulated_uncompressed(
    const CodecDecodeFrameInput& input, CodecError& out_error) noexcept;

namespace {

constexpr double kDefaultLossyJ2kTargetPsnr = 45.0;
constexpr int kDefaultNearLosslessJpegLsError = 2;
constexpr double kDefaultLossyJpegXlDistance = 1.0;

#if DICOMSDL_CODEC_JPEG_BUILTIN
constexpr codec_encode_frame_fn kJpegEncodeDispatch =
    &encode_frame_plugin_jpeg_via_abi;
constexpr codec_decode_frame_fn kJpegDecodeDispatch =
    &decode_frame_plugin_jpeg_via_abi;
#else
constexpr codec_encode_frame_fn kJpegEncodeDispatch = nullptr;
constexpr codec_decode_frame_fn kJpegDecodeDispatch = nullptr;
#endif

#if DICOMSDL_CODEC_JPEGLS_BUILTIN
constexpr codec_encode_frame_fn kJpegLsEncodeDispatch =
    &encode_frame_plugin_jpegls_via_abi;
constexpr codec_decode_frame_fn kJpegLsDecodeDispatch =
    &decode_frame_plugin_jpegls_via_abi;
#else
constexpr codec_encode_frame_fn kJpegLsEncodeDispatch = nullptr;
constexpr codec_decode_frame_fn kJpegLsDecodeDispatch = nullptr;
#endif

#if DICOMSDL_CODEC_JPEG2K_BUILTIN
constexpr codec_encode_frame_fn kJpeg2kEncodeDispatch =
    &encode_frame_plugin_jpeg2k_via_abi;
constexpr codec_decode_frame_fn kJpeg2kDecodeDispatch =
    &decode_frame_plugin_jpeg2k_via_abi;
#else
constexpr codec_encode_frame_fn kJpeg2kEncodeDispatch = nullptr;
constexpr codec_decode_frame_fn kJpeg2kDecodeDispatch = nullptr;
#endif

#if DICOMSDL_CODEC_HTJ2K_BUILTIN
constexpr codec_encode_frame_fn kHtj2kEncodeDispatch =
    &encode_frame_plugin_htj2k_via_abi;
constexpr codec_decode_frame_fn kHtj2kDecodeDispatch =
    &decode_frame_plugin_htj2k_via_abi;
#else
constexpr codec_encode_frame_fn kHtj2kEncodeDispatch = nullptr;
constexpr codec_decode_frame_fn kHtj2kDecodeDispatch = nullptr;
#endif

#if DICOMSDL_CODEC_JPEGXL_BUILTIN
constexpr codec_encode_frame_fn kJpegXlEncodeDispatch =
    &encode_frame_plugin_jpegxl_via_abi;
constexpr codec_decode_frame_fn kJpegXlDecodeDispatch =
    &decode_frame_plugin_jpegxl_via_abi;
#else
constexpr codec_encode_frame_fn kJpegXlEncodeDispatch = nullptr;
constexpr codec_decode_frame_fn kJpegXlDecodeDispatch = nullptr;
#endif

constexpr std::array<CodecOptionSchema, 0> kNoOptionSchema{};
constexpr std::array<CodecOptionSchema, 1> kJpegOptionSchema{{
    {"quality", "int", "[1,100]", "90..95"},
}};
constexpr std::array<CodecOptionSchema, 1> kJpegLsOptionSchema{{
    {"near_lossless_error", "int", "[0,255]", "lossless=0, near-lossless start 2..3"},
}};
constexpr std::array<CodecOptionSchema, 4> kJpeg2kOptionSchema{{
    {"target_bpp", "double", ">=0", "start 1.5 (1.0 smaller, 2.0 higher quality)"},
    {"target_psnr", "double", ">=0", "start 45 (40 smaller, 55 higher quality)"},
    {"threads", "int", "[-1,0] or >0", "-1(auto), 0(library default), >0 explicit"},
    {"color_transform", "bool", "{true,false}", "RGB source: usually true"},
}};
constexpr std::array<CodecOptionSchema, 4> kHtj2kOptionSchema{{
    {"target_bpp", "double", ">=0", "start 1.5 (1.0 smaller, 2.0 higher quality)"},
    {"target_psnr", "double", ">=0", "start 50 (45 smaller, 60 higher quality)"},
    {"threads", "int", "[-1,0] or >0", "-1(auto), 0(library default), >0 explicit"},
    {"color_transform", "bool", "{true,false}", "RGB source: usually true"},
}};
constexpr std::array<CodecOptionSchema, 3> kJpegXlOptionSchema{{
    {"distance", "double", "[0,25]", "lossless=0, lossy start 1.5 (0.5..3.0)"},
    {"effort", "int", "[1,10]", "start 7 (5..8)"},
    {"threads", "int", "[-1,0] or >0", "-1(auto), 0(library default), >0 explicit"},
}};

template <std::size_t N>
[[nodiscard]] constexpr std::span<const CodecOptionSchema> option_schema_span(
    const std::array<CodecOptionSchema, N>& option_schema) noexcept {
	return std::span<const CodecOptionSchema>(option_schema.data(),
	    option_schema.size());
}

void push_option(codec_option_pairs& pairs, std::string_view key, double value) {
	pairs.push_back(CodecOptionKv{key, codec_option_value{value}});
}

void push_option(codec_option_pairs& pairs, std::string_view key, std::int64_t value) {
	pairs.push_back(CodecOptionKv{key, codec_option_value{value}});
}

void push_option(codec_option_pairs& pairs, std::string_view key, bool value) {
	pairs.push_back(CodecOptionKv{key, codec_option_value{value}});
}

[[nodiscard]] std::optional<std::string> default_no_compression(
    uid::WellKnown transfer_syntax, codec_option_pairs& out_pairs) {
	(void)transfer_syntax;
	out_pairs.clear();
	return std::nullopt;
}

[[nodiscard]] std::optional<std::string> default_rle_options(
    uid::WellKnown transfer_syntax, codec_option_pairs& out_pairs) {
	(void)transfer_syntax;
	out_pairs.clear();
	return std::nullopt;
}

[[nodiscard]] std::optional<std::string> default_jpeg_options(
    uid::WellKnown transfer_syntax, codec_option_pairs& out_pairs) {
	(void)transfer_syntax;
	out_pairs.clear();
	push_option(out_pairs, "quality", std::int64_t{90});
	return std::nullopt;
}

[[nodiscard]] std::optional<std::string> default_jpegls_options(
    uid::WellKnown transfer_syntax, codec_option_pairs& out_pairs) {
	out_pairs.clear();
	const auto near_lossless_error = transfer_syntax.is_lossy()
	                                     ? std::int64_t{kDefaultNearLosslessJpegLsError}
	                                     : std::int64_t{0};
	push_option(out_pairs, "near_lossless_error", near_lossless_error);
	return std::nullopt;
}

[[nodiscard]] std::optional<std::string> default_jpeg2k_options(
    uid::WellKnown transfer_syntax, codec_option_pairs& out_pairs) {
	out_pairs.clear();
	push_option(out_pairs, "target_bpp", 0.0);
	push_option(out_pairs, "target_psnr",
	    transfer_syntax.is_lossy() ? kDefaultLossyJ2kTargetPsnr : 0.0);
	push_option(out_pairs, "threads", std::int64_t{-1});
	push_option(out_pairs, "color_transform", true);
	return std::nullopt;
}

[[nodiscard]] std::optional<std::string> default_htj2k_options(
    uid::WellKnown transfer_syntax, codec_option_pairs& out_pairs) {
	out_pairs.clear();
	push_option(out_pairs, "target_bpp", 0.0);
	push_option(out_pairs, "target_psnr",
	    transfer_syntax.is_lossy() ? kDefaultLossyJ2kTargetPsnr : 0.0);
	push_option(out_pairs, "threads", std::int64_t{-1});
	push_option(out_pairs, "color_transform", true);
	return std::nullopt;
}

[[nodiscard]] std::optional<std::string> default_jpegxl_options(
    uid::WellKnown transfer_syntax, codec_option_pairs& out_pairs) {
	if (transfer_syntax == "JPEGXLJPEGRecompression"_uid) {
		return std::string(
		    "JPEGXLJPEGRecompression transfer syntax is decode-only");
	}
	double distance = kDefaultLossyJpegXlDistance;
	if (transfer_syntax == "JPEGXLLossless"_uid) {
		distance = 0.0;
	} else if (transfer_syntax == "JPEGXL"_uid) {
		distance = kDefaultLossyJpegXlDistance;
	}
	out_pairs.clear();
	push_option(out_pairs, "distance", distance);
	push_option(out_pairs, "effort", std::int64_t{7});
	push_option(out_pairs, "threads", std::int64_t{-1});
	return std::nullopt;
}

[[nodiscard]] std::string_view plugin_key_for_transfer_syntax(
    uid::WellKnown transfer_syntax) noexcept {
	if (transfer_syntax.is_uncompressed()) {
		return transfer_syntax.is_encapsulated()
		           ? std::string_view("encapsulated-uncompressed")
		           : std::string_view("native");
	}
	if (transfer_syntax.is_rle()) {
		return std::string_view("rle");
	}
	if (transfer_syntax.is_htj2k()) {
		return std::string_view("htj2k");
	}
	if (transfer_syntax.is_jpeg2000()) {
		return std::string_view("jpeg2k");
	}
	if (transfer_syntax.is_jpegls()) {
		return std::string_view("jpegls");
	}
	if (transfer_syntax.is_jpegxl()) {
		return std::string_view("jpegxl");
	}
	if (transfer_syntax.is_jpeg_family()) {
		return std::string_view("jpeg");
	}
	return {};
}

[[nodiscard]] CodecProfile profile_for_transfer_syntax(
    uid::WellKnown transfer_syntax) noexcept {
	if (transfer_syntax.is_uncompressed()) {
		return transfer_syntax.is_encapsulated()
		           ? CodecProfile::encapsulated_uncompressed
		           : CodecProfile::native_uncompressed;
	}
	if (transfer_syntax.is_rle()) {
		return CodecProfile::rle_lossless;
	}
	if (transfer_syntax.is_htj2k()) {
		if (transfer_syntax == "HTJ2KLosslessRPCL"_uid) {
			return CodecProfile::htj2k_lossless_rpcl;
		}
		return transfer_syntax.is_lossless() ? CodecProfile::htj2k_lossless
		                                     : CodecProfile::htj2k_lossy;
	}
	if (transfer_syntax.is_jpeg2000()) {
		return transfer_syntax.is_lossless() ? CodecProfile::jpeg2000_lossless
		                                     : CodecProfile::jpeg2000_lossy;
	}
	if (transfer_syntax.is_jpegls()) {
		return transfer_syntax.is_lossless()
		           ? CodecProfile::jpegls_lossless
		           : CodecProfile::jpegls_near_lossless;
	}
	if (transfer_syntax.is_jpegxl()) {
		if (transfer_syntax == "JPEGXLJPEGRecompression"_uid) {
			return CodecProfile::jpegxl_jpeg_recompression;
		}
		return transfer_syntax.is_lossless() ? CodecProfile::jpegxl_lossless
		                                     : CodecProfile::jpegxl_lossy;
	}
	if (transfer_syntax.is_jpeg_family()) {
		return transfer_syntax.is_lossless() ? CodecProfile::jpeg_lossless
		                                     : CodecProfile::jpeg_lossy;
	}
	return CodecProfile::unknown;
}

void refresh_binding_capability_from_plugin(const CodecPlugin* plugin,
    TransferSyntaxPluginBinding& binding) {
	const bool ts_encode_supported = binding.transfer_syntax.supports_pixel_encode();
	const bool ts_decode_supported = binding.transfer_syntax.supports_pixel_decode();
	const bool has_encode_dispatch =
	    plugin != nullptr && plugin->encode_frame != nullptr;
	const bool has_decode_dispatch =
	    plugin != nullptr && plugin->decode_frame != nullptr;

	// Native uncompressed encode does not require plugin encode dispatch.
	if (binding.profile == CodecProfile::native_uncompressed) {
		binding.encode_supported = ts_encode_supported;
	} else {
		binding.encode_supported = ts_encode_supported && has_encode_dispatch;
	}
	binding.decode_supported = ts_decode_supported && has_decode_dispatch;
}

void register_plugin_if_missing(CodecRegistry& registry,
    const CodecPlugin& plugin) {
	if (registry.find_plugin(plugin.key)) {
		return;
	}
	(void)registry.register_plugin(plugin);
}

void register_binding_if_missing(CodecRegistry& registry,
    const TransferSyntaxPluginBinding& binding) {
	if (registry.find_binding(binding.transfer_syntax)) {
		return;
	}
	(void)registry.register_binding(binding);
}

} // namespace

bool CodecRegistry::register_plugin(const CodecPlugin& plugin) {
	std::unique_lock<std::shared_mutex> lock(dispatch_mutex_);
	if (plugin.key.empty() || find_plugin(plugin.key)) {
		return false;
	}
	plugins_.push_back(plugin);
	const auto plugin_index = plugins_.size() - 1;
	const auto* plugin_ptr = &plugins_[plugin_index];
	for (auto& binding : bindings_) {
		if (binding.plugin_key == plugin.key) {
			binding.plugin_index = plugin_index;
			refresh_binding_capability_from_plugin(plugin_ptr, binding);
		}
	}
	return true;
}

bool CodecRegistry::register_binding(
    const TransferSyntaxPluginBinding& binding) {
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
	const CodecPlugin* plugin_ptr = nullptr;
	if (normalized.plugin_index < plugins_.size()) {
		plugin_ptr = &plugins_[normalized.plugin_index];
	}
	refresh_binding_capability_from_plugin(plugin_ptr, normalized);
	bindings_.push_back(normalized);
	binding_index_by_transfer_syntax_[transfer_syntax_key] = bindings_.size() - 1;
	return true;
}

std::size_t CodecRegistry::find_plugin_index(
    std::string_view plugin_key) const noexcept {
	for (std::size_t i = 0; i < plugins_.size(); ++i) {
		if (plugins_[i].key == plugin_key) {
			return i;
		}
	}
	return kInvalidPluginIndex;
}

const CodecPlugin* CodecRegistry::find_plugin(
    std::string_view plugin_key) const noexcept {
	const auto plugin_index = find_plugin_index(plugin_key);
	if (plugin_index != kInvalidPluginIndex) {
		return &plugins_[plugin_index];
	}
	return nullptr;
}

const TransferSyntaxPluginBinding* CodecRegistry::find_binding(
    uid::WellKnown transfer_syntax) const noexcept {
	const auto it =
	    binding_index_by_transfer_syntax_.find(transfer_syntax.raw_index());
	if (it == binding_index_by_transfer_syntax_.end()) {
		return nullptr;
	}
	const auto index = it->second;
	if (index < bindings_.size()) {
		return &bindings_[index];
	}
	return nullptr;
}

const CodecPlugin* CodecRegistry::resolve_encoder_plugin(
    const TransferSyntaxPluginBinding& binding) const noexcept {
	if (!binding.encode_supported) {
		return nullptr;
	}
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
	if (!binding.decode_supported) {
		return nullptr;
	}
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
	if (!binding) {
		return nullptr;
	}
	return resolve_encoder_plugin(*binding);
}

const CodecPlugin* CodecRegistry::resolve_decoder_plugin(
    uid::WellKnown transfer_syntax) const noexcept {
	const auto* binding = find_binding(transfer_syntax);
	if (!binding) {
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
	if (out_previous_encode_frame) {
		*out_previous_encode_frame = plugin.encode_frame;
	}
	if (out_previous_decode_frame) {
		*out_previous_decode_frame = plugin.decode_frame;
	}
	if (update_encode) {
		plugin.encode_frame = encode_frame;
	}
	if (update_decode) {
		plugin.decode_frame = decode_frame;
	}
	for (auto& binding : bindings_) {
		if (binding.plugin_key == plugin.key) {
			refresh_binding_capability_from_plugin(&plugin, binding);
		}
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
	static const bool initialized = [] {
		register_default_codec_plugins(registry);
		register_default_transfer_syntax_bindings(registry);
		return true;
	}();
	(void)initialized;
	return registry;
}

void register_default_codec_plugins(CodecRegistry& registry) {
	register_plugin_if_missing(registry, CodecPlugin{
	    .key = "native",
	    .display_name = "Native Uncompressed",
	    .option_schema = option_schema_span(kNoOptionSchema),
	    .default_options = &default_no_compression,
	    .encode_frame = nullptr,
	    .decode_frame = &decode_frame_plugin_native,
	});

	register_plugin_if_missing(registry, CodecPlugin{
	    .key = "encapsulated-uncompressed",
	    .display_name = "Encapsulated Uncompressed",
	    .option_schema = option_schema_span(kNoOptionSchema),
	    .default_options = &default_no_compression,
	    .encode_frame = &encode_frame_plugin_encapsulated_uncompressed,
	    .decode_frame = &decode_frame_plugin_encapsulated_uncompressed,
	});

	register_plugin_if_missing(registry, CodecPlugin{
	    .key = "rle",
	    .display_name = "RLE Lossless",
	    .option_schema = option_schema_span(kNoOptionSchema),
	    .default_options = &default_rle_options,
	    .encode_frame = &encode_frame_plugin_rle_via_abi,
	    .decode_frame = &decode_frame_plugin_rle_via_abi,
	});

	register_plugin_if_missing(registry, CodecPlugin{
	    .key = "jpeg2k",
	    .display_name = "JPEG 2000",
	    .option_schema = option_schema_span(kJpeg2kOptionSchema),
	    .default_options = &default_jpeg2k_options,
	    .encode_frame = kJpeg2kEncodeDispatch,
	    .decode_frame = kJpeg2kDecodeDispatch,
	});

	register_plugin_if_missing(registry, CodecPlugin{
	    .key = "htj2k",
	    .display_name = "HTJ2K",
	    .option_schema = option_schema_span(kHtj2kOptionSchema),
	    .default_options = &default_htj2k_options,
	    .encode_frame = kHtj2kEncodeDispatch,
	    .decode_frame = kHtj2kDecodeDispatch,
	});

	register_plugin_if_missing(registry, CodecPlugin{
	    .key = "jpegls",
	    .display_name = "JPEG-LS",
	    .option_schema = option_schema_span(kJpegLsOptionSchema),
	    .default_options = &default_jpegls_options,
	    .encode_frame = kJpegLsEncodeDispatch,
	    .decode_frame = kJpegLsDecodeDispatch,
	});

	register_plugin_if_missing(registry, CodecPlugin{
	    .key = "jpeg",
	    .display_name = "JPEG",
	    .option_schema = option_schema_span(kJpegOptionSchema),
	    .default_options = &default_jpeg_options,
	    .encode_frame = kJpegEncodeDispatch,
	    .decode_frame = kJpegDecodeDispatch,
	});

	register_plugin_if_missing(registry, CodecPlugin{
	    .key = "jpegxl",
	    .display_name = "JPEG XL",
	    .option_schema = option_schema_span(kJpegXlOptionSchema),
	    .default_options = &default_jpegxl_options,
	    .encode_frame = kJpegXlEncodeDispatch,
	    .decode_frame = kJpegXlDecodeDispatch,
	});
}

void register_default_transfer_syntax_bindings(CodecRegistry& registry) {
	for (const auto& uid_entry : kUidRegistry) {
		if (uid_entry.uid_type != UidType::TransferSyntax) {
			continue;
		}
		const auto transfer_syntax_opt = uid::from_keyword(uid_entry.keyword);
		if (!transfer_syntax_opt) {
			continue;
		}

		const auto transfer_syntax = *transfer_syntax_opt;
		if (!transfer_syntax.supports_pixel_encode() &&
		    !transfer_syntax.supports_pixel_decode()) {
			continue;
		}

		const auto plugin_key = plugin_key_for_transfer_syntax(transfer_syntax);
		if (plugin_key.empty()) {
			continue;
		}

		register_binding_if_missing(registry, TransferSyntaxPluginBinding{
		    .transfer_syntax = transfer_syntax,
		    .plugin_key = plugin_key,
		    .profile = profile_for_transfer_syntax(transfer_syntax),
		    .encode_supported = transfer_syntax.supports_pixel_encode(),
		    .decode_supported = transfer_syntax.supports_pixel_decode(),
		});
	}
}

} // namespace dicom::pixel::detail

namespace dicom::pixel {

namespace {

void set_optional_error(std::string* out_error, std::string message) {
	if (out_error) {
		*out_error = std::move(message);
	}
}

[[nodiscard]] detail::codec_decode_frame_fn decode_dispatch_for_backend(
    Htj2kDecoder backend) noexcept {
#if DICOMSDL_CODEC_HTJ2K_BUILTIN
	return detail::htj2k_decode_dispatch_for_backend(backend);
#else
	(void)backend;
	return nullptr;
#endif
}

} // namespace

bool set_htj2k_decoder_backend(Htj2kDecoder backend, std::string* out_error) {
	if (out_error) {
		out_error->clear();
	}
#if DICOMSDL_CODEC_HTJ2K_BUILTIN
	auto& registry = detail::global_codec_registry();
	const auto decode_dispatch = decode_dispatch_for_backend(backend);
	detail::codec_decode_frame_fn previous_decode = nullptr;
	if (!registry.update_plugin_dispatch("htj2k", nullptr, false, decode_dispatch,
	        true, nullptr, &previous_decode)) {
		set_optional_error(out_error,
		    "htj2k plugin is not registered in codec registry");
		return false;
	}
	if (!detail::is_builtin_htj2k_decode_dispatch(previous_decode)) {
		(void)registry.update_plugin_dispatch(
		    "htj2k", nullptr, false, previous_decode, true);
		set_optional_error(out_error,
		    "htj2k decode dispatch is externally overridden; backend switch is unavailable");
		return false;
	}
	return true;
#else
	(void)backend;
	set_optional_error(out_error,
	    "htj2k builtin dispatch is disabled by CMake configuration");
	return false;
#endif
}

Htj2kDecoder get_htj2k_decoder_backend() noexcept {
#if DICOMSDL_CODEC_HTJ2K_BUILTIN
	auto& registry = detail::global_codec_registry();
	[[maybe_unused]] const auto dispatch_lock = registry.acquire_dispatch_read_lock();
	const auto* plugin = registry.find_plugin("htj2k");
	if (!plugin) {
		return Htj2kDecoder::auto_select;
	}
	return detail::htj2k_decoder_backend_for_dispatch(plugin->decode_frame);
#else
	return Htj2kDecoder::auto_select;
#endif
}

} // namespace dicom::pixel
