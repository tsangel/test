#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "internal.hpp"

namespace pixel::openjpeg_codec {

namespace {

bool frame_looks_like_jp2(const uint8_t* data, std::size_t size) {
  constexpr std::array<uint8_t, 12> kJp2Signature = {
      0x00, 0x00, 0x00, 0x0C, 0x6A, 0x50, 0x20, 0x20, 0x0D, 0x0A, 0x87, 0x0A};
  if (data == nullptr || size < kJp2Signature.size()) {
    return false;
  }
  return std::memcmp(data, kJp2Signature.data(), kJp2Signature.size()) == 0;
}

}  // namespace

std::string trim_trailing_ws(std::string text) {
  while (!text.empty()) {
    const char ch = text.back();
    if (ch == '\n' || ch == '\r' || ch == ' ' || ch == '\t') {
      text.pop_back();
      continue;
    }
    break;
  }
  return text;
}

void OPJ_CALLCONV opj_warning_handler(const char* message, void* user_data) {
  auto* sink = static_cast<OpenJpegLogSink*>(user_data);
  if (sink == nullptr || message == nullptr) {
    return;
  }
  sink->warning.append(message);
}

void OPJ_CALLCONV opj_error_handler(const char* message, void* user_data) {
  auto* sink = static_cast<OpenJpegLogSink*>(user_data);
  if (sink == nullptr || message == nullptr) {
    return;
  }
  sink->error.append(message);
}

std::string openjpeg_failure_message(
    const OpenJpegLogSink& sink, const char* fallback) {
  const std::string err = trim_trailing_ws(sink.error);
  if (!err.empty()) {
    return err;
  }
  const std::string warn = trim_trailing_ws(sink.warning);
  if (!warn.empty()) {
    return warn;
  }
  return (fallback != nullptr) ? std::string(fallback) : std::string("OpenJPEG failed");
}

OPJ_SIZE_T OPJ_CALLCONV opj_read_from_memory(
    void* out_buffer, OPJ_SIZE_T bytes_to_read, void* user_data) {
  auto* ctx = static_cast<DecodeStreamContext*>(user_data);
  if (ctx == nullptr || out_buffer == nullptr || bytes_to_read == 0) {
    return 0;
  }
  if (ctx->position >= ctx->size) {
    return static_cast<OPJ_SIZE_T>(-1);
  }
  const std::size_t remaining = ctx->size - ctx->position;
  const std::size_t requested = static_cast<std::size_t>(bytes_to_read);
  const std::size_t copy_size = std::min(remaining, requested);
  if (copy_size == 0) {
    return static_cast<OPJ_SIZE_T>(-1);
  }
  std::memcpy(out_buffer, ctx->data + ctx->position, copy_size);
  ctx->position += copy_size;
  return static_cast<OPJ_SIZE_T>(copy_size);
}

OPJ_OFF_T OPJ_CALLCONV opj_skip_in_memory(OPJ_OFF_T bytes_to_skip, void* user_data) {
  auto* ctx = static_cast<DecodeStreamContext*>(user_data);
  if (ctx == nullptr || bytes_to_skip < 0) {
    return static_cast<OPJ_OFF_T>(-1);
  }
  const std::size_t requested = static_cast<std::size_t>(bytes_to_skip);
  const std::size_t remaining = ctx->size - ctx->position;
  const std::size_t skipped = std::min(requested, remaining);
  ctx->position += skipped;
  return static_cast<OPJ_OFF_T>(skipped);
}

OPJ_BOOL OPJ_CALLCONV opj_seek_in_memory(OPJ_OFF_T absolute_position, void* user_data) {
  auto* ctx = static_cast<DecodeStreamContext*>(user_data);
  if (ctx == nullptr || absolute_position < 0) {
    return OPJ_FALSE;
  }
  const std::size_t target = static_cast<std::size_t>(absolute_position);
  if (target > ctx->size) {
    return OPJ_FALSE;
  }
  ctx->position = target;
  return OPJ_TRUE;
}

opj_stream_ptr create_decode_stream(DecodeStreamContext* context) {
  constexpr OPJ_SIZE_T kStreamBufferSize = 64 * 1024;
  opj_stream_ptr stream(opj_stream_create(kStreamBufferSize, OPJ_STREAM_READ));
  if (!stream) {
    return {};
  }
  context->position = 0;
  opj_stream_set_user_data(stream.get(), context, nullptr);
  opj_stream_set_user_data_length(stream.get(), static_cast<OPJ_UINT64>(context->size));
  opj_stream_set_read_function(stream.get(), opj_read_from_memory);
  opj_stream_set_skip_function(stream.get(), opj_skip_in_memory);
  opj_stream_set_seek_function(stream.get(), opj_seek_in_memory);
  return stream;
}

OPJ_SIZE_T OPJ_CALLCONV opj_write_to_memory(
    void* in_buffer, OPJ_SIZE_T bytes_to_write, void* user_data) {
  auto* ctx = static_cast<EncodeStreamContext*>(user_data);
  if (ctx == nullptr || in_buffer == nullptr) {
    return static_cast<OPJ_SIZE_T>(-1);
  }
  const std::size_t count = static_cast<std::size_t>(bytes_to_write);
  const std::size_t next_position = ctx->position + count;
  if (next_position < ctx->position) {
    return static_cast<OPJ_SIZE_T>(-1);
  }
  if (next_position > ctx->bytes.size()) {
    ctx->bytes.resize(next_position);
  }
  std::memcpy(ctx->bytes.data() + ctx->position, in_buffer, count);
  ctx->position = next_position;
  return bytes_to_write;
}

OPJ_OFF_T OPJ_CALLCONV opj_skip_in_output(OPJ_OFF_T bytes_to_skip, void* user_data) {
  auto* ctx = static_cast<EncodeStreamContext*>(user_data);
  if (ctx == nullptr || bytes_to_skip < 0) {
    return static_cast<OPJ_OFF_T>(-1);
  }
  const std::size_t count = static_cast<std::size_t>(bytes_to_skip);
  const std::size_t next_position = ctx->position + count;
  if (next_position < ctx->position) {
    return static_cast<OPJ_OFF_T>(-1);
  }
  if (next_position > ctx->bytes.size()) {
    ctx->bytes.resize(next_position);
  }
  ctx->position = next_position;
  return bytes_to_skip;
}

OPJ_BOOL OPJ_CALLCONV opj_seek_in_output(OPJ_OFF_T absolute_position, void* user_data) {
  auto* ctx = static_cast<EncodeStreamContext*>(user_data);
  if (ctx == nullptr || absolute_position < 0) {
    return OPJ_FALSE;
  }
  const std::size_t next_position = static_cast<std::size_t>(absolute_position);
  if (next_position > ctx->bytes.size()) {
    ctx->bytes.resize(next_position);
  }
  ctx->position = next_position;
  return OPJ_TRUE;
}

opj_stream_ptr create_encode_stream(EncodeStreamContext* context) {
  constexpr OPJ_SIZE_T kStreamBufferSize = 64 * 1024;
  opj_stream_ptr stream(opj_stream_create(kStreamBufferSize, OPJ_STREAM_WRITE));
  if (!stream) {
    return {};
  }
  context->position = 0;
  opj_stream_set_user_data(stream.get(), context, nullptr);
  opj_stream_set_write_function(stream.get(), opj_write_to_memory);
  opj_stream_set_skip_function(stream.get(), opj_skip_in_output);
  opj_stream_set_seek_function(stream.get(), opj_seek_in_output);
  return stream;
}

bool decode_with_openjpeg_format(const uint8_t* data, std::size_t size,
    OPJ_CODEC_FORMAT format, OPJ_UINT32 thread_count, opj_image_ptr* out_image,
    std::string* out_error) {
  if (out_image == nullptr || out_error == nullptr) {
    return false;
  }

  DecodeStreamContext stream_context{};
  stream_context.data = data;
  stream_context.size = size;

  opj_dparameters_t parameters{};
  opj_set_default_decoder_parameters(&parameters);

  opj_codec_ptr codec(opj_create_decompress(format));
  if (!codec) {
    *out_error = "OpenJPEG decoder creation failed";
    return false;
  }

  OpenJpegLogSink sink{};
  opj_set_warning_handler(codec.get(), opj_warning_handler, &sink);
  opj_set_error_handler(codec.get(), opj_error_handler, &sink);

  if (!opj_setup_decoder(codec.get(), &parameters)) {
    *out_error = openjpeg_failure_message(sink, "opj_setup_decoder failed");
    return false;
  }

  if (thread_count > 0 &&
      !opj_codec_set_threads(codec.get(), static_cast<int>(thread_count))) {
    *out_error = "opj_codec_set_threads failed";
    return false;
  }

  opj_stream_ptr stream = create_decode_stream(&stream_context);
  if (!stream) {
    *out_error = "OpenJPEG input stream creation failed";
    return false;
  }

  opj_image_t* raw_image = nullptr;
  if (!opj_read_header(stream.get(), codec.get(), &raw_image)) {
    if (raw_image != nullptr) {
      opj_image_destroy(raw_image);
    }
    *out_error = openjpeg_failure_message(sink, "opj_read_header failed");
    return false;
  }

  opj_image_ptr image(raw_image);
  if (!image) {
    *out_error = "OpenJPEG read header returned null image";
    return false;
  }

  if (!opj_decode(codec.get(), stream.get(), image.get())) {
    *out_error = openjpeg_failure_message(sink, "opj_decode failed");
    return false;
  }

  if (!opj_end_decompress(codec.get(), stream.get())) {
    *out_error = openjpeg_failure_message(sink, "opj_end_decompress failed");
    return false;
  }

  *out_image = std::move(image);
  return true;
}

bool decode_with_openjpeg_auto(const uint8_t* data, std::size_t size,
    OPJ_UINT32 thread_count, opj_image_ptr* out_image, std::string* out_error) {
  if (out_image == nullptr || out_error == nullptr) {
    return false;
  }

  const bool prefer_jp2 = frame_looks_like_jp2(data, size);
  const OPJ_CODEC_FORMAT first = prefer_jp2 ? OPJ_CODEC_JP2 : OPJ_CODEC_J2K;
  const OPJ_CODEC_FORMAT second = prefer_jp2 ? OPJ_CODEC_J2K : OPJ_CODEC_JP2;

  std::string first_error;
  if (decode_with_openjpeg_format(
          data, size, first, thread_count, out_image, &first_error)) {
    return true;
  }

  std::string second_error;
  if (decode_with_openjpeg_format(
          data, size, second, thread_count, out_image, &second_error)) {
    return true;
  }

  char detail[512];
  std::snprintf(detail, sizeof(detail), "OpenJPEG decode failed (first=%s, second=%s)",
      first_error.c_str(), second_error.c_str());
  *out_error = detail;
  return false;
}

}  // namespace pixel::openjpeg_codec
