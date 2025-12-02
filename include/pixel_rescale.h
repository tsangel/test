#pragma once

#include <cstdint>
#include <cstddef>
#include <algorithm>
#include <vector>
#include "dicom.h"
#include "pixel_codec.h"

namespace dicom {

// Minimal rescale helpers shared by pixel decoders (raw, RLE, future JPEG*).
struct RescalePlan {
	bool enabled{false};
	bool use_float{false};
	bool signed_input{false};
	int offset{0};       // only when use_float==false
	double slope{1.0};   // only when use_float==true
	double intercept{0}; // only when use_float==true
};

inline RescalePlan make_rescale_plan(bool requires_rescale, bool has_modality_lut,
    bool signed_input, double slope, double intercept, bool output_float) {
	RescalePlan plan{};
	if (!requires_rescale || has_modality_lut) {
		return plan;
	}
	plan.enabled = true;
	plan.signed_input = signed_input;
	if (output_float) {
		plan.use_float = true;
		plan.slope = slope;
		plan.intercept = intercept;
	} else {
		plan.use_float = false;
		plan.offset = static_cast<int>(intercept);
	}
	return plan;
}

inline uint16_t swap16(uint16_t v) {
	return static_cast<uint16_t>((v >> 8) | (v << 8));
}

template <bool Signed, bool NeedSwap>
inline int load16(const std::byte* p) {
	uint16_t v = static_cast<uint16_t>(static_cast<uint8_t>(p[0])) |
	    (static_cast<uint16_t>(static_cast<uint8_t>(p[1])) << 8);
	if constexpr (NeedSwap) v = swap16(v);
	if constexpr (Signed) {
		return static_cast<int>(static_cast<int16_t>(v));
	}
	return static_cast<int>(v);
}

// Rescale from 8-bit unsigned source to uint8 / int16 / float
inline void rescale_line_u8(const uint8_t* src, std::size_t count,
    std::byte* dst, std::size_t dst_stride_samples, const RescalePlan& plan) {
	if (!plan.enabled) {
		std::copy(src, src + count, reinterpret_cast<uint8_t*>(dst));
		return;
	}
	if (plan.use_float) {
		float* out = reinterpret_cast<float*>(dst);
		const float a = static_cast<float>(plan.slope);
		const float b = static_cast<float>(plan.intercept);
		for (std::size_t i = 0; i < count; ++i) {
			out[i] = static_cast<float>(src[i]) * a + b;
		}
	} else {
		uint8_t* out = reinterpret_cast<uint8_t*>(dst);
		for (std::size_t i = 0; i < count; ++i) {
			int v = static_cast<int>(src[i]) + plan.offset;
			if (v < 0) v = 0;
			if (v > 255) v = 255;
			out[i] = static_cast<uint8_t>(v);
		}
	}
}

// Rescale from 16-bit source (signed or unsigned) to uint8 / int16 / float
template <bool Signed, bool NeedSwap>
inline void rescale_line_16(const std::byte* src, std::size_t count,
    std::byte* dst, const RescalePlan& plan) {
	if (!plan.enabled) {
		std::byte* out_bytes = dst;
		for (std::size_t i = 0; i < count; ++i) {
			const auto* p = src + i * 2;
			if constexpr (NeedSwap) {
				out_bytes[i * 2] = p[1];
				out_bytes[i * 2 + 1] = p[0];
			} else {
				out_bytes[i * 2] = p[0];
				out_bytes[i * 2 + 1] = p[1];
			}
		}
		return;
	}

	if (plan.use_float) {
		float* out = reinterpret_cast<float*>(dst);
		const float a = static_cast<float>(plan.slope);
		const float b = static_cast<float>(plan.intercept);
		for (std::size_t i = 0; i < count; ++i) {
			int v = load16<Signed, NeedSwap>(src + i * 2);
			out[i] = static_cast<float>(v) * a + b;
		}
	} else {
		int16_t* out = reinterpret_cast<int16_t*>(dst);
		for (std::size_t i = 0; i < count; ++i) {
			int v = load16<Signed, NeedSwap>(src + i * 2) + plan.offset;
			if (v < -32768) v = -32768;
			if (v > 32767) v = 32767;
			out[i] = static_cast<int16_t>(v);
		}
	}
}

// Convenience runtime-dispatch wrapper (keeps call sites simple).
template <bool Signed>
inline void rescale_line_16(const std::byte* src, std::size_t count,
    std::byte* dst, bool need_swap, const RescalePlan& plan) {
	if (need_swap) {
		rescale_line_16<Signed, true>(src, count, dst, plan);
	} else {
		rescale_line_16<Signed, false>(src, count, dst, plan);
	}
}

struct ModalityLut {
	std::vector<std::uint16_t> entries;
	int first_input{0};
	int entry_bits{16};
	bool valid() const { return !entries.empty(); }
};

inline bool load_modality_lut(const DataSet& ds, ModalityLut& lut) {
	using namespace dicom::literals;
	auto* seq = ds["ModalityLUTSequence"_tag].sequence();
	if (!seq || seq->size() == 0) return false;
	DataSet* item = seq->get_dataset(0);
	if (!item) return false;
	auto desc_span = item->get_dataelement("LUTDescriptor"_tag)->value_span();
	if (desc_span.size() < 6) return false;
	const bool le = ds.is_little_endian();
	auto load_u16 = [le](const std::uint8_t* p) {
		uint16_t v = static_cast<uint16_t>(p[0]) |
		    (static_cast<uint16_t>(p[1]) << 8);
		if (!le) v = swap16(v);
		return v;
	};
	uint32_t num_entries = load_u16(reinterpret_cast<const std::uint8_t*>(desc_span.data()));
	if (num_entries == 0) num_entries = 65536;
	const int first_input = static_cast<int>(load_u16(reinterpret_cast<const std::uint8_t*>(desc_span.data()) + 2));
	const int bits = static_cast<int>(load_u16(reinterpret_cast<const std::uint8_t*>(desc_span.data()) + 4));
	if (bits != 8 && bits != 16) return false;
	auto data_span = item->get_dataelement("LUTData"_tag)->value_span();
	const std::size_t expected_bytes = static_cast<std::size_t>(num_entries) * (bits / 8);
	if (data_span.size() < expected_bytes) return false;
	lut.entries.resize(num_entries);
	if (bits == 8) {
		for (std::size_t i = 0; i < num_entries; ++i) {
			lut.entries[i] = static_cast<uint16_t>(static_cast<uint8_t>(data_span[i]));
		}
	} else {
		for (std::size_t i = 0; i < num_entries; ++i) {
			lut.entries[i] = load_u16(reinterpret_cast<const std::uint8_t*>(data_span.data()) + i * 2);
		}
	}
	lut.first_input = first_input;
	lut.entry_bits = bits;
	return true;
}

template <bool Signed>
inline uint16_t modality_lookup(int stored, const ModalityLut& lut) {
	int idx = stored - lut.first_input;
	if (idx < 0) idx = 0;
	if (idx >= static_cast<int>(lut.entries.size())) idx = static_cast<int>(lut.entries.size()) - 1;
	return lut.entries[static_cast<std::size_t>(idx)];
}

inline void store_mapped_sample(uint16_t mapped, PixelFormat fmt, std::byte* dst) {
	switch (fmt) {
	case PixelFormat::uint8: {
		uint16_t v = mapped > 255 ? 255 : mapped;
		reinterpret_cast<uint8_t*>(dst)[0] = static_cast<uint8_t>(v);
		break;
	}
	case PixelFormat::int16: {
		reinterpret_cast<int16_t*>(dst)[0] = static_cast<int16_t>(mapped);
		break;
	}
	case PixelFormat::float32: {
		reinterpret_cast<float*>(dst)[0] = static_cast<float>(mapped);
		break;
	}
	default:
		break;
	}
}

}  // namespace dicom
