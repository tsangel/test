#include "pixel_decoder_detail.hpp"
#include "pixel_codec_plugin_abi_adapter.hpp"
#include "pixel_codec_registry.hpp"

#include "dicom_endian.h"
#include "diagnostics.h"

#include <bit>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <string_view>
#include <type_traits>
#include <vector>

using namespace dicom::literals;
namespace diag = dicom::diag;

namespace dicom {
namespace pixel::detail {

PlanarTransform select_planar_transform(Planar src_planar, Planar dst_planar) noexcept {
	if (src_planar == Planar::interleaved) {
		return (dst_planar == Planar::interleaved)
		           ? PlanarTransform::interleaved_to_interleaved
		           : PlanarTransform::interleaved_to_planar;
	}
	return (dst_planar == Planar::interleaved)
	           ? PlanarTransform::planar_to_interleaved
	           : PlanarTransform::planar_to_planar;
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

EncapsulatedFrameSource load_encapsulated_frame_source(const DataSet& ds,
    std::string_view file_path, std::size_t frame_index, std::string_view codec_name) {
	const auto& pixel_data = ds["PixelData"_tag];
	if (!pixel_data || !pixel_data.vr().is_pixel_sequence()) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason={} requires encapsulated PixelData",
		    file_path, codec_name);
	}

	const auto* pixel_sequence = pixel_data.as_pixel_sequence();
	if (!pixel_sequence) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason={} pixel sequence is missing",
		    file_path, codec_name);
	}

	const auto frame_count = pixel_sequence->number_of_frames();
	if (frame_index >= frame_count) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} frame={} reason={} frame index out of range (frames={})",
		    file_path, frame_index, codec_name, frame_count);
	}

	const auto* frame = pixel_sequence->frame(frame_index);
	if (!frame) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} frame={} reason={} frame is missing",
		    file_path, frame_index, codec_name);
	}

	EncapsulatedFrameSource source{};
	source.frame = frame;
	if (frame->encoded_data_size() != 0) {
		source.contiguous = frame->encoded_data_view();
		source.total_size = source.contiguous.size();
		return source;
	}

	const auto& fragments = frame->fragments();
	if (fragments.empty()) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} frame={} reason={} frame has no fragments",
		    file_path, frame_index, codec_name);
	}
	for (const auto& fragment : fragments) {
		if (fragment.length == 0) {
			diag::error_and_throw(
			    "pixel::decode_frame_into file={} frame={} reason={} zero-length fragment is not supported",
			    file_path, frame_index, codec_name);
		}
	}

	auto* mutable_sequence = const_cast<PixelSequence*>(pixel_sequence);
	source.contiguous = mutable_sequence->frame_encoded_span(frame_index);
	if (!source.contiguous.empty()) {
		source.total_size = source.contiguous.size();
		return source;
	}

	const auto* stream = pixel_sequence->stream();
	if (!stream) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} frame={} reason={} pixel sequence stream is missing",
		    file_path, frame_index, codec_name);
	}
	source.stream = stream;

	if (fragments.size() == 1) {
		const auto& fragment = fragments.front();
		source.contiguous = stream->get_span(fragment.offset, fragment.length);
		source.total_size = source.contiguous.size();
		return source;
	}

	source.fragments = &fragments;
	std::size_t total_size = 0;
	for (const auto& fragment : fragments) {
		if (fragment.length > std::numeric_limits<std::size_t>::max() - total_size) {
			diag::error_and_throw(
			    "pixel::decode_frame_into file={} frame={} reason={} frame size overflow",
			    file_path, frame_index, codec_name);
		}
		total_size += fragment.length;
	}
	source.total_size = total_size;
	return source;
}

namespace {

struct NativeSourceElement {
	const DataElement* element{nullptr};
	std::string_view name{"PixelData"};
};

NativeSourceElement select_native_source_element(
    const DataSet& ds, DataType sv_dtype) {
	switch (sv_dtype) {
	case DataType::f32:
		return NativeSourceElement{&ds["FloatPixelData"_tag], "FloatPixelData"};
	case DataType::f64:
		return NativeSourceElement{&ds["DoubleFloatPixelData"_tag], "DoubleFloatPixelData"};
	default:
		return NativeSourceElement{&ds["PixelData"_tag], "PixelData"};
	}
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

} // namespace

NativeFrameSource load_native_frame_source(
    const DataSet& ds, std::string_view file_path,
    const pixel::PixelDataInfo& info, std::size_t frame_index) {
	const auto sv_dtype = info.sv_dtype;
	const auto source = select_native_source_element(ds, sv_dtype);
	if (!source.element || !(*source.element)) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=missing {}",
		    file_path, source.name);
	}
	if (source.element->vr().is_pixel_sequence()) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=encapsulated {} is not raw",
		    file_path, source.name);
	}

	if (info.rows <= 0 || info.cols <= 0 || info.samples_per_pixel <= 0 ||
	    info.frames <= 0) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=invalid raw pixel metadata",
		    file_path);
	}

	const auto bytes_per_sample = sv_dtype_bytes(info.sv_dtype);
	if (bytes_per_sample == 0) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=unsupported sv_dtype for native raw source",
		    file_path);
	}

	const auto rows = static_cast<std::size_t>(info.rows);
	const auto cols = static_cast<std::size_t>(info.cols);
	const auto samples_per_pixel = static_cast<std::size_t>(info.samples_per_pixel);
	const auto frame_count = static_cast<std::size_t>(info.frames);
	if (frame_index >= frame_count) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} frame={} reason=raw frame index out of range (frames={})",
		    file_path, frame_index, frame_count);
	}

	const auto src_planar = info.planar_configuration;
	const std::size_t src_row_components =
	    (src_planar == Planar::interleaved) ? samples_per_pixel : std::size_t{1};
	std::size_t src_row_pixels = 0;
	std::size_t src_row_bytes = 0;
	if (!checked_mul_size_t(cols, src_row_components, src_row_pixels) ||
	    !checked_mul_size_t(src_row_pixels, bytes_per_sample, src_row_bytes)) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=raw row bytes exceed size_t range",
		    file_path);
	}

	std::size_t src_frame_bytes = 0;
	if (!checked_mul_size_t(src_row_bytes, rows, src_frame_bytes)) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=raw frame bytes exceed size_t range",
		    file_path);
	}
	if (src_planar == Planar::planar &&
	    !checked_mul_size_t(src_frame_bytes, samples_per_pixel, src_frame_bytes)) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=raw planar frame bytes exceed size_t range",
		    file_path);
	}

	NativeFrameSource native_source{};
	native_source.contiguous = source.element->value_span();
	native_source.name = source.name;
	std::size_t src_frame_offset = 0;
	if (!checked_mul_size_t(frame_index, src_frame_bytes, src_frame_offset)) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=raw frame offset exceeds size_t range",
		    file_path);
	}
	if (native_source.contiguous.size() < src_frame_offset ||
	    native_source.contiguous.size() - src_frame_offset < src_frame_bytes) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} frame={} reason={} length is shorter than expected",
		    file_path, frame_index, native_source.name);
	}
	native_source.contiguous =
	    native_source.contiguous.subspan(src_frame_offset, src_frame_bytes);
	return native_source;
}

std::span<const std::uint8_t> materialize_encapsulated_frame_source(
    std::string_view file_path, std::size_t frame_index, std::string_view codec_name,
    const EncapsulatedFrameSource& source, std::vector<std::uint8_t>& out_owned) {
	if (!source.contiguous.empty()) {
		return source.contiguous;
	}
	if (!source.frame || !source.stream || !source.fragments) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} frame={} reason={} frame source is not materializable",
		    file_path, frame_index, codec_name);
	}
	out_owned = source.frame->coalesce_encoded_data(*source.stream);
	return std::span<const std::uint8_t>(out_owned);
}

namespace {

template <typename SampleT>
inline SampleT load_scaled_source_sample(const std::uint8_t* src) noexcept {
	if constexpr (sizeof(SampleT) == 1) {
		SampleT value{};
		std::memcpy(&value, src, sizeof(SampleT));
		return value;
	} else if constexpr (std::is_integral_v<SampleT>) {
		return endian::load_le<SampleT>(src);
	} else if constexpr (std::is_same_v<SampleT, float>) {
		const auto bits = endian::load_le<std::uint32_t>(src);
		return std::bit_cast<float>(bits);
	} else {
		static_assert(std::is_same_v<SampleT, double>, "unsupported scaled sample type");
		const auto bits = endian::load_le<std::uint64_t>(src);
		return std::bit_cast<double>(bits);
	}
}

template <typename SampleT>
void decode_mono_scaled_samples_into(const std::uint8_t* src_frame, std::uint8_t* dst_base,
    std::size_t rows, std::size_t cols, std::size_t src_row_bytes, std::size_t dst_row_bytes,
    double slope, double intercept) {
	for (std::size_t r = 0; r < rows; ++r) {
		const auto* src_sample = src_frame + r * src_row_bytes;
		auto* dst_sample = dst_base + r * dst_row_bytes;
		for (std::size_t c = 0; c < cols; ++c) {
			const auto sv = load_scaled_source_sample<SampleT>(src_sample);
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
    const ModalityLut& lut) {
	const auto last_index = static_cast<std::int64_t>(lut.values.size() - 1);
	for (std::size_t r = 0; r < rows; ++r) {
		const auto* src_sample = src_frame + r * src_row_bytes;
		auto* dst_sample = dst_base + r * dst_row_bytes;
		for (std::size_t c = 0; c < cols; ++c) {
			const auto sv = load_scaled_source_sample<SampleT>(src_sample);
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

template <std::size_t Bytes>
inline void copy_sample(const std::uint8_t* src, std::uint8_t* dst) noexcept {
	std::memcpy(dst, src, Bytes);
}

template <std::size_t Bytes, std::size_t SamplesPerPixel>
void copy_interleaved_to_planar_fast(const std::uint8_t* src_frame, std::uint8_t* dst_base,
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
					copy_sample<2>(src, d0);
					src += 2;
					d0 += 2;

					copy_sample<2>(src, d1);
					src += 2;
					d1 += 2;

					copy_sample<2>(src, d2);
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
					copy_sample<2>(src, d0);
					src += 2;
					d0 += 2;

					copy_sample<2>(src, d1);
					src += 2;
					d1 += 2;

					copy_sample<2>(src, d2);
					src += 2;
					d2 += 2;

					copy_sample<2>(src, d3);
					src += 2;
					d3 += 2;
				}
			}
		}
	}
}

template <std::size_t Bytes, std::size_t SamplesPerPixel>
void copy_planar_to_interleaved_fast(const std::uint8_t* src_frame, std::uint8_t* dst_base,
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
					copy_sample<2>(s0, dst);
					s0 += 2;
					dst += 2;

					copy_sample<2>(s1, dst);
					s1 += 2;
					dst += 2;

					copy_sample<2>(s2, dst);
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
					copy_sample<2>(s0, dst);
					s0 += 2;
					dst += 2;

					copy_sample<2>(s1, dst);
					s1 += 2;
					dst += 2;

					copy_sample<2>(s2, dst);
					s2 += 2;
					dst += 2;

					copy_sample<2>(s3, dst);
					s3 += 2;
					dst += 2;
				}
			}
		}
	}
}

template <std::size_t Bytes>
void copy_interleaved_to_interleaved(const std::uint8_t* src_frame, std::uint8_t* dst_base,
    std::size_t rows, std::size_t cols, std::size_t samples_per_pixel,
    std::size_t src_row_bytes, std::size_t dst_row_bytes) noexcept {
	const std::size_t packed_row_bytes = cols * samples_per_pixel * Bytes;
	for (std::size_t r = 0; r < rows; ++r) {
		const auto* src_row = src_frame + r * src_row_bytes;
		auto* dst_row = dst_base + r * dst_row_bytes;
		std::memcpy(dst_row, src_row, packed_row_bytes);
	}
}

template <std::size_t Bytes>
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
				copy_sample<Bytes>(src_sample, dst0);
				src_sample += Bytes;
				dst0 += Bytes;
				copy_sample<Bytes>(src_sample, dst1);
				src_sample += Bytes;
				dst1 += Bytes;
				copy_sample<Bytes>(src_sample, dst2);
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
				copy_sample<Bytes>(src_sample, dst0);
				src_sample += Bytes;
				dst0 += Bytes;
				copy_sample<Bytes>(src_sample, dst1);
				src_sample += Bytes;
				dst1 += Bytes;
				copy_sample<Bytes>(src_sample, dst2);
				src_sample += Bytes;
				dst2 += Bytes;
				copy_sample<Bytes>(src_sample, dst3);
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

template <std::size_t Bytes>
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
				copy_sample<Bytes>(src0, dst_sample);
				src0 += Bytes;
				dst_sample += Bytes;
				copy_sample<Bytes>(src1, dst_sample);
				src1 += Bytes;
				dst_sample += Bytes;
				copy_sample<Bytes>(src2, dst_sample);
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
				copy_sample<Bytes>(src0, dst_sample);
				src0 += Bytes;
				dst_sample += Bytes;
				copy_sample<Bytes>(src1, dst_sample);
				src1 += Bytes;
				dst_sample += Bytes;
				copy_sample<Bytes>(src2, dst_sample);
				src2 += Bytes;
				dst_sample += Bytes;
				copy_sample<Bytes>(src3, dst_sample);
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

template <std::size_t Bytes>
void copy_planar_to_planar(const std::uint8_t* src_frame, std::uint8_t* dst_base,
    std::size_t rows, std::size_t cols, std::size_t samples_per_pixel,
    std::size_t src_row_bytes, std::size_t dst_row_bytes) noexcept {
	const std::size_t packed_row_bytes = cols * Bytes;
	const std::size_t src_plane_bytes = src_row_bytes * rows;
	const std::size_t dst_plane_bytes = dst_row_bytes * rows;

	for (std::size_t p = 0; p < samples_per_pixel; ++p) {
		const auto* src_plane = src_frame + p * src_plane_bytes;
		auto* dst_plane = dst_base + p * dst_plane_bytes;
		for (std::size_t r = 0; r < rows; ++r) {
			std::memcpy(dst_plane + r * dst_row_bytes, src_plane + r * src_row_bytes, packed_row_bytes);
		}
	}
}

template <std::size_t Bytes>
void dispatch_planar_transform_copy(PlanarTransform transform,
    const std::uint8_t* src_frame, std::uint8_t* dst_base,
    std::size_t rows, std::size_t cols, std::size_t samples_per_pixel,
    std::size_t src_row_bytes, std::size_t dst_row_bytes) noexcept {
	switch (transform) {
	case PlanarTransform::interleaved_to_interleaved:
		copy_interleaved_to_interleaved<Bytes>(
		    src_frame, dst_base, rows, cols, samples_per_pixel, src_row_bytes, dst_row_bytes);
		return;
	case PlanarTransform::interleaved_to_planar:
		copy_interleaved_to_planar<Bytes>(
		    src_frame, dst_base, rows, cols, samples_per_pixel, src_row_bytes, dst_row_bytes);
		return;
	case PlanarTransform::planar_to_interleaved:
		copy_planar_to_interleaved<Bytes>(
		    src_frame, dst_base, rows, cols, samples_per_pixel, src_row_bytes, dst_row_bytes);
		return;
	case PlanarTransform::planar_to_planar:
		copy_planar_to_planar<Bytes>(
		    src_frame, dst_base, rows, cols, samples_per_pixel, src_row_bytes, dst_row_bytes);
		return;
	}
}

void copy_interleaved_to_interleaved_by_bytes(std::size_t bytes_per_sample,
    const std::uint8_t* src_frame, std::uint8_t* dst_base,
    std::size_t rows, std::size_t cols, std::size_t samples_per_pixel,
    std::size_t src_row_bytes, std::size_t dst_row_bytes) {
	switch (bytes_per_sample) {
	case 1:
		copy_interleaved_to_interleaved<1>(
		    src_frame, dst_base, rows, cols, samples_per_pixel, src_row_bytes, dst_row_bytes);
		return;
	case 2:
		copy_interleaved_to_interleaved<2>(
		    src_frame, dst_base, rows, cols, samples_per_pixel, src_row_bytes, dst_row_bytes);
		return;
	case 4:
		copy_interleaved_to_interleaved<4>(
		    src_frame, dst_base, rows, cols, samples_per_pixel, src_row_bytes, dst_row_bytes);
		return;
	case 8:
		copy_interleaved_to_interleaved<8>(
		    src_frame, dst_base, rows, cols, samples_per_pixel, src_row_bytes, dst_row_bytes);
		return;
	default:
		diag::error_and_throw(
		    "pixel::decode_frame_into reason=unsupported bytes_per_sample={}", bytes_per_sample);
		return;
	}
}

void dispatch_planar_transform_copy_by_bytes(PlanarTransform transform, std::size_t bytes_per_sample,
    const std::uint8_t* src_frame, std::uint8_t* dst_base,
    std::size_t rows, std::size_t cols, std::size_t samples_per_pixel,
    std::size_t src_row_bytes, std::size_t dst_row_bytes) {
	switch (bytes_per_sample) {
	case 1:
		dispatch_planar_transform_copy<1>(
		    transform, src_frame, dst_base, rows, cols, samples_per_pixel, src_row_bytes, dst_row_bytes);
		return;
	case 2:
		dispatch_planar_transform_copy<2>(
		    transform, src_frame, dst_base, rows, cols, samples_per_pixel, src_row_bytes, dst_row_bytes);
		return;
	case 4:
		dispatch_planar_transform_copy<4>(
		    transform, src_frame, dst_base, rows, cols, samples_per_pixel, src_row_bytes, dst_row_bytes);
		return;
	case 8:
		dispatch_planar_transform_copy<8>(
		    transform, src_frame, dst_base, rows, cols, samples_per_pixel, src_row_bytes, dst_row_bytes);
		return;
	default:
		diag::error_and_throw(
		    "pixel::decode_frame_into reason=unsupported bytes_per_sample={}", bytes_per_sample);
		return;
	}
}

} // namespace

void decode_mono_scaled_into_f32(const DecodeValueTransform& value_transform,
    const pixel::PixelDataInfo& info,
    const std::uint8_t* src_frame, std::span<std::uint8_t> dst,
    const DecodeStrides& dst_strides, std::size_t rows, std::size_t cols,
    std::size_t src_row_bytes) {
	if (info.samples_per_pixel != 1) {
		diag::error_and_throw(
		    "pixel::decode_frame_into reason=scaled output supports SamplesPerPixel=1 only");
	}

	if (!value_transform.enabled) {
		diag::error_and_throw(
		    "pixel::decode_frame_into reason=scaled output requested without value transform metadata");
	}

	if (value_transform.modality_lut) {
		const auto& modality_lut = *value_transform.modality_lut;
		switch (info.sv_dtype) {
			case DataType::u8:
				decode_mono_lut_samples_into<std::uint8_t>(
				    src_frame, dst.data(), rows, cols, src_row_bytes, dst_strides.row,
				    modality_lut);
				return;
			case DataType::s8:
				decode_mono_lut_samples_into<std::int8_t>(
				    src_frame, dst.data(), rows, cols, src_row_bytes, dst_strides.row,
				    modality_lut);
				return;
			case DataType::u16:
				decode_mono_lut_samples_into<std::uint16_t>(
				    src_frame, dst.data(), rows, cols, src_row_bytes, dst_strides.row,
				    modality_lut);
				return;
			case DataType::s16:
				decode_mono_lut_samples_into<std::int16_t>(
				    src_frame, dst.data(), rows, cols, src_row_bytes, dst_strides.row,
				    modality_lut);
				return;
			case DataType::u32:
				decode_mono_lut_samples_into<std::uint32_t>(
				    src_frame, dst.data(), rows, cols, src_row_bytes, dst_strides.row,
				    modality_lut);
				return;
			case DataType::s32:
				decode_mono_lut_samples_into<std::int32_t>(
				    src_frame, dst.data(), rows, cols, src_row_bytes, dst_strides.row,
				    modality_lut);
				return;
		default:
			diag::error_and_throw(
			    "pixel::decode_frame_into reason=Modality LUT path supports integral sv_dtype only");
			return;
		}
	}

	const auto slope = value_transform.rescale_slope;
	const auto intercept = value_transform.rescale_intercept;
	if (!std::isfinite(slope) || !std::isfinite(intercept)) {
		diag::error_and_throw(
		    "pixel::decode_frame_into reason=RescaleSlope/RescaleIntercept must be finite");
	}

	switch (info.sv_dtype) {
	case DataType::u8:
		decode_mono_scaled_samples_into<std::uint8_t>(
		    src_frame, dst.data(), rows, cols, src_row_bytes, dst_strides.row,
		    slope, intercept);
		return;
	case DataType::s8:
		decode_mono_scaled_samples_into<std::int8_t>(
		    src_frame, dst.data(), rows, cols, src_row_bytes, dst_strides.row,
		    slope, intercept);
		return;
	case DataType::u16:
		decode_mono_scaled_samples_into<std::uint16_t>(
		    src_frame, dst.data(), rows, cols, src_row_bytes, dst_strides.row,
		    slope, intercept);
		return;
	case DataType::s16:
		decode_mono_scaled_samples_into<std::int16_t>(
		    src_frame, dst.data(), rows, cols, src_row_bytes, dst_strides.row,
		    slope, intercept);
		return;
	case DataType::u32:
		decode_mono_scaled_samples_into<std::uint32_t>(
		    src_frame, dst.data(), rows, cols, src_row_bytes, dst_strides.row,
		    slope, intercept);
		return;
	case DataType::s32:
		decode_mono_scaled_samples_into<std::int32_t>(
		    src_frame, dst.data(), rows, cols, src_row_bytes, dst_strides.row,
		    slope, intercept);
		return;
	case DataType::f32:
		decode_mono_scaled_samples_into<float>(
		    src_frame, dst.data(), rows, cols, src_row_bytes, dst_strides.row,
		    slope, intercept);
		return;
	case DataType::f64:
		decode_mono_scaled_samples_into<double>(
		    src_frame, dst.data(), rows, cols, src_row_bytes, dst_strides.row,
		    slope, intercept);
		return;
	default:
		diag::error_and_throw(
		    "pixel::decode_frame_into reason=scaled output does not support sv_dtype={}",
		    static_cast<int>(info.sv_dtype));
		return;
	}
}

void run_planar_transform_copy(PlanarTransform transform, std::size_t bytes_per_sample,
    const std::uint8_t* src_frame, std::uint8_t* dst_base,
    std::size_t rows, std::size_t cols, std::size_t samples_per_pixel,
    std::size_t src_row_bytes, std::size_t dst_row_bytes) {
	// For single-channel data, Planar/interleaved layouts are equivalent.
	// Route through ii kernel and avoid redundant spp=1 branches in ip/pi kernels.
	if (samples_per_pixel == 1) {
		copy_interleaved_to_interleaved_by_bytes(
		    bytes_per_sample, src_frame, dst_base, rows, cols, samples_per_pixel, src_row_bytes, dst_row_bytes);
		return;
	}

	if ((bytes_per_sample == 1 || bytes_per_sample == 2) &&
	    (samples_per_pixel == 3 || samples_per_pixel == 4)) {
		if (transform == PlanarTransform::interleaved_to_planar) {
			if (bytes_per_sample == 1) {
				if (samples_per_pixel == 3) {
						copy_interleaved_to_planar_fast<1, 3>(
					    src_frame, dst_base, rows, cols, src_row_bytes, dst_row_bytes);
				} else {
						copy_interleaved_to_planar_fast<1, 4>(
					    src_frame, dst_base, rows, cols, src_row_bytes, dst_row_bytes);
				}
			} else {
				if (samples_per_pixel == 3) {
						copy_interleaved_to_planar_fast<2, 3>(
					    src_frame, dst_base, rows, cols, src_row_bytes, dst_row_bytes);
				} else {
						copy_interleaved_to_planar_fast<2, 4>(
					    src_frame, dst_base, rows, cols, src_row_bytes, dst_row_bytes);
				}
			}
			return;
		}
		if (transform == PlanarTransform::planar_to_interleaved) {
			if (bytes_per_sample == 1) {
				if (samples_per_pixel == 3) {
						copy_planar_to_interleaved_fast<1, 3>(
					    src_frame, dst_base, rows, cols, src_row_bytes, dst_row_bytes);
				} else {
						copy_planar_to_interleaved_fast<1, 4>(
					    src_frame, dst_base, rows, cols, src_row_bytes, dst_row_bytes);
				}
			} else {
				if (samples_per_pixel == 3) {
						copy_planar_to_interleaved_fast<2, 3>(
					    src_frame, dst_base, rows, cols, src_row_bytes, dst_row_bytes);
				} else {
						copy_planar_to_interleaved_fast<2, 4>(
					    src_frame, dst_base, rows, cols, src_row_bytes, dst_row_bytes);
				}
			}
			return;
		}
	}

	dispatch_planar_transform_copy_by_bytes(
	    transform, bytes_per_sample, src_frame, dst_base, rows, cols, samples_per_pixel, src_row_bytes, dst_row_bytes);
}

} // namespace pixel::detail

namespace pixel {

namespace {

bool has_rescale_transform_metadata(const DataSet& ds) {
	return static_cast<bool>(ds["RescaleSlope"_tag]) ||
	    static_cast<bool>(ds["RescaleIntercept"_tag]);
}

bool has_modality_lut_metadata(const DataSet& ds) {
	return static_cast<bool>(ds["ModalityLUTSequence"_tag]);
}

std::string decorate_decode_detail_with_callsite_context(
    std::string detail, std::string_view file_path, std::size_t frame_index) {
	// Legacy decoder helpers often return fully formatted strings like:
	// "pixel::decode_frame_into file=... frame=... reason=..."
	// Strip only that prefix form; keep codec-native detail intact.
	if (detail.rfind("pixel::decode_frame_into ", 0) == 0) {
		const auto reason_pos = detail.find("reason=");
		if (reason_pos != std::string::npos) {
			detail = detail.substr(reason_pos + 7);
		}
	}
	while (!detail.empty() &&
	       std::isspace(static_cast<unsigned char>(detail.front())) != 0) {
		detail.erase(detail.begin());
	}
	if (detail.empty()) {
		detail = "decoder plugin failed";
	}
	return "file=" + std::string(file_path) + " frame=" +
	    std::to_string(frame_index) + " " + detail;
}

bool can_apply_scaled_output(
    const pixel::PixelDataInfo& info, const DecodeOptions& opt) {
	if (!opt.scaled) {
		return false;
	}
	if (!info.has_pixel_data) {
		return false;
	}
	return info.samples_per_pixel == 1;
}

std::optional<pixel::ModalityLut> load_modality_lut_for_scaled_output(
    const DicomFile& df, const DataSet& ds, const pixel::PixelDataInfo& info,
    const DecodeOptions& opt) {
	if (!can_apply_scaled_output(info, opt)) {
		return std::nullopt;
	}
	if (!has_modality_lut_metadata(ds)) {
		return std::nullopt;
	}
	// Validate and load LUT eagerly so malformed metadata still fails loudly.
	return df.modality_lut();
}

detail::DecodeValueTransform prepare_decode_value_transform(
    const DataSet& ds, const pixel::PixelDataInfo& info, const DecodeOptions& opt,
    std::optional<pixel::ModalityLut> modality_lut) {
	detail::DecodeValueTransform value_transform{};
	if (!can_apply_scaled_output(info, opt)) {
		return value_transform;
	}

	if (has_modality_lut_metadata(ds)) {
		value_transform.modality_lut = std::move(modality_lut);
		value_transform.enabled = true;
		return value_transform;
	}

	if (has_rescale_transform_metadata(ds)) {
		value_transform.rescale_slope =
		    ds["RescaleSlope"_tag].to_double().value_or(1.0);
		value_transform.rescale_intercept =
		    ds["RescaleIntercept"_tag].to_double().value_or(0.0);
		value_transform.enabled = true;
	}
	return value_transform;
}

void decode_frame_into_with_prepared_transform(const DicomFile& df, const DataSet& ds,
    const pixel::PixelDataInfo& info, const detail::DecodeValueTransform& value_transform,
    std::size_t frame_index, std::span<std::uint8_t> dst,
    const DecodeStrides& dst_strides, const DecodeOptions& effective_opt) {
	const auto& CodecRegistry = detail::global_codec_registry();
	const auto* binding = CodecRegistry.find_binding(info.ts);
	detail::CodecError decode_error{};
	if (!binding || !binding->decode_supported) {
		decode_error.code = detail::CodecStatusCode::unsupported;
		decode_error.stage = "plugin_lookup";
		decode_error.detail =
		    "transfer syntax is not supported for decode by codec registry binding";
		detail::throw_codec_error_with_context("pixel::decode_frame_into",
		    df.path(), info.ts, "<none>", frame_index, decode_error);
	}
	const auto* plugin = CodecRegistry.select_decoder(*binding);
	if (!plugin) {
		decode_error.code = detail::CodecStatusCode::internal_error;
		decode_error.stage = "plugin_lookup";
		decode_error.detail = "registry binding references a missing plugin";
		detail::throw_codec_error_with_context("pixel::decode_frame_into",
		    df.path(), info.ts, binding->plugin_key, frame_index, decode_error);
	}
	if (!plugin->decode_frame) {
		decode_error.code = detail::CodecStatusCode::internal_error;
		decode_error.stage = "plugin_lookup";
		decode_error.detail = "registered decode plugin has no dispatcher";
		detail::throw_codec_error_with_context("pixel::decode_frame_into",
		    df.path(), info.ts, binding->plugin_key, frame_index, decode_error);
	}
	std::span<const std::uint8_t> prepared_source{};
	std::vector<std::uint8_t> prepared_source_owned{};
	try {
		switch (binding->profile) {
		case detail::CodecProfile::native_uncompressed: {
			const auto native_source = detail::load_native_frame_source(
			    ds, df.path(), info, frame_index);
			prepared_source = native_source.contiguous;
			break;
		}
		case detail::CodecProfile::encapsulated_uncompressed:
		case detail::CodecProfile::rle_lossless:
		case detail::CodecProfile::jpeg_lossless:
		case detail::CodecProfile::jpeg_lossy:
		case detail::CodecProfile::jpegls_lossless:
		case detail::CodecProfile::jpegls_near_lossless:
		case detail::CodecProfile::jpeg2000_lossless:
		case detail::CodecProfile::jpeg2000_lossy:
		case detail::CodecProfile::htj2k_lossless:
		case detail::CodecProfile::htj2k_lossless_rpcl:
		case detail::CodecProfile::htj2k_lossy:
		case detail::CodecProfile::jpegxl_lossless:
		case detail::CodecProfile::jpegxl_lossy:
		case detail::CodecProfile::jpegxl_jpeg_recompression: {
			const auto source = detail::load_encapsulated_frame_source(
			    ds, df.path(), frame_index, binding->plugin_key);
			prepared_source = detail::materialize_encapsulated_frame_source(
			    df.path(), frame_index, binding->plugin_key, source,
			    prepared_source_owned);
			break;
		}
		case detail::CodecProfile::unknown:
		default:
			break;
		}
	} catch (const std::bad_alloc&) {
		decode_error.code = detail::CodecStatusCode::internal_error;
		decode_error.stage = "allocate";
		decode_error.detail = "memory allocation failed";
		detail::throw_codec_error_with_context("pixel::decode_frame_into",
		    df.path(), info.ts, binding->plugin_key, frame_index, decode_error);
	} catch (const std::exception& e) {
		decode_error.code = detail::CodecStatusCode::invalid_argument;
		decode_error.stage = "load_frame_source";
		decode_error.detail = e.what();
		detail::throw_codec_error_with_context("pixel::decode_frame_into",
		    df.path(), info.ts, binding->plugin_key, frame_index, decode_error);
	} catch (...) {
		decode_error.code = detail::CodecStatusCode::backend_error;
		decode_error.stage = "load_frame_source";
		decode_error.detail = "non-standard exception";
		detail::throw_codec_error_with_context("pixel::decode_frame_into",
		    df.path(), info.ts, binding->plugin_key, frame_index, decode_error);
	}
	const detail::CodecDecodeFrameInput decode_input{
	    .info = info,
	    .value_transform = value_transform,
	    .prepared_source = prepared_source,
	    .destination = dst,
	    .destination_strides = dst_strides,
	    .options = effective_opt,
	};
	dicomsdl_decoder_request_v1 abi_request{};
	detail::abi::build_decoder_request_v1(decode_input, abi_request);
	if (abi_request.frame.transfer_syntax_code ==
	    DICOMSDL_TRANSFER_SYNTAX_CODE_INVALID) {
		decode_error.code = detail::CodecStatusCode::invalid_argument;
		decode_error.stage = "validate";
		decode_error.detail = "invalid transfer syntax code for decoder ABI request";
		detail::throw_codec_error_with_context("pixel::decode_frame_into",
		    df.path(), info.ts, binding->plugin_key, frame_index, decode_error);
	}
	if (abi_request.frame.source_dtype == DICOMSDL_DTYPE_UNKNOWN &&
	    info.sv_dtype != DataType::unknown) {
		decode_error.code = detail::CodecStatusCode::invalid_argument;
		decode_error.stage = "validate";
		decode_error.detail = "source dtype is not representable in decoder ABI request";
		detail::throw_codec_error_with_context("pixel::decode_frame_into",
		    df.path(), info.ts, binding->plugin_key, frame_index, decode_error);
	}
	const bool decode_ok = plugin->decode_frame(decode_input, decode_error);

	if (!decode_ok) {
		if (decode_error.code == detail::CodecStatusCode::ok) {
			decode_error.code = detail::CodecStatusCode::backend_error;
		}
		if (decode_error.stage.empty()) {
			decode_error.stage = "decode_frame";
		}
		if (decode_error.detail.empty()) {
			decode_error.detail = "decoder plugin failed";
		}
		decode_error.detail = decorate_decode_detail_with_callsite_context(
		    std::move(decode_error.detail), df.path(), frame_index);
		detail::throw_codec_error_with_context("pixel::decode_frame_into",
		    df.path(), info.ts, binding->plugin_key, frame_index, decode_error);
	}
}

} // namespace

bool should_use_scaled_output(const DicomFile& df, const DecodeOptions& opt) {
	const auto& info = df.pixeldata_info();
	const auto& ds = df.dataset();
	auto modality_lut = load_modality_lut_for_scaled_output(df, ds, info, opt);
	return prepare_decode_value_transform(ds, info, opt, std::move(modality_lut)).enabled;
}

void decode_frame_into(const DicomFile& df, std::size_t frame_index,
    std::span<std::uint8_t> dst, const DecodeOptions& opt) {
	const auto& info = df.pixeldata_info();
	const auto& ds = df.dataset();
	auto modality_lut = load_modality_lut_for_scaled_output(df, ds, info, opt);
	const auto value_transform = prepare_decode_value_transform(
	    ds, info, opt, std::move(modality_lut));
	auto effective_opt = opt;
	effective_opt.scaled = value_transform.enabled;

	const auto dst_strides = df.calc_decode_strides(effective_opt);
	decode_frame_into_with_prepared_transform(
	    df, ds, info, value_transform, frame_index, dst, dst_strides, effective_opt);
}

void decode_frame_into(const DicomFile& df, std::size_t frame_index,
    std::span<std::uint8_t> dst, const DecodeStrides& dst_strides, const DecodeOptions& opt) {
	const auto& info = df.pixeldata_info();
	const auto& ds = df.dataset();
	auto modality_lut = load_modality_lut_for_scaled_output(df, ds, info, opt);
	const auto value_transform = prepare_decode_value_transform(
	    ds, info, opt, std::move(modality_lut));
	auto effective_opt = opt;
	effective_opt.scaled = value_transform.enabled;
	decode_frame_into_with_prepared_transform(
	    df, ds, info, value_transform, frame_index, dst, dst_strides, effective_opt);
}

} // namespace pixel
} // namespace dicom
