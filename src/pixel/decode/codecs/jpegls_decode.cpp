#include "pixel/decode/core/decode_codec_impl_detail.hpp"
#include "pixel/registry/codec_registry.hpp"

#include <charls/charls.h>

#include <cstdint>
#include <exception>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

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

void validate_decoded_header(const pixel::PixelDataInfo& info,
    const charls::frame_info& frame_info,
    std::size_t rows, std::size_t cols, std::size_t samples_per_pixel,
    std::size_t src_bytes_per_sample) {
	if (frame_info.height != rows || frame_info.width != cols) {
		throw_decode_error(
		    "JPEG-LS decoded dimensions mismatch (decoded={}x{}, expected={}x{})",
		    frame_info.height, frame_info.width, rows, cols);
	}
	if (frame_info.component_count != static_cast<int>(samples_per_pixel)) {
		throw_decode_error(
		    "JPEG-LS component count mismatch (decoded={}, expected={})",
		    frame_info.component_count, samples_per_pixel);
	}

	if (frame_info.bits_per_sample <= 0 || frame_info.bits_per_sample > 16) {
		throw_decode_error(
		    "JPEG-LS decoded bits-per-sample is invalid ({})",
		    frame_info.bits_per_sample);
	}

	const auto max_output_bits = static_cast<int>(src_bytes_per_sample * 8);
	if (frame_info.bits_per_sample > max_output_bits) {
		throw_decode_error(
		    "JPEG-LS decoded precision {} exceeds output {} bits",
		    frame_info.bits_per_sample, max_output_bits);
	}

	// DICOM metadata and codestream header can disagree in practice.
	// Reject only when the decoded precision requires a wider storage width.
	if (info.bits_stored > 0 && frame_info.bits_per_sample > info.bits_stored &&
	    (static_cast<unsigned int>(frame_info.bits_per_sample) + 7u) / 8u >
	        (static_cast<unsigned int>(info.bits_stored) + 7u) / 8u) {
		throw_decode_error(
		    "JPEG-LS decoded precision {} exceeds BitsStored {}",
		    frame_info.bits_per_sample, info.bits_stored);
	}
}

std::uint32_t checked_u32_stride(const char* path_name, std::size_t stride) {
	if (stride > std::numeric_limits<std::uint32_t>::max()) {
		throw_decode_error(
		    "{} stride exceeds uint32_t ({})",
		    path_name, stride);
	}
	return static_cast<std::uint32_t>(stride);
}

} // namespace

bool decode_jpegls_into(const pixel::PixelDataInfo& info,
    const DecodeValueTransform& value_transform,
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
		if (!info.ts.is_jpegls()) {
			return fail(CodecStatusCode::unsupported, "validate",
			    "transfer syntax is not JPEG-LS");
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
			    "only SamplesPerPixel=1/3/4 is supported in current JPEG-LS path");
		}
		const auto samples_per_pixel = static_cast<std::size_t>(samples_per_pixel_value);
		if (opt.scaled && samples_per_pixel != 1) {
			return fail(CodecStatusCode::invalid_argument, "validate",
			    "scaled output supports SamplesPerPixel=1 only");
		}
		if (!sv_dtype_is_integral(info.sv_dtype)) {
			return fail(CodecStatusCode::unsupported, "validate",
			    "JPEG-LS supports integral sv_dtype only");
		}

		const auto src_bytes_per_sample = sv_dtype_bytes(info.sv_dtype);
		if (src_bytes_per_sample == 0 || src_bytes_per_sample > 2) {
			return fail(CodecStatusCode::unsupported, "validate",
			    "JPEG-LS supports integral sv_dtype up to 16-bit only");
		}

		const auto rows = static_cast<std::size_t>(info.rows);
		const auto cols = static_cast<std::size_t>(info.cols);
		const auto dst_bytes_per_sample =
		    opt.scaled ? sizeof(float) : src_bytes_per_sample;
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
				    "JPEG-LS frame has empty codestream");
			}

		charls::jpegls_decoder decoder{};
		charls::frame_info frame_info{};
		charls::interleave_mode interleave_mode{};
		try {
				decoder.source(frame_source.data(), frame_source.size());
			decoder.read_header();
			frame_info = decoder.frame_info();
			interleave_mode = decoder.interleave_mode();
		} catch (const std::exception& e) {
			return fail(CodecStatusCode::backend_error, "decode_frame", e.what());
		}

		validate_decoded_header(
		    info, frame_info, rows, cols, samples_per_pixel, src_bytes_per_sample);

		Planar src_planar = Planar::interleaved;
		switch (interleave_mode) {
		case charls::interleave_mode::none:
			src_planar = Planar::planar;
			break;
		case charls::interleave_mode::line:
		case charls::interleave_mode::sample:
			src_planar = Planar::interleaved;
			break;
		default:
			return fail(CodecStatusCode::unsupported, "validate",
			    "JPEG-LS reported unsupported interleave mode");
		}

		// Decode directly into destination when no layout/value transform is needed.
		if (!opt.scaled && (samples_per_pixel == 1 || src_planar == opt.planar_out)) {
			const auto decode_stride = checked_u32_stride("destination", dst_strides.row);
			try {
				decoder.decode(dst.data(), dst_strides.frame, decode_stride);
			} catch (const std::exception& e) {
				return fail(CodecStatusCode::backend_error, "decode_frame", e.what());
			}
			return true;
		}

		const std::size_t src_row_components =
		    (src_planar == Planar::interleaved) ? samples_per_pixel : std::size_t{1};
		const std::size_t src_row_bytes = cols * src_row_components * src_bytes_per_sample;
		std::size_t src_frame_bytes = src_row_bytes * rows;
		if (src_planar == Planar::planar) {
			src_frame_bytes *= samples_per_pixel;
		}

		std::vector<std::uint8_t> decoded(src_frame_bytes);
		const auto decode_stride = checked_u32_stride("source", src_row_bytes);
		try {
			decoder.decode(decoded.data(), decoded.size(), decode_stride);
		} catch (const std::exception& e) {
			return fail(CodecStatusCode::backend_error, "decode_frame", e.what());
		}

		if (opt.scaled) {
			decode_mono_scaled_into_f32(
			    value_transform, info, decoded.data(), dst, dst_strides,
			    rows, cols, src_row_bytes);
			return true;
		}

		const auto transform = select_planar_transform(src_planar, opt.planar_out);
		run_planar_transform_copy(transform, src_bytes_per_sample, decoded.data(),
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
