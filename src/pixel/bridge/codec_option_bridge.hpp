#pragma once

#include "../registry/codec_registry.hpp"

#include <string>
#include <vector>

namespace dicom::pixel::detail {

struct AbiOptionStorage {
  std::vector<std::string> keys{};
  std::vector<std::string> values{};
  std::vector<dicomsdl_codec_option_kv_v1> items{};
  dicomsdl_codec_option_list_v1 list{};
};

bool build_abi_option_storage_from_pairs(std::span<const CodecOptionKv> option_pairs,
    AbiOptionStorage& out_storage, CodecError& out_error) noexcept;

[[nodiscard]] inline const dicomsdl_codec_option_list_v1* abi_option_list_ptr(
    const AbiOptionStorage& storage) noexcept {
  return storage.list.count == 0 ? nullptr : &storage.list;
}

}  // namespace dicom::pixel::detail
