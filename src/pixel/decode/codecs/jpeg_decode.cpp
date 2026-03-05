#include "pixel/decode/core/decode_codec_impl_detail.hpp"
#include "pixel/registry/codec_registry.hpp"

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4819)
#endif
#include <turbojpeg.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

using namespace dicom::literals;

namespace dicom {
namespace pixel::detail {

namespace {

class TurboJpegHandleGuard {
public:
	explicit TurboJpegHandleGuard(tjhandle handle) noexcept : handle_(handle) {}
	~TurboJpegHandleGuard() {
		if (handle_) {
			tj3Destroy(handle_);
		}
	}

	TurboJpegHandleGuard(const TurboJpegHandleGuard&) = delete;
	TurboJpegHandleGuard& operator=(const TurboJpegHandleGuard&) = delete;

	[[nodiscard]] tjhandle get() const noexcept { return handle_; }

private:
	tjhandle handle_{nullptr};
};

bool sv_dtype_is_integral(DataType sv_dtype) noexcept {
	switch (sv_dtype) {
	case DataType::u8:
	case DataType::s8:
	case DataType::u16:
	case DataType::s16:
	case DataType::u32:
	case DataType::s32:
		return true;
	default:
		return false;
	}
}

template <typename... Args>
[[noreturn]] void throw_decode_error(fmt::format_string<Args...> format, Args... args) {
	throw std::runtime_error(fmt::vformat(format, fmt::make_format_args(args...)));
}

void validate_destination(std::span<std::uint8_t> dst,
    const DecodeStrides& dst_strides, Planar dst_planar, std::size_t rows, std::size_t cols,
    std::size_t samples_per_pixel, std::size_t bytes_per_sample) {
	const std::size_t dst_row_components =
	    (dst_planar == Planar::interleaved) ? samples_per_pixel : std::size_t{1};
	const std::size_t dst_min_row_bytes = cols * dst_row_components * bytes_per_sample;
	if (dst_strides.row < dst_min_row_bytes) {
		throw_decode_error(
		    "row stride too small (need>={}, got={})",
		    dst_min_row_bytes, dst_strides.row);
	}

	std::size_t min_frame_bytes = dst_strides.row * rows;
	if (dst_planar == Planar::planar) {
		min_frame_bytes *= samples_per_pixel;
	}
	if (dst_strides.frame < min_frame_bytes) {
		throw_decode_error(
		    "frame stride too small (need>={}, got={})",
		    min_frame_bytes, dst_strides.frame);
	}
	if (dst.size() < dst_strides.frame) {
		throw_decode_error(
		    "destination too small (need={}, got={})",
		    dst_strides.frame, dst.size());
	}
}

[[noreturn]] void throw_turbojpeg_error(tjhandle handle, const char* action) {
	const char* message = tj3GetErrorStr(handle);
	if (!message || message[0] == '\0') {
		message = "unknown error";
	}
	throw_decode_error(
	    "{} ({})", action, message);
}

[[nodiscard]] std::size_t colorspace_component_count(int colorspace) noexcept {
	switch (colorspace) {
	case TJCS_GRAY:
		return 1;
	case TJCS_RGB:
	case TJCS_YCbCr:
		return 3;
	case TJCS_CMYK:
	case TJCS_YCCK:
		return 4;
	default:
		return 0;
	}
}

[[nodiscard]] int pixel_format_for_samples(std::size_t samples_per_pixel) {
	switch (samples_per_pixel) {
	case 1:
		return TJPF_GRAY;
	case 3:
		return TJPF_RGB;
	case 4:
		return TJPF_CMYK;
	default:
		return TJPF_UNKNOWN;
	}
}

[[nodiscard]] int checked_int_stride(const char* path_name, std::size_t stride) {
	if (stride > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
		throw_decode_error(
		    "{} stride exceeds int ({})",
		    path_name, stride);
	}
	return static_cast<int>(stride);
}

[[nodiscard]] bool is_pointer_aligned(const void* pointer, std::size_t alignment) noexcept {
	return (reinterpret_cast<std::uintptr_t>(pointer) % alignment) == 0;
}

std::optional<std::size_t> find_sequential_sos_se_patch_offset(
    std::span<const std::uint8_t> codestream) {
	// Some DICOM JPEG Extended codestreams use SOF1 with SOS Se=0.
	// Libjpeg-turbo rejects these as invalid sequential JPEG, while other
	// medical stacks often decode them by treating Se as 63.
	if (codestream.size() < 4 || codestream[0] != 0xFF || codestream[1] != 0xD8) {
		return std::nullopt;
	}

	std::size_t i = 2;
	bool saw_sof1 = false;
	while (i + 1 < codestream.size()) {
		if (codestream[i] != 0xFF) {
			// Segment stream before SOS should be marker-aligned.
			return std::nullopt;
		}

		std::size_t marker_index = i + 1;
		while (marker_index < codestream.size() && codestream[marker_index] == 0xFF) {
			++marker_index;
		}
		if (marker_index >= codestream.size()) {
			return std::nullopt;
		}
		const auto marker = codestream[marker_index];
		i = marker_index + 1;

		if (marker == 0xD9) {
			return std::nullopt;
		}
		if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
			continue;
		}
		if (i + 1 >= codestream.size()) {
			return std::nullopt;
		}

		const std::size_t segment_length =
		    (static_cast<std::size_t>(codestream[i]) << 8) |
		    static_cast<std::size_t>(codestream[i + 1]);
		if (segment_length < 2 || i + segment_length > codestream.size()) {
			return std::nullopt;
		}

		if (marker == 0xC1) {
			saw_sof1 = true;
		} else if (marker == 0xDA) {
			if (!saw_sof1) {
				return std::nullopt;
			}

			const std::size_t payload_offset = i + 2;
			const std::size_t payload_length = segment_length - 2;
			if (payload_length < 4) {
				return std::nullopt;
			}

			const auto component_count = static_cast<std::size_t>(codestream[payload_offset]);
			const std::size_t required_length = 1 + 2 * component_count + 3;
			if (payload_length < required_length) {
				return std::nullopt;
			}

			const std::size_t ss_offset = payload_offset + 1 + 2 * component_count;
			const std::size_t se_offset = ss_offset + 1;
			const std::size_t ahal_offset = ss_offset + 2;
			if (codestream[ss_offset] == 0x00 && codestream[se_offset] == 0x00 &&
			    codestream[ahal_offset] == 0x00) {
				return se_offset;
			}
			return std::nullopt;
		}

		i += segment_length;
	}

	return std::nullopt;
}

void decode_with_turbojpeg(tjhandle handle,
    std::span<const std::uint8_t> source, void* destination,
    int destination_pitch_samples, int destination_pixel_format,
    int precision_bits) {
	int rc = -1;
	if (precision_bits <= 8) {
		rc = tj3Decompress8(
		    handle, source.data(), source.size(), static_cast<unsigned char*>(destination),
		    destination_pitch_samples, destination_pixel_format);
	} else if (precision_bits <= 12) {
		rc = tj3Decompress12(
		    handle, source.data(), source.size(), static_cast<short*>(destination),
		    destination_pitch_samples, destination_pixel_format);
	} else {
		rc = tj3Decompress16(
		    handle, source.data(), source.size(), static_cast<unsigned short*>(destination),
		    destination_pitch_samples, destination_pixel_format);
	}

	if (rc != 0) {
		throw_turbojpeg_error(handle, "JPEG decode failed");
	}
}

void validate_decoded_header(const pixel::PixelDataInfo& info, std::size_t rows,
    std::size_t cols,
    std::size_t samples_per_pixel, std::size_t src_bytes_per_sample,
    int decoded_width, int decoded_height, int decoded_precision, int decoded_lossless,
    int decoded_colorspace) {
	if (decoded_width != static_cast<int>(cols) ||
	    decoded_height != static_cast<int>(rows)) {
		throw_decode_error(
		    "JPEG decoded dimensions mismatch (decoded={}x{}, expected={}x{})",
		    decoded_height, decoded_width, rows, cols);
	}

	if (decoded_precision < 2 || decoded_precision > 16) {
		throw_decode_error(
		    "JPEG decoded precision is invalid ({})",
		    decoded_precision);
	}

	const auto decoded_components = colorspace_component_count(decoded_colorspace);
	if (decoded_components == 0) {
		throw_decode_error(
		    "JPEG colorspace {} is unsupported",
		    decoded_colorspace);
	}
	if (decoded_components != samples_per_pixel) {
		throw_decode_error(
		    "JPEG component count mismatch (decoded={}, expected={})",
		    decoded_components, samples_per_pixel);
	}

	if (info.ts.is_jpeg_lossless() && decoded_lossless == 0) {
		throw_decode_error(
		    "transfer syntax is JPEG lossless but codestream is lossy");
	}
	if (info.ts.is_jpeg_baseline() && decoded_lossless != 0) {
		throw_decode_error(
		    "transfer syntax is JPEG lossy but codestream is lossless");
	}

	const auto max_output_bits = static_cast<int>(src_bytes_per_sample * 8);
	if (decoded_precision > max_output_bits) {
		throw_decode_error(
		    "JPEG decoded precision {} exceeds output {} bits",
		    decoded_precision, max_output_bits);
	}
	// DICOM metadata and codestream header can disagree in practice.
	// Reject only when the decoded precision requires a wider storage width.
	if (info.bits_stored > 0 && decoded_precision > info.bits_stored &&
	    (static_cast<unsigned int>(decoded_precision) + 7u) / 8u >
	        (static_cast<unsigned int>(info.bits_stored) + 7u) / 8u) {
		throw_decode_error(
		    "JPEG decoded precision {} exceeds BitsStored {}",
		    decoded_precision, info.bits_stored);
	}

	if (decoded_precision <= 8 && src_bytes_per_sample != 1) {
		throw_decode_error(
		    "JPEG decoded precision {} requires 8-bit output but sv_dtype uses {} bytes",
		    decoded_precision, src_bytes_per_sample);
	}
	if (decoded_precision > 8 && src_bytes_per_sample != 2) {
		throw_decode_error(
		    "JPEG decoded precision {} requires 16-bit output but sv_dtype uses {} bytes",
		    decoded_precision, src_bytes_per_sample);
	}
}

} // namespace

bool decode_jpeg_into(const pixel::PixelDataInfo& info,
    const ModalityValueTransform& modality_value_transform,
    std::span<std::uint8_t> dst,
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
		if (!info.ts.is_jpeg_family() || info.ts.is_jpegls() || info.ts.is_jpeg2000() ||
		    info.ts.is_jpegxl()) {
			return fail(CodecStatusCode::unsupported, "validate",
			    "transfer syntax is not supported by libjpeg-turbo path");
		}
		if (info.rows <= 0 || info.cols <= 0 || info.samples_per_pixel <= 0) {
			return fail(CodecStatusCode::invalid_argument, "validate",
			    "invalid Rows/Columns/SamplesPerPixel");
		}
		if (info.frames <= 0) {
			return fail(CodecStatusCode::invalid_argument, "validate",
			    "invalid NumberOfFrames");
		}

		const auto samples_per_pixel_value = info.samples_per_pixel;
		if (samples_per_pixel_value != 1 && samples_per_pixel_value != 3 &&
		    samples_per_pixel_value != 4) {
			return fail(CodecStatusCode::unsupported, "validate",
			    "only SamplesPerPixel=1/3/4 is supported in current JPEG path");
		}
		const auto samples_per_pixel = static_cast<std::size_t>(samples_per_pixel_value);
		if (opt.to_modality_value && samples_per_pixel != 1) {
			return fail(CodecStatusCode::invalid_argument, "validate",
			    "scaled output supports SamplesPerPixel=1 only");
		}
		if (!sv_dtype_is_integral(info.sv_dtype)) {
			return fail(CodecStatusCode::unsupported, "validate",
			    "JPEG supports integral sv_dtype only");
		}

		const auto src_bytes_per_sample = sv_dtype_bytes(info.sv_dtype);
		if (src_bytes_per_sample == 0 || src_bytes_per_sample > 2) {
			return fail(CodecStatusCode::unsupported, "validate",
			    "JPEG supports integral sv_dtype up to 16-bit only");
		}

		const auto rows = static_cast<std::size_t>(info.rows);
		const auto cols = static_cast<std::size_t>(info.cols);
		const auto dst_bytes_per_sample =
		    opt.to_modality_value ? sizeof(float) : src_bytes_per_sample;
		try {
			validate_destination(dst, dst_strides, opt.planar_out, rows, cols,
			    samples_per_pixel, dst_bytes_per_sample);
		} catch (const std::bad_alloc&) {
			return fail(CodecStatusCode::internal_error, "allocate",
			    "memory allocation failed");
		} catch (const std::exception& e) {
			return fail(CodecStatusCode::invalid_argument, "validate", e.what());
		} catch (...) {
			return fail(CodecStatusCode::backend_error, "validate",
			    "non-standard exception");
		}

			const auto frame_source = prepared_source;
			if (frame_source.empty()) {
				return fail(CodecStatusCode::invalid_argument, "load_frame_source",
				    "JPEG frame has empty codestream");
			}
			std::span<const std::uint8_t> decode_source = frame_source;
			std::vector<std::uint8_t> patched_source{};
			if (const auto se_patch_offset =
			        find_sequential_sos_se_patch_offset(frame_source)) {
				patched_source.assign(frame_source.begin(), frame_source.end());
				patched_source[*se_patch_offset] = 0x3F;
				decode_source = std::span<const std::uint8_t>(patched_source);
			}

		TurboJpegHandleGuard handle(tj3Init(TJINIT_DECOMPRESS));
		if (!handle.get()) {
			return fail(CodecStatusCode::backend_error, "decode_frame",
			    "failed to initialize libjpeg-turbo decompressor");
		}

		if (tj3DecompressHeader(handle.get(), decode_source.data(), decode_source.size()) !=
		    0) {
			throw_turbojpeg_error(handle.get(), "JPEG header decode failed");
		}

		const int decoded_width = tj3Get(handle.get(), TJPARAM_JPEGWIDTH);
		const int decoded_height = tj3Get(handle.get(), TJPARAM_JPEGHEIGHT);
		const int decoded_precision = tj3Get(handle.get(), TJPARAM_PRECISION);
		const int decoded_lossless = tj3Get(handle.get(), TJPARAM_LOSSLESS);
		const int decoded_colorspace = tj3Get(handle.get(), TJPARAM_COLORSPACE);

		validate_decoded_header(info, rows, cols, samples_per_pixel,
		    src_bytes_per_sample, decoded_width, decoded_height, decoded_precision,
		    decoded_lossless, decoded_colorspace);

		const int pixel_format = pixel_format_for_samples(samples_per_pixel);
		if (pixel_format == TJPF_UNKNOWN) {
			return fail(CodecStatusCode::unsupported, "validate",
			    "unsupported JPEG pixel format for SamplesPerPixel");
		}

		const std::size_t src_row_bytes =
		    cols * samples_per_pixel * src_bytes_per_sample;
		const std::size_t src_row_samples = src_row_bytes / src_bytes_per_sample;
		const bool requires_u16_alignment = decoded_precision > 8;
		const bool can_decode_directly = !opt.to_modality_value &&
		    (samples_per_pixel == 1 || opt.planar_out == Planar::interleaved) &&
		    (!requires_u16_alignment ||
		        is_pointer_aligned(dst.data(), alignof(std::uint16_t)));

		if (can_decode_directly) {
			if (dst_strides.row % src_bytes_per_sample != 0) {
				return fail(CodecStatusCode::invalid_argument, "validate",
				    "destination row stride is not aligned to sample size");
			}
			const int destination_pitch_samples = checked_int_stride(
			    "destination", dst_strides.row / src_bytes_per_sample);
			decode_with_turbojpeg(handle.get(), decode_source,
			    dst.data(), destination_pitch_samples, pixel_format,
			    decoded_precision);
			return true;
		}

		if (src_bytes_per_sample == 1) {
			std::vector<std::uint8_t> decoded(src_row_bytes * rows);
			const int source_pitch_samples =
			    checked_int_stride("source", src_row_samples);
			decode_with_turbojpeg(handle.get(), decode_source,
			    decoded.data(), source_pitch_samples, pixel_format,
			    decoded_precision);

			if (opt.to_modality_value) {
				decode_mono_scaled_into_f32(
				    modality_value_transform, info, decoded.data(), dst, dst_strides,
				    rows, cols, src_row_bytes);
				return true;
			}

			const auto transform =
			    select_planar_transform(Planar::interleaved, opt.planar_out);
			run_planar_transform_copy(transform, src_bytes_per_sample, decoded.data(),
			    dst.data(), rows, cols, samples_per_pixel, src_row_bytes,
			    dst_strides.row);
			return true;
		}

		std::vector<std::uint16_t> decoded(src_row_samples * rows);
		const int source_pitch_samples = checked_int_stride("source", src_row_samples);
		decode_with_turbojpeg(handle.get(), decode_source,
		    decoded.data(), source_pitch_samples, pixel_format, decoded_precision);
		const auto* decoded_bytes =
		    reinterpret_cast<const std::uint8_t*>(decoded.data());

		if (opt.to_modality_value) {
			decode_mono_scaled_into_f32(
			    modality_value_transform, info, decoded_bytes, dst, dst_strides,
			    rows, cols, src_row_bytes);
			return true;
		}

		const auto transform = select_planar_transform(Planar::interleaved, opt.planar_out);
		run_planar_transform_copy(transform, src_bytes_per_sample, decoded_bytes,
		    dst.data(), rows, cols, samples_per_pixel, src_row_bytes, dst_strides.row);
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

} // namespace pixel::detail
} // namespace dicom
