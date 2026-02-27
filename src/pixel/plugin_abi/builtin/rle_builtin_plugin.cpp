#include "pixel/plugin_abi/builtin/rle_builtin_plugin.hpp"

#include "pixel/bridge/codec_plugin_abi_adapter.hpp"
#include "pixel/plugin_abi/common/codec_plugin_common.hpp"
#include "dicom_endian.h"
#include "pixel/decode/core/decode_codec_impl_detail.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

namespace dicom::pixel::detail {

namespace {

constexpr std::size_t kMaxPluginFrameBytes =
    std::size_t{2} * 1024u * 1024u * 1024u;

struct RleDecoderPluginContext {
  std::uint16_t transfer_syntax_code{DICOMSDL_TRANSFER_SYNTAX_CODE_INVALID};
  bool configured{false};
};

struct RleEncoderPluginContext {
  std::uint16_t transfer_syntax_code{DICOMSDL_TRANSFER_SYNTAX_CODE_INVALID};
  bool configured{false};
};

struct DecoderContextGuard {
  const dicomsdl_decoder_plugin_api_v1* api{nullptr};
  void* context{nullptr};

  ~DecoderContextGuard() {
    if (api && api->destroy && context) {
      api->destroy(context);
    }
  }
};

struct EncoderContextGuard {
  const dicomsdl_encoder_plugin_api_v1* api{nullptr};
  void* context{nullptr};

  ~EncoderContextGuard() {
    if (api && api->destroy && context) {
      api->destroy(context);
    }
  }
};

using plugin_common::checked_u64_to_size_t;
using plugin_common::prepare_plugin_decode_request;
using plugin_common::prepare_plugin_encode_request;
using plugin_common::PreparedDecodeRequest;
using plugin_common::PreparedEncodeRequest;
using plugin_common::set_abi_error;
using plugin_common::set_abi_error_from_codec_error;
using plugin_common::set_abi_ok;
using plugin_common::validate_configure_no_options;
using plugin_common::write_plugin_encoded_output;

[[nodiscard]] bool is_rle_transfer_syntax_code(
    std::uint16_t transfer_syntax_code) noexcept {
  const auto transfer_syntax =
      abi::from_transfer_syntax_code(transfer_syntax_code);
  return transfer_syntax.has_value() && transfer_syntax->is_rle();
}

[[nodiscard]] bool checked_mul_size_t(
    std::size_t lhs, std::size_t rhs, std::size_t& out) noexcept {
  if (lhs == 0 || rhs == 0) {
    out = 0;
    return true;
  }
  if (lhs > std::numeric_limits<std::size_t>::max() / rhs) {
    return false;
  }
  out = lhs * rhs;
  return true;
}

struct RleHeader {
  std::size_t segment_count{0};
  std::array<std::uint32_t, 15> offsets{};
};

template <typename... Args>
[[noreturn]] void throw_rle_decode_error(
    fmt::format_string<Args...> format, Args... args) {
  throw std::runtime_error(fmt::vformat(format, fmt::make_format_args(args...)));
}

[[nodiscard]] RleHeader parse_rle_header(
    std::span<const std::uint8_t> encoded_frame) {
  if (encoded_frame.size() < 64) {
    throw_rle_decode_error("RLE frame is shorter than 64-byte header");
  }

  RleHeader header{};
  header.segment_count = endian::load_le<std::uint32_t>(encoded_frame.data());
  if (header.segment_count == 0 || header.segment_count > header.offsets.size()) {
    throw_rle_decode_error(
        "invalid RLE segment count {}", header.segment_count);
  }

  for (std::size_t i = 0; i < header.segment_count; ++i) {
    header.offsets[i] = endian::load_le<std::uint32_t>(
        encoded_frame.data() + 4 + i * sizeof(std::uint32_t));
  }

  for (std::size_t i = 0; i < header.segment_count; ++i) {
    const auto start = static_cast<std::size_t>(header.offsets[i]);
    if (start < 64 || start >= encoded_frame.size()) {
      throw_rle_decode_error("invalid RLE segment {} offset {}", i, start);
    }
    const auto end = (i + 1 < header.segment_count)
                         ? static_cast<std::size_t>(header.offsets[i + 1])
                         : encoded_frame.size();
    if (end < start || end > encoded_frame.size()) {
      throw_rle_decode_error(
          "invalid RLE segment {} range [{}, {})", i, start, end);
    }
  }

  return header;
}

void decode_rle_packbits_segment(std::size_t segment_index,
    std::span<const std::uint8_t> encoded,
    std::span<std::uint8_t> decoded) {
  std::size_t in = 0;
  std::size_t out = 0;

  while (out < decoded.size()) {
    if (in >= encoded.size()) {
      throw_rle_decode_error(
          "RLE segment {} ended early (decoded={}/{})",
          segment_index, out, decoded.size());
    }

    const auto control = static_cast<std::int8_t>(encoded[in++]);
    if (control >= 0) {
      const auto literal_count = static_cast<std::size_t>(control) + 1;
      if (in + literal_count > encoded.size() ||
          out + literal_count > decoded.size()) {
        throw_rle_decode_error(
            "RLE segment {} literal run out of bounds", segment_index);
      }
      std::memcpy(decoded.data() + out, encoded.data() + in, literal_count);
      in += literal_count;
      out += literal_count;
      continue;
    }

    if (control >= -127) {
      const auto repeat_count = static_cast<std::size_t>(1 - control);
      if (in >= encoded.size() || out + repeat_count > decoded.size()) {
        throw_rle_decode_error(
            "RLE segment {} repeat run out of bounds", segment_index);
      }
      std::memset(decoded.data() + out, encoded[in], repeat_count);
      ++in;
      out += repeat_count;
      continue;
    }

    // control == -128 : no-op
  }
}

[[nodiscard]] std::vector<std::uint8_t> decode_rle_frame_to_planar(
    std::span<const std::uint8_t> encoded_frame, std::size_t rows,
    std::size_t cols, std::size_t samples_per_pixel,
    std::size_t bytes_per_sample) {
  const auto expected_segments = samples_per_pixel * bytes_per_sample;
  if (expected_segments == 0 || expected_segments > 15) {
    throw_rle_decode_error(
        "unsupported RLE segment layout (spp={}, bytes_per_sample={})",
        samples_per_pixel, bytes_per_sample);
  }

  const auto header = parse_rle_header(encoded_frame);
  if (header.segment_count < expected_segments) {
    throw_rle_decode_error(
        "RLE segment count {} is smaller than expected {}",
        header.segment_count, expected_segments);
  }

  const auto pixels_per_plane = rows * cols;
  const auto src_row_bytes = cols * bytes_per_sample;
  const auto src_plane_bytes = src_row_bytes * rows;
  std::vector<std::uint8_t> decoded_planar(src_plane_bytes * samples_per_pixel, 0);
  std::vector<std::uint8_t> decoded_byte_plane(pixels_per_plane, 0);

  for (std::size_t sample = 0; sample < samples_per_pixel; ++sample) {
    auto* plane_base = decoded_planar.data() + sample * src_plane_bytes;
    for (std::size_t byte_plane = 0; byte_plane < bytes_per_sample; ++byte_plane) {
      const auto segment_index = sample * bytes_per_sample + byte_plane;
      const auto segment_start =
          static_cast<std::size_t>(header.offsets[segment_index]);
      const auto segment_end = (segment_index + 1 < header.segment_count)
                                   ? static_cast<std::size_t>(
                                         header.offsets[segment_index + 1])
                                   : encoded_frame.size();
      const auto segment_size = segment_end - segment_start;
      const auto segment_data = encoded_frame.subspan(segment_start, segment_size);

      decode_rle_packbits_segment(
          segment_index, segment_data, std::span<std::uint8_t>(decoded_byte_plane));

      const auto byte_offset = bytes_per_sample - 1 - byte_plane;
      for (std::size_t r = 0; r < rows; ++r) {
        const auto* src_row = decoded_byte_plane.data() + r * cols;
        auto* dst_row = plane_base + r * src_row_bytes + byte_offset;
        for (std::size_t c = 0; c < cols; ++c) {
          dst_row[c * bytes_per_sample] = src_row[c];
        }
      }
    }
  }

  return decoded_planar;
}

bool decode_rle_frame_into(const pixel::PixelDataInfo& info,
    const DecodeValueTransform& value_transform, std::span<std::uint8_t> dst,
    const DecodeStrides& dst_strides, const DecodeOptions& opt,
    CodecError& out_error, std::span<const std::uint8_t> prepared_source) noexcept {
  out_error = CodecError{};
  auto fail = [&](CodecStatusCode code, std::string_view stage,
                  std::string detail) noexcept -> bool {
    set_codec_error(out_error, code, stage, std::move(detail));
    return false;
  };

  try {
    if (!info.has_pixel_data) {
      return fail(CodecStatusCode::invalid_argument, "validate",
          "sv_dtype is unknown");
    }

    if (info.rows <= 0 || info.cols <= 0 || info.samples_per_pixel <= 0) {
      return fail(CodecStatusCode::invalid_argument, "validate",
          "invalid Rows/Columns/SamplesPerPixel");
    }

    const auto samples_per_pixel_value = info.samples_per_pixel;
    if (samples_per_pixel_value != 1 && samples_per_pixel_value != 3 &&
        samples_per_pixel_value != 4) {
      return fail(CodecStatusCode::unsupported, "validate",
          "only SamplesPerPixel=1/3/4 is supported in current RLE path");
    }
    const auto samples_per_pixel = static_cast<std::size_t>(samples_per_pixel_value);
    if (opt.scaled && samples_per_pixel != 1) {
      return fail(CodecStatusCode::invalid_argument, "validate",
          "scaled output supports SamplesPerPixel=1 only");
    }

    const auto src_bytes_per_sample = sv_dtype_bytes(info.sv_dtype);
    if (src_bytes_per_sample == 0) {
      return fail(CodecStatusCode::unsupported, "validate",
          "only sv_dtype=u8/s8/u16/s16/u32/s32/f32/f64 is supported in current RLE path");
    }

    const auto rows = static_cast<std::size_t>(info.rows);
    const auto cols = static_cast<std::size_t>(info.cols);
    const auto dst_bytes_per_sample =
        opt.scaled ? sizeof(float) : src_bytes_per_sample;

    const auto dst_planar = opt.planar_out;
    const std::size_t dst_row_components =
        (dst_planar == Planar::interleaved) ? samples_per_pixel : std::size_t{1};

    std::size_t dst_row_pixels = 0;
    std::size_t dst_min_row_bytes = 0;
    if (!checked_mul_size_t(cols, dst_row_components, dst_row_pixels) ||
        !checked_mul_size_t(dst_row_pixels, dst_bytes_per_sample, dst_min_row_bytes)) {
      return fail(CodecStatusCode::internal_error, "validate",
          "destination row bytes exceed size_t range");
    }
    if (dst_strides.row < dst_min_row_bytes) {
      return fail(CodecStatusCode::invalid_argument, "validate",
          fmt::format("row stride too small (need>={}, got={})",
              dst_min_row_bytes, dst_strides.row));
    }

    std::size_t min_frame_bytes = 0;
    if (!checked_mul_size_t(dst_strides.row, rows, min_frame_bytes)) {
      return fail(CodecStatusCode::internal_error, "validate",
          "destination frame bytes exceed size_t range");
    }
    if (dst_planar == Planar::planar &&
        !checked_mul_size_t(min_frame_bytes, samples_per_pixel, min_frame_bytes)) {
      return fail(CodecStatusCode::internal_error, "validate",
          "destination planar frame bytes exceed size_t range");
    }
    if (dst_strides.frame < min_frame_bytes) {
      return fail(CodecStatusCode::invalid_argument, "validate",
          fmt::format("frame stride too small (need>={}, got={})",
              min_frame_bytes, dst_strides.frame));
    }
    if (dst.size() < dst_strides.frame) {
      return fail(CodecStatusCode::invalid_argument, "validate",
          fmt::format("destination too small (need={}, got={})",
              dst_strides.frame, dst.size()));
    }

    const auto rle_source = prepared_source;
    if (rle_source.empty()) {
      return fail(CodecStatusCode::invalid_argument, "load_frame_source",
          "RLE frame has empty codestream");
    }

    std::vector<std::uint8_t> decoded_planar{};
    try {
      decoded_planar = decode_rle_frame_to_planar(
          rle_source, rows, cols, samples_per_pixel, src_bytes_per_sample);
    } catch (const std::bad_alloc&) {
      return fail(CodecStatusCode::internal_error, "allocate",
          "memory allocation failed");
    } catch (const std::exception& e) {
      return fail(CodecStatusCode::invalid_argument, "decode_frame",
          e.what());
    } catch (...) {
      return fail(CodecStatusCode::backend_error, "decode_frame",
          "non-standard exception");
    }

    std::size_t src_row_bytes = 0;
    if (!checked_mul_size_t(cols, src_bytes_per_sample, src_row_bytes)) {
      return fail(CodecStatusCode::internal_error, "validate",
          "source row bytes exceed size_t range");
    }

    if (opt.scaled) {
      try {
        decode_mono_scaled_into_f32(
            value_transform, info, decoded_planar.data(), dst, dst_strides,
            rows, cols, src_row_bytes);
        return true;
      } catch (const std::bad_alloc&) {
        return fail(CodecStatusCode::internal_error, "allocate",
            "memory allocation failed");
      } catch (const std::exception& e) {
        return fail(CodecStatusCode::invalid_argument, "postprocess",
            e.what());
      } catch (...) {
        return fail(CodecStatusCode::backend_error, "postprocess",
            "scaled decode failed (non-standard exception)");
      }
    }

    const auto transform = select_planar_transform(Planar::planar, dst_planar);
    run_planar_transform_copy(transform, src_bytes_per_sample,
        decoded_planar.data(), dst.data(), rows, cols, samples_per_pixel,
        src_row_bytes, dst_strides.row);
    return true;
  } catch (const std::bad_alloc&) {
    return fail(CodecStatusCode::internal_error, "allocate",
        "memory allocation failed");
  } catch (const std::exception& e) {
    if (out_error.code != CodecStatusCode::ok) {
      return false;
    }
    return fail(CodecStatusCode::backend_error, "decode_frame", e.what());
  } catch (...) {
    if (out_error.code != CodecStatusCode::ok) {
      return false;
    }
    return fail(CodecStatusCode::backend_error, "decode_frame",
        "non-standard exception");
  }
}

struct RleSourceLayout {
  bool source_is_planar{false};
  std::size_t plane_stride{0};
  std::size_t interleaved_pixel_stride{0};
  std::size_t minimum_frame_bytes{0};
};

bool try_resolve_rle_source_layout(std::span<const std::uint8_t> frame_data,
    std::size_t rows, std::size_t cols, std::size_t samples_per_pixel,
    std::size_t bytes_per_sample, Planar source_planar, std::size_t row_stride,
    RleSourceLayout& out_layout, CodecError& out_error) noexcept {
  if (rows == 0 || cols == 0 || samples_per_pixel == 0 || bytes_per_sample == 0) {
    set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
        "rows/cols/samples_per_pixel/bytes_per_sample must be positive");
    return false;
  }
  if (rows > 65535 || cols > 65535) {
    set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
        "rows/cols must be <= 65535 for DICOM pixel data");
    return false;
  }
  if (samples_per_pixel > 16) {
    set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
        "samples_per_pixel is outside practical encoder range");
    return false;
  }
  if (bytes_per_sample > 8) {
    set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
        "bytes_per_sample is outside practical encoder range");
    return false;
  }
  if (row_stride > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
    set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
        "row_stride exceeds 32-bit range");
    return false;
  }

  const bool source_is_planar =
      source_planar == Planar::planar && samples_per_pixel > std::size_t{1};
  std::size_t row_components = cols;
  if (!source_is_planar &&
      !checked_mul_size_t(cols, samples_per_pixel, row_components)) {
    set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
        "row component count exceeds size_t range");
    return false;
  }

  std::size_t row_payload = 0;
  if (!checked_mul_size_t(row_components, bytes_per_sample, row_payload)) {
    set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
        "row payload exceeds size_t range");
    return false;
  }
  if (row_stride < row_payload) {
    set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
        "row_stride is smaller than row payload");
    return false;
  }

  std::size_t plane_stride = 0;
  if (!checked_mul_size_t(row_stride, rows, plane_stride)) {
    set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
        "plane stride exceeds size_t range");
    return false;
  }

  std::size_t minimum_frame_bytes = plane_stride;
  if (source_is_planar &&
      !checked_mul_size_t(plane_stride, samples_per_pixel, minimum_frame_bytes)) {
    set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
        "minimum frame bytes exceeds size_t range");
    return false;
  }
  if (frame_data.size() < minimum_frame_bytes) {
    set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
        "frame_data is shorter than required frame size");
    return false;
  }

  std::size_t interleaved_pixel_stride = 0;
  if (!source_is_planar &&
      !checked_mul_size_t(
          samples_per_pixel, bytes_per_sample, interleaved_pixel_stride)) {
    set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
        "interleaved pixel stride exceeds size_t range");
    return false;
  }

  out_layout.source_is_planar = source_is_planar;
  out_layout.plane_stride = plane_stride;
  out_layout.interleaved_pixel_stride = interleaved_pixel_stride;
  out_layout.minimum_frame_bytes = minimum_frame_bytes;
  return true;
}

[[nodiscard]] std::vector<std::uint8_t> encode_packbits_segment(
    std::span<const std::uint8_t> source) {
  std::vector<std::uint8_t> encoded;
  encoded.reserve(source.size() + source.size() / 128 + 16);

  std::size_t i = 0;
  while (i < source.size()) {
    std::size_t repeat_count = 1;
    while (i + repeat_count < source.size() && repeat_count < 128 &&
           source[i + repeat_count] == source[i]) {
      ++repeat_count;
    }

    if (repeat_count >= 2) {
      const auto control = static_cast<std::int8_t>(
          1 - static_cast<int>(repeat_count));
      encoded.push_back(static_cast<std::uint8_t>(control));
      encoded.push_back(source[i]);
      i += repeat_count;
      continue;
    }

    const std::size_t literal_begin = i;
    std::size_t literal_count = 1;
    ++i;

    while (i < source.size() && literal_count < 128) {
      repeat_count = 1;
      while (i + repeat_count < source.size() && repeat_count < 128 &&
             source[i + repeat_count] == source[i]) {
        ++repeat_count;
      }
      if (repeat_count >= 2) {
        break;
      }
      ++i;
      ++literal_count;
    }

    encoded.push_back(static_cast<std::uint8_t>(literal_count - 1));
    encoded.insert(encoded.end(),
        source.begin() + static_cast<std::ptrdiff_t>(literal_begin),
        source.begin() + static_cast<std::ptrdiff_t>(
            literal_begin + literal_count));
  }

  return encoded;
}

bool try_encode_rle_frame_builtin(std::span<const std::uint8_t> frame_data,
    std::size_t rows, std::size_t cols, std::size_t samples_per_pixel,
    std::size_t bytes_per_sample, Planar source_planar, std::size_t row_stride,
    std::vector<std::uint8_t>& out_encoded, CodecError& out_error) noexcept {
  out_encoded.clear();
  out_error = CodecError{};

  if (samples_per_pixel == 0 || bytes_per_sample == 0 ||
      bytes_per_sample > 15 || samples_per_pixel > (15 / bytes_per_sample)) {
    set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
        "unsupported RLE segment layout");
    return false;
  }

  RleSourceLayout source_layout{};
  if (!try_resolve_rle_source_layout(frame_data, rows, cols, samples_per_pixel,
          bytes_per_sample, source_planar, row_stride, source_layout, out_error)) {
    return false;
  }

  try {
    const std::size_t segment_count = samples_per_pixel * bytes_per_sample;
    std::size_t pixels_per_plane = 0;
    if (!checked_mul_size_t(rows, cols, pixels_per_plane)) {
      set_codec_error(out_error, CodecStatusCode::invalid_argument,
          "validate", "rows*cols exceeds size_t range");
      return false;
    }

    std::vector<std::uint8_t> byte_plane(pixels_per_plane, 0);
    std::vector<std::vector<std::uint8_t>> segments(segment_count);

    const auto* frame_ptr = frame_data.data();
    for (std::size_t sample = 0; sample < samples_per_pixel; ++sample) {
      for (std::size_t byte_plane_index = 0; byte_plane_index < bytes_per_sample;
           ++byte_plane_index) {
        const auto component_byte_index = bytes_per_sample - 1 - byte_plane_index;
        for (std::size_t row = 0; row < rows; ++row) {
          const std::uint8_t* src_row = nullptr;
          if (source_layout.source_is_planar) {
            src_row = frame_ptr + sample * source_layout.plane_stride +
                row * row_stride;
            for (std::size_t col = 0; col < cols; ++col) {
              byte_plane[row * cols + col] =
                  src_row[col * bytes_per_sample + component_byte_index];
            }
          } else {
            src_row = frame_ptr + row * row_stride;
            const std::size_t sample_offset = sample * bytes_per_sample;
            for (std::size_t col = 0; col < cols; ++col) {
              byte_plane[row * cols + col] =
                  src_row[col * source_layout.interleaved_pixel_stride +
                      sample_offset + component_byte_index];
            }
          }
        }

        const std::size_t segment_index =
            sample * bytes_per_sample + byte_plane_index;
        segments[segment_index] = encode_packbits_segment(byte_plane);
      }
    }

    constexpr std::size_t kMaxOffset =
        static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());
    std::size_t encoded_payload_size = 64;
    for (const auto& segment : segments) {
      if (encoded_payload_size > kMaxOffset ||
          segment.size() > kMaxOffset - encoded_payload_size) {
        set_codec_error(out_error, CodecStatusCode::invalid_argument,
            "encode", "RLE frame size exceeds 32-bit range");
        return false;
      }
      encoded_payload_size += segment.size();
    }

    std::vector<std::uint8_t> encoded_frame(64, 0);
    encoded_frame.reserve(encoded_payload_size);
    endian::store_le<std::uint32_t>(
        encoded_frame.data(), static_cast<std::uint32_t>(segment_count));

    std::uint32_t segment_offset = 64;
    for (std::size_t i = 0; i < segment_count; ++i) {
      endian::store_le<std::uint32_t>(
          encoded_frame.data() + 4 + i * sizeof(std::uint32_t), segment_offset);
      const auto segment_size = segments[i].size();
      if (segment_size > static_cast<std::size_t>(
              std::numeric_limits<std::uint32_t>::max() - segment_offset)) {
        set_codec_error(out_error, CodecStatusCode::invalid_argument,
            "encode", "RLE segment offsets exceed 32-bit range");
        return false;
      }
      segment_offset += static_cast<std::uint32_t>(segment_size);
    }

    for (const auto& segment : segments) {
      encoded_frame.insert(encoded_frame.end(), segment.begin(), segment.end());
    }
    out_encoded = std::move(encoded_frame);
    out_error = CodecError{};
    return true;
  } catch (const std::bad_alloc&) {
    set_codec_error(out_error, CodecStatusCode::internal_error, "allocate",
        "memory allocation failed");
  } catch (const std::exception& e) {
    set_codec_error(out_error, CodecStatusCode::backend_error, "encode",
        e.what());
  } catch (...) {
    set_codec_error(out_error, CodecStatusCode::backend_error, "encode",
        "non-standard exception");
  }
  out_encoded.clear();
  return false;
}

void* rle_decoder_create() noexcept {
  return new (std::nothrow) RleDecoderPluginContext{};
}

void rle_decoder_destroy(void* ctx) noexcept {
  delete static_cast<RleDecoderPluginContext*>(ctx);
}

int rle_decoder_configure(void* ctx, std::uint16_t transfer_syntax_code,
    const dicomsdl_codec_option_list_v1* options,
    dicomsdl_codec_error_v1* error) noexcept {
  auto* context = static_cast<RleDecoderPluginContext*>(ctx);
  if (!context) {
    set_abi_error(error, DICOMSDL_CODEC_INTERNAL_ERROR,
        DICOMSDL_CODEC_STAGE_PLUGIN_LOOKUP, "decoder context is null");
    return 0;
  }
  if (!validate_configure_no_options(options, transfer_syntax_code,
          "RLE decoder does not accept options",
          "transfer syntax is not an RLE family syntax",
          is_rle_transfer_syntax_code, error)) {
    return 0;
  }

  context->transfer_syntax_code = transfer_syntax_code;
  context->configured = true;
  set_abi_ok(error);
  return 1;
}

int rle_decoder_decode_frame(void* ctx, const dicomsdl_decoder_request_v1* request,
    dicomsdl_codec_error_v1* error) noexcept {
  auto* context = static_cast<RleDecoderPluginContext*>(ctx);
  if (!context) {
    set_abi_error(error, DICOMSDL_CODEC_INTERNAL_ERROR,
        DICOMSDL_CODEC_STAGE_PLUGIN_LOOKUP, "decoder context is null");
    return 0;
  }
  if (!context->configured) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_PARSE_OPTIONS,
        "decoder configure() must be called before decode_frame()");
    return 0;
  }
  if (!request) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_DECODE_FRAME, "decoder request is null");
    return 0;
  }
  PreparedDecodeRequest prepared{};
  if (!prepare_plugin_decode_request(*request, context->transfer_syntax_code, -1,
          "request transfer syntax is not an RLE family syntax",
          "RLE decoder plugin supports destination dtype == source dtype or f32",
          is_rle_transfer_syntax_code, prepared, error)) {
    return 0;
  }

  CodecError decode_error{};
  prepared.options.decode_mct = request->frame.decode_mct != 0;
  if (!decode_rle_frame_into(prepared.info, prepared.value_transform,
          prepared.destination, prepared.strides, prepared.options, decode_error,
          prepared.source)) {
    set_abi_error_from_codec_error(error, decode_error, "decode_frame",
        "RLE decoder backend failed");
    return 0;
  }

  set_abi_ok(error);
  return 1;
}

void* rle_encoder_create() noexcept {
  return new (std::nothrow) RleEncoderPluginContext{};
}

void rle_encoder_destroy(void* ctx) noexcept {
  delete static_cast<RleEncoderPluginContext*>(ctx);
}

int rle_encoder_configure(void* ctx, std::uint16_t transfer_syntax_code,
    const dicomsdl_codec_option_list_v1* options,
    dicomsdl_codec_error_v1* error) noexcept {
  auto* context = static_cast<RleEncoderPluginContext*>(ctx);
  if (!context) {
    set_abi_error(error, DICOMSDL_CODEC_INTERNAL_ERROR,
        DICOMSDL_CODEC_STAGE_PLUGIN_LOOKUP, "encoder context is null");
    return 0;
  }
  if (!validate_configure_no_options(options, transfer_syntax_code,
          "RLE encoder does not accept options",
          "transfer syntax is not an RLE family syntax",
          is_rle_transfer_syntax_code, error)) {
    return 0;
  }

  context->transfer_syntax_code = transfer_syntax_code;
  context->configured = true;
  set_abi_ok(error);
  return 1;
}

int rle_encoder_encode_frame(void* ctx, const dicomsdl_encoder_request_v1* request,
    dicomsdl_codec_error_v1* error) noexcept {
  auto* context = static_cast<RleEncoderPluginContext*>(ctx);
  if (!context) {
    set_abi_error(error, DICOMSDL_CODEC_INTERNAL_ERROR,
        DICOMSDL_CODEC_STAGE_PLUGIN_LOOKUP, "encoder context is null");
    return 0;
  }
  if (!context->configured) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_PARSE_OPTIONS,
        "encoder configure() must be called before encode_frame()");
    return 0;
  }
  if (!request) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_ENCODE_FRAME, "encoder request is null");
    return 0;
  }
  PreparedEncodeRequest prepared{};
  if (!prepare_plugin_encode_request(*request, context->transfer_syntax_code,
          "request transfer syntax is not an RLE family syntax",
          is_rle_transfer_syntax_code, prepared, error)) {
    return 0;
  }

  std::vector<std::uint8_t> encoded{};
  CodecError encode_error{};
  if (!try_encode_rle_frame_builtin(prepared.source, prepared.rows, prepared.cols,
          prepared.samples_per_pixel, prepared.bytes_per_sample,
          prepared.source_planar, prepared.row_stride, encoded, encode_error)) {
    set_abi_error_from_codec_error(error, encode_error, "encode_frame",
        "RLE encoder backend failed");
    return 0;
  }

  if (!write_plugin_encoded_output(*request, prepared.encoded_capacity, encoded,
          error)) {
    return 0;
  }
  set_abi_ok(error);
  return 1;
}

[[nodiscard]] const dicomsdl_decoder_plugin_api_v1&
rle_decoder_plugin_api_v1() noexcept {
  static const dicomsdl_decoder_plugin_api_v1 api = [] {
    dicomsdl_decoder_plugin_api_v1 value{};
    value.struct_size = sizeof(dicomsdl_decoder_plugin_api_v1);
    value.abi_version = DICOMSDL_DECODER_PLUGIN_ABI_V1;
    value.info.struct_size = sizeof(dicomsdl_decoder_plugin_info_v1);
    value.info.abi_version = DICOMSDL_DECODER_PLUGIN_ABI_V1;
    value.info.plugin_key = "rle";
    value.info.display_name = "RLE Lossless (Builtin ABI)";
    value.create = &rle_decoder_create;
    value.destroy = &rle_decoder_destroy;
    value.configure = &rle_decoder_configure;
    value.decode_frame = &rle_decoder_decode_frame;
    return value;
  }();
  return api;
}

[[nodiscard]] const dicomsdl_encoder_plugin_api_v1&
rle_encoder_plugin_api_v1() noexcept {
  static const dicomsdl_encoder_plugin_api_v1 api = [] {
    dicomsdl_encoder_plugin_api_v1 value{};
    value.struct_size = sizeof(dicomsdl_encoder_plugin_api_v1);
    value.abi_version = DICOMSDL_ENCODER_PLUGIN_ABI_V1;
    value.info.struct_size = sizeof(dicomsdl_encoder_plugin_info_v1);
    value.info.abi_version = DICOMSDL_ENCODER_PLUGIN_ABI_V1;
    value.info.plugin_key = "rle";
    value.info.display_name = "RLE Lossless (Builtin ABI)";
    value.create = &rle_encoder_create;
    value.destroy = &rle_encoder_destroy;
    value.configure = &rle_encoder_configure;
    value.encode_frame = &rle_encoder_encode_frame;
    return value;
  }();
  return api;
}

bool invoke_rle_decoder_via_plugin_api(
    const CodecDecodeFrameInput& input, CodecError& out_error) noexcept {
  out_error = {};

  dicomsdl_decoder_request_v1 request{};
  abi::build_decoder_request_v1(input, request);

  if (request.frame.transfer_syntax_code == DICOMSDL_TRANSFER_SYNTAX_CODE_INVALID) {
    set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
        "invalid transfer syntax code for decoder ABI request");
    return false;
  }
  if (request.frame.source_dtype == DICOMSDL_DTYPE_UNKNOWN &&
      input.info.sv_dtype != DataType::unknown) {
    set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
        "source dtype is not representable in decoder ABI request");
    return false;
  }

  const auto& api = rle_decoder_plugin_api_v1();
  DecoderContextGuard guard{};
  guard.api = &api;
  guard.context = api.create ? api.create() : nullptr;
  if (guard.context == nullptr) {
    set_codec_error(out_error, CodecStatusCode::internal_error,
        "plugin_lookup", "RLE decoder plugin create() returned null context");
    return false;
  }

  char detail_buffer[1024];
  dicomsdl_codec_error_v1 plugin_error{};
  abi::initialize_codec_error_buffer(plugin_error, detail_buffer,
      static_cast<std::uint32_t>(sizeof(detail_buffer)));

  if (!api.configure(guard.context, request.frame.transfer_syntax_code,
          nullptr, &plugin_error)) {
    out_error = abi::decode_plugin_error_v1(
        plugin_error, "parse_options", "RLE decoder configure failed");
    if (out_error.code == CodecStatusCode::ok) {
      out_error.code = CodecStatusCode::backend_error;
    }
    return false;
  }

  abi::initialize_codec_error_buffer(plugin_error, detail_buffer,
      static_cast<std::uint32_t>(sizeof(detail_buffer)));
  if (!api.decode_frame(guard.context, &request, &plugin_error)) {
    out_error = abi::decode_plugin_error_v1(
        plugin_error, "decode_frame", "RLE decoder failed");
    if (out_error.code == CodecStatusCode::ok) {
      out_error.code = CodecStatusCode::backend_error;
    }
    return false;
  }
  return true;
}

}  // namespace

const dicomsdl_decoder_plugin_api_v1&
rle_decoder_plugin_api_for_shared() noexcept {
  return rle_decoder_plugin_api_v1();
}

const dicomsdl_encoder_plugin_api_v1&
rle_encoder_plugin_api_for_shared() noexcept {
  return rle_encoder_plugin_api_v1();
}

bool decode_frame_plugin_rle_via_abi(
    const CodecDecodeFrameInput& input, CodecError& out_error) noexcept {
  try {
    if (!input.info.has_pixel_data) {
      set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
          "sv_dtype is unknown");
      return false;
    }
    return invoke_rle_decoder_via_plugin_api(input, out_error);
  } catch (const std::exception& e) {
    set_codec_error(out_error, CodecStatusCode::backend_error, "decode_frame",
        e.what());
  } catch (...) {
    set_codec_error(out_error, CodecStatusCode::backend_error, "decode_frame",
        "non-standard exception");
  }
  return false;
}

bool encode_frame_plugin_rle_via_abi(const CodecEncodeFrameInput& input,
    std::span<const CodecOptionKv> encode_options,
    std::vector<std::uint8_t>& out_encoded_frame,
    CodecError& out_error) noexcept {
  out_encoded_frame.clear();
  out_error = {};
  (void)encode_options;

  if (input.transfer_syntax.uid_type() != UidType::TransferSyntax) {
    set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
        "invalid transfer syntax");
    return false;
  }

  const auto transfer_syntax_code = abi::to_transfer_syntax_code(input.transfer_syntax);
  if (transfer_syntax_code == DICOMSDL_TRANSFER_SYNTAX_CODE_INVALID) {
    set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
        "invalid transfer syntax code for encoder ABI request");
    return false;
  }

  const auto& api = rle_encoder_plugin_api_v1();
  EncoderContextGuard guard{};
  guard.api = &api;
  guard.context = api.create ? api.create() : nullptr;
  if (guard.context == nullptr) {
    set_codec_error(out_error, CodecStatusCode::internal_error, "plugin_lookup",
        "RLE encoder plugin create() returned null context");
    return false;
  }

  char detail_buffer[1024];
  dicomsdl_codec_error_v1 plugin_error{};
  abi::initialize_codec_error_buffer(plugin_error, detail_buffer,
      static_cast<std::uint32_t>(sizeof(detail_buffer)));
  if (!api.configure(guard.context, transfer_syntax_code, nullptr, &plugin_error)) {
    out_error = abi::decode_plugin_error_v1(
        plugin_error, "parse_options", "RLE encoder configure failed");
    if (out_error.code == CodecStatusCode::ok) {
      out_error.code = CodecStatusCode::backend_error;
    }
    return false;
  }

  const std::size_t minimum_capacity = std::max<std::size_t>(
      input.destination_frame_payload, std::size_t{4096});
  if (minimum_capacity > kMaxPluginFrameBytes) {
    set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
        "initial output capacity exceeds max frame bytes");
    return false;
  }

  out_encoded_frame.resize(minimum_capacity);
  for (int attempt = 0; attempt < 4; ++attempt) {
    dicomsdl_encoder_request_v1 request{};
    abi::build_encoder_request_v1(input, std::span<std::uint8_t>(out_encoded_frame),
        static_cast<std::uint64_t>(out_encoded_frame.size()), request);

    abi::initialize_codec_error_buffer(plugin_error, detail_buffer,
        static_cast<std::uint32_t>(sizeof(detail_buffer)));
    if (api.encode_frame(guard.context, &request, &plugin_error)) {
      if (request.output.encoded_size >
          static_cast<std::uint64_t>(out_encoded_frame.size())) {
        set_codec_error(out_error, CodecStatusCode::internal_error,
            "encode_frame",
            "RLE encoder returned encoded_size larger than output buffer");
        out_encoded_frame.clear();
        return false;
      }
      out_encoded_frame.resize(
          static_cast<std::size_t>(request.output.encoded_size));
      return true;
    }

    if (plugin_error.status_code != DICOMSDL_CODEC_OUTPUT_TOO_SMALL) {
      out_error = abi::decode_plugin_error_v1(
          plugin_error, "encode_frame", "RLE encoder failed");
      if (out_error.code == CodecStatusCode::ok) {
        out_error.code = CodecStatusCode::backend_error;
      }
      out_encoded_frame.clear();
      return false;
    }

    std::size_t required_size = 0;
    if (!checked_u64_to_size_t(request.output.encoded_size, required_size)) {
      set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
          "requested output size exceeds host size_t range");
      out_encoded_frame.clear();
      return false;
    }
    if (required_size <= out_encoded_frame.size()) {
      if (out_encoded_frame.size() >
          (std::numeric_limits<std::size_t>::max)() / 2) {
        required_size = (std::numeric_limits<std::size_t>::max)();
      } else {
        required_size = out_encoded_frame.size() * 2;
      }
    }
    if (required_size == 0) {
      required_size = minimum_capacity;
    }
    if (required_size > kMaxPluginFrameBytes) {
      set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
          "requested output size exceeds max frame bytes");
      out_encoded_frame.clear();
      return false;
    }
    out_encoded_frame.resize(required_size);
  }

  set_codec_error(out_error, CodecStatusCode::backend_error, "encode_frame",
      "RLE encoder repeatedly returned OUTPUT_TOO_SMALL");
  out_encoded_frame.clear();
  return false;
}

}  // namespace dicom::pixel::detail
