#include "pixel_encoder_detail.hpp"

#include "dicom_endian.h"
#include "diagnostics.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <openjpeg.h>

namespace dicom::pixel::detail {
namespace diag = dicom::diag;

namespace {

struct openjpeg_log_sink {
	std::string warning{};
	std::string error{};
};

struct jpeg2k_output_stream {
	std::vector<std::uint8_t> bytes{};
	std::size_t position{0};
};

class opj_stream_deleter {
public:
	void operator()(opj_stream_t* stream) const noexcept {
		if (stream) {
			opj_stream_destroy(stream);
		}
	}
};

class opj_codec_deleter {
public:
	void operator()(opj_codec_t* codec) const noexcept {
		if (codec) {
			opj_destroy_codec(codec);
		}
	}
};

class opj_image_deleter {
public:
	void operator()(opj_image_t* image) const noexcept {
		if (image) {
			opj_image_destroy(image);
		}
	}
};

using opj_stream_ptr = std::unique_ptr<opj_stream_t, opj_stream_deleter>;
using opj_codec_ptr = std::unique_ptr<opj_codec_t, opj_codec_deleter>;
using opj_image_ptr = std::unique_ptr<opj_image_t, opj_image_deleter>;

[[nodiscard]] std::string trimmed_message(std::string message) {
	while (!message.empty() &&
	       (message.back() == '\n' || message.back() == '\r' || message.back() == ' ' ||
	           message.back() == '\t')) {
		message.pop_back();
	}
	return message;
}

void OPJ_CALLCONV opj_warning_handler(const char* message, void* user_data) {
	if (!user_data || !message) {
		return;
	}
	auto* sink = static_cast<openjpeg_log_sink*>(user_data);
	sink->warning = message;
}

void OPJ_CALLCONV opj_error_handler(const char* message, void* user_data) {
	if (!user_data || !message) {
		return;
	}
	auto* sink = static_cast<openjpeg_log_sink*>(user_data);
	sink->error = message;
}

[[nodiscard]] std::string encode_failure_message(
    const openjpeg_log_sink& sink, std::string_view stage, std::string_view fallback) {
	if (!sink.error.empty()) {
		return trimmed_message(sink.error);
	}
	if (!sink.warning.empty()) {
		return trimmed_message(sink.warning);
	}
	std::string failure(stage);
	failure.push_back(':');
	failure.push_back(' ');
	failure += fallback;
	return failure;
}

OPJ_SIZE_T OPJ_CALLCONV opj_write_to_memory_stream(void* buffer,
    OPJ_SIZE_T bytes_to_write, void* user_data) {
	if (!buffer || !user_data) {
		return static_cast<OPJ_SIZE_T>(-1);
	}

	auto* sink = static_cast<jpeg2k_output_stream*>(user_data);
	const auto write_size = static_cast<std::size_t>(bytes_to_write);
	const auto write_end = sink->position + write_size;
	if (write_end < sink->position) {
		return static_cast<OPJ_SIZE_T>(-1);
	}
	if (write_end > sink->bytes.size()) {
		sink->bytes.resize(write_end);
	}
	std::memcpy(sink->bytes.data() + sink->position, buffer, write_size);
	sink->position = write_end;
	return bytes_to_write;
}

OPJ_OFF_T OPJ_CALLCONV opj_skip_in_memory_stream(OPJ_OFF_T bytes_to_skip, void* user_data) {
	if (!user_data || bytes_to_skip < 0) {
		return static_cast<OPJ_OFF_T>(-1);
	}
	auto* sink = static_cast<jpeg2k_output_stream*>(user_data);
	const auto skip_size = static_cast<std::size_t>(bytes_to_skip);
	const auto next_position = sink->position + skip_size;
	if (next_position < sink->position) {
		return static_cast<OPJ_OFF_T>(-1);
	}
	if (next_position > sink->bytes.size()) {
		sink->bytes.resize(next_position);
	}
	sink->position = next_position;
	return bytes_to_skip;
}

OPJ_BOOL OPJ_CALLCONV opj_seek_in_memory_stream(OPJ_OFF_T absolute_position, void* user_data) {
	if (!user_data || absolute_position < 0) {
		return OPJ_FALSE;
	}
	auto* sink = static_cast<jpeg2k_output_stream*>(user_data);
	const auto next_position = static_cast<std::size_t>(absolute_position);
	if (next_position > sink->bytes.size()) {
		sink->bytes.resize(next_position);
	}
	sink->position = next_position;
	return OPJ_TRUE;
}

[[nodiscard]] opj_stream_ptr create_openjpeg_output_stream(jpeg2k_output_stream& sink) {
	constexpr OPJ_SIZE_T kStreamBufferSize = 64 * 1024;
	opj_stream_ptr stream(opj_stream_create(kStreamBufferSize, OPJ_STREAM_WRITE));
	if (!stream) {
		return {};
	}
	opj_stream_set_user_data(stream.get(), &sink, nullptr);
	opj_stream_set_write_function(stream.get(), opj_write_to_memory_stream);
	opj_stream_set_skip_function(stream.get(), opj_skip_in_memory_stream);
	opj_stream_set_seek_function(stream.get(), opj_seek_in_memory_stream);
	return stream;
}

[[nodiscard]] OPJ_COLOR_SPACE resolve_color_space(std::size_t samples_per_pixel) {
	switch (samples_per_pixel) {
	case 1:
		return OPJ_CLRSPC_GRAY;
	case 3:
		return OPJ_CLRSPC_SRGB;
	default:
		return OPJ_CLRSPC_UNSPECIFIED;
	}
}

[[nodiscard]] std::int32_t load_sample_from_source(const std::uint8_t* sample_ptr,
    std::size_t bytes_per_sample, bool is_signed, int bits_stored,
    std::string_view file_path) {
	switch (bytes_per_sample) {
	case 1:
	case 2:
		return load_i8_or_i16_sample_from_source(sample_ptr, bytes_per_sample, is_signed,
		    bits_stored, "pixel::encode_jpeg2k_frame", file_path);
	case 4:
		if (is_signed) {
			return normalize_signed_sample_bits(
			    endian::load_le<std::int32_t>(sample_ptr), bits_stored);
		}
		// OpenJPEG component samples are signed 32-bit integers.
		// To keep behavior deterministic, reject values beyond int32 range here.
		{
			const auto raw = endian::load_le<std::uint32_t>(sample_ptr);
			if (raw > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) {
				diag::error_and_throw(
				    "pixel::encode_jpeg2k_frame file={} reason=unsigned 32-bit sample exceeds OpenJPEG int32 range",
				    file_path);
			}
			return static_cast<std::int32_t>(raw);
		}
	default:
		diag::error_and_throw(
		    "pixel::encode_jpeg2k_frame file={} reason=unsupported bytes_per_sample={}",
		    file_path, bytes_per_sample);
		return 0;
	}
}

void configure_j2k_encoder_parameters(opj_cparameters_t& parameters, bool lossless,
    std::size_t samples_per_pixel, int bits_stored, bool use_multicomponent_transform,
    const J2kOptions& options, std::string_view file_path) {
	opj_set_default_encoder_parameters(&parameters);
	parameters.cod_format = 0; // raw J2K codestream
	parameters.tcp_numlayers = 1;
	parameters.cp_disto_alloc = 1;
	parameters.tcp_rates[0] = 0.0f;
	parameters.irreversible = 0;
	parameters.tcp_mct = use_multicomponent_transform ? 1 : 0;

	if (lossless) {
		return;
	}

	if (options.target_psnr > 0.0) {
		parameters.cp_disto_alloc = 0;
		parameters.cp_fixed_quality = 1;
		parameters.tcp_distoratio[0] = static_cast<float>(options.target_psnr);
		parameters.irreversible = 1;
		return;
	}

	if (options.target_bpp > 0.0) {
		const auto uncompressed_bpp =
		    static_cast<double>(bits_stored) * static_cast<double>(samples_per_pixel);
		float compression_ratio = static_cast<float>(uncompressed_bpp / options.target_bpp);
		if (compression_ratio < 1.0f) {
			compression_ratio = 1.0f;
		}
		parameters.cp_disto_alloc = 1;
		parameters.tcp_rates[0] = compression_ratio;
		parameters.irreversible = 1;
		return;
	}

	diag::error_and_throw(
	    "pixel::encode_jpeg2k_frame file={} reason=lossy JPEG2000 transfer syntax requires J2kOptions.target_psnr or target_bpp",
	    file_path);
}

[[nodiscard]] int max_num_resolutions_for_image(std::size_t rows, std::size_t cols) {
	const auto min_dim = std::min(rows, cols);
	int max_num_resolutions = 1;
	std::size_t scale = 1;
	while (scale <= min_dim / 2 && max_num_resolutions < 32) {
		scale *= 2;
		++max_num_resolutions;
	}
	return max_num_resolutions;
}

[[nodiscard]] OPJ_UINT32 resolve_openjpeg_encode_thread_count(
    const J2kOptions& options, std::string_view file_path) {
	int configured_threads = options.threads;
	if (configured_threads < -1) {
		diag::error_and_throw(
		    "pixel::encode_jpeg2k_frame file={} reason=J2kOptions.threads must be -1, 0, or positive",
		    file_path);
	}
	if (configured_threads == 0) {
		return 0;
	}
	if (configured_threads == -1) {
		configured_threads = opj_get_num_cpus();
	}
	if (configured_threads <= 0) {
		return 0;
	}
	if (static_cast<unsigned long long>(configured_threads) >
	    static_cast<unsigned long long>(std::numeric_limits<OPJ_UINT32>::max())) {
		diag::error_and_throw(
		    "pixel::encode_jpeg2k_frame file={} reason=encoder threads {} exceed OpenJPEG limit {}",
		    file_path, configured_threads, std::numeric_limits<OPJ_UINT32>::max());
	}
	return static_cast<OPJ_UINT32>(configured_threads);
}

} // namespace

std::vector<std::uint8_t> encode_jpeg2k_frame(std::span<const std::uint8_t> frame_data,
    std::size_t rows, std::size_t cols, std::size_t samples_per_pixel,
    std::size_t bytes_per_sample, int bits_allocated, int bits_stored,
    int pixel_representation, Planar source_planar, std::size_t row_stride,
    bool use_multicomponent_transform, bool lossless,
    const J2kOptions& options, std::string_view file_path) {
	constexpr std::string_view kFunctionName = "pixel::encode_jpeg2k_frame";
	if (samples_per_pixel != 1 && samples_per_pixel != 3 && samples_per_pixel != 4) {
		diag::error_and_throw(
		    "pixel::encode_jpeg2k_frame file={} reason=only samples_per_pixel=1/3/4 are supported in current JPEG2000 encoder path",
		    file_path);
	}
	if (use_multicomponent_transform && samples_per_pixel != 3) {
		diag::error_and_throw(
		    "pixel::encode_jpeg2k_frame file={} reason=multicomponent JPEG2000 requires samples_per_pixel=3",
		    file_path);
	}
	if (bits_allocated <= 0 || bits_allocated > 16) {
		diag::error_and_throw(
		    "pixel::encode_jpeg2k_frame file={} reason=bits_allocated={} is not supported yet (supported: <= 16)",
		    file_path, bits_allocated);
	}
	if (bits_stored <= 0 || bits_stored > bits_allocated) {
		diag::error_and_throw(
		    "pixel::encode_jpeg2k_frame file={} reason=bits_stored={} must be in [1, bits_allocated={}]",
		    file_path, bits_stored, bits_allocated);
	}
	if (pixel_representation != 0 && pixel_representation != 1) {
		diag::error_and_throw(
		    "pixel::encode_jpeg2k_frame file={} reason=pixel_representation must be 0 or 1",
		    file_path);
	}

	const auto source_layout = make_source_frame_layout(frame_data, rows, cols, samples_per_pixel,
	    bytes_per_sample, source_planar, row_stride, kFunctionName, file_path);
	const bool source_is_planar = source_layout.source_is_planar;
	const std::size_t plane_stride = source_layout.plane_stride;

	opj_cparameters_t parameters{};
	configure_j2k_encoder_parameters(parameters, lossless, samples_per_pixel,
	    bits_stored, use_multicomponent_transform, options, file_path);
	parameters.numresolution = std::min(
	    parameters.numresolution, max_num_resolutions_for_image(rows, cols));
	if (parameters.numresolution < 1) {
		parameters.numresolution = 1;
	}

	std::vector<opj_image_cmptparm_t> component_parameters(samples_per_pixel);
	for (auto& component_parameter : component_parameters) {
		std::memset(&component_parameter, 0, sizeof(component_parameter));
		component_parameter.dx = static_cast<OPJ_UINT32>(parameters.subsampling_dx);
		component_parameter.dy = static_cast<OPJ_UINT32>(parameters.subsampling_dy);
		component_parameter.w = static_cast<OPJ_UINT32>(cols);
		component_parameter.h = static_cast<OPJ_UINT32>(rows);
		component_parameter.x0 = 0;
		component_parameter.y0 = 0;
		component_parameter.prec = static_cast<OPJ_UINT32>(bits_stored);
		component_parameter.sgnd = static_cast<OPJ_UINT32>(pixel_representation);
	}

	opj_image_ptr image(opj_image_create(static_cast<OPJ_UINT32>(samples_per_pixel),
	    component_parameters.data(), resolve_color_space(samples_per_pixel)));
	if (!image) {
		diag::error_and_throw(
		    "pixel::encode_jpeg2k_frame file={} reason=failed to create OpenJPEG image",
		    file_path);
	}

	image->x0 = 0;
	image->y0 = 0;
	image->x1 = static_cast<OPJ_UINT32>(cols);
	image->y1 = static_cast<OPJ_UINT32>(rows);

	const auto* frame_ptr = frame_data.data();
	const bool is_signed = pixel_representation == 1;
	const std::size_t pixel_stride = source_layout.interleaved_pixel_stride;
	for (std::size_t sample = 0; sample < samples_per_pixel; ++sample) {
		auto& component = image->comps[sample];
		if (!component.data) {
			diag::error_and_throw(
			    "pixel::encode_jpeg2k_frame file={} reason=OpenJPEG component {} data buffer is null",
			    file_path, sample);
		}
		for (std::size_t row = 0; row < rows; ++row) {
			const std::uint8_t* source_row = nullptr;
			if (source_is_planar) {
				source_row = frame_ptr + sample * plane_stride + row * row_stride;
			} else {
				source_row = frame_ptr + row * row_stride;
			}

			for (std::size_t col = 0; col < cols; ++col) {
				const std::uint8_t* sample_ptr = nullptr;
				if (source_is_planar) {
					sample_ptr = source_row + col * bytes_per_sample;
				} else {
					sample_ptr = source_row + col * pixel_stride + sample * bytes_per_sample;
				}
				const auto sample_value =
				    load_sample_from_source(sample_ptr, bytes_per_sample, is_signed,
				        bits_stored, file_path);
				component.data[row * cols + col] = sample_value;
			}
		}
	}

	opj_codec_ptr codec(opj_create_compress(OPJ_CODEC_J2K));
	if (!codec) {
		diag::error_and_throw(
		    "pixel::encode_jpeg2k_frame file={} reason=failed to create OpenJPEG encoder codec",
		    file_path);
	}

	openjpeg_log_sink sink{};
	opj_set_warning_handler(codec.get(), opj_warning_handler, &sink);
	opj_set_error_handler(codec.get(), opj_error_handler, &sink);

	if (!opj_setup_encoder(codec.get(), &parameters, image.get())) {
		diag::error_and_throw(
		    "pixel::encode_jpeg2k_frame file={} reason=OpenJPEG setup_encoder failed ({})",
		    file_path, encode_failure_message(sink, "setup", "setup_encoder returned false"));
	}
	const auto thread_count = resolve_openjpeg_encode_thread_count(options, file_path);
	if (thread_count > 0 && !opj_codec_set_threads(codec.get(), static_cast<int>(thread_count))) {
		diag::error_and_throw(
		    "pixel::encode_jpeg2k_frame file={} reason=failed to set OpenJPEG encoder threads={}",
		    file_path, thread_count);
	}

	jpeg2k_output_stream output_stream{};
	opj_stream_ptr stream = create_openjpeg_output_stream(output_stream);
	if (!stream) {
		diag::error_and_throw(
		    "pixel::encode_jpeg2k_frame file={} reason=failed to create OpenJPEG output stream",
		    file_path);
	}

	if (!opj_start_compress(codec.get(), image.get(), stream.get())) {
		diag::error_and_throw(
		    "pixel::encode_jpeg2k_frame file={} reason=OpenJPEG start_compress failed ({})",
		    file_path, encode_failure_message(sink, "start_compress", "start_compress returned false"));
	}
	if (!opj_encode(codec.get(), stream.get())) {
		diag::error_and_throw(
		    "pixel::encode_jpeg2k_frame file={} reason=OpenJPEG encode failed ({})",
		    file_path, encode_failure_message(sink, "encode", "encode returned false"));
	}
	if (!opj_end_compress(codec.get(), stream.get())) {
		diag::error_and_throw(
		    "pixel::encode_jpeg2k_frame file={} reason=OpenJPEG end_compress failed ({})",
		    file_path, encode_failure_message(sink, "end_compress", "end_compress returned false"));
	}

	if (output_stream.position < output_stream.bytes.size()) {
		output_stream.bytes.resize(output_stream.position);
	}
	if (output_stream.bytes.empty()) {
		diag::error_and_throw(
		    "pixel::encode_jpeg2k_frame file={} reason=OpenJPEG produced empty codestream",
		    file_path);
	}
	return output_stream.bytes;
}

void encode_jpeg2k_pixel_data(DicomFile& file, const EncapsulatedEncodeInput& input,
    std::size_t rows, std::size_t cols, std::size_t samples_per_pixel,
    std::size_t bytes_per_sample, int bits_allocated, int bits_stored,
    int pixel_representation, Planar source_planar, std::size_t row_stride,
    bool use_multicomponent_transform, bool lossless, const J2kOptions& options,
    std::string_view file_path) {
	encode_frames_to_encapsulated_pixel_data(
	    file, input, [&](std::span<const std::uint8_t> source_frame_view) {
		    return encode_jpeg2k_frame(source_frame_view, rows, cols, samples_per_pixel,
		        bytes_per_sample, bits_allocated, bits_stored, pixel_representation,
		        source_planar, row_stride, use_multicomponent_transform, lossless,
		        options, file_path);
	    });
}

} // namespace dicom::pixel::detail
