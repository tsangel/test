#include "pixel/host/support/dicom_pixel_support.hpp"

#include "diagnostics.h"

#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace diag = dicom::diag;

namespace dicom::pixel::support_detail {
using namespace dicom::literals;

namespace {

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
    const DataSet& ds, std::size_t frame_index) {
	const auto& pixel_data = ds["PixelData"_tag];
	if (!pixel_data || !pixel_data.vr().is_pixel_sequence()) {
		throw_decode_frame_source_error("encapsulated decode requires PixelData sequence");
	}

	const auto* pixel_sequence = pixel_data.as_pixel_sequence();
	if (!pixel_sequence) {
		throw_decode_frame_source_error("PixelData sequence is missing");
	}

	const auto frame_count = pixel_sequence->number_of_frames();
	if (frame_index >= frame_count) {
		throw_decode_frame_source_error(
		    "frame index out of range (frames=" + std::to_string(frame_count) + ")");
	}

	const auto* frame = pixel_sequence->frame(frame_index);
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

	auto* mutable_sequence = const_cast<PixelSequence*>(pixel_sequence);
	source.contiguous = mutable_sequence->frame_encoded_span(frame_index);
	if (!source.contiguous.empty()) {
		source.total_size = source.contiguous.size();
		return source;
	}

	const auto* stream = pixel_sequence->stream();
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

NativeDecodeSourceView build_native_decode_source_view_or_throw(
    const DataSet& ds, const pixel::PixelDataInfo& info) {
	const auto source = select_native_source_element(ds, info.sv_dtype);
	if (!source.element || !(*source.element)) {
		throw_decode_frame_source_error("missing " + std::string(source.name));
	}
	if (source.element->vr().is_pixel_sequence()) {
		throw_decode_frame_source_error(
		    std::string(source.name) + " is encapsulated, not raw");
	}

	if (info.rows <= 0 || info.cols <= 0 || info.samples_per_pixel <= 0 ||
	    info.frames <= 0) {
		throw_decode_frame_source_error("invalid raw pixel metadata");
	}

	const auto bytes_per_sample = bytes_per_sample_of(info.sv_dtype);
	if (bytes_per_sample == 0) {
		throw_decode_frame_source_error("unsupported source dtype for raw decode");
	}

	const auto rows = static_cast<std::size_t>(info.rows);
	const auto cols = static_cast<std::size_t>(info.cols);
	const auto samples_per_pixel = static_cast<std::size_t>(info.samples_per_pixel);
	const auto frames = static_cast<std::size_t>(info.frames);
	const bool planar_source =
	    info.planar_configuration == Planar::planar && samples_per_pixel > std::size_t{1};
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

PreparedDecodeFrameSource prepare_decode_frame_source_or_throw(
    const DicomFile& df, const PixelDataInfo& info, std::size_t frame_index) {
	const auto& ds = df.dataset();

	PreparedDecodeFrameSource prepared_source{};
	if (info.ts.is_uncompressed() && !info.ts.is_encapsulated()) {
		const auto source = build_native_decode_source_view_or_throw(ds, info);
		prepared_source.bytes = native_decode_frame_bytes_or_throw(source, frame_index);
		return prepared_source;
	}

	const auto source = load_encapsulated_frame_source_or_throw(ds, frame_index);
	prepared_source.bytes = materialize_encapsulated_frame_source_or_throw(
	    source, prepared_source.owned_bytes);
	return prepared_source;
}

NativeDecodeSourceView compute_native_decode_source_view_or_throw(
    const DicomFile& df, const PixelDataInfo& info) {
	return build_native_decode_source_view_or_throw(df.dataset(), info);
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

namespace {

[[nodiscard]] ComputedEncodeSourceLayout compute_encode_source_layout_impl(
    const pixel::PixelSource& source, std::string_view file_path,
    bool validate_source_bytes) {
	const auto native_layout = native_source_layout_of(source.data_type);
	if (!native_layout) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} reason=source.data_type must be one of u8/s8/u16/s16/u32/s32 for current implementation",
		    file_path);
	}
	const auto bytes_per_sample = bytes_per_sample_of(source.data_type);
	if (bytes_per_sample == 0) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} reason=invalid source.data_type",
		    file_path);
	}

	if (source.rows <= 0 || source.cols <= 0 || source.frames <= 0 ||
	    source.samples_per_pixel <= 0) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} reason=rows/cols/frames/samples_per_pixel must be positive",
		    file_path);
	}
	if (source.rows > 65535 || source.cols > 65535) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} reason=rows/cols must be <= 65535",
		    file_path);
	}

	const auto rows = static_cast<std::size_t>(source.rows);
	const auto cols = static_cast<std::size_t>(source.cols);
	const auto frames = static_cast<std::size_t>(source.frames);
	const auto samples_per_pixel = static_cast<std::size_t>(source.samples_per_pixel);
	const bool planar_source =
	    source.planar == pixel::Planar::planar && samples_per_pixel > std::size_t{1};
	constexpr std::size_t kSizeMax = std::numeric_limits<std::size_t>::max();

	if (!planar_source && samples_per_pixel > kSizeMax / cols) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} reason=row samples overflows size_t",
		    file_path);
	}
	const std::size_t row_samples = planar_source ? cols : cols * samples_per_pixel;
	if (row_samples > kSizeMax / bytes_per_sample) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} reason=row payload bytes overflows size_t",
		    file_path);
	}
	const std::size_t row_payload_bytes = row_samples * bytes_per_sample;

	const std::size_t source_row_stride =
	    source.row_stride == 0 ? row_payload_bytes : source.row_stride;
	if (source_row_stride < row_payload_bytes) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} reason=row_stride({}) is smaller than row payload({})",
		    file_path, source_row_stride, row_payload_bytes);
	}
	if (source_row_stride > kSizeMax / rows) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} reason=source plane stride overflows size_t",
		    file_path);
	}
	const std::size_t source_plane_stride = source_row_stride * rows;

	std::size_t source_frame_size_bytes = source_plane_stride;
	if (planar_source) {
		if (samples_per_pixel > kSizeMax / source_plane_stride) {
			diag::error_and_throw(
			    "DicomFile::set_pixel_data file={} reason=source frame size bytes overflows size_t",
			    file_path);
		}
		source_frame_size_bytes = source_plane_stride * samples_per_pixel;
	}
	const std::size_t source_frame_stride =
	    source.frame_stride == 0 ? source_frame_size_bytes : source.frame_stride;
	if (source_frame_stride < source_frame_size_bytes) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} reason=frame_stride({}) is smaller than frame size({})",
		    file_path, source_frame_stride, source_frame_size_bytes);
	}

	if (row_payload_bytes > kSizeMax / rows) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} reason=destination plane stride overflows size_t",
		    file_path);
	}
	const std::size_t destination_plane_stride = row_payload_bytes * rows;
	std::size_t destination_frame_payload = destination_plane_stride;
	if (planar_source) {
		if (samples_per_pixel > kSizeMax / destination_plane_stride) {
			diag::error_and_throw(
			    "DicomFile::set_pixel_data file={} reason=destination frame size bytes overflows size_t",
			    file_path);
		}
		destination_frame_payload = destination_plane_stride * samples_per_pixel;
	}
	if (frames > kSizeMax / destination_frame_payload) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} reason=destination total bytes overflows size_t",
		    file_path);
	}
	const std::size_t destination_total_bytes = destination_frame_payload * frames;

	if (frames > std::size_t{1} && (frames - 1) > kSizeMax / source_frame_stride) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} reason=source last frame begin overflows size_t",
		    file_path);
	}
	const std::size_t source_last_frame_begin = (frames - 1) * source_frame_stride;
	const std::size_t source_last_plane_used =
	    (source_plane_stride - source_row_stride) + row_payload_bytes;
	const std::size_t source_last_frame_used = planar_source
	                                               ? (source_frame_size_bytes - source_plane_stride) +
	                                                     source_last_plane_used
	                                               : source_last_plane_used;
	if (source_last_frame_begin > kSizeMax - source_last_frame_used) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} reason=minimum source byte requirement overflows size_t",
		    file_path);
	}
	const std::size_t source_required_bytes = source_last_frame_begin + source_last_frame_used;
	if (validate_source_bytes && source.bytes.size() < source_required_bytes) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} reason=source bytes({}) are shorter than required({})",
		    file_path, source.bytes.size(), source_required_bytes);
	}

	const int bits_allocated = native_layout->bits_allocated;
	const int bits_stored = source.bits_stored > 0 ? source.bits_stored : bits_allocated;
	const int pixel_representation = native_layout->pixel_representation;
	if (bits_stored <= 0 || bits_stored > bits_allocated) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} reason=bits_stored({}) must be in [1, bits_allocated({})]",
		    file_path, bits_stored, bits_allocated);
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
    const pixel::PixelSource& source, std::string_view file_path) {
	return compute_encode_source_layout_impl(source, file_path, true);
}

ComputedEncodeSourceLayout compute_encode_source_layout_without_source_bytes_or_throw(
    const pixel::PixelSource& source, std::string_view file_path) {
	return compute_encode_source_layout_impl(source, file_path, false);
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
