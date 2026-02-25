#include "pixel_encoder_detail.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#if DICOMSDL_HAS_JPEGXL
#include <jxl/codestream_header.h>
#include <jxl/color_encoding.h>
#include <jxl/encode.h>
#include <jxl/thread_parallel_runner.h>
#include <jxl/types.h>
#endif

namespace dicom::pixel::detail {
namespace {

void set_codec_error(codec_error& out_error, codec_status_code code,
    std::string_view stage, std::string detail) {
	out_error.code = code;
	out_error.stage = std::string(stage);
	out_error.detail = std::move(detail);
}

#if DICOMSDL_HAS_JPEGXL

class jxl_encoder_guard {
public:
	explicit jxl_encoder_guard(JxlEncoder* encoder) noexcept : encoder_(encoder) {}
	~jxl_encoder_guard() {
		if (encoder_) {
			JxlEncoderDestroy(encoder_);
		}
	}

	jxl_encoder_guard(const jxl_encoder_guard&) = delete;
	jxl_encoder_guard& operator=(const jxl_encoder_guard&) = delete;

	[[nodiscard]] JxlEncoder* get() const noexcept { return encoder_; }

private:
	JxlEncoder* encoder_{nullptr};
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

[[nodiscard]] const char* jxl_encode_status_name(JxlEncoderStatus status) noexcept {
	switch (status) {
	case JXL_ENC_SUCCESS:
		return "success";
	case JXL_ENC_ERROR:
		return "error";
	case JXL_ENC_NEED_MORE_OUTPUT:
		return "need_more_output";
	default:
		return "other";
	}
}

[[nodiscard]] const char* jxl_encode_error_name(JxlEncoderError error) noexcept {
	switch (error) {
	case JXL_ENC_ERR_OK:
		return "ok";
	case JXL_ENC_ERR_GENERIC:
		return "generic";
	case JXL_ENC_ERR_OOM:
		return "oom";
	case JXL_ENC_ERR_JBRD:
		return "jpeg_reconstruction";
	case JXL_ENC_ERR_BAD_INPUT:
		return "bad_input";
	case JXL_ENC_ERR_NOT_SUPPORTED:
		return "not_supported";
	case JXL_ENC_ERR_API_USAGE:
		return "api_usage";
	default:
		return "unknown";
	}
}

[[nodiscard]] std::size_t resolve_jpegxl_worker_threads(
    const JpegXlOptions& options, std::string_view function_name) {
	if (options.threads < -1) {
		throw_encode_error(
		    "{} reason=JpegXlOptions.threads must be -1, 0, or positive",
		    function_name);
	}
	if (options.threads == -1) {
		return JxlThreadParallelRunnerDefaultNumWorkerThreads();
	}
	if (options.threads == 0) {
		return 0;
	}
	return static_cast<std::size_t>(options.threads);
}

[[nodiscard]] std::vector<std::uint8_t> build_normalized_interleaved_buffer(
    std::span<const std::uint8_t> frame_data, const SourceFrameLayout& source_layout,
    std::size_t rows, std::size_t cols, std::size_t samples_per_pixel,
    std::size_t bytes_per_sample, std::size_t row_stride, int bits_stored,
    std::string_view function_name) {
	const auto row_bytes_u64 =
	    static_cast<std::uint64_t>(cols) *
	    static_cast<std::uint64_t>(samples_per_pixel) *
	    static_cast<std::uint64_t>(bytes_per_sample);
	if (row_bytes_u64 > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
		throw_encode_error("{} reason=interleaved row bytes exceed size_t range",
		    function_name);
	}
	const std::size_t row_bytes = static_cast<std::size_t>(row_bytes_u64);

	const auto total_bytes_u64 =
	    static_cast<std::uint64_t>(row_bytes) * static_cast<std::uint64_t>(rows);
	if (total_bytes_u64 > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
		throw_encode_error("{} reason=interleaved frame bytes exceed size_t range",
		    function_name);
	}
	const std::size_t total_bytes = static_cast<std::size_t>(total_bytes_u64);
	std::vector<std::uint8_t> interleaved(total_bytes);

	const std::uint32_t mask =
	    bits_stored >= 32 ? std::numeric_limits<std::uint32_t>::max()
	                      : ((std::uint32_t{1} << bits_stored) - 1u);
	const auto* frame_ptr = frame_data.data();
	for (std::size_t row = 0; row < rows; ++row) {
		for (std::size_t col = 0; col < cols; ++col) {
			for (std::size_t sample = 0; sample < samples_per_pixel; ++sample) {
				const std::uint8_t* src = nullptr;
				if (source_layout.source_is_planar) {
					src = frame_ptr +
					    sample * source_layout.plane_stride +
					    row * row_stride +
					    col * bytes_per_sample;
				} else {
					src = frame_ptr +
					    row * row_stride +
					    (col * samples_per_pixel + sample) * bytes_per_sample;
				}
				auto* dst = interleaved.data() +
				    row * row_bytes +
				    (col * samples_per_pixel + sample) * bytes_per_sample;

				if (bytes_per_sample == 1) {
					dst[0] = static_cast<std::uint8_t>(*src & static_cast<std::uint8_t>(mask));
				} else {
					const auto raw = static_cast<std::uint32_t>(
					    static_cast<std::uint16_t>(endian::load_le<std::uint16_t>(src)));
					const auto normalized = static_cast<std::uint16_t>(raw & mask);
					endian::store_le<std::uint16_t>(dst, normalized);
				}
			}
		}
	}

	return interleaved;
}

void check_jxl_success(JxlEncoder* encoder, JxlEncoderStatus status,
    std::string_view action, std::string_view function_name) {
	if (status == JXL_ENC_SUCCESS) {
		return;
	}
	const auto error = JxlEncoderGetError(encoder);
	throw_encode_error(
	    "{} reason={} failed (status={}, error={})",
	    function_name, action, jxl_encode_status_name(status),
	    jxl_encode_error_name(error));
}

#endif

} // namespace

bool try_encode_jpegxl_frame(std::span<const std::uint8_t> frame_data,
    std::size_t rows, std::size_t cols, std::size_t samples_per_pixel,
    std::size_t bytes_per_sample, int bits_allocated, int bits_stored,
    int pixel_representation, Planar source_planar, std::size_t row_stride,
    bool lossless, const JpegXlOptions& options,
    std::vector<std::uint8_t>& out_encoded, codec_error& out_error) noexcept {
	out_encoded.clear();
	out_error = codec_error{};

	if (rows == 0 || cols == 0 || samples_per_pixel == 0 || bytes_per_sample == 0) {
		set_codec_error(out_error, codec_status_code::invalid_argument, "validate",
		    "rows/cols/samples_per_pixel/bytes_per_sample must be positive");
		return false;
	}
	if (samples_per_pixel != 1 && samples_per_pixel != 3 && samples_per_pixel != 4) {
		set_codec_error(out_error, codec_status_code::invalid_argument, "validate",
		    "only samples_per_pixel=1/3/4 are supported in current JPEG-XL encoder path");
		return false;
	}
	if (bits_allocated <= 0 || bits_allocated > 16 || (bits_allocated % 8) != 0) {
		set_codec_error(out_error, codec_status_code::invalid_argument, "validate",
		    "bits_allocated must be 8 or 16");
		return false;
	}
	if (bits_stored <= 0 || bits_stored > bits_allocated) {
		set_codec_error(out_error, codec_status_code::invalid_argument, "validate",
		    "bits_stored must be in [1,bits_allocated]");
		return false;
	}
	if (pixel_representation != 0 && pixel_representation != 1) {
		set_codec_error(out_error, codec_status_code::invalid_argument, "validate",
		    "pixel_representation must be 0 or 1");
		return false;
	}
	if (options.effort < 1 || options.effort > 10) {
		set_codec_error(out_error, codec_status_code::invalid_argument, "validate",
		    "JpegXlOptions.effort must be in [1,10]");
		return false;
	}
	if (options.threads < -1) {
		set_codec_error(out_error, codec_status_code::invalid_argument, "validate",
		    "JpegXlOptions.threads must be -1, 0, or positive");
		return false;
	}
	if (!lossless && (!std::isfinite(options.distance) || options.distance <= 0.0 ||
	        options.distance > 25.0)) {
		set_codec_error(out_error, codec_status_code::invalid_argument, "validate",
		    "JpegXlOptions.distance must be in (0,25] for lossy JPEG-XL");
		return false;
	}
	if (lossless && options.distance != 0.0) {
		set_codec_error(out_error, codec_status_code::invalid_argument, "validate",
		    "lossless JPEG-XL requires distance=0");
		return false;
	}
	if (bytes_per_sample != 1 && bytes_per_sample != 2) {
		set_codec_error(out_error, codec_status_code::invalid_argument, "validate",
		    "bytes_per_sample must be 1 or 2");
		return false;
	}
	if ((bits_stored <= 8 && bytes_per_sample != 1) ||
	    (bits_stored > 8 && bytes_per_sample != 2)) {
		set_codec_error(out_error, codec_status_code::invalid_argument, "validate",
		    "bytes_per_sample is incompatible with bits_stored");
		return false;
	}

	try {
		out_encoded = encode_jpegxl_frame(frame_data, rows, cols, samples_per_pixel,
		    bytes_per_sample, bits_allocated, bits_stored, pixel_representation,
		    source_planar, row_stride, lossless, options);
		out_error = codec_error{};
		return true;
	} catch (const std::bad_alloc&) {
		set_codec_error(out_error, codec_status_code::internal_error, "allocate",
		    "memory allocation failed");
	} catch (const std::exception& e) {
		set_codec_error(out_error, codec_status_code::backend_error, "encode",
		    e.what());
	} catch (...) {
		set_codec_error(out_error, codec_status_code::backend_error, "encode",
		    "non-standard exception");
	}
	out_encoded.clear();
	return false;
}

std::vector<std::uint8_t> encode_jpegxl_frame(std::span<const std::uint8_t> frame_data,
    std::size_t rows, std::size_t cols, std::size_t samples_per_pixel,
    std::size_t bytes_per_sample, int bits_allocated, int bits_stored,
    int pixel_representation, Planar source_planar, std::size_t row_stride,
    bool lossless, const JpegXlOptions& options) {
	constexpr std::string_view kFunctionName = "pixel::encode_jpegxl_frame";
	if (samples_per_pixel != 1 && samples_per_pixel != 3 && samples_per_pixel != 4) {
		throw_encode_error(
		    "{} reason=only samples_per_pixel=1/3/4 are supported in current JPEG-XL encoder path",
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
	if (pixel_representation != 0 && pixel_representation != 1) {
		throw_encode_error(
		    "{} reason=pixel_representation must be 0 or 1",
		    kFunctionName);
	}
	if (options.effort < 1 || options.effort > 10) {
		throw_encode_error(
		    "{} reason=JpegXlOptions.effort must be in [1, 10]",
		    kFunctionName);
	}
	if (!lossless && (!std::isfinite(options.distance) || options.distance <= 0.0 ||
	        options.distance > 25.0)) {
		throw_encode_error(
		    "{} reason=JpegXlOptions.distance must be in (0, 25] for lossy JPEG-XL",
		    kFunctionName);
	}
	if (lossless && options.distance != 0.0) {
		throw_encode_error(
		    "{} reason=lossless JPEG-XL requires distance=0",
		    kFunctionName);
	}
	if (bytes_per_sample != 1 && bytes_per_sample != 2) {
		throw_encode_error(
		    "{} reason=bytes_per_sample={} is not supported (supported: 1 or 2)",
		    kFunctionName, bytes_per_sample);
	}
	if ((bits_stored <= 8 && bytes_per_sample != 1) ||
	    (bits_stored > 8 && bytes_per_sample != 2)) {
		throw_encode_error(
		    "{} reason=bytes_per_sample={} is incompatible with bits_stored={}",
		    kFunctionName, bytes_per_sample, bits_stored);
	}

	const auto source_layout = make_source_frame_layout(frame_data, rows, cols, samples_per_pixel,
	    bytes_per_sample, source_planar, row_stride);

#if DICOMSDL_HAS_JPEGXL
	auto interleaved = build_normalized_interleaved_buffer(frame_data, source_layout, rows, cols,
	    samples_per_pixel, bytes_per_sample, row_stride, bits_stored, kFunctionName);

	jxl_encoder_guard encoder(JxlEncoderCreate(nullptr));
	if (!encoder.get()) {
		throw_encode_error(
		    "{} reason=failed to initialize JPEG-XL encoder",
		    kFunctionName);
	}

	const auto worker_threads = resolve_jpegxl_worker_threads(options, kFunctionName);
	jxl_runner_guard runner{};
	if (worker_threads > 0) {
		runner.reset(JxlThreadParallelRunnerCreate(nullptr, worker_threads));
		if (!runner.get()) {
			throw_encode_error(
			    "{} reason=failed to initialize JPEG-XL thread runner (threads={})",
			    kFunctionName, worker_threads);
		}
		check_jxl_success(encoder.get(),
		    JxlEncoderSetParallelRunner(encoder.get(), JxlThreadParallelRunner, runner.get()),
		    "set parallel runner", kFunctionName);
	}

	JxlBasicInfo basic_info{};
	JxlEncoderInitBasicInfo(&basic_info);
	basic_info.xsize = static_cast<std::uint32_t>(cols);
	basic_info.ysize = static_cast<std::uint32_t>(rows);
	basic_info.bits_per_sample = static_cast<std::uint32_t>(bits_stored);
	basic_info.exponent_bits_per_sample = 0;
	basic_info.num_color_channels = (samples_per_pixel == 1) ? 1u : 3u;
	basic_info.num_extra_channels = (samples_per_pixel == 4) ? 1u : 0u;
	basic_info.alpha_bits =
	    (samples_per_pixel == 4) ? static_cast<std::uint32_t>(bits_stored) : 0u;
	basic_info.alpha_exponent_bits = 0;
	basic_info.alpha_premultiplied = JXL_FALSE;
	basic_info.uses_original_profile = lossless ? JXL_TRUE : JXL_FALSE;
	check_jxl_success(encoder.get(),
	    JxlEncoderSetBasicInfo(encoder.get(), &basic_info), "set basic info",
	    kFunctionName);

	if (samples_per_pixel == 4) {
		JxlExtraChannelInfo alpha_info{};
		JxlEncoderInitExtraChannelInfo(JXL_CHANNEL_ALPHA, &alpha_info);
		alpha_info.bits_per_sample = static_cast<std::uint32_t>(bits_stored);
		alpha_info.exponent_bits_per_sample = 0;
		alpha_info.alpha_premultiplied = JXL_FALSE;
		check_jxl_success(encoder.get(),
		    JxlEncoderSetExtraChannelInfo(encoder.get(), 0, &alpha_info),
		    "set alpha channel info", kFunctionName);
	}

	JxlColorEncoding color_encoding{};
	JxlColorEncodingSetToSRGB(&color_encoding, samples_per_pixel == 1 ? JXL_TRUE : JXL_FALSE);
	check_jxl_success(encoder.get(),
	    JxlEncoderSetColorEncoding(encoder.get(), &color_encoding),
	    "set color encoding", kFunctionName);

	auto* frame_settings = JxlEncoderFrameSettingsCreate(encoder.get(), nullptr);
	if (!frame_settings) {
		throw_encode_error(
		    "{} reason=failed to allocate JPEG-XL frame settings",
		    kFunctionName);
	}

	check_jxl_success(encoder.get(),
	    JxlEncoderFrameSettingsSetOption(
	        frame_settings, JXL_ENC_FRAME_SETTING_EFFORT, options.effort),
	    "set effort", kFunctionName);

	if (lossless) {
		check_jxl_success(encoder.get(),
		    JxlEncoderSetFrameLossless(frame_settings, JXL_TRUE),
		    "set lossless mode", kFunctionName);
	} else {
		check_jxl_success(encoder.get(),
		    JxlEncoderSetFrameDistance(frame_settings, static_cast<float>(options.distance)),
		    "set distance", kFunctionName);
	}

	const JxlBitDepth bit_depth{
	    JXL_BIT_DEPTH_FROM_CODESTREAM,
	    static_cast<std::uint32_t>(bits_stored),
	    0,
	};
	check_jxl_success(encoder.get(),
	    JxlEncoderSetFrameBitDepth(frame_settings, &bit_depth),
	    "set frame bit depth", kFunctionName);

	const JxlPixelFormat pixel_format{
	    static_cast<std::uint32_t>(samples_per_pixel),
	    bytes_per_sample == 1 ? JXL_TYPE_UINT8 : JXL_TYPE_UINT16,
	    JXL_LITTLE_ENDIAN,
	    0,
	};
	check_jxl_success(encoder.get(),
	    JxlEncoderAddImageFrame(
	        frame_settings, &pixel_format, interleaved.data(), interleaved.size()),
	    "add image frame", kFunctionName);

	JxlEncoderCloseInput(encoder.get());

	std::vector<std::uint8_t> encoded(64 * 1024);
	std::size_t produced_bytes = 0;
	while (true) {
		if (encoded.size() - produced_bytes < 32) {
			const auto next_size = std::max<std::size_t>(encoded.size() * 2, produced_bytes + 64);
			encoded.resize(next_size);
		}

		auto* next_out = encoded.data() + produced_bytes;
		std::size_t avail_out = encoded.size() - produced_bytes;
		const auto status = JxlEncoderProcessOutput(encoder.get(), &next_out, &avail_out);
		produced_bytes = static_cast<std::size_t>(next_out - encoded.data());
		if (status == JXL_ENC_SUCCESS) {
			break;
		}
		if (status == JXL_ENC_NEED_MORE_OUTPUT) {
			if (produced_bytes == encoded.size()) {
				encoded.resize(encoded.size() * 2);
			}
			continue;
		}
		const auto error = JxlEncoderGetError(encoder.get());
		throw_encode_error(
		    "{} reason=JPEG-XL encode failed (status={}, error={})",
		    kFunctionName, jxl_encode_status_name(status),
		    jxl_encode_error_name(error));
	}

	encoded.resize(produced_bytes);
	if (encoded.empty()) {
		throw_encode_error(
		    "{} reason=JPEG-XL encoder produced empty codestream",
		    kFunctionName);
	}
	return encoded;
#else
	(void)source_layout;
	(void)lossless;
	(void)options;
	throw_encode_error(
	    "{} reason=JPEG-XL backend is disabled; configure with DICOMSDL_ENABLE_JPEGXL=ON",
	    kFunctionName);
	return {};
#endif
}

} // namespace dicom::pixel::detail
