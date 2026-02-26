#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include <dicom.h>
#include "../src/pixel_codec_plugin_abi_adapter.hpp"

namespace {
using namespace dicom::literals;

[[noreturn]] void fail(const std::string& message) {
  std::cerr << message << std::endl;
  std::exit(1);
}

template <typename T, typename U>
void expect_eq(const T& actual, const U& expected, std::string_view label) {
  if (!(actual == expected)) {
    fail(std::string(label) + " mismatch");
  }
}

void expect_true(bool value, std::string_view label) {
  if (!value) {
    fail(std::string(label) + " expected true");
  }
}

void test_decoder_request_mapping() {
  using dicom::pixel::DecodeOptions;
  using dicom::pixel::DecodeStrides;
  using dicom::pixel::Planar;
  using dicom::pixel::PixelDataInfo;
  using dicom::pixel::detail::CodecDecodeFrameInput;
  using dicom::pixel::detail::abi::build_decoder_request_v1;
  using dicom::pixel::detail::abi::to_transfer_syntax_code;

  std::vector<std::uint8_t> source_bytes{1, 2, 3, 4, 5};
  std::vector<std::uint8_t> destination_bytes(64, 0);

  PixelDataInfo info{};
  info.ts = "JPEGLSLossless"_uid;
  info.sv_dtype = dicom::pixel::DataType::u16;
  info.rows = 4;
  info.cols = 8;
  info.frames = 1;
  info.samples_per_pixel = 1;
  info.planar_configuration = Planar::interleaved;
  info.bits_stored = 12;

  CodecDecodeFrameInput input{
      .info = info,
      .prepared_source = std::span<const std::uint8_t>(source_bytes),
      .destination = std::span<std::uint8_t>(destination_bytes),
      .destination_strides = DecodeStrides{.row = 16, .frame = 64},
      .options = DecodeOptions{
          .planar_out = Planar::planar,
          .scaled = false,
          .decode_mct = false,
      },
  };

  dicomsdl_decoder_request_v1 request{};
  build_decoder_request_v1(input, request);

  expect_eq(request.struct_size, sizeof(dicomsdl_decoder_request_v1),
      "decoder request struct_size");
  expect_eq(request.abi_version, DICOMSDL_DECODER_PLUGIN_ABI_V1,
      "decoder request abi_version");
  expect_eq(request.source.source_buffer.size,
      static_cast<std::uint64_t>(source_bytes.size()), "decoder source size");
  expect_true(request.source.source_buffer.data == source_bytes.data(),
      "decoder source pointer");
  expect_eq(request.frame.transfer_syntax_code,
      to_transfer_syntax_code(info.ts), "decoder ts code");
  expect_eq(request.frame.source_dtype, DICOMSDL_DTYPE_U16,
      "decoder source dtype");
  expect_eq(request.frame.source_planar, DICOMSDL_PLANAR_INTERLEAVED,
      "decoder source planar");
  expect_eq(request.frame.decode_mct, 0u, "decoder decode_mct");
  expect_eq(request.output.dst_size,
      static_cast<std::uint64_t>(destination_bytes.size()),
      "decoder destination size");
  expect_true(request.output.dst == destination_bytes.data(),
      "decoder destination pointer");
  expect_eq(request.output.row_stride, 16u, "decoder row stride");
  expect_eq(request.output.frame_stride, 64u, "decoder frame stride");
  expect_eq(request.output.dst_planar, DICOMSDL_PLANAR_PLANAR,
      "decoder destination planar");
  expect_eq(request.output.dst_dtype, DICOMSDL_DTYPE_U16,
      "decoder destination dtype");
  expect_eq(request.value_transform.transform_kind,
      DICOMSDL_DECODER_VALUE_TRANSFORM_NONE,
      "decoder value transform kind default");

  input.options.scaled = true;
  input.value_transform.enabled = true;
  input.value_transform.rescale_slope = 1.5;
  input.value_transform.rescale_intercept = -8.0;
  build_decoder_request_v1(input, request);
  expect_eq(request.output.dst_dtype, DICOMSDL_DTYPE_F32,
      "decoder scaled destination dtype");
  expect_eq(request.value_transform.transform_kind,
      DICOMSDL_DECODER_VALUE_TRANSFORM_RESCALE,
      "decoder rescale transform kind");
  expect_eq(request.value_transform.rescale_slope, 1.5,
      "decoder rescale slope");
  expect_eq(request.value_transform.rescale_intercept, -8.0,
      "decoder rescale intercept");

  dicom::pixel::ModalityLut lut{};
  lut.first_mapped = -1024;
  lut.values = {0.0f, 2.0f, 4.0f};
  input.value_transform.modality_lut = lut;
  build_decoder_request_v1(input, request);
  expect_eq(request.value_transform.transform_kind,
      DICOMSDL_DECODER_VALUE_TRANSFORM_MODALITY_LUT,
      "decoder modality lut transform kind");
  expect_eq(request.value_transform.lut_first_mapped, static_cast<std::int64_t>(-1024),
      "decoder modality lut first mapped");
  expect_eq(request.value_transform.lut_value_count, static_cast<std::uint64_t>(3),
      "decoder modality lut count");
  expect_true(request.value_transform.lut_values_f32.data != nullptr,
      "decoder modality lut pointer");
  expect_eq(request.value_transform.lut_values_f32.size,
      static_cast<std::uint64_t>(sizeof(float) * 3),
      "decoder modality lut byte size");
}

void test_encoder_request_mapping() {
  using dicom::pixel::Planar;
  using dicom::pixel::detail::CodecEncodeFrameInput;
  using dicom::pixel::detail::abi::build_encoder_request_v1;
  using dicom::pixel::detail::abi::to_transfer_syntax_code;

  std::vector<std::uint8_t> source_bytes(96, 0x11);
  std::vector<std::uint8_t> encoded_buffer(128, 0);

  CodecEncodeFrameInput input{};
  input.source_frame = std::span<const std::uint8_t>(source_bytes);
  input.transfer_syntax = "JPEGBaseline8Bit"_uid;
  input.rows = 4;
  input.cols = 8;
  input.samples_per_pixel = 1;
  input.bytes_per_sample = 2;
  input.bits_allocated = 16;
  input.bits_stored = 12;
  input.pixel_representation = 1;
  input.use_multicomponent_transform = false;
  input.source_planar = Planar::interleaved;
  input.source_row_stride = 16;
  input.source_plane_stride = 64;
  input.source_frame_size_bytes = 64;
  input.destination_frame_payload = 64;
  input.profile = dicom::pixel::detail::CodecProfile::jpeg_lossy;

  dicomsdl_encoder_request_v1 request{};
  build_encoder_request_v1(
      input, std::span<std::uint8_t>(encoded_buffer), 17u, request);

  expect_eq(request.struct_size, sizeof(dicomsdl_encoder_request_v1),
      "encoder request struct_size");
  expect_eq(request.abi_version, DICOMSDL_ENCODER_PLUGIN_ABI_V1,
      "encoder request abi_version");
  expect_eq(request.source.source_buffer.size,
      static_cast<std::uint64_t>(source_bytes.size()), "encoder source size");
  expect_true(request.source.source_buffer.data == source_bytes.data(),
      "encoder source pointer");
  expect_eq(request.frame.transfer_syntax_code,
      to_transfer_syntax_code(input.transfer_syntax), "encoder ts code");
  expect_eq(request.frame.source_dtype, DICOMSDL_DTYPE_S16,
      "encoder source dtype");
  expect_eq(request.frame.codec_profile_code, DICOMSDL_CODEC_PROFILE_JPEG_LOSSY,
      "encoder profile code");
  expect_eq(request.frame.source_planar, DICOMSDL_PLANAR_INTERLEAVED,
      "encoder source planar");
  expect_eq(request.output.encoded_size, 17u, "encoder encoded_size");
  expect_eq(request.output.encoded_buffer.size,
      static_cast<std::uint64_t>(encoded_buffer.size()),
      "encoder output buffer size");
  expect_true(request.output.encoded_buffer.data == encoded_buffer.data(),
      "encoder output buffer pointer");
}

void test_error_mapping() {
  using dicom::pixel::detail::CodecStatusCode;
  using dicom::pixel::detail::abi::decode_plugin_error_v1;

  char detail_buffer[] = "bad option key";
  dicomsdl_codec_error_v1 plugin_error{};
  plugin_error.struct_size = sizeof(dicomsdl_codec_error_v1);
  plugin_error.abi_version = DICOMSDL_CODEC_PLUGIN_ABI_V1;
  plugin_error.status_code = DICOMSDL_CODEC_INVALID_ARGUMENT;
  plugin_error.stage_code = DICOMSDL_CODEC_STAGE_PARSE_OPTIONS;
  plugin_error.detail = detail_buffer;
  plugin_error.detail_capacity = sizeof(detail_buffer);
  plugin_error.detail_length = 14;

  auto converted = decode_plugin_error_v1(plugin_error, "decode_frame", "fallback");
  expect_eq(converted.code, CodecStatusCode::invalid_argument,
      "error status mapping");
  expect_eq(converted.stage, std::string("parse_options"),
      "error stage mapping");
  expect_eq(converted.detail, std::string("bad option key"),
      "error detail mapping");

  plugin_error.status_code = DICOMSDL_CODEC_OUTPUT_TOO_SMALL;
  plugin_error.stage_code = DICOMSDL_CODEC_STAGE_UNKNOWN;
  plugin_error.detail = nullptr;
  plugin_error.detail_capacity = 0;
  plugin_error.detail_length = 0;
  converted = decode_plugin_error_v1(plugin_error, "encode_frame", "need more bytes");
  expect_eq(converted.code, CodecStatusCode::backend_error,
      "output-too-small fallback status");
  expect_eq(converted.stage, std::string("encode_frame"),
      "unknown stage fallback");
  expect_eq(converted.detail, std::string("need more bytes"),
      "detail fallback");
}

}  // namespace

int main() {
  test_decoder_request_mapping();
  test_encoder_request_mapping();
  test_error_mapping();
  return 0;
}
