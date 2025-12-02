#include "pixel_codec.h"
#include "dicom.h"
#include "diagnostics.h"

#include <array>
#include <cstring>
#include <vector>

using namespace dicom::literals;

namespace dicom {
namespace {

inline std::size_t align_up(std::size_t value, std::size_t alignment) {
	if (alignment <= 1) {
		return value;
	}
	const auto rem = value % alignment;
	return rem ? value + (alignment - rem) : value;
}

bool decode_rle_segment(std::span<std::uint8_t> dst, std::span<const std::uint8_t> src) {
	std::size_t out = 0;
	for (std::size_t i = 0; i < src.size();) {
		const int8_t n = static_cast<int8_t>(src[i++]);
		if (n >= 0) {
			const std::size_t count = static_cast<std::size_t>(n) + 1;
			if (i + count > src.size() || out + count > dst.size()) {
				return false;
			}
			std::memcpy(dst.data() + out, src.data() + i, count);
			i += count;
			out += count;
		} else if (n != -128) {
			const std::size_t count = 1u - static_cast<int>(n);
			if (i >= src.size() || out + count > dst.size()) {
				return false;
			}
			std::memset(dst.data() + out, src[i], count);
			++i;
			out += count;
		} // n == -128 is a no-op
	}
	return out == dst.size();
}

class RlePixelDecoder final : public PixelDecoder {
public:
	FrameInfo frame_info(const DataSet& ds, std::size_t /*frame*/) const override {
		FrameInfo info{};
		constexpr Tag kLoadUntil = "PixelRepresentation"_tag; // (0028,0103)
		ds.ensure_loaded(kLoadUntil);

		info.rows = static_cast<int>(ds["Rows"_tag].toLong(0));                         // (0028,0010)
		info.cols = static_cast<int>(ds["Columns"_tag].toLong(0));                      // (0028,0011)
		info.bits_allocated = static_cast<int>(ds["BitsAllocated"_tag].toLong(0));      // (0028,0100)
		info.samples_per_pixel = static_cast<int>(ds["SamplesPerPixel"_tag].toLong(1));  // (0028,0002)
		info.signed_samples = ds["PixelRepresentation"_tag].toLong(0) == 1;              // (0028,0103)
		info.planar_config = 1;  // RLE is color-by-plane
		info.lossless = true;
		return info;
	}

	DecodeStatus decode_into(const DataSet& ds, std::span<std::byte> dst,
	    std::size_t frame, const DecodeOptions& opts) const override {
		ds.ensure_loaded("7fe0,0010"_tag); // PixelData

		if (opts.apply_rescale) {
			const double slope = ds["RescaleSlope"_tag].toDouble(1.0);
			const double intercept = ds["RescaleIntercept"_tag].toDouble(0.0);
			const bool has_modality_lut = ds["ModalityLUTSequence"_tag].sequence() != nullptr;
			const bool requires_rescale = has_modality_lut || slope != 1.0 || intercept != 0.0;
			if (requires_rescale) {
				return DecodeStatus::unsupported_ts;  // rescale not implemented yet
			}
		}
		if (opts.apply_voi) {
			const bool has_voi_lut = ds["VOILUTSequence"_tag].sequence() != nullptr;
			const bool has_window_center = !ds["WindowCenter"_tag].toDoubleVector().empty();
			const bool has_window_width = !ds["WindowWidth"_tag].toDoubleVector().empty();
			const bool requires_voi = has_voi_lut || has_window_center || has_window_width;
			if (requires_voi) {
				return DecodeStatus::unsupported_ts;  // VOI LUT/window not implemented yet
			}
		}

		const auto info = frame_info(ds, frame);
		const auto fmt = resolve_output_format(info, opts);
		const auto stored_bytes = static_cast<std::size_t>((info.bits_allocated + 7) / 8);
		const auto out_bytes = bytes_per_sample(fmt);
		if (stored_bytes == 0 || out_bytes == 0 || out_bytes != stored_bytes) {
			return DecodeStatus::unsupported_ts;
		}

		const std::size_t src_row_bytes = static_cast<std::size_t>(info.cols) *
		    static_cast<std::size_t>(info.samples_per_pixel) * stored_bytes;
		const bool request_planar_out = opts.output_layout == OutputLayout::planar;
		const std::size_t base_dst_row = request_planar_out
		    ? static_cast<std::size_t>(info.cols) * stored_bytes
		    : src_row_bytes;
		const std::size_t dst_row_bytes = opts.output_stride
		    ? opts.output_stride
		    : align_up(base_dst_row, opts.output_alignment);
		const std::size_t required = dst_row_bytes * static_cast<std::size_t>(info.rows) *
		    (request_planar_out ? static_cast<std::size_t>(info.samples_per_pixel) : 1);
		if (dst.size() < required) {
			return DecodeStatus::insufficient_buffer;
		}

		const auto& pix = ds["7fe0,0010"_tag];
		auto* pixseq = pix.pixel_sequence();
		if (!pix || !pixseq) {
			return DecodeStatus::unsupported_ts;
		}
		const auto span = pixseq->frame_encoded_span(frame);
		if (span.size() < 64) {
			return DecodeStatus::invalid_frame;
		}

		const std::size_t plane_size = static_cast<std::size_t>(info.rows) *
		    static_cast<std::size_t>(info.cols);
		const std::size_t segment_count_expected =
		    static_cast<std::size_t>(info.samples_per_pixel) * stored_bytes;

		// Parse header
		const auto load_u32 = [](const std::uint8_t* p) {
			return static_cast<std::uint32_t>(p[0]) |
			       (static_cast<std::uint32_t>(p[1]) << 8) |
			       (static_cast<std::uint32_t>(p[2]) << 16) |
			       (static_cast<std::uint32_t>(p[3]) << 24);
		};
		const std::uint8_t* base = reinterpret_cast<const std::uint8_t*>(span.data());
		const std::uint32_t segment_count = load_u32(base);
		if (segment_count == 0 || segment_count > 15 || segment_count != segment_count_expected) {
			return DecodeStatus::invalid_frame;
		}
		std::array<std::uint32_t, 16> offsets{};
		for (int i = 0; i < 15; ++i) {
			offsets[i] = load_u32(base + 4 + 4 * i);
		}
		offsets[15] = static_cast<std::uint32_t>(span.size());

		// Decode each segment into its plane buffer.
		std::vector<std::vector<std::uint8_t>> planes(segment_count);
		for (std::size_t i = 0; i < segment_count; ++i) {
			const auto start = offsets[i];
			const auto end = (i + 1 < 15) ? offsets[i + 1] : static_cast<std::uint32_t>(span.size());
			if (start < 64 || end > span.size() || start >= end) {
				return DecodeStatus::invalid_frame;
			}
			std::span<const std::uint8_t> seg(reinterpret_cast<const std::uint8_t*>(span.data()) + start,
			                                  end - start);
			planes[i].resize(plane_size);
			if (!decode_rle_segment(std::span<std::uint8_t>(planes[i]), seg)) {
				return DecodeStatus::invalid_frame;
			}
		}

		// Assemble output according to requested layout.
		std::byte* out = dst.data();
		const bool want_interleaved = opts.output_layout != OutputLayout::planar;
		const std::size_t dst_plane_size = dst_row_bytes * static_cast<std::size_t>(info.rows);

		for (std::size_t idx = 0; idx < plane_size; ++idx) {
			const std::size_t row = idx / static_cast<std::size_t>(info.cols);
			const std::size_t col = idx % static_cast<std::size_t>(info.cols);
			for (int s = 0; s < info.samples_per_pixel; ++s) {
				for (std::size_t b = 0; b < stored_bytes; ++b) {
					const std::size_t plane_index = static_cast<std::size_t>(s) * stored_bytes + b;
					const std::uint8_t value = planes[plane_index][idx];
					if (want_interleaved) {
						auto* pix_dst = reinterpret_cast<std::uint8_t*>(
						    out + dst_row_bytes * row +
						    (col * static_cast<std::size_t>(info.samples_per_pixel) +
						     static_cast<std::size_t>(s)) * stored_bytes +
						    b);
						*pix_dst = value;
					} else {
						auto* pix_dst = reinterpret_cast<std::uint8_t*>(
						    out + static_cast<std::size_t>(s) * dst_plane_size +
						    dst_row_bytes * row +
						    col * stored_bytes + b);
						*pix_dst = value;
					}
				}
			}
		}

		return DecodeStatus::ok;
	}
};

}  // namespace

void register_rle_decoders() {
	auto add_rle = []() { return std::make_unique<RlePixelDecoder>(); };
	register_decoder("RLELossless", add_rle);
}

}  // namespace dicom
