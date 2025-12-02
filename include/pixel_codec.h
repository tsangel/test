#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <vector>
#include <array>
#include <cmath>

namespace dicom {

class DataSet;

enum class PixelFormat {
	auto_format = 0,
	uint8,
	int16,
	int32,
	float32,
};

enum class DecodeStatus {
	ok,
	insufficient_buffer,
	unsupported_ts,
	invalid_frame,
	corrupt_stream,
};

enum class OutputLayout {
	interleaved,
	planar,
	keep_config,
};

struct StrideInfo {
	std::size_t row_bytes{0};
	std::size_t frame_bytes{0};
};

struct DecodeOptions {
	bool convert_to_rgb{true};
	bool apply_rescale{true};
	OutputLayout output_layout{OutputLayout::interleaved};
	PixelFormat output_format{PixelFormat::auto_format};
	std::size_t output_stride{0};      // optional caller-provided row stride
	std::size_t output_alignment{1};   // alignment used by compute_strides when output_stride==0
};

struct FrameInfo {
	int rows{0};
	int cols{0};
	int bits_allocated{0};
	int samples_per_pixel{1};
	bool signed_samples{false};
	int planar_config{0};  // 0: interleaved, 1: planar
	bool lossless{true};
	// Rescale / VOI hints for output format selection
	double rescale_slope{1.0};
	double rescale_intercept{0.0};
	bool has_modality_lut{false};

	[[nodiscard]] StrideInfo compute_strides(const DecodeOptions* opts = nullptr) const;
};

inline std::size_t bytes_per_sample(PixelFormat fmt) {
	switch (fmt) {
	case PixelFormat::uint8: return 1;
	case PixelFormat::int16: return 2;
	case PixelFormat::int32: return 4;
	case PixelFormat::float32: return 4;
	default: return 0;
	}
}

inline PixelFormat resolve_output_format(const FrameInfo& info, const DecodeOptions& opts) {
	if (opts.output_format != PixelFormat::auto_format) {
		return opts.output_format;
	}

	if (opts.apply_rescale) {
		const bool requires_rescale = info.has_modality_lut ||
		    info.rescale_slope != 1.0 || info.rescale_intercept != 0.0;
		if (requires_rescale) {
			const auto int_part = static_cast<long long>(info.rescale_intercept);
			const bool intercept_is_int = info.rescale_intercept == static_cast<double>(int_part);
			const bool intercept_in_range = int_part >= -10000 && int_part <= 10000;
			if (info.rescale_slope == 1.0 && intercept_is_int && intercept_in_range) {
				// Integer-preserving shift; only keep uint8 when no offset; otherwise stay 16-bit.
				if (info.bits_allocated <= 8) {
					return int_part == 0 ? PixelFormat::uint8 : PixelFormat::int16;
				}
				return PixelFormat::int16;
			}
			return PixelFormat::float32;
		}
	}

	// Default auto selection by stored bit depth.
	return info.bits_allocated <= 8 ? PixelFormat::uint8 : PixelFormat::int16;
}

class PixelDecoder {
public:
	virtual ~PixelDecoder() = default;
	virtual FrameInfo frame_info(const DataSet& ds, std::size_t frame) const = 0;
	virtual DecodeStatus decode_into(const DataSet& ds, std::span<std::byte> dst,
	                                 std::size_t frame, const DecodeOptions& opts) const = 0;
};

using DecoderFactory = std::function<std::unique_ptr<PixelDecoder>()>;

void register_decoder(std::string_view ts_uid, DecoderFactory factory);
std::unique_ptr<PixelDecoder> make_decoder(const DataSet& ds);

}  // namespace dicom
