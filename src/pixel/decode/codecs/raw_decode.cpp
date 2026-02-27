#include "pixel/decode/core/decode_codec_impl_detail.hpp"
#include "pixel/registry/codec_registry.hpp"

#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <string>
#include <string_view>

#include <fmt/format.h>

namespace dicom {
namespace pixel::detail {
using namespace dicom::literals;

namespace {

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

} // namespace

bool decode_raw_into(const pixel::PixelDataInfo& info,
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

		if (info.rows <= 0 || info.cols <= 0 || info.samples_per_pixel <= 0) {
			return fail(CodecStatusCode::invalid_argument, "validate",
			    "invalid Rows/Columns/SamplesPerPixel");
		}

		const auto samples_per_pixel_value = info.samples_per_pixel;
		if (samples_per_pixel_value != 1 && samples_per_pixel_value != 3 &&
		    samples_per_pixel_value != 4) {
			return fail(CodecStatusCode::unsupported, "validate",
			    "only SamplesPerPixel=1/3/4 is supported in current raw path");
		}
		const auto samples_per_pixel = static_cast<std::size_t>(samples_per_pixel_value);

		const auto src_bytes_per_sample = sv_dtype_bytes(info.sv_dtype);
		if (src_bytes_per_sample == 0) {
			return fail(CodecStatusCode::unsupported, "validate",
			    "only sv_dtype=u8/s8/u16/s16/u32/s32/f32/f64 is supported in current raw path");
		}
		const std::size_t dst_bytes_per_sample =
		    opt.scaled ? sizeof(float) : src_bytes_per_sample;

		if (info.frames <= 0) {
			return fail(CodecStatusCode::invalid_argument, "validate",
			    "invalid NumberOfFrames");
		}

		const auto rows = static_cast<std::size_t>(info.rows);
		const auto cols = static_cast<std::size_t>(info.cols);
		NativeFrameSource source{};
		source.contiguous = prepared_source;
		source.name = "prepared_source";
		if (source.contiguous.empty()) {
			return fail(CodecStatusCode::invalid_argument, "load_frame_source",
			    fmt::format("{} is empty", source.name));
		}

		const auto src_planar = info.planar_configuration;
		const auto dst_planar = opt.planar_out;
		const auto transform = select_planar_transform(src_planar, dst_planar);

		const std::size_t src_row_components =
		    (src_planar == Planar::interleaved) ? samples_per_pixel : std::size_t{1};
		const std::size_t dst_row_components =
		    (dst_planar == Planar::interleaved) ? samples_per_pixel : std::size_t{1};

		const auto src = source.contiguous;

		std::size_t src_row_pixels = 0;
		std::size_t src_row_bytes = 0;
		if (!checked_mul_size_t(cols, src_row_components, src_row_pixels) ||
		    !checked_mul_size_t(src_row_pixels, src_bytes_per_sample, src_row_bytes)) {
			return fail(CodecStatusCode::internal_error, "validate",
			    "source row bytes exceed size_t range");
		}

		std::size_t src_frame_bytes = 0;
		if (!checked_mul_size_t(src_row_bytes, rows, src_frame_bytes)) {
			return fail(CodecStatusCode::internal_error, "validate",
			    "source frame bytes exceed size_t range");
		}
		if (src_planar == Planar::planar &&
		    !checked_mul_size_t(src_frame_bytes, samples_per_pixel, src_frame_bytes)) {
			return fail(CodecStatusCode::internal_error, "validate",
			    "source planar frame bytes exceed size_t range");
		}

		std::size_t dst_row_pixels = 0;
		std::size_t dst_min_row_bytes = 0;
		if (!checked_mul_size_t(cols, dst_row_components, dst_row_pixels) ||
		    !checked_mul_size_t(dst_row_pixels, dst_bytes_per_sample, dst_min_row_bytes)) {
			return fail(CodecStatusCode::internal_error, "validate",
			    "destination row bytes exceed size_t range");
		}
		if (dst_strides.row < dst_min_row_bytes) {
			return fail(CodecStatusCode::invalid_argument, "validate",
			    fmt::format("row stride too small (need>={}, got={})",
			        dst_min_row_bytes, dst_strides.row));
		}

		std::size_t min_frame_bytes = 0;
		if (!checked_mul_size_t(dst_strides.row, rows, min_frame_bytes)) {
			return fail(CodecStatusCode::internal_error, "validate",
			    "destination frame bytes exceed size_t range");
		}
		if (dst_planar == Planar::planar &&
		    !checked_mul_size_t(min_frame_bytes, samples_per_pixel, min_frame_bytes)) {
			return fail(CodecStatusCode::internal_error, "validate",
			    "destination planar frame bytes exceed size_t range");
		}
		if (dst_strides.frame < min_frame_bytes) {
			return fail(CodecStatusCode::invalid_argument, "validate",
			    fmt::format("frame stride too small (need>={}, got={})",
			        min_frame_bytes, dst_strides.frame));
		}
		if (dst.size() < dst_strides.frame) {
			return fail(CodecStatusCode::invalid_argument, "validate",
			    fmt::format("destination too small (need={}, got={})",
			        dst_strides.frame, dst.size()));
		}

		if (src.size() < src_frame_bytes) {
			return fail(CodecStatusCode::invalid_argument, "load_frame_source",
			    fmt::format("{} length is shorter than expected (have={}, need={})",
			        source.name, src.size(), src_frame_bytes));
		}

		const auto* src_frame = src.data();
		if (opt.scaled) {
			try {
				decode_mono_scaled_into_f32(
				    value_transform, info, src_frame, dst, dst_strides,
				    rows, cols, src_row_bytes);
				return true;
			} catch (const std::bad_alloc&) {
				return fail(CodecStatusCode::internal_error, "allocate",
				    "memory allocation failed");
			} catch (const std::exception& e) {
				return fail(CodecStatusCode::invalid_argument, "postprocess",
				    e.what());
			} catch (...) {
				return fail(CodecStatusCode::backend_error, "postprocess",
				    "scaled decode failed (non-standard exception)");
			}
		}

		// Fast path for raw copies when no Planar transform work is needed.
		const bool equivalent_single_channel_layout = samples_per_pixel == 1;
		const bool interleaved_no_transform =
		    transform == PlanarTransform::interleaved_to_interleaved;
		if (dst_strides.row == src_row_bytes &&
		    (equivalent_single_channel_layout || interleaved_no_transform)) {
			std::size_t copy_size = 0;
			if (!checked_mul_size_t(src_row_bytes, rows, copy_size)) {
				return fail(CodecStatusCode::internal_error, "validate",
				    "copy size exceeds size_t range");
			}
			std::memcpy(dst.data(), src_frame, copy_size);
			return true;
		}

		run_planar_transform_copy(transform, src_bytes_per_sample,
		    src_frame, dst.data(), rows, cols, samples_per_pixel,
		    src_row_bytes, dst_strides.row);
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
