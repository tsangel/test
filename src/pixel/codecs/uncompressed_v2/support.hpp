#pragma once

#include <cstddef>
#include <cstdint>

#include "direct_api_v2.hpp"

namespace pixel::core_v2 {

struct DtypeInfo {
  uint32_t bytes;
  bool is_signed;
  bool is_float;
};

void copy_text(char* dst, std::size_t dst_capacity, const char* src);
void set_error_detail(ErrorState* state, const char* detail);

pixel_error_code_v2 fail_detail(ErrorState* state, pixel_error_code_v2 code,
    const char* stage, const char* reason);
pixel_error_code_v2 fail_detail_u32(ErrorState* state, pixel_error_code_v2 code,
    const char* stage, const char* reason_fmt, uint32_t value);

bool mul_u64(uint64_t a, uint64_t b, uint64_t* out);
bool u64_to_size(uint64_t value, std::size_t* out);
bool dtype_info_from_code(uint8_t code, DtypeInfo* out);
bool is_valid_planar_code(uint8_t code);
bool is_planar_code(uint8_t code);

}  // namespace pixel::core_v2
