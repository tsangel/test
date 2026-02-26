#include "pixel_codec_option_bridge.hpp"

#include <limits>
#include <type_traits>
#include <utility>

#include <fmt/format.h>

namespace dicom::pixel::detail {

namespace {

constexpr std::size_t kMaxOptionCount = 64;
constexpr std::size_t kMaxOptionKeyBytes = 128;
constexpr std::size_t kMaxOptionValueBytes = 1024;

}  // namespace

bool build_abi_option_storage_from_pairs(std::span<const CodecOptionKv> option_pairs,
    AbiOptionStorage& out_storage, CodecError& out_error) noexcept {
  out_storage = {};
  out_error = {};

  if (option_pairs.size() >
      static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
    set_codec_error(out_error, CodecStatusCode::internal_error,
        "parse_options", "option count exceeds uint32 range");
    return false;
  }
  if (option_pairs.size() > kMaxOptionCount) {
    set_codec_error(out_error, CodecStatusCode::invalid_argument,
        "parse_options", fmt::format(
            "option count exceeds max_option_count (count={}, max={})",
            option_pairs.size(), kMaxOptionCount));
    return false;
  }

  out_storage.keys.reserve(option_pairs.size());
  out_storage.values.reserve(option_pairs.size());
  out_storage.items.reserve(option_pairs.size());
  for (const auto& pair : option_pairs) {
    if (pair.key.empty()) {
      set_codec_error(out_error, CodecStatusCode::invalid_argument,
          "parse_options", "empty option key is not allowed");
      return false;
    }
    if (pair.key.size() > kMaxOptionKeyBytes) {
      set_codec_error(out_error, CodecStatusCode::invalid_argument,
          "parse_options", fmt::format(
              "option key exceeds max_option_key_bytes (key='{}', size={}, max={})",
              pair.key, pair.key.size(), kMaxOptionKeyBytes));
      return false;
    }

    out_storage.keys.emplace_back(pair.key);
    std::string value{};
    std::visit([&value](const auto& option_value) {
      using value_type = std::decay_t<decltype(option_value)>;
      if constexpr (std::is_same_v<value_type, std::int64_t>) {
        value = std::to_string(option_value);
      } else if constexpr (std::is_same_v<value_type, double>) {
        value = fmt::format("{:.17g}", option_value);
      } else if constexpr (std::is_same_v<value_type, std::string>) {
        value = option_value;
      } else {
        value = option_value ? "true" : "false";
      }
    }, pair.value);
    if (value.size() > kMaxOptionValueBytes) {
      set_codec_error(out_error, CodecStatusCode::invalid_argument,
          "parse_options", fmt::format(
              "option value exceeds max_option_value_bytes "
              "(key='{}', size={}, max={})",
              pair.key, value.size(), kMaxOptionValueBytes));
      return false;
    }
    out_storage.values.push_back(std::move(value));
  }

  for (std::size_t i = 0; i < out_storage.keys.size(); ++i) {
    out_storage.items.push_back(
        {out_storage.keys[i].c_str(), out_storage.values[i].c_str()});
  }
  out_storage.list.items =
      out_storage.items.empty() ? nullptr : out_storage.items.data();
  out_storage.list.count = static_cast<std::uint32_t>(out_storage.items.size());
  return true;
}

}  // namespace dicom::pixel::detail
