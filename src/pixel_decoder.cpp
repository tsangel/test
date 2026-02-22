#include "pixel_decoder_detail.hpp"

#include "dicom_endian.h"
#include "diagnostics.h"

#include <bit>
#include <cmath>
#include <cstring>
#include <type_traits>

using namespace dicom::literals;
namespace diag = dicom::diag;

namespace dicom {
namespace pixel::detail {

decode_backend select_decode_backend(uid::WellKnown ts) noexcept {
	if (ts.is_uncompressed()) {
		return decode_backend::raw;
	}
	if (ts.is_rle()) {
		return decode_backend::rle;
	}
	if (ts.is_jpeg_family()) {
		return decode_backend::jpeg_family;
	}
	return decode_backend::unsupported;
}

planar_transform select_planar_transform(Planar src_planar, Planar dst_planar) noexcept {
	if (src_planar == Planar::interleaved) {
		return (dst_planar == Planar::interleaved)
		           ? planar_transform::interleaved_to_interleaved
		           : planar_transform::interleaved_to_planar;
	}
	return (dst_planar == Planar::interleaved)
	           ? planar_transform::planar_to_interleaved
	           : planar_transform::planar_to_planar;
}

std::size_t sv_dtype_bytes(DataType sv_dtype) noexcept {
	switch (sv_dtype) {
	case DataType::u8:
	case DataType::s8:
		return 1;
	case DataType::u16:
	case DataType::s16:
		return 2;
	case DataType::u32:
	case DataType::s32:
	case DataType::f32:
		return 4;
	case DataType::f64:
		return 8;
	default:
		return 0;
	}
}

namespace {

template <typename SampleT>
inline SampleT load_scaled_source_sample(const std::uint8_t* src, bool source_little_endian) noexcept {
	if constexpr (sizeof(SampleT) == 1) {
		SampleT value{};
		std::memcpy(&value, src, sizeof(SampleT));
		return value;
	} else if constexpr (std::is_integral_v<SampleT>) {
		return endian::load_value<SampleT>(src, source_little_endian);
	} else if constexpr (std::is_same_v<SampleT, float>) {
		const auto bits = endian::load_value<std::uint32_t>(src, source_little_endian);
		return std::bit_cast<float>(bits);
	} else {
		static_assert(std::is_same_v<SampleT, double>, "unsupported scaled sample type");
		const auto bits = endian::load_value<std::uint64_t>(src, source_little_endian);
		return std::bit_cast<double>(bits);
	}
}

template <typename SampleT>
void decode_mono_scaled_samples_into(const std::uint8_t* src_frame, std::uint8_t* dst_base,
    std::size_t rows, std::size_t cols, std::size_t src_row_bytes, std::size_t dst_row_bytes,
    bool source_little_endian, double slope, double intercept) {
	for (std::size_t r = 0; r < rows; ++r) {
		const auto* src_sample = src_frame + r * src_row_bytes;
		auto* dst_sample = dst_base + r * dst_row_bytes;
		for (std::size_t c = 0; c < cols; ++c) {
			const auto sv = load_scaled_source_sample<SampleT>(src_sample, source_little_endian);
			const auto modality_value =
			    static_cast<float>(static_cast<double>(sv) * slope + intercept);
			std::memcpy(dst_sample, &modality_value, sizeof(float));
			src_sample += sizeof(SampleT);
			dst_sample += sizeof(float);
		}
	}
}

template <typename SampleT>
void decode_mono_lut_samples_into(const std::uint8_t* src_frame, std::uint8_t* dst_base,
    std::size_t rows, std::size_t cols, std::size_t src_row_bytes, std::size_t dst_row_bytes,
    bool source_little_endian, const ModalityLut& lut) {
	const auto last_index = static_cast<std::int64_t>(lut.values.size() - 1);
	for (std::size_t r = 0; r < rows; ++r) {
		const auto* src_sample = src_frame + r * src_row_bytes;
		auto* dst_sample = dst_base + r * dst_row_bytes;
		for (std::size_t c = 0; c < cols; ++c) {
			const auto sv = load_scaled_source_sample<SampleT>(src_sample, source_little_endian);
			std::int64_t lut_index = static_cast<std::int64_t>(sv) - lut.first_mapped;
			if (lut_index < 0) {
				lut_index = 0;
			} else if (lut_index > last_index) {
				lut_index = last_index;
			}
			const auto modality_value = lut.values[static_cast<std::size_t>(lut_index)];
			std::memcpy(dst_sample, &modality_value, sizeof(float));
			src_sample += sizeof(SampleT);
			dst_sample += sizeof(float);
		}
	}
}

template <std::size_t Bytes, bool Swap>
inline void copy_sample(const std::uint8_t* src, std::uint8_t* dst) noexcept {
	if constexpr (!Swap || Bytes == 1) {
		std::memcpy(dst, src, Bytes);
		return;
	}

	if constexpr (Bytes == 2) {
		dst[0] = src[1];
		dst[1] = src[0];
		return;
	}
	if constexpr (Bytes == 4) {
		dst[0] = src[3];
		dst[1] = src[2];
		dst[2] = src[1];
		dst[3] = src[0];
		return;
	}

	// TODO: Generic 8-byte swap path is intentionally naive; optimize later if it becomes hot.
	for (std::size_t i = 0; i < Bytes; ++i) {
		dst[i] = src[Bytes - 1 - i];
	}
}

template <std::size_t Bytes, std::size_t SamplesPerPixel>
void copy_interleaved_to_planar_noswap_fast(const std::uint8_t* src_frame, std::uint8_t* dst_base,
    std::size_t rows, std::size_t cols, std::size_t src_row_bytes, std::size_t dst_row_bytes) noexcept {
	static_assert((Bytes == 1 || Bytes == 2), "fast path supports Bytes=1/2");
	static_assert((SamplesPerPixel == 3 || SamplesPerPixel == 4), "fast path supports SPP=3/4");

	const std::size_t dst_plane_bytes = dst_row_bytes * rows;
	for (std::size_t r = 0; r < rows; ++r) {
		const auto* src = src_frame + r * src_row_bytes;
		auto* d0 = dst_base + r * dst_row_bytes;
		auto* d1 = d0 + dst_plane_bytes;
		auto* d2 = d1 + dst_plane_bytes;
		if constexpr (SamplesPerPixel == 3) {
			for (std::size_t c = 0; c < cols; ++c) {
				if constexpr (Bytes == 1) {
					*d0++ = *src++;
					*d1++ = *src++;
					*d2++ = *src++;
				} else {
					copy_sample<2, false>(src, d0);
					src += 2;
					d0 += 2;

					copy_sample<2, false>(src, d1);
					src += 2;
					d1 += 2;

					copy_sample<2, false>(src, d2);
					src += 2;
					d2 += 2;
				}
			}
		} else {
			auto* d3 = d2 + dst_plane_bytes;
			for (std::size_t c = 0; c < cols; ++c) {
				if constexpr (Bytes == 1) {
					*d0++ = *src++;
					*d1++ = *src++;
					*d2++ = *src++;
					*d3++ = *src++;
				} else {
					copy_sample<2, false>(src, d0);
					src += 2;
					d0 += 2;

					copy_sample<2, false>(src, d1);
					src += 2;
					d1 += 2;

					copy_sample<2, false>(src, d2);
					src += 2;
					d2 += 2;

					copy_sample<2, false>(src, d3);
					src += 2;
					d3 += 2;
				}
			}
		}
	}
}

template <std::size_t Bytes, std::size_t SamplesPerPixel>
void copy_planar_to_interleaved_noswap_fast(const std::uint8_t* src_frame, std::uint8_t* dst_base,
    std::size_t rows, std::size_t cols, std::size_t src_row_bytes, std::size_t dst_row_bytes) noexcept {
	static_assert((Bytes == 1 || Bytes == 2), "fast path supports Bytes=1/2");
	static_assert((SamplesPerPixel == 3 || SamplesPerPixel == 4), "fast path supports SPP=3/4");

	const std::size_t src_plane_bytes = src_row_bytes * rows;
	for (std::size_t r = 0; r < rows; ++r) {
		auto* dst = dst_base + r * dst_row_bytes;
		const auto* s0 = src_frame + r * src_row_bytes;
		const auto* s1 = s0 + src_plane_bytes;
		const auto* s2 = s1 + src_plane_bytes;
		if constexpr (SamplesPerPixel == 3) {
			for (std::size_t c = 0; c < cols; ++c) {
				if constexpr (Bytes == 1) {
					*dst++ = *s0++;
					*dst++ = *s1++;
					*dst++ = *s2++;
				} else {
					copy_sample<2, false>(s0, dst);
					s0 += 2;
					dst += 2;

					copy_sample<2, false>(s1, dst);
					s1 += 2;
					dst += 2;

					copy_sample<2, false>(s2, dst);
					s2 += 2;
					dst += 2;
				}
			}
		} else {
			const auto* s3 = s2 + src_plane_bytes;
			for (std::size_t c = 0; c < cols; ++c) {
				if constexpr (Bytes == 1) {
					*dst++ = *s0++;
					*dst++ = *s1++;
					*dst++ = *s2++;
					*dst++ = *s3++;
				} else {
					copy_sample<2, false>(s0, dst);
					s0 += 2;
					dst += 2;

					copy_sample<2, false>(s1, dst);
					s1 += 2;
					dst += 2;

					copy_sample<2, false>(s2, dst);
					s2 += 2;
					dst += 2;

					copy_sample<2, false>(s3, dst);
					s3 += 2;
					dst += 2;
				}
			}
		}
	}
}

template <std::size_t Bytes, bool Swap>
void copy_interleaved_to_interleaved(const std::uint8_t* src_frame, std::uint8_t* dst_base,
    std::size_t rows, std::size_t cols, std::size_t samples_per_pixel,
    std::size_t src_row_bytes, std::size_t dst_row_bytes) noexcept {
	const std::size_t packed_row_bytes = cols * samples_per_pixel * Bytes;
	if constexpr (!Swap || Bytes == 1) {
		for (std::size_t r = 0; r < rows; ++r) {
			const auto* src_row = src_frame + r * src_row_bytes;
			auto* dst_row = dst_base + r * dst_row_bytes;
			std::memcpy(dst_row, src_row, packed_row_bytes);
		}
		return;
	}

	const std::size_t samples_per_row = cols * samples_per_pixel;
	for (std::size_t r = 0; r < rows; ++r) {
		const auto* src_row = src_frame + r * src_row_bytes;
		auto* dst_row = dst_base + r * dst_row_bytes;
		for (std::size_t s = 0; s < samples_per_row; ++s) {
			copy_sample<Bytes, Swap>(src_row + s * Bytes, dst_row + s * Bytes);
		}
	}
}

template <std::size_t Bytes, bool Swap>
void copy_interleaved_to_planar(const std::uint8_t* src_frame, std::uint8_t* dst_base,
    std::size_t rows, std::size_t cols, std::size_t samples_per_pixel,
    std::size_t src_row_bytes, std::size_t dst_row_bytes) noexcept {
	const std::size_t dst_plane_bytes = dst_row_bytes * rows;
	auto copy_spp3 = [&]() {
		for (std::size_t r = 0; r < rows; ++r) {
			const auto* src_sample = src_frame + r * src_row_bytes;
			auto* dst0 = dst_base + r * dst_row_bytes;
			auto* dst1 = dst_base + dst_plane_bytes + r * dst_row_bytes;
			auto* dst2 = dst_base + 2 * dst_plane_bytes + r * dst_row_bytes;
			for (std::size_t c = 0; c < cols; ++c) {
				copy_sample<Bytes, Swap>(src_sample, dst0);
				src_sample += Bytes;
				dst0 += Bytes;
				copy_sample<Bytes, Swap>(src_sample, dst1);
				src_sample += Bytes;
				dst1 += Bytes;
				copy_sample<Bytes, Swap>(src_sample, dst2);
				src_sample += Bytes;
				dst2 += Bytes;
			}
		}
	};
	auto copy_spp4 = [&]() {
		for (std::size_t r = 0; r < rows; ++r) {
			const auto* src_sample = src_frame + r * src_row_bytes;
			auto* dst0 = dst_base + r * dst_row_bytes;
			auto* dst1 = dst_base + dst_plane_bytes + r * dst_row_bytes;
			auto* dst2 = dst_base + 2 * dst_plane_bytes + r * dst_row_bytes;
			auto* dst3 = dst_base + 3 * dst_plane_bytes + r * dst_row_bytes;
			for (std::size_t c = 0; c < cols; ++c) {
				copy_sample<Bytes, Swap>(src_sample, dst0);
				src_sample += Bytes;
				dst0 += Bytes;
				copy_sample<Bytes, Swap>(src_sample, dst1);
				src_sample += Bytes;
				dst1 += Bytes;
				copy_sample<Bytes, Swap>(src_sample, dst2);
				src_sample += Bytes;
				dst2 += Bytes;
				copy_sample<Bytes, Swap>(src_sample, dst3);
				src_sample += Bytes;
				dst3 += Bytes;
			}
		}
	};

	switch (samples_per_pixel) {
	case 3:
		copy_spp3();
		return;
	case 4:
		copy_spp4();
		return;
	default:
		return; // unreachable: decode_raw_into validates spp and run_planar_transform_copy handles spp=1
	}
}

template <std::size_t Bytes, bool Swap>
void copy_planar_to_interleaved(const std::uint8_t* src_frame, std::uint8_t* dst_base,
    std::size_t rows, std::size_t cols, std::size_t samples_per_pixel,
    std::size_t src_row_bytes, std::size_t dst_row_bytes) noexcept {
	const std::size_t src_plane_bytes = src_row_bytes * rows;
	auto copy_spp3 = [&]() {
		for (std::size_t r = 0; r < rows; ++r) {
			const auto* src0 = src_frame + r * src_row_bytes;
			const auto* src1 = src_frame + src_plane_bytes + r * src_row_bytes;
			const auto* src2 = src_frame + 2 * src_plane_bytes + r * src_row_bytes;
			auto* dst_sample = dst_base + r * dst_row_bytes;
			for (std::size_t c = 0; c < cols; ++c) {
				copy_sample<Bytes, Swap>(src0, dst_sample);
				src0 += Bytes;
				dst_sample += Bytes;
				copy_sample<Bytes, Swap>(src1, dst_sample);
				src1 += Bytes;
				dst_sample += Bytes;
				copy_sample<Bytes, Swap>(src2, dst_sample);
				src2 += Bytes;
				dst_sample += Bytes;
			}
		}
	};
	auto copy_spp4 = [&]() {
		for (std::size_t r = 0; r < rows; ++r) {
			const auto* src0 = src_frame + r * src_row_bytes;
			const auto* src1 = src_frame + src_plane_bytes + r * src_row_bytes;
			const auto* src2 = src_frame + 2 * src_plane_bytes + r * src_row_bytes;
			const auto* src3 = src_frame + 3 * src_plane_bytes + r * src_row_bytes;
			auto* dst_sample = dst_base + r * dst_row_bytes;
			for (std::size_t c = 0; c < cols; ++c) {
				copy_sample<Bytes, Swap>(src0, dst_sample);
				src0 += Bytes;
				dst_sample += Bytes;
				copy_sample<Bytes, Swap>(src1, dst_sample);
				src1 += Bytes;
				dst_sample += Bytes;
				copy_sample<Bytes, Swap>(src2, dst_sample);
				src2 += Bytes;
				dst_sample += Bytes;
				copy_sample<Bytes, Swap>(src3, dst_sample);
				src3 += Bytes;
				dst_sample += Bytes;
			}
		}
	};

	switch (samples_per_pixel) {
	case 3:
		copy_spp3();
		return;
	case 4:
		copy_spp4();
		return;
	default:
		return; // unreachable: decode_raw_into validates spp and run_planar_transform_copy handles spp=1
	}
}

template <std::size_t Bytes, bool Swap>
void copy_planar_to_planar(const std::uint8_t* src_frame, std::uint8_t* dst_base,
    std::size_t rows, std::size_t cols, std::size_t samples_per_pixel,
    std::size_t src_row_bytes, std::size_t dst_row_bytes) noexcept {
	const std::size_t packed_row_bytes = cols * Bytes;
	const std::size_t src_plane_bytes = src_row_bytes * rows;
	const std::size_t dst_plane_bytes = dst_row_bytes * rows;

	if constexpr (!Swap || Bytes == 1) {
		for (std::size_t p = 0; p < samples_per_pixel; ++p) {
			const auto* src_plane = src_frame + p * src_plane_bytes;
			auto* dst_plane = dst_base + p * dst_plane_bytes;
			for (std::size_t r = 0; r < rows; ++r) {
				std::memcpy(dst_plane + r * dst_row_bytes, src_plane + r * src_row_bytes, packed_row_bytes);
			}
		}
		return;
	}

	for (std::size_t p = 0; p < samples_per_pixel; ++p) {
		const auto* src_plane = src_frame + p * src_plane_bytes;
		auto* dst_plane = dst_base + p * dst_plane_bytes;
		for (std::size_t r = 0; r < rows; ++r) {
			const auto* src_row = src_plane + r * src_row_bytes;
			auto* dst_row = dst_plane + r * dst_row_bytes;
			for (std::size_t c = 0; c < cols; ++c) {
				copy_sample<Bytes, Swap>(src_row + c * Bytes, dst_row + c * Bytes);
			}
		}
	}
}

template <std::size_t Bytes, bool Swap>
void dispatch_planar_transform_copy(planar_transform transform,
    const std::uint8_t* src_frame, std::uint8_t* dst_base,
    std::size_t rows, std::size_t cols, std::size_t samples_per_pixel,
    std::size_t src_row_bytes, std::size_t dst_row_bytes) noexcept {
	switch (transform) {
	case planar_transform::interleaved_to_interleaved:
		copy_interleaved_to_interleaved<Bytes, Swap>(
		    src_frame, dst_base, rows, cols, samples_per_pixel, src_row_bytes, dst_row_bytes);
		return;
	case planar_transform::interleaved_to_planar:
		copy_interleaved_to_planar<Bytes, Swap>(
		    src_frame, dst_base, rows, cols, samples_per_pixel, src_row_bytes, dst_row_bytes);
		return;
	case planar_transform::planar_to_interleaved:
		copy_planar_to_interleaved<Bytes, Swap>(
		    src_frame, dst_base, rows, cols, samples_per_pixel, src_row_bytes, dst_row_bytes);
		return;
	case planar_transform::planar_to_planar:
		copy_planar_to_planar<Bytes, Swap>(
		    src_frame, dst_base, rows, cols, samples_per_pixel, src_row_bytes, dst_row_bytes);
		return;
	}
}

template <bool Swap>
void copy_interleaved_to_interleaved_by_bytes(std::size_t bytes_per_sample,
    const std::uint8_t* src_frame, std::uint8_t* dst_base,
    std::size_t rows, std::size_t cols, std::size_t samples_per_pixel,
    std::size_t src_row_bytes, std::size_t dst_row_bytes) {
	switch (bytes_per_sample) {
	case 1:
		copy_interleaved_to_interleaved<1, Swap>(
		    src_frame, dst_base, rows, cols, samples_per_pixel, src_row_bytes, dst_row_bytes);
		return;
	case 2:
		copy_interleaved_to_interleaved<2, Swap>(
		    src_frame, dst_base, rows, cols, samples_per_pixel, src_row_bytes, dst_row_bytes);
		return;
	case 4:
		copy_interleaved_to_interleaved<4, Swap>(
		    src_frame, dst_base, rows, cols, samples_per_pixel, src_row_bytes, dst_row_bytes);
		return;
	case 8:
		copy_interleaved_to_interleaved<8, Swap>(
		    src_frame, dst_base, rows, cols, samples_per_pixel, src_row_bytes, dst_row_bytes);
		return;
	default:
		diag::error_and_throw(
		    "pixel::decode_frame_into reason=unsupported bytes_per_sample={}", bytes_per_sample);
		return;
	}
}

template <bool Swap>
void dispatch_planar_transform_copy_by_bytes(planar_transform transform, std::size_t bytes_per_sample,
    const std::uint8_t* src_frame, std::uint8_t* dst_base,
    std::size_t rows, std::size_t cols, std::size_t samples_per_pixel,
    std::size_t src_row_bytes, std::size_t dst_row_bytes) {
	switch (bytes_per_sample) {
	case 1:
		dispatch_planar_transform_copy<1, Swap>(
		    transform, src_frame, dst_base, rows, cols, samples_per_pixel, src_row_bytes, dst_row_bytes);
		return;
	case 2:
		dispatch_planar_transform_copy<2, Swap>(
		    transform, src_frame, dst_base, rows, cols, samples_per_pixel, src_row_bytes, dst_row_bytes);
		return;
	case 4:
		dispatch_planar_transform_copy<4, Swap>(
		    transform, src_frame, dst_base, rows, cols, samples_per_pixel, src_row_bytes, dst_row_bytes);
		return;
	case 8:
		dispatch_planar_transform_copy<8, Swap>(
		    transform, src_frame, dst_base, rows, cols, samples_per_pixel, src_row_bytes, dst_row_bytes);
		return;
	default:
		diag::error_and_throw(
		    "pixel::decode_frame_into reason=unsupported bytes_per_sample={}", bytes_per_sample);
		return;
	}
}

} // namespace

void decode_mono_scaled_into_f32(const DicomFile& df, const DicomFile::pixel_info_t& info,
    const std::uint8_t* src_frame, std::span<std::uint8_t> dst,
    const DecodeStrides& dst_strides, std::size_t rows, std::size_t cols,
    std::size_t src_row_bytes) {
	const auto& ds = df.dataset();
	if (info.samples_per_pixel != 1) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=scaled output supports SamplesPerPixel=1 only",
		    df.path());
	}

	const auto source_little_endian = ds.is_little_endian();
	const auto modality_lut = df.modality_lut();
	if (modality_lut) {
		switch (info.sv_dtype) {
		case DataType::u8:
			decode_mono_lut_samples_into<std::uint8_t>(
			    src_frame, dst.data(), rows, cols, src_row_bytes, dst_strides.row,
			    source_little_endian, *modality_lut);
			return;
		case DataType::s8:
			decode_mono_lut_samples_into<std::int8_t>(
			    src_frame, dst.data(), rows, cols, src_row_bytes, dst_strides.row,
			    source_little_endian, *modality_lut);
			return;
		case DataType::u16:
			decode_mono_lut_samples_into<std::uint16_t>(
			    src_frame, dst.data(), rows, cols, src_row_bytes, dst_strides.row,
			    source_little_endian, *modality_lut);
			return;
		case DataType::s16:
			decode_mono_lut_samples_into<std::int16_t>(
			    src_frame, dst.data(), rows, cols, src_row_bytes, dst_strides.row,
			    source_little_endian, *modality_lut);
			return;
		case DataType::u32:
			decode_mono_lut_samples_into<std::uint32_t>(
			    src_frame, dst.data(), rows, cols, src_row_bytes, dst_strides.row,
			    source_little_endian, *modality_lut);
			return;
		case DataType::s32:
			decode_mono_lut_samples_into<std::int32_t>(
			    src_frame, dst.data(), rows, cols, src_row_bytes, dst_strides.row,
			    source_little_endian, *modality_lut);
			return;
		default:
			diag::error_and_throw(
			    "pixel::decode_frame_into file={} reason=Modality LUT path supports integral sv_dtype only",
			    df.path());
			return;
		}
	}

	const auto slope = ds["RescaleSlope"_tag].to_double().value_or(1.0);
	const auto intercept = ds["RescaleIntercept"_tag].to_double().value_or(0.0);
	if (!std::isfinite(slope) || !std::isfinite(intercept)) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=RescaleSlope/RescaleIntercept must be finite",
		    df.path());
	}

	switch (info.sv_dtype) {
	case DataType::u8:
		decode_mono_scaled_samples_into<std::uint8_t>(
		    src_frame, dst.data(), rows, cols, src_row_bytes, dst_strides.row,
		    source_little_endian, slope, intercept);
		return;
	case DataType::s8:
		decode_mono_scaled_samples_into<std::int8_t>(
		    src_frame, dst.data(), rows, cols, src_row_bytes, dst_strides.row,
		    source_little_endian, slope, intercept);
		return;
	case DataType::u16:
		decode_mono_scaled_samples_into<std::uint16_t>(
		    src_frame, dst.data(), rows, cols, src_row_bytes, dst_strides.row,
		    source_little_endian, slope, intercept);
		return;
	case DataType::s16:
		decode_mono_scaled_samples_into<std::int16_t>(
		    src_frame, dst.data(), rows, cols, src_row_bytes, dst_strides.row,
		    source_little_endian, slope, intercept);
		return;
	case DataType::u32:
		decode_mono_scaled_samples_into<std::uint32_t>(
		    src_frame, dst.data(), rows, cols, src_row_bytes, dst_strides.row,
		    source_little_endian, slope, intercept);
		return;
	case DataType::s32:
		decode_mono_scaled_samples_into<std::int32_t>(
		    src_frame, dst.data(), rows, cols, src_row_bytes, dst_strides.row,
		    source_little_endian, slope, intercept);
		return;
	case DataType::f32:
		decode_mono_scaled_samples_into<float>(
		    src_frame, dst.data(), rows, cols, src_row_bytes, dst_strides.row,
		    source_little_endian, slope, intercept);
		return;
	case DataType::f64:
		decode_mono_scaled_samples_into<double>(
		    src_frame, dst.data(), rows, cols, src_row_bytes, dst_strides.row,
		    source_little_endian, slope, intercept);
		return;
	default:
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=scaled output does not support sv_dtype={}",
		    df.path(), static_cast<int>(info.sv_dtype));
		return;
	}
}

void run_planar_transform_copy(planar_transform transform, std::size_t bytes_per_sample, bool needs_swap,
    const std::uint8_t* src_frame, std::uint8_t* dst_base,
    std::size_t rows, std::size_t cols, std::size_t samples_per_pixel,
    std::size_t src_row_bytes, std::size_t dst_row_bytes) {
	// For single-channel data, Planar/interleaved layouts are equivalent.
	// Route through ii kernel and avoid redundant spp=1 branches in ip/pi kernels.
	if (samples_per_pixel == 1) {
		if (needs_swap) {
			copy_interleaved_to_interleaved_by_bytes<true>(
			    bytes_per_sample, src_frame, dst_base, rows, cols, samples_per_pixel, src_row_bytes, dst_row_bytes);
		} else {
			copy_interleaved_to_interleaved_by_bytes<false>(
			    bytes_per_sample, src_frame, dst_base, rows, cols, samples_per_pixel, src_row_bytes, dst_row_bytes);
		}
		return;
	}

	if (!needs_swap && (bytes_per_sample == 1 || bytes_per_sample == 2) &&
	    (samples_per_pixel == 3 || samples_per_pixel == 4)) {
		if (transform == planar_transform::interleaved_to_planar) {
			if (bytes_per_sample == 1) {
				if (samples_per_pixel == 3) {
					copy_interleaved_to_planar_noswap_fast<1, 3>(
					    src_frame, dst_base, rows, cols, src_row_bytes, dst_row_bytes);
				} else {
					copy_interleaved_to_planar_noswap_fast<1, 4>(
					    src_frame, dst_base, rows, cols, src_row_bytes, dst_row_bytes);
				}
			} else {
				if (samples_per_pixel == 3) {
					copy_interleaved_to_planar_noswap_fast<2, 3>(
					    src_frame, dst_base, rows, cols, src_row_bytes, dst_row_bytes);
				} else {
					copy_interleaved_to_planar_noswap_fast<2, 4>(
					    src_frame, dst_base, rows, cols, src_row_bytes, dst_row_bytes);
				}
			}
			return;
		}
		if (transform == planar_transform::planar_to_interleaved) {
			if (bytes_per_sample == 1) {
				if (samples_per_pixel == 3) {
					copy_planar_to_interleaved_noswap_fast<1, 3>(
					    src_frame, dst_base, rows, cols, src_row_bytes, dst_row_bytes);
				} else {
					copy_planar_to_interleaved_noswap_fast<1, 4>(
					    src_frame, dst_base, rows, cols, src_row_bytes, dst_row_bytes);
				}
			} else {
				if (samples_per_pixel == 3) {
					copy_planar_to_interleaved_noswap_fast<2, 3>(
					    src_frame, dst_base, rows, cols, src_row_bytes, dst_row_bytes);
				} else {
					copy_planar_to_interleaved_noswap_fast<2, 4>(
					    src_frame, dst_base, rows, cols, src_row_bytes, dst_row_bytes);
				}
			}
			return;
		}
	}

	if (needs_swap) {
		dispatch_planar_transform_copy_by_bytes<true>(
		    transform, bytes_per_sample, src_frame, dst_base, rows, cols, samples_per_pixel, src_row_bytes, dst_row_bytes);
	} else {
		dispatch_planar_transform_copy_by_bytes<false>(
		    transform, bytes_per_sample, src_frame, dst_base, rows, cols, samples_per_pixel, src_row_bytes, dst_row_bytes);
	}
}

} // namespace pixel::detail

namespace pixel {

namespace {

bool has_rescale_transform_metadata(const DicomFile& df) {
	const auto& ds = df.dataset();
	return static_cast<bool>(ds["RescaleSlope"_tag]) ||
	    static_cast<bool>(ds["RescaleIntercept"_tag]);
}

bool should_use_scaled_output_impl(
    const DicomFile& df, const DicomFile::pixel_info_t& info, const DecodeOptions& opt) {
	if (!opt.scaled) {
		return false;
	}
	if (!info.has_pixel_data) {
		return false;
	}
	if (info.samples_per_pixel != 1) {
		return false;
	}

	const auto& ds = df.dataset();
	const bool has_modality_lut = static_cast<bool>(ds["ModalityLUTSequence"_tag]);
	if (has_modality_lut) {
		// Validate and load LUT eagerly so malformed metadata still fails loudly.
		(void)df.modality_lut();
		return true;
	}

	return has_rescale_transform_metadata(df);
}

} // namespace

bool should_use_scaled_output(const DicomFile& df, const DecodeOptions& opt) {
	const auto& info = df.pixel_info();
	return should_use_scaled_output_impl(df, info, opt);
}

void decode_frame_into(const DicomFile& df, std::size_t frame_index,
    std::span<std::uint8_t> dst, const DecodeOptions& opt) {
	const auto& info = df.pixel_info();
	auto effective_opt = opt;
	effective_opt.scaled = should_use_scaled_output_impl(df, info, opt);

	const auto dst_strides = df.calc_decode_strides(effective_opt);
	decode_frame_into(df, frame_index, dst, dst_strides, effective_opt);
}

void decode_frame_into(const DicomFile& df, std::size_t frame_index,
    std::span<std::uint8_t> dst, const DecodeStrides& dst_strides, const DecodeOptions& opt) {
	const auto& info = df.pixel_info();
	auto effective_opt = opt;
	effective_opt.scaled = should_use_scaled_output_impl(df, info, opt);

	const auto backend = detail::select_decode_backend(info.ts);
	switch (backend) {
	case detail::decode_backend::raw:
		detail::decode_raw_into(df, info, frame_index, dst, dst_strides, effective_opt);
		return;
	case detail::decode_backend::rle:
		detail::decode_rle_into(df, info, frame_index, dst, dst_strides, effective_opt);
		return;
	case detail::decode_backend::jpeg_family:
		if (info.ts.is_htj2k()) {
			detail::decode_htj2k_into(df, info, frame_index, dst, dst_strides, effective_opt);
			return;
		}
		if (info.ts.is_jpeg2000()) {
			detail::decode_jpeg2k_into(df, info, frame_index, dst, dst_strides, effective_opt);
			return;
		}
		if (info.ts.is_jpegls()) {
			detail::decode_jpegls_into(df, info, frame_index, dst, dst_strides, effective_opt);
			return;
		}
		detail::decode_jpeg_into(df, info, frame_index, dst, dst_strides, effective_opt);
		return;
	default:
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=unsupported transfer syntax {}",
		    df.path(), df.transfer_syntax_uid().value());
		return;
	}
}

} // namespace pixel
} // namespace dicom
