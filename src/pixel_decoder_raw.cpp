#include "pixel_codec.h"
#include "dicom.h"
#include "pixel_rescale.h"

#include <array>
#include <cstring>
#include <mutex>
#include <string>

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

class RawPixelDecoder final : public PixelDecoder {
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
		// PixelRepresentation: 0=unsigned, 1=twos-complement signed
		info.signed_samples = ds["PixelRepresentation"_tag].toLong(0) == 1;              // (0028,0103)
		// PlanarConfiguration: 0=interleaved (RGBRGB...), 1=planar (RR..GG..BB..)
		info.planar_config = static_cast<int>(ds["PlanarConfiguration"_tag].toLong(0));  // (0028,0006)
		info.lossless = true;
		info.rescale_slope = ds["RescaleSlope"_tag].toDouble(1.0);                       // (0028,1053)
		info.rescale_intercept = ds["RescaleIntercept"_tag].toDouble(0.0);               // (0028,1052)
		info.has_modality_lut = ds["ModalityLUTSequence"_tag].sequence() != nullptr;     // (0028,3000)
		return info;
	}

	DecodeStatus decode_into(const DataSet& ds, std::span<std::byte> dst,
	    std::size_t frame, const DecodeOptions& opts) const override {
		// Ensure pixel data is available before any access.
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
		if (!pix || pix.vr().is_pixel_sequence()) {
			return DecodeStatus::unsupported_ts;
		}

		// Very light photometric handling: decline YBR->RGB conversion for now.
		if (opts.convert_to_rgb) {
			const auto& photo = ds["PhotometricInterpretation"_tag];
			if (auto v = photo.to_string_view()) {
				if (v->rfind("YBR", 0) == 0) {
					return DecodeStatus::unsupported_ts;
				}
			}
		}

		const auto span = pix.value_span();
		if (span.empty()) {
			return DecodeStatus::invalid_frame;
		}

		const auto frame_count = static_cast<std::size_t>(ds["NumberOfFrames"_tag].toLong(1));
		const std::size_t frame_bytes = src_row_bytes * static_cast<std::size_t>(info.rows);
		if (frame_bytes == 0 || frame >= frame_count || span.size() < frame_bytes * (frame + 1)) {
			return DecodeStatus::invalid_frame;
		}

		const auto* src = reinterpret_cast<const std::uint8_t*>(span.data()) + frame_bytes * frame;
		const bool needs_swap = !ds.is_little_endian() && stored_bytes == 2;
		const bool stored_planar = (info.samples_per_pixel > 1) && (info.planar_config == 1);
		bool want_interleaved = true; // default
		switch (opts.output_layout) {
		case OutputLayout::interleaved: want_interleaved = true; break;
		case OutputLayout::planar: want_interleaved = false; break;
		case OutputLayout::keep_config: want_interleaved = !stored_planar; break;
		}

		// Fast path: layout/stride match, no swap/rescale, same element size.
		if (!stored_planar && want_interleaved && !needs_swap &&
		    !rescale_plan.enabled && !use_modality_lut && out_bytes == stored_bytes &&
		    dst_row_bytes == src_row_bytes) {
			std::memcpy(dst.data(), src, frame_bytes);
			return DecodeStatus::ok;
		}

		std::byte* out = dst.data();
		auto map_sample = [&](const std::uint8_t* sample_src, std::byte* sample_dst) {
			int stored_value = 0;
			if (stored_bytes == 1) {
				const auto u = *sample_src;
				stored_value = info.signed_samples
				    ? static_cast<int>(static_cast<int8_t>(u))
				    : static_cast<int>(u);
			} else {
				if (info.signed_samples) {
					stored_value = needs_swap
					    ? load16<true, true>(reinterpret_cast<const std::byte*>(sample_src))
					    : load16<true, false>(reinterpret_cast<const std::byte*>(sample_src));
				} else {
					stored_value = needs_swap
					    ? load16<false, true>(reinterpret_cast<const std::byte*>(sample_src))
					    : load16<false, false>(reinterpret_cast<const std::byte*>(sample_src));
				}
			}
			const uint16_t mapped = info.signed_samples
			    ? modality_lookup<true>(stored_value, modality_lut)
			    : modality_lookup<false>(stored_value, modality_lut);
			store_mapped_sample(mapped, fmt, sample_dst);
		};

		if (stored_planar && want_interleaved) {
			const std::size_t plane_size = frame_bytes / static_cast<std::size_t>(info.samples_per_pixel);
			const std::size_t plane_row_bytes = plane_size / static_cast<std::size_t>(info.rows);
			for (int r = 0; r < info.rows; ++r) {
				for (int c = 0; c < info.cols; ++c) {
					for (int s = 0; s < info.samples_per_pixel; ++s) {
						const auto* pix_src = src
						    + static_cast<std::size_t>(s) * plane_size
						    + static_cast<std::size_t>(r) * plane_row_bytes
						    + static_cast<std::size_t>(c) * stored_bytes;
						auto* pix_dst = out
						    + dst_row_bytes * static_cast<std::size_t>(r)
						    + (static_cast<std::size_t>(c) * static_cast<std::size_t>(info.samples_per_pixel)
						       + static_cast<std::size_t>(s)) * out_bytes;
						if (stored_bytes == 1) {
							if (use_modality_lut) {
								map_sample(pix_src, pix_dst);
							} else {
								rescale_line_u8(reinterpret_cast<const std::uint8_t*>(pix_src), 1,
								    pix_dst, out_bytes, rescale_plan);
							}
						} else if (info.signed_samples) {
							if (use_modality_lut) {
								map_sample(pix_src, pix_dst);
							} else {
								rescale_line_16<true>(reinterpret_cast<const std::byte*>(pix_src), 1, pix_dst, needs_swap, rescale_plan);
							}
						} else {
							if (use_modality_lut) {
								map_sample(pix_src, pix_dst);
							} else {
								rescale_line_16<false>(reinterpret_cast<const std::byte*>(pix_src), 1, pix_dst, needs_swap, rescale_plan);
							}
						}
					}
				}
			}
			return DecodeStatus::ok;
		}

		if (stored_planar && !want_interleaved) {
			const std::size_t plane_size = frame_bytes / static_cast<std::size_t>(info.samples_per_pixel);
			const std::size_t src_plane_row_bytes = src_row_bytes / static_cast<std::size_t>(info.samples_per_pixel);
			const std::size_t dst_plane_row_bytes = dst_row_bytes;
			for (int s = 0; s < info.samples_per_pixel; ++s) {
				const auto* plane_src = src + static_cast<std::size_t>(s) * plane_size;
				auto* plane_dst = out + static_cast<std::size_t>(s) * dst_plane_row_bytes * static_cast<std::size_t>(info.rows);
				for (int r = 0; r < info.rows; ++r) {
					const auto* row_src = plane_src + src_plane_row_bytes * static_cast<std::size_t>(r);
					auto* row_dst = plane_dst + dst_plane_row_bytes * static_cast<std::size_t>(r);
					if (use_modality_lut) {
						for (int c = 0; c < info.cols; ++c) {
							const auto* pix_src = row_src + static_cast<std::size_t>(c) * stored_bytes;
							auto* pix_dst = row_dst + static_cast<std::size_t>(c) * out_bytes;
							map_sample(pix_src, pix_dst);
						}
					} else if (stored_bytes == 1) {
						rescale_line_u8(reinterpret_cast<const std::uint8_t*>(row_src),
						    static_cast<std::size_t>(info.cols), row_dst, out_bytes, rescale_plan);
					} else if (info.signed_samples) {
						rescale_line_16<true>(reinterpret_cast<const std::byte*>(row_src),
						    static_cast<std::size_t>(info.cols), row_dst, needs_swap, rescale_plan);
					} else {
						rescale_line_16<false>(reinterpret_cast<const std::byte*>(row_src),
						    static_cast<std::size_t>(info.cols), row_dst, needs_swap, rescale_plan);
					}
				}
			}
			return DecodeStatus::ok;
		}

		if (!stored_planar && !want_interleaved) {
			const std::size_t dst_plane_row_bytes = dst_row_bytes;
			for (int r = 0; r < info.rows; ++r) {
				const auto* row_src = src + src_row_bytes * static_cast<std::size_t>(r);
				for (int c = 0; c < info.cols; ++c) {
					for (int s = 0; s < info.samples_per_pixel; ++s) {
						const auto* pix_src = row_src
						    + (static_cast<std::size_t>(c) * static_cast<std::size_t>(info.samples_per_pixel)
						       + static_cast<std::size_t>(s)) * stored_bytes;
						auto* pix_dst = out
						    + static_cast<std::size_t>(s) * dst_plane_row_bytes * static_cast<std::size_t>(info.rows)
						    + dst_plane_row_bytes * static_cast<std::size_t>(r)
						    + static_cast<std::size_t>(c) * out_bytes;
						if (use_modality_lut) {
							map_sample(pix_src, pix_dst);
						} else if (stored_bytes == 1) {
							rescale_line_u8(reinterpret_cast<const std::uint8_t*>(pix_src), 1,
							    pix_dst, out_bytes, rescale_plan);
						} else if (info.signed_samples) {
							rescale_line_16<true>(reinterpret_cast<const std::byte*>(pix_src), 1, pix_dst, needs_swap, rescale_plan);
						} else {
							rescale_line_16<false>(reinterpret_cast<const std::byte*>(pix_src), 1, pix_dst, needs_swap, rescale_plan);
						}
					}
				}
			}
			return DecodeStatus::ok;
		}

		// Interleaved -> interleaved (possibly swap/rescale/stride)
		for (int r = 0; r < info.rows; ++r) {
			const auto* row_src = src + src_row_bytes * static_cast<std::size_t>(r);
			auto* row_dst = out + dst_row_bytes * static_cast<std::size_t>(r);
			const std::size_t sample_count = static_cast<std::size_t>(info.cols) *
			    static_cast<std::size_t>(info.samples_per_pixel);
			if (use_modality_lut) {
				for (std::size_t i = 0; i < sample_count; ++i) {
					const auto* pix_src = row_src + i * stored_bytes;
					auto* pix_dst = row_dst + i * out_bytes;
					map_sample(pix_src, pix_dst);
				}
			} else if (stored_bytes == 1) {
				rescale_line_u8(reinterpret_cast<const std::uint8_t*>(row_src),
				    sample_count, row_dst, out_bytes, rescale_plan);
			} else if (info.signed_samples) {
				rescale_line_16<true>(reinterpret_cast<const std::byte*>(row_src),
				    sample_count, row_dst, needs_swap, rescale_plan);
			} else {
				rescale_line_16<false>(reinterpret_cast<const std::byte*>(row_src),
				    sample_count, row_dst, needs_swap, rescale_plan);
			}
		}

		return DecodeStatus::ok;
	}
};

}  // namespace

void register_raw_decoders() {
	auto add_raw = []() { return std::make_unique<RawPixelDecoder>(); };
	register_decoder("ExplicitVRLittleEndian", add_raw);
	register_decoder("ImplicitVRLittleEndian", add_raw);
	register_decoder("Papyrus3ImplicitVRLittleEndian", add_raw);
	register_decoder("ExplicitVRBigEndian", add_raw);
}

}  // namespace dicom
