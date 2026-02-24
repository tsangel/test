#include "pixel_encoder_detail.hpp"

#include "diagnostics.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <string_view>
#include <vector>

#include <ojph_codestream.h>
#include <ojph_file.h>
#include <ojph_mem.h>
#include <ojph_params.h>

namespace dicom::pixel::detail {
namespace diag = dicom::diag;

namespace {

[[nodiscard]] double resolve_htj2k_qstep(std::size_t samples_per_pixel, int bits_stored,
    const Htj2kOptions& options, std::string_view function_name, std::string_view file_path) {
	if (options.target_bpp < 0.0 || options.target_psnr < 0.0) {
		diag::error_and_throw(
		    "{} file={} reason=Htj2kOptions.target_bpp/target_psnr must be >= 0",
		    function_name, file_path);
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
		diag::error_and_throw(
		    "{} file={} reason=failed to resolve HTJ2K qstep from options",
		    function_name, file_path);
	}
	return std::clamp(qstep, 0.00001, 0.5);
}

void fill_openjph_line_from_source(ojph::line_buf* line, std::span<const std::uint8_t> frame_data,
    const SourceFrameLayout& source_layout, std::size_t row_stride, std::size_t row,
    std::size_t component_index, std::size_t cols, std::size_t samples_per_pixel,
    std::size_t bytes_per_sample, int bits_stored, bool source_signed,
    std::string_view function_name, std::string_view file_path) {
	if (!line) {
		diag::error_and_throw(
		    "{} file={} reason=OpenJPH returned null line buffer",
		    function_name, file_path);
	}
	if (line->size < cols) {
		diag::error_and_throw(
		    "{} file={} reason=OpenJPH line size too small (have={}, need={})",
		    function_name, file_path, line->size, cols);
	}

	const bool line_is_integer = (line->flags & ojph::line_buf::LFT_INTEGER) != 0;
	if (line_is_integer && !line->i32) {
		diag::error_and_throw(
		    "{} file={} reason=OpenJPH integer line has null data pointer",
		    function_name, file_path);
	}
	if (!line_is_integer && !line->f32) {
		diag::error_and_throw(
		    "{} file={} reason=OpenJPH floating line has null data pointer",
		    function_name, file_path);
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
		    bytes_per_sample, source_signed, bits_stored, function_name, file_path);
		if (line_is_integer) {
			line->i32[col] = sample_value;
		} else {
			line->f32[col] = static_cast<float>(sample_value);
		}
	}
}

} // namespace

std::vector<std::uint8_t> encode_htj2k_frame(std::span<const std::uint8_t> frame_data,
    std::size_t rows, std::size_t cols, std::size_t samples_per_pixel,
    std::size_t bytes_per_sample, int bits_allocated, int bits_stored,
    int pixel_representation, Planar source_planar, std::size_t row_stride,
    bool use_multicomponent_transform, bool lossless, bool rpcl_progression,
    const Htj2kOptions& options, std::string_view file_path) {
	constexpr std::string_view kFunctionName = "pixel::encode_htj2k_frame";
	if (samples_per_pixel != 1 && samples_per_pixel != 3 && samples_per_pixel != 4) {
		diag::error_and_throw(
		    "{} file={} reason=only samples_per_pixel=1/3/4 are supported in current HTJ2K encoder path",
		    kFunctionName, file_path);
	}
	if (use_multicomponent_transform && samples_per_pixel != 3) {
		diag::error_and_throw(
		    "{} file={} reason=multicomponent HTJ2K requires samples_per_pixel=3",
		    kFunctionName, file_path);
	}
	if (bits_allocated <= 0 || bits_allocated > 16 || (bits_allocated % 8) != 0) {
		diag::error_and_throw(
		    "{} file={} reason=bits_allocated={} is not supported (supported: 8 or 16)",
		    kFunctionName, file_path, bits_allocated);
	}
	if (bits_stored <= 0 || bits_stored > bits_allocated) {
		diag::error_and_throw(
		    "{} file={} reason=bits_stored={} must be in [1, bits_allocated={}]",
		    kFunctionName, file_path, bits_stored, bits_allocated);
	}
	if (pixel_representation != 0 && pixel_representation != 1) {
		diag::error_and_throw(
		    "{} file={} reason=pixel_representation must be 0 or 1",
		    kFunctionName, file_path);
	}
	if (cols > std::numeric_limits<ojph::ui32>::max() ||
	    rows > std::numeric_limits<ojph::ui32>::max() ||
	    samples_per_pixel > std::numeric_limits<ojph::ui32>::max()) {
		diag::error_and_throw(
		    "{} file={} reason=rows/cols/samples_per_pixel exceed OpenJPH range",
		    kFunctionName, file_path);
	}

	const auto source_layout = make_source_frame_layout(frame_data, rows, cols, samples_per_pixel,
	    bytes_per_sample, source_planar, row_stride, kFunctionName, file_path);
	const bool source_signed = pixel_representation == 1;
	const double qstep = lossless
	                         ? 0.0
	                         : resolve_htj2k_qstep(
	                               samples_per_pixel, bits_stored, options, kFunctionName, file_path);

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
						diag::error_and_throw(
						    "{} file={} reason=OpenJPH planar exchange order mismatch (expected={}, got={})",
						    kFunctionName, file_path, component, next_component);
					}
					fill_openjph_line_from_source(line, frame_data, source_layout, row_stride, row,
					    component, cols, samples_per_pixel, bytes_per_sample, bits_stored,
					    source_signed, kFunctionName, file_path);
					line = codestream.exchange(line, next_component);
				}
			}
		} else {
			for (std::size_t row = 0; row < rows; ++row) {
				for (std::size_t component = 0; component < samples_per_pixel; ++component) {
					if (next_component != static_cast<ojph::ui32>(component)) {
						diag::error_and_throw(
						    "{} file={} reason=OpenJPH interleaved exchange order mismatch (expected={}, got={})",
						    kFunctionName, file_path, component, next_component);
					}
					fill_openjph_line_from_source(line, frame_data, source_layout, row_stride, row,
					    component, cols, samples_per_pixel, bytes_per_sample, bits_stored,
					    source_signed, kFunctionName, file_path);
					line = codestream.exchange(line, next_component);
				}
			}
		}

		codestream.flush();
		codestream.close();

		const auto used_size = outfile.get_used_size();
		if (used_size == 0 || !outfile.get_data()) {
			diag::error_and_throw(
			    "{} file={} reason=OpenJPH produced empty codestream",
			    kFunctionName, file_path);
		}

		std::vector<std::uint8_t> encoded(used_size);
		std::memcpy(encoded.data(), outfile.get_data(), used_size);
		outfile.close();
		return encoded;
	} catch (const std::exception& e) {
		diag::error_and_throw(
		    "{} file={} reason=OpenJPH encode failed ({})",
		    kFunctionName, file_path, e.what());
	} catch (const char* e) {
		diag::error_and_throw(
		    "{} file={} reason=OpenJPH encode failed ({})",
		    kFunctionName, file_path, e ? e : "unknown error");
	} catch (...) {
		diag::error_and_throw(
		    "{} file={} reason=OpenJPH encode failed (unknown exception)",
		    kFunctionName, file_path);
	}

	return {};
}

void encode_htj2k_pixel_data(DicomFile& file, const EncapsulatedEncodeInput& input,
    std::size_t rows, std::size_t cols, std::size_t samples_per_pixel,
    std::size_t bytes_per_sample, int bits_allocated, int bits_stored,
    int pixel_representation, Planar source_planar, std::size_t row_stride,
    bool use_multicomponent_transform, bool lossless, bool rpcl_progression,
    const Htj2kOptions& options, std::string_view file_path) {
	encode_frames_to_encapsulated_pixel_data(
	    file, input, [&](std::span<const std::uint8_t> source_frame_view) {
		    return encode_htj2k_frame(source_frame_view, rows, cols, samples_per_pixel,
		        bytes_per_sample, bits_allocated, bits_stored, pixel_representation,
		        source_planar, row_stride, use_multicomponent_transform, lossless,
		        rpcl_progression, options, file_path);
	    });
}

} // namespace dicom::pixel::detail
