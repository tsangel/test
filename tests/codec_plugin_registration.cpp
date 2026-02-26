#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include <dicom.h>
#include "../src/pixel_codec_registry.hpp"

namespace {
using namespace dicom::literals;

[[noreturn]] void fail(const std::string& message) {
  std::cerr << message << std::endl;
  std::exit(1);
}

void expect_true(bool value, std::string_view label) {
  if (!value) {
    fail(std::string(label) + " expected true");
  }
}

void expect_false(bool value, std::string_view label) {
  if (value) {
    fail(std::string(label) + " expected false");
  }
}

template <typename T, typename U>
void expect_eq(const T& actual, const U& expected, std::string_view label) {
  if (!(actual == expected)) {
    fail(std::string(label) + " mismatch");
  }
}

void expect_contains(
    std::string_view actual, std::string_view needle, std::string_view label) {
  if (actual.find(needle) == std::string_view::npos) {
    fail(std::string(label) + " missing expected substring");
  }
}

template <typename Fn>
void expect_no_throw(Fn&& fn, std::string_view label) {
  try {
    fn();
  } catch (const std::exception& e) {
    fail(std::string(label) + " threw exception: " + e.what());
  } catch (...) {
    fail(std::string(label) + " threw non-standard exception");
  }
}

void append_u16_le(std::vector<std::uint8_t>& out, std::uint16_t value) {
  out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
  out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
}

void append_u32_le(std::vector<std::uint8_t>& out, std::uint32_t value) {
  out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
  out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
  out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFu));
  out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFFu));
}

void append_bytes(
    std::vector<std::uint8_t>& out, const std::vector<std::uint8_t>& value) {
  out.insert(out.end(), value.begin(), value.end());
}

void append_explicit_vr_le_16(std::vector<std::uint8_t>& out, dicom::Tag tag,
    char vr0, char vr1, const std::vector<std::uint8_t>& value) {
  if (value.size() > 0xFFFFu) {
    fail("append_explicit_vr_le_16 value too large");
  }
  append_u16_le(out, tag.group());
  append_u16_le(out, tag.element());
  out.push_back(static_cast<std::uint8_t>(vr0));
  out.push_back(static_cast<std::uint8_t>(vr1));
  append_u16_le(out, static_cast<std::uint16_t>(value.size()));
  append_bytes(out, value);
}

void append_explicit_vr_le_32(std::vector<std::uint8_t>& out, dicom::Tag tag,
    char vr0, char vr1, const std::vector<std::uint8_t>& value,
    bool undefined_length = false) {
  append_u16_le(out, tag.group());
  append_u16_le(out, tag.element());
  out.push_back(static_cast<std::uint8_t>(vr0));
  out.push_back(static_cast<std::uint8_t>(vr1));
  append_u16_le(out, 0);
  const auto value_length =
      undefined_length ? 0xFFFFFFFFu : static_cast<std::uint32_t>(value.size());
  append_u32_le(out, value_length);
  append_bytes(out, value);
}

std::vector<std::uint8_t> ui_value(std::string uid) {
  if (uid.empty() || uid.back() != '\0') {
    uid.push_back('\0');
  }
  if ((uid.size() & 1u) != 0u) {
    uid.push_back('\0');
  }
  return std::vector<std::uint8_t>(uid.begin(), uid.end());
}

std::vector<std::uint8_t> cs_value(std::string text) {
  if ((text.size() & 1u) != 0u) {
    text.push_back(' ');
  }
  return std::vector<std::uint8_t>(text.begin(), text.end());
}

std::vector<std::uint8_t> is_value(std::string text) {
  if ((text.size() & 1u) != 0u) {
    text.push_back(' ');
  }
  return std::vector<std::uint8_t>(text.begin(), text.end());
}

std::vector<std::uint8_t> us_value(std::uint16_t value) {
  std::vector<std::uint8_t> out{};
  append_u16_le(out, value);
  return out;
}

std::vector<std::uint8_t> build_part10(
    std::string transfer_syntax_uid, const std::vector<std::uint8_t>& dataset_body) {
  std::vector<std::uint8_t> meta_ts{};
  append_explicit_vr_le_16(
      meta_ts, dicom::Tag(0x0002u, 0x0010u), 'U', 'I',
      ui_value(std::move(transfer_syntax_uid)));

  std::vector<std::uint8_t> meta_gl_value{};
  append_u32_le(meta_gl_value, static_cast<std::uint32_t>(meta_ts.size()));

  std::vector<std::uint8_t> meta_gl{};
  append_explicit_vr_le_16(
      meta_gl, dicom::Tag(0x0002u, 0x0000u), 'U', 'L', meta_gl_value);

  std::vector<std::uint8_t> out(128, 0);
  out.insert(out.end(), {'D', 'I', 'C', 'M'});
  append_bytes(out, meta_gl);
  append_bytes(out, meta_ts);
  append_bytes(out, dataset_body);
  return out;
}

std::vector<std::uint8_t> build_jpeg_encapsulated_single_frame_body(
    const std::vector<std::vector<std::uint8_t>>& frame_fragments) {
  if (frame_fragments.empty()) {
    fail("build_jpeg_encapsulated_single_frame_body requires at least one fragment");
  }

  std::vector<std::uint8_t> body{};
  append_explicit_vr_le_16(
      body, dicom::Tag(0x0028u, 0x0002u), 'U', 'S', us_value(1));
  append_explicit_vr_le_16(
      body, dicom::Tag(0x0028u, 0x0004u), 'C', 'S', cs_value("MONOCHROME2"));
  append_explicit_vr_le_16(
      body, dicom::Tag(0x0028u, 0x0010u), 'U', 'S', us_value(1));
  append_explicit_vr_le_16(
      body, dicom::Tag(0x0028u, 0x0011u), 'U', 'S', us_value(1));
  append_explicit_vr_le_16(
      body, dicom::Tag(0x0028u, 0x0100u), 'U', 'S', us_value(8));
  append_explicit_vr_le_16(
      body, dicom::Tag(0x0028u, 0x0101u), 'U', 'S', us_value(8));
  append_explicit_vr_le_16(
      body, dicom::Tag(0x0028u, 0x0102u), 'U', 'S', us_value(7));
  append_explicit_vr_le_16(
      body, dicom::Tag(0x0028u, 0x0103u), 'U', 'S', us_value(0));
  append_explicit_vr_le_16(
      body, dicom::Tag(0x0028u, 0x0008u), 'I', 'S', is_value("1"));

  std::vector<std::uint8_t> encapsulated_pixel_value{};
  append_u16_le(encapsulated_pixel_value, 0xFFFEu);
  append_u16_le(encapsulated_pixel_value, 0xE000u);
  append_u32_le(encapsulated_pixel_value, 0u);
  for (const auto& fragment : frame_fragments) {
    append_u16_le(encapsulated_pixel_value, 0xFFFEu);
    append_u16_le(encapsulated_pixel_value, 0xE000u);
    std::uint32_t fragment_length = static_cast<std::uint32_t>(fragment.size());
    if ((fragment_length & 1u) != 0u) {
      ++fragment_length;
    }
    append_u32_le(encapsulated_pixel_value, fragment_length);
    append_bytes(encapsulated_pixel_value, fragment);
    if ((fragment.size() & 1u) != 0u) {
      encapsulated_pixel_value.push_back(0x00u);
    }
  }
  append_u16_le(encapsulated_pixel_value, 0xFFFEu);
  append_u16_le(encapsulated_pixel_value, 0xE0DDu);
  append_u32_le(encapsulated_pixel_value, 0u);

  append_explicit_vr_le_32(
      body, dicom::Tag(0x7FE0u, 0x0010u), 'O', 'B',
      encapsulated_pixel_value, true);
  return body;
}

int g_stub_decoder_decode_call_count = 0;
std::vector<std::uint8_t> g_stub_decoder_last_source{};
bool g_stub_decoder_force_configure_failure = false;

void set_abi_error(dicomsdl_codec_error_v1* error, uint32_t status_code,
    uint32_t stage_code, const char* detail) {
  if (!error) {
    return;
  }
  error->status_code = status_code;
  error->stage_code = stage_code;
  if (!error->detail || error->detail_capacity == 0 || !detail) {
    error->detail_length = 0;
    return;
  }
  const std::size_t detail_length = std::strlen(detail);
  const std::size_t copy_length =
      std::min<std::size_t>(detail_length, error->detail_capacity - 1);
  std::memcpy(error->detail, detail, copy_length);
  error->detail[copy_length] = '\0';
  error->detail_length = static_cast<uint32_t>(copy_length);
}

struct StubDecoderContext {
  uint16_t last_ts{0};
  uint32_t configure_count{0};
};

void* stub_decoder_create() {
  return new StubDecoderContext{};
}

void stub_decoder_destroy(void* ctx) {
  delete static_cast<StubDecoderContext*>(ctx);
}

int stub_decoder_configure(void* ctx, uint16_t transfer_syntax_code,
    const dicomsdl_codec_option_list_v1* options, dicomsdl_codec_error_v1* error) {
  auto* context = static_cast<StubDecoderContext*>(ctx);
  if (!context) {
    set_abi_error(error, DICOMSDL_CODEC_INTERNAL_ERROR,
        DICOMSDL_CODEC_STAGE_PLUGIN_LOOKUP, "decoder ctx is null");
    return 0;
  }
  context->last_ts = transfer_syntax_code;
  context->configure_count += 1;
  if (g_stub_decoder_force_configure_failure) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_PARSE_OPTIONS, "decoder configure forced failure");
    return 0;
  }
  if (options && options->count > 0) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_PARSE_OPTIONS, "decoder options are not supported");
    return 0;
  }
  set_abi_error(error, DICOMSDL_CODEC_OK, DICOMSDL_CODEC_STAGE_UNKNOWN, "");
  return 1;
}

int stub_decoder_decode_frame(void* ctx, const dicomsdl_decoder_request_v1* request,
    dicomsdl_codec_error_v1* error) {
  auto* context = static_cast<StubDecoderContext*>(ctx);
  if (!context || !request) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_DECODE_FRAME, "decoder request is null");
    return 0;
  }
  if (request->output.dst == nullptr || request->output.dst_size == 0) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_VALIDATE, "decoder destination is empty");
    return 0;
  }
  ++g_stub_decoder_decode_call_count;
  g_stub_decoder_last_source.clear();
  if (request->source.source_buffer.data &&
      request->source.source_buffer.size > 0) {
    const auto source_size =
        static_cast<std::size_t>(request->source.source_buffer.size);
    g_stub_decoder_last_source.assign(
        request->source.source_buffer.data,
        request->source.source_buffer.data + source_size);
  }
  request->output.dst[0] = static_cast<uint8_t>(0x7f);
  set_abi_error(error, DICOMSDL_CODEC_OK, DICOMSDL_CODEC_STAGE_UNKNOWN, "");
  return 1;
}

struct StubEncoderContext {
  uint16_t last_ts{0};
  uint32_t configure_count{0};
};

int g_stub_encoder_encode_call_count = 0;
bool g_stub_encoder_force_output_too_small_once = false;
std::string g_option_limit_long_key(129, 'k');
std::string g_option_limit_long_value(1025, 'v');

enum class OptionLimitMode {
  normal = 0,
  too_many_options,
  key_too_long,
  empty_key,
  value_too_long,
};

OptionLimitMode g_option_limit_mode = OptionLimitMode::normal;

void* stub_encoder_create() {
  return new StubEncoderContext{};
}

void stub_encoder_destroy(void* ctx) {
  delete static_cast<StubEncoderContext*>(ctx);
}

int stub_encoder_configure(void* ctx, uint16_t transfer_syntax_code,
    const dicomsdl_codec_option_list_v1* options, dicomsdl_codec_error_v1* error) {
  auto* context = static_cast<StubEncoderContext*>(ctx);
  if (!context) {
    set_abi_error(error, DICOMSDL_CODEC_INTERNAL_ERROR,
        DICOMSDL_CODEC_STAGE_PLUGIN_LOOKUP, "encoder ctx is null");
    return 0;
  }
  context->last_ts = transfer_syntax_code;
  context->configure_count += 1;
  if (options == nullptr || options->count == 0) {
    set_abi_error(error, DICOMSDL_CODEC_OK, DICOMSDL_CODEC_STAGE_UNKNOWN, "");
    return 1;
  }
  // Accept known JPEG quality option only.
  for (uint32_t i = 0; i < options->count; ++i) {
    const auto& item = options->items[i];
    if (!item.key || !item.value) {
      set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
          DICOMSDL_CODEC_STAGE_PARSE_OPTIONS, "encoder option key/value is null");
      return 0;
    }
    if (std::string_view(item.key) != "quality") {
      set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
          DICOMSDL_CODEC_STAGE_PARSE_OPTIONS, "unknown encoder option");
      return 0;
    }
    if (std::string_view(item.value) == "13") {
      set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
          DICOMSDL_CODEC_STAGE_PARSE_OPTIONS, "quality 13 is rejected for test");
      return 0;
    }
  }
  set_abi_error(error, DICOMSDL_CODEC_OK, DICOMSDL_CODEC_STAGE_UNKNOWN, "");
  return 1;
}

int stub_encoder_encode_frame(void* ctx, const dicomsdl_encoder_request_v1* request,
    dicomsdl_codec_error_v1* error) {
  if (!ctx) {
    set_abi_error(error, DICOMSDL_CODEC_INTERNAL_ERROR,
        DICOMSDL_CODEC_STAGE_ENCODE_FRAME, "encoder ctx is null");
    return 0;
  }
  if (!request) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_ENCODE_FRAME, "encoder request is null");
    return 0;
  }
  ++g_stub_encoder_encode_call_count;
  if (g_stub_encoder_force_output_too_small_once) {
    g_stub_encoder_force_output_too_small_once = false;
    const_cast<dicomsdl_encoder_request_v1*>(request)->output.encoded_size =
        request->output.encoded_buffer.size + 1;
    set_abi_error(error, DICOMSDL_CODEC_OUTPUT_TOO_SMALL,
        DICOMSDL_CODEC_STAGE_ENCODE_FRAME, "forced retry for test");
    return 0;
  }
  constexpr uint64_t kEncodedSize = 3;
  if (!request->output.encoded_buffer.data ||
      request->output.encoded_buffer.size < kEncodedSize) {
    if (request) {
      const_cast<dicomsdl_encoder_request_v1*>(request)->output.encoded_size =
          kEncodedSize;
    }
    set_abi_error(error, DICOMSDL_CODEC_OUTPUT_TOO_SMALL,
        DICOMSDL_CODEC_STAGE_ENCODE_FRAME, "encoded buffer too small");
    return 0;
  }
  request->output.encoded_buffer.data[0] = 0xaa;
  request->output.encoded_buffer.data[1] = 0xbb;
  request->output.encoded_buffer.data[2] = 0xcc;
  const_cast<dicomsdl_encoder_request_v1*>(request)->output.encoded_size =
      kEncodedSize;
  set_abi_error(error, DICOMSDL_CODEC_OK, DICOMSDL_CODEC_STAGE_UNKNOWN, "");
  return 1;
}

void* stub_option_limit_encoder_create() {
  return new std::uint8_t{0};
}

void stub_option_limit_encoder_destroy(void* ctx) {
  delete static_cast<std::uint8_t*>(ctx);
}

int stub_option_limit_encoder_configure(void* ctx, uint16_t transfer_syntax_code,
    const dicomsdl_codec_option_list_v1* options, dicomsdl_codec_error_v1* error) {
  (void)transfer_syntax_code;
  if (!ctx) {
    set_abi_error(error, DICOMSDL_CODEC_INTERNAL_ERROR,
        DICOMSDL_CODEC_STAGE_PLUGIN_LOOKUP, "option-limit encoder ctx is null");
    return 0;
  }
  if (options != nullptr && options->count > 0 && options->items == nullptr) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_PARSE_OPTIONS, "option-limit encoder options are malformed");
    return 0;
  }
  set_abi_error(error, DICOMSDL_CODEC_OK, DICOMSDL_CODEC_STAGE_UNKNOWN, "");
  return 1;
}

int stub_option_limit_encoder_encode_frame(void* ctx,
    const dicomsdl_encoder_request_v1* request, dicomsdl_codec_error_v1* error) {
  (void)ctx;
  if (!request || !request->output.encoded_buffer.data ||
      request->output.encoded_buffer.size == 0) {
    set_abi_error(error, DICOMSDL_CODEC_INVALID_ARGUMENT,
        DICOMSDL_CODEC_STAGE_ENCODE_FRAME, "option-limit encoder output buffer is empty");
    return 0;
  }
  request->output.encoded_buffer.data[0] = 0x42;
  const_cast<dicomsdl_encoder_request_v1*>(request)->output.encoded_size = 1;
  set_abi_error(error, DICOMSDL_CODEC_OK, DICOMSDL_CODEC_STAGE_UNKNOWN, "");
  return 1;
}

std::optional<std::string> export_option_limit_options(
    dicom::uid::WellKnown transfer_syntax,
    dicom::pixel::detail::codec_option_pairs& out_pairs) {
  (void)transfer_syntax;
  out_pairs.clear();
  switch (g_option_limit_mode) {
  case OptionLimitMode::normal:
    out_pairs.push_back(dicom::pixel::detail::CodecOptionKv{
        .key = "quality",
        .value = dicom::pixel::detail::codec_option_value{
            static_cast<std::int64_t>(90)},
    });
    return std::nullopt;
  case OptionLimitMode::too_many_options:
    for (int i = 0; i < 65; ++i) {
      out_pairs.push_back(dicom::pixel::detail::CodecOptionKv{
          .key = "quality",
          .value = dicom::pixel::detail::codec_option_value{
              static_cast<std::int64_t>(90)},
      });
    }
    return std::nullopt;
  case OptionLimitMode::key_too_long:
    out_pairs.push_back(dicom::pixel::detail::CodecOptionKv{
        .key = g_option_limit_long_key,
        .value = dicom::pixel::detail::codec_option_value{
            static_cast<std::int64_t>(90)},
    });
    return std::nullopt;
  case OptionLimitMode::empty_key:
    out_pairs.push_back(dicom::pixel::detail::CodecOptionKv{
        .key = "",
        .value = dicom::pixel::detail::codec_option_value{
            static_cast<std::int64_t>(90)},
    });
    return std::nullopt;
  case OptionLimitMode::value_too_long:
    out_pairs.push_back(dicom::pixel::detail::CodecOptionKv{
        .key = "quality",
        .value = dicom::pixel::detail::codec_option_value{
            g_option_limit_long_value},
    });
    return std::nullopt;
  }
  return std::nullopt;
}

dicomsdl_decoder_plugin_api_v1 make_stub_decoder_api() {
  dicomsdl_decoder_plugin_api_v1 api{};
  api.struct_size = sizeof(dicomsdl_decoder_plugin_api_v1);
  api.abi_version = DICOMSDL_DECODER_PLUGIN_ABI_V1;
  api.info.struct_size = sizeof(dicomsdl_decoder_plugin_info_v1);
  api.info.abi_version = DICOMSDL_DECODER_PLUGIN_ABI_V1;
  api.info.plugin_key = "jpeg";
  api.info.display_name = "Stub JPEG Decoder";
  api.create = &stub_decoder_create;
  api.destroy = &stub_decoder_destroy;
  api.configure = &stub_decoder_configure;
  api.decode_frame = &stub_decoder_decode_frame;
  return api;
}

dicomsdl_encoder_plugin_api_v1 make_stub_encoder_api() {
  dicomsdl_encoder_plugin_api_v1 api{};
  api.struct_size = sizeof(dicomsdl_encoder_plugin_api_v1);
  api.abi_version = DICOMSDL_ENCODER_PLUGIN_ABI_V1;
  api.info.struct_size = sizeof(dicomsdl_encoder_plugin_info_v1);
  api.info.abi_version = DICOMSDL_ENCODER_PLUGIN_ABI_V1;
  api.info.plugin_key = "jpeg";
  api.info.display_name = "Stub JPEG Encoder";
  api.create = &stub_encoder_create;
  api.destroy = &stub_encoder_destroy;
  api.configure = &stub_encoder_configure;
  api.encode_frame = &stub_encoder_encode_frame;
  return api;
}

dicomsdl_encoder_plugin_api_v1 make_option_limit_encoder_api() {
  dicomsdl_encoder_plugin_api_v1 api{};
  api.struct_size = sizeof(dicomsdl_encoder_plugin_api_v1);
  api.abi_version = DICOMSDL_ENCODER_PLUGIN_ABI_V1;
  api.info.struct_size = sizeof(dicomsdl_encoder_plugin_info_v1);
  api.info.abi_version = DICOMSDL_ENCODER_PLUGIN_ABI_V1;
  api.info.plugin_key = "optlimit";
  api.info.display_name = "Option Limit Stub Encoder";
  api.create = &stub_option_limit_encoder_create;
  api.destroy = &stub_option_limit_encoder_destroy;
  api.configure = &stub_option_limit_encoder_configure;
  api.encode_frame = &stub_option_limit_encoder_encode_frame;
  return api;
}

}  // namespace

int main() {
  using dicom::pixel::detail::CodecDecodeFrameInput;
  using dicom::pixel::detail::CodecEncodeFrameInput;
  using dicom::pixel::detail::CodecOptionSchema;
  using dicom::pixel::detail::CodecPlugin;
  using dicom::pixel::detail::CodecError;
  using dicom::pixel::detail::CodecProfile;
  using dicom::pixel::detail::CodecStatusCode;
  using dicom::pixel::detail::TransferSyntaxPluginBinding;
  using dicom::pixel::detail::global_codec_registry;

  auto& registry = global_codec_registry();
  const auto* jpeg_plugin = registry.find_plugin("jpeg");
  if (!jpeg_plugin) {
    fail("jpeg plugin is not registered");
  }
  const auto original_decode = jpeg_plugin->decode_frame;
  const auto original_encode = jpeg_plugin->encode_frame;

  auto invalid_decoder_api = make_stub_decoder_api();
  invalid_decoder_api.abi_version = 0;
  invalid_decoder_api.info.abi_version = 0;
  std::string error{};
  expect_false(dicom::pixel::register_external_decoder_plugin_static(
                   &invalid_decoder_api, &error),
      "register invalid decoder abi version");
  expect_contains(error, "ABI version mismatch",
      "register invalid decoder abi error");

  auto invalid_encoder_api = make_stub_encoder_api();
  invalid_encoder_api.abi_version = 0;
  invalid_encoder_api.info.abi_version = 0;
  error.clear();
  expect_false(dicom::pixel::register_external_encoder_plugin_static(
                   &invalid_encoder_api, &error),
      "register invalid encoder abi version");
  expect_contains(error, "ABI version mismatch",
      "register invalid encoder abi error");

  auto decoder_api = make_stub_decoder_api();
  auto encoder_api = make_stub_encoder_api();
  error.clear();
  expect_true(dicom::pixel::register_external_decoder_plugin_static(
                  &decoder_api, &error),
      "register external decoder static");
  expect_true(dicom::pixel::register_external_encoder_plugin_static(
                  &encoder_api, &error),
      "register external encoder static");

  if (!registry.find_plugin("optlimit")) {
    expect_true(registry.register_plugin(CodecPlugin{
                    .key = "optlimit",
                    .display_name = "Option Limit Test Plugin",
                    .option_schema = std::span<const CodecOptionSchema>{},
                    .default_options = nullptr,
                    .encode_frame = nullptr,
                    .decode_frame = nullptr,
                }),
        "register option limit plugin");
  }

  dicom::uid::WellKnown option_limit_ts{};
  constexpr std::array<dicom::uid::WellKnown, 4> kOptionLimitTsCandidates{
      "MPEG2MPML"_uid,
      "JPIPReferenced"_uid,
      "MPEG4HP41"_uid,
      "HEVCMP51"_uid,
  };
  for (const auto candidate : kOptionLimitTsCandidates) {
    const auto* existing_binding = registry.find_binding(candidate);
    if (existing_binding) {
      if (existing_binding->plugin_key == "optlimit") {
        option_limit_ts = candidate;
        break;
      }
      continue;
    }
    if (registry.register_binding(TransferSyntaxPluginBinding{
            .transfer_syntax = candidate,
            .plugin_key = "optlimit",
            .plugin_index = 0,
            .profile = CodecProfile::unknown,
            .encode_supported = true,
            .decode_supported = false,
        })) {
      option_limit_ts = candidate;
      break;
    }
  }
  expect_true(option_limit_ts.valid(),
      "register option limit transfer syntax binding");

  auto option_limit_api = make_option_limit_encoder_api();
  error.clear();
  expect_true(dicom::pixel::register_external_encoder_plugin_static(
                  &option_limit_api, &error),
      "register option limit encoder static");
  const auto* option_limit_plugin = registry.find_plugin("optlimit");
  expect_true(option_limit_plugin != nullptr, "option limit plugin exists");
  expect_true(option_limit_plugin->encode_frame != nullptr,
      "option limit plugin encode dispatch available");

  jpeg_plugin = registry.find_plugin("jpeg");
  expect_true(jpeg_plugin != nullptr, "jpeg plugin exists after register");
  expect_true(jpeg_plugin->decode_frame != original_decode,
      "decode dispatch overridden");
  expect_true(jpeg_plugin->encode_frame != original_encode,
      "encode dispatch overridden");

  std::vector<std::uint8_t> decode_src{0x00, 0x01};
  std::vector<std::uint8_t> decode_dst(8, 0);
  CodecDecodeFrameInput decode_input{
      .info = dicom::pixel::PixelDataInfo{
          .ts = "JPEGBaseline8Bit"_uid,
          .sv_dtype = dicom::pixel::DataType::u8,
          .rows = 1,
          .cols = 1,
          .frames = 1,
          .samples_per_pixel = 1,
          .planar_configuration = dicom::pixel::Planar::interleaved,
          .bits_stored = 8,
          .has_pixel_data = true,
      },
      .prepared_source = std::span<const std::uint8_t>(decode_src),
      .destination = std::span<std::uint8_t>(decode_dst),
      .destination_strides = dicom::pixel::DecodeStrides{.row = 1, .frame = 1},
      .options = dicom::pixel::DecodeOptions{},
  };
  CodecError decode_error{};
  expect_true(jpeg_plugin->decode_frame(decode_input, decode_error),
      "external decoder dispatch call");
  expect_eq(decode_dst[0], static_cast<std::uint8_t>(0x7f),
      "external decoder output marker");

  const std::vector<std::uint8_t> contiguous_fragment{0x11, 0x22, 0x33, 0x44};
  const auto contiguous_file_bytes = build_part10(
      "1.2.840.10008.1.2.4.50",
      build_jpeg_encapsulated_single_frame_body(
          std::vector<std::vector<std::uint8_t>>{contiguous_fragment}));
  auto contiguous_file = dicom::read_bytes(
      "plugin-contiguous", contiguous_file_bytes.data(), contiguous_file_bytes.size());
  expect_true(contiguous_file != nullptr, "read contiguous test file");
  auto* contiguous_pixel_data = contiguous_file->get_dataelement("PixelData"_tag);
  expect_true(contiguous_pixel_data != nullptr && *contiguous_pixel_data,
      "contiguous PixelData exists");
  auto* contiguous_sequence = contiguous_pixel_data->as_pixel_sequence();
  expect_true(contiguous_sequence != nullptr, "contiguous PixelData is sequence");
  auto* contiguous_frame = contiguous_sequence->frame(0);
  expect_true(contiguous_frame != nullptr, "contiguous frame exists");
  expect_eq(contiguous_frame->encoded_data_size(), std::size_t{0},
      "contiguous frame encoded buffer before decode");

  std::vector<std::uint8_t> contiguous_decode_dst(1, 0);
  g_stub_decoder_decode_call_count = 0;
  g_stub_decoder_last_source.clear();
  expect_no_throw([&]() {
    dicom::pixel::decode_frame_into(*contiguous_file, 0,
        std::span<std::uint8_t>(contiguous_decode_dst), dicom::pixel::DecodeOptions{});
  }, "decode_frame_into contiguous");
  expect_eq(contiguous_decode_dst[0], static_cast<std::uint8_t>(0x7f),
      "decode_frame_into contiguous marker");
  expect_eq(g_stub_decoder_decode_call_count, 1,
      "decode_frame_into contiguous decoder calls");
  expect_eq(g_stub_decoder_last_source, contiguous_fragment,
      "decode_frame_into contiguous source bytes");
  expect_eq(contiguous_frame->encoded_data_size(), std::size_t{0},
      "single-fragment decode should stay zero-copy");

  g_stub_decoder_force_configure_failure = true;
  try {
    dicom::pixel::decode_frame_into(*contiguous_file, 0,
        std::span<std::uint8_t>(contiguous_decode_dst),
        dicom::pixel::DecodeOptions{});
    fail("decode_frame_into external configure failure should throw");
  } catch (const std::exception& e) {
    const std::string message = e.what();
    expect_contains(message, "pixel::decode_frame_into",
        "external decode error format function");
    expect_contains(message, "file=plugin-contiguous",
        "external decode error format file");
    expect_contains(message, "plugin=jpeg",
        "external decode error format plugin");
    expect_contains(message, "frame=0",
        "external decode error format frame");
    expect_contains(message, "status=invalid_argument",
        "external decode error format status");
    expect_contains(message, "stage=parse_options",
        "external decode error format stage");
    expect_contains(message, "decoder configure forced failure",
        "external decode error format reason");
  }
  g_stub_decoder_force_configure_failure = false;

  const std::vector<std::uint8_t> fragment_a{0x10, 0x20};
  const std::vector<std::uint8_t> fragment_b{0x30, 0x40, 0x50, 0x60};
  std::vector<std::uint8_t> expected_coalesced{};
  expected_coalesced.insert(expected_coalesced.end(),
      fragment_a.begin(), fragment_a.end());
  expected_coalesced.insert(expected_coalesced.end(),
      fragment_b.begin(), fragment_b.end());
  const auto multifragment_file_bytes = build_part10(
      "1.2.840.10008.1.2.4.50",
      build_jpeg_encapsulated_single_frame_body(
          std::vector<std::vector<std::uint8_t>>{fragment_a, fragment_b}));
  auto multifragment_file = dicom::read_bytes(
      "plugin-multifragment", multifragment_file_bytes.data(), multifragment_file_bytes.size());
  expect_true(multifragment_file != nullptr, "read multifragment test file");
  auto* multifragment_pixel_data = multifragment_file->get_dataelement("PixelData"_tag);
  expect_true(multifragment_pixel_data != nullptr && *multifragment_pixel_data,
      "multifragment PixelData exists");
  auto* multifragment_sequence = multifragment_pixel_data->as_pixel_sequence();
  expect_true(multifragment_sequence != nullptr, "multifragment PixelData is sequence");
  auto* multifragment_frame = multifragment_sequence->frame(0);
  expect_true(multifragment_frame != nullptr, "multifragment frame exists");
  expect_eq(multifragment_frame->encoded_data_size(), std::size_t{0},
      "multifragment frame encoded buffer before decode");

  std::vector<std::uint8_t> multifragment_decode_dst(1, 0);
  g_stub_decoder_decode_call_count = 0;
  g_stub_decoder_last_source.clear();
  expect_no_throw([&]() {
    dicom::pixel::decode_frame_into(*multifragment_file, 0,
        std::span<std::uint8_t>(multifragment_decode_dst),
        dicom::pixel::DecodeOptions{});
  }, "decode_frame_into multifragment");
  expect_eq(multifragment_decode_dst[0], static_cast<std::uint8_t>(0x7f),
      "decode_frame_into multifragment marker");
  expect_eq(g_stub_decoder_decode_call_count, 1,
      "decode_frame_into multifragment decoder calls");
  expect_eq(g_stub_decoder_last_source, expected_coalesced,
      "decode_frame_into multifragment coalesced source bytes");
  expect_eq(multifragment_frame->encoded_data_size(), expected_coalesced.size(),
      "multi-fragment decode should coalesce and cache contiguous bytes");

  g_stub_decoder_decode_call_count = 0;
  g_stub_decoder_last_source.clear();
  expect_no_throw([&]() {
    dicom::pixel::decode_frame_into(*multifragment_file, 0,
        std::span<std::uint8_t>(multifragment_decode_dst),
        dicom::pixel::DecodeOptions{});
  }, "decode_frame_into multifragment cached");
  expect_eq(g_stub_decoder_decode_call_count, 1,
      "decode_frame_into multifragment cached decoder calls");
  expect_eq(g_stub_decoder_last_source, expected_coalesced,
      "decode_frame_into multifragment cached source bytes");
  expect_eq(multifragment_frame->encoded_data_size(), expected_coalesced.size(),
      "coalesced cache should be reused");

  CodecDecodeFrameInput invalid_decode_rows = decode_input;
  invalid_decode_rows.info.rows = 70000;
  CodecError invalid_decode_rows_error{};
  expect_false(jpeg_plugin->decode_frame(invalid_decode_rows, invalid_decode_rows_error),
      "decode rows over max");
  expect_eq(invalid_decode_rows_error.code, CodecStatusCode::invalid_argument,
      "decode rows over max status");
  expect_eq(invalid_decode_rows_error.stage, std::string("validate"),
      "decode rows over max stage");
  expect_contains(invalid_decode_rows_error.detail, "max_rows",
      "decode rows over max detail");

  std::vector<std::uint8_t> encode_src(64, 0x12);
  CodecEncodeFrameInput encode_input{
      .source_frame = std::span<const std::uint8_t>(encode_src),
      .transfer_syntax = "JPEGBaseline8Bit"_uid,
      .rows = 8,
      .cols = 8,
      .samples_per_pixel = 1,
      .bytes_per_sample = 1,
      .bits_allocated = 8,
      .bits_stored = 8,
      .pixel_representation = 0,
      .use_multicomponent_transform = false,
      .source_planar = dicom::pixel::Planar::interleaved,
      .planar_source = false,
      .row_payload_bytes = 8,
      .source_row_stride = 8,
      .source_plane_stride = 64,
      .source_frame_size_bytes = 64,
      .destination_frame_payload = 1,
      .profile = dicom::pixel::detail::CodecProfile::jpeg_lossy,
  };
  CodecError encode_error{};
  std::vector<std::uint8_t> encoded{};
  dicom::pixel::detail::codec_option_pairs parsed_options{
      dicom::pixel::detail::CodecOptionKv{
          .key = "quality",
          .value = dicom::pixel::detail::codec_option_value{
              static_cast<std::int64_t>(90)},
      },
  };
  g_stub_encoder_encode_call_count = 0;
  g_stub_encoder_force_output_too_small_once = true;
  expect_true(jpeg_plugin->encode_frame(
                  encode_input, std::span<const dicom::pixel::detail::CodecOptionKv>(parsed_options),
                  encoded, encode_error),
      "external encoder dispatch call");
  expect_eq(g_stub_encoder_encode_call_count, 2,
      "external encoder retry call count");
  expect_eq(encoded.size(), std::size_t{3}, "external encoder output size");
  expect_eq(encoded[0], static_cast<std::uint8_t>(0xaa),
      "external encoder output byte0");
  expect_eq(encoded[1], static_cast<std::uint8_t>(0xbb),
      "external encoder output byte1");
  expect_eq(encoded[2], static_cast<std::uint8_t>(0xcc),
      "external encoder output byte2");

  CodecEncodeFrameInput invalid_encode_rows = encode_input;
  invalid_encode_rows.rows = 70000;
  CodecError invalid_encode_rows_error{};
  std::vector<std::uint8_t> invalid_rows_encoded{};
  expect_false(jpeg_plugin->encode_frame(invalid_encode_rows,
                  std::span<const dicom::pixel::detail::CodecOptionKv>(parsed_options),
                  invalid_rows_encoded, invalid_encode_rows_error),
      "encode rows over max");
  expect_eq(invalid_encode_rows_error.code, CodecStatusCode::invalid_argument,
      "encode rows over max status");
  expect_eq(invalid_encode_rows_error.stage, std::string("validate"),
      "encode rows over max stage");
  expect_contains(invalid_encode_rows_error.detail, "max_rows",
      "encode rows over max detail");

  CodecEncodeFrameInput invalid_payload = encode_input;
  invalid_payload.destination_frame_payload =
      static_cast<std::size_t>(2) * 1024u * 1024u * 1024u + 1;
  CodecError invalid_payload_error{};
  std::vector<std::uint8_t> invalid_payload_encoded{};
  expect_false(jpeg_plugin->encode_frame(invalid_payload,
                  std::span<const dicom::pixel::detail::CodecOptionKv>(parsed_options),
                  invalid_payload_encoded, invalid_payload_error),
      "encode payload over max");
  expect_eq(invalid_payload_error.code, CodecStatusCode::invalid_argument,
      "encode payload over max status");
  expect_eq(invalid_payload_error.stage, std::string("validate"),
      "encode payload over max stage");
  expect_contains(invalid_payload_error.detail, "max_source_frame_bytes",
      "encode payload over max detail");

  dicom::pixel::detail::codec_option_pairs bad_quality_options{
      dicom::pixel::detail::CodecOptionKv{
          .key = "quality",
          .value = dicom::pixel::detail::codec_option_value{
              static_cast<std::int64_t>(13)},
      },
  };
  CodecError bad_quality_error{};
  std::vector<std::uint8_t> bad_quality_encoded{};
  expect_false(jpeg_plugin->encode_frame(
                  encode_input, std::span<const dicom::pixel::detail::CodecOptionKv>(bad_quality_options),
                  bad_quality_encoded, bad_quality_error),
      "encoder configure failure");
  expect_eq(bad_quality_error.code, CodecStatusCode::invalid_argument,
      "encoder configure failure status");
  expect_eq(bad_quality_error.stage, std::string("parse_options"),
      "encoder configure failure stage");
  expect_contains(bad_quality_error.detail, "quality 13",
      "encoder configure failure detail");

  CodecEncodeFrameInput option_limit_encode_input = encode_input;
  option_limit_encode_input.transfer_syntax = option_limit_ts;
  dicom::pixel::detail::codec_option_pairs option_limit_options{};
  const auto build_option_limit_options = [&] {
    option_limit_options.clear();
    const auto export_error = export_option_limit_options(
        option_limit_ts, option_limit_options);
    if (export_error.has_value()) {
      fail("option limit export hook returned an unexpected error");
    }
  };

  g_option_limit_mode = OptionLimitMode::normal;
  build_option_limit_options();
  CodecError option_limit_ok_error{};
  std::vector<std::uint8_t> option_limit_ok_encoded{};
  expect_true(option_limit_plugin->encode_frame(option_limit_encode_input,
                  std::span<const dicom::pixel::detail::CodecOptionKv>(option_limit_options),
                  option_limit_ok_encoded,
                  option_limit_ok_error),
      "option limit normal encode success");
  expect_eq(option_limit_ok_encoded.size(), std::size_t{1},
      "option limit normal encoded size");
  expect_eq(option_limit_ok_encoded[0], static_cast<std::uint8_t>(0x42),
      "option limit normal encoded byte");

  g_option_limit_mode = OptionLimitMode::too_many_options;
  build_option_limit_options();
  CodecError option_limit_too_many_error{};
  std::vector<std::uint8_t> option_limit_too_many_encoded{};
  expect_false(option_limit_plugin->encode_frame(option_limit_encode_input,
                   std::span<const dicom::pixel::detail::CodecOptionKv>(option_limit_options),
                   option_limit_too_many_encoded,
                   option_limit_too_many_error),
      "option limit too many options");
  expect_eq(option_limit_too_many_error.code, CodecStatusCode::invalid_argument,
      "option limit too many options status");
  expect_eq(option_limit_too_many_error.stage, std::string("parse_options"),
      "option limit too many options stage");
  expect_contains(option_limit_too_many_error.detail, "max_option_count",
      "option limit too many options detail");

  g_option_limit_mode = OptionLimitMode::key_too_long;
  build_option_limit_options();
  CodecError option_limit_key_too_long_error{};
  std::vector<std::uint8_t> option_limit_key_too_long_encoded{};
  expect_false(option_limit_plugin->encode_frame(option_limit_encode_input,
                   std::span<const dicom::pixel::detail::CodecOptionKv>(option_limit_options),
                   option_limit_key_too_long_encoded,
                   option_limit_key_too_long_error),
      "option limit key too long");
  expect_eq(option_limit_key_too_long_error.code, CodecStatusCode::invalid_argument,
      "option limit key too long status");
  expect_eq(option_limit_key_too_long_error.stage, std::string("parse_options"),
      "option limit key too long stage");
  expect_contains(option_limit_key_too_long_error.detail, "max_option_key_bytes",
      "option limit key too long detail");

  g_option_limit_mode = OptionLimitMode::empty_key;
  build_option_limit_options();
  CodecError option_limit_empty_key_error{};
  std::vector<std::uint8_t> option_limit_empty_key_encoded{};
  expect_false(option_limit_plugin->encode_frame(option_limit_encode_input,
                   std::span<const dicom::pixel::detail::CodecOptionKv>(option_limit_options),
                   option_limit_empty_key_encoded,
                   option_limit_empty_key_error),
      "option limit empty key");
  expect_eq(option_limit_empty_key_error.code, CodecStatusCode::invalid_argument,
      "option limit empty key status");
  expect_eq(option_limit_empty_key_error.stage, std::string("parse_options"),
      "option limit empty key stage");
  expect_contains(option_limit_empty_key_error.detail, "empty option key",
      "option limit empty key detail");

  g_option_limit_mode = OptionLimitMode::value_too_long;
  build_option_limit_options();
  CodecError option_limit_value_too_long_error{};
  std::vector<std::uint8_t> option_limit_value_too_long_encoded{};
  expect_false(option_limit_plugin->encode_frame(option_limit_encode_input,
                   std::span<const dicom::pixel::detail::CodecOptionKv>(option_limit_options),
                   option_limit_value_too_long_encoded,
                   option_limit_value_too_long_error),
      "option limit value too long");
  expect_eq(option_limit_value_too_long_error.code, CodecStatusCode::invalid_argument,
      "option limit value too long status");
  expect_eq(option_limit_value_too_long_error.stage, std::string("parse_options"),
      "option limit value too long stage");
  expect_contains(option_limit_value_too_long_error.detail, "max_option_value_bytes",
      "option limit value too long detail");
  g_option_limit_mode = OptionLimitMode::normal;

  expect_true(dicom::pixel::unregister_external_codec_plugin("optlimit", &error),
      "unregister option limit plugin");

  expect_true(dicom::pixel::unregister_external_codec_plugin("jpeg", &error),
      "unregister external plugin");

  jpeg_plugin = registry.find_plugin("jpeg");
  expect_true(jpeg_plugin != nullptr, "jpeg plugin exists after unregister");
  expect_true(jpeg_plugin->decode_frame == original_decode,
      "decode dispatch restored");
  expect_true(jpeg_plugin->encode_frame == original_encode,
      "encode dispatch restored");

  CodecError restored_decode_error{};
  expect_false(jpeg_plugin->decode_frame(decode_input, restored_decode_error),
      "restored decode should not use stub and should fail for fake codestream");

  return 0;
}
