#include "pixel_codec.h"
#include "dicom.h"

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
		// Load common pixel metadata in one pass (tags are sorted, so the last is enough).
		constexpr Tag kLoadUntil = "PixelRepresentation"_tag; // (0028,0103)
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
		return info;
	}

	DecodeStatus decode_into(const DataSet& ds, std::span<std::byte> dst,
	    std::size_t frame, const DecodeOptions& opts) const override {
		// Ensure pixel data is available before any access.
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

		// Only stored-format copy is supported in this minimal decoder.
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

		// Determine frame count and offset.
		const auto frame_count = static_cast<std::size_t>(ds["NumberOfFrames"_tag].toLong(1));
		const std::size_t frame_bytes = src_row_bytes * static_cast<std::size_t>(info.rows);
		if (frame_bytes == 0) {
			return DecodeStatus::invalid_frame;
		}
		if (frame >= frame_count) {
			return DecodeStatus::invalid_frame;
		}
		const std::size_t frame_offset = frame_bytes * frame;
		if (span.size() < frame_offset + frame_bytes) {
			return DecodeStatus::invalid_frame;
		}

		const auto* src = reinterpret_cast<const std::uint8_t*>(span.data()) + frame_offset;
		const bool needs_swap = !ds.is_little_endian() && stored_bytes == 2;
		const bool stored_planar = (info.samples_per_pixel > 1) && (info.planar_config == 1);
		bool want_interleaved = true; // default
		switch (opts.output_layout) {
		case OutputLayout::interleaved: want_interleaved = true; break;
		case OutputLayout::planar: want_interleaved = false; break;
		case OutputLayout::keep_config: want_interleaved = !stored_planar; break;
		}

		// Fast path: layout/stride match and no endianness swap.
		if (!stored_planar && want_interleaved && !needs_swap && dst_row_bytes == src_row_bytes) {
			std::memcpy(dst.data(), src, frame_bytes);
			return DecodeStatus::ok;
		}

		// Planar -> interleaved conversion (and optional byte-swap).
		if (stored_planar && want_interleaved) {
			const std::size_t plane_size = frame_bytes / static_cast<std::size_t>(info.samples_per_pixel);
			const std::size_t plane_row_bytes = plane_size / static_cast<std::size_t>(info.rows);
			std::byte* out = dst.data();
			for (int r = 0; r < info.rows; ++r) {
				for (int c = 0; c < info.cols; ++c) {
					for (int s = 0; s < info.samples_per_pixel; ++s) {
						const auto* pix_src = src
						    + static_cast<std::size_t>(s) * plane_size
						    + static_cast<std::size_t>(r) * plane_row_bytes
						    + static_cast<std::size_t>(c) * stored_bytes;
						auto* pix_dst = reinterpret_cast<std::uint8_t*>(out
						    + dst_row_bytes * static_cast<std::size_t>(r)
						    + (static_cast<std::size_t>(c) * static_cast<std::size_t>(info.samples_per_pixel)
						       + static_cast<std::size_t>(s)) * stored_bytes);
						if (!needs_swap) {
							std::memcpy(pix_dst, pix_src, stored_bytes);
						} else {
							for (std::size_t i = 0; i + 1 < stored_bytes; i += 2) {
								pix_dst[i] = pix_src[i + 1];
								pix_dst[i + 1] = pix_src[i];
							}
							if ((stored_bytes & 1u) != 0) {
								pix_dst[stored_bytes - 1] = pix_src[stored_bytes - 1];
							}
						}
					}
				}
			}
			return DecodeStatus::ok;
		}

		std::byte* out = dst.data();

		if (stored_planar) {
			// Keep planar layout: copy plane by plane.
			const std::size_t plane_size = frame_bytes / static_cast<std::size_t>(info.samples_per_pixel);
			const std::size_t src_plane_row_bytes = src_row_bytes / static_cast<std::size_t>(info.samples_per_pixel);
			const std::size_t dst_plane_row_bytes = dst_row_bytes / static_cast<std::size_t>(info.samples_per_pixel);
			for (int s = 0; s < info.samples_per_pixel; ++s) {
				const auto* plane_src = src + static_cast<std::size_t>(s) * plane_size;
				auto* plane_dst = reinterpret_cast<std::uint8_t*>(out + static_cast<std::size_t>(s) * plane_size);
				for (int r = 0; r < info.rows; ++r) {
					const auto* row_src = plane_src + src_plane_row_bytes * static_cast<std::size_t>(r);
					auto* row_dst = plane_dst + dst_plane_row_bytes * static_cast<std::size_t>(r);
					if (!needs_swap) {
						std::memcpy(row_dst, row_src, src_plane_row_bytes);
					} else {
						for (std::size_t i = 0; i + 1 < src_plane_row_bytes; i += 2) {
							row_dst[i] = row_src[i + 1];
							row_dst[i + 1] = row_src[i];
						}
					}
				}
			}
		} else if (!want_interleaved) {
			// Interleaved -> planar conversion when caller requests planar output.
			const std::size_t plane_size = frame_bytes / static_cast<std::size_t>(info.samples_per_pixel);
			const std::size_t dst_plane_row_bytes = dst_row_bytes / static_cast<std::size_t>(info.samples_per_pixel);
			for (int r = 0; r < info.rows; ++r) {
				const auto* row_src = src + src_row_bytes * static_cast<std::size_t>(r);
				for (int c = 0; c < info.cols; ++c) {
					for (int s = 0; s < info.samples_per_pixel; ++s) {
						const auto* pix_src = row_src
						    + (static_cast<std::size_t>(c) * static_cast<std::size_t>(info.samples_per_pixel)
						       + static_cast<std::size_t>(s)) * stored_bytes;
						auto* pix_dst = reinterpret_cast<std::uint8_t*>(out
						    + static_cast<std::size_t>(s) * plane_size
						    + dst_plane_row_bytes * static_cast<std::size_t>(r)
						    + static_cast<std::size_t>(c) * stored_bytes);
						if (!needs_swap) {
							std::memcpy(pix_dst, pix_src, stored_bytes);
						} else {
							for (std::size_t i = 0; i + 1 < stored_bytes; i += 2) {
								pix_dst[i] = pix_src[i + 1];
								pix_dst[i + 1] = pix_src[i];
							}
							if ((stored_bytes & 1u) != 0) {
								pix_dst[stored_bytes - 1] = pix_src[stored_bytes - 1];
							}
						}
					}
				}
			}
		} else {
			// Interleaved (possibly with endian swap or stride fix-up).
			for (int r = 0; r < info.rows; ++r) {
				const auto* row_src = src + src_row_bytes * static_cast<std::size_t>(r);
				auto* row_dst = reinterpret_cast<std::uint8_t*>(out + dst_row_bytes * static_cast<std::size_t>(r));
				if (!needs_swap) {
					std::memcpy(row_dst, row_src, src_row_bytes);
				} else {
					for (std::size_t i = 0; i + 1 < src_row_bytes; i += 2) {
						row_dst[i] = row_src[i + 1];
						row_dst[i + 1] = row_src[i];
					}
				}
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
