#include "pixel_decoder_detail.hpp"
#include "pixel_codec_registry.hpp"

#include "dicom_endian.h"

#include <array>
#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <string>
#include <string_view>

#include <fmt/format.h>

using namespace dicom::literals;

namespace dicom {
namespace pixel::detail {

namespace {

struct rle_header {
	std::size_t segment_count{0};
	std::array<std::uint32_t, 15> offsets{};
};

void set_codec_error(codec_error& out_error, codec_status_code code,
    std::string_view stage, std::string detail) {
	out_error.code = code;
	out_error.stage = std::string(stage);
	out_error.detail = std::move(detail);
}

template <typename... Args>
[[noreturn]] void throw_decode_error(fmt::format_string<Args...> format, Args... args) {
	throw std::runtime_error(fmt::vformat(format, fmt::make_format_args(args...)));
}

[[nodiscard]] bool checked_mul_size_t(
    std::size_t lhs, std::size_t rhs, std::size_t& out) noexcept {
	if (lhs == 0 || rhs == 0) {
		out = 0;
		return true;
	}
	if (lhs > std::numeric_limits<std::size_t>::max() / rhs) {
		return false;
	}
	out = lhs * rhs;
	return true;
}

rle_header parse_rle_header(std::span<const std::uint8_t> encoded_frame) {
	if (encoded_frame.size() < 64) {
		throw_decode_error(
		    "RLE frame is shorter than 64-byte header");
	}

	rle_header header{};
	header.segment_count = endian::load_le<std::uint32_t>(encoded_frame.data());
	if (header.segment_count == 0 || header.segment_count > header.offsets.size()) {
		throw_decode_error(
		    "invalid RLE segment count {}",
		    header.segment_count);
	}

	for (std::size_t i = 0; i < header.segment_count; ++i) {
		header.offsets[i] = endian::load_le<std::uint32_t>(
		    encoded_frame.data() + 4 + i * sizeof(std::uint32_t));
	}

	for (std::size_t i = 0; i < header.segment_count; ++i) {
		const auto start = static_cast<std::size_t>(header.offsets[i]);
		if (start < 64 || start >= encoded_frame.size()) {
			throw_decode_error(
			    "invalid RLE segment {} offset {}",
			    i, start);
		}
		const auto end = (i + 1 < header.segment_count)
		                     ? static_cast<std::size_t>(header.offsets[i + 1])
		                     : encoded_frame.size();
		if (end < start || end > encoded_frame.size()) {
			throw_decode_error(
			    "invalid RLE segment {} range [{}, {})",
			    i, start, end);
		}
	}

	return header;
}

void decode_rle_packbits_segment(std::size_t segment_index,
    std::span<const std::uint8_t> encoded,
    std::span<std::uint8_t> decoded) {
	std::size_t in = 0;
	std::size_t out = 0;

	while (out < decoded.size()) {
		if (in >= encoded.size()) {
			throw_decode_error(
			    "RLE segment {} ended early (decoded={}/{})",
			    segment_index, out, decoded.size());
		}

		const auto control = static_cast<std::int8_t>(encoded[in++]);
		if (control >= 0) {
			const auto literal_count = static_cast<std::size_t>(control) + 1;
			if (in + literal_count > encoded.size() || out + literal_count > decoded.size()) {
				throw_decode_error(
				    "RLE segment {} literal run out of bounds",
				    segment_index);
			}
			std::memcpy(decoded.data() + out, encoded.data() + in, literal_count);
			in += literal_count;
			out += literal_count;
			continue;
		}

		if (control >= -127) {
			const auto repeat_count = static_cast<std::size_t>(1 - control);
			if (in >= encoded.size() || out + repeat_count > decoded.size()) {
				throw_decode_error(
				    "RLE segment {} repeat run out of bounds",
				    segment_index);
			}
			std::memset(decoded.data() + out, encoded[in], repeat_count);
			++in;
			out += repeat_count;
			continue;
		}

		// control == -128 : no-op
	}
}

std::vector<std::uint8_t> decode_rle_frame_to_planar(
    std::span<const std::uint8_t> encoded_frame, std::size_t rows, std::size_t cols,
    std::size_t samples_per_pixel, std::size_t bytes_per_sample) {
	const auto expected_segments = samples_per_pixel * bytes_per_sample;
	if (expected_segments == 0 || expected_segments > 15) {
		throw_decode_error(
		    "unsupported RLE segment layout (spp={}, bytes_per_sample={})",
		    samples_per_pixel, bytes_per_sample);
	}

	const auto header = parse_rle_header(encoded_frame);
	if (header.segment_count < expected_segments) {
		throw_decode_error(
		    "RLE segment count {} is smaller than expected {}",
		    header.segment_count, expected_segments);
	}

	const auto pixels_per_plane = rows * cols;
	const auto src_row_bytes = cols * bytes_per_sample;
	const auto src_plane_bytes = src_row_bytes * rows;
	std::vector<std::uint8_t> decoded_planar(src_plane_bytes * samples_per_pixel, 0);
	std::vector<std::uint8_t> decoded_byte_plane(pixels_per_plane, 0);

	for (std::size_t sample = 0; sample < samples_per_pixel; ++sample) {
		auto* plane_base = decoded_planar.data() + sample * src_plane_bytes;
		for (std::size_t byte_plane = 0; byte_plane < bytes_per_sample; ++byte_plane) {
			const auto segment_index = sample * bytes_per_sample + byte_plane;
			const auto segment_start = static_cast<std::size_t>(header.offsets[segment_index]);
			const auto segment_end = (segment_index + 1 < header.segment_count)
			                             ? static_cast<std::size_t>(header.offsets[segment_index + 1])
			                             : encoded_frame.size();
			const auto segment_size = segment_end - segment_start;
			const auto segment_data = encoded_frame.subspan(segment_start, segment_size);

			decode_rle_packbits_segment(segment_index, segment_data,
			    std::span<std::uint8_t>(decoded_byte_plane));

			const auto byte_offset = bytes_per_sample - 1 - byte_plane;
			for (std::size_t r = 0; r < rows; ++r) {
				const auto* src_row = decoded_byte_plane.data() + r * cols;
				auto* dst_row = plane_base + r * src_row_bytes + byte_offset;
				for (std::size_t c = 0; c < cols; ++c) {
					dst_row[c * bytes_per_sample] = src_row[c];
				}
			}
		}
	}

	return decoded_planar;
}

} // namespace

bool decode_rle_into(const pixel::PixelDataInfo& info,
    const decode_value_transform& value_transform,
    std::span<std::uint8_t> dst,
    const DecodeStrides& dst_strides, const DecodeOptions& opt,
    codec_error& out_error, std::span<const std::uint8_t> prepared_source) noexcept {
	out_error = codec_error{};
	auto fail = [&](codec_status_code code, std::string_view stage,
	                std::string detail) noexcept -> bool {
		set_codec_error(out_error, code, stage, std::move(detail));
		return false;
	};

	try {
		if (!info.has_pixel_data) {
			return fail(codec_status_code::invalid_argument, "validate",
			    "sv_dtype is unknown");
		}

		if (info.rows <= 0 || info.cols <= 0 || info.samples_per_pixel <= 0) {
			return fail(codec_status_code::invalid_argument, "validate",
			    "invalid Rows/Columns/SamplesPerPixel");
		}

		const auto samples_per_pixel_value = info.samples_per_pixel;
		if (samples_per_pixel_value != 1 && samples_per_pixel_value != 3 &&
		    samples_per_pixel_value != 4) {
			return fail(codec_status_code::unsupported, "validate",
			    "only SamplesPerPixel=1/3/4 is supported in current RLE path");
		}
		const auto samples_per_pixel = static_cast<std::size_t>(samples_per_pixel_value);
		if (opt.scaled && samples_per_pixel != 1) {
			return fail(codec_status_code::invalid_argument, "validate",
			    "scaled output supports SamplesPerPixel=1 only");
		}

		const auto src_bytes_per_sample = sv_dtype_bytes(info.sv_dtype);
		if (src_bytes_per_sample == 0) {
			return fail(codec_status_code::unsupported, "validate",
			    "only sv_dtype=u8/s8/u16/s16/u32/s32/f32/f64 is supported in current RLE path");
		}

		const auto rows = static_cast<std::size_t>(info.rows);
		const auto cols = static_cast<std::size_t>(info.cols);
		const auto dst_bytes_per_sample =
		    opt.scaled ? sizeof(float) : src_bytes_per_sample;

		const auto dst_planar = opt.planar_out;
		const std::size_t dst_row_components =
		    (dst_planar == Planar::interleaved) ? samples_per_pixel : std::size_t{1};

		std::size_t dst_row_pixels = 0;
		std::size_t dst_min_row_bytes = 0;
		if (!checked_mul_size_t(cols, dst_row_components, dst_row_pixels) ||
		    !checked_mul_size_t(dst_row_pixels, dst_bytes_per_sample, dst_min_row_bytes)) {
			return fail(codec_status_code::internal_error, "validate",
			    "destination row bytes exceed size_t range");
		}
		if (dst_strides.row < dst_min_row_bytes) {
			return fail(codec_status_code::invalid_argument, "validate",
			    fmt::format("row stride too small (need>={}, got={})",
			        dst_min_row_bytes, dst_strides.row));
		}

		std::size_t min_frame_bytes = 0;
		if (!checked_mul_size_t(dst_strides.row, rows, min_frame_bytes)) {
			return fail(codec_status_code::internal_error, "validate",
			    "destination frame bytes exceed size_t range");
		}
		if (dst_planar == Planar::planar &&
		    !checked_mul_size_t(min_frame_bytes, samples_per_pixel, min_frame_bytes)) {
			return fail(codec_status_code::internal_error, "validate",
			    "destination planar frame bytes exceed size_t range");
		}
		if (dst_strides.frame < min_frame_bytes) {
			return fail(codec_status_code::invalid_argument, "validate",
			    fmt::format("frame stride too small (need>={}, got={})",
			        min_frame_bytes, dst_strides.frame));
		}
		if (dst.size() < dst_strides.frame) {
			return fail(codec_status_code::invalid_argument, "validate",
			    fmt::format("destination too small (need={}, got={})",
			        dst_strides.frame, dst.size()));
		}

		const auto rle_source = prepared_source;
		if (rle_source.empty()) {
			return fail(codec_status_code::invalid_argument, "load_frame_source",
			    "RLE frame has empty codestream");
		}

		std::vector<std::uint8_t> decoded_planar{};
		try {
			decoded_planar = decode_rle_frame_to_planar(
			    rle_source, rows, cols, samples_per_pixel, src_bytes_per_sample);
		} catch (const std::bad_alloc&) {
			return fail(codec_status_code::internal_error, "allocate",
			    "memory allocation failed");
		} catch (const std::exception& e) {
			return fail(codec_status_code::invalid_argument, "decode_frame",
			    e.what());
		} catch (...) {
			return fail(codec_status_code::backend_error, "decode_frame",
			    "non-standard exception");
		}

		std::size_t src_row_bytes = 0;
		if (!checked_mul_size_t(cols, src_bytes_per_sample, src_row_bytes)) {
			return fail(codec_status_code::internal_error, "validate",
			    "source row bytes exceed size_t range");
		}

		if (opt.scaled) {
			try {
				decode_mono_scaled_into_f32(
				    value_transform, info, decoded_planar.data(), dst,
				    dst_strides, rows, cols, src_row_bytes);
				return true;
			} catch (const std::bad_alloc&) {
				return fail(codec_status_code::internal_error, "allocate",
				    "memory allocation failed");
			} catch (const std::exception& e) {
				return fail(codec_status_code::invalid_argument, "postprocess",
				    e.what());
			} catch (...) {
				return fail(codec_status_code::backend_error, "postprocess",
				    "scaled decode failed (non-standard exception)");
			}
		}

		const auto transform = select_planar_transform(Planar::planar, dst_planar);
		run_planar_transform_copy(transform, src_bytes_per_sample,
		    decoded_planar.data(), dst.data(), rows, cols, samples_per_pixel,
		    src_row_bytes, dst_strides.row);
		return true;
	} catch (const std::bad_alloc&) {
		return fail(codec_status_code::internal_error, "allocate",
		    "memory allocation failed");
	} catch (const std::exception& e) {
		if (out_error.code != codec_status_code::ok) {
			return false;
		}
		return fail(codec_status_code::backend_error, "decode_frame", e.what());
	} catch (...) {
		if (out_error.code != codec_status_code::ok) {
			return false;
		}
		return fail(codec_status_code::backend_error, "decode_frame",
		    "non-standard exception");
	}
}

} // namespace pixel::detail
} // namespace dicom
