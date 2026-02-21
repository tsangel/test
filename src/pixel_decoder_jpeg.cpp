#include "pixel_decoder_detail.hpp"

#include "diagnostics.h"

#include <turbojpeg.h>

#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

using namespace dicom::literals;
namespace diag = dicom::diag;

namespace dicom {
namespace pixel::detail {

namespace {

struct jpeg_frame_buffer {
	std::vector<std::uint8_t> owned{};
	std::span<const std::uint8_t> view{};
};

class turbojpeg_handle_guard {
public:
	explicit turbojpeg_handle_guard(tjhandle handle) noexcept : handle_(handle) {}
	~turbojpeg_handle_guard() {
		if (handle_) {
			tj3Destroy(handle_);
		}
	}

	turbojpeg_handle_guard(const turbojpeg_handle_guard&) = delete;
	turbojpeg_handle_guard& operator=(const turbojpeg_handle_guard&) = delete;

	[[nodiscard]] tjhandle get() const noexcept { return handle_; }

private:
	tjhandle handle_{nullptr};
};

bool sv_dtype_is_integral(dtype sv_dtype) noexcept {
	switch (sv_dtype) {
	case dtype::u8:
	case dtype::s8:
	case dtype::u16:
	case dtype::s16:
	case dtype::u32:
	case dtype::s32:
		return true;
	default:
		return false;
	}
}

void validate_destination(const DicomFile& df, std::span<std::uint8_t> dst,
    const strides& dst_strides, planar dst_planar, std::size_t rows, std::size_t cols,
    std::size_t samples_per_pixel, std::size_t bytes_per_sample) {
	const std::size_t dst_row_components =
	    (dst_planar == planar::interleaved) ? samples_per_pixel : std::size_t{1};
	const std::size_t dst_min_row_bytes = cols * dst_row_components * bytes_per_sample;
	if (dst_strides.row < dst_min_row_bytes) {
		diag::error_and_throw(
		    "pixel::decode_into file={} reason=row stride too small (need>={}, got={})",
		    df.path(), dst_min_row_bytes, dst_strides.row);
	}

	std::size_t min_frame_bytes = dst_strides.row * rows;
	if (dst_planar == planar::planar) {
		min_frame_bytes *= samples_per_pixel;
	}
	if (dst_strides.frame < min_frame_bytes) {
		diag::error_and_throw(
		    "pixel::decode_into file={} reason=frame stride too small (need>={}, got={})",
		    df.path(), min_frame_bytes, dst_strides.frame);
	}
	if (dst.size() < dst_strides.frame) {
		diag::error_and_throw(
		    "pixel::decode_into file={} reason=destination too small (need={}, got={})",
		    df.path(), dst_strides.frame, dst.size());
	}
}

jpeg_frame_buffer load_jpeg_frame_buffer(const DicomFile& df, std::size_t frame_index) {
	const auto& ds = df.dataset();
	const auto& pixel_data = ds["PixelData"_tag];
	if (!pixel_data || !pixel_data.vr().is_pixel_sequence()) {
		diag::error_and_throw(
		    "pixel::decode_into file={} reason=JPEG transfer syntax requires encapsulated PixelData",
		    df.path());
	}

	const auto* pixel_sequence = pixel_data.as_pixel_sequence();
	if (!pixel_sequence) {
		diag::error_and_throw(
		    "pixel::decode_into file={} reason=JPEG pixel sequence is missing",
		    df.path());
	}

	const auto frame_count = pixel_sequence->number_of_frames();
	if (frame_index >= frame_count) {
		diag::error_and_throw(
		    "pixel::decode_into file={} frame={} reason=JPEG frame index out of range (frames={})",
		    df.path(), frame_index, frame_count);
	}

	const auto* frame = pixel_sequence->frame(frame_index);
	if (!frame) {
		diag::error_and_throw(
		    "pixel::decode_into file={} frame={} reason=JPEG frame is missing",
		    df.path(), frame_index);
	}

	jpeg_frame_buffer source{};
	if (frame->encoded_data_size() != 0) {
		source.view = frame->encoded_data_view();
		return source;
	}

	const auto& fragments = frame->fragments();
	if (fragments.empty()) {
		diag::error_and_throw(
		    "pixel::decode_into file={} frame={} reason=JPEG frame has no fragments",
		    df.path(), frame_index);
	}
	for (const auto& fragment : fragments) {
		if (fragment.length == 0) {
			diag::error_and_throw(
			    "pixel::decode_into file={} frame={} reason=JPEG zero-length fragment is not supported",
			    df.path(), frame_index);
		}
	}

	const auto* stream = pixel_sequence->stream();
	if (!stream) {
		diag::error_and_throw(
		    "pixel::decode_into file={} frame={} reason=JPEG pixel sequence stream is missing",
		    df.path(), frame_index);
	}

	if (fragments.size() == 1) {
		const auto fragment = fragments.front();
		source.view = stream->get_span(fragment.offset, fragment.length);
		return source;
	}

	source.owned = frame->coalesce_encoded_data(*stream);
	source.view = std::span<const std::uint8_t>(source.owned);
	return source;
}

[[noreturn]] void throw_turbojpeg_error(const DicomFile& df, std::size_t frame_index,
    tjhandle handle, const char* action) {
	const char* message = tj3GetErrorStr(handle);
	if (!message || message[0] == '\0') {
		message = "unknown error";
	}
	diag::error_and_throw(
	    "pixel::decode_into file={} frame={} reason={} ({})",
	    df.path(), frame_index, action, message);
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

[[nodiscard]] int checked_int_stride(const DicomFile& df, const char* path_name, std::size_t stride) {
	if (stride > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
		diag::error_and_throw(
		    "pixel::decode_into file={} reason={} stride exceeds int ({})",
		    df.path(), path_name, stride);
	}
	return static_cast<int>(stride);
}

[[nodiscard]] bool is_pointer_aligned(const void* pointer, std::size_t alignment) noexcept {
	return (reinterpret_cast<std::uintptr_t>(pointer) % alignment) == 0;
}

std::optional<std::size_t> find_sequential_sos_se_patch_offset(
    std::span<const std::uint8_t> codestream) {
	// Some legacy DICOM JPEG Extended codestreams use SOF1 with SOS Se=0.
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

void decode_with_turbojpeg(const DicomFile& df, std::size_t frame_index, tjhandle handle,
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
		throw_turbojpeg_error(df, frame_index, handle, "JPEG decode failed");
	}
}

void validate_decoded_header(const DicomFile& df, std::size_t frame_index,
    const DicomFile::pixel_info_t& info, std::size_t rows, std::size_t cols,
    std::size_t samples_per_pixel, std::size_t src_bytes_per_sample,
    int decoded_width, int decoded_height, int decoded_precision, int decoded_lossless,
    int decoded_colorspace) {
	if (decoded_width != static_cast<int>(cols) ||
	    decoded_height != static_cast<int>(rows)) {
		diag::error_and_throw(
		    "pixel::decode_into file={} frame={} reason=JPEG decoded dimensions mismatch (decoded={}x{}, expected={}x{})",
		    df.path(), frame_index, decoded_height, decoded_width, rows, cols);
	}

	if (decoded_precision < 2 || decoded_precision > 16) {
		diag::error_and_throw(
		    "pixel::decode_into file={} frame={} reason=JPEG decoded precision is invalid ({})",
		    df.path(), frame_index, decoded_precision);
	}

	const auto decoded_components = colorspace_component_count(decoded_colorspace);
	if (decoded_components == 0) {
		diag::error_and_throw(
		    "pixel::decode_into file={} frame={} reason=JPEG colorspace {} is unsupported",
		    df.path(), frame_index, decoded_colorspace);
	}
	if (decoded_components != samples_per_pixel) {
		diag::error_and_throw(
		    "pixel::decode_into file={} frame={} reason=JPEG component count mismatch (decoded={}, expected={})",
		    df.path(), frame_index, decoded_components, samples_per_pixel);
	}

	if (info.ts.is_jpeg_lossless() && decoded_lossless == 0) {
		diag::error_and_throw(
		    "pixel::decode_into file={} frame={} reason=transfer syntax is JPEG lossless but codestream is lossy",
		    df.path(), frame_index);
	}
	if (info.ts.is_jpeg_baseline() && decoded_lossless != 0) {
		diag::error_and_throw(
		    "pixel::decode_into file={} frame={} reason=transfer syntax is JPEG lossy but codestream is lossless",
		    df.path(), frame_index);
	}

	const auto max_output_bits = static_cast<int>(src_bytes_per_sample * 8);
	if (decoded_precision > max_output_bits) {
		diag::error_and_throw(
		    "pixel::decode_into file={} frame={} reason=JPEG decoded precision {} exceeds output {} bits",
		    df.path(), frame_index, decoded_precision, max_output_bits);
	}
	if (info.bits_allocated > 0 && decoded_precision > info.bits_allocated) {
		diag::error_and_throw(
		    "pixel::decode_into file={} frame={} reason=JPEG decoded precision {} exceeds BitsAllocated {}",
		    df.path(), frame_index, decoded_precision, info.bits_allocated);
	}

	if (decoded_precision <= 8 && src_bytes_per_sample != 1) {
		diag::error_and_throw(
		    "pixel::decode_into file={} frame={} reason=JPEG decoded precision {} requires 8-bit output but sv_dtype uses {} bytes",
		    df.path(), frame_index, decoded_precision, src_bytes_per_sample);
	}
	if (decoded_precision > 8 && src_bytes_per_sample != 2) {
		diag::error_and_throw(
		    "pixel::decode_into file={} frame={} reason=JPEG decoded precision {} requires 16-bit output but sv_dtype uses {} bytes",
		    df.path(), frame_index, decoded_precision, src_bytes_per_sample);
	}
}

} // namespace

void decode_jpeg_into(const DicomFile& df, const DicomFile::pixel_info_t& info,
    std::size_t frame_index, std::span<std::uint8_t> dst,
    const strides& dst_strides, const decode_opts& opt) {
	if (!info.has_pixel_data) {
		diag::error_and_throw(
		    "pixel::decode_into file={} reason=sv_dtype is unknown", df.path());
	}
	if (!info.ts.is_jpeg_family() || info.ts.is_jpegls() || info.ts.is_jpeg2000() ||
	    info.ts.is_jpegxl()) {
		diag::error_and_throw(
		    "pixel::decode_into file={} reason=transfer syntax is not supported by libjpeg-turbo path ({})",
		    df.path(), df.transfer_syntax_uid().value());
	}
	if (info.rows <= 0 || info.cols <= 0 || info.samples_per_pixel <= 0) {
		diag::error_and_throw(
		    "pixel::decode_into file={} reason=invalid Rows/Columns/SamplesPerPixel",
		    df.path());
	}
	if (info.frames <= 0) {
		diag::error_and_throw(
		    "pixel::decode_into file={} reason=invalid NumberOfFrames",
		    df.path());
	}

	const auto samples_per_pixel_value = info.samples_per_pixel;
	if (samples_per_pixel_value != 1 && samples_per_pixel_value != 3 &&
	    samples_per_pixel_value != 4) {
		diag::error_and_throw(
		    "pixel::decode_into file={} reason=only SamplesPerPixel=1/3/4 is supported in current JPEG path",
		    df.path());
	}
	const auto samples_per_pixel = static_cast<std::size_t>(samples_per_pixel_value);
	if (opt.scaled && samples_per_pixel != 1) {
		diag::error_and_throw(
		    "pixel::decode_into file={} reason=scaled output supports SamplesPerPixel=1 only",
		    df.path());
	}
	if (!sv_dtype_is_integral(info.sv_dtype)) {
		diag::error_and_throw(
		    "pixel::decode_into file={} reason=JPEG supports integral sv_dtype only",
		    df.path());
	}

	const auto src_bytes_per_sample = sv_dtype_bytes(info.sv_dtype);
	if (src_bytes_per_sample == 0 || src_bytes_per_sample > 2) {
		diag::error_and_throw(
		    "pixel::decode_into file={} reason=JPEG supports integral sv_dtype up to 16-bit only",
		    df.path());
	}

	const auto frame_count = static_cast<std::size_t>(info.frames);
	if (frame_index >= frame_count) {
		diag::error_and_throw(
		    "pixel::decode_into file={} frame={} reason=frame index out of range (frames={})",
		    df.path(), frame_index, frame_count);
	}

	const auto rows = static_cast<std::size_t>(info.rows);
	const auto cols = static_cast<std::size_t>(info.cols);
	const auto dst_bytes_per_sample = opt.scaled ? sizeof(float) : src_bytes_per_sample;
	validate_destination(
	    df, dst, dst_strides, opt.planar_out, rows, cols, samples_per_pixel, dst_bytes_per_sample);

	const auto frame_source = load_jpeg_frame_buffer(df, frame_index);
	if (frame_source.view.empty()) {
		diag::error_and_throw(
		    "pixel::decode_into file={} frame={} reason=JPEG frame has empty codestream",
		    df.path(), frame_index);
	}
	std::span<const std::uint8_t> decode_source = frame_source.view;
	std::vector<std::uint8_t> patched_source{};
	if (const auto se_patch_offset = find_sequential_sos_se_patch_offset(frame_source.view)) {
		patched_source.assign(frame_source.view.begin(), frame_source.view.end());
		patched_source[*se_patch_offset] = 0x3F;
		decode_source = std::span<const std::uint8_t>(patched_source);
	}

	turbojpeg_handle_guard handle(tj3Init(TJINIT_DECOMPRESS));
	if (!handle.get()) {
		diag::error_and_throw(
		    "pixel::decode_into file={} frame={} reason=failed to initialize libjpeg-turbo decompressor",
		    df.path(), frame_index);
	}

	if (tj3DecompressHeader(handle.get(), decode_source.data(), decode_source.size()) != 0) {
		throw_turbojpeg_error(df, frame_index, handle.get(), "JPEG header decode failed");
	}

	const int decoded_width = tj3Get(handle.get(), TJPARAM_JPEGWIDTH);
	const int decoded_height = tj3Get(handle.get(), TJPARAM_JPEGHEIGHT);
	const int decoded_precision = tj3Get(handle.get(), TJPARAM_PRECISION);
	const int decoded_lossless = tj3Get(handle.get(), TJPARAM_LOSSLESS);
	const int decoded_colorspace = tj3Get(handle.get(), TJPARAM_COLORSPACE);

	validate_decoded_header(
	    df, frame_index, info, rows, cols, samples_per_pixel, src_bytes_per_sample,
	    decoded_width, decoded_height, decoded_precision, decoded_lossless,
	    decoded_colorspace);

	const int pixel_format = pixel_format_for_samples(samples_per_pixel);
	if (pixel_format == TJPF_UNKNOWN) {
		diag::error_and_throw(
		    "pixel::decode_into file={} frame={} reason=unsupported JPEG pixel format for SamplesPerPixel={}",
		    df.path(), frame_index, samples_per_pixel);
	}

	const std::size_t src_row_bytes = cols * samples_per_pixel * src_bytes_per_sample;
	const std::size_t src_row_samples = src_row_bytes / src_bytes_per_sample;
	const bool requires_u16_alignment = decoded_precision > 8;
	const bool can_decode_directly = !opt.scaled &&
	    (samples_per_pixel == 1 || opt.planar_out == planar::interleaved) &&
	    (!requires_u16_alignment || is_pointer_aligned(dst.data(), alignof(std::uint16_t)));

	if (can_decode_directly) {
		if (dst_strides.row % src_bytes_per_sample != 0) {
			diag::error_and_throw(
			    "pixel::decode_into file={} frame={} reason=destination row stride is not aligned to sample size {}",
			    df.path(), frame_index, src_bytes_per_sample);
		}
		const int destination_pitch_samples = checked_int_stride(
		    df, "destination", dst_strides.row / src_bytes_per_sample);
		decode_with_turbojpeg(df, frame_index, handle.get(), decode_source,
		    dst.data(), destination_pitch_samples, pixel_format, decoded_precision);
		return;
	}

	if (src_bytes_per_sample == 1) {
		std::vector<std::uint8_t> decoded(src_row_bytes * rows);
		const int source_pitch_samples = checked_int_stride(df, "source", src_row_samples);
		decode_with_turbojpeg(df, frame_index, handle.get(), decode_source,
		    decoded.data(), source_pitch_samples, pixel_format, decoded_precision);

		if (opt.scaled) {
			decode_mono_scaled_into_f32(
			    df, info, decoded.data(), dst, dst_strides, rows, cols, src_row_bytes);
			return;
		}

		const auto transform = select_planar_transform(planar::interleaved, opt.planar_out);
		run_planar_transform_copy(transform, src_bytes_per_sample, false,
		    decoded.data(), dst.data(), rows, cols, samples_per_pixel,
		    src_row_bytes, dst_strides.row);
		return;
	}

	std::vector<std::uint16_t> decoded(src_row_samples * rows);
	const int source_pitch_samples = checked_int_stride(df, "source", src_row_samples);
	decode_with_turbojpeg(df, frame_index, handle.get(), decode_source,
	    decoded.data(), source_pitch_samples, pixel_format, decoded_precision);
	const auto* decoded_bytes = reinterpret_cast<const std::uint8_t*>(decoded.data());

	if (opt.scaled) {
		decode_mono_scaled_into_f32(
		    df, info, decoded_bytes, dst, dst_strides, rows, cols, src_row_bytes);
		return;
	}

	const auto transform = select_planar_transform(planar::interleaved, opt.planar_out);
	run_planar_transform_copy(transform, src_bytes_per_sample, false,
	    decoded_bytes, dst.data(), rows, cols, samples_per_pixel,
	    src_row_bytes, dst_strides.row);
}

} // namespace pixel::detail
} // namespace dicom
