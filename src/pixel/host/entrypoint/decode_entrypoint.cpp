#include "dicom.h"

#include "diagnostics.h"
#include "pixel_decoder_plugin_abi.h"
#include "pixel/host/decode/decode_frame_dispatch.hpp"
#include "pixel/host/decode/decode_plan_compute.hpp"
#include "pixel/host/error/codec_error.hpp"
#include "pixel/host/support/dicom_pixel_support.hpp"
#include "photometric_text_detail.hpp"

#include <charconv>
#include <cstring>
#include <limits>
#include <system_error>

namespace dicom {

namespace pixel {
using namespace dicom::literals;

namespace {

[[nodiscard]] std::optional<Photometric> representable_photometric(
    Photometric photometric) noexcept {
	switch (photometric) {
	case Photometric::monochrome1:
	case Photometric::monochrome2:
	case Photometric::palette_color:
	case Photometric::rgb:
	case Photometric::ybr_full:
	case Photometric::ybr_full_422:
	case Photometric::ybr_rct:
	case Photometric::ybr_ict:
	case Photometric::ybr_partial_420:
	case Photometric::xyb:
	case Photometric::hsv:
	case Photometric::argb:
	case Photometric::cmyk:
	case Photometric::ybr_partial_422:
		return photometric;
	default:
		return std::nullopt;
	}
}

[[nodiscard]] std::optional<Photometric> map_decoded_photometric(
    uint8_t color_space_code, Photometric planned_photometric) noexcept {
	switch (color_space_code) {
	case PIXEL_DECODED_COLOR_SPACE_MONOCHROME:
		switch (planned_photometric) {
		case Photometric::monochrome1:
		case Photometric::monochrome2:
		case Photometric::palette_color:
			return planned_photometric;
		default:
			return std::nullopt;
		}
	case PIXEL_DECODED_COLOR_SPACE_PALETTE_COLOR:
		return Photometric::palette_color;
	case PIXEL_DECODED_COLOR_SPACE_RGB:
		return Photometric::rgb;
	case PIXEL_DECODED_COLOR_SPACE_CMYK:
		return Photometric::cmyk;
	case PIXEL_DECODED_COLOR_SPACE_YBR_FULL:
		return Photometric::ybr_full;
	case PIXEL_DECODED_COLOR_SPACE_YBR_FULL_422:
		return Photometric::ybr_full_422;
	case PIXEL_DECODED_COLOR_SPACE_YBR_PARTIAL_420:
		return Photometric::ybr_partial_420;
	case PIXEL_DECODED_COLOR_SPACE_YBR_PARTIAL_422:
		return Photometric::ybr_partial_422;
	case PIXEL_DECODED_COLOR_SPACE_YBR_RCT:
		return Photometric::ybr_rct;
	case PIXEL_DECODED_COLOR_SPACE_YBR_ICT:
		return Photometric::ybr_ict;
	case PIXEL_DECODED_COLOR_SPACE_XYB:
		return Photometric::xyb;
	case PIXEL_DECODED_COLOR_SPACE_HSV:
		return Photometric::hsv;
	case PIXEL_DECODED_COLOR_SPACE_ARGB:
		return Photometric::argb;
	case PIXEL_DECODED_COLOR_SPACE_RGBA:
		return std::nullopt;
	case PIXEL_DECODED_COLOR_SPACE_UNKNOWN:
	default:
		return representable_photometric(planned_photometric);
	}
}

[[nodiscard]] EncodedLossyState map_encoded_lossy_state(
    uint8_t encoded_lossy_state) noexcept {
	switch (encoded_lossy_state) {
	case PIXEL_ENCODED_LOSSY_STATE_LOSSLESS:
		return EncodedLossyState::lossless;
	case PIXEL_ENCODED_LOSSY_STATE_LOSSY:
		return EncodedLossyState::lossy;
	case PIXEL_ENCODED_LOSSY_STATE_NEAR_LOSSLESS:
		return EncodedLossyState::near_lossless;
	case PIXEL_ENCODED_LOSSY_STATE_UNKNOWN:
	default:
		return EncodedLossyState::unknown;
	}
}

[[nodiscard]] std::optional<DataType> map_decoded_dtype(
    uint8_t dtype_code) noexcept {
	switch (dtype_code) {
	case PIXEL_DTYPE_U8:
		return DataType::u8;
	case PIXEL_DTYPE_S8:
		return DataType::s8;
	case PIXEL_DTYPE_U16:
		return DataType::u16;
	case PIXEL_DTYPE_S16:
		return DataType::s16;
	case PIXEL_DTYPE_U32:
		return DataType::u32;
	case PIXEL_DTYPE_S32:
		return DataType::s32;
	case PIXEL_DTYPE_F32:
		return DataType::f32;
	case PIXEL_DTYPE_F64:
		return DataType::f64;
	case PIXEL_DTYPE_UNKNOWN:
	default:
		return std::nullopt;
	}
}

[[nodiscard]] std::optional<Planar> map_decoded_planar(
    uint8_t planar_code) noexcept {
	switch (planar_code) {
	case PIXEL_DECODED_PLANAR_INTERLEAVED:
		return Planar::interleaved;
	case PIXEL_DECODED_PLANAR_PLANAR:
		return Planar::planar;
	case PIXEL_DECODED_PLANAR_UNKNOWN:
	default:
		return std::nullopt;
	}
}

void assign_decode_info(const pixel_decoder_info& abi_info,
    Photometric planned_photometric, DecodeInfo& decode_info) {
	decode_info = DecodeInfo{
	    .photometric = map_decoded_photometric(
	        abi_info.actual_color_space, planned_photometric),
	    .encoded_lossy_state = map_encoded_lossy_state(abi_info.encoded_lossy_state),
	    .data_type = map_decoded_dtype(abi_info.actual_dtype),
	    .planar = map_decoded_planar(abi_info.actual_planar),
	    .bits_per_sample = abi_info.bits_per_sample,
	};
}

void assign_native_one_bit_decode_info(
    const PixelLayout& output_layout, DecodeInfo& decode_info) {
	decode_info = DecodeInfo{
	    .photometric = output_layout.photometric,
	    .encoded_lossy_state = EncodedLossyState::lossless,
	    .data_type = DataType::u8,
	    .planar = output_layout.planar,
	    .bits_per_sample = 1,
	};
}

[[nodiscard]] std::string_view trim_ascii_space(std::string_view text) noexcept {
	while (!text.empty() && (text.front() == ' ' || text.front() == '\t' ||
	                            text.front() == '\r' || text.front() == '\n')) {
		text.remove_prefix(1);
	}
	while (!text.empty() && (text.back() == ' ' || text.back() == '\t' ||
	                           text.back() == '\r' || text.back() == '\n')) {
		text.remove_suffix(1);
	}
	return text;
}

[[nodiscard]] std::size_t parse_fragment_size_or_throw(
    std::string_view text, std::string_view label) {
	text = trim_ascii_space(text);
	if (text.empty()) {
		detail::throw_codec_stage_exception(
		    detail::CodecStatusCode::invalid_argument, "parse_frame_fragments",
		    "{} is empty", label);
	}
	std::uint64_t value = 0;
	const char* begin = text.data();
	const char* end = text.data() + text.size();
	const auto [ptr, ec] = std::from_chars(begin, end, value, 10);
	if (ec != std::errc{} || ptr != end) {
		detail::throw_codec_stage_exception(
		    detail::CodecStatusCode::invalid_argument, "parse_frame_fragments",
		    "{} is not a valid unsigned integer: {}", label, text);
	}
	if (value > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
		detail::throw_codec_stage_exception(
		    detail::CodecStatusCode::invalid_argument, "parse_frame_fragments",
		    "{} exceeds size_t range", label);
	}
	return static_cast<std::size_t>(value);
}

[[nodiscard]] DataType data_type_from_descriptor_or_throw(
    std::uint16_t bits_allocated, std::uint16_t pixel_representation) {
	const bool is_signed = pixel_representation != 0;
	switch (bits_allocated) {
	case 8:
		return is_signed ? DataType::s8 : DataType::u8;
	case 16:
		return is_signed ? DataType::s16 : DataType::u16;
	case 32:
		return is_signed ? DataType::s32 : DataType::u32;
	default:
		detail::throw_codec_stage_exception(
		    detail::CodecStatusCode::invalid_argument, "create_payload_decoder",
		    "bits_allocated must be 8, 16, or 32");
	}
}

[[nodiscard]] std::size_t checked_add_seven_divide_by_eight_or_throw(
    std::size_t value, std::string_view stage) {
	if (value > std::numeric_limits<std::size_t>::max() - std::size_t{7}) {
		detail::throw_codec_stage_exception(
		    detail::CodecStatusCode::invalid_argument, stage,
		    "1-bit pixel payload size exceeds size_t range");
	}
	return (value + std::size_t{7}) / std::size_t{8};
}

[[nodiscard]] support_detail::NativeOneBitPixelLayout
one_bit_layout_from_descriptor_or_throw(
    const PixelPayloadDecodeDescriptor& desc) {
	if (desc.rows == 0 || desc.cols == 0 || desc.frames == 0 ||
	    desc.samples_per_pixel == 0) {
		detail::throw_codec_stage_exception(
		    detail::CodecStatusCode::invalid_argument, "create_payload_decoder",
		    "rows, cols, frames, and samples_per_pixel must be positive");
	}
	if (desc.pixel_representation != 0) {
		detail::throw_codec_stage_exception(
		    detail::CodecStatusCode::invalid_argument, "create_payload_decoder",
		    "1-bit pixel payload requires pixel_representation=0");
	}
	const auto bits_stored =
	    desc.bits_stored != 0 ? desc.bits_stored : desc.bits_allocated;
	if (bits_stored != 1) {
		detail::throw_codec_stage_exception(
		    detail::CodecStatusCode::invalid_argument, "create_payload_decoder",
		    "1-bit pixel payload requires bits_stored=1");
	}
	const auto photometric =
	    parse_photometric(trim_ascii_space(desc.photometric));
	if (!photometric.has_value()) {
		detail::throw_codec_stage_exception(
		    detail::CodecStatusCode::invalid_argument, "create_payload_decoder",
		    "unknown PhotometricInterpretation: {}", desc.photometric);
	}

	std::size_t pixels_per_plane = 0;
	if (!detail::checked_mul_size_t(
	        static_cast<std::size_t>(desc.rows),
	        static_cast<std::size_t>(desc.cols), pixels_per_plane)) {
		detail::throw_codec_stage_exception(
		    detail::CodecStatusCode::invalid_argument, "create_payload_decoder",
		    "1-bit frame plane size exceeds size_t range");
	}
	std::size_t frame_bits = 0;
	if (!detail::checked_mul_size_t(
	        pixels_per_plane, static_cast<std::size_t>(desc.samples_per_pixel),
	        frame_bits)) {
		detail::throw_codec_stage_exception(
		    detail::CodecStatusCode::invalid_argument, "create_payload_decoder",
		    "1-bit frame bit count exceeds size_t range");
	}
	std::size_t total_bits = 0;
	if (!detail::checked_mul_size_t(
	        frame_bits, static_cast<std::size_t>(desc.frames), total_bits)) {
		detail::throw_codec_stage_exception(
		    detail::CodecStatusCode::invalid_argument, "create_payload_decoder",
		    "1-bit total bit count exceeds size_t range");
	}

	return support_detail::NativeOneBitPixelLayout{
	    .photometric = *photometric,
	    .planar = desc.planar_configuration == 1 ? Planar::planar
	                                             : Planar::interleaved,
	    .rows = desc.rows,
	    .cols = desc.cols,
	    .frames = desc.frames,
	    .samples_per_pixel = desc.samples_per_pixel,
	    .frame_bits = frame_bits,
	    .total_bits = total_bits,
	    .required_payload_bytes =
	        checked_add_seven_divide_by_eight_or_throw(total_bits, "create_payload_decoder"),
	};
}

[[nodiscard]] PixelLayout source_layout_from_descriptor_or_throw(
    const PixelPayloadDecodeDescriptor& desc) {
	if (desc.rows == 0 || desc.cols == 0 || desc.frames == 0 ||
	    desc.samples_per_pixel == 0) {
		detail::throw_codec_stage_exception(
		    detail::CodecStatusCode::invalid_argument, "create_payload_decoder",
		    "rows, cols, frames, and samples_per_pixel must be positive");
	}
	if (desc.pixel_representation > 1) {
		detail::throw_codec_stage_exception(
		    detail::CodecStatusCode::invalid_argument, "create_payload_decoder",
		    "pixel_representation must be 0 or 1");
	}
	if (desc.bits_allocated == 1) {
		try {
			return one_bit_layout_from_descriptor_or_throw(desc).decoded_source_layout();
		} catch (const std::exception& e) {
			detail::throw_codec_stage_exception(
			    detail::CodecStatusCode::invalid_argument, "create_payload_decoder",
			    "invalid 1-bit source layout: {}", e.what());
		}
	}

	const auto data_type = data_type_from_descriptor_or_throw(
	    desc.bits_allocated, desc.pixel_representation);
	const auto bits_stored =
	    desc.bits_stored != 0 ? desc.bits_stored : desc.bits_allocated;
	if (bits_stored == 0 || bits_stored > desc.bits_allocated) {
		detail::throw_codec_stage_exception(
		    detail::CodecStatusCode::invalid_argument, "create_payload_decoder",
		    "bits_stored must be in [1, bits_allocated]");
	}

	const auto photometric =
	    parse_photometric(trim_ascii_space(desc.photometric));
	if (!photometric.has_value()) {
		detail::throw_codec_stage_exception(
		    detail::CodecStatusCode::invalid_argument, "create_payload_decoder",
		    "unknown PhotometricInterpretation: {}", desc.photometric);
	}

	PixelLayout layout{
	    .data_type = data_type,
	    .photometric = *photometric,
	    .planar = desc.planar_configuration == 1 ? Planar::planar
	                                             : Planar::interleaved,
	    .reserved = 0,
	    .rows = desc.rows,
	    .cols = desc.cols,
	    .frames = desc.frames,
	    .samples_per_pixel = desc.samples_per_pixel,
	    .bits_stored = bits_stored,
	    .row_stride = 0,
	    .frame_stride = 0,
	};
	try {
		return layout.packed(1);
	} catch (const std::exception& e) {
		detail::throw_codec_stage_exception(
		    detail::CodecStatusCode::invalid_argument, "create_payload_decoder",
		    "invalid source layout: {}", e.what());
	}
}

void decode_frame_into_impl(const DicomFile& df, std::size_t frame_index,
    std::span<std::uint8_t> dst, const DecodePlan& plan,
    DecodeInfo* decode_info) {
	if (const auto transfer_syntax = df.transfer_syntax_uid();
	    transfer_syntax.is_uncompressed() && !transfer_syntax.is_encapsulated()) {
		if (const auto one_bit_layout =
		        support_detail::try_compute_native_one_bit_pixel_layout(df)) {
			const auto& pixel_data = df.dataset()["PixelData"_tag];
			if (!pixel_data || pixel_data.is_missing() ||
			    pixel_data.vr().is_pixel_sequence()) {
				detail::throw_frame_codec_stage_exception(frame_index,
				    detail::CodecStatusCode::invalid_argument,
				    "load_frame_source",
				    "native 1-bit decode requires binary PixelData");
			}
			support_detail::unpack_native_one_bit_frame_or_throw(
			    pixel_data.value_span(), *one_bit_layout, frame_index, dst,
			    plan.output_layout);
			if (decode_info != nullptr) {
				assign_native_one_bit_decode_info(plan.output_layout, *decode_info);
			}
			return;
		}
	}

	pixel_decoder_info abi_info{};
	pixel_decoder_info* abi_info_ptr = nullptr;
	if (decode_info != nullptr) {
		abi_info.struct_size = sizeof(pixel_decoder_info);
		abi_info.abi_version = PIXEL_DECODER_PLUGIN_ABI;
		abi_info_ptr = &abi_info;
	}

	try {
		detail::dispatch_decode_frame(df, frame_index, dst, plan, abi_info_ptr);
		if (decode_info != nullptr) {
			assign_decode_info(abi_info, plan.output_layout.photometric, *decode_info);
		}
	} catch (const diag::DicomException& ex) {
		detail::rethrow_codec_exception_at_boundary_or_throw(
		    "pixel::decode_frame_into", df, ex);
	}
}

void decode_all_frames_into_impl(const DicomFile& df, std::span<std::uint8_t> dst,
    const DecodePlan& plan, DecodeInfo* decode_info,
    const ExecutionObserver* observer) {
	if (const auto transfer_syntax = df.transfer_syntax_uid();
	    transfer_syntax.is_uncompressed() && !transfer_syntax.is_encapsulated()) {
		if (const auto one_bit_layout =
		        support_detail::try_compute_native_one_bit_pixel_layout(df)) {
			const auto frames = static_cast<std::size_t>(plan.output_layout.frames);
			if (plan.output_layout.frame_stride >
			    (std::numeric_limits<std::size_t>::max() / frames)) {
				detail::throw_codec_stage_exception(
				    detail::CodecStatusCode::invalid_argument, "validate_dst",
				    "decoded output size overflow for all frames");
			}
			const auto required_bytes = plan.output_layout.frame_stride * frames;
			if (dst.size() < required_bytes) {
				detail::throw_codec_stage_exception(
				    detail::CodecStatusCode::invalid_argument, "validate_dst",
				    "destination buffer is smaller than required decoded size");
			}
			const auto& pixel_data = df.dataset()["PixelData"_tag];
			if (!pixel_data || pixel_data.is_missing() ||
			    pixel_data.vr().is_pixel_sequence()) {
				detail::throw_codec_stage_exception(
				    detail::CodecStatusCode::invalid_argument,
				    "load_frame_source",
				    "native 1-bit decode requires binary PixelData");
			}
			for (std::size_t frame_index = 0; frame_index < frames; ++frame_index) {
				if (observer != nullptr && observer->should_cancel != nullptr &&
				    observer->should_cancel(observer->user_data)) {
					detail::throw_codec_stage_exception(
					    detail::CodecStatusCode::cancelled, "cancel",
					    "decode cancelled by observer after {} of {} frames",
					    frame_index, frames);
				}
				auto frame_dst = dst.subspan(
				    frame_index * plan.output_layout.frame_stride,
				    plan.output_layout.frame_stride);
				support_detail::unpack_native_one_bit_frame_or_throw(
				    pixel_data.value_span(), *one_bit_layout, frame_index,
				    frame_dst, plan.output_layout);
				if (observer != nullptr && observer->on_progress != nullptr) {
					observer->on_progress(
					    frame_index + 1u, frames, observer->user_data);
				}
			}
			if (decode_info != nullptr) {
				assign_native_one_bit_decode_info(plan.output_layout, *decode_info);
			}
			return;
		}
	}

	pixel_decoder_info abi_info{};
	pixel_decoder_info* abi_info_ptr = nullptr;
	if (decode_info != nullptr) {
		abi_info.struct_size = sizeof(pixel_decoder_info);
		abi_info.abi_version = PIXEL_DECODER_PLUGIN_ABI;
		abi_info_ptr = &abi_info;
	}

	try {
		detail::dispatch_decode_all_frames(df, dst, plan, observer, abi_info_ptr);
		if (decode_info != nullptr) {
			assign_decode_info(abi_info, plan.output_layout.photometric, *decode_info);
		}
	} catch (const diag::DicomException& ex) {
		detail::rethrow_codec_exception_at_boundary_or_throw(
		    "pixel::decode_all_frames_into", df, ex);
	}
}

} // namespace

std::optional<Photometric> parse_photometric(std::string_view text) noexcept {
	return detail::parse_photometric_text(trim_ascii_space(text));
}

std::string_view photometric_to_string(Photometric photometric) noexcept {
	return detail::to_photometric_text(photometric);
}

DecodePlan create_decode_plan(const DicomFile& df, const DecodeOptions& opt) {
	DecodePlan plan{};
	// Snapshot pixel metadata once so decode_frame_into() can stay on the hot path.
	if (const auto transfer_syntax = df.transfer_syntax_uid();
	    transfer_syntax.is_uncompressed() && !transfer_syntax.is_encapsulated()) {
		if (const auto one_bit_layout =
		        support_detail::try_compute_native_one_bit_pixel_layout(df)) {
			plan.options = opt;
			plan.output_layout =
			    detail::compute_decode_output_layout_or_throw(
			        df.path(), one_bit_layout->decoded_source_layout(), plan.options);
			return plan;
		}
	}
	const auto source_layout = support_detail::compute_decode_source_layout(df);
	// Store the caller policy unchanged so every decode entry point uses the same plan contract.
	plan.options = opt;
	// Precompute the exact destination layout expected by the decode path.
	plan.output_layout =
	    detail::compute_decode_output_layout_or_throw(df, source_layout, plan.options);
	return plan;
}

PixelPayloadDecoder::PixelPayloadDecoder(
    const PixelPayloadDecodeDescriptor& desc,
    std::span<const std::uint8_t> pixel_payload)
    : pixel_payload_(pixel_payload) {
	source_name_ = desc.source_name.empty() ? std::string{"<pixel-payload>"}
	                                        : std::string(desc.source_name);
	if (pixel_payload_.empty()) {
		detail::throw_codec_stage_exception(
		    detail::CodecStatusCode::invalid_argument, "create_payload_decoder",
		    "pixel payload is empty");
	}
	if (pixel_payload_.data() == nullptr) {
		detail::throw_codec_stage_exception(
		    detail::CodecStatusCode::invalid_argument, "create_payload_decoder",
		    "pixel payload pointer is null");
	}
	if (desc.expected_payload_length != 0 &&
	    desc.expected_payload_length != pixel_payload_.size()) {
		detail::throw_codec_stage_exception(
		    detail::CodecStatusCode::invalid_argument, "create_payload_decoder",
		    "pixel payload length mismatch: expected {} byte(s), got {} byte(s)",
		    desc.expected_payload_length, pixel_payload_.size());
	}
	const auto ts_text = trim_ascii_space(desc.transfer_syntax_uid);
	const auto transfer_syntax = uid::lookup(ts_text);
	if (!transfer_syntax.has_value() ||
	    transfer_syntax->uid_type() != UidType::TransferSyntax) {
		detail::throw_codec_stage_exception(
		    detail::CodecStatusCode::invalid_argument, "create_payload_decoder",
		    "unknown transfer syntax UID: {}", desc.transfer_syntax_uid);
	}
	transfer_syntax_uid_ = *transfer_syntax;
	source_layout_ = source_layout_from_descriptor_or_throw(desc);
	if (desc.bits_allocated == 1) {
		if (transfer_syntax_uid_.is_encapsulated()) {
			detail::throw_codec_stage_exception(
			    detail::CodecStatusCode::unsupported,
			    "create_payload_decoder",
			    "encapsulated 1-bit pixel payload decode is not supported");
		}
		const auto one_bit_layout = one_bit_layout_from_descriptor_or_throw(desc);
		native_one_bit_packed_ = true;
		native_one_bit_frame_bits_ = one_bit_layout.frame_bits;
		native_one_bit_required_payload_bytes_ =
		    one_bit_layout.required_payload_bytes;
	}

	std::size_t total_native_bytes = 0;
	if (native_one_bit_packed_) {
		total_native_bytes = native_one_bit_required_payload_bytes_;
	} else if (!detail::checked_mul_size_t(
	               static_cast<std::size_t>(source_layout_.frames),
	               source_layout_.frame_stride, total_native_bytes)) {
		detail::throw_codec_stage_exception(
		    detail::CodecStatusCode::invalid_argument, "create_payload_decoder",
		    "native pixel payload size exceeds size_t range");
	}

	if (!transfer_syntax_uid_.is_encapsulated()) {
		if (pixel_payload_.size() < total_native_bytes) {
			detail::throw_codec_stage_exception(
			    detail::CodecStatusCode::invalid_argument, "create_payload_decoder",
			    "native pixel payload is shorter than required frame bytes");
		}
		return;
	}

	if (desc.frame_fragments.empty()) {
		detail::throw_codec_stage_exception(
		    detail::CodecStatusCode::invalid_argument, "create_payload_decoder",
		    "encapsulated pixel payload requires frame_fragments");
	}

	std::string_view fragments_text = desc.frame_fragments;
	std::size_t frame_start = 0;
	frame_fragments_.reserve(source_layout_.frames);
	while (frame_start <= fragments_text.size()) {
		const auto frame_end = fragments_text.find(';', frame_start);
		const auto frame_text =
		    fragments_text.substr(frame_start,
		        frame_end == std::string_view::npos
		            ? std::string_view::npos
		            : frame_end - frame_start);
		if (frame_text.empty()) {
			detail::throw_codec_stage_exception(
			    detail::CodecStatusCode::invalid_argument, "parse_frame_fragments",
			    "frame fragment list contains an empty frame");
		}

		std::vector<Fragment> frame;
		std::size_t fragment_start = 0;
		while (fragment_start <= frame_text.size()) {
			const auto fragment_end = frame_text.find(',', fragment_start);
			const auto fragment_text =
			    frame_text.substr(fragment_start,
			        fragment_end == std::string_view::npos
			            ? std::string_view::npos
			            : fragment_end - fragment_start);
			if (fragment_text.empty()) {
				detail::throw_codec_stage_exception(
				    detail::CodecStatusCode::invalid_argument, "parse_frame_fragments",
				    "frame fragment list contains an empty fragment");
			}
			const auto colon = fragment_text.find(':');
			if (colon == std::string_view::npos ||
			    fragment_text.find(':', colon + 1u) != std::string_view::npos) {
				detail::throw_codec_stage_exception(
				    detail::CodecStatusCode::invalid_argument, "parse_frame_fragments",
				    "fragment must use offset:length syntax");
			}
			const auto offset = parse_fragment_size_or_throw(
			    fragment_text.substr(0, colon), "fragment offset");
			const auto length = parse_fragment_size_or_throw(
			    fragment_text.substr(colon + 1u), "fragment length");
			if (length == 0) {
				detail::throw_codec_stage_exception(
				    detail::CodecStatusCode::invalid_argument, "parse_frame_fragments",
				    "fragment length must be positive");
			}
			if (offset > pixel_payload_.size() ||
			    length > pixel_payload_.size() - offset) {
				detail::throw_codec_stage_exception(
				    detail::CodecStatusCode::invalid_argument, "parse_frame_fragments",
				    "fragment range is outside pixel payload: offset={} length={} payload={}",
				    offset, length, pixel_payload_.size());
			}
			frame.push_back(Fragment{.offset = offset, .length = length});

			if (fragment_end == std::string_view::npos) {
				break;
			}
			fragment_start = fragment_end + 1u;
		}
		frame_fragments_.push_back(std::move(frame));
		if (frame_fragments_.size() >
		    static_cast<std::size_t>(source_layout_.frames)) {
			detail::throw_codec_stage_exception(
			    detail::CodecStatusCode::invalid_argument, "parse_frame_fragments",
			    "frame_fragments describes more frames than descriptor.frames");
		}

		if (frame_end == std::string_view::npos) {
			break;
		}
		frame_start = frame_end + 1u;
	}

	if (frame_fragments_.size() != static_cast<std::size_t>(source_layout_.frames)) {
		detail::throw_codec_stage_exception(
		    detail::CodecStatusCode::invalid_argument, "parse_frame_fragments",
		    "frame_fragments frame count does not match descriptor.frames");
	}
}

DecodePlan PixelPayloadDecoder::create_decode_plan(const DecodeOptions& opt) const {
	DecodePlan plan{};
	plan.options = opt;
	plan.output_layout =
	    detail::compute_decode_output_layout_or_throw(
	        source_name_, source_layout_, plan.options);
	return plan;
}

void PixelPayloadDecoder::decode_into(std::size_t frame_index,
    std::span<std::uint8_t> dst, const DecodePlan& plan) const {
	std::vector<std::uint8_t> owned_prepared_source{};
	std::span<const std::uint8_t> prepared_source{};
	try {
		if (frame_index >= static_cast<std::size_t>(source_layout_.frames)) {
			detail::throw_frame_codec_stage_exception(frame_index,
			    detail::CodecStatusCode::invalid_argument, "validate_frame",
			    "frame index out of range");
		}
		if (plan.output_layout.empty()) {
			detail::throw_frame_codec_stage_exception(frame_index,
			    detail::CodecStatusCode::invalid_argument, "validate_plan",
			    "decode plan is not initialized; call create_decode_plan()");
		}
		if (dst.size() < plan.output_layout.frame_stride) {
			detail::throw_frame_codec_stage_exception(frame_index,
			    detail::CodecStatusCode::invalid_argument, "validate_dst",
			    "destination buffer is smaller than required decoded size");
		}
		if (native_one_bit_packed_) {
			support_detail::NativeOneBitPixelLayout one_bit_layout{
			    .photometric = source_layout_.photometric,
			    .planar = source_layout_.planar,
			    .rows = source_layout_.rows,
			    .cols = source_layout_.cols,
			    .frames = source_layout_.frames,
			    .samples_per_pixel = source_layout_.samples_per_pixel,
			    .frame_bits = native_one_bit_frame_bits_,
			    .total_bits = 0,
			    .required_payload_bytes = native_one_bit_required_payload_bytes_,
			};
			support_detail::unpack_native_one_bit_frame_or_throw(
			    pixel_payload_, one_bit_layout, frame_index, dst, plan.output_layout);
			return;
		}

		if (!transfer_syntax_uid_.is_encapsulated()) {
			std::size_t frame_offset = 0;
			if (!detail::checked_mul_size_t(
			        frame_index, source_layout_.frame_stride, frame_offset) ||
			    pixel_payload_.size() < frame_offset ||
			    pixel_payload_.size() - frame_offset <
			        source_layout_.frame_stride) {
				detail::throw_frame_codec_stage_exception(frame_index,
				    detail::CodecStatusCode::invalid_argument, "load_frame_source",
				    "native frame range is outside pixel payload");
			}
			prepared_source = pixel_payload_.subspan(
			    frame_offset, source_layout_.frame_stride);
		} else {
			const auto& fragments = frame_fragments_.at(frame_index);
			if (fragments.size() == 1) {
				const auto& fragment = fragments.front();
				prepared_source =
				    pixel_payload_.subspan(fragment.offset, fragment.length);
			} else {
				std::size_t total_size = 0;
				for (const auto& fragment : fragments) {
					if (fragment.length >
					    std::numeric_limits<std::size_t>::max() - total_size) {
						detail::throw_frame_codec_stage_exception(frame_index,
						    detail::CodecStatusCode::invalid_argument,
						    "load_frame_source",
						    "encapsulated frame size exceeds size_t range");
					}
					total_size += fragment.length;
				}
				owned_prepared_source.resize(total_size);
				std::size_t dst_offset = 0;
				for (const auto& fragment : fragments) {
					std::memcpy(owned_prepared_source.data() + dst_offset,
					    pixel_payload_.data() + fragment.offset, fragment.length);
					dst_offset += fragment.length;
				}
				prepared_source = std::span<const std::uint8_t>(
				    owned_prepared_source.data(), owned_prepared_source.size());
			}
		}

		detail::dispatch_decode_prepared_frame(transfer_syntax_uid_,
		    source_layout_, frame_index, prepared_source, dst, plan, nullptr);
	} catch (const diag::DicomException& ex) {
		detail::rethrow_codec_exception_at_boundary_or_throw(
		    "pixel::PixelPayloadDecoder::decode_into",
		    source_name_, transfer_syntax_uid_, ex);
	}
}

PixelBuffer PixelPayloadDecoder::pixel_buffer(
    std::size_t frame_index, const DecodePlan& plan) const {
	auto decoded = PixelBuffer::allocate(plan.output_layout.single_frame());
	decode_into(frame_index, decoded.bytes, plan);
	return decoded;
}

void decode_frame_into(const DicomFile& df, std::size_t frame_index,
    std::span<std::uint8_t> dst, const DecodePlan& plan) {
	decode_frame_into_impl(df, frame_index, dst, plan, nullptr);
}

void decode_frame_into(const DicomFile& df, std::size_t frame_index,
    std::span<std::uint8_t> dst, const DecodePlan& plan, DecodeInfo& decode_info) {
	decode_frame_into_impl(df, frame_index, dst, plan, &decode_info);
}

PixelBuffer decode_frame(
    const DicomFile& df, std::size_t frame_index, const DecodePlan& plan) {
	// Single-frame decode owns only one frame worth of storage even when the plan
	// was prepared for a multi-frame pixel payload.
	auto decoded = PixelBuffer::allocate(plan.output_layout.single_frame());

	// Reuse the same validated dispatch path as the span-based API.
	decode_frame_into(df, frame_index, decoded.bytes, plan);
	return decoded;
}

PixelBuffer decode_frame(const DicomFile& df, std::size_t frame_index,
    const DecodePlan& plan, DecodeInfo& decode_info) {
	auto decoded = PixelBuffer::allocate(plan.output_layout.single_frame());
	decode_frame_into(df, frame_index, decoded.bytes, plan, decode_info);
	return decoded;
}

void decode_all_frames_into(
    const DicomFile& df, std::span<std::uint8_t> dst, const DecodePlan& plan) {
	decode_all_frames_into_impl(df, dst, plan, nullptr, nullptr);
}

void decode_all_frames_into(const DicomFile& df, std::span<std::uint8_t> dst,
    const DecodePlan& plan, DecodeInfo& decode_info) {
	decode_all_frames_into_impl(df, dst, plan, &decode_info, nullptr);
}

PixelBuffer decode_all_frames(const DicomFile& df, const DecodePlan& plan) {
	// Full-volume decode owns exactly the storage implied by the planned output layout.
	auto decoded = PixelBuffer::allocate(plan.output_layout);

	// Reuse the existing batch path so validation and backend selection stay centralized.
	decode_all_frames_into(df, decoded.bytes, plan);
	return decoded;
}

PixelBuffer decode_all_frames(
    const DicomFile& df, const DecodePlan& plan, DecodeInfo& decode_info) {
	auto decoded = PixelBuffer::allocate(plan.output_layout);
	decode_all_frames_into(df, decoded.bytes, plan, decode_info);
	return decoded;
}

void decode_all_frames_into(const DicomFile& df, std::span<std::uint8_t> dst,
    const DecodePlan& plan, const ExecutionObserver* observer) {
	decode_all_frames_into_impl(df, dst, plan, nullptr, observer);
}

void decode_all_frames_into(const DicomFile& df, std::span<std::uint8_t> dst,
    const DecodePlan& plan, DecodeInfo& decode_info,
    const ExecutionObserver* observer) {
	decode_all_frames_into_impl(df, dst, plan, &decode_info, observer);
}

PixelBuffer decode_all_frames(
    const DicomFile& df, const DecodePlan& plan, const ExecutionObserver* observer) {
	// The owning convenience keeps observer-aware decode on the same dispatcher.
	auto decoded = PixelBuffer::allocate(plan.output_layout);

	// Reuse the observer-enabled batch path without duplicating scheduling logic.
	decode_all_frames_into(df, decoded.bytes, plan, observer);
	return decoded;
}

PixelBuffer decode_all_frames(
    const DicomFile& df, const DecodePlan& plan, DecodeInfo& decode_info,
    const ExecutionObserver* observer) {
	auto decoded = PixelBuffer::allocate(plan.output_layout);
	decode_all_frames_into(df, decoded.bytes, plan, decode_info, observer);
	return decoded;
}

} // namespace pixel

} // namespace dicom
