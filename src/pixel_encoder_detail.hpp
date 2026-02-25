#pragma once

#include "dicom.h"
#include "dicom_endian.h"
#include "pixel_codec_registry.hpp"

#include <cstddef>
#include <cstdint>
#include <fmt/format.h>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace dicom::pixel::detail {

template <typename... Args>
[[noreturn]] inline void throw_encode_error(fmt::format_string<Args...> format,
    Args... args) {
	throw std::runtime_error(fmt::vformat(format, fmt::make_format_args(args...)));
}

struct EncapsulatedEncodeInput {
	const std::uint8_t* source_base{nullptr};
	std::size_t frame_count{0};
	std::size_t source_frame_stride{0};
	std::size_t source_frame_payload{0};
	bool source_aliases_current_native_pixel_data{false};
};

// Bundle of arguments passed to encapsulated encode orchestration.
struct CodecEncodeFnInput {
	DicomFile& file;
	uid::WellKnown transfer_syntax{};
	const EncapsulatedEncodeInput& encode_input;
	std::span<const codec_option_kv> codec_options{};
	std::size_t rows{0};
	std::size_t cols{0};
	std::size_t samples_per_pixel{0};
	std::size_t bytes_per_sample{0};
	int bits_allocated{0};
	int bits_stored{0};
	int pixel_representation{0};
	bool use_multicomponent_transform{false};
	Planar source_planar{Planar::interleaved};
	bool planar_source{false};
	std::size_t row_payload_bytes{0};
	std::size_t source_row_stride{0};
	std::size_t source_plane_stride{0};
	std::size_t source_frame_payload{0};
	std::size_t destination_frame_payload{0};
	codec_profile profile{codec_profile::unknown};
	std::string_view plugin_key{};
};

struct SourceFrameLayout {
	bool source_is_planar{false};
	std::size_t row_payload{0};
	std::size_t plane_stride{0};
	std::size_t minimum_frame_bytes{0};
	std::size_t interleaved_pixel_stride{0};
};

[[nodiscard]] inline std::int32_t normalize_signed_sample_bits(
    std::int32_t value, int bits_stored) noexcept {
	if (bits_stored <= 0 || bits_stored >= 32) {
		return value;
	}

	const std::uint32_t mask = (std::uint32_t{1} << bits_stored) - 1u;
	const std::uint32_t sign_bit = std::uint32_t{1} << (bits_stored - 1);
	const std::uint32_t raw = static_cast<std::uint32_t>(value) & mask;
	if ((raw & sign_bit) != 0u) {
		return static_cast<std::int32_t>(raw) -
		    static_cast<std::int32_t>(std::uint32_t{1} << bits_stored);
	}
	return static_cast<std::int32_t>(raw);
}

[[nodiscard]] inline std::int32_t load_i8_or_i16_sample_from_source(
    const std::uint8_t* sample_ptr, std::size_t bytes_per_sample, bool source_signed,
    int bits_stored) {
	switch (bytes_per_sample) {
	case 1:
		return source_signed
		           ? static_cast<std::int32_t>(static_cast<std::int8_t>(*sample_ptr))
		           : static_cast<std::int32_t>(*sample_ptr);
	case 2:
		if (source_signed) {
			return normalize_signed_sample_bits(
			    static_cast<std::int32_t>(endian::load_le<std::int16_t>(sample_ptr)),
			    bits_stored);
		}
		return static_cast<std::int32_t>(
		    static_cast<std::uint16_t>(endian::load_le<std::uint16_t>(sample_ptr)));
	default:
		throw_encode_error(
		    "unsupported bytes_per_sample={} (expected 1 or 2)",
		    bytes_per_sample);
		return 0;
	}
}

[[nodiscard]] inline SourceFrameLayout make_source_frame_layout(std::span<const std::uint8_t> frame_data,
    std::size_t rows, std::size_t cols, std::size_t samples_per_pixel,
    std::size_t bytes_per_sample, Planar source_planar, std::size_t row_stride) {
	if (rows == 0 || cols == 0 || samples_per_pixel == 0 || bytes_per_sample == 0) {
		throw_encode_error(
		    "rows/cols/samples_per_pixel/bytes_per_sample must be positive");
	}
	if (rows > 65535 || cols > 65535) {
		throw_encode_error("rows/cols must be <= 65535 for DICOM pixel data");
	}
	if (samples_per_pixel > 16) {
		throw_encode_error("samples_per_pixel={} is outside practical encoder range",
		    samples_per_pixel);
	}
	if (bytes_per_sample > 8) {
		throw_encode_error("bytes_per_sample={} is outside practical encoder range",
		    bytes_per_sample);
	}
	if (row_stride > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
		throw_encode_error("row_stride={} exceeds 32-bit range", row_stride);
	}

	SourceFrameLayout layout{};
	layout.source_is_planar =
	    source_planar == Planar::planar && samples_per_pixel > std::size_t{1};
	const std::size_t row_components =
	    layout.source_is_planar ? cols : cols * samples_per_pixel;
	layout.row_payload = row_components * bytes_per_sample;
	if (row_stride < layout.row_payload) {
		throw_encode_error("row_stride({}) is smaller than row payload({})",
		    row_stride, layout.row_payload);
	}

	const auto plane_stride_u64 =
	    static_cast<std::uint64_t>(row_stride) * static_cast<std::uint64_t>(rows);
	if (plane_stride_u64 > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
		throw_encode_error("plane stride exceeds size_t range");
	}
	layout.plane_stride = static_cast<std::size_t>(plane_stride_u64);
	if (layout.source_is_planar) {
		const auto minimum_frame_bytes_u64 =
		    static_cast<std::uint64_t>(layout.plane_stride) *
		    static_cast<std::uint64_t>(samples_per_pixel);
		if (minimum_frame_bytes_u64 >
		    static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
			throw_encode_error("minimum frame bytes exceeds size_t range");
		}
		layout.minimum_frame_bytes = static_cast<std::size_t>(minimum_frame_bytes_u64);
	} else {
		layout.minimum_frame_bytes = layout.plane_stride;
	}
	if (frame_data.size() < layout.minimum_frame_bytes) {
		throw_encode_error("frame_data too short (need={}, got={})",
		    layout.minimum_frame_bytes, frame_data.size());
	}
	if (!layout.source_is_planar) {
		layout.interleaved_pixel_stride = samples_per_pixel * bytes_per_sample;
	}

	return layout;
}

bool try_encode_rle_frame(std::span<const std::uint8_t> frame_data,
    std::size_t rows, std::size_t cols, std::size_t samples_per_pixel,
    std::size_t bytes_per_sample, Planar source_planar, std::size_t row_stride,
    std::vector<std::uint8_t>& out_encoded, codec_error& out_error) noexcept;

// Encodes one native frame into JPEG 2000 codestream (J2K) for encapsulated PixelData.
std::vector<std::uint8_t> encode_jpeg2k_frame(std::span<const std::uint8_t> frame_data,
    std::size_t rows, std::size_t cols, std::size_t samples_per_pixel,
    std::size_t bytes_per_sample, int bits_allocated, int bits_stored,
    int pixel_representation, Planar source_planar, std::size_t row_stride,
    bool use_multicomponent_transform, bool lossless,
    const J2kOptions& options);

bool try_encode_jpeg2k_frame(std::span<const std::uint8_t> frame_data,
    std::size_t rows, std::size_t cols, std::size_t samples_per_pixel,
    std::size_t bytes_per_sample, int bits_allocated, int bits_stored,
    int pixel_representation, Planar source_planar, std::size_t row_stride,
    bool use_multicomponent_transform, bool lossless,
    const J2kOptions& options, std::vector<std::uint8_t>& out_encoded,
    codec_error& out_error) noexcept;

// Encodes one native frame into HTJ2K codestream (J2C) for encapsulated PixelData.
std::vector<std::uint8_t> encode_htj2k_frame(std::span<const std::uint8_t> frame_data,
    std::size_t rows, std::size_t cols, std::size_t samples_per_pixel,
    std::size_t bytes_per_sample, int bits_allocated, int bits_stored,
    int pixel_representation, Planar source_planar, std::size_t row_stride,
    bool use_multicomponent_transform, bool lossless, bool rpcl_progression,
    const Htj2kOptions& options);

bool try_encode_htj2k_frame(std::span<const std::uint8_t> frame_data,
    std::size_t rows, std::size_t cols, std::size_t samples_per_pixel,
    std::size_t bytes_per_sample, int bits_allocated, int bits_stored,
    int pixel_representation, Planar source_planar, std::size_t row_stride,
    bool use_multicomponent_transform, bool lossless, bool rpcl_progression,
    const Htj2kOptions& options, std::vector<std::uint8_t>& out_encoded,
    codec_error& out_error) noexcept;

bool try_encode_jpegls_frame(std::span<const std::uint8_t> frame_data,
    std::size_t rows, std::size_t cols, std::size_t samples_per_pixel,
    std::size_t bytes_per_sample, int bits_allocated, int bits_stored, Planar source_planar,
    std::size_t row_stride, int near_lossless_error, std::vector<std::uint8_t>& out_encoded,
    codec_error& out_error) noexcept;

// Encodes one native frame into classic JPEG codestream for encapsulated PixelData.
std::vector<std::uint8_t> encode_jpeg_frame(std::span<const std::uint8_t> frame_data,
    std::size_t rows, std::size_t cols, std::size_t samples_per_pixel,
    std::size_t bytes_per_sample, int bits_allocated, int bits_stored,
    Planar source_planar, std::size_t row_stride, bool lossless,
    const JpegOptions& options);

bool try_encode_jpeg_frame(std::span<const std::uint8_t> frame_data,
    std::size_t rows, std::size_t cols, std::size_t samples_per_pixel,
    std::size_t bytes_per_sample, int bits_allocated, int bits_stored,
    Planar source_planar, std::size_t row_stride, bool lossless,
    const JpegOptions& options, std::vector<std::uint8_t>& out_encoded,
    codec_error& out_error) noexcept;

// Encodes one native frame into JPEG-XL codestream for encapsulated PixelData.
std::vector<std::uint8_t> encode_jpegxl_frame(std::span<const std::uint8_t> frame_data,
    std::size_t rows, std::size_t cols, std::size_t samples_per_pixel,
    std::size_t bytes_per_sample, int bits_allocated, int bits_stored,
    int pixel_representation, Planar source_planar, std::size_t row_stride,
    bool lossless, const JpegXlOptions& options);

bool try_encode_jpegxl_frame(std::span<const std::uint8_t> frame_data,
    std::size_t rows, std::size_t cols, std::size_t samples_per_pixel,
    std::size_t bytes_per_sample, int bits_allocated, int bits_stored,
    int pixel_representation, Planar source_planar, std::size_t row_stride,
    bool lossless, const JpegXlOptions& options, std::vector<std::uint8_t>& out_encoded,
    codec_error& out_error) noexcept;

void encode_encapsulated_pixel_data(const CodecEncodeFnInput& input);

} // namespace dicom::pixel::detail
