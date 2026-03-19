#include "host_adapter.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>

#include "direct_api.hpp"
#include "pixel/host/support/abi_convert.hpp"

namespace pixel::runtime {

namespace {

bool mul_u64(uint64_t a, uint64_t b, uint64_t* out_value) {
  if (out_value == nullptr) {
    return false;
  }
  if (a == 0 || b == 0) {
    *out_value = 0;
    return true;
  }
  if (a > (std::numeric_limits<uint64_t>::max)() / b) {
    return false;
  }
  *out_value = a * b;
  return true;
}

uint32_t copy_text_to_buffer(
    std::string_view text, char* out_detail, uint32_t out_detail_capacity) {
  if (out_detail == nullptr || out_detail_capacity == 0) {
    return 0;
  }
  const uint32_t copy_len = std::min<uint32_t>(
      static_cast<uint32_t>(text.size()), out_detail_capacity - 1u);
  if (copy_len > 0) {
    std::memcpy(out_detail, text.data(), copy_len);
  }
  out_detail[copy_len] = '\0';
  return copy_len;
}

void clear_detail(std::string* out_detail) {
  if (out_detail != nullptr) {
    out_detail->clear();
  }
}

void set_detail(std::string* out_detail, std::string_view stage, std::string_view reason) {
  if (out_detail == nullptr) {
    return;
  }
  out_detail->assign("stage=");
  out_detail->append(stage);
  out_detail->append(";reason=");
  out_detail->append(reason);
}

template <typename CopyFn>
void capture_plugin_detail(CopyFn copy_fn, const void* plugin_ctx, std::string* out_detail) {
  if (out_detail == nullptr) {
    return;
  }
  out_detail->clear();
  if (copy_fn == nullptr || plugin_ctx == nullptr) {
    return;
  }
  std::array<char, 1024> buffer{};
  const uint32_t copied = copy_fn(
      plugin_ctx, buffer.data(), static_cast<uint32_t>(buffer.size()));
  if (copied == 0) {
    return;
  }
  out_detail->assign(buffer.data(), buffer.data() + copied);
}

bool resolve_encode_source_strides(const dicom::pixel::PixelLayout& layout,
    const DtypeMeta& source_dtype, EncoderStrideMeta* out_meta) {
  if (out_meta == nullptr) {
    return false;
  }
  if (layout.empty()) {
    return false;
  }

  const bool planar_source =
      layout.planar == dicom::pixel::Planar::planar &&
      layout.samples_per_pixel > std::uint16_t{1};
  const uint64_t rows = static_cast<uint64_t>(layout.rows);
  const uint64_t cols = static_cast<uint64_t>(layout.cols);
  const uint64_t samples = static_cast<uint64_t>(layout.samples_per_pixel);
  const uint64_t row_components = planar_source ? cols : cols * samples;

  uint64_t min_row_bytes = 0;
  if (!mul_u64(row_components, source_dtype.bytes, &min_row_bytes)) {
    return false;
  }

  const uint64_t row_stride = static_cast<uint64_t>(layout.row_stride);
  if (row_stride < min_row_bytes) {
    return false;
  }

  uint64_t plane_stride = 0;
  if (planar_source) {
    if (!mul_u64(row_stride, rows, &plane_stride)) {
      return false;
    }
  }

  // Runtime encoders receive one frame at a time, so the required input span is the
  // minimum bytes that cover that frame's rows/planes, not the caller's inter-frame gap.
  uint64_t frame_size_bytes = 0;
  if (!mul_u64(row_stride, rows, &frame_size_bytes)) {
    return false;
  }
  if (planar_source) {
    if (!mul_u64(frame_size_bytes, samples, &frame_size_bytes)) {
      return false;
    }
  }

  out_meta->min_row_bytes = min_row_bytes;
  out_meta->row_stride = row_stride;
  out_meta->plane_stride = plane_stride;
  out_meta->frame_size_bytes = frame_size_bytes;
  return true;
}

void reset_decoder_context_fields(HostDecoderContext* ctx) {
  if (ctx == nullptr) {
    return;
  }
  ctx->codec_profile_code = PIXEL_CODEC_PROFILE_UNKNOWN;
  ctx->binding_kind = DecoderBindingKind::kNone;
  ctx->plugin_api = nullptr;
  ctx->plugin_ctx = nullptr;
  ctx->display_name = nullptr;
}

void reset_encoder_context_fields(HostEncoderContext* ctx) {
  if (ctx == nullptr) {
    return;
  }
  ctx->codec_profile_code = PIXEL_CODEC_PROFILE_UNKNOWN;
  ctx->binding_kind = EncoderBindingKind::kNone;
  ctx->plugin_api = nullptr;
  ctx->plugin_ctx = nullptr;
  ctx->display_name = nullptr;
}

pixel_error_code fail_decoder(
    HostDecoderContext* ctx, pixel_error_code ec, std::string_view stage,
    std::string_view reason) {
  set_detail(&ctx->last_error_detail, stage, reason);
  return ec;
}

pixel_error_code fail_encoder(
    HostEncoderContext* ctx, pixel_error_code ec, std::string_view stage,
    std::string_view reason) {
  set_detail(&ctx->last_error_detail, stage, reason);
  return ec;
}

}  // namespace

bool codec_profile_code_from_transfer_syntax(
    dicom::uid::WellKnown transfer_syntax, uint32_t* out_codec_profile_code) noexcept {
  if (out_codec_profile_code == nullptr || !transfer_syntax.valid() ||
      transfer_syntax.uid_type() != dicom::UidType::TransferSyntax) {
    return false;
  }

  using namespace dicom::literals;

  // Keep this switch aligned with uid transfer syntax classification.
  switch (transfer_syntax.raw_index()) {
  case "ImplicitVRLittleEndian"_uid.raw_index():
  case "ExplicitVRLittleEndian"_uid.raw_index():
  case "DeflatedExplicitVRLittleEndian"_uid.raw_index():
  case "ExplicitVRBigEndian"_uid.raw_index():
  case "Papyrus3ImplicitVRLittleEndian"_uid.raw_index():
    *out_codec_profile_code = PIXEL_CODEC_PROFILE_NATIVE_UNCOMPRESSED;
    return true;
  case "EncapsulatedUncompressedExplicitVRLittleEndian"_uid.raw_index():
    *out_codec_profile_code = PIXEL_CODEC_PROFILE_ENCAPSULATED_UNCOMPRESSED;
    return true;
  case "RLELossless"_uid.raw_index():
    *out_codec_profile_code = PIXEL_CODEC_PROFILE_RLE_LOSSLESS;
    return true;
  case "HTJ2KLossless"_uid.raw_index():
    *out_codec_profile_code = PIXEL_CODEC_PROFILE_HTJ2K_LOSSLESS;
    return true;
  case "HTJ2KLosslessRPCL"_uid.raw_index():
    *out_codec_profile_code = PIXEL_CODEC_PROFILE_HTJ2K_LOSSLESS_RPCL;
    return true;
  case "HTJ2K"_uid.raw_index():
  case "JPIPHTJ2KReferenced"_uid.raw_index():
  case "JPIPHTJ2KReferencedDeflate"_uid.raw_index():
    *out_codec_profile_code = PIXEL_CODEC_PROFILE_HTJ2K_LOSSY;
    return true;
  case "JPEG2000Lossless"_uid.raw_index():
  case "JPEG2000MCLossless"_uid.raw_index():
    *out_codec_profile_code = PIXEL_CODEC_PROFILE_JPEG2000_LOSSLESS;
    return true;
  case "JPEG2000"_uid.raw_index():
  case "JPEG2000MC"_uid.raw_index():
  case "JPIPReferenced"_uid.raw_index():
  case "JPIPReferencedDeflate"_uid.raw_index():
    *out_codec_profile_code = PIXEL_CODEC_PROFILE_JPEG2000_LOSSY;
    return true;
  case "JPEGLSLossless"_uid.raw_index():
    *out_codec_profile_code = PIXEL_CODEC_PROFILE_JPEGLS_LOSSLESS;
    return true;
  case "JPEGLSNearLossless"_uid.raw_index():
    *out_codec_profile_code = PIXEL_CODEC_PROFILE_JPEGLS_NEAR_LOSSLESS;
    return true;
  case "JPEGXLLossless"_uid.raw_index():
    *out_codec_profile_code = PIXEL_CODEC_PROFILE_JPEGXL_LOSSLESS;
    return true;
  case "JPEGXL"_uid.raw_index():
    *out_codec_profile_code = PIXEL_CODEC_PROFILE_JPEGXL_LOSSY;
    return true;
  case "JPEGXLJPEGRecompression"_uid.raw_index():
    *out_codec_profile_code = PIXEL_CODEC_PROFILE_JPEGXL_JPEG_RECOMPRESSION;
    return true;
  case "JPEGLossless"_uid.raw_index():
  case "JPEGLosslessNonHierarchical15"_uid.raw_index():
  case "JPEGLosslessSV1"_uid.raw_index():
  case "JPEGLosslessHierarchical28"_uid.raw_index():
  case "JPEGLosslessHierarchical29"_uid.raw_index():
    *out_codec_profile_code = PIXEL_CODEC_PROFILE_JPEG_LOSSLESS;
    return true;
  case "JPEGBaseline8Bit"_uid.raw_index():
  case "JPEGExtended12Bit"_uid.raw_index():
  case "JPEGExtended35"_uid.raw_index():
  case "JPEGSpectralSelectionNonHierarchical68"_uid.raw_index():
  case "JPEGSpectralSelectionNonHierarchical79"_uid.raw_index():
  case "JPEGFullProgressionNonHierarchical1012"_uid.raw_index():
  case "JPEGFullProgressionNonHierarchical1113"_uid.raw_index():
  case "JPEGExtendedHierarchical1618"_uid.raw_index():
  case "JPEGExtendedHierarchical1719"_uid.raw_index():
  case "JPEGSpectralSelectionHierarchical2022"_uid.raw_index():
  case "JPEGSpectralSelectionHierarchical2123"_uid.raw_index():
  case "JPEGFullProgressionHierarchical2426"_uid.raw_index():
  case "JPEGFullProgressionHierarchical2527"_uid.raw_index():
    *out_codec_profile_code = PIXEL_CODEC_PROFILE_JPEG_LOSSY;
    return true;
  default:
    return false;
  }
}

void destroy_host_decoder_context(HostDecoderContext* ctx) noexcept {
  if (ctx == nullptr) {
    return;
  }
  if (ctx->plugin_api != nullptr && ctx->plugin_api->destroy != nullptr &&
      ctx->plugin_ctx != nullptr) {
    ctx->plugin_api->destroy(ctx->plugin_ctx);
  }
  reset_decoder_context_fields(ctx);
}

void destroy_host_encoder_context(HostEncoderContext* ctx) noexcept {
  if (ctx == nullptr) {
    return;
  }
  if (ctx->plugin_api != nullptr && ctx->plugin_api->destroy != nullptr &&
      ctx->plugin_ctx != nullptr) {
    ctx->plugin_api->destroy(ctx->plugin_ctx);
  }
  reset_encoder_context_fields(ctx);
}

// Configure one decoder context end-to-end: resolve profile, select binding, and
// create/configure plugin state when the binding is plugin-backed.
pixel_error_code configure_host_decoder_context(HostDecoderContext* ctx,
    const BindingRegistry* registry, dicom::uid::WellKnown transfer_syntax,
    const pixel_option_list* options) {
  // Validate required inputs before touching decoder state.
  if (ctx == nullptr || registry == nullptr) {
    return PIXEL_CODEC_ERR_INVALID_ARGUMENT;
  }

  // Start from a clean context because configure is defined as a full reset.
  destroy_host_decoder_context(ctx);
  clear_detail(&ctx->last_error_detail);

  // Map the transfer syntax to the runtime codec profile understood by the registry.
  uint32_t codec_profile_code = PIXEL_CODEC_PROFILE_UNKNOWN;
  if (!codec_profile_code_from_transfer_syntax(
          transfer_syntax, &codec_profile_code)) {
    return fail_decoder(ctx, PIXEL_CODEC_ERR_UNSUPPORTED, "resolve",
        "unsupported transfer syntax for host decoder adapter");
  }

  // Find the decoder binding that will serve this codec profile.
  const DecoderBinding* binding = registry->find_decoder_binding(codec_profile_code);
  if (binding == nullptr || binding->binding_kind == DecoderBindingKind::kNone) {
    return fail_decoder(ctx, PIXEL_CODEC_ERR_UNSUPPORTED, "plugin_lookup",
        "decoder binding is not registered for resolved codec profile");
  }

  ctx->codec_profile_code = codec_profile_code;
  ctx->binding_kind = binding->binding_kind;
  ctx->display_name = binding->display_name;

  // Core-direct bindings ignore options because the binding fully defines the behavior.
  if (ctx->binding_kind == DecoderBindingKind::kCoreDirect) {
    return PIXEL_CODEC_ERR_OK;
  }

  if (ctx->binding_kind != DecoderBindingKind::kPluginApi ||
      binding->plugin_api == nullptr) {
    return fail_decoder(ctx, PIXEL_CODEC_ERR_FAILED, "plugin_lookup",
        "invalid decoder binding");
  }

  // Plugin bindings need a plugin-owned context before they can be configured.
  ctx->plugin_api = binding->plugin_api;
  ctx->plugin_ctx = ctx->plugin_api->create();
  if (ctx->plugin_ctx == nullptr) {
    reset_decoder_context_fields(ctx);
    return fail_decoder(ctx, PIXEL_CODEC_ERR_FAILED, "create",
        "decoder plugin create failed");
  }

  // Let the plugin validate and absorb codec-specific options.
  const pixel_error_code configure_ec = ctx->plugin_api->configure(
      ctx->plugin_ctx, ctx->codec_profile_code, options);
  if (configure_ec != PIXEL_CODEC_ERR_OK) {
    capture_plugin_detail(
        ctx->plugin_api->copy_last_error_detail, ctx->plugin_ctx, &ctx->last_error_detail);
    ctx->plugin_api->destroy(ctx->plugin_ctx);
    reset_decoder_context_fields(ctx);
    return configure_ec;
  }

  // Successful configure clears any stale detail carried over from prior failures.
  clear_detail(&ctx->last_error_detail);
  return PIXEL_CODEC_ERR_OK;
}

// Configure one encoder context end-to-end: resolve profile, select binding, and
// create/configure plugin state when the binding is plugin-backed.
pixel_error_code configure_host_encoder_context(HostEncoderContext* ctx,
    const BindingRegistry* registry, dicom::uid::WellKnown transfer_syntax,
    const pixel_option_list* options) {
  // Validate required inputs before touching encoder state.
  if (ctx == nullptr || registry == nullptr) {
    return PIXEL_CODEC_ERR_INVALID_ARGUMENT;
  }

  // Start from a clean context because configure is defined as a full reset.
  destroy_host_encoder_context(ctx);
  clear_detail(&ctx->last_error_detail);

  // Map the transfer syntax to the runtime codec profile understood by the registry.
  uint32_t codec_profile_code = PIXEL_CODEC_PROFILE_UNKNOWN;
  if (!codec_profile_code_from_transfer_syntax(
          transfer_syntax, &codec_profile_code)) {
    return fail_encoder(ctx, PIXEL_CODEC_ERR_UNSUPPORTED, "resolve",
        "unsupported transfer syntax for host encoder adapter");
  }

  // Find the encoder binding that will serve this codec profile.
  const EncoderBinding* binding = registry->find_encoder_binding(codec_profile_code);
  if (binding == nullptr || binding->binding_kind == EncoderBindingKind::kNone) {
    return fail_encoder(ctx, PIXEL_CODEC_ERR_UNSUPPORTED, "plugin_lookup",
        "encoder binding is not registered for resolved codec profile");
  }

  ctx->codec_profile_code = codec_profile_code;
  ctx->binding_kind = binding->binding_kind;
  ctx->display_name = binding->display_name;

  // Core-direct bindings ignore options because the binding fully defines the behavior.
  if (ctx->binding_kind == EncoderBindingKind::kCoreDirect) {
    return PIXEL_CODEC_ERR_OK;
  }

  if (ctx->binding_kind != EncoderBindingKind::kPluginApi ||
      binding->plugin_api == nullptr) {
    return fail_encoder(ctx, PIXEL_CODEC_ERR_FAILED, "plugin_lookup",
        "invalid encoder binding");
  }

  // Plugin bindings need a plugin-owned context before they can be configured.
  ctx->plugin_api = binding->plugin_api;
  ctx->plugin_ctx = ctx->plugin_api->create();
  if (ctx->plugin_ctx == nullptr) {
    reset_encoder_context_fields(ctx);
    return fail_encoder(ctx, PIXEL_CODEC_ERR_FAILED, "create",
        "encoder plugin create failed");
  }

  // Let the plugin validate and absorb codec-specific options.
  const pixel_error_code configure_ec = ctx->plugin_api->configure(
      ctx->plugin_ctx, ctx->codec_profile_code, options);
  if (configure_ec != PIXEL_CODEC_ERR_OK) {
    capture_plugin_detail(
        ctx->plugin_api->copy_last_error_detail, ctx->plugin_ctx, &ctx->last_error_detail);
    ctx->plugin_api->destroy(ctx->plugin_ctx);
    reset_encoder_context_fields(ctx);
    return configure_ec;
  }

  // Successful configure clears any stale detail carried over from prior failures.
  clear_detail(&ctx->last_error_detail);
  return PIXEL_CODEC_ERR_OK;
}

pixel_error_code decode_frame_with_host_context(HostDecoderContext* ctx,
    const dicom::pixel::PixelLayout* source_layout, std::span<const uint8_t> prepared_source,
    std::span<uint8_t> destination, const dicom::pixel::PixelLayout* output_layout,
    const dicom::pixel::DecodeOptions* options) {
  // Validate host-side invariants before constructing the ABI request handed to
  // builtin decoders or external plugins.
  if (ctx == nullptr || source_layout == nullptr || prepared_source.empty() ||
      destination.empty()) {
    return PIXEL_CODEC_ERR_INVALID_ARGUMENT;
  }
  if (ctx->binding_kind == DecoderBindingKind::kNone ||
      ctx->codec_profile_code == PIXEL_CODEC_PROFILE_UNKNOWN) {
    return fail_decoder(ctx, PIXEL_CODEC_ERR_FAILED, "decode_frame",
        "decoder host context is not configured");
  }
  if (source_layout->rows == 0 || source_layout->cols == 0 ||
      source_layout->samples_per_pixel == 0) {
    return fail_decoder(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "invalid source PixelLayout dimensions");
  }

  DtypeMeta source_dtype{};
  if (!resolve_dtype_meta(source_layout->data_type, &source_dtype)) {
    return fail_decoder(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "unsupported source dtype in PixelLayout");
  }

  const dicom::pixel::DecodeOptions effective_options =
      options == nullptr ? dicom::pixel::DecodeOptions{} : *options;

  // Normalize the destination layout once so the ABI request builder can
  // translate one explicit output contract for both plugin and core-direct paths.
  const dicom::pixel::PixelLayout normalized_output_layout =
      output_layout != nullptr ? *output_layout : source_layout->packed();

  DtypeMeta output_dtype{};
  if (!resolve_dtype_meta(normalized_output_layout.data_type, &output_dtype)) {
      return fail_decoder(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
          "unsupported destination dtype in PixelLayout");
  }

  pixel_decoder_request request{};
  if (!build_decoder_request(ctx->codec_profile_code, source_dtype.code, *source_layout,
          prepared_source, destination, output_dtype.code,
          normalized_output_layout.planar,
          static_cast<uint64_t>(normalized_output_layout.row_stride),
          static_cast<uint64_t>(normalized_output_layout.frame_stride),
          is_mct_capable_profile(ctx->codec_profile_code) && effective_options.decode_mct,
          &request)) {
    return fail_decoder(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "failed to build decoder ABI request");
  }

  pixel_error_code decode_ec = PIXEL_CODEC_ERR_FAILED;
  if (ctx->binding_kind == DecoderBindingKind::kCoreDirect) {
    // Core-direct bindings bypass plugin dispatch and call the shared core implementation.
    pixel::core::ErrorState core_error{};
    decode_ec = pixel::core::decode_uncompressed_frame(&core_error, &request);
    if (decode_ec != PIXEL_CODEC_ERR_OK) {
      std::array<char, 1024> buffer{};
      const uint32_t copied = pixel::core::copy_last_error_detail(
          &core_error, buffer.data(), static_cast<uint32_t>(buffer.size()));
      if (copied == 0) {
        set_detail(&ctx->last_error_detail, "decode_frame",
            "core direct decode failed");
      } else {
        ctx->last_error_detail.assign(buffer.data(), buffer.data() + copied);
      }
      return decode_ec;
    }
    clear_detail(&ctx->last_error_detail);
    return PIXEL_CODEC_ERR_OK;
  }

  if (ctx->binding_kind != DecoderBindingKind::kPluginApi ||
      ctx->plugin_api == nullptr || ctx->plugin_ctx == nullptr) {
    return fail_decoder(ctx, PIXEL_CODEC_ERR_FAILED, "decode_frame",
        "invalid decoder plugin context");
  }

  // Plugin-backed decode reuses the same ABI request shape after host-side normalization.
  decode_ec = ctx->plugin_api->decode_frame(ctx->plugin_ctx, &request);
  if (decode_ec != PIXEL_CODEC_ERR_OK) {
    capture_plugin_detail(
        ctx->plugin_api->copy_last_error_detail, ctx->plugin_ctx, &ctx->last_error_detail);
    if (ctx->last_error_detail.empty()) {
      set_detail(&ctx->last_error_detail, "decode_frame", "plugin decode failed");
    }
    return decode_ec;
  }

  clear_detail(&ctx->last_error_detail);
  return PIXEL_CODEC_ERR_OK;
}

pixel_error_code encode_frame_with_host_context(HostEncoderContext* ctx,
    const dicom::pixel::PixelLayout* source_layout, std::span<const uint8_t> source_frame,
    bool use_multicomponent_transform, pixel_output_buffer encoded_buffer,
    uint64_t* inout_encoded_size) {
  // Encode now receives the normalized source layout directly, so request
  // building only needs validation and ABI field translation here.
  if (ctx == nullptr || source_layout == nullptr || source_frame.empty() ||
      inout_encoded_size == nullptr) {
    return PIXEL_CODEC_ERR_INVALID_ARGUMENT;
  }
  if (ctx->binding_kind == EncoderBindingKind::kNone ||
      ctx->codec_profile_code == PIXEL_CODEC_PROFILE_UNKNOWN) {
    return fail_encoder(ctx, PIXEL_CODEC_ERR_FAILED, "encode_frame",
        "encoder host context is not configured");
  }

  DtypeMeta source_dtype{};
  if (!resolve_dtype_meta(source_layout->data_type, &source_dtype)) {
    return fail_encoder(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "unsupported source dtype in PixelLayout");
  }
  if (source_layout->empty()) {
    return fail_encoder(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "invalid PixelLayout dimensions");
  }

  EncoderStrideMeta stride_meta{};
  if (!resolve_encode_source_strides(*source_layout, source_dtype, &stride_meta)) {
    return fail_encoder(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "invalid source strides in PixelLayout");
  }
  if (source_frame.size() < stride_meta.frame_size_bytes) {
    return fail_encoder(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "source frame bytes shorter than required source_frame_size_bytes");
  }

  pixel_encoder_request request{};
  if (!build_encoder_request(ctx->codec_profile_code, source_dtype, stride_meta,
          *source_layout, source_frame, use_multicomponent_transform, encoded_buffer,
          *inout_encoded_size, &request)) {
    return fail_encoder(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "failed to build encoder ABI request");
  }

  pixel_error_code encode_ec = PIXEL_CODEC_ERR_FAILED;
  if (ctx->binding_kind == EncoderBindingKind::kCoreDirect) {
    pixel::core::ErrorState core_error{};
    encode_ec = pixel::core::encode_uncompressed_frame(&core_error, &request);
    *inout_encoded_size = request.output.encoded_size;
    if (encode_ec != PIXEL_CODEC_ERR_OK) {
      std::array<char, 1024> buffer{};
      const uint32_t copied = pixel::core::copy_last_error_detail(
          &core_error, buffer.data(), static_cast<uint32_t>(buffer.size()));
      if (copied == 0) {
        set_detail(&ctx->last_error_detail, "encode_frame",
            "core direct encode failed");
      } else {
        ctx->last_error_detail.assign(buffer.data(), buffer.data() + copied);
      }
      return encode_ec;
    }
    clear_detail(&ctx->last_error_detail);
    return PIXEL_CODEC_ERR_OK;
  }

  if (ctx->binding_kind != EncoderBindingKind::kPluginApi ||
      ctx->plugin_api == nullptr || ctx->plugin_ctx == nullptr) {
    return fail_encoder(ctx, PIXEL_CODEC_ERR_FAILED, "encode_frame",
        "invalid encoder plugin context");
  }

  encode_ec = ctx->plugin_api->encode_frame(ctx->plugin_ctx, &request);
  *inout_encoded_size = request.output.encoded_size;
  if (encode_ec != PIXEL_CODEC_ERR_OK) {
    capture_plugin_detail(
        ctx->plugin_api->copy_last_error_detail, ctx->plugin_ctx, &ctx->last_error_detail);
    if (ctx->last_error_detail.empty()) {
      set_detail(&ctx->last_error_detail, "encode_frame", "plugin encode failed");
    }
    return encode_ec;
  }

  clear_detail(&ctx->last_error_detail);
  return PIXEL_CODEC_ERR_OK;
}

bool host_encoder_context_supports_context_buffer(
    const HostEncoderContext* ctx) noexcept {
  return ctx != nullptr &&
      ctx->binding_kind == EncoderBindingKind::kPluginApi &&
      ctx->plugin_api != nullptr &&
      ctx->plugin_ctx != nullptr &&
      ctx->plugin_api->encode_frame_to_context_buffer != nullptr &&
      ctx->plugin_api->get_encoded_buffer != nullptr;
}

pixel_error_code encode_frame_to_context_buffer_with_host_context(
    HostEncoderContext* ctx, const dicom::pixel::PixelLayout* source_layout,
    std::span<const uint8_t> source_frame, bool use_multicomponent_transform,
    pixel_const_buffer* out_encoded_buffer) {
  // This variant shares the same validation and frame metadata path, but lets the
  // plugin own the encoded buffer lifetime inside its context.
  if (ctx == nullptr || source_layout == nullptr || source_frame.empty() ||
      out_encoded_buffer == nullptr) {
    return PIXEL_CODEC_ERR_INVALID_ARGUMENT;
  }
  out_encoded_buffer->data = nullptr;
  out_encoded_buffer->size = 0;

  if (ctx->binding_kind == EncoderBindingKind::kNone ||
      ctx->codec_profile_code == PIXEL_CODEC_PROFILE_UNKNOWN) {
    return fail_encoder(ctx, PIXEL_CODEC_ERR_FAILED, "encode_frame",
        "encoder host context is not configured");
  }
  if (!host_encoder_context_supports_context_buffer(ctx)) {
    return fail_encoder(ctx, PIXEL_CODEC_ERR_UNSUPPORTED, "encode_frame",
        "context-buffer encode is not supported for this encoder binding");
  }

  DtypeMeta source_dtype{};
  if (!resolve_dtype_meta(source_layout->data_type, &source_dtype)) {
    return fail_encoder(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "unsupported source dtype in PixelLayout");
  }
  if (source_layout->empty()) {
    return fail_encoder(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "invalid PixelLayout dimensions");
  }

  EncoderStrideMeta stride_meta{};
  if (!resolve_encode_source_strides(*source_layout, source_dtype, &stride_meta)) {
    return fail_encoder(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "invalid source strides in PixelLayout");
  }
  if (source_frame.size() < stride_meta.frame_size_bytes) {
    return fail_encoder(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "source frame bytes shorter than required source_frame_size_bytes");
  }

  pixel_encoder_request request{};
  if (!build_encoder_request(ctx->codec_profile_code, source_dtype, stride_meta,
          *source_layout, source_frame, use_multicomponent_transform,
          pixel_output_buffer{}, 0u, &request)) {
    return fail_encoder(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "failed to build encoder ABI request");
  }

  pixel_error_code encode_ec =
      ctx->plugin_api->encode_frame_to_context_buffer(ctx->plugin_ctx, &request);
  if (encode_ec != PIXEL_CODEC_ERR_OK) {
    capture_plugin_detail(
        ctx->plugin_api->copy_last_error_detail, ctx->plugin_ctx, &ctx->last_error_detail);
    if (ctx->last_error_detail.empty()) {
      set_detail(&ctx->last_error_detail, "encode_frame",
          "plugin context-buffer encode failed");
    }
    return encode_ec;
  }

  encode_ec = ctx->plugin_api->get_encoded_buffer(ctx->plugin_ctx, out_encoded_buffer);
  if (encode_ec != PIXEL_CODEC_ERR_OK) {
    capture_plugin_detail(
        ctx->plugin_api->copy_last_error_detail, ctx->plugin_ctx, &ctx->last_error_detail);
    if (ctx->last_error_detail.empty()) {
      set_detail(&ctx->last_error_detail, "encode_frame",
          "plugin encoded buffer lookup failed");
    }
    return encode_ec;
  }

  clear_detail(&ctx->last_error_detail);
  return PIXEL_CODEC_ERR_OK;
}

uint32_t copy_host_decoder_last_error_detail(
    const HostDecoderContext* ctx, char* out_detail, uint32_t out_detail_capacity) {
  if (ctx == nullptr) {
    return copy_text_to_buffer({}, out_detail, out_detail_capacity);
  }
  return copy_text_to_buffer(ctx->last_error_detail, out_detail, out_detail_capacity);
}

uint32_t copy_host_encoder_last_error_detail(
    const HostEncoderContext* ctx, char* out_detail, uint32_t out_detail_capacity) {
  if (ctx == nullptr) {
    return copy_text_to_buffer({}, out_detail, out_detail_capacity);
  }
  return copy_text_to_buffer(ctx->last_error_detail, out_detail, out_detail_capacity);
}

}  // namespace pixel::runtime

