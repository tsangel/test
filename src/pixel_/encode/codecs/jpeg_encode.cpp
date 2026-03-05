#include "pixel_/encode/core/encode_codec_impl_detail.hpp"

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4819)
#endif
#include <turbojpeg.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace dicom::pixel::detail {
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

class TurboJpegBufferGuard {
public:
	TurboJpegBufferGuard() = default;
	~TurboJpegBufferGuard() {
		if (data_) {
			tj3Free(data_);
		}
	}

	TurboJpegBufferGuard(const TurboJpegBufferGuard&) = delete;
	TurboJpegBufferGuard& operator=(const TurboJpegBufferGuard&) = delete;

	[[nodiscard]] unsigned char** out_ptr() noexcept { return &data_; }
	[[nodiscard]] unsigned char* get() const noexcept { return data_; }
	void release() noexcept { data_ = nullptr; }

private:
	unsigned char* data_{nullptr};
};

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

[[nodiscard]] int colorspace_for_samples(std::size_t samples_per_pixel, bool lossless) {
	switch (samples_per_pixel) {
	case 1:
		return TJCS_GRAY;
	case 3:
		return lossless ? TJCS_RGB : TJCS_YCbCr;
	case 4:
		return lossless ? TJCS_CMYK : TJCS_CMYK;
	default:
		return -1;
	}
}

[[nodiscard]] std::string_view turbojpeg_error_str(tjhandle handle) noexcept {
	const char* message = tj3GetErrorStr(handle);
	return (message && message[0] != '\0') ? std::string_view(message)
	                                        : std::string_view("unknown error");
}

void set_turbojpeg_param_or_throw(tjhandle handle, int param, int value,
    std::string_view param_name, std::string_view function_name) {
	if (tj3Set(handle, param, value) != 0) {
		throw_encode_error(
		    "{} reason=failed to set TurboJPEG param {}={} ({})",
		    function_name, param_name, value, turbojpeg_error_str(handle));
	}
}

[[nodiscard]] std::vector<std::uint8_t> interleave_planar_frame(
    std::span<const std::uint8_t> frame_data, const SourceFrameLayout& source_layout,
    std::size_t rows, std::size_t cols, std::size_t samples_per_pixel,
    std::size_t bytes_per_sample, std::size_t row_stride, std::string_view function_name) {
	const auto interleaved_row_bytes_u64 =
	    static_cast<std::uint64_t>(cols) *
	    static_cast<std::uint64_t>(samples_per_pixel) *
	    static_cast<std::uint64_t>(bytes_per_sample);
	if (interleaved_row_bytes_u64 >
	    static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
		throw_encode_error(
		    "{} reason=interleaved row bytes exceed size_t range",
		    function_name);
	}
	const std::size_t interleaved_row_bytes =
	    static_cast<std::size_t>(interleaved_row_bytes_u64);
	const auto total_bytes_u64 =
	    static_cast<std::uint64_t>(interleaved_row_bytes) *
	    static_cast<std::uint64_t>(rows);
	if (total_bytes_u64 >
	    static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
		throw_encode_error(
		    "{} reason=interleaved frame bytes exceed size_t range",
		    function_name);
	}
	const std::size_t total_bytes = static_cast<std::size_t>(total_bytes_u64);

	std::vector<std::uint8_t> interleaved(total_bytes);
	const auto* frame_ptr = frame_data.data();
	for (std::size_t row = 0; row < rows; ++row) {
		auto* dst_row = interleaved.data() + row * interleaved_row_bytes;
		for (std::size_t col = 0; col < cols; ++col) {
			for (std::size_t sample = 0; sample < samples_per_pixel; ++sample) {
				const auto* src_sample = frame_ptr +
				    sample * source_layout.plane_stride +
				    row * row_stride +
				    col * bytes_per_sample;
				std::memcpy(dst_row + (col * samples_per_pixel + sample) * bytes_per_sample,
				    src_sample, bytes_per_sample);
			}
		}
	}
	return interleaved;
}

} // namespace

bool try_encode_jpeg_frame(std::span<const std::uint8_t> frame_data,
    std::size_t rows, std::size_t cols, std::size_t samples_per_pixel,
    std::size_t bytes_per_sample, int bits_allocated, int bits_stored,
    Planar source_planar, std::size_t row_stride, bool lossless,
    const JpegOptions& options, std::vector<std::uint8_t>& out_encoded,
    CodecError& out_error) noexcept {
	out_encoded.clear();
	out_error = CodecError{};

	if (rows == 0 || cols == 0 || samples_per_pixel == 0 || bytes_per_sample == 0) {
		set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
		    "rows/cols/samples_per_pixel/bytes_per_sample must be positive");
		return false;
	}
	if (samples_per_pixel != 1 && samples_per_pixel != 3 && samples_per_pixel != 4) {
		set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
		    "only samples_per_pixel=1/3/4 are supported in current JPEG encoder path");
		return false;
	}
	if (bits_allocated <= 0 || bits_allocated > 16 || (bits_allocated % 8) != 0) {
		set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
		    "bits_allocated must be 8 or 16");
		return false;
	}
	if (bits_stored <= 0 || bits_stored > bits_allocated) {
		set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
		    "bits_stored must be in [1,bits_allocated]");
		return false;
	}
	if (!lossless && bits_stored > 12) {
		set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
		    "lossy JPEG encoder supports precision up to 12 bits");
		return false;
	}
	if ((bits_stored <= 8 && bytes_per_sample != 1) ||
	    (bits_stored > 8 && bytes_per_sample != 2)) {
		set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
		    "bytes_per_sample is incompatible with bits_stored");
		return false;
	}
	if (options.quality < 1 || options.quality > 100) {
		set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
		    "JpegOptions.quality must be in [1,100]");
		return false;
	}

	try {
		out_encoded = encode_jpeg_frame(frame_data, rows, cols, samples_per_pixel,
		    bytes_per_sample, bits_allocated, bits_stored, source_planar,
		    row_stride, lossless, options);
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

std::vector<std::uint8_t> encode_jpeg_frame(std::span<const std::uint8_t> frame_data,
    std::size_t rows, std::size_t cols, std::size_t samples_per_pixel,
    std::size_t bytes_per_sample, int bits_allocated, int bits_stored,
    Planar source_planar, std::size_t row_stride, bool lossless,
    const JpegOptions& options) {
	constexpr std::string_view kFunctionName = "pixel::encode_jpeg_frame";
	if (samples_per_pixel != 1 && samples_per_pixel != 3 && samples_per_pixel != 4) {
		throw_encode_error(
		    "{} reason=only samples_per_pixel=1/3/4 are supported in current JPEG encoder path",
		    kFunctionName);
	}
	if (bits_allocated <= 0 || bits_allocated > 16 || (bits_allocated % 8) != 0) {
		throw_encode_error(
		    "{} reason=bits_allocated={} is not supported (supported: 8 or 16)",
		    kFunctionName, bits_allocated);
	}
	if (bits_stored <= 0 || bits_stored > bits_allocated) {
		throw_encode_error(
		    "{} reason=bits_stored={} must be in [1, bits_allocated={}]",
		    kFunctionName, bits_stored, bits_allocated);
	}
	if (!lossless && bits_stored > 12) {
		throw_encode_error(
		    "{} reason=lossy JPEG encoder supports precision up to 12 bits",
		    kFunctionName);
	}
	if ((bits_stored <= 8 && bytes_per_sample != 1) ||
	    (bits_stored > 8 && bytes_per_sample != 2)) {
		throw_encode_error(
		    "{} reason=bytes_per_sample={} is incompatible with bits_stored={}",
		    kFunctionName, bytes_per_sample, bits_stored);
	}

	const auto source_layout = make_source_frame_layout(frame_data, rows, cols, samples_per_pixel,
	    bytes_per_sample, source_planar, row_stride);
	const auto pixel_format = pixel_format_for_samples(samples_per_pixel);
	if (pixel_format == TJPF_UNKNOWN) {
		throw_encode_error(
		    "{} reason=unsupported samples_per_pixel={}",
		    kFunctionName, samples_per_pixel);
	}
	const auto colorspace = colorspace_for_samples(samples_per_pixel, lossless);
	if (colorspace < 0) {
		throw_encode_error(
		    "{} reason=unsupported JPEG colorspace for samples_per_pixel={}",
		    kFunctionName, samples_per_pixel);
	}

	std::vector<std::uint8_t> interleaved_storage{};
	const std::uint8_t* source_ptr = frame_data.data();
	std::size_t source_stride_bytes = row_stride;
	if (source_layout.source_is_planar) {
		interleaved_storage = interleave_planar_frame(frame_data, source_layout, rows, cols,
		    samples_per_pixel, bytes_per_sample, row_stride, kFunctionName);
		source_ptr = interleaved_storage.data();
		const auto source_stride_bytes_u64 =
		    static_cast<std::uint64_t>(cols) *
		    static_cast<std::uint64_t>(samples_per_pixel) *
		    static_cast<std::uint64_t>(bytes_per_sample);
		if (source_stride_bytes_u64 >
		    static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
				throw_encode_error(
				    "{} reason=packed row bytes exceed size_t range",
				    kFunctionName);
		}
		source_stride_bytes = static_cast<std::size_t>(source_stride_bytes_u64);
	}
	if ((source_stride_bytes % bytes_per_sample) != 0) {
		throw_encode_error(
		    "{} reason=row_stride={} is not aligned to bytes_per_sample={}",
		    kFunctionName, source_stride_bytes, bytes_per_sample);
	}
	std::vector<std::uint16_t> aligned_u16_source{};
	if (bits_stored > 8 &&
	    (reinterpret_cast<std::uintptr_t>(source_ptr) % alignof(std::uint16_t)) != 0) {
		const auto source_total_bytes_u64 =
		    static_cast<std::uint64_t>(source_stride_bytes) *
		    static_cast<std::uint64_t>(rows);
		if (source_total_bytes_u64 >
		    static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
				throw_encode_error(
				    "{} reason=source byte copy size exceeds size_t range",
				    kFunctionName);
		}
		const auto source_total_bytes = static_cast<std::size_t>(source_total_bytes_u64);
		if ((source_total_bytes % sizeof(std::uint16_t)) != 0) {
				throw_encode_error(
				    "{} reason=16-bit source byte count must be even (got={})",
				    kFunctionName, source_total_bytes);
		}
		aligned_u16_source.resize(source_total_bytes / sizeof(std::uint16_t));
		std::memcpy(aligned_u16_source.data(), source_ptr, source_total_bytes);
		source_ptr = reinterpret_cast<const std::uint8_t*>(aligned_u16_source.data());
	}
	const std::size_t source_pitch_samples = source_stride_bytes / bytes_per_sample;
	if (cols > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
	    rows > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
	    source_pitch_samples > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
		throw_encode_error(
		    "{} reason=rows/cols/stride exceed TurboJPEG int range",
		    kFunctionName);
	}

	TurboJpegHandleGuard handle(tj3Init(TJINIT_COMPRESS));
	if (!handle.get()) {
		throw_encode_error(
		    "{} reason=failed to initialize TurboJPEG compressor",
		    kFunctionName);
	}

	const int quality = std::clamp(options.quality, 1, 100);
	set_turbojpeg_param_or_throw(
	    handle.get(), TJPARAM_LOSSLESS, lossless ? 1 : 0, "LOSSLESS", kFunctionName);
	set_turbojpeg_param_or_throw(
	    handle.get(), TJPARAM_PRECISION, bits_stored, "PRECISION", kFunctionName);
	set_turbojpeg_param_or_throw(
	    handle.get(), TJPARAM_COLORSPACE, colorspace, "COLORSPACE", kFunctionName);
	if (samples_per_pixel == std::size_t{1}) {
		set_turbojpeg_param_or_throw(
		    handle.get(), TJPARAM_SUBSAMP, TJSAMP_GRAY, "SUBSAMP", kFunctionName);
	} else {
		set_turbojpeg_param_or_throw(
		    handle.get(), TJPARAM_SUBSAMP, TJSAMP_444, "SUBSAMP", kFunctionName);
	}
	if (lossless) {
		// DICOM JPEG lossless profile in practice is most often SV1 (predictor 1).
		set_turbojpeg_param_or_throw(
		    handle.get(), TJPARAM_LOSSLESSPSV, 1, "LOSSLESSPSV", kFunctionName);
		set_turbojpeg_param_or_throw(
		    handle.get(), TJPARAM_LOSSLESSPT, 0, "LOSSLESSPT", kFunctionName);
	} else {
		set_turbojpeg_param_or_throw(
		    handle.get(), TJPARAM_QUALITY, quality, "QUALITY", kFunctionName);
	}

	TurboJpegBufferGuard encoded_buffer{};
	std::size_t encoded_size = 0;
	int rc = -1;
	if (bits_stored <= 8) {
		rc = tj3Compress8(handle.get(), source_ptr, static_cast<int>(cols),
		    static_cast<int>(source_pitch_samples), static_cast<int>(rows), pixel_format,
		    encoded_buffer.out_ptr(), &encoded_size);
	} else if (bits_stored <= 12) {
		rc = tj3Compress12(handle.get(),
		    reinterpret_cast<const short*>(source_ptr), static_cast<int>(cols),
		    static_cast<int>(source_pitch_samples), static_cast<int>(rows), pixel_format,
		    encoded_buffer.out_ptr(), &encoded_size);
	} else {
		rc = tj3Compress16(handle.get(),
		    reinterpret_cast<const unsigned short*>(source_ptr), static_cast<int>(cols),
		    static_cast<int>(source_pitch_samples), static_cast<int>(rows), pixel_format,
		    encoded_buffer.out_ptr(), &encoded_size);
	}
	if (rc != 0) {
		throw_encode_error(
		    "{} reason=TurboJPEG encode failed ({})",
		    kFunctionName, turbojpeg_error_str(handle.get()));
	}
	if (!encoded_buffer.get() || encoded_size == 0) {
		throw_encode_error(
		    "{} reason=TurboJPEG produced empty codestream",
		    kFunctionName);
	}

	std::vector<std::uint8_t> encoded(encoded_size);
	std::memcpy(encoded.data(), encoded_buffer.get(), encoded_size);
	return encoded;
}

} // namespace dicom::pixel::detail
