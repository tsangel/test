#include "pixel/host/support/dicom_pixel_support.hpp"

#include "pixel/host/error/codec_error.hpp"
#include "photometric_text_detail.hpp"
#include "diagnostics.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace diag = dicom::diag;
namespace codec_detail = dicom::pixel::detail;

namespace dicom::pixel::support_detail {
using namespace dicom::literals;

namespace {

[[nodiscard]] std::optional<pixel::Photometric> parse_photometric_from_text(
    std::string_view text) noexcept {
	return pixel::detail::parse_photometric_text(text);
}

struct EncapsulatedFrameSource {
	const PixelFrame* frame{nullptr};
	const InStream* stream{nullptr};
	std::span<const std::uint8_t> contiguous{};
	const std::vector<PixelFragment>* fragments{nullptr};
	std::size_t total_size{0};
};

struct NativeSourceLayout {
	int bits_allocated{0};
	int pixel_representation{0};
};

struct NativeSourceElement {
	const DataElement* element{nullptr};
	std::string_view name{"PixelData"};
};

[[noreturn]] void throw_decode_frame_source_error(std::string_view reason) {
	throw std::runtime_error(std::string(reason));
}

[[nodiscard]] const std::array<std::array<std::uint8_t, 8>, 256>&
one_bit_expand_lut() {
	static const auto table = [] {
		std::array<std::array<std::uint8_t, 8>, 256> out{};
		for (std::size_t byte = 0; byte < out.size(); ++byte) {
			for (std::size_t bit = 0; bit < out[byte].size(); ++bit) {
				out[byte][bit] =
				    static_cast<std::uint8_t>((byte >> bit) & std::size_t{1});
			}
		}
		return out;
	}();
	return table;
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

[[nodiscard]] bool checked_add_size_t(
    std::size_t lhs, std::size_t rhs, std::size_t& out) noexcept {
	if (lhs > std::numeric_limits<std::size_t>::max() - rhs) {
		return false;
	}
	out = lhs + rhs;
	return true;
}

[[nodiscard]] std::optional<std::size_t> positive_size_value(
    const DataSet& ds, Tag tag) {
	const auto value = ds[tag].to_long();
	if (!value.has_value() || *value <= 0) {
		return std::nullopt;
	}
	const auto as_unsigned = static_cast<unsigned long long>(*value);
	if (as_unsigned > static_cast<unsigned long long>(
	                      std::numeric_limits<std::size_t>::max())) {
		return std::nullopt;
	}
	return static_cast<std::size_t>(as_unsigned);
}

[[nodiscard]] std::optional<NativeOneBitPixelLayout>
compute_native_one_bit_pixel_layout_from_dataset(const DataSet& dataset) {
	const auto bits_allocated = dataset["BitsAllocated"_tag].to_long();
	if (!bits_allocated.has_value() || *bits_allocated != 1) {
		return std::nullopt;
	}
	const auto pixel_representation =
	    dataset["PixelRepresentation"_tag].to_long().value_or(0);
	if (pixel_representation != 0) {
		return std::nullopt;
	}
	const auto bits_stored = dataset["BitsStored"_tag].to_long().value_or(1);
	if (bits_stored != 1) {
		return std::nullopt;
	}

	const auto rows = positive_size_value(dataset, "Rows"_tag);
	const auto cols = positive_size_value(dataset, "Columns"_tag);
	if (!rows.has_value() || !cols.has_value()) {
		return std::nullopt;
	}
	if (*rows > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) ||
	    *cols > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
		return std::nullopt;
	}

	std::size_t samples_per_pixel = 1;
	if (const auto samples_value = dataset["SamplesPerPixel"_tag].to_long();
	    samples_value.has_value()) {
		if (*samples_value <= 0 ||
		    static_cast<unsigned long long>(*samples_value) >
		        static_cast<unsigned long long>(
		            std::numeric_limits<std::uint16_t>::max())) {
			return std::nullopt;
		}
		samples_per_pixel = static_cast<std::size_t>(*samples_value);
	}

	std::size_t frames = 1;
	if (const auto frames_value = dataset["NumberOfFrames"_tag].to_long();
	    frames_value.has_value()) {
		if (*frames_value <= 0 ||
		    static_cast<unsigned long long>(*frames_value) >
		        static_cast<unsigned long long>(
		            std::numeric_limits<std::uint32_t>::max())) {
			return std::nullopt;
		}
		frames = static_cast<std::size_t>(*frames_value);
	}
	if (samples_per_pixel == 0 || frames == 0) {
		return std::nullopt;
	}

	const auto stored_planar =
	    (dataset["PlanarConfiguration"_tag].to_long().value_or(0) == 1)
	        ? pixel::Planar::planar
	        : pixel::Planar::interleaved;
	const auto photometric = [&]() -> pixel::Photometric {
		if (const auto photometric_text =
		        dataset["PhotometricInterpretation"_tag].to_string_view();
		    photometric_text.has_value()) {
			if (const auto parsed = parse_photometric_from_text(*photometric_text);
			    parsed.has_value()) {
				return *parsed;
			}
		}
		return samples_per_pixel > 1 ? pixel::Photometric::rgb
		                             : pixel::Photometric::monochrome2;
	}();

	std::size_t pixels_per_plane = 0;
	if (!checked_mul_size_t(*rows, *cols, pixels_per_plane)) {
		return std::nullopt;
	}
	std::size_t frame_bits = 0;
	if (!checked_mul_size_t(pixels_per_plane, samples_per_pixel, frame_bits)) {
		return std::nullopt;
	}
	std::size_t total_bits = 0;
	if (!checked_mul_size_t(frame_bits, frames, total_bits)) {
		return std::nullopt;
	}
	std::size_t total_bits_plus_rounding = 0;
	if (!checked_add_size_t(total_bits, std::size_t{7}, total_bits_plus_rounding)) {
		return std::nullopt;
	}

	return NativeOneBitPixelLayout{
	    .photometric = photometric,
	    .planar = stored_planar,
	    .rows = *rows,
	    .cols = *cols,
	    .frames = frames,
	    .samples_per_pixel = samples_per_pixel,
	    .frame_bits = frame_bits,
	    .total_bits = total_bits,
	    .required_payload_bytes = total_bits_plus_rounding / std::size_t{8},
	};
}

[[nodiscard]] std::optional<NativeSourceLayout> native_source_layout_of(
    pixel::DataType data_type) noexcept {
	switch (data_type) {
	case pixel::DataType::u8:
		return NativeSourceLayout{8, 0};
	case pixel::DataType::s8:
		return NativeSourceLayout{8, 1};
	case pixel::DataType::u16:
		return NativeSourceLayout{16, 0};
	case pixel::DataType::s16:
		return NativeSourceLayout{16, 1};
	case pixel::DataType::u32:
		return NativeSourceLayout{32, 0};
	case pixel::DataType::s32:
		return NativeSourceLayout{32, 1};
	default:
		return std::nullopt;
	}
}

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

EncapsulatedFrameSource load_encapsulated_frame_source_or_throw(
    const PixelSequence& pixel_sequence, std::size_t frame_index,
    bool allow_sequence_cache) {
	const auto frame_count = pixel_sequence.number_of_frames();
	if (frame_index >= frame_count) {
		throw_decode_frame_source_error(
		    "frame index out of range (frames=" + std::to_string(frame_count) + ")");
	}

	const auto* frame = pixel_sequence.frame(frame_index);
	if (!frame) {
		throw_decode_frame_source_error("encoded frame is missing");
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
		throw_decode_frame_source_error("encoded frame has no fragments");
	}
	for (const auto& fragment : fragments) {
		if (fragment.length == 0) {
			throw_decode_frame_source_error("zero-length fragment is not supported");
		}
	}

	if (allow_sequence_cache) {
		auto* mutable_sequence = const_cast<PixelSequence*>(&pixel_sequence);
		source.contiguous = mutable_sequence->frame_encoded_span(frame_index);
		if (!source.contiguous.empty()) {
			source.total_size = source.contiguous.size();
			return source;
		}
	}

	const auto* stream = pixel_sequence.stream();
	if (!stream) {
		throw_decode_frame_source_error("pixel sequence stream is missing");
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
			throw_decode_frame_source_error("encoded frame size overflow");
		}
		total_size += fragment.length;
	}
	source.total_size = total_size;
	return source;
}

EncapsulatedFrameSource load_encapsulated_frame_source_or_throw(
    const DataSet& ds, std::size_t frame_index) {
	const auto& pixel_data = ds["PixelData"_tag];
	if (!pixel_data || !pixel_data.vr().is_pixel_sequence()) {
		throw_decode_frame_source_error("encapsulated decode requires PixelData sequence");
	}

	const auto* pixel_sequence = pixel_data.as_pixel_sequence();
	if (!pixel_sequence) {
		throw_decode_frame_source_error("PixelData sequence is missing");
	}

	return load_encapsulated_frame_source_or_throw(
	    *pixel_sequence, frame_index, true);
}

EncapsulatedFrameSource load_encapsulated_frame_source_without_cache_or_throw(
    const DataSet& ds, std::size_t frame_index) {
	const auto& pixel_data = ds["PixelData"_tag];
	if (!pixel_data || !pixel_data.vr().is_pixel_sequence()) {
		throw_decode_frame_source_error("encapsulated decode requires PixelData sequence");
	}

	const auto* pixel_sequence = pixel_data.as_pixel_sequence();
	if (!pixel_sequence) {
		throw_decode_frame_source_error("PixelData sequence is missing");
	}

	return load_encapsulated_frame_source_or_throw(
	    *pixel_sequence, frame_index, false);
}

NativeDecodeSourceView build_native_decode_source_view_or_throw(
    const DataSet& ds, const pixel::PixelLayout& source_layout) {
	const auto source = select_native_source_element(ds, source_layout.data_type);
	if (!source.element || !(*source.element)) {
		throw_decode_frame_source_error("missing " + std::string(source.name));
	}
	if (source.element->vr().is_pixel_sequence()) {
		throw_decode_frame_source_error(
		    std::string(source.name) + " is encapsulated, not raw");
	}
	if (dicom::detail::is_detached_pixel_payload_marker(*source.element)) {
		throw_decode_frame_source_error(
		    std::string(source.name) + " payload is detached");
	}

	if (source_layout.empty()) {
		throw_decode_frame_source_error("invalid raw pixel metadata");
	}

	const auto bytes_per_sample = source_layout.bytes_per_sample();
	if (bytes_per_sample == 0) {
		throw_decode_frame_source_error("unsupported source dtype for raw decode");
	}

	const auto rows = static_cast<std::size_t>(source_layout.rows);
	const auto cols = static_cast<std::size_t>(source_layout.cols);
	const auto samples_per_pixel = static_cast<std::size_t>(source_layout.samples_per_pixel);
	const auto frames = static_cast<std::size_t>(source_layout.frames);
	const bool planar_source =
	    source_layout.planar == Planar::planar && samples_per_pixel > std::size_t{1};
	const std::size_t row_components =
	    planar_source ? cols : cols * samples_per_pixel;
	std::size_t row_payload_bytes = 0;
	if (!checked_mul_size_t(row_components, bytes_per_sample, row_payload_bytes)) {
		throw_decode_frame_source_error("raw row bytes exceed size_t range");
	}

	std::size_t frame_bytes = 0;
	if (!checked_mul_size_t(row_payload_bytes, rows, frame_bytes)) {
		throw_decode_frame_source_error("raw frame bytes exceed size_t range");
	}
	if (planar_source &&
	    !checked_mul_size_t(frame_bytes, samples_per_pixel, frame_bytes)) {
		throw_decode_frame_source_error("raw planar frame bytes exceed size_t range");
	}

	std::size_t total_bytes = 0;
	if (!checked_mul_size_t(frame_bytes, frames, total_bytes)) {
		throw_decode_frame_source_error("raw total frame bytes exceed size_t range");
	}

	NativeDecodeSourceView native_source{};
	native_source.source_bytes = source.element->value_span();
	native_source.source_name = source.name;
	native_source.rows = rows;
	native_source.cols = cols;
	native_source.frames = frames;
	native_source.samples_per_pixel = samples_per_pixel;
	native_source.planar_source = planar_source;
	native_source.bytes_per_sample = bytes_per_sample;
	native_source.row_payload_bytes = row_payload_bytes;
	native_source.frame_bytes = frame_bytes;
	if (native_source.source_bytes.size() < total_bytes) {
		throw_decode_frame_source_error(
		    std::string(native_source.source_name) + " is shorter than expected");
	}
	return native_source;
}

std::span<const std::uint8_t> materialize_encapsulated_frame_source_or_throw(
    const EncapsulatedFrameSource& source, std::vector<std::uint8_t>& out_owned) {
	if (!source.contiguous.empty()) {
		return source.contiguous;
	}
	if (!source.frame || !source.stream || !source.fragments) {
		throw_decode_frame_source_error("encapsulated frame source is not materializable");
	}
	if (source.total_size == 0) {
		throw_decode_frame_source_error("encapsulated frame has zero total size");
	}

	out_owned.clear();
	out_owned.resize(source.total_size);

	std::size_t offset = 0;
	for (const auto& fragment : *source.fragments) {
		const auto fragment_bytes = source.stream->get_span(fragment.offset, fragment.length);
		if (fragment_bytes.size() != fragment.length) {
			throw_decode_frame_source_error("fragment stream view is shorter than expected");
		}
		std::memcpy(out_owned.data() + offset, fragment_bytes.data(), fragment.length);
		offset += fragment.length;
	}
	return std::span<const std::uint8_t>(out_owned.data(), out_owned.size());
}

[[nodiscard]] bool spans_overlap(std::span<const std::uint8_t> lhs,
    std::span<const std::uint8_t> rhs) noexcept {
	if (lhs.empty() || rhs.empty()) {
		return false;
	}

	const auto lhs_begin = reinterpret_cast<std::uintptr_t>(lhs.data());
	const auto rhs_begin = reinterpret_cast<std::uintptr_t>(rhs.data());
	const auto lhs_end = lhs_begin + lhs.size();
	const auto rhs_end = rhs_begin + rhs.size();
	if (lhs_end < lhs_begin || rhs_end < rhs_begin) {
		return false;
	}
	return lhs_begin < rhs_end && rhs_begin < lhs_end;
}

} // namespace

pixel::PixelLayout NativeOneBitPixelLayout::decoded_source_layout() const {
	pixel::PixelLayout layout{
	    .data_type = pixel::DataType::u8,
	    .photometric = photometric,
	    .planar = planar,
	    .reserved = 0,
	    .rows = static_cast<std::uint32_t>(rows),
	    .cols = static_cast<std::uint32_t>(cols),
	    .frames = static_cast<std::uint32_t>(frames),
	    .samples_per_pixel = static_cast<std::uint16_t>(samples_per_pixel),
	    .bits_stored = 1,
	    .row_stride = 0,
	    .frame_stride = 0,
	};
	return layout.packed(1);
}

pixel::PixelLayout compute_decode_source_layout(const DicomFile& df) {
	// Collect the pixel metadata snapshot from root-level DICOM elements once so
	// decode, transcode, and direct raw access share one normalized source layout.
	const auto& dataset = df.dataset();

	PixelLayout source_layout{};
	const auto rows = static_cast<int>(dataset["Rows"_tag].to_long().value_or(0));
	const auto cols = static_cast<int>(dataset["Columns"_tag].to_long().value_or(0));
	const auto samples_per_pixel =
	    static_cast<int>(dataset["SamplesPerPixel"_tag].to_long().value_or(1));
	const auto bits_allocated =
	    static_cast<int>(dataset["BitsAllocated"_tag].to_long().value_or(0));
	const auto bits_stored =
	    static_cast<int>(dataset["BitsStored"_tag].to_long().value_or(0));
	const auto pixel_representation =
	    dataset["PixelRepresentation"_tag].to_long().value_or(0);
	const auto frames =
	    static_cast<int>(dataset["NumberOfFrames"_tag].to_long().value_or(1));
	const auto stored_planar =
	    (dataset["PlanarConfiguration"_tag].to_long().value_or(0) == 1)
	        ? pixel::Planar::planar
	        : pixel::Planar::interleaved;
	const auto photometric = [&]() -> pixel::Photometric {
		if (const auto photometric_text =
		        dataset["PhotometricInterpretation"_tag].to_string_view();
		    photometric_text.has_value()) {
			if (const auto parsed = parse_photometric_from_text(*photometric_text);
			    parsed.has_value()) {
				return *parsed;
			}
		}
		return samples_per_pixel > 1 ? pixel::Photometric::rgb
		                             : pixel::Photometric::monochrome2;
	}();

	if (rows <= 0 || cols <= 0 || samples_per_pixel <= 0 || frames <= 0) {
		return source_layout;
	}

	source_layout.photometric = photometric;
	source_layout.planar = stored_planar;
	source_layout.reserved = 0;
	source_layout.rows = static_cast<std::uint32_t>(rows);
	source_layout.cols = static_cast<std::uint32_t>(cols);
	source_layout.frames = static_cast<std::uint32_t>(frames);
	source_layout.samples_per_pixel = static_cast<std::uint16_t>(samples_per_pixel);

	// Resolve the stored sample dtype from the pixel value element family first.
	if (const auto& double_float_pixel = dataset["DoubleFloatPixelData"_tag];
	    double_float_pixel) {
		source_layout.data_type = pixel::DataType::f64;
		source_layout.bits_stored = 64;
	} else if (const auto& float_pixel = dataset["FloatPixelData"_tag]; float_pixel) {
		source_layout.data_type = pixel::DataType::f32;
		source_layout.bits_stored = 32;
	} else {
		switch (bits_allocated) {
		case 8:
			source_layout.data_type =
			    (pixel_representation == 0) ? pixel::DataType::u8 : pixel::DataType::s8;
			break;
		case 16:
			source_layout.data_type =
			    (pixel_representation == 0) ? pixel::DataType::u16 : pixel::DataType::s16;
			break;
		case 32:
			source_layout.data_type =
			    (pixel_representation == 0) ? pixel::DataType::u32 : pixel::DataType::s32;
			break;
		default:
			source_layout.data_type = pixel::DataType::unknown;
			break;
		}
		const auto storage_bits =
		    static_cast<int>(bytes_per_sample_of(source_layout.data_type) * std::size_t{8});
		source_layout.bits_stored = static_cast<std::uint16_t>(bits_stored > 0
		                       ? bits_stored
		                       : (bits_allocated > 0 ? bits_allocated : storage_bits));
	}
	if (source_layout.data_type == pixel::DataType::unknown) {
		return PixelLayout{};
	}
	try {
		return source_layout.packed(1);
	} catch (...) {
		return PixelLayout{};
	}
}

PreparedDecodeFrameSource prepare_decode_frame_source_or_throw(
    const DicomFile& df, const PixelLayout& source_layout, std::size_t frame_index) {
	const auto& ds = df.dataset();

	PreparedDecodeFrameSource prepared_source{};
	if (const auto transfer_syntax = df.transfer_syntax_uid();
	    transfer_syntax.is_uncompressed() && !transfer_syntax.is_encapsulated()) {
		const auto source = build_native_decode_source_view_or_throw(ds, source_layout);
		prepared_source.bytes = native_decode_frame_bytes_or_throw(source, frame_index);
		return prepared_source;
	}

	const auto source = load_encapsulated_frame_source_or_throw(ds, frame_index);
	prepared_source.bytes = materialize_encapsulated_frame_source_or_throw(
	    source, prepared_source.owned_bytes);
	return prepared_source;
}

PreparedDecodeFrameSource prepare_decode_frame_source_or_throw(
    const PixelSequence& pixel_sequence, std::size_t frame_index) {
	PreparedDecodeFrameSource prepared_source{};
	const auto source =
	    load_encapsulated_frame_source_or_throw(pixel_sequence, frame_index, true);
	prepared_source.bytes = materialize_encapsulated_frame_source_or_throw(
	    source, prepared_source.owned_bytes);
	return prepared_source;
}

PreparedDecodeFrameSource prepare_decode_frame_source_without_cache_or_throw(
    const DicomFile& df, const PixelLayout& source_layout, std::size_t frame_index) {
	const auto& ds = df.dataset();

	PreparedDecodeFrameSource prepared_source{};
	if (const auto transfer_syntax = df.transfer_syntax_uid();
	    transfer_syntax.is_uncompressed() && !transfer_syntax.is_encapsulated()) {
		const auto source = build_native_decode_source_view_or_throw(ds, source_layout);
		prepared_source.bytes = native_decode_frame_bytes_or_throw(source, frame_index);
		return prepared_source;
	}

	const auto source =
	    load_encapsulated_frame_source_without_cache_or_throw(ds, frame_index);
	prepared_source.bytes = materialize_encapsulated_frame_source_or_throw(
	    source, prepared_source.owned_bytes);
	return prepared_source;
}

PreparedDecodeFrameSource prepare_decode_frame_source_without_cache_or_throw(
    const PixelSequence& pixel_sequence, std::size_t frame_index) {
	PreparedDecodeFrameSource prepared_source{};
	const auto source =
	    load_encapsulated_frame_source_or_throw(pixel_sequence, frame_index, false);
	prepared_source.bytes = materialize_encapsulated_frame_source_or_throw(
	    source, prepared_source.owned_bytes);
	return prepared_source;
}

NativeDecodeSourceView compute_native_decode_source_view_or_throw(
    const DicomFile& df, const PixelLayout& source_layout) {
	return build_native_decode_source_view_or_throw(df.dataset(), source_layout);
}

std::span<const std::uint8_t> native_decode_frame_bytes_or_throw(
    const NativeDecodeSourceView& source_view, std::size_t frame_index) {
	if (frame_index >= source_view.frames) {
		throw_decode_frame_source_error(
		    "raw frame index out of range (frames=" + std::to_string(source_view.frames) + ")");
	}

	std::size_t frame_offset = 0;
	if (!checked_mul_size_t(frame_index, source_view.frame_bytes, frame_offset)) {
		throw_decode_frame_source_error("raw frame offset exceeds size_t range");
	}
	if (source_view.source_bytes.size() < frame_offset ||
	    source_view.source_bytes.size() - frame_offset < source_view.frame_bytes) {
		throw_decode_frame_source_error(
		    std::string(source_view.source_name) + " is shorter than expected");
	}
	return source_view.source_bytes.subspan(frame_offset, source_view.frame_bytes);
}

std::optional<NativeOneBitPixelLayout>
try_compute_native_one_bit_pixel_layout(const DataSet& dataset) {
	return compute_native_one_bit_pixel_layout_from_dataset(dataset);
}

std::optional<NativeOneBitPixelLayout>
try_compute_native_one_bit_pixel_layout(const DicomFile& df) {
	return compute_native_one_bit_pixel_layout_from_dataset(df.dataset());
}

[[nodiscard]] std::uint8_t shifted_one_bit_source_byte(
    std::span<const std::uint8_t> source_bytes, std::size_t bit_index) noexcept {
	const auto byte_index = bit_index / std::size_t{8};
	const auto bit_offset = bit_index % std::size_t{8};
	std::uint16_t window = 0;
	if (byte_index < source_bytes.size()) {
		window = source_bytes[byte_index];
	}
	if (bit_offset != 0 && byte_index + 1u < source_bytes.size()) {
		window |= static_cast<std::uint16_t>(source_bytes[byte_index + 1u]) << 8u;
	}
	return static_cast<std::uint8_t>((window >> bit_offset) & 0xFFu);
}

void unpack_native_one_bit_mono_frame_or_throw(
    std::span<const std::uint8_t> source_bytes,
    const NativeOneBitPixelLayout& one_bit_layout, std::size_t frame_index,
    std::span<std::uint8_t> dst, const pixel::PixelLayout& output_layout) {
	if (output_layout.row_stride < one_bit_layout.cols) {
		throw_decode_frame_source_error("output row stride is smaller than 1-bit source row");
	}
	std::size_t frame_used_bytes = 0;
	if (!checked_mul_size_t(
	        output_layout.row_stride, one_bit_layout.rows, frame_used_bytes) ||
	    frame_used_bytes > output_layout.frame_stride) {
		throw_decode_frame_source_error("output frame stride is smaller than decoded rows");
	}

	std::size_t frame_bit_offset = 0;
	if (!checked_mul_size_t(frame_index, one_bit_layout.frame_bits, frame_bit_offset)) {
		throw_decode_frame_source_error("raw 1-bit frame bit offset exceeds size_t range");
	}

	const auto& expand = one_bit_expand_lut();
	for (std::size_t row = 0; row < one_bit_layout.rows; ++row) {
		auto* row_dst = dst.data() + row * output_layout.row_stride;
		auto bit_index = frame_bit_offset + row * one_bit_layout.cols;
		std::size_t remaining = one_bit_layout.cols;
		auto* out = row_dst;
		while (remaining >= 8u) {
			const auto source_byte = shifted_one_bit_source_byte(source_bytes, bit_index);
			std::memcpy(out, expand[source_byte].data(), 8u);
			out += 8u;
			bit_index += 8u;
			remaining -= 8u;
		}
		if (remaining != 0) {
			const auto source_byte = shifted_one_bit_source_byte(source_bytes, bit_index);
			std::memcpy(out, expand[source_byte].data(), remaining);
		}
		if (output_layout.row_stride > one_bit_layout.cols) {
			std::memset(row_dst + one_bit_layout.cols, 0,
			    output_layout.row_stride - one_bit_layout.cols);
		}
	}
	if (output_layout.frame_stride > frame_used_bytes) {
		std::memset(dst.data() + frame_used_bytes, 0,
		    output_layout.frame_stride - frame_used_bytes);
	}
}

void unpack_native_one_bit_frame_or_throw(
    std::span<const std::uint8_t> source_bytes,
    const NativeOneBitPixelLayout& one_bit_layout, std::size_t frame_index,
    std::span<std::uint8_t> dst, const pixel::PixelLayout& output_layout) {
	if (frame_index >= one_bit_layout.frames) {
		throw_decode_frame_source_error(
		    "raw 1-bit frame index out of range (frames=" +
		    std::to_string(one_bit_layout.frames) + ")");
	}
	if (source_bytes.size() < one_bit_layout.required_payload_bytes) {
		throw_decode_frame_source_error("PixelData is shorter than expected");
	}
	if (dst.size() < output_layout.frame_stride) {
		throw_decode_frame_source_error("destination buffer is smaller than required decoded size");
	}
	if (output_layout.data_type != pixel::DataType::u8 ||
	    output_layout.rows != one_bit_layout.rows ||
	    output_layout.cols != one_bit_layout.cols ||
	    output_layout.samples_per_pixel != one_bit_layout.samples_per_pixel) {
		throw_decode_frame_source_error("output layout does not match 1-bit source geometry");
	}

	if (one_bit_layout.samples_per_pixel == std::size_t{1}) {
		unpack_native_one_bit_mono_frame_or_throw(
		    source_bytes, one_bit_layout, frame_index, dst, output_layout);
		return;
	}

	std::fill_n(dst.begin(), output_layout.frame_stride, std::uint8_t{0});

	std::size_t frame_bit_offset = 0;
	if (!checked_mul_size_t(frame_index, one_bit_layout.frame_bits, frame_bit_offset)) {
		throw_decode_frame_source_error("raw 1-bit frame bit offset exceeds size_t range");
	}

	const bool source_planar =
	    one_bit_layout.planar == pixel::Planar::planar &&
	    one_bit_layout.samples_per_pixel > std::size_t{1};
	const bool output_planar =
	    output_layout.planar == pixel::Planar::planar &&
	    one_bit_layout.samples_per_pixel > std::size_t{1};
	const auto rows = one_bit_layout.rows;
	const auto cols = one_bit_layout.cols;
	const auto samples = one_bit_layout.samples_per_pixel;
	const std::size_t pixels_per_plane = rows * cols;
	const std::size_t output_plane_stride = output_layout.row_stride * rows;

	for (std::size_t row = 0; row < rows; ++row) {
		for (std::size_t col = 0; col < cols; ++col) {
			for (std::size_t sample = 0; sample < samples; ++sample) {
				const auto source_component = source_planar
				                                  ? sample * pixels_per_plane + row * cols + col
				                                  : (row * cols + col) * samples + sample;
				const auto bit_index = frame_bit_offset + source_component;
				const auto byte_index = bit_index / std::size_t{8};
				const auto bit_offset = bit_index % std::size_t{8};
				const auto value =
				    static_cast<std::uint8_t>((source_bytes[byte_index] >> bit_offset) & 0x01u);
				const auto output_offset = output_planar
				                               ? sample * output_plane_stride +
				                                     row * output_layout.row_stride + col
				                               : row * output_layout.row_stride +
				                                     col * samples + sample;
				dst[output_offset] = value;
			}
		}
	}
}

namespace {

[[nodiscard]] ComputedEncodeSourceLayout compute_encode_source_layout_impl(
    const pixel::PixelLayout& layout, std::span<const std::uint8_t> source_bytes,
    bool validate_source_bytes) {
	constexpr std::string_view kStage = "compute_encode_source_layout";
	const auto throw_invalid_argument =
	    [kStage](std::string_view reason) -> void {
		    codec_detail::throw_codec_stage_exception(
		        codec_detail::CodecStatusCode::invalid_argument, kStage, "{}", reason);
	    };
	const auto throw_internal_error =
	    [kStage](std::string_view reason) -> void {
		    codec_detail::throw_codec_stage_exception(
		        codec_detail::CodecStatusCode::internal_error, kStage, "{}", reason);
	    };

	const auto native_layout = native_source_layout_of(layout.data_type);
	if (!native_layout) {
		throw_invalid_argument(
		    "source layout dtype must be one of u8/s8/u16/s16/u32/s32 for current implementation");
	}
	const auto bytes_per_sample = bytes_per_sample_of(layout.data_type);
	if (bytes_per_sample == 0) {
		throw_invalid_argument("invalid source layout dtype");
	}

	// Encode always expects a normalized non-empty layout with concrete geometry.
	if (layout.empty()) {
		throw_invalid_argument("source layout must be non-empty");
	}
	if (layout.rows > 65535u || layout.cols > 65535u) {
		throw_invalid_argument("rows/cols must be <= 65535");
	}

	const auto rows = static_cast<std::size_t>(layout.rows);
	const auto cols = static_cast<std::size_t>(layout.cols);
	const auto frames = static_cast<std::size_t>(layout.frames);
	const auto samples_per_pixel = static_cast<std::size_t>(layout.samples_per_pixel);
	const bool planar_source =
	    layout.planar == pixel::Planar::planar &&
	    samples_per_pixel > std::size_t{1};
	constexpr std::size_t kSizeMax = std::numeric_limits<std::size_t>::max();

	if (!planar_source && samples_per_pixel > kSizeMax / cols) {
		throw_internal_error("row samples overflows size_t");
	}
	const std::size_t row_samples = planar_source ? cols : cols * samples_per_pixel;
	if (row_samples > kSizeMax / bytes_per_sample) {
		throw_internal_error("row payload bytes overflows size_t");
	}
	const std::size_t row_payload_bytes = row_samples * bytes_per_sample;

	// Preserve any explicit caller stride instead of silently repacking it here.
	const std::size_t source_row_stride = layout.row_stride;
	if (source_row_stride < row_payload_bytes) {
		codec_detail::throw_codec_stage_exception(
		    codec_detail::CodecStatusCode::invalid_argument, kStage,
		    "row_stride({}) is smaller than row payload({})",
		    source_row_stride, row_payload_bytes);
	}
	if (source_row_stride > kSizeMax / rows) {
		throw_internal_error("source plane stride overflows size_t");
	}
	const std::size_t source_plane_stride = source_row_stride * rows;

	std::size_t source_frame_size_bytes = source_plane_stride;
	if (planar_source) {
		if (samples_per_pixel > kSizeMax / source_plane_stride) {
			throw_internal_error("source frame size bytes overflows size_t");
		}
		source_frame_size_bytes = source_plane_stride * samples_per_pixel;
	}
	const std::size_t source_frame_stride = layout.frame_stride;
	if (source_frame_stride < source_frame_size_bytes) {
		codec_detail::throw_codec_stage_exception(
		    codec_detail::CodecStatusCode::invalid_argument, kStage,
		    "frame_stride({}) is smaller than frame size({})",
		    source_frame_stride, source_frame_size_bytes);
	}

	if (row_payload_bytes > kSizeMax / rows) {
		throw_internal_error("destination plane stride overflows size_t");
	}
	const std::size_t destination_plane_stride = row_payload_bytes * rows;
	std::size_t destination_frame_payload = destination_plane_stride;
	if (planar_source) {
		if (samples_per_pixel > kSizeMax / destination_plane_stride) {
			throw_internal_error("destination frame size bytes overflows size_t");
		}
		destination_frame_payload = destination_plane_stride * samples_per_pixel;
	}
	if (frames > kSizeMax / destination_frame_payload) {
		throw_internal_error("destination total bytes overflows size_t");
	}
	const std::size_t destination_total_bytes = destination_frame_payload * frames;

	if (frames > std::size_t{1} && (frames - 1) > kSizeMax / source_frame_stride) {
		throw_internal_error("source last frame begin overflows size_t");
	}
	const std::size_t source_last_frame_begin = (frames - 1) * source_frame_stride;
	const std::size_t source_last_plane_used =
	    (source_plane_stride - source_row_stride) + row_payload_bytes;
	const std::size_t source_last_frame_used = planar_source
	                                               ? (source_frame_size_bytes - source_plane_stride) +
	                                                     source_last_plane_used
	                                               : source_last_plane_used;
	if (source_last_frame_begin > kSizeMax - source_last_frame_used) {
		throw_internal_error("minimum source byte requirement overflows size_t");
	}
	const std::size_t source_required_bytes = source_last_frame_begin + source_last_frame_used;
	if (validate_source_bytes && source_bytes.size() < source_required_bytes) {
		codec_detail::throw_codec_stage_exception(
		    codec_detail::CodecStatusCode::invalid_argument, kStage,
		    "source bytes({}) are shorter than required({})",
		    source_bytes.size(), source_required_bytes);
	}

	const int bits_allocated = native_layout->bits_allocated;
	const int bits_stored =
	    layout.bits_stored > 0 ? static_cast<int>(layout.bits_stored) : bits_allocated;
	const int pixel_representation = native_layout->pixel_representation;
	if (bits_stored <= 0 || bits_stored > bits_allocated) {
		codec_detail::throw_codec_stage_exception(
		    codec_detail::CodecStatusCode::invalid_argument, kStage,
		    "bits_stored({}) must be in [1, bits_allocated({})]",
		    bits_stored, bits_allocated);
	}

	return ComputedEncodeSourceLayout{
	    .bytes_per_sample = bytes_per_sample,
	    .rows = rows,
	    .cols = cols,
	    .frames = frames,
	    .samples_per_pixel = samples_per_pixel,
	    .planar_source = planar_source,
	    .row_payload_bytes = row_payload_bytes,
	    .source_row_stride = source_row_stride,
	    .source_plane_stride = source_plane_stride,
	    .source_frame_size_bytes = source_frame_size_bytes,
	    .source_frame_stride = source_frame_stride,
	    .destination_frame_payload = destination_frame_payload,
	    .destination_total_bytes = destination_total_bytes,
	    .bits_allocated = bits_allocated,
	    .bits_stored = bits_stored,
	    .pixel_representation = pixel_representation,
	    .high_bit = bits_stored - 1,
	};
}

} // namespace

ComputedEncodeSourceLayout compute_encode_source_layout_or_throw(
    pixel::ConstPixelSpan source) {
	return compute_encode_source_layout_impl(
	    source.layout, source.bytes, true);
}

ComputedEncodeSourceLayout compute_encode_source_layout_without_source_bytes_or_throw(
    const pixel::PixelLayout& source_layout) {
	return compute_encode_source_layout_impl(
	    source_layout, {}, false);
}

bool source_aliases_native_pixel_data(
    const DataSet& dataset, std::span<const std::uint8_t> source_bytes) noexcept {
	if (source_bytes.empty()) {
		return false;
	}

	const auto& pixel_data = dataset["PixelData"_tag];
	if (!pixel_data || pixel_data.vr().is_pixel_sequence() || !pixel_data.vr().is_binary()) {
		return false;
	}
	return spans_overlap(source_bytes, pixel_data.value_span());
}

std::vector<std::uint8_t> build_native_pixel_payload(
    const NativePixelCopyInput& input) {
	std::vector<std::uint8_t> native_pixel_data(input.destination_total_bytes);
	if (native_pixel_data.empty()) {
		return native_pixel_data;
	}

	const auto* source_base = input.source_bytes.data();
	auto* destination_base = native_pixel_data.data();

	if (input.planar_source) {
		const std::size_t destination_plane_stride =
		    input.destination_frame_payload / input.samples_per_pixel;
		for (std::size_t frame_index = 0; frame_index < input.frames; ++frame_index) {
			const auto* source_frame = source_base + frame_index * input.source_frame_stride;
			auto* destination_frame =
			    destination_base + frame_index * input.destination_frame_payload;
			for (std::size_t sample = 0; sample < input.samples_per_pixel; ++sample) {
				const auto* source_plane = source_frame + sample * input.source_plane_stride;
				auto* destination_plane = destination_frame + sample * destination_plane_stride;
				for (std::size_t row = 0; row < input.rows; ++row) {
					std::memcpy(destination_plane + row * input.row_payload_bytes,
					    source_plane + row * input.source_row_stride, input.row_payload_bytes);
				}
			}
		}
		return native_pixel_data;
	}

	for (std::size_t frame_index = 0; frame_index < input.frames; ++frame_index) {
		const auto* source_frame = source_base + frame_index * input.source_frame_stride;
		auto* destination_frame =
		    destination_base + frame_index * input.destination_frame_payload;
		for (std::size_t row = 0; row < input.rows; ++row) {
			std::memcpy(destination_frame + row * input.row_payload_bytes,
			    source_frame + row * input.source_row_stride, input.row_payload_bytes);
		}
	}
	return native_pixel_data;
}

} // namespace dicom::pixel::support_detail
