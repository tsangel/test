#include "host_adapter_v2.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>

#include "direct_api_v2.hpp"
#include "pixel/host/support/abi_convert_v2.hpp"

namespace pixel::runtime_v2 {

namespace {

struct StrideMeta {
  uint64_t min_row_bytes{0};
  uint64_t row_stride{0};
  uint64_t plane_stride{0};
  uint64_t frame_size_bytes{0};
};

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

bool resolve_source_strides(const dicom::pixel::PixelSource& source,
    const DtypeMeta& source_dtype, StrideMeta* out_meta) {
  if (out_meta == nullptr || source.rows <= 0 || source.cols <= 0 ||
      source.samples_per_pixel <= 0) {
    return false;
  }

  const bool planar_source = source.planar == dicom::pixel::Planar::planar;
  const uint64_t rows = static_cast<uint64_t>(source.rows);
  const uint64_t cols = static_cast<uint64_t>(source.cols);
  const uint64_t samples = static_cast<uint64_t>(source.samples_per_pixel);
  const uint64_t row_components = planar_source ? 1u : samples;

  uint64_t min_row_bytes = 0;
  if (!mul_u64(cols, row_components, &min_row_bytes) ||
      !mul_u64(min_row_bytes, source_dtype.bytes, &min_row_bytes)) {
    return false;
  }

  uint64_t row_stride = source.row_stride == 0
      ? min_row_bytes
      : static_cast<uint64_t>(source.row_stride);
  if (row_stride < min_row_bytes) {
    return false;
  }

  uint64_t plane_stride = 0;
  if (planar_source) {
    if (!mul_u64(row_stride, rows, &plane_stride)) {
      return false;
    }
  }

  uint64_t frame_size_bytes = source.frame_stride == 0
      ? 0
      : static_cast<uint64_t>(source.frame_stride);
  if (frame_size_bytes == 0) {
    if (!mul_u64(row_stride, rows, &frame_size_bytes)) {
      return false;
    }
    if (planar_source) {
      if (!mul_u64(frame_size_bytes, samples, &frame_size_bytes)) {
        return false;
      }
    }
  }

  out_meta->min_row_bytes = min_row_bytes;
  out_meta->row_stride = row_stride;
  out_meta->plane_stride = plane_stride;
  out_meta->frame_size_bytes = frame_size_bytes;
  return true;
}

void reset_decoder_context_fields(HostDecoderContextV2* ctx) {
  if (ctx == nullptr) {
    return;
  }
  ctx->codec_profile_code = PIXEL_CODEC_PROFILE_UNKNOWN_V2;
  ctx->binding_kind = DecoderBindingKind::kNone;
  ctx->plugin_api = nullptr;
  ctx->plugin_ctx = nullptr;
  ctx->display_name = nullptr;
}

void reset_encoder_context_fields(HostEncoderContextV2* ctx) {
  if (ctx == nullptr) {
    return;
  }
  ctx->codec_profile_code = PIXEL_CODEC_PROFILE_UNKNOWN_V2;
  ctx->binding_kind = EncoderBindingKind::kNone;
  ctx->plugin_api = nullptr;
  ctx->plugin_ctx = nullptr;
  ctx->display_name = nullptr;
}

pixel_error_code_v2 fail_decoder(
    HostDecoderContextV2* ctx, pixel_error_code_v2 ec, std::string_view stage,
    std::string_view reason) {
  set_detail(&ctx->last_error_detail, stage, reason);
  return ec;
}

pixel_error_code_v2 fail_encoder(
    HostEncoderContextV2* ctx, pixel_error_code_v2 ec, std::string_view stage,
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
    *out_codec_profile_code = PIXEL_CODEC_PROFILE_NATIVE_UNCOMPRESSED_V2;
    return true;
  case "EncapsulatedUncompressedExplicitVRLittleEndian"_uid.raw_index():
    *out_codec_profile_code = PIXEL_CODEC_PROFILE_ENCAPSULATED_UNCOMPRESSED_V2;
    return true;
  case "RLELossless"_uid.raw_index():
    *out_codec_profile_code = PIXEL_CODEC_PROFILE_RLE_LOSSLESS_V2;
    return true;
  case "HTJ2KLossless"_uid.raw_index():
    *out_codec_profile_code = PIXEL_CODEC_PROFILE_HTJ2K_LOSSLESS_V2;
    return true;
  case "HTJ2KLosslessRPCL"_uid.raw_index():
    *out_codec_profile_code = PIXEL_CODEC_PROFILE_HTJ2K_LOSSLESS_RPCL_V2;
    return true;
  case "HTJ2K"_uid.raw_index():
  case "JPIPHTJ2KReferenced"_uid.raw_index():
  case "JPIPHTJ2KReferencedDeflate"_uid.raw_index():
    *out_codec_profile_code = PIXEL_CODEC_PROFILE_HTJ2K_LOSSY_V2;
    return true;
  case "JPEG2000Lossless"_uid.raw_index():
  case "JPEG2000MCLossless"_uid.raw_index():
    *out_codec_profile_code = PIXEL_CODEC_PROFILE_JPEG2000_LOSSLESS_V2;
    return true;
  case "JPEG2000"_uid.raw_index():
  case "JPEG2000MC"_uid.raw_index():
  case "JPIPReferenced"_uid.raw_index():
  case "JPIPReferencedDeflate"_uid.raw_index():
    *out_codec_profile_code = PIXEL_CODEC_PROFILE_JPEG2000_LOSSY_V2;
    return true;
  case "JPEGLSLossless"_uid.raw_index():
    *out_codec_profile_code = PIXEL_CODEC_PROFILE_JPEGLS_LOSSLESS_V2;
    return true;
  case "JPEGLSNearLossless"_uid.raw_index():
    *out_codec_profile_code = PIXEL_CODEC_PROFILE_JPEGLS_NEAR_LOSSLESS_V2;
    return true;
  case "JPEGXLLossless"_uid.raw_index():
    *out_codec_profile_code = PIXEL_CODEC_PROFILE_JPEGXL_LOSSLESS_V2;
    return true;
  case "JPEGXL"_uid.raw_index():
    *out_codec_profile_code = PIXEL_CODEC_PROFILE_JPEGXL_LOSSY_V2;
    return true;
  case "JPEGXLJPEGRecompression"_uid.raw_index():
    *out_codec_profile_code = PIXEL_CODEC_PROFILE_JPEGXL_JPEG_RECOMPRESSION_V2;
    return true;
  case "JPEGLossless"_uid.raw_index():
  case "JPEGLosslessNonHierarchical15"_uid.raw_index():
  case "JPEGLosslessSV1"_uid.raw_index():
  case "JPEGLosslessHierarchical28"_uid.raw_index():
  case "JPEGLosslessHierarchical29"_uid.raw_index():
    *out_codec_profile_code = PIXEL_CODEC_PROFILE_JPEG_LOSSLESS_V2;
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
    *out_codec_profile_code = PIXEL_CODEC_PROFILE_JPEG_LOSSY_V2;
    return true;
  default:
    return false;
  }
}

void destroy_host_decoder_context_v2(HostDecoderContextV2* ctx) noexcept {
  if (ctx == nullptr) {
    return;
  }
  if (ctx->plugin_api != nullptr && ctx->plugin_api->destroy != nullptr &&
      ctx->plugin_ctx != nullptr) {
    ctx->plugin_api->destroy(ctx->plugin_ctx);
  }
  reset_decoder_context_fields(ctx);
}

void destroy_host_encoder_context_v2(HostEncoderContextV2* ctx) noexcept {
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
pixel_error_code_v2 configure_host_decoder_context_v2(HostDecoderContextV2* ctx,
    const BindingRegistryV2* registry, dicom::uid::WellKnown transfer_syntax,
    const pixel_option_list_v2* options) {
  // Validate required inputs before touching decoder state.
  if (ctx == nullptr || registry == nullptr) {
    return PIXEL_CODEC_ERR_INVALID_ARGUMENT;
  }

  // Start from a clean context because configure is defined as a full reset.
  destroy_host_decoder_context_v2(ctx);
  clear_detail(&ctx->last_error_detail);

  // Map the transfer syntax to the runtime codec profile understood by the registry.
  uint32_t codec_profile_code = PIXEL_CODEC_PROFILE_UNKNOWN_V2;
  if (!codec_profile_code_from_transfer_syntax(
          transfer_syntax, &codec_profile_code)) {
    return fail_decoder(ctx, PIXEL_CODEC_ERR_UNSUPPORTED, "resolve",
        "unsupported transfer syntax for v2 host decoder adapter");
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
  const pixel_error_code_v2 configure_ec = ctx->plugin_api->configure(
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
pixel_error_code_v2 configure_host_encoder_context_v2(HostEncoderContextV2* ctx,
    const BindingRegistryV2* registry, dicom::uid::WellKnown transfer_syntax,
    const pixel_option_list_v2* options) {
  // Validate required inputs before touching encoder state.
  if (ctx == nullptr || registry == nullptr) {
    return PIXEL_CODEC_ERR_INVALID_ARGUMENT;
  }

  // Start from a clean context because configure is defined as a full reset.
  destroy_host_encoder_context_v2(ctx);
  clear_detail(&ctx->last_error_detail);

  // Map the transfer syntax to the runtime codec profile understood by the registry.
  uint32_t codec_profile_code = PIXEL_CODEC_PROFILE_UNKNOWN_V2;
  if (!codec_profile_code_from_transfer_syntax(
          transfer_syntax, &codec_profile_code)) {
    return fail_encoder(ctx, PIXEL_CODEC_ERR_UNSUPPORTED, "resolve",
        "unsupported transfer syntax for v2 host encoder adapter");
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
  const pixel_error_code_v2 configure_ec = ctx->plugin_api->configure(
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

pixel_error_code_v2 decode_frame_with_host_context_v2(HostDecoderContextV2* ctx,
    const dicom::pixel::PixelDataInfo* info, std::span<const uint8_t> prepared_source,
    std::span<uint8_t> destination, const dicom::pixel::DecodeStrides* destination_strides,
    const dicom::pixel::DecodeOptions* options,
    const HostModalityValueTransformV2* modality_value_transform) {
  if (ctx == nullptr || info == nullptr || prepared_source.empty() ||
      destination.empty()) {
    return PIXEL_CODEC_ERR_INVALID_ARGUMENT;
  }
  if (ctx->binding_kind == DecoderBindingKind::kNone ||
      ctx->codec_profile_code == PIXEL_CODEC_PROFILE_UNKNOWN_V2) {
    return fail_decoder(ctx, PIXEL_CODEC_ERR_FAILED, "decode_frame",
        "decoder host context is not configured");
  }
  if (info->rows <= 0 || info->cols <= 0 || info->samples_per_pixel <= 0) {
    return fail_decoder(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "invalid PixelDataInfo dimensions");
  }

  DtypeMeta source_dtype{};
  if (!resolve_dtype_meta(info->sv_dtype, &source_dtype)) {
    return fail_decoder(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "unsupported source dtype in PixelDataInfo");
  }

  const dicom::pixel::DecodeOptions effective_options =
      options == nullptr ? dicom::pixel::DecodeOptions{} : *options;
  const bool mct_capable_profile =
      ctx->codec_profile_code == PIXEL_CODEC_PROFILE_JPEG2000_LOSSLESS_V2 ||
      ctx->codec_profile_code == PIXEL_CODEC_PROFILE_JPEG2000_LOSSY_V2 ||
      ctx->codec_profile_code == PIXEL_CODEC_PROFILE_HTJ2K_LOSSLESS_V2 ||
      ctx->codec_profile_code == PIXEL_CODEC_PROFILE_HTJ2K_LOSSLESS_RPCL_V2 ||
      ctx->codec_profile_code == PIXEL_CODEC_PROFILE_HTJ2K_LOSSY_V2;

  uint8_t dst_dtype = source_dtype.code;
  if (modality_value_transform != nullptr &&
      modality_value_transform->kind != HostModalityValueTransformKindV2::kNone) {
    dst_dtype = PIXEL_DTYPE_F32_V2;
  }

  pixel_decoder_request_v2 request{};
  request.struct_size = sizeof(pixel_decoder_request_v2);
  request.abi_version = PIXEL_DECODER_PLUGIN_ABI_V2;

  request.source.struct_size = sizeof(pixel_decoder_source_v2);
  request.source.abi_version = PIXEL_DECODER_PLUGIN_ABI_V2;
  request.source.source_buffer.data = prepared_source.data();
  request.source.source_buffer.size = static_cast<uint64_t>(prepared_source.size());

  request.frame.struct_size = sizeof(pixel_decoder_frame_info_v2);
  request.frame.abi_version = PIXEL_DECODER_PLUGIN_ABI_V2;
  request.frame.codec_profile_code = ctx->codec_profile_code;
  request.frame.source_dtype = source_dtype.code;
  request.frame.source_planar = to_planar_code(info->planar_configuration);
  request.frame.rows = info->rows;
  request.frame.cols = info->cols;
  request.frame.samples_per_pixel = info->samples_per_pixel;
  request.frame.bits_stored = info->bits_stored;
  request.frame.decode_mct =
      (mct_capable_profile && effective_options.decode_mct) ? 1u : 0u;

  request.output.struct_size = sizeof(pixel_decoder_output_v2);
  request.output.abi_version = PIXEL_DECODER_PLUGIN_ABI_V2;
  request.output.dst = destination.data();
  request.output.dst_size = static_cast<uint64_t>(destination.size());
  request.output.row_stride = destination_strides == nullptr
      ? 0u
      : static_cast<uint64_t>(destination_strides->row);
  request.output.frame_stride = destination_strides == nullptr
      ? 0u
      : static_cast<uint64_t>(destination_strides->frame);
  request.output.dst_dtype = dst_dtype;
  request.output.dst_planar = to_planar_code(effective_options.planar_out);

  request.value_transform.struct_size = sizeof(pixel_decoder_value_transform_v2);
  request.value_transform.abi_version = PIXEL_DECODER_PLUGIN_ABI_V2;
  request.value_transform.transform_kind = PIXEL_DECODER_VALUE_TRANSFORM_NONE_V2;
  request.value_transform.rescale_slope = 1.0;
  request.value_transform.rescale_intercept = 0.0;

  if (modality_value_transform != nullptr) {
    switch (modality_value_transform->kind) {
    case HostModalityValueTransformKindV2::kNone:
      break;
    case HostModalityValueTransformKindV2::kRescale:
      request.value_transform.transform_kind = PIXEL_DECODER_VALUE_TRANSFORM_RESCALE_V2;
      request.value_transform.rescale_slope = modality_value_transform->rescale_slope;
      request.value_transform.rescale_intercept = modality_value_transform->rescale_intercept;
      break;
    case HostModalityValueTransformKindV2::kModalityLut:
      if (modality_value_transform->modality_lut == nullptr ||
          modality_value_transform->modality_lut->values.empty()) {
        return fail_decoder(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
            "modality LUT transform requires non-empty LUT values");
      }
      request.value_transform.transform_kind = PIXEL_DECODER_VALUE_TRANSFORM_MODALITY_LUT_V2;
      request.value_transform.lut_first_mapped =
          modality_value_transform->modality_lut->first_mapped;
      request.value_transform.lut_value_count =
          static_cast<uint64_t>(modality_value_transform->modality_lut->values.size());
      request.value_transform.lut_values_f32.data =
          reinterpret_cast<const uint8_t*>(
              modality_value_transform->modality_lut->values.data());
      request.value_transform.lut_values_f32.size =
          static_cast<uint64_t>(
              modality_value_transform->modality_lut->values.size() * sizeof(float));
      break;
    }
  }

  pixel_error_code_v2 decode_ec = PIXEL_CODEC_ERR_FAILED;
  if (ctx->binding_kind == DecoderBindingKind::kCoreDirect) {
    pixel::core_v2::ErrorState core_error{};
    decode_ec = pixel::core_v2::decode_uncompressed_frame(&core_error, &request);
    if (decode_ec != PIXEL_CODEC_ERR_OK) {
      std::array<char, 1024> buffer{};
      const uint32_t copied = pixel::core_v2::copy_last_error_detail(
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

pixel_error_code_v2 encode_frame_with_host_context_v2(HostEncoderContextV2* ctx,
    const dicom::pixel::PixelSource* source, std::span<const uint8_t> source_frame,
    bool use_multicomponent_transform, pixel_output_buffer_v2 encoded_buffer,
    uint64_t* inout_encoded_size) {
  if (ctx == nullptr || source == nullptr || source_frame.empty() ||
      inout_encoded_size == nullptr) {
    return PIXEL_CODEC_ERR_INVALID_ARGUMENT;
  }
  if (ctx->binding_kind == EncoderBindingKind::kNone ||
      ctx->codec_profile_code == PIXEL_CODEC_PROFILE_UNKNOWN_V2) {
    return fail_encoder(ctx, PIXEL_CODEC_ERR_FAILED, "encode_frame",
        "encoder host context is not configured");
  }

  DtypeMeta source_dtype{};
  if (!resolve_dtype_meta(source->data_type, &source_dtype)) {
    return fail_encoder(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "unsupported source dtype in PixelSource");
  }
  if (source->rows <= 0 || source->cols <= 0 || source->samples_per_pixel <= 0) {
    return fail_encoder(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "invalid PixelSource dimensions");
  }

  StrideMeta stride_meta{};
  if (!resolve_source_strides(*source, source_dtype, &stride_meta)) {
    return fail_encoder(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "invalid source strides in PixelSource");
  }
  if (source_frame.size() < stride_meta.frame_size_bytes) {
    return fail_encoder(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "source frame bytes shorter than required source_frame_size_bytes");
  }

  const int32_t bits_allocated = static_cast<int32_t>(source_dtype.bytes * 8u);
  const int32_t bits_stored = source->bits_stored > 0 ? source->bits_stored : bits_allocated;
  const int32_t pixel_representation =
      (source_dtype.is_signed && !source_dtype.is_float) ? 1 : 0;

  pixel_encoder_request_v2 request{};
  request.struct_size = sizeof(pixel_encoder_request_v2);
  request.abi_version = PIXEL_ENCODER_PLUGIN_ABI_V2;

  request.source.struct_size = sizeof(pixel_encoder_source_v2);
  request.source.abi_version = PIXEL_ENCODER_PLUGIN_ABI_V2;
  request.source.source_buffer.data = source_frame.data();
  request.source.source_buffer.size = static_cast<uint64_t>(source_frame.size());

  request.frame.struct_size = sizeof(pixel_encoder_frame_info_v2);
  request.frame.abi_version = PIXEL_ENCODER_PLUGIN_ABI_V2;
  request.frame.codec_profile_code = ctx->codec_profile_code;
  request.frame.source_dtype = source_dtype.code;
  request.frame.source_planar = to_planar_code(source->planar);
  request.frame.rows = source->rows;
  request.frame.cols = source->cols;
  request.frame.samples_per_pixel = source->samples_per_pixel;
  request.frame.bits_allocated = bits_allocated;
  request.frame.bits_stored = bits_stored;
  request.frame.pixel_representation = pixel_representation;
  request.frame.source_row_stride = stride_meta.row_stride;
  request.frame.source_plane_stride = stride_meta.plane_stride;
  request.frame.source_frame_size_bytes = stride_meta.frame_size_bytes;
  request.frame.use_multicomponent_transform = use_multicomponent_transform ? 1u : 0u;

  request.output.struct_size = sizeof(pixel_encoder_output_v2);
  request.output.abi_version = PIXEL_ENCODER_PLUGIN_ABI_V2;
  request.output.encoded_buffer = encoded_buffer;
  request.output.encoded_size = *inout_encoded_size;

  pixel_error_code_v2 encode_ec = PIXEL_CODEC_ERR_FAILED;
  if (ctx->binding_kind == EncoderBindingKind::kCoreDirect) {
    pixel::core_v2::ErrorState core_error{};
    encode_ec = pixel::core_v2::encode_uncompressed_frame(&core_error, &request);
    *inout_encoded_size = request.output.encoded_size;
    if (encode_ec != PIXEL_CODEC_ERR_OK) {
      std::array<char, 1024> buffer{};
      const uint32_t copied = pixel::core_v2::copy_last_error_detail(
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

bool host_encoder_context_supports_context_buffer_v2(
    const HostEncoderContextV2* ctx) noexcept {
  return ctx != nullptr &&
      ctx->binding_kind == EncoderBindingKind::kPluginApi &&
      ctx->plugin_api != nullptr &&
      ctx->plugin_ctx != nullptr &&
      ctx->plugin_api->encode_frame_to_context_buffer != nullptr &&
      ctx->plugin_api->get_encoded_buffer != nullptr;
}

pixel_error_code_v2 encode_frame_to_context_buffer_with_host_context_v2(
    HostEncoderContextV2* ctx, const dicom::pixel::PixelSource* source,
    std::span<const uint8_t> source_frame, bool use_multicomponent_transform,
    pixel_const_buffer_v2* out_encoded_buffer) {
  if (ctx == nullptr || source == nullptr || source_frame.empty() ||
      out_encoded_buffer == nullptr) {
    return PIXEL_CODEC_ERR_INVALID_ARGUMENT;
  }
  out_encoded_buffer->data = nullptr;
  out_encoded_buffer->size = 0;

  if (ctx->binding_kind == EncoderBindingKind::kNone ||
      ctx->codec_profile_code == PIXEL_CODEC_PROFILE_UNKNOWN_V2) {
    return fail_encoder(ctx, PIXEL_CODEC_ERR_FAILED, "encode_frame",
        "encoder host context is not configured");
  }
  if (!host_encoder_context_supports_context_buffer_v2(ctx)) {
    return fail_encoder(ctx, PIXEL_CODEC_ERR_UNSUPPORTED, "encode_frame",
        "context-buffer encode is not supported for this encoder binding");
  }

  DtypeMeta source_dtype{};
  if (!resolve_dtype_meta(source->data_type, &source_dtype)) {
    return fail_encoder(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "unsupported source dtype in PixelSource");
  }
  if (source->rows <= 0 || source->cols <= 0 || source->samples_per_pixel <= 0) {
    return fail_encoder(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "invalid PixelSource dimensions");
  }

  StrideMeta stride_meta{};
  if (!resolve_source_strides(*source, source_dtype, &stride_meta)) {
    return fail_encoder(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "invalid source strides in PixelSource");
  }
  if (source_frame.size() < stride_meta.frame_size_bytes) {
    return fail_encoder(ctx, PIXEL_CODEC_ERR_INVALID_ARGUMENT, "validate",
        "source frame bytes shorter than required source_frame_size_bytes");
  }

  const int32_t bits_allocated = static_cast<int32_t>(source_dtype.bytes * 8u);
  const int32_t bits_stored = source->bits_stored > 0 ? source->bits_stored : bits_allocated;
  const int32_t pixel_representation =
      (source_dtype.is_signed && !source_dtype.is_float) ? 1 : 0;

  pixel_encoder_request_v2 request{};
  request.struct_size = sizeof(pixel_encoder_request_v2);
  request.abi_version = PIXEL_ENCODER_PLUGIN_ABI_V2;

  request.source.struct_size = sizeof(pixel_encoder_source_v2);
  request.source.abi_version = PIXEL_ENCODER_PLUGIN_ABI_V2;
  request.source.source_buffer.data = source_frame.data();
  request.source.source_buffer.size = static_cast<uint64_t>(source_frame.size());

  request.frame.struct_size = sizeof(pixel_encoder_frame_info_v2);
  request.frame.abi_version = PIXEL_ENCODER_PLUGIN_ABI_V2;
  request.frame.codec_profile_code = ctx->codec_profile_code;
  request.frame.source_dtype = source_dtype.code;
  request.frame.source_planar = to_planar_code(source->planar);
  request.frame.rows = source->rows;
  request.frame.cols = source->cols;
  request.frame.samples_per_pixel = source->samples_per_pixel;
  request.frame.bits_allocated = bits_allocated;
  request.frame.bits_stored = bits_stored;
  request.frame.pixel_representation = pixel_representation;
  request.frame.source_row_stride = stride_meta.row_stride;
  request.frame.source_plane_stride = stride_meta.plane_stride;
  request.frame.source_frame_size_bytes = stride_meta.frame_size_bytes;
  request.frame.use_multicomponent_transform = use_multicomponent_transform ? 1u : 0u;

  request.output.struct_size = sizeof(pixel_encoder_output_v2);
  request.output.abi_version = PIXEL_ENCODER_PLUGIN_ABI_V2;

  pixel_error_code_v2 encode_ec =
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

uint32_t copy_host_decoder_last_error_detail_v2(
    const HostDecoderContextV2* ctx, char* out_detail, uint32_t out_detail_capacity) {
  if (ctx == nullptr) {
    return copy_text_to_buffer({}, out_detail, out_detail_capacity);
  }
  return copy_text_to_buffer(ctx->last_error_detail, out_detail, out_detail_capacity);
}

uint32_t copy_host_encoder_last_error_detail_v2(
    const HostEncoderContextV2* ctx, char* out_detail, uint32_t out_detail_capacity) {
  if (ctx == nullptr) {
    return copy_text_to_buffer({}, out_detail, out_detail_capacity);
  }
  return copy_text_to_buffer(ctx->last_error_detail, out_detail, out_detail_capacity);
}

}  // namespace pixel::runtime_v2
