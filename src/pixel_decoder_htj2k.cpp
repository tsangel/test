#include "pixel_decoder_detail.hpp"

#include "diagnostics.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <ojph_codestream.h>
#include <ojph_file.h>
#include <ojph_mem.h>
#include <ojph_params.h>

using namespace dicom::literals;
namespace diag = dicom::diag;

namespace dicom {
namespace pixel::detail {

namespace {

struct htj2k_frame_source {
	const PixelFrame* frame{nullptr};
	const InStream* stream{nullptr};
	std::span<const std::uint8_t> contiguous{};
	const std::vector<PixelFragment>* fragments{nullptr};
	std::size_t total_size{0};
};

bool sv_dtype_is_signed(DataType sv_dtype) noexcept {
	switch (sv_dtype) {
	case DataType::s8:
	case DataType::s16:
	case DataType::s32:
		return true;
	default:
		return false;
	}
}

bool sv_dtype_is_integral(DataType sv_dtype) noexcept {
	switch (sv_dtype) {
	case DataType::u8:
	case DataType::s8:
	case DataType::u16:
	case DataType::s16:
	case DataType::u32:
	case DataType::s32:
		return true;
	default:
		return false;
	}
}

std::string trimmed_message(std::string message) {
	while (!message.empty() &&
	       (message.back() == '\n' || message.back() == '\r' || message.back() == ' ' ||
	           message.back() == '\t')) {
		message.pop_back();
	}
	return message;
}

const char* htj2k_backend_name(Htj2kDecoder backend) noexcept {
	switch (backend) {
	case Htj2kDecoder::auto_select:
		return "auto";
	case Htj2kDecoder::openjph:
		return "openjph";
	case Htj2kDecoder::openjpeg:
		return "openjpeg";
	default:
		return "unknown";
	}
}

void validate_destination(const DicomFile& df, std::span<std::uint8_t> dst,
    const DecodeStrides& dst_strides, Planar dst_planar, std::size_t rows, std::size_t cols,
    std::size_t samples_per_pixel, std::size_t bytes_per_sample) {
	const std::size_t dst_row_components =
	    (dst_planar == Planar::interleaved) ? samples_per_pixel : std::size_t{1};
	const std::size_t dst_min_row_bytes = cols * dst_row_components * bytes_per_sample;
	if (dst_strides.row < dst_min_row_bytes) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=row stride too small (need>={}, got={})",
		    df.path(), dst_min_row_bytes, dst_strides.row);
	}

	std::size_t min_frame_bytes = dst_strides.row * rows;
	if (dst_planar == Planar::planar) {
		min_frame_bytes *= samples_per_pixel;
	}
	if (dst_strides.frame < min_frame_bytes) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=frame stride too small (need>={}, got={})",
		    df.path(), min_frame_bytes, dst_strides.frame);
	}
	if (dst.size() < dst_strides.frame) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=destination too small (need={}, got={})",
		    df.path(), dst_strides.frame, dst.size());
	}
}

htj2k_frame_source load_htj2k_frame_source(const DicomFile& df, std::size_t frame_index) {
	const auto& ds = df.dataset();
	const auto& pixel_data = ds["PixelData"_tag];
	if (!pixel_data || !pixel_data.vr().is_pixel_sequence()) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=HTJ2K requires encapsulated PixelData",
		    df.path());
	}

	const auto* pixel_sequence = pixel_data.as_pixel_sequence();
	if (!pixel_sequence) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=HTJ2K pixel sequence is missing",
		    df.path());
	}

	const auto frame_count = pixel_sequence->number_of_frames();
	if (frame_index >= frame_count) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} frame={} reason=HTJ2K frame index out of range (frames={})",
		    df.path(), frame_index, frame_count);
	}

	const auto* frame = pixel_sequence->frame(frame_index);
	if (!frame) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} frame={} reason=HTJ2K frame is missing",
		    df.path(), frame_index);
	}

	htj2k_frame_source source{};
	source.frame = frame;
	if (frame->encoded_data_size() != 0) {
		source.contiguous = frame->encoded_data_view();
		source.total_size = source.contiguous.size();
		return source;
	}

	const auto& fragments = frame->fragments();
	if (fragments.empty()) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} frame={} reason=HTJ2K frame has no fragments",
		    df.path(), frame_index);
	}
	for (const auto& fragment : fragments) {
		if (fragment.length == 0) {
			diag::error_and_throw(
			    "pixel::decode_frame_into file={} frame={} reason=HTJ2K zero-length fragment is not supported",
			    df.path(), frame_index);
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
		    "pixel::decode_frame_into file={} frame={} reason=HTJ2K pixel sequence stream is missing",
		    df.path(), frame_index);
	}
	source.stream = stream;
	source.fragments = &fragments;

	std::size_t total_size = 0;
	for (const auto& frag : fragments) {
		if (frag.length > std::numeric_limits<std::size_t>::max() - total_size) {
			diag::error_and_throw(
			    "pixel::decode_frame_into file={} frame={} reason=HTJ2K frame size overflow",
			    df.path(), frame_index);
		}
		total_size += frag.length;
	}
	source.total_size = total_size;
	return source;
}

template <typename DstT>
inline void store_htj2k_value(std::uint8_t* dst, DstT value) {
	std::memcpy(dst, &value, sizeof(DstT));
}

std::span<const std::uint8_t> materialize_frame_codestream_for_openjph(const DicomFile& df,
    std::size_t frame_index, const htj2k_frame_source& source, std::vector<std::uint8_t>& owned) {
	if (!source.contiguous.empty()) {
		return source.contiguous;
	}
	if (!source.fragments || !source.stream) {
		throw std::runtime_error(fmt::format(
		    "HTJ2K frame source is not contiguous for OpenJPH (file={}, frame={})",
		    df.path(), frame_index));
	}

	owned.clear();
	owned.resize(source.total_size);
	std::size_t copied = 0;
	for (const auto& fragment : *source.fragments) {
		if (fragment.length == 0) {
			continue;
		}
		if (copied > owned.size() || fragment.length > owned.size() - copied) {
			throw std::runtime_error(fmt::format(
			    "HTJ2K frame materialization overflow (file={}, frame={})",
			    df.path(), frame_index));
		}
		const auto fragment_span = source.stream->get_span(fragment.offset, fragment.length);
		std::memcpy(owned.data() + copied, fragment_span.data(), fragment.length);
		copied += fragment.length;
	}
	if (copied != owned.size()) {
		throw std::runtime_error(fmt::format(
		    "HTJ2K frame materialization size mismatch (file={}, frame={}, expected={}, copied={})",
		    df.path(), frame_index, owned.size(), copied));
	}
	return std::span<const std::uint8_t>(owned.data(), owned.size());
}

void validate_openjph_decoded_metadata(const DicomFile& df, std::size_t frame_index,
    const DicomFile::pixel_info_t& info, const ojph::param_siz& siz, std::size_t rows,
    std::size_t cols, std::size_t samples_per_pixel) {
	const auto decoded_components = static_cast<std::size_t>(siz.get_num_components());
	if (decoded_components != samples_per_pixel) {
		throw std::runtime_error(fmt::format(
		    "HTJ2K component count mismatch (file={}, frame={}, decoded={}, expected={})",
		    df.path(), frame_index, decoded_components, samples_per_pixel));
	}

	const auto bytes_per_sample = sv_dtype_bytes(info.sv_dtype);
	if (bytes_per_sample == 0 || bytes_per_sample > 4 || !sv_dtype_is_integral(info.sv_dtype)) {
		throw std::runtime_error(fmt::format(
		    "HTJ2K supports integral sv_dtype up to 32-bit only (file={}, frame={})",
		    df.path(), frame_index));
	}

	const bool expected_signed = sv_dtype_is_signed(info.sv_dtype);
	const auto max_precision = static_cast<std::uint32_t>(bytes_per_sample * 8);
	if (!expected_signed && max_precision == 32) {
		throw std::runtime_error(fmt::format(
		    "HTJ2K unsigned 32-bit samples are not supported (file={}, frame={})",
		    df.path(), frame_index));
	}

	for (std::size_t c = 0; c < samples_per_pixel; ++c) {
		const auto decoded_width = static_cast<std::size_t>(siz.get_recon_width(static_cast<ojph::ui32>(c)));
		const auto decoded_height = static_cast<std::size_t>(siz.get_recon_height(static_cast<ojph::ui32>(c)));
		if (decoded_width != cols || decoded_height != rows) {
			throw std::runtime_error(fmt::format(
			    "HTJ2K component {} dimensions mismatch (file={}, frame={}, decoded={}x{}, expected={}x{})",
			    c, df.path(), frame_index, decoded_height, decoded_width, rows, cols));
		}

		const auto component_signed = siz.is_signed(static_cast<ojph::ui32>(c));
		if (component_signed != expected_signed) {
			throw std::runtime_error(fmt::format(
			    "HTJ2K component {} signedness mismatch (file={}, frame={}, decoded={}, expected={})",
			    c, df.path(), frame_index, component_signed ? 1 : 0, expected_signed ? 1 : 0));
		}

		const auto precision = static_cast<std::uint32_t>(siz.get_bit_depth(static_cast<ojph::ui32>(c)));
		if (precision == 0 || precision > max_precision) {
			throw std::runtime_error(fmt::format(
			    "HTJ2K component {} precision {} exceeds output {} bits (file={}, frame={})",
			    c, precision, max_precision, df.path(), frame_index));
		}
	}
}

const std::int32_t* openjph_line_as_i32(const DicomFile& df, std::size_t frame_index,
    const ojph::line_buf* line, std::size_t cols, std::vector<std::int32_t>& scratch) {
	if (!line) {
		throw std::runtime_error(fmt::format(
		    "OpenJPH returned null line buffer (file={}, frame={})",
		    df.path(), frame_index));
	}
	if (line->size < cols) {
		throw std::runtime_error(fmt::format(
		    "OpenJPH line width too small (file={}, frame={}, have={}, need={})",
		    df.path(), frame_index, line->size, cols));
	}

	const bool is_integer = (line->flags & ojph::line_buf::LFT_INTEGER) != 0;
	if (is_integer && !line->i32) {
		throw std::runtime_error(fmt::format(
		    "OpenJPH integer line has null data pointer (file={}, frame={})",
		    df.path(), frame_index));
	}
	if (!is_integer && !line->f32) {
		throw std::runtime_error(fmt::format(
		    "OpenJPH floating-point line has null data pointer (file={}, frame={})",
		    df.path(), frame_index));
	}
	if (is_integer) {
		return line->i32;
	}
	scratch.resize(cols);
	for (std::size_t c = 0; c < cols; ++c) {
		scratch[c] = static_cast<std::int32_t>(std::lround(line->f32[c]));
	}
	return scratch.data();
}

void copy_openjph_line_to_interleaved(const DicomFile& df, std::size_t frame_index,
    const ojph::line_buf* line, std::size_t row, std::size_t comp, std::size_t cols,
    std::size_t samples_per_pixel, std::vector<std::int32_t>& decoded,
    std::vector<std::int32_t>& scratch) {
	const auto* samples = openjph_line_as_i32(df, frame_index, line, cols, scratch);
	for (std::size_t c = 0; c < cols; ++c) {
		const auto pixel_index = row * cols + c;
		const auto dst_index = pixel_index * samples_per_pixel + comp;
		decoded[dst_index] = samples[c];
	}
}

std::vector<std::int32_t> decode_openjph_to_interleaved(const DicomFile& df, std::size_t frame_index,
    const DicomFile::pixel_info_t& info, const htj2k_frame_source& source,
    std::size_t rows, std::size_t cols, std::size_t samples_per_pixel) {
	std::vector<std::uint8_t> contiguous_storage{};
	const auto codestream_bytes = materialize_frame_codestream_for_openjph(
	    df, frame_index, source, contiguous_storage);
	if (codestream_bytes.empty()) {
		throw std::runtime_error(fmt::format(
		    "HTJ2K frame has empty codestream (file={}, frame={})",
		    df.path(), frame_index));
	}

	ojph::mem_infile infile{};
	infile.open(codestream_bytes.data(), codestream_bytes.size());

	ojph::codestream codestream{};
	codestream.set_planar(false);
	codestream.read_headers(&infile);
	const auto siz = codestream.access_siz();
	validate_openjph_decoded_metadata(df, frame_index, info, siz, rows, cols, samples_per_pixel);
	codestream.create();

	if (rows != 0 && cols > std::numeric_limits<std::size_t>::max() / rows) {
		throw std::runtime_error(fmt::format(
		    "HTJ2K decoded pixel count overflow (file={}, frame={})",
		    df.path(), frame_index));
	}
	const auto pixel_count = rows * cols;
	if (samples_per_pixel != 0 &&
	    pixel_count > std::numeric_limits<std::size_t>::max() / samples_per_pixel) {
		throw std::runtime_error(fmt::format(
		    "HTJ2K decoded sample count overflow (file={}, frame={})",
		    df.path(), frame_index));
	}
	std::vector<std::int32_t> decoded(pixel_count * samples_per_pixel);
	std::vector<std::int32_t> scratch_line{};

	if (codestream.is_planar()) {
		for (std::size_t comp = 0; comp < samples_per_pixel; ++comp) {
			for (std::size_t row = 0; row < rows; ++row) {
				ojph::ui32 comp_num = 0;
				auto* line = codestream.pull(comp_num);
					if (comp_num != static_cast<ojph::ui32>(comp)) {
						throw std::runtime_error(fmt::format(
						    "OpenJPH Planar pull order mismatch (file={}, frame={}, expected={}, got={})",
						    df.path(), frame_index, comp, comp_num));
					}
					copy_openjph_line_to_interleaved(
					    df, frame_index, line, row, comp, cols, samples_per_pixel, decoded,
					    scratch_line);
			}
		}
	} else {
		for (std::size_t row = 0; row < rows; ++row) {
			for (std::size_t comp = 0; comp < samples_per_pixel; ++comp) {
				ojph::ui32 comp_num = 0;
				auto* line = codestream.pull(comp_num);
					if (comp_num != static_cast<ojph::ui32>(comp)) {
						throw std::runtime_error(fmt::format(
						    "OpenJPH interleaved pull order mismatch (file={}, frame={}, expected={}, got={})",
						    df.path(), frame_index, comp, comp_num));
					}
					copy_openjph_line_to_interleaved(
					    df, frame_index, line, row, comp, cols, samples_per_pixel, decoded,
					    scratch_line);
			}
		}
	}

	codestream.close();
	infile.close();
	return decoded;
}

template <typename DstT>
void write_openjph_line_unscaled_to_dst(const std::int32_t* samples,
    std::span<std::uint8_t> dst, const DecodeStrides& dst_strides, Planar dst_planar,
    std::size_t rows, std::size_t row, std::size_t comp, std::size_t cols,
    std::size_t samples_per_pixel) {
	if (dst_planar == Planar::planar && samples_per_pixel > 1) {
		const std::size_t plane_bytes = dst_strides.row * rows;
		auto* dst_row = dst.data() + comp * plane_bytes + row * dst_strides.row;
		for (std::size_t c = 0; c < cols; ++c) {
			const auto sample = static_cast<DstT>(samples[c]);
			store_htj2k_value(dst_row + c * sizeof(DstT), sample);
		}
		return;
	}

	auto* dst_row = dst.data() + row * dst_strides.row;
	const std::size_t pixel_stride = samples_per_pixel * sizeof(DstT);
	for (std::size_t c = 0; c < cols; ++c) {
		const auto sample = static_cast<DstT>(samples[c]);
		store_htj2k_value(dst_row + c * pixel_stride + comp * sizeof(DstT), sample);
	}
}

template <typename DstT>
void decode_openjph_unscaled_into(const DicomFile& df, std::size_t frame_index,
    const DicomFile::pixel_info_t& info, const htj2k_frame_source& source,
    std::span<std::uint8_t> dst, const DecodeStrides& dst_strides, Planar dst_planar,
    std::size_t rows, std::size_t cols, std::size_t samples_per_pixel) {
	std::vector<std::uint8_t> contiguous_storage{};
	const auto codestream_bytes = materialize_frame_codestream_for_openjph(
	    df, frame_index, source, contiguous_storage);
	if (codestream_bytes.empty()) {
		throw std::runtime_error(fmt::format(
		    "HTJ2K frame has empty codestream (file={}, frame={})",
		    df.path(), frame_index));
	}

	ojph::mem_infile infile{};
	infile.open(codestream_bytes.data(), codestream_bytes.size());

	ojph::codestream codestream{};
	codestream.set_planar(false);
	codestream.read_headers(&infile);
	const auto siz = codestream.access_siz();
	validate_openjph_decoded_metadata(df, frame_index, info, siz, rows, cols, samples_per_pixel);
	codestream.create();

	std::vector<std::int32_t> scratch_line{};
	if (codestream.is_planar()) {
		for (std::size_t comp = 0; comp < samples_per_pixel; ++comp) {
			for (std::size_t row = 0; row < rows; ++row) {
				ojph::ui32 comp_num = 0;
				auto* line = codestream.pull(comp_num);
					if (comp_num != static_cast<ojph::ui32>(comp)) {
						throw std::runtime_error(fmt::format(
						    "OpenJPH Planar pull order mismatch (file={}, frame={}, expected={}, got={})",
						    df.path(), frame_index, comp, comp_num));
					}
					const auto* samples = openjph_line_as_i32(df, frame_index, line, cols, scratch_line);
					write_openjph_line_unscaled_to_dst<DstT>(
					    samples, dst, dst_strides, dst_planar, rows, row, comp, cols, samples_per_pixel);
				}
		}
	} else {
		for (std::size_t row = 0; row < rows; ++row) {
			for (std::size_t comp = 0; comp < samples_per_pixel; ++comp) {
				ojph::ui32 comp_num = 0;
				auto* line = codestream.pull(comp_num);
					if (comp_num != static_cast<ojph::ui32>(comp)) {
						throw std::runtime_error(fmt::format(
						    "OpenJPH interleaved pull order mismatch (file={}, frame={}, expected={}, got={})",
						    df.path(), frame_index, comp, comp_num));
					}
					const auto* samples = openjph_line_as_i32(df, frame_index, line, cols, scratch_line);
					write_openjph_line_unscaled_to_dst<DstT>(
					    samples, dst, dst_strides, dst_planar, rows, row, comp, cols, samples_per_pixel);
				}
		}
	}

	codestream.close();
	infile.close();
}

template <typename SampleT>
void write_openjph_scaled_mono_to_dst(const DicomFile& df,
    const std::vector<std::int32_t>& decoded, std::span<std::uint8_t> dst,
    const DecodeStrides& dst_strides, std::size_t rows, std::size_t cols) {
	const auto& ds = df.dataset();
	const auto modality_lut = df.modality_lut();
	if (modality_lut) {
		const auto last_index = static_cast<std::int64_t>(modality_lut->values.size() - 1);
		for (std::size_t r = 0; r < rows; ++r) {
			auto* dst_row = dst.data() + r * dst_strides.row;
			const auto src_index = r * cols;
			for (std::size_t c = 0; c < cols; ++c) {
				const auto sv = static_cast<SampleT>(decoded[src_index + c]);
				std::int64_t lut_index = static_cast<std::int64_t>(sv) - modality_lut->first_mapped;
				if (lut_index < 0) {
					lut_index = 0;
				} else if (lut_index > last_index) {
					lut_index = last_index;
				}
				const auto value = modality_lut->values[static_cast<std::size_t>(lut_index)];
				store_htj2k_value(dst_row + c * sizeof(float), value);
			}
		}
		return;
	}

	const auto slope = ds["RescaleSlope"_tag].to_double().value_or(1.0);
	const auto intercept = ds["RescaleIntercept"_tag].to_double().value_or(0.0);
	if (!std::isfinite(slope) || !std::isfinite(intercept)) {
		throw std::runtime_error(fmt::format(
		    "RescaleSlope/RescaleIntercept must be finite (file={})", df.path()));
	}

	for (std::size_t r = 0; r < rows; ++r) {
		auto* dst_row = dst.data() + r * dst_strides.row;
		const auto src_index = r * cols;
		for (std::size_t c = 0; c < cols; ++c) {
			const auto sv = static_cast<SampleT>(decoded[src_index + c]);
			const auto value = static_cast<float>(static_cast<double>(sv) * slope + intercept);
			store_htj2k_value(dst_row + c * sizeof(float), value);
		}
	}
}

bool try_decode_openjph_into(const DicomFile& df, const DicomFile::pixel_info_t& info,
    std::size_t frame_index, const htj2k_frame_source& source, std::span<std::uint8_t> dst,
    const DecodeStrides& dst_strides, const DecodeOptions& opt, std::size_t rows, std::size_t cols,
    std::size_t samples_per_pixel, std::string& failure) {
	failure.clear();
	try {
		if (opt.scaled) {
			auto decoded = decode_openjph_to_interleaved(
			    df, frame_index, info, source, rows, cols, samples_per_pixel);
			switch (info.sv_dtype) {
			case DataType::u8:
				write_openjph_scaled_mono_to_dst<std::uint8_t>(df, decoded, dst, dst_strides, rows, cols);
				return true;
			case DataType::s8:
				write_openjph_scaled_mono_to_dst<std::int8_t>(df, decoded, dst, dst_strides, rows, cols);
				return true;
			case DataType::u16:
				write_openjph_scaled_mono_to_dst<std::uint16_t>(df, decoded, dst, dst_strides, rows, cols);
				return true;
			case DataType::s16:
				write_openjph_scaled_mono_to_dst<std::int16_t>(df, decoded, dst, dst_strides, rows, cols);
				return true;
			case DataType::u32:
				write_openjph_scaled_mono_to_dst<std::uint32_t>(df, decoded, dst, dst_strides, rows, cols);
				return true;
			case DataType::s32:
				write_openjph_scaled_mono_to_dst<std::int32_t>(df, decoded, dst, dst_strides, rows, cols);
				return true;
			default:
				throw std::runtime_error(fmt::format(
				    "scaled output does not support sv_dtype={} (file={})",
				    static_cast<int>(info.sv_dtype), df.path()));
			}
		}

		switch (info.sv_dtype) {
		case DataType::u8:
			decode_openjph_unscaled_into<std::uint8_t>(
			    df, frame_index, info, source, dst, dst_strides, opt.planar_out, rows, cols,
			    samples_per_pixel);
			return true;
		case DataType::s8:
			decode_openjph_unscaled_into<std::int8_t>(
			    df, frame_index, info, source, dst, dst_strides, opt.planar_out, rows, cols,
			    samples_per_pixel);
			return true;
		case DataType::u16:
			decode_openjph_unscaled_into<std::uint16_t>(
			    df, frame_index, info, source, dst, dst_strides, opt.planar_out, rows, cols,
			    samples_per_pixel);
			return true;
		case DataType::s16:
			decode_openjph_unscaled_into<std::int16_t>(
			    df, frame_index, info, source, dst, dst_strides, opt.planar_out, rows, cols,
			    samples_per_pixel);
			return true;
		case DataType::u32:
			decode_openjph_unscaled_into<std::uint32_t>(
			    df, frame_index, info, source, dst, dst_strides, opt.planar_out, rows, cols,
			    samples_per_pixel);
			return true;
		case DataType::s32:
			decode_openjph_unscaled_into<std::int32_t>(
			    df, frame_index, info, source, dst, dst_strides, opt.planar_out, rows, cols,
			    samples_per_pixel);
			return true;
		default:
			throw std::runtime_error(fmt::format(
			    "JPEG2000 output does not support sv_dtype={} (file={})",
			    static_cast<int>(info.sv_dtype), df.path()));
		}
	} catch (const std::exception& e) {
		failure = trimmed_message(e.what());
		if (failure.empty()) {
			failure = "OpenJPH decode failed";
		}
		return false;
	} catch (const char* msg) {
		failure = trimmed_message(msg ? std::string(msg) : std::string{});
		if (failure.empty()) {
			failure = "OpenJPH decode failed";
		}
		return false;
	} catch (...) {
		failure = "OpenJPH decode failed (unknown error)";
		return false;
	}
}

} // namespace

void decode_htj2k_into(const DicomFile& df, const DicomFile::pixel_info_t& info,
    std::size_t frame_index, std::span<std::uint8_t> dst,
    const DecodeStrides& dst_strides, const DecodeOptions& opt) {
	if (!info.has_pixel_data) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=sv_dtype is unknown", df.path());
	}
	if (!info.ts.is_htj2k()) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=transfer syntax is not HTJ2K ({})",
		    df.path(), df.transfer_syntax_uid().value());
	}
	if (info.rows <= 0 || info.cols <= 0 || info.samples_per_pixel <= 0) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=invalid Rows/Columns/SamplesPerPixel",
		    df.path());
	}
	if (info.frames <= 0) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=invalid NumberOfFrames",
		    df.path());
	}

	const auto samples_per_pixel_value = info.samples_per_pixel;
	if (samples_per_pixel_value != 1 && samples_per_pixel_value != 3 &&
	    samples_per_pixel_value != 4) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=only SamplesPerPixel=1/3/4 is supported in current HTJ2K path",
		    df.path());
	}
	const auto samples_per_pixel = static_cast<std::size_t>(samples_per_pixel_value);
	if (opt.scaled && samples_per_pixel != 1) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=scaled output supports SamplesPerPixel=1 only",
		    df.path());
	}
	if (!sv_dtype_is_integral(info.sv_dtype)) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=HTJ2K supports integral sv_dtype only",
		    df.path());
	}

	const auto src_bytes_per_sample = sv_dtype_bytes(info.sv_dtype);
	if (src_bytes_per_sample == 0 || src_bytes_per_sample > 4) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=HTJ2K supports integral sv_dtype up to 32-bit only",
		    df.path());
	}

	const auto frame_count = static_cast<std::size_t>(info.frames);
	if (frame_index >= frame_count) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} frame={} reason=frame index out of range (frames={})",
		    df.path(), frame_index, frame_count);
	}

	const auto rows = static_cast<std::size_t>(info.rows);
	const auto cols = static_cast<std::size_t>(info.cols);
	const auto dst_bytes_per_sample = opt.scaled ? sizeof(float) : src_bytes_per_sample;
	validate_destination(
	    df, dst, dst_strides, opt.planar_out, rows, cols, samples_per_pixel, dst_bytes_per_sample);

	const auto frame_source = load_htj2k_frame_source(df, frame_index);
	if (frame_source.total_size == 0) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} frame={} reason=HTJ2K frame has empty codestream",
		    df.path(), frame_index);
	}

	if (opt.htj2k_decoder_backend == Htj2kDecoder::openjpeg) {
		decode_jpeg2k_into(df, info, frame_index, dst, dst_strides, opt);
		return;
	}

	if (opt.htj2k_decoder_backend == Htj2kDecoder::openjph) {
		std::string openjph_failure{};
		if (try_decode_openjph_into(df, info, frame_index, frame_source, dst, dst_strides, opt,
		        rows, cols, samples_per_pixel, openjph_failure)) {
			return;
		}
		const auto failure_reason = trimmed_message(openjph_failure);
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} frame={} reason=HTJ2K decode failed "
		    "(backend={}): {}",
		    df.path(), frame_index, htj2k_backend_name(opt.htj2k_decoder_backend),
		    failure_reason.empty() ? "OpenJPH decode failed" : failure_reason);
	}

	std::string openjph_failure{};
	if (try_decode_openjph_into(df, info, frame_index, frame_source, dst, dst_strides, opt,
	        rows, cols, samples_per_pixel, openjph_failure)) {
		return;
	}
	try {
		decode_jpeg2k_into(df, info, frame_index, dst, dst_strides, opt);
		return;
	} catch (const std::exception& e) {
		if (!openjph_failure.empty()) {
			diag::error_and_throw(
			    "pixel::decode_frame_into file={} frame={} reason=HTJ2K decode failed "
			    "(OpenJPH: {}; OpenJPEG: {})",
			    df.path(), frame_index, trimmed_message(openjph_failure), trimmed_message(e.what()));
		}
		throw;
	}
}

} // namespace pixel::detail
} // namespace dicom
