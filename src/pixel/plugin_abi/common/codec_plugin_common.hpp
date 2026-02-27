#pragma once

#include "pixel/bridge/codec_plugin_abi_adapter.hpp"
#include "pixel/registry/codec_registry.hpp"
#include "pixel/common/codec_typed_options.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dicom::pixel::detail::plugin_common {

[[nodiscard]] inline constexpr std::uint32_t to_abi_status_code(
    CodecStatusCode code) noexcept {
  switch (code) {
  case CodecStatusCode::ok:
    return DICOMSDL_CODEC_OK;
  case CodecStatusCode::invalid_argument:
    return DICOMSDL_CODEC_INVALID_ARGUMENT;
  case CodecStatusCode::unsupported:
    return DICOMSDL_CODEC_UNSUPPORTED;
  case CodecStatusCode::internal_error:
    return DICOMSDL_CODEC_INTERNAL_ERROR;
  case CodecStatusCode::backend_error:
  default:
    return DICOMSDL_CODEC_BACKEND_ERROR;
  }
}

[[nodiscard]] inline constexpr std::uint32_t to_abi_stage_code(
    std::string_view stage) noexcept {
  if (stage == "plugin_lookup") {
    return DICOMSDL_CODEC_STAGE_PLUGIN_LOOKUP;
  }
  if (stage == "parse_options") {
    return DICOMSDL_CODEC_STAGE_PARSE_OPTIONS;
  }
  if (stage == "validate") {
    return DICOMSDL_CODEC_STAGE_VALIDATE;
  }
  if (stage == "load_frame_source") {
    return DICOMSDL_CODEC_STAGE_LOAD_FRAME_SOURCE;
  }
  if (stage == "encode_frame" || stage == "encode") {
    return DICOMSDL_CODEC_STAGE_ENCODE_FRAME;
  }
  if (stage == "decode_frame" || stage == "decode") {
    return DICOMSDL_CODEC_STAGE_DECODE_FRAME;
  }
  if (stage == "postprocess") {
    return DICOMSDL_CODEC_STAGE_POSTPROCESS;
  }
  if (stage == "allocate") {
    return DICOMSDL_CODEC_STAGE_ALLOCATE;
  }
  return DICOMSDL_CODEC_STAGE_UNKNOWN;
}

inline void set_abi_error(dicomsdl_codec_error_v1* error,
    std::uint32_t status_code, std::uint32_t stage_code,
    std::string_view detail) noexcept {
  if (!error) {
    return;
  }
  error->status_code = status_code;
  error->stage_code = stage_code;
  error->detail_length = 0;
  if (!error->detail || error->detail_capacity == 0) {
    return;
  }

  const std::size_t copy_size = std::min<std::size_t>(
      detail.size(), static_cast<std::size_t>(error->detail_capacity - 1));
  if (copy_size > 0) {
    std::memcpy(error->detail, detail.data(), copy_size);
  }
  error->detail[copy_size] = '\0';
  error->detail_length = static_cast<std::uint32_t>(copy_size);
}

inline void set_abi_ok(dicomsdl_codec_error_v1* error) noexcept {
  set_abi_error(error, DICOMSDL_CODEC_OK, DICOMSDL_CODEC_STAGE_UNKNOWN, "");
}

inline void set_abi_error_from_codec_error(dicomsdl_codec_error_v1* error,
    const CodecError& in_error, std::string_view fallback_stage,
    std::string_view fallback_detail) noexcept {
  CodecError effective = in_error;
  if (effective.code == CodecStatusCode::ok) {
    effective.code = CodecStatusCode::backend_error;
  }
  if (effective.stage.empty()) {
    effective.stage = std::string(fallback_stage);
  }
  if (effective.detail.empty()) {
    effective.detail = std::string(fallback_detail);
  }
  set_abi_error(error, to_abi_status_code(effective.code),
      to_abi_stage_code(effective.stage), effective.detail);
}

[[nodiscard]] inline bool checked_u64_to_size_t(
    std::uint64_t value, std::size_t& out) noexcept {
  if (value >
      static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return false;
  }
  out = static_cast<std::size_t>(value);
  return true;
}

[[nodiscard]] inline bool checked_positive_i32_to_size_t(
    std::int32_t value, std::size_t& out) noexcept {
  if (value <= 0) {
    return false;
  }
  out = static_cast<std::size_t>(value);
  return true;
}

[[nodiscard]] inline std::size_t data_type_bytes(
    DataType data_type) noexcept {
  switch (data_type) {
  case DataType::u8:
  case DataType::s8:
    return 1;
  case DataType::u16:
  case DataType::s16:
    return 2;
  case DataType::u32:
  case DataType::s32:
  case DataType::f32:
    return 4;
  case DataType::f64:
    return 8;
  case DataType::unknown:
  default:
    return 0;
  }
}

[[nodiscard]] inline bool is_option_separator(char c) noexcept {
  return c == '_' || c == '-' || c == ' ' || c == '\t';
}

[[nodiscard]] inline char normalized_option_key_char(char c) noexcept {
  return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

[[nodiscard]] inline bool option_key_matches(
    std::string_view input, std::string_view expected) noexcept {
  std::size_t input_index = 0;
  std::size_t expected_index = 0;
  while (true) {
    while (input_index < input.size() &&
           is_option_separator(input[input_index])) {
      ++input_index;
    }
    while (expected_index < expected.size() &&
           is_option_separator(expected[expected_index])) {
      ++expected_index;
    }
    if (input_index == input.size() || expected_index == expected.size()) {
      break;
    }
    if (normalized_option_key_char(input[input_index]) !=
        normalized_option_key_char(expected[expected_index])) {
      return false;
    }
    ++input_index;
    ++expected_index;
  }
  while (input_index < input.size() && is_option_separator(input[input_index])) {
    ++input_index;
  }
  while (expected_index < expected.size() &&
         is_option_separator(expected[expected_index])) {
    ++expected_index;
  }
  return input_index == input.size() && expected_index == expected.size();
}

[[nodiscard]] inline bool parse_strict_int_option(
    std::string_view text, int& out_value) noexcept {
  long long parsed = 0;
  const auto* begin = text.data();
  const auto* end = text.data() + text.size();
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc{} || result.ptr != end) {
    return false;
  }
  if (parsed < static_cast<long long>(std::numeric_limits<int>::min()) ||
      parsed > static_cast<long long>(std::numeric_limits<int>::max())) {
    return false;
  }
  out_value = static_cast<int>(parsed);
  return true;
}

[[nodiscard]] inline bool parse_strict_double_option(
    std::string_view text, double& out_value) noexcept {
  if (text.empty()) {
    return false;
  }

  std::string text_copy(text);
  char* end = nullptr;
  errno = 0;
  const double parsed = std::strtod(text_copy.c_str(), &end);
  if (end != text_copy.c_str() + text_copy.size()) {
    return false;
  }
  if (errno == ERANGE || !std::isfinite(parsed)) {
    return false;
  }
  out_value = parsed;
  return true;
}

[[nodiscard]] inline bool parse_bool_option(
    std::string_view text, bool& out_value) noexcept {
  if (text == "1") {
    out_value = true;
    return true;
  }
  if (text == "0") {
    out_value = false;
    return true;
  }

  std::string normalized{};
  normalized.reserve(text.size());
  for (const unsigned char ch : text) {
    if (ch == ' ' || ch == '\t') {
      continue;
    }
    normalized.push_back(static_cast<char>(std::tolower(ch)));
  }
  if (normalized == "true") {
    out_value = true;
    return true;
  }
  if (normalized == "false") {
    out_value = false;
    return true;
  }
  return false;
}

template <typename IsSupportedTransferSyntaxCode>
[[nodiscard]] inline bool validate_configure_no_options(
    const dicomsdl_codec_option_list_v1* options,
    std::uint16_t transfer_syntax_code, std::string_view non_empty_option_detail,
    std::string_view unsupported_transfer_syntax_detail,
    IsSupportedTransferSyntaxCode&& is_supported_transfer_syntax_code,
    dicomsdl_codec_error_v1* error) noexcept {
  if (options && options->count > 0) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_PARSE_OPTIONS, non_empty_option_detail);
    return false;
  }
  if (!is_supported_transfer_syntax_code(transfer_syntax_code)) {
    set_abi_error(error, DICOMSDL_CODEC_UNSUPPORTED,
        DICOMSDL_CODEC_STAGE_VALIDATE, unsupported_transfer_syntax_detail);
    return false;
  }
  return true;
}

[[nodiscard]] inline bool parse_single_int_configure_option(
    const dicomsdl_codec_option_list_v1* options,
    std::string_view expected_option_key, std::string_view null_option_detail,
    std::string_view unknown_option_prefix, std::string_view duplicate_option_detail,
    std::string_view invalid_value_detail, int default_value,
    int& out_value, dicomsdl_codec_error_v1* error) noexcept {
  out_value = default_value;
  if (!options) {
    return true;
  }

  bool option_seen = false;
  for (std::uint32_t i = 0; i < options->count; ++i) {
    const auto& item = options->items[i];
    if (!item.key || !item.value) {
      set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
          DICOMSDL_CODEC_STAGE_PARSE_OPTIONS, null_option_detail);
      return false;
    }
    const std::string_view key(item.key);
    if (!option_key_matches(key, expected_option_key)) {
      set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
          DICOMSDL_CODEC_STAGE_PARSE_OPTIONS,
          std::string(unknown_option_prefix) + std::string(key));
      return false;
    }
    if (option_seen) {
      set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
          DICOMSDL_CODEC_STAGE_PARSE_OPTIONS, duplicate_option_detail);
      return false;
    }
    if (!parse_strict_int_option(std::string_view(item.value), out_value)) {
      set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
          DICOMSDL_CODEC_STAGE_PARSE_OPTIONS, invalid_value_detail);
      return false;
    }
    option_seen = true;
  }
  return true;
}

[[nodiscard]] inline bool parse_jpeg_quality_configure_option(
    const dicomsdl_codec_option_list_v1* options, int default_quality,
    int& out_quality, dicomsdl_codec_error_v1* error) noexcept {
  return parse_single_int_configure_option(options, "quality",
      "JPEG option key/value is null", "unknown JPEG option key: ",
      "duplicate JPEG option key: quality",
      "quality must be an integer in [1,100]", default_quality, out_quality,
      error);
}

[[nodiscard]] inline bool validate_jpeg_quality_configure_option(
    int quality, dicomsdl_codec_error_v1* error) noexcept {
  if (quality < 1 || quality > 100) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_PARSE_OPTIONS,
        "JpegOptions.quality must be in [1,100]");
    return false;
  }
  return true;
}

[[nodiscard]] inline bool parse_jpegls_near_lossless_configure_option(
    const dicomsdl_codec_option_list_v1* options,
    int default_near_lossless_error, int& out_near_lossless_error,
    dicomsdl_codec_error_v1* error) noexcept {
  return parse_single_int_configure_option(options, "near_lossless_error",
      "JPEG-LS option key/value is null", "unknown JPEG-LS option key: ",
      "duplicate JPEG-LS option key: near_lossless_error",
      "near_lossless_error must be an integer in [0,255]",
      default_near_lossless_error, out_near_lossless_error, error);
}

[[nodiscard]] inline bool validate_jpegls_near_lossless_configure_option(
    int near_lossless_error, bool transfer_syntax_is_lossless,
    bool transfer_syntax_is_lossy, dicomsdl_codec_error_v1* error) noexcept {
  if (near_lossless_error < 0 || near_lossless_error > 255) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_PARSE_OPTIONS,
        "JpegLsOptions.near_lossless_error must be in [0,255]");
    return false;
  }
  if (transfer_syntax_is_lossless && near_lossless_error != 0) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_PARSE_OPTIONS,
        "JPEG-LS lossless transfer syntax requires near_lossless_error=0");
    return false;
  }
  if (transfer_syntax_is_lossy && near_lossless_error <= 0) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_PARSE_OPTIONS,
        "JPEG-LS lossy transfer syntax requires near_lossless_error>0");
    return false;
  }
  return true;
}

[[nodiscard]] inline bool parse_jpegxl_encoder_configure_options(
    const dicomsdl_codec_option_list_v1* options, JpegXlOptions& out_options,
    dicomsdl_codec_error_v1* error) noexcept {
  if (!options) {
    return true;
  }

  for (std::uint32_t i = 0; i < options->count; ++i) {
    const auto& item = options->items[i];
    if (!item.key || !item.value) {
      set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
          DICOMSDL_CODEC_STAGE_PARSE_OPTIONS,
          "JPEG-XL option key/value is null");
      return false;
    }
    const std::string_view key(item.key);
    const std::string_view value(item.value);
    if (option_key_matches(key, "distance")) {
      double parsed = 0.0;
      if (!parse_strict_double_option(value, parsed)) {
        set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
            DICOMSDL_CODEC_STAGE_PARSE_OPTIONS,
            "distance must be numeric");
        return false;
      }
      out_options.distance = parsed;
      continue;
    }
    if (option_key_matches(key, "effort")) {
      int parsed = 0;
      if (!parse_strict_int_option(value, parsed)) {
        set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
            DICOMSDL_CODEC_STAGE_PARSE_OPTIONS,
            "effort must be an integer");
        return false;
      }
      out_options.effort = parsed;
      continue;
    }
    if (option_key_matches(key, "threads")) {
      int parsed = 0;
      if (!parse_strict_int_option(value, parsed)) {
        set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
            DICOMSDL_CODEC_STAGE_PARSE_OPTIONS,
            "threads must be an integer");
        return false;
      }
      out_options.threads = parsed;
      continue;
    }

    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_PARSE_OPTIONS,
        std::string("unknown JPEG-XL option key: ") + std::string(key));
    return false;
  }
  return true;
}

[[nodiscard]] inline bool validate_jpegxl_encoder_configure_options(
    const JpegXlOptions& options, bool transfer_syntax_is_lossless,
    bool transfer_syntax_is_lossy, dicomsdl_codec_error_v1* error) noexcept {
  if (options.threads < -1) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_PARSE_OPTIONS,
        "JpegXlOptions.threads must be -1, 0, or positive");
    return false;
  }
  if (options.effort < 1 || options.effort > 10) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_PARSE_OPTIONS,
        "JpegXlOptions.effort must be in [1,10]");
    return false;
  }
  if (!std::isfinite(options.distance) ||
      options.distance < 0.0 || options.distance > 25.0) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_PARSE_OPTIONS,
        "JpegXlOptions.distance must be in [0,25]");
    return false;
  }
  if (transfer_syntax_is_lossless && options.distance != 0.0) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_PARSE_OPTIONS,
        "JPEGXLLossless transfer syntax requires distance=0");
    return false;
  }
  if (transfer_syntax_is_lossy && options.distance <= 0.0) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_PARSE_OPTIONS,
        "JPEGXL transfer syntax requires distance>0");
    return false;
  }
  return true;
}

enum class OptionParseResult {
  not_matched = 0,
  matched_ok = 1,
  matched_error = 2,
};

[[nodiscard]] inline OptionParseResult parse_decoder_threads_option(
    std::string_view key, std::string_view value, bool reject_duplicates,
    bool& threads_seen, int& out_threads, std::string_view duplicate_detail,
    std::string_view invalid_value_detail,
    dicomsdl_codec_error_v1* error) noexcept {
  if (!option_key_matches(key, "threads") &&
      !option_key_matches(key, "decoder_threads")) {
    return OptionParseResult::not_matched;
  }

  if (reject_duplicates && threads_seen) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_PARSE_OPTIONS, duplicate_detail);
    return OptionParseResult::matched_error;
  }

  int parsed_threads = 0;
  if (!parse_strict_int_option(value, parsed_threads)) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_PARSE_OPTIONS, invalid_value_detail);
    return OptionParseResult::matched_error;
  }

  out_threads = parsed_threads;
  threads_seen = true;
  return OptionParseResult::matched_ok;
}

[[nodiscard]] inline bool validate_decoder_threads_option(
    int decoder_threads, std::string_view invalid_range_detail,
    dicomsdl_codec_error_v1* error) noexcept {
  if (decoder_threads < -1) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_PARSE_OPTIONS, invalid_range_detail);
    return false;
  }
  return true;
}

[[nodiscard]] inline std::string normalize_option_value(
    std::string_view value) noexcept {
  std::string normalized{};
  normalized.reserve(value.size());
  for (const unsigned char ch : value) {
    if (is_option_separator(static_cast<char>(ch))) {
      continue;
    }
    normalized.push_back(static_cast<char>(std::tolower(ch)));
  }
  return normalized;
}

[[nodiscard]] inline bool parse_htj2k_decoder_backend_option(
    std::string_view text, Htj2kDecoder& out_backend) noexcept {
  const auto normalized = normalize_option_value(text);
  if (normalized == "auto" || normalized == "autoselect") {
    out_backend = Htj2kDecoder::auto_select;
    return true;
  }
  if (normalized == "openjph") {
    out_backend = Htj2kDecoder::openjph;
    return true;
  }
  if (normalized == "openjpeg") {
    out_backend = Htj2kDecoder::openjpeg;
    return true;
  }
  return false;
}

[[nodiscard]] inline constexpr const char* htj2k_decoder_backend_option_value(
    Htj2kDecoder backend) noexcept {
  switch (backend) {
  case Htj2kDecoder::openjph:
    return "openjph";
  case Htj2kDecoder::openjpeg:
    return "openjpeg";
  case Htj2kDecoder::auto_select:
  default:
    return "auto";
  }
}

struct Htj2kDecoderConfigureOptions {
  int decoder_threads{-1};
  Htj2kDecoder backend{Htj2kDecoder::auto_select};
};

[[nodiscard]] inline bool parse_htj2k_decoder_configure_options(
    const dicomsdl_codec_option_list_v1* options,
    Htj2kDecoderConfigureOptions& out_options,
    dicomsdl_codec_error_v1* error) noexcept {
  out_options = {};
  if (!options) {
    return true;
  }

  bool threads_seen = false;
  for (std::uint32_t i = 0; i < options->count; ++i) {
    const auto& item = options->items[i];
    if (!item.key || !item.value) {
      set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
          DICOMSDL_CODEC_STAGE_PARSE_OPTIONS,
          "HTJ2K decoder option key/value is null");
      return false;
    }

    const std::string_view key(item.key);
    const std::string_view value(item.value);
    if (option_key_matches(key, "backend") ||
        option_key_matches(key, "htj2k_backend") ||
        option_key_matches(key, "decoder_backend")) {
      if (!parse_htj2k_decoder_backend_option(value, out_options.backend)) {
        set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
            DICOMSDL_CODEC_STAGE_PARSE_OPTIONS,
            "backend must be one of: auto, openjph, openjpeg");
        return false;
      }
      continue;
    }
    switch (parse_decoder_threads_option(key, value, false, threads_seen,
        out_options.decoder_threads,
        "duplicate HTJ2K decoder option key: threads",
        "threads must be an integer", error)) {
    case OptionParseResult::matched_ok:
      continue;
    case OptionParseResult::matched_error:
      return false;
    case OptionParseResult::not_matched:
      break;
    }

    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_PARSE_OPTIONS,
        std::string("unknown HTJ2K decoder option key: ") + std::string(key));
    return false;
  }
  return true;
}

[[nodiscard]] inline bool validate_htj2k_decoder_configure_options(
    const Htj2kDecoderConfigureOptions& options,
    dicomsdl_codec_error_v1* error) noexcept {
  return validate_decoder_threads_option(options.decoder_threads,
      "DecodeOptions.decoder_threads must be -1, 0, or positive", error);
}

struct J2kLikeEncoderConfigureOptions {
  double target_bpp{0.0};
  double target_psnr{0.0};
  int threads{-1};
  bool use_color_transform{true};
};

[[nodiscard]] inline bool parse_j2k_like_encoder_configure_options(
    const dicomsdl_codec_option_list_v1* options,
    std::string_view null_option_detail, std::string_view unknown_key_prefix,
    J2kLikeEncoderConfigureOptions& out_options,
    dicomsdl_codec_error_v1* error) noexcept {
  out_options = {};
  if (!options) {
    return true;
  }

  for (std::uint32_t i = 0; i < options->count; ++i) {
    const auto& item = options->items[i];
    if (!item.key || !item.value) {
      set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
          DICOMSDL_CODEC_STAGE_PARSE_OPTIONS, null_option_detail);
      return false;
    }
    const std::string_view key(item.key);
    const std::string_view value(item.value);
    if (option_key_matches(key, "target_bpp") ||
        option_key_matches(key, "bpp")) {
      double parsed = 0.0;
      if (!parse_strict_double_option(value, parsed)) {
        set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
            DICOMSDL_CODEC_STAGE_PARSE_OPTIONS,
            "target_bpp must be numeric");
        return false;
      }
      out_options.target_bpp = parsed;
      continue;
    }
    if (option_key_matches(key, "target_psnr") ||
        option_key_matches(key, "psnr")) {
      double parsed = 0.0;
      if (!parse_strict_double_option(value, parsed)) {
        set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
            DICOMSDL_CODEC_STAGE_PARSE_OPTIONS,
            "target_psnr must be numeric");
        return false;
      }
      out_options.target_psnr = parsed;
      continue;
    }
    if (option_key_matches(key, "threads")) {
      int parsed = 0;
      if (!parse_strict_int_option(value, parsed)) {
        set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
            DICOMSDL_CODEC_STAGE_PARSE_OPTIONS,
            "threads must be an integer");
        return false;
      }
      out_options.threads = parsed;
      continue;
    }
    if (option_key_matches(key, "color_transform") ||
        option_key_matches(key, "mct") ||
        option_key_matches(key, "use_mct")) {
      bool parsed = false;
      if (!parse_bool_option(value, parsed)) {
        set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
            DICOMSDL_CODEC_STAGE_PARSE_OPTIONS,
            "color_transform/mct must be bool (or 0/1)");
        return false;
      }
      out_options.use_color_transform = parsed;
      continue;
    }

    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_PARSE_OPTIONS,
        std::string(unknown_key_prefix) + std::string(key));
    return false;
  }
  return true;
}

[[nodiscard]] inline bool validate_j2k_like_encoder_configure_options(
    const J2kLikeEncoderConfigureOptions& options,
    std::string_view options_type_name, dicomsdl_codec_error_v1* error) noexcept {
  if (options.target_bpp < 0.0 || options.target_psnr < 0.0) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_PARSE_OPTIONS,
        std::string(options_type_name) + ".target_bpp/target_psnr must be >= 0");
    return false;
  }
  if (options.threads < -1) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_PARSE_OPTIONS,
        std::string(options_type_name) + ".threads must be -1, 0, or positive");
    return false;
  }
  return true;
}

[[nodiscard]] inline bool decoder_request_has_value_transform(
    const dicomsdl_decoder_request_v1& request) noexcept {
  const std::size_t required_size =
      offsetof(dicomsdl_decoder_request_v1, value_transform) +
      sizeof(dicomsdl_decoder_value_transform_v1);
  return static_cast<std::size_t>(request.struct_size) >= required_size;
}

[[nodiscard]] inline bool decode_value_transform_from_request(
    const dicomsdl_decoder_request_v1& request,
    DecodeValueTransform& out_value_transform,
    CodecError& out_error) noexcept {
  out_value_transform = {};
  if (!decoder_request_has_value_transform(request)) {
    return true;
  }

  const auto& in_transform = request.value_transform;
  switch (in_transform.transform_kind) {
  case DICOMSDL_DECODER_VALUE_TRANSFORM_NONE:
    return true;
  case DICOMSDL_DECODER_VALUE_TRANSFORM_RESCALE:
    out_value_transform.enabled = true;
    out_value_transform.rescale_slope = in_transform.rescale_slope;
    out_value_transform.rescale_intercept = in_transform.rescale_intercept;
    return true;
  case DICOMSDL_DECODER_VALUE_TRANSFORM_MODALITY_LUT: {
    std::size_t lut_byte_size = 0;
    std::size_t lut_value_count = 0;
    if (!checked_u64_to_size_t(in_transform.lut_values_f32.size, lut_byte_size) ||
        !checked_u64_to_size_t(in_transform.lut_value_count, lut_value_count)) {
      set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
          "Modality LUT size exceeds host size_t range");
      return false;
    }
    if (lut_byte_size % sizeof(float) != 0) {
      set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
          "Modality LUT byte size is not a multiple of float size");
      return false;
    }
    if (lut_byte_size > 0 && in_transform.lut_values_f32.data == nullptr) {
      set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
          "Modality LUT values pointer is null");
      return false;
    }
    if (lut_value_count != (lut_byte_size / sizeof(float))) {
      set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
          "Modality LUT value_count does not match value byte size");
      return false;
    }

    pixel::ModalityLut lut{};
    lut.first_mapped = in_transform.lut_first_mapped;
    try {
      lut.values.resize(lut_value_count);
    } catch (const std::bad_alloc&) {
      set_codec_error(out_error, CodecStatusCode::internal_error, "allocate",
          "memory allocation failed");
      return false;
    }
    if (lut_byte_size > 0) {
      std::memcpy(lut.values.data(), in_transform.lut_values_f32.data, lut_byte_size);
    }
    out_value_transform.enabled = true;
    out_value_transform.modality_lut = std::move(lut);
    return true;
  }
  default:
    set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
        "decoder value transform kind is invalid");
    return false;
  }
}

struct PreparedDecodeRequest {
  pixel::PixelDataInfo info{};
  DecodeValueTransform value_transform{};
  DecodeOptions options{};
  std::span<const std::uint8_t> source{};
  std::span<std::uint8_t> destination{};
  DecodeStrides strides{};
};

template <typename IsSupportedTransferSyntaxCode>
[[nodiscard]] inline bool prepare_plugin_decode_request(
    const dicomsdl_decoder_request_v1& request,
    std::uint16_t configured_transfer_syntax_code, int decoder_threads,
    std::string_view unsupported_transfer_syntax_detail,
    std::string_view destination_dtype_detail,
    IsSupportedTransferSyntaxCode&& is_supported_transfer_syntax_code,
    PreparedDecodeRequest& out_request,
    dicomsdl_codec_error_v1* error) noexcept {
  out_request = {};

  if (!is_supported_transfer_syntax_code(request.frame.transfer_syntax_code)) {
    set_abi_error(error, DICOMSDL_CODEC_UNSUPPORTED, DICOMSDL_CODEC_STAGE_VALIDATE,
        unsupported_transfer_syntax_detail);
    return false;
  }
  if (request.frame.transfer_syntax_code != configured_transfer_syntax_code) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_VALIDATE,
        "request transfer syntax differs from configured transfer syntax");
    return false;
  }

  const auto source_dtype = abi::from_dtype_code(request.frame.source_dtype);
  if (!source_dtype || *source_dtype == DataType::unknown) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_VALIDATE, "source_dtype is invalid");
    return false;
  }
  const auto source_planar = abi::from_planar_code(request.frame.source_planar);
  if (!source_planar) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_VALIDATE, "source_planar is invalid");
    return false;
  }
  const auto dst_planar = abi::from_planar_code(request.output.dst_planar);
  if (!dst_planar) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_VALIDATE, "dst_planar is invalid");
    return false;
  }
  const auto dst_dtype = abi::from_dtype_code(request.output.dst_dtype);
  if (!dst_dtype || *dst_dtype == DataType::unknown) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_VALIDATE, "dst_dtype is invalid");
    return false;
  }
  const bool scaled_output = (*dst_dtype == DataType::f32);
  if (*dst_dtype != *source_dtype && !scaled_output) {
    set_abi_error(error, DICOMSDL_CODEC_UNSUPPORTED, DICOMSDL_CODEC_STAGE_VALIDATE,
        destination_dtype_detail);
    return false;
  }

  std::size_t source_size = 0;
  std::size_t dst_size = 0;
  std::size_t row_stride = 0;
  std::size_t frame_stride = 0;
  if (!checked_u64_to_size_t(request.source.source_buffer.size, source_size) ||
      !checked_u64_to_size_t(request.output.dst_size, dst_size) ||
      !checked_u64_to_size_t(request.output.row_stride, row_stride) ||
      !checked_u64_to_size_t(request.output.frame_stride, frame_stride)) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_VALIDATE,
        "request size/stride exceeds host size_t range");
    return false;
  }
  if (source_size > 0 && request.source.source_buffer.data == nullptr) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_VALIDATE, "source_buffer.data is null");
    return false;
  }
  if (dst_size > 0 && request.output.dst == nullptr) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_VALIDATE, "output.dst is null");
    return false;
  }

  const auto transfer_syntax =
      abi::from_transfer_syntax_code(request.frame.transfer_syntax_code);
  if (!transfer_syntax) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_VALIDATE, "transfer syntax code is invalid");
    return false;
  }
  out_request.info.ts = *transfer_syntax;
  out_request.info.sv_dtype = *source_dtype;
  out_request.info.rows = request.frame.rows;
  out_request.info.cols = request.frame.cols;
  out_request.info.frames = 1;
  out_request.info.samples_per_pixel = request.frame.samples_per_pixel;
  out_request.info.planar_configuration = *source_planar;
  out_request.info.bits_stored = request.frame.bits_stored;
  out_request.info.has_pixel_data = true;

  CodecError decode_error{};
  if (!decode_value_transform_from_request(
          request, out_request.value_transform, decode_error)) {
    set_abi_error_from_codec_error(error, decode_error, "validate",
        "failed to parse decoder value transform");
    return false;
  }
  if (scaled_output && !out_request.value_transform.enabled) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_VALIDATE,
        "scaled output requires a decoder value transform");
    return false;
  }

  out_request.options.planar_out = *dst_planar;
  out_request.options.scaled = scaled_output;
  out_request.options.decode_mct = request.frame.decode_mct != 0;
  out_request.options.decoder_threads = decoder_threads;

  out_request.source = std::span<const std::uint8_t>(
      request.source.source_buffer.data, source_size);
  out_request.destination =
      std::span<std::uint8_t>(request.output.dst, dst_size);
  out_request.strides = DecodeStrides{.row = row_stride, .frame = frame_stride};
  return true;
}

struct PreparedEncodeRequest {
  uid::WellKnown transfer_syntax{};
  DataType source_dtype{DataType::unknown};
  Planar source_planar{Planar::interleaved};
  std::size_t rows{0};
  std::size_t cols{0};
  std::size_t samples_per_pixel{0};
  std::size_t source_size{0};
  std::size_t row_stride{0};
  std::size_t encoded_capacity{0};
  std::size_t bytes_per_sample{0};
  bool lossless{false};
  bool use_multicomponent_transform{false};
  std::span<const std::uint8_t> source{};
};

template <typename IsSupportedTransferSyntaxCode>
[[nodiscard]] inline bool prepare_plugin_encode_request(
    const dicomsdl_encoder_request_v1& request,
    std::uint16_t configured_transfer_syntax_code,
    std::string_view unsupported_transfer_syntax_detail,
    IsSupportedTransferSyntaxCode&& is_supported_transfer_syntax_code,
    PreparedEncodeRequest& out_request,
    dicomsdl_codec_error_v1* error) noexcept {
  out_request = {};
  if (!is_supported_transfer_syntax_code(request.frame.transfer_syntax_code)) {
    set_abi_error(error, DICOMSDL_CODEC_UNSUPPORTED, DICOMSDL_CODEC_STAGE_VALIDATE,
        unsupported_transfer_syntax_detail);
    return false;
  }
  if (request.frame.transfer_syntax_code != configured_transfer_syntax_code) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_VALIDATE,
        "request transfer syntax differs from configured transfer syntax");
    return false;
  }

  const auto source_dtype = abi::from_dtype_code(request.frame.source_dtype);
  if (!source_dtype || *source_dtype == DataType::unknown) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_VALIDATE, "source_dtype is invalid");
    return false;
  }
  const auto source_planar = abi::from_planar_code(request.frame.source_planar);
  if (!source_planar) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_VALIDATE, "source_planar is invalid");
    return false;
  }

  std::size_t rows = 0;
  std::size_t cols = 0;
  std::size_t samples_per_pixel = 0;
  std::size_t source_size = 0;
  std::size_t row_stride = 0;
  std::size_t encoded_capacity = 0;
  if (!checked_positive_i32_to_size_t(request.frame.rows, rows) ||
      !checked_positive_i32_to_size_t(request.frame.cols, cols) ||
      !checked_positive_i32_to_size_t(
          request.frame.samples_per_pixel, samples_per_pixel) ||
      !checked_u64_to_size_t(request.source.source_buffer.size, source_size) ||
      !checked_u64_to_size_t(request.frame.source_row_stride, row_stride) ||
      !checked_u64_to_size_t(request.output.encoded_buffer.size, encoded_capacity)) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_VALIDATE,
        "request numeric field is invalid or exceeds size_t range");
    return false;
  }
  if (source_size > 0 && request.source.source_buffer.data == nullptr) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_VALIDATE, "source_buffer.data is null");
    return false;
  }

  const auto bytes_per_sample = data_type_bytes(*source_dtype);
  if (bytes_per_sample == 0) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_VALIDATE, "source_dtype is not byte-addressable");
    return false;
  }

  const auto transfer_syntax =
      abi::from_transfer_syntax_code(request.frame.transfer_syntax_code);
  if (!transfer_syntax) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_VALIDATE, "transfer syntax code is invalid");
    return false;
  }

  out_request.transfer_syntax = *transfer_syntax;
  out_request.source_dtype = *source_dtype;
  out_request.source_planar = *source_planar;
  out_request.rows = rows;
  out_request.cols = cols;
  out_request.samples_per_pixel = samples_per_pixel;
  out_request.source_size = source_size;
  out_request.row_stride = row_stride;
  out_request.encoded_capacity = encoded_capacity;
  out_request.bytes_per_sample = bytes_per_sample;
  out_request.lossless = transfer_syntax->is_lossless();
  out_request.use_multicomponent_transform =
      request.frame.use_multicomponent_transform != 0;
  out_request.source = std::span<const std::uint8_t>(
      request.source.source_buffer.data, source_size);
  return true;
}

[[nodiscard]] inline bool write_plugin_encoded_output(
    const dicomsdl_encoder_request_v1& request, std::size_t encoded_capacity,
    const std::vector<std::uint8_t>& encoded,
    dicomsdl_codec_error_v1* error) noexcept {
  auto* mutable_request = const_cast<dicomsdl_encoder_request_v1*>(&request);
  mutable_request->output.encoded_size =
      static_cast<std::uint64_t>(encoded.size());
  if (encoded.size() > encoded_capacity ||
      mutable_request->output.encoded_buffer.data == nullptr) {
    set_abi_error(error, DICOMSDL_CODEC_OUTPUT_TOO_SMALL,
        DICOMSDL_CODEC_STAGE_ENCODE_FRAME, "output buffer too small");
    return false;
  }
  std::memcpy(mutable_request->output.encoded_buffer.data, encoded.data(),
      encoded.size());
  return true;
}

} // namespace dicom::pixel::detail::plugin_common
