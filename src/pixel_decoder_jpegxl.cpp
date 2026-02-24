#include "pixel_decoder_detail.hpp"

#include "diagnostics.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#if DICOMSDL_HAS_JPEGXL
#include <jxl/codestream_header.h>
#include <jxl/decode.h>
#include <jxl/thread_parallel_runner.h>
#include <jxl/types.h>
#endif

using namespace dicom::literals;
namespace diag = dicom::diag;

namespace dicom {
namespace pixel::detail {

namespace {

struct jpegxl_frame_buffer {
	std::vector<std::uint8_t> owned{};
	std::span<const std::uint8_t> view{};
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

void validate_destination(const DicomFile& df, std::span<std::uint8_t> dst,
    const DecodeStrides& dst_strides, Planar dst_planar, std::size_t rows, std::size_t cols,
    std::size_t samples_per_pixel, std::size_t bytes_per_sample) {
	const std::size_t dst_row_components =
	    (dst_planar == Planar::interleaved) ? samples_per_pixel : std::size_t{1};
	const std::size_t dst_min_row_bytes = cols * dst_row_components * bytes_per_sample;
	if (dst_strides.row < dst_min_row_bytes) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=row stride too small (need>={}, got={})",
		    df.path(), dst_min_row_bytes, dst_strides.row);
	}

	std::size_t min_frame_bytes = dst_strides.row * rows;
	if (dst_planar == Planar::planar) {
		min_frame_bytes *= samples_per_pixel;
	}
	if (dst_strides.frame < min_frame_bytes) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=frame stride too small (need>={}, got={})",
		    df.path(), min_frame_bytes, dst_strides.frame);
	}
	if (dst.size() < dst_strides.frame) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=destination too small (need={}, got={})",
		    df.path(), dst_strides.frame, dst.size());
	}
}

jpegxl_frame_buffer load_jpegxl_frame_buffer(const DicomFile& df, std::size_t frame_index) {
	const auto& ds = df.dataset();
	const auto& pixel_data = ds["PixelData"_tag];
	if (!pixel_data || !pixel_data.vr().is_pixel_sequence()) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=JPEG-XL requires encapsulated PixelData",
		    df.path());
	}

	const auto* pixel_sequence = pixel_data.as_pixel_sequence();
	if (!pixel_sequence) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=JPEG-XL pixel sequence is missing",
		    df.path());
	}

	const auto frame_count = pixel_sequence->number_of_frames();
	if (frame_index >= frame_count) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} frame={} reason=JPEG-XL frame index out of range (frames={})",
		    df.path(), frame_index, frame_count);
	}

	const auto* frame = pixel_sequence->frame(frame_index);
	if (!frame) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} frame={} reason=JPEG-XL frame is missing",
		    df.path(), frame_index);
	}

	jpegxl_frame_buffer source{};
	if (frame->encoded_data_size() != 0) {
		source.view = frame->encoded_data_view();
		return source;
	}

	const auto& fragments = frame->fragments();
	if (fragments.empty()) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} frame={} reason=JPEG-XL frame has no fragments",
		    df.path(), frame_index);
	}
	for (const auto& fragment : fragments) {
		if (fragment.length == 0) {
			diag::error_and_throw(
			    "pixel::decode_frame_into file={} frame={} reason=JPEG-XL zero-length fragment is not supported",
			    df.path(), frame_index);
		}
	}

	const auto* stream = pixel_sequence->stream();
	if (!stream) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} frame={} reason=JPEG-XL pixel sequence stream is missing",
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

#if DICOMSDL_HAS_JPEGXL

class jxl_decoder_guard {
public:
	explicit jxl_decoder_guard(JxlDecoder* decoder) noexcept : decoder_(decoder) {}
	~jxl_decoder_guard() {
		if (decoder_) {
			JxlDecoderDestroy(decoder_);
		}
	}

	jxl_decoder_guard(const jxl_decoder_guard&) = delete;
	jxl_decoder_guard& operator=(const jxl_decoder_guard&) = delete;

	[[nodiscard]] JxlDecoder* get() const noexcept { return decoder_; }

private:
	JxlDecoder* decoder_{nullptr};
};

class jxl_runner_guard {
public:
	jxl_runner_guard() = default;
	~jxl_runner_guard() {
		if (runner_) {
			JxlThreadParallelRunnerDestroy(runner_);
		}
	}

	jxl_runner_guard(const jxl_runner_guard&) = delete;
	jxl_runner_guard& operator=(const jxl_runner_guard&) = delete;

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

std::size_t resolve_jpegxl_worker_threads(const DicomFile& df, const DecodeOptions& opt) {
	if (opt.decoder_threads < -1) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=invalid decoder_threads {} (expected -1, 0, or positive)",
		    df.path(), opt.decoder_threads);
	}

	if (opt.decoder_threads == -1) {
		return JxlThreadParallelRunnerDefaultNumWorkerThreads();
	}
	if (opt.decoder_threads == 0) {
		return 0;
	}
	return static_cast<std::size_t>(opt.decoder_threads);
}

void validate_basic_info(const DicomFile& df, std::size_t frame_index,
    const pixel::PixelDataInfo& info, const JxlBasicInfo& basic_info,
    std::size_t rows, std::size_t cols, std::size_t samples_per_pixel,
    std::size_t src_bytes_per_sample) {
	if (basic_info.have_animation == JXL_TRUE) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} frame={} reason=JPEG-XL animation codestream is not supported",
		    df.path(), frame_index);
	}
	if (basic_info.xsize != cols || basic_info.ysize != rows) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} frame={} reason=JPEG-XL decoded dimensions mismatch (decoded={}x{}, expected={}x{})",
		    df.path(), frame_index, basic_info.ysize, basic_info.xsize, rows, cols);
	}

	if (basic_info.exponent_bits_per_sample != 0) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} frame={} reason=JPEG-XL floating-point codestream is not supported",
		    df.path(), frame_index);
	}
	if (basic_info.bits_per_sample == 0) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} frame={} reason=JPEG-XL decoded precision is invalid (0)",
		    df.path(), frame_index);
	}

	if (samples_per_pixel == 1) {
		if (basic_info.num_color_channels != 1) {
			diag::error_and_throw(
			    "pixel::decode_frame_into file={} frame={} reason=JPEG-XL component count mismatch (decoded color={}, expected=1)",
			    df.path(), frame_index, basic_info.num_color_channels);
		}
	} else if (samples_per_pixel == 3) {
		if (basic_info.num_color_channels != 3 ||
		    basic_info.num_extra_channels != 0 || basic_info.alpha_bits != 0) {
			diag::error_and_throw(
			    "pixel::decode_frame_into file={} frame={} reason=JPEG-XL component layout mismatch for SamplesPerPixel=3 (color={}, extra={}, alpha_bits={})",
			    df.path(), frame_index, basic_info.num_color_channels,
			    basic_info.num_extra_channels, basic_info.alpha_bits);
		}
	} else if (samples_per_pixel == 4) {
		if (basic_info.num_color_channels != 3 || basic_info.num_extra_channels == 0 ||
		    basic_info.alpha_bits == 0) {
			diag::error_and_throw(
			    "pixel::decode_frame_into file={} frame={} reason=JPEG-XL component layout mismatch for SamplesPerPixel=4 (color={}, extra={}, alpha_bits={})",
			    df.path(), frame_index, basic_info.num_color_channels,
			    basic_info.num_extra_channels, basic_info.alpha_bits);
		}
	} else {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} frame={} reason=unsupported SamplesPerPixel {}",
		    df.path(), frame_index, samples_per_pixel);
	}

	const auto max_output_bits = static_cast<std::uint32_t>(src_bytes_per_sample * 8);
	if (basic_info.bits_per_sample > max_output_bits) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} frame={} reason=JPEG-XL decoded precision {} exceeds output {} bits",
		    df.path(), frame_index, basic_info.bits_per_sample, max_output_bits);
	}
	if (info.bits_stored > 0 && basic_info.bits_per_sample > info.bits_stored) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} frame={} reason=JPEG-XL decoded precision {} exceeds BitsStored {}",
		    df.path(), frame_index, basic_info.bits_per_sample, info.bits_stored);
	}
}

struct jpegxl_decode_result {
	std::vector<std::uint8_t> pixels{};
	std::size_t row_bytes{0};
};

jpegxl_decode_result decode_jpegxl_frame(const DicomFile& df, const pixel::PixelDataInfo& info,
    std::size_t frame_index, std::span<const std::uint8_t> encoded,
    std::size_t rows, std::size_t cols, std::size_t samples_per_pixel,
    std::size_t src_bytes_per_sample, const DecodeOptions& opt) {
	if (encoded.empty()) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} frame={} reason=JPEG-XL frame has empty codestream",
		    df.path(), frame_index);
	}

	const auto sig = JxlSignatureCheck(encoded.data(), encoded.size());
	if (sig == JXL_SIG_INVALID || sig == JXL_SIG_NOT_ENOUGH_BYTES) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} frame={} reason=invalid JPEG-XL signature",
		    df.path(), frame_index);
	}

	jxl_decoder_guard decoder(JxlDecoderCreate(nullptr));
	if (!decoder.get()) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} frame={} reason=failed to initialize JPEG-XL decoder",
		    df.path(), frame_index);
	}

	if (JxlDecoderSetKeepOrientation(decoder.get(), JXL_TRUE) != JXL_DEC_SUCCESS) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} frame={} reason=failed to configure JPEG-XL orientation handling",
		    df.path(), frame_index);
	}

	const auto subscribe_status = JxlDecoderSubscribeEvents(
	    decoder.get(), JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE);
	if (subscribe_status != JXL_DEC_SUCCESS) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} frame={} reason=failed to subscribe JPEG-XL decoder events ({})",
		    df.path(), frame_index, jxl_status_name(subscribe_status));
	}

	const auto worker_threads = resolve_jpegxl_worker_threads(df, opt);
	jxl_runner_guard runner{};
	if (worker_threads > 0) {
		runner.reset(JxlThreadParallelRunnerCreate(nullptr, worker_threads));
		if (!runner.get()) {
			diag::error_and_throw(
			    "pixel::decode_frame_into file={} frame={} reason=failed to initialize JPEG-XL thread runner (threads={})",
			    df.path(), frame_index, worker_threads);
		}
		if (JxlDecoderSetParallelRunner(decoder.get(), JxlThreadParallelRunner, runner.get()) !=
		    JXL_DEC_SUCCESS) {
			diag::error_and_throw(
			    "pixel::decode_frame_into file={} frame={} reason=failed to set JPEG-XL thread runner",
			    df.path(), frame_index);
		}
	}

	const auto set_input_status = JxlDecoderSetInput(decoder.get(), encoded.data(), encoded.size());
	if (set_input_status != JXL_DEC_SUCCESS) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} frame={} reason=failed to provide JPEG-XL input ({})",
		    df.path(), frame_index, jxl_status_name(set_input_status));
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
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} frame={} reason=JPEG-XL decoded frame size overflow",
		    df.path(), frame_index);
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
				diag::error_and_throw(
				    "pixel::decode_frame_into file={} frame={} reason=failed to read JPEG-XL basic info",
				    df.path(), frame_index);
			}
			validate_basic_info(
			    df, frame_index, info, basic_info, rows, cols, samples_per_pixel,
			    src_bytes_per_sample);
			saw_basic_info = true;
			break;
		}
		case JXL_DEC_NEED_IMAGE_OUT_BUFFER: {
			if (!saw_basic_info) {
				diag::error_and_throw(
				    "pixel::decode_frame_into file={} frame={} reason=JPEG-XL requested image buffer before basic info",
				    df.path(), frame_index);
			}

			std::size_t image_out_bytes = 0;
			if (JxlDecoderImageOutBufferSize(decoder.get(), &pixel_format, &image_out_bytes) !=
			    JXL_DEC_SUCCESS) {
				diag::error_and_throw(
				    "pixel::decode_frame_into file={} frame={} reason=failed to query JPEG-XL output buffer size",
				    df.path(), frame_index);
			}
			if (image_out_bytes < decoded_min_total_bytes) {
				diag::error_and_throw(
				    "pixel::decode_frame_into file={} frame={} reason=JPEG-XL output buffer is smaller than expected (have={}, need={})",
				    df.path(), frame_index, image_out_bytes, decoded_min_total_bytes);
			}
			if (rows == 0 || image_out_bytes % rows != 0) {
				diag::error_and_throw(
				    "pixel::decode_frame_into file={} frame={} reason=invalid JPEG-XL output buffer layout (bytes={}, rows={})",
				    df.path(), frame_index, image_out_bytes, rows);
			}
			decoded_row_bytes = image_out_bytes / rows;
			if (decoded_row_bytes < decoded_min_row_bytes) {
				diag::error_and_throw(
				    "pixel::decode_frame_into file={} frame={} reason=invalid JPEG-XL output row stride (have={}, need={})",
				    df.path(), frame_index, decoded_row_bytes, decoded_min_row_bytes);
			}

			decoded.resize(image_out_bytes);
			if (JxlDecoderSetImageOutBuffer(
			        decoder.get(), &pixel_format, decoded.data(), decoded.size()) != JXL_DEC_SUCCESS) {
				diag::error_and_throw(
				    "pixel::decode_frame_into file={} frame={} reason=failed to set JPEG-XL output buffer",
				    df.path(), frame_index);
			}

			const JxlBitDepth output_bit_depth{JXL_BIT_DEPTH_FROM_CODESTREAM, 0, 0};
			if (JxlDecoderSetImageOutBitDepth(decoder.get(), &output_bit_depth) != JXL_DEC_SUCCESS) {
				diag::error_and_throw(
				    "pixel::decode_frame_into file={} frame={} reason=failed to set JPEG-XL output bit depth policy",
				    df.path(), frame_index);
			}
			break;
		}
		case JXL_DEC_FULL_IMAGE:
			saw_full_image = true;
			return jpegxl_decode_result{std::move(decoded), decoded_row_bytes};
		case JXL_DEC_SUCCESS:
			if (!saw_full_image) {
				diag::error_and_throw(
				    "pixel::decode_frame_into file={} frame={} reason=JPEG-XL decode finished before full image event",
				    df.path(), frame_index);
			}
			return jpegxl_decode_result{std::move(decoded), decoded_row_bytes};
		case JXL_DEC_NEED_MORE_INPUT:
			diag::error_and_throw(
			    "pixel::decode_frame_into file={} frame={} reason=truncated JPEG-XL codestream",
			    df.path(), frame_index);
		case JXL_DEC_ERROR:
			diag::error_and_throw(
			    "pixel::decode_frame_into file={} frame={} reason=JPEG-XL decode failed",
			    df.path(), frame_index);
		default:
			break;
		}
	}
}

#endif

} // namespace

void decode_jpegxl_into(const DicomFile& df, const pixel::PixelDataInfo& info,
    std::size_t frame_index, std::span<std::uint8_t> dst,
    const DecodeStrides& dst_strides, const DecodeOptions& opt) {
	if (!info.has_pixel_data) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=sv_dtype is unknown", df.path());
	}
	if (!info.ts.is_jpegxl()) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=transfer syntax is not JPEG-XL ({})",
		    df.path(), df.transfer_syntax_uid().value());
	}
	if (info.rows <= 0 || info.cols <= 0 || info.samples_per_pixel <= 0) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=invalid Rows/Columns/SamplesPerPixel",
		    df.path());
	}
	if (info.frames <= 0) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=invalid NumberOfFrames",
		    df.path());
	}

	const auto samples_per_pixel_value = info.samples_per_pixel;
	if (samples_per_pixel_value != 1 && samples_per_pixel_value != 3 &&
	    samples_per_pixel_value != 4) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=only SamplesPerPixel=1/3/4 is supported in current JPEG-XL path",
		    df.path());
	}
	const auto samples_per_pixel = static_cast<std::size_t>(samples_per_pixel_value);
	if (opt.scaled && samples_per_pixel != 1) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=scaled output supports SamplesPerPixel=1 only",
		    df.path());
	}
	if (!sv_dtype_is_integral(info.sv_dtype)) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=JPEG-XL supports integral sv_dtype only",
		    df.path());
	}

	const auto src_bytes_per_sample = sv_dtype_bytes(info.sv_dtype);
	if (src_bytes_per_sample == 0 || src_bytes_per_sample > 2) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=JPEG-XL supports integral sv_dtype up to 16-bit only",
		    df.path());
	}

	const auto frame_count = static_cast<std::size_t>(info.frames);
	if (frame_index >= frame_count) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} frame={} reason=frame index out of range (frames={})",
		    df.path(), frame_index, frame_count);
	}

	const auto rows = static_cast<std::size_t>(info.rows);
	const auto cols = static_cast<std::size_t>(info.cols);
	const auto dst_bytes_per_sample = opt.scaled ? sizeof(float) : src_bytes_per_sample;
	validate_destination(
	    df, dst, dst_strides, opt.planar_out, rows, cols, samples_per_pixel, dst_bytes_per_sample);

	const auto frame_source = load_jpegxl_frame_buffer(df, frame_index);
	if (frame_source.view.empty()) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} frame={} reason=JPEG-XL frame has empty codestream",
		    df.path(), frame_index);
	}

#if DICOMSDL_HAS_JPEGXL
	auto decoded = decode_jpegxl_frame(df, info, frame_index, frame_source.view,
	    rows, cols, samples_per_pixel, src_bytes_per_sample, opt);
	if (decoded.pixels.empty()) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} frame={} reason=JPEG-XL decoded component has no data",
		    df.path(), frame_index);
	}

	if (opt.scaled) {
		decode_mono_scaled_into_f32(
		    df, info, decoded.pixels.data(), dst, dst_strides, rows, cols, decoded.row_bytes);
		return;
	}

	const auto transform = select_planar_transform(Planar::interleaved, opt.planar_out);
	run_planar_transform_copy(transform, src_bytes_per_sample,
	    decoded.pixels.data(), dst.data(), rows, cols, samples_per_pixel,
	    decoded.row_bytes, dst_strides.row);
#else
	(void)frame_source;
	diag::error_and_throw(
	    "pixel::decode_frame_into file={} reason=JPEG-XL backend is disabled; configure with DICOMSDL_ENABLE_JPEGXL=ON",
	    df.path());
#endif
}

} // namespace pixel::detail
} // namespace dicom
