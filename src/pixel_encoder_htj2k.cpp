#include "pixel_encoder_detail.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include <ojph_codestream.h>
#include <ojph_file.h>
#include <ojph_mem.h>
#include <ojph_params.h>

namespace dicom::pixel::detail {
namespace {

void set_codec_error(codec_error& out_error, codec_status_code code,
    std::string_view stage, std::string detail) {
	out_error.code = code;
	out_error.stage = std::string(stage);
	out_error.detail = std::move(detail);
}

struct sample_value_range {
	std::int32_t min_value{0};
	std::int32_t max_value{0};
};

[[nodiscard]] double resolve_htj2k_qstep(std::size_t samples_per_pixel, int bits_stored,
    const Htj2kOptions& options, std::string_view function_name) {
	if (options.target_bpp < 0.0 || options.target_psnr < 0.0) {
		throw_encode_error(
		    "{} reason=Htj2kOptions.target_bpp/target_psnr must be >= 0",
		    function_name);
	}

	double qstep = 0.01;
	if (options.target_psnr > 0.0) {
		qstep = std::pow(10.0, -options.target_psnr / 20.0);
	} else if (options.target_bpp > 0.0) {
		const auto uncompressed_bpp =
		    static_cast<double>(bits_stored) * static_cast<double>(samples_per_pixel);
		const auto compression_ratio = uncompressed_bpp / options.target_bpp;
		qstep = compression_ratio * 0.01;
	}

	if (!std::isfinite(qstep) || qstep <= 0.0) {
		throw_encode_error(
		    "{} reason=failed to resolve HTJ2K qstep from options",
		    function_name);
	}
	return std::clamp(qstep, 0.00001, 0.5);
}

[[nodiscard]] sample_value_range measure_source_sample_range(
    std::span<const std::uint8_t> frame_data, const SourceFrameLayout& source_layout,
    std::size_t row_stride, std::size_t rows, std::size_t cols, std::size_t samples_per_pixel,
    std::size_t bytes_per_sample, int bits_stored, bool source_signed,
    std::string_view function_name) {
	const auto* frame_ptr = frame_data.data();
	std::int32_t min_value = std::numeric_limits<std::int32_t>::max();
	std::int32_t max_value = std::numeric_limits<std::int32_t>::min();

	for (std::size_t component = 0; component < samples_per_pixel; ++component) {
		for (std::size_t row = 0; row < rows; ++row) {
			const std::uint8_t* source_row = nullptr;
			if (source_layout.source_is_planar) {
				source_row = frame_ptr + component * source_layout.plane_stride + row * row_stride;
			} else {
				source_row = frame_ptr + row * row_stride;
			}
			for (std::size_t col = 0; col < cols; ++col) {
				const std::uint8_t* sample_ptr = nullptr;
				if (source_layout.source_is_planar) {
					sample_ptr = source_row + col * bytes_per_sample;
				} else {
					sample_ptr = source_row +
					    (col * samples_per_pixel + component) * bytes_per_sample;
				}
					const auto sample_value = load_i8_or_i16_sample_from_source(sample_ptr,
					    bytes_per_sample, source_signed, bits_stored);
				min_value = std::min(min_value, sample_value);
				max_value = std::max(max_value, sample_value);
			}
		}
	}

	return {min_value, max_value};
}

void fill_openjph_line_from_source(ojph::line_buf* line, std::span<const std::uint8_t> frame_data,
    const SourceFrameLayout& source_layout, std::size_t row_stride, std::size_t row,
    std::size_t component_index, std::size_t cols, std::size_t samples_per_pixel,
    std::size_t bytes_per_sample, int bits_stored, bool source_signed,
    std::string_view function_name) {
	if (!line) {
		throw_encode_error(
		    "{} reason=OpenJPH returned null line buffer",
		    function_name);
	}
	if (line->size < cols) {
		throw_encode_error(
		    "{} reason=OpenJPH line size too small (have={}, need={})",
		    function_name, line->size, cols);
	}

	const bool line_is_integer = (line->flags & ojph::line_buf::LFT_INTEGER) != 0;
	if (line_is_integer && !line->i32) {
		throw_encode_error(
		    "{} reason=OpenJPH integer line has null data pointer",
		    function_name);
	}
	if (!line_is_integer && !line->f32) {
		throw_encode_error(
		    "{} reason=OpenJPH floating line has null data pointer",
		    function_name);
	}

	const auto* frame_ptr = frame_data.data();
	const std::uint8_t* source_row = nullptr;
	if (source_layout.source_is_planar) {
		source_row = frame_ptr + component_index * source_layout.plane_stride + row * row_stride;
	} else {
		source_row = frame_ptr + row * row_stride;
	}

	for (std::size_t col = 0; col < cols; ++col) {
		const std::uint8_t* sample_ptr = nullptr;
		if (source_layout.source_is_planar) {
			sample_ptr = source_row + col * bytes_per_sample;
		} else {
			sample_ptr = source_row +
			    (col * samples_per_pixel + component_index) * bytes_per_sample;
		}
		const auto sample_value = load_i8_or_i16_sample_from_source(sample_ptr,
		    bytes_per_sample, source_signed, bits_stored);
		if (line_is_integer) {
			line->i32[col] = sample_value;
		} else {
			line->f32[col] = static_cast<float>(sample_value);
		}
	}
}

} // namespace

bool try_encode_htj2k_frame(std::span<const std::uint8_t> frame_data,
    std::size_t rows, std::size_t cols, std::size_t samples_per_pixel,
    std::size_t bytes_per_sample, int bits_allocated, int bits_stored,
    int pixel_representation, Planar source_planar, std::size_t row_stride,
    bool use_multicomponent_transform, bool lossless, bool rpcl_progression,
    const Htj2kOptions& options, std::vector<std::uint8_t>& out_encoded,
    codec_error& out_error) noexcept {
	out_encoded.clear();
	out_error = codec_error{};

	if (rows == 0 || cols == 0 || samples_per_pixel == 0 || bytes_per_sample == 0) {
		set_codec_error(out_error, codec_status_code::invalid_argument, "validate",
		    "rows/cols/samples_per_pixel/bytes_per_sample must be positive");
		return false;
	}
	if (samples_per_pixel != 1 && samples_per_pixel != 3 && samples_per_pixel != 4) {
		set_codec_error(out_error, codec_status_code::invalid_argument, "validate",
		    "only samples_per_pixel=1/3/4 are supported in current HTJ2K encoder path");
		return false;
	}
	if (use_multicomponent_transform && samples_per_pixel != 3) {
		set_codec_error(out_error, codec_status_code::invalid_argument, "validate",
		    "multicomponent HTJ2K requires samples_per_pixel=3");
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
	if (bytes_per_sample != 1 && bytes_per_sample != 2) {
		set_codec_error(out_error, codec_status_code::invalid_argument, "validate",
		    "unsupported bytes_per_sample (expected 1 or 2)");
		return false;
	}
	if (options.target_bpp < 0.0 || options.target_psnr < 0.0) {
		set_codec_error(out_error, codec_status_code::invalid_argument, "validate",
		    "Htj2kOptions.target_bpp/target_psnr must be >= 0");
		return false;
	}
	if (options.threads < -1) {
		set_codec_error(out_error, codec_status_code::invalid_argument, "validate",
		    "Htj2kOptions.threads must be -1, 0, or positive");
		return false;
	}

	try {
		out_encoded = encode_htj2k_frame(frame_data, rows, cols, samples_per_pixel,
		    bytes_per_sample, bits_allocated, bits_stored, pixel_representation,
		    source_planar, row_stride, use_multicomponent_transform, lossless,
		    rpcl_progression, options);
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

std::vector<std::uint8_t> encode_htj2k_frame(std::span<const std::uint8_t> frame_data,
    std::size_t rows, std::size_t cols, std::size_t samples_per_pixel,
    std::size_t bytes_per_sample, int bits_allocated, int bits_stored,
    int pixel_representation, Planar source_planar, std::size_t row_stride,
    bool use_multicomponent_transform, bool lossless, bool rpcl_progression,
    const Htj2kOptions& options) {
	constexpr std::string_view kFunctionName = "pixel::encode_htj2k_frame";
	if (samples_per_pixel != 1 && samples_per_pixel != 3 && samples_per_pixel != 4) {
		throw_encode_error(
		    "{} reason=only samples_per_pixel=1/3/4 are supported in current HTJ2K encoder path",
		    kFunctionName);
	}
	if (use_multicomponent_transform && samples_per_pixel != 3) {
		throw_encode_error(
		    "{} reason=multicomponent HTJ2K requires samples_per_pixel=3",
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
	if (cols > std::numeric_limits<ojph::ui32>::max() ||
	    rows > std::numeric_limits<ojph::ui32>::max() ||
	    samples_per_pixel > std::numeric_limits<ojph::ui32>::max()) {
		throw_encode_error(
		    "{} reason=rows/cols/samples_per_pixel exceed OpenJPH range",
		    kFunctionName);
	}

	const auto source_layout = make_source_frame_layout(frame_data, rows, cols, samples_per_pixel,
	    bytes_per_sample, source_planar, row_stride);
	const bool source_signed = pixel_representation == 1;
	if (options.target_bpp < 0.0 || options.target_psnr < 0.0) {
		throw_encode_error(
		    "{} reason=Htj2kOptions.target_bpp/target_psnr must be >= 0",
		    kFunctionName);
	}
	Htj2kOptions effective_options = options;
	if (!lossless && options.target_psnr > 0.0 && bits_stored > 0 && bits_stored < 31) {
		const auto source_range = measure_source_sample_range(frame_data, source_layout, row_stride,
		    rows, cols, samples_per_pixel, bytes_per_sample, bits_stored, source_signed,
		    kFunctionName);
		const std::int64_t dynamic_range_i64 = static_cast<std::int64_t>(source_range.max_value) -
		    static_cast<std::int64_t>(source_range.min_value);
		if (dynamic_range_i64 > 0) {
			const double nominal_peak =
			    static_cast<double>((std::uint64_t{1} << static_cast<unsigned>(bits_stored)) - 1u);
			const double dynamic_peak = static_cast<double>(dynamic_range_i64);
			if (dynamic_peak > 0.0 && nominal_peak > dynamic_peak) {
				const double correction_db = 20.0 * std::log10(nominal_peak / dynamic_peak);
				effective_options.target_psnr =
				    std::clamp(options.target_psnr + correction_db, 0.0, 120.0);
			}
		}
	}

	const auto encode_with_qstep = [&](double qstep) -> std::vector<std::uint8_t> {
		try {
			ojph::codestream codestream{};
			codestream.set_planar(false);

			auto siz = codestream.access_siz();
			siz.set_image_extent(ojph::point(static_cast<ojph::ui32>(cols), static_cast<ojph::ui32>(rows)));
			siz.set_num_components(static_cast<ojph::ui32>(samples_per_pixel));
			for (std::size_t component = 0; component < samples_per_pixel; ++component) {
				siz.set_component(static_cast<ojph::ui32>(component), ojph::point(1, 1),
				    static_cast<ojph::ui32>(bits_stored), source_signed);
			}
			siz.set_image_offset(ojph::point(0, 0));
			siz.set_tile_size(ojph::size(static_cast<ojph::ui32>(cols), static_cast<ojph::ui32>(rows)));
			siz.set_tile_offset(ojph::point(0, 0));

			auto cod = codestream.access_cod();
			cod.set_progression_order(rpcl_progression ? "RPCL" : "LRCP");
			cod.set_color_transform(use_multicomponent_transform);
			cod.set_reversible(lossless);
			if (!lossless) {
				codestream.access_qcd().set_irrev_quant(static_cast<float>(qstep));
			}

			ojph::mem_outfile outfile{};
			outfile.open();
			codestream.write_headers(&outfile);

			ojph::ui32 next_component = 0;
			ojph::line_buf* line = codestream.exchange(nullptr, next_component);
			if (codestream.is_planar()) {
				for (std::size_t component = 0; component < samples_per_pixel; ++component) {
					for (std::size_t row = 0; row < rows; ++row) {
						if (next_component != static_cast<ojph::ui32>(component)) {
							throw_encode_error(
							    "{} reason=OpenJPH planar exchange order mismatch (expected={}, got={})",
							    kFunctionName, component, next_component);
						}
						fill_openjph_line_from_source(line, frame_data, source_layout, row_stride, row,
						    component, cols, samples_per_pixel, bytes_per_sample, bits_stored,
						    source_signed, kFunctionName);
						line = codestream.exchange(line, next_component);
					}
				}
			} else {
				for (std::size_t row = 0; row < rows; ++row) {
					for (std::size_t component = 0; component < samples_per_pixel; ++component) {
						if (next_component != static_cast<ojph::ui32>(component)) {
							throw_encode_error(
							    "{} reason=OpenJPH interleaved exchange order mismatch (expected={}, got={})",
							    kFunctionName, component, next_component);
						}
						fill_openjph_line_from_source(line, frame_data, source_layout, row_stride, row,
						    component, cols, samples_per_pixel, bytes_per_sample, bits_stored,
						    source_signed, kFunctionName);
						line = codestream.exchange(line, next_component);
					}
				}
			}

			codestream.flush();
			codestream.close();

			const auto used_size = outfile.get_used_size();
			if (used_size == 0 || !outfile.get_data()) {
				throw_encode_error(
				    "{} reason=OpenJPH produced empty codestream",
				    kFunctionName);
			}

			std::vector<std::uint8_t> encoded(used_size);
			std::memcpy(encoded.data(), outfile.get_data(), used_size);
			outfile.close();
			return encoded;
		} catch (const std::exception& e) {
			throw_encode_error(
			    "{} reason=OpenJPH encode failed (qstep={} {})",
			    kFunctionName, qstep, e.what());
		} catch (const char* e) {
			throw_encode_error(
			    "{} reason=OpenJPH encode failed (qstep={} {})",
			    kFunctionName, qstep, e ? e : "unknown error");
		} catch (...) {
			throw_encode_error(
			    "{} reason=OpenJPH encode failed (qstep={} unknown exception)",
			    kFunctionName, qstep);
		}
		return {};
	};

	if (lossless) {
		return encode_with_qstep(0.0);
	}

	const bool has_target_psnr = effective_options.target_psnr > 0.0;
	const bool has_target_bpp = effective_options.target_bpp > 0.0;
	if (has_target_bpp && !has_target_psnr) {
		const double target_payload_bytes_f =
		    (static_cast<double>(rows) * static_cast<double>(cols) * effective_options.target_bpp) /
		    8.0;
		const auto target_payload_bytes = static_cast<std::size_t>(
		    std::max(1.0, std::ceil(target_payload_bytes_f)));

		constexpr int kQstepSearchIterations = 16;
		double lower_qstep = 0.00001;
		double upper_qstep = 0.5;
		std::vector<std::uint8_t> best{};
		std::size_t best_abs_diff = std::numeric_limits<std::size_t>::max();

		for (int iteration = 0; iteration < kQstepSearchIterations; ++iteration) {
			// Quantization response is multiplicative; search in log-domain.
			const double mid_qstep = std::sqrt(lower_qstep * upper_qstep);
			auto encoded = encode_with_qstep(mid_qstep);
			const std::size_t encoded_bytes = encoded.size();
			const std::size_t abs_diff =
			    encoded_bytes > target_payload_bytes
			        ? encoded_bytes - target_payload_bytes
			        : target_payload_bytes - encoded_bytes;
			if (abs_diff < best_abs_diff) {
				best_abs_diff = abs_diff;
				best = std::move(encoded);
			}
			if (encoded_bytes > target_payload_bytes) {
				// Need a smaller codestream; increase quantization.
				lower_qstep = mid_qstep;
			} else if (encoded_bytes < target_payload_bytes) {
				// Need a larger codestream; reduce quantization.
				upper_qstep = mid_qstep;
			} else {
				break;
			}
		}
		return best;
	}

	const double qstep =
	    resolve_htj2k_qstep(samples_per_pixel, bits_stored, effective_options, kFunctionName);
	return encode_with_qstep(qstep);
}

} // namespace dicom::pixel::detail
