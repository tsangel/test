#include "pixel_decoder_detail.hpp"

#include "diagnostics.h"

#include <charls/charls.h>

#include <cstdint>
#include <exception>
#include <limits>
#include <span>
#include <vector>

using namespace dicom::literals;
namespace diag = dicom::diag;

namespace dicom {
namespace pixel::detail {

namespace {

struct jpegls_frame_buffer {
	std::vector<std::uint8_t> owned{};
	std::span<const std::uint8_t> view{};
};

bool sv_dtype_is_integral(dtype sv_dtype) noexcept {
	switch (sv_dtype) {
	case dtype::u8:
	case dtype::s8:
	case dtype::u16:
	case dtype::s16:
	case dtype::u32:
	case dtype::s32:
		return true;
	default:
		return false;
	}
}

void validate_destination(const DicomFile& df, std::span<std::uint8_t> dst,
    const strides& dst_strides, planar dst_planar, std::size_t rows, std::size_t cols,
    std::size_t samples_per_pixel, std::size_t bytes_per_sample) {
	const std::size_t dst_row_components =
	    (dst_planar == planar::interleaved) ? samples_per_pixel : std::size_t{1};
	const std::size_t dst_min_row_bytes = cols * dst_row_components * bytes_per_sample;
	if (dst_strides.row < dst_min_row_bytes) {
		diag::error_and_throw(
		    "pixel::decode_into file={} reason=row stride too small (need>={}, got={})",
		    df.path(), dst_min_row_bytes, dst_strides.row);
	}

	std::size_t min_frame_bytes = dst_strides.row * rows;
	if (dst_planar == planar::planar) {
		min_frame_bytes *= samples_per_pixel;
	}
	if (dst_strides.frame < min_frame_bytes) {
		diag::error_and_throw(
		    "pixel::decode_into file={} reason=frame stride too small (need>={}, got={})",
		    df.path(), min_frame_bytes, dst_strides.frame);
	}
	if (dst.size() < dst_strides.frame) {
		diag::error_and_throw(
		    "pixel::decode_into file={} reason=destination too small (need={}, got={})",
		    df.path(), dst_strides.frame, dst.size());
	}
}

jpegls_frame_buffer load_jpegls_frame_buffer(const DicomFile& df, std::size_t frame_index) {
	const auto& ds = df.dataset();
	const auto& pixel_data = ds["PixelData"_tag];
	if (!pixel_data || !pixel_data.vr().is_pixel_sequence()) {
		diag::error_and_throw(
		    "pixel::decode_into file={} reason=JPEG-LS requires encapsulated PixelData",
		    df.path());
	}

	const auto* pixel_sequence = pixel_data.as_pixel_sequence();
	if (!pixel_sequence) {
		diag::error_and_throw(
		    "pixel::decode_into file={} reason=JPEG-LS pixel sequence is missing",
		    df.path());
	}

	const auto frame_count = pixel_sequence->number_of_frames();
	if (frame_index >= frame_count) {
		diag::error_and_throw(
		    "pixel::decode_into file={} frame={} reason=JPEG-LS frame index out of range (frames={})",
		    df.path(), frame_index, frame_count);
	}

	const auto* frame = pixel_sequence->frame(frame_index);
	if (!frame) {
		diag::error_and_throw(
		    "pixel::decode_into file={} frame={} reason=JPEG-LS frame is missing",
		    df.path(), frame_index);
	}

	jpegls_frame_buffer source{};
	if (frame->encoded_data_size() != 0) {
		source.view = frame->encoded_data_view();
		return source;
	}

	const auto& fragments = frame->fragments();
	if (fragments.empty()) {
		diag::error_and_throw(
		    "pixel::decode_into file={} frame={} reason=JPEG-LS frame has no fragments",
		    df.path(), frame_index);
	}
	for (const auto& fragment : fragments) {
		if (fragment.length == 0) {
			diag::error_and_throw(
			    "pixel::decode_into file={} frame={} reason=JPEG-LS zero-length fragment is not supported",
				    df.path(), frame_index);
		}
	}

	const auto* stream = pixel_sequence->stream();
	if (!stream) {
		diag::error_and_throw(
		    "pixel::decode_into file={} frame={} reason=JPEG-LS pixel sequence stream is missing",
		    df.path(), frame_index);
	}

	if (fragments.size() == 1) {
		const auto fragment = fragments.front();
		source.view = stream->get_span(fragment.offset, fragment.length);
		return source;
	}

	source.owned = frame->coalesce_encoded_data(*stream);
	source.view = std::span<const std::uint8_t>(source.owned);
	return source;
}

void validate_decoded_header(const DicomFile& df, std::size_t frame_index,
    const DicomFile::pixel_info_t& info, const charls::frame_info& frame_info,
    std::size_t rows, std::size_t cols, std::size_t samples_per_pixel,
    std::size_t src_bytes_per_sample) {
	if (frame_info.height != rows || frame_info.width != cols) {
		diag::error_and_throw(
		    "pixel::decode_into file={} frame={} reason=JPEG-LS decoded dimensions mismatch (decoded={}x{}, expected={}x{})",
		    df.path(), frame_index, frame_info.height, frame_info.width, rows, cols);
	}
	if (frame_info.component_count != static_cast<int>(samples_per_pixel)) {
		diag::error_and_throw(
		    "pixel::decode_into file={} frame={} reason=JPEG-LS component count mismatch (decoded={}, expected={})",
		    df.path(), frame_index, frame_info.component_count, samples_per_pixel);
	}

	if (frame_info.bits_per_sample <= 0 || frame_info.bits_per_sample > 16) {
		diag::error_and_throw(
		    "pixel::decode_into file={} frame={} reason=JPEG-LS decoded bits-per-sample is invalid ({})",
		    df.path(), frame_index, frame_info.bits_per_sample);
	}

	const auto max_output_bits = static_cast<int>(src_bytes_per_sample * 8);
	if (frame_info.bits_per_sample > max_output_bits) {
		diag::error_and_throw(
		    "pixel::decode_into file={} frame={} reason=JPEG-LS decoded precision {} exceeds output {} bits",
		    df.path(), frame_index, frame_info.bits_per_sample, max_output_bits);
	}

	if (info.bits_allocated > 0 && frame_info.bits_per_sample > info.bits_allocated) {
		diag::error_and_throw(
		    "pixel::decode_into file={} frame={} reason=JPEG-LS decoded precision {} exceeds BitsAllocated {}",
		    df.path(), frame_index, frame_info.bits_per_sample, info.bits_allocated);
	}
}

std::uint32_t checked_u32_stride(const DicomFile& df, const char* path_name, std::size_t stride) {
	if (stride > std::numeric_limits<std::uint32_t>::max()) {
		diag::error_and_throw(
		    "pixel::decode_into file={} reason={} stride exceeds uint32_t ({})",
		    df.path(), path_name, stride);
	}
	return static_cast<std::uint32_t>(stride);
}

} // namespace

void decode_jpegls_into(const DicomFile& df, const DicomFile::pixel_info_t& info,
    std::size_t frame_index, std::span<std::uint8_t> dst,
    const strides& dst_strides, const decode_opts& opt) {
	if (!info.has_pixel_data) {
		diag::error_and_throw(
		    "pixel::decode_into file={} reason=sv_dtype is unknown", df.path());
	}
	if (!info.ts.is_jpegls()) {
		diag::error_and_throw(
		    "pixel::decode_into file={} reason=transfer syntax is not JPEG-LS ({})",
		    df.path(), df.transfer_syntax_uid().value());
	}
	if (info.rows <= 0 || info.cols <= 0 || info.samples_per_pixel <= 0) {
		diag::error_and_throw(
		    "pixel::decode_into file={} reason=invalid Rows/Columns/SamplesPerPixel",
		    df.path());
	}
	if (info.frames <= 0) {
		diag::error_and_throw(
		    "pixel::decode_into file={} reason=invalid NumberOfFrames",
		    df.path());
	}

	const auto samples_per_pixel_value = info.samples_per_pixel;
	if (samples_per_pixel_value != 1 && samples_per_pixel_value != 3 &&
	    samples_per_pixel_value != 4) {
		diag::error_and_throw(
		    "pixel::decode_into file={} reason=only SamplesPerPixel=1/3/4 is supported in current JPEG-LS path",
		    df.path());
	}
	const auto samples_per_pixel = static_cast<std::size_t>(samples_per_pixel_value);
	if (opt.scaled && samples_per_pixel != 1) {
		diag::error_and_throw(
		    "pixel::decode_into file={} reason=scaled output supports SamplesPerPixel=1 only",
		    df.path());
	}
	if (!sv_dtype_is_integral(info.sv_dtype)) {
		diag::error_and_throw(
		    "pixel::decode_into file={} reason=JPEG-LS supports integral sv_dtype only",
		    df.path());
	}

	const auto src_bytes_per_sample = sv_dtype_bytes(info.sv_dtype);
	if (src_bytes_per_sample == 0 || src_bytes_per_sample > 2) {
		diag::error_and_throw(
		    "pixel::decode_into file={} reason=JPEG-LS supports integral sv_dtype up to 16-bit only",
		    df.path());
	}

	const auto frame_count = static_cast<std::size_t>(info.frames);
	if (frame_index >= frame_count) {
		diag::error_and_throw(
		    "pixel::decode_into file={} frame={} reason=frame index out of range (frames={})",
		    df.path(), frame_index, frame_count);
	}

	const auto rows = static_cast<std::size_t>(info.rows);
	const auto cols = static_cast<std::size_t>(info.cols);
	const auto dst_bytes_per_sample = opt.scaled ? sizeof(float) : src_bytes_per_sample;
	validate_destination(
	    df, dst, dst_strides, opt.planar_out, rows, cols, samples_per_pixel, dst_bytes_per_sample);

	const auto frame_source = load_jpegls_frame_buffer(df, frame_index);
	if (frame_source.view.empty()) {
		diag::error_and_throw(
		    "pixel::decode_into file={} frame={} reason=JPEG-LS frame has empty codestream",
		    df.path(), frame_index);
	}

	charls::jpegls_decoder decoder{};
	charls::frame_info frame_info{};
	charls::interleave_mode interleave_mode{};
	try {
		decoder.source(frame_source.view.data(), frame_source.view.size());
		decoder.read_header();
		frame_info = decoder.frame_info();
		interleave_mode = decoder.interleave_mode();
	} catch (const std::exception& e) {
		diag::error_and_throw(
		    "pixel::decode_into file={} frame={} reason=JPEG-LS header decode failed ({})",
		    df.path(), frame_index, e.what());
	}

	validate_decoded_header(
	    df, frame_index, info, frame_info, rows, cols, samples_per_pixel, src_bytes_per_sample);

	planar src_planar = planar::interleaved;
	switch (interleave_mode) {
	case charls::interleave_mode::none:
		src_planar = planar::planar;
		break;
	case charls::interleave_mode::line:
	case charls::interleave_mode::sample:
		src_planar = planar::interleaved;
		break;
	default:
		diag::error_and_throw(
		    "pixel::decode_into file={} frame={} reason=JPEG-LS reported unsupported interleave mode {}",
		    df.path(), frame_index, static_cast<int>(interleave_mode));
		return;
	}

	// Decode directly into destination when no layout/value transform is needed.
	if (!opt.scaled && (samples_per_pixel == 1 || src_planar == opt.planar_out)) {
		const auto decode_stride = checked_u32_stride(df, "destination", dst_strides.row);
		try {
			decoder.decode(dst.data(), dst_strides.frame, decode_stride);
		} catch (const std::exception& e) {
			diag::error_and_throw(
			    "pixel::decode_into file={} frame={} reason=JPEG-LS decode failed ({})",
			    df.path(), frame_index, e.what());
		}
		return;
	}

	const std::size_t src_row_components =
	    (src_planar == planar::interleaved) ? samples_per_pixel : std::size_t{1};
	const std::size_t src_row_bytes = cols * src_row_components * src_bytes_per_sample;
	std::size_t src_frame_bytes = src_row_bytes * rows;
	if (src_planar == planar::planar) {
		src_frame_bytes *= samples_per_pixel;
	}

	std::vector<std::uint8_t> decoded(src_frame_bytes);
	const auto decode_stride = checked_u32_stride(df, "source", src_row_bytes);
	try {
		decoder.decode(decoded.data(), decoded.size(), decode_stride);
	} catch (const std::exception& e) {
		diag::error_and_throw(
		    "pixel::decode_into file={} frame={} reason=JPEG-LS decode failed ({})",
		    df.path(), frame_index, e.what());
	}

	if (opt.scaled) {
		decode_mono_scaled_into_f32(
		    df, info, decoded.data(), dst, dst_strides, rows, cols, src_row_bytes);
		return;
	}

	const auto transform = select_planar_transform(src_planar, opt.planar_out);
	run_planar_transform_copy(transform, src_bytes_per_sample, false,
	    decoded.data(), dst.data(), rows, cols, samples_per_pixel,
	    src_row_bytes, dst_strides.row);
}

} // namespace pixel::detail
} // namespace dicom
