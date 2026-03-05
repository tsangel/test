#include "pixel/decode/core/decode_codec_impl_detail.hpp"
#include "pixel/registry/codec_registry.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#if DICOMSDL_HAS_JPEGXL
#include <jxl/codestream_header.h>
#include <jxl/decode.h>
#include <jxl/thread_parallel_runner.h>
#include <jxl/types.h>
#endif

using namespace dicom::literals;

namespace dicom {
namespace pixel::detail {

namespace {

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

#if DICOMSDL_HAS_JPEGXL

class JxlDecoderGuard {
public:
	explicit JxlDecoderGuard(JxlDecoder* decoder) noexcept : decoder_(decoder) {}
	~JxlDecoderGuard() {
		if (decoder_) {
			JxlDecoderDestroy(decoder_);
		}
	}

	JxlDecoderGuard(const JxlDecoderGuard&) = delete;
	JxlDecoderGuard& operator=(const JxlDecoderGuard&) = delete;

	[[nodiscard]] JxlDecoder* get() const noexcept { return decoder_; }

private:
	JxlDecoder* decoder_{nullptr};
};

class JxlRunnerGuard {
public:
	JxlRunnerGuard() = default;
	~JxlRunnerGuard() {
		if (runner_) {
			JxlThreadParallelRunnerDestroy(runner_);
		}
	}

	JxlRunnerGuard(const JxlRunnerGuard&) = delete;
	JxlRunnerGuard& operator=(const JxlRunnerGuard&) = delete;

	void reset(void* runner) {
		if (runner_) {
			JxlThreadParallelRunnerDestroy(runner_);
		}
		runner_ = runner;
	}

	[[nodiscard]] void* get() const noexcept { return runner_; }

private:
	void* runner_{nullptr};
};

const char* jxl_status_name(JxlDecoderStatus status) noexcept {
	switch (status) {
	case JXL_DEC_SUCCESS:
		return "success";
	case JXL_DEC_ERROR:
		return "error";
	case JXL_DEC_NEED_MORE_INPUT:
		return "need_more_input";
	case JXL_DEC_NEED_IMAGE_OUT_BUFFER:
		return "need_image_out_buffer";
	case JXL_DEC_BASIC_INFO:
		return "basic_info";
	case JXL_DEC_FULL_IMAGE:
		return "full_image";
	default:
		return "other";
	}
}

std::size_t resolve_jpegxl_worker_threads(const DecodeOptions& opt) {
	if (opt.decoder_threads < -1) {
		throw_decode_error(
		    "invalid decoder_threads {} (expected -1, 0, or positive)",
		    opt.decoder_threads);
	}

	if (opt.decoder_threads == -1) {
		return JxlThreadParallelRunnerDefaultNumWorkerThreads();
	}
	if (opt.decoder_threads == 0) {
		return 0;
	}
	return static_cast<std::size_t>(opt.decoder_threads);
}

void validate_basic_info(const pixel::PixelDataInfo& info, const JxlBasicInfo& basic_info,
    std::size_t rows, std::size_t cols, std::size_t samples_per_pixel,
    std::size_t src_bytes_per_sample) {
	if (basic_info.have_animation == JXL_TRUE) {
		throw_decode_error(
		    "JPEG-XL animation codestream is not supported");
	}
	if (basic_info.xsize != cols || basic_info.ysize != rows) {
		throw_decode_error(
		    "JPEG-XL decoded dimensions mismatch (decoded={}x{}, expected={}x{})",
		    basic_info.ysize, basic_info.xsize, rows, cols);
	}

	if (basic_info.exponent_bits_per_sample != 0) {
		throw_decode_error(
		    "JPEG-XL floating-point codestream is not supported");
	}
	if (basic_info.bits_per_sample == 0) {
		throw_decode_error(
		    "JPEG-XL decoded precision is invalid (0)");
	}

	if (samples_per_pixel == 1) {
		if (basic_info.num_color_channels != 1) {
			throw_decode_error(
			    "JPEG-XL component count mismatch (decoded color={}, expected=1)",
			    basic_info.num_color_channels);
		}
	} else if (samples_per_pixel == 3) {
		if (basic_info.num_color_channels != 3 ||
		    basic_info.num_extra_channels != 0 || basic_info.alpha_bits != 0) {
			throw_decode_error(
			    "JPEG-XL component layout mismatch for SamplesPerPixel=3 (color={}, extra={}, alpha_bits={})",
			    basic_info.num_color_channels,
			    basic_info.num_extra_channels, basic_info.alpha_bits);
		}
	} else if (samples_per_pixel == 4) {
		if (basic_info.num_color_channels != 3 || basic_info.num_extra_channels == 0 ||
		    basic_info.alpha_bits == 0) {
			throw_decode_error(
			    "JPEG-XL component layout mismatch for SamplesPerPixel=4 (color={}, extra={}, alpha_bits={})",
			    basic_info.num_color_channels,
			    basic_info.num_extra_channels, basic_info.alpha_bits);
		}
	} else {
		throw_decode_error(
		    "unsupported SamplesPerPixel {}",
		    samples_per_pixel);
	}

	const auto max_output_bits = static_cast<std::uint32_t>(src_bytes_per_sample * 8);
	if (basic_info.bits_per_sample > max_output_bits) {
		throw_decode_error(
		    "JPEG-XL decoded precision {} exceeds output {} bits",
		    basic_info.bits_per_sample, max_output_bits);
	}
	// DICOM metadata and codestream header can disagree in practice.
	// Reject only when the decoded precision requires a wider storage width.
	if (info.bits_stored > 0 &&
	    basic_info.bits_per_sample > static_cast<std::uint32_t>(info.bits_stored) &&
	    (basic_info.bits_per_sample + 7u) / 8u >
	        (static_cast<std::uint32_t>(info.bits_stored) + 7u) / 8u) {
		throw_decode_error(
		    "JPEG-XL decoded precision {} exceeds BitsStored {}",
		    basic_info.bits_per_sample, info.bits_stored);
	}
}

struct JpegXlDecodeResult {
	std::vector<std::uint8_t> pixels{};
	std::size_t row_bytes{0};
};

JpegXlDecodeResult decode_jpegxl_frame(const pixel::PixelDataInfo& info,
    std::span<const std::uint8_t> encoded,
    std::size_t rows, std::size_t cols, std::size_t samples_per_pixel,
    std::size_t src_bytes_per_sample, const DecodeOptions& opt) {
	if (encoded.empty()) {
		throw_decode_error(
		    "JPEG-XL frame has empty codestream");
	}

	const auto sig = JxlSignatureCheck(encoded.data(), encoded.size());
	if (sig == JXL_SIG_INVALID || sig == JXL_SIG_NOT_ENOUGH_BYTES) {
		throw_decode_error(
		    "invalid JPEG-XL signature");
	}

	JxlDecoderGuard decoder(JxlDecoderCreate(nullptr));
	if (!decoder.get()) {
		throw_decode_error(
		    "failed to initialize JPEG-XL decoder");
	}

	if (JxlDecoderSetKeepOrientation(decoder.get(), JXL_TRUE) != JXL_DEC_SUCCESS) {
		throw_decode_error(
		    "failed to configure JPEG-XL orientation handling");
	}

	const auto subscribe_status = JxlDecoderSubscribeEvents(
	    decoder.get(), JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE);
	if (subscribe_status != JXL_DEC_SUCCESS) {
		throw_decode_error(
		    "failed to subscribe JPEG-XL decoder events ({})",
		    jxl_status_name(subscribe_status));
	}

	const auto worker_threads = resolve_jpegxl_worker_threads(opt);
	JxlRunnerGuard runner{};
	if (worker_threads > 0) {
		runner.reset(JxlThreadParallelRunnerCreate(nullptr, worker_threads));
		if (!runner.get()) {
			throw_decode_error(
			    "failed to initialize JPEG-XL thread runner (threads={})",
			    worker_threads);
		}
		if (JxlDecoderSetParallelRunner(decoder.get(), JxlThreadParallelRunner, runner.get()) !=
		    JXL_DEC_SUCCESS) {
			throw_decode_error(
			    "failed to set JPEG-XL thread runner");
		}
	}

	const auto set_input_status = JxlDecoderSetInput(decoder.get(), encoded.data(), encoded.size());
	if (set_input_status != JXL_DEC_SUCCESS) {
		throw_decode_error(
		    "failed to provide JPEG-XL input ({})",
		    jxl_status_name(set_input_status));
	}
	JxlDecoderCloseInput(decoder.get());

	const JxlPixelFormat pixel_format{
	    static_cast<std::uint32_t>(samples_per_pixel),
	    (src_bytes_per_sample == 1) ? JXL_TYPE_UINT8 : JXL_TYPE_UINT16,
	    JXL_LITTLE_ENDIAN,
	    0,
	};

	const std::size_t decoded_min_row_bytes = cols * samples_per_pixel * src_bytes_per_sample;
	if (rows != 0 && decoded_min_row_bytes > std::numeric_limits<std::size_t>::max() / rows) {
		throw_decode_error(
		    "JPEG-XL decoded frame size overflow");
	}
	const std::size_t decoded_min_total_bytes = decoded_min_row_bytes * rows;

	bool saw_basic_info = false;
	bool saw_full_image = false;
	std::vector<std::uint8_t> decoded{};
	std::size_t decoded_row_bytes = 0;
	while (true) {
		const auto status = JxlDecoderProcessInput(decoder.get());
		switch (status) {
		case JXL_DEC_BASIC_INFO: {
			JxlBasicInfo basic_info{};
			if (JxlDecoderGetBasicInfo(decoder.get(), &basic_info) != JXL_DEC_SUCCESS) {
				throw_decode_error(
				    "failed to read JPEG-XL basic info");
			}
			validate_basic_info(
			    info, basic_info, rows, cols, samples_per_pixel,
			    src_bytes_per_sample);
			saw_basic_info = true;
			break;
		}
		case JXL_DEC_NEED_IMAGE_OUT_BUFFER: {
			if (!saw_basic_info) {
				throw_decode_error(
				    "JPEG-XL requested image buffer before basic info");
			}

			std::size_t image_out_bytes = 0;
			if (JxlDecoderImageOutBufferSize(decoder.get(), &pixel_format, &image_out_bytes) !=
			    JXL_DEC_SUCCESS) {
				throw_decode_error(
				    "failed to query JPEG-XL output buffer size");
			}
			if (image_out_bytes < decoded_min_total_bytes) {
				throw_decode_error(
				    "JPEG-XL output buffer is smaller than expected (have={}, need={})",
				    image_out_bytes, decoded_min_total_bytes);
			}
			if (rows == 0 || image_out_bytes % rows != 0) {
				throw_decode_error(
				    "invalid JPEG-XL output buffer layout (bytes={}, rows={})",
				    image_out_bytes, rows);
			}
			decoded_row_bytes = image_out_bytes / rows;
			if (decoded_row_bytes < decoded_min_row_bytes) {
				throw_decode_error(
				    "invalid JPEG-XL output row stride (have={}, need={})",
				    decoded_row_bytes, decoded_min_row_bytes);
			}

			decoded.resize(image_out_bytes);
			if (JxlDecoderSetImageOutBuffer(
			        decoder.get(), &pixel_format, decoded.data(), decoded.size()) != JXL_DEC_SUCCESS) {
				throw_decode_error(
				    "failed to set JPEG-XL output buffer");
			}

			const JxlBitDepth output_bit_depth{JXL_BIT_DEPTH_FROM_CODESTREAM, 0, 0};
			if (JxlDecoderSetImageOutBitDepth(decoder.get(), &output_bit_depth) != JXL_DEC_SUCCESS) {
				throw_decode_error(
				    "failed to set JPEG-XL output bit depth policy");
			}
			break;
		}
		case JXL_DEC_FULL_IMAGE:
			saw_full_image = true;
			return JpegXlDecodeResult{std::move(decoded), decoded_row_bytes};
		case JXL_DEC_SUCCESS:
			if (!saw_full_image) {
				throw_decode_error(
				    "JPEG-XL decode finished before full image event");
			}
			return JpegXlDecodeResult{std::move(decoded), decoded_row_bytes};
		case JXL_DEC_NEED_MORE_INPUT:
			throw_decode_error(
			    "truncated JPEG-XL codestream");
		case JXL_DEC_ERROR:
			throw_decode_error(
			    "JPEG-XL decode failed");
		default:
			break;
		}
	}
}

#endif

} // namespace

bool decode_jpegxl_into(const pixel::PixelDataInfo& info,
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
		if (!info.ts.is_jpegxl()) {
			return fail(CodecStatusCode::unsupported, "validate",
			    "transfer syntax is not JPEG-XL");
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
			    "only SamplesPerPixel=1/3/4 is supported in current JPEG-XL path");
		}
		const auto samples_per_pixel = static_cast<std::size_t>(samples_per_pixel_value);
		if (opt.to_modality_value && samples_per_pixel != 1) {
			return fail(CodecStatusCode::invalid_argument, "validate",
			    "scaled output supports SamplesPerPixel=1 only");
		}
		if (!sv_dtype_is_integral(info.sv_dtype)) {
			return fail(CodecStatusCode::unsupported, "validate",
			    "JPEG-XL supports integral sv_dtype only");
		}

		const auto src_bytes_per_sample = sv_dtype_bytes(info.sv_dtype);
		if (src_bytes_per_sample == 0 || src_bytes_per_sample > 2) {
			return fail(CodecStatusCode::unsupported, "validate",
			    "JPEG-XL supports integral sv_dtype up to 16-bit only");
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
				    "JPEG-XL frame has empty codestream");
			}

	#if DICOMSDL_HAS_JPEGXL
			auto decoded = decode_jpegxl_frame(info, frame_source,
			    rows, cols, samples_per_pixel, src_bytes_per_sample, opt);
		if (decoded.pixels.empty()) {
			return fail(CodecStatusCode::backend_error, "decode_frame",
			    "JPEG-XL decoded component has no data");
		}

		if (opt.to_modality_value) {
			decode_mono_scaled_into_f32(
			    modality_value_transform, info, decoded.pixels.data(), dst,
			    dst_strides, rows, cols, decoded.row_bytes);
			return true;
		}

		const auto transform = select_planar_transform(Planar::interleaved, opt.planar_out);
		run_planar_transform_copy(transform, src_bytes_per_sample,
		    decoded.pixels.data(), dst.data(), rows, cols, samples_per_pixel,
		    decoded.row_bytes, dst_strides.row);
		return true;
	#else
			return fail(CodecStatusCode::unsupported, "decode_frame",
			    "JPEG-XL backend is disabled; configure with DICOMSDL_ENABLE_JPEGXL=ON");
#endif
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
