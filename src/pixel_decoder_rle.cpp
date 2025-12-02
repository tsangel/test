#include "pixel_codec.h"
#include "dicom.h"
#include "diagnostics.h"
#include "pixel_rescale.h"

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
		// Load all pixel-metadata we consume in one pass (tags are sorted; last one is enough).
		constexpr Tag kLoadUntil = "RescaleSlope"_tag; // (0028,1053)
		ds.ensure_loaded(kLoadUntil);

		info.rows = static_cast<int>(ds["Rows"_tag].toLong(0));                         // (0028,0010)
		info.cols = static_cast<int>(ds["Columns"_tag].toLong(0));                      // (0028,0011)
		info.bits_allocated = static_cast<int>(ds["BitsAllocated"_tag].toLong(0));      // (0028,0100)
		info.samples_per_pixel = static_cast<int>(ds["SamplesPerPixel"_tag].toLong(1));  // (0028,0002)
		info.signed_samples = ds["PixelRepresentation"_tag].toLong(0) == 1;              // (0028,0103)
		info.planar_config = 1;  // RLE is color-by-plane
		info.lossless = true;
		info.rescale_slope = ds["RescaleSlope"_tag].toDouble(1.0);                       // (0028,1053)
		info.rescale_intercept = ds["RescaleIntercept"_tag].toDouble(0.0);               // (0028,1052)
		info.has_modality_lut = ds["ModalityLUTSequence"_tag].sequence() != nullptr;     // (0028,3000)
		return info;
	}

	DecodeStatus decode_into(const DataSet& ds, std::span<std::byte> dst,
	    std::size_t frame, const DecodeOptions& opts) const override {
		ds.ensure_loaded("7fe0,0010"_tag); // PixelData

		const auto info = frame_info(ds, frame);
		const auto fmt = resolve_output_format(info, opts);
		const auto stored_bytes = static_cast<std::size_t>((info.bits_allocated + 7) / 8);
		const auto out_bytes = bytes_per_sample(fmt);
		if (stored_bytes == 0 || out_bytes == 0 || (stored_bytes != 1 && stored_bytes != 2)) {
			return DecodeStatus::unsupported_ts;
		}

		const bool requires_rescale = opts.apply_rescale &&
		    (info.has_modality_lut || info.rescale_slope != 1.0 || info.rescale_intercept != 0.0);
		const bool use_modality_lut = requires_rescale && info.has_modality_lut;
		ModalityLut modality_lut;
		if (use_modality_lut) {
			if (!load_modality_lut(ds, modality_lut)) {
				return DecodeStatus::invalid_frame;
			}
		}
		const RescalePlan rescale_plan = make_rescale_plan(
		    requires_rescale, info.has_modality_lut, info.signed_samples,
		    info.rescale_slope, info.rescale_intercept, fmt == PixelFormat::float32);

		const std::size_t src_row_bytes = static_cast<std::size_t>(info.cols) *
		    static_cast<std::size_t>(info.samples_per_pixel) * stored_bytes;
		const bool request_planar_out = opts.output_layout == OutputLayout::planar;
		const std::size_t base_dst_row = request_planar_out
		    ? static_cast<std::size_t>(info.cols) * out_bytes
		    : static_cast<std::size_t>(info.cols) * static_cast<std::size_t>(info.samples_per_pixel) * out_bytes;
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

		// Assemble output according to requested layout with optional rescale.
		std::byte* out = dst.data();
		const bool want_interleaved = opts.output_layout != OutputLayout::planar;
		const std::size_t dst_plane_size = dst_row_bytes * static_cast<std::size_t>(info.rows);

		if (stored_bytes == 1) {
			auto map_sample = [&](std::uint8_t stored, std::byte* dst_ptr) {
				const int stored_value = info.signed_samples
				    ? static_cast<int>(static_cast<int8_t>(stored))
				    : static_cast<int>(stored);
				const uint16_t mapped = info.signed_samples
				    ? modality_lookup<true>(stored_value, modality_lut)
				    : modality_lookup<false>(stored_value, modality_lut);
				store_mapped_sample(mapped, fmt, dst_ptr);
			};
			for (std::size_t row = 0; row < static_cast<std::size_t>(info.rows); ++row) {
				for (std::size_t col = 0; col < static_cast<std::size_t>(info.cols); ++col) {
					const std::size_t idx = row * static_cast<std::size_t>(info.cols) + col;
					for (int s = 0; s < info.samples_per_pixel; ++s) {
						const std::uint8_t v = planes[static_cast<std::size_t>(s)][idx];
						std::byte* pix_dst;
						if (want_interleaved) {
							pix_dst = out + dst_row_bytes * row +
							    (col * static_cast<std::size_t>(info.samples_per_pixel) +
							     static_cast<std::size_t>(s)) * out_bytes;
						} else {
							pix_dst = out + static_cast<std::size_t>(s) * dst_plane_size +
							    dst_row_bytes * row +
							    col * out_bytes;
						}
						if (use_modality_lut) {
							map_sample(v, pix_dst);
						} else if (!rescale_plan.enabled && out_bytes == 1) {
							*reinterpret_cast<std::uint8_t*>(pix_dst) = v;
						} else {
							rescale_line_u8(&v, 1, pix_dst, out_bytes, rescale_plan);
						}
					}
				}
			}
		} else { // stored_bytes == 2
			auto map_sample = [&](const std::byte packed[2], std::byte* dst_ptr) {
				int stored_value = 0;
				if (info.signed_samples) {
					stored_value = load16<true, false>(packed);
				} else {
					stored_value = load16<false, false>(packed);
				}
				const uint16_t mapped = info.signed_samples
				    ? modality_lookup<true>(stored_value, modality_lut)
				    : modality_lookup<false>(stored_value, modality_lut);
				store_mapped_sample(mapped, fmt, dst_ptr);
			};
			for (std::size_t row = 0; row < static_cast<std::size_t>(info.rows); ++row) {
				for (std::size_t col = 0; col < static_cast<std::size_t>(info.cols); ++col) {
					const std::size_t idx = row * static_cast<std::size_t>(info.cols) + col;
					for (int s = 0; s < info.samples_per_pixel; ++s) {
						const std::uint8_t lo = planes[static_cast<std::size_t>(s) * 2 + 0][idx];
						const std::uint8_t hi = planes[static_cast<std::size_t>(s) * 2 + 1][idx];
						const std::byte packed[2] = {static_cast<std::byte>(lo), static_cast<std::byte>(hi)};
						std::byte* pix_dst;
						if (want_interleaved) {
							pix_dst = out + dst_row_bytes * row +
							    (col * static_cast<std::size_t>(info.samples_per_pixel) +
							     static_cast<std::size_t>(s)) * out_bytes;
						} else {
							pix_dst = out + static_cast<std::size_t>(s) * dst_plane_size +
							    dst_row_bytes * row +
							    col * out_bytes;
						}
						if (use_modality_lut) {
							map_sample(packed, pix_dst);
						} else if (info.signed_samples) {
							rescale_line_16<true>(packed, 1, pix_dst, /*need_swap=*/false, rescale_plan);
						} else {
							rescale_line_16<false>(packed, 1, pix_dst, /*need_swap=*/false, rescale_plan);
						}
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
