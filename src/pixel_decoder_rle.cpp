#include "pixel_decoder_detail.hpp"

#include "dicom_endian.h"
#include "diagnostics.h"

#include <array>
#include <cstring>

using namespace dicom::literals;
namespace diag = dicom::diag;

namespace dicom {
namespace pixel::detail {

namespace {

struct rle_frame_buffer {
	std::vector<std::uint8_t> owned{};
	std::span<const std::uint8_t> view{};
};

struct rle_header {
	std::size_t segment_count{0};
	std::array<std::uint32_t, 15> offsets{};
};

rle_frame_buffer load_rle_frame_buffer(const DicomFile& df, std::size_t frame_index) {
	const auto& ds = df.dataset();
	const auto& pixel_data = ds["PixelData"_tag];
	if (!pixel_data || !pixel_data.vr().is_pixel_sequence()) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=RLE requires encapsulated PixelData",
		    df.path());
	}

	const auto* pixel_sequence = pixel_data.as_pixel_sequence();
	if (!pixel_sequence) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=RLE pixel sequence is missing",
		    df.path());
	}

	const auto frame_count = pixel_sequence->number_of_frames();
	if (frame_index >= frame_count) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} frame={} reason=RLE frame index out of range (frames={})",
		    df.path(), frame_index, frame_count);
	}

	const auto* frame = pixel_sequence->frame(frame_index);
	if (!frame) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} frame={} reason=RLE frame is missing",
		    df.path(), frame_index);
	}

	rle_frame_buffer source{};
	if (frame->encoded_data_size() != 0) {
		source.view = frame->encoded_data_view();
		return source;
	}

	const auto& fragments = frame->fragments();
	if (fragments.empty()) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} frame={} reason=RLE frame has no fragments",
		    df.path(), frame_index);
	}

	const auto* stream = pixel_sequence->stream();
	if (!stream) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} frame={} reason=RLE pixel sequence stream is missing",
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

rle_header parse_rle_header(const DicomFile& df, std::size_t frame_index,
    std::span<const std::uint8_t> encoded_frame) {
	if (encoded_frame.size() < 64) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} frame={} reason=RLE frame is shorter than 64-byte header",
		    df.path(), frame_index);
	}

	rle_header header{};
	header.segment_count = endian::load_le<std::uint32_t>(encoded_frame.data());
	if (header.segment_count == 0 || header.segment_count > header.offsets.size()) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} frame={} reason=invalid RLE segment count {}",
		    df.path(), frame_index, header.segment_count);
	}

	for (std::size_t i = 0; i < header.segment_count; ++i) {
		header.offsets[i] = endian::load_le<std::uint32_t>(
		    encoded_frame.data() + 4 + i * sizeof(std::uint32_t));
	}

	for (std::size_t i = 0; i < header.segment_count; ++i) {
		const auto start = static_cast<std::size_t>(header.offsets[i]);
		if (start < 64 || start >= encoded_frame.size()) {
			diag::error_and_throw(
			    "pixel::decode_frame_into file={} frame={} reason=invalid RLE segment {} offset {}",
			    df.path(), frame_index, i, start);
		}
		const auto end = (i + 1 < header.segment_count)
		                     ? static_cast<std::size_t>(header.offsets[i + 1])
		                     : encoded_frame.size();
		if (end < start || end > encoded_frame.size()) {
			diag::error_and_throw(
			    "pixel::decode_frame_into file={} frame={} reason=invalid RLE segment {} range [{}, {})",
			    df.path(), frame_index, i, start, end);
		}
	}

	return header;
}

void decode_rle_packbits_segment(const DicomFile& df, std::size_t frame_index,
    std::size_t segment_index, std::span<const std::uint8_t> encoded,
    std::span<std::uint8_t> decoded) {
	std::size_t in = 0;
	std::size_t out = 0;

	while (out < decoded.size()) {
		if (in >= encoded.size()) {
			diag::error_and_throw(
			    "pixel::decode_frame_into file={} frame={} reason=RLE segment {} ended early (decoded={}/{})",
			    df.path(), frame_index, segment_index, out, decoded.size());
		}

		const auto control = static_cast<std::int8_t>(encoded[in++]);
		if (control >= 0) {
			const auto literal_count = static_cast<std::size_t>(control) + 1;
			if (in + literal_count > encoded.size() || out + literal_count > decoded.size()) {
				diag::error_and_throw(
				    "pixel::decode_frame_into file={} frame={} reason=RLE segment {} literal run out of bounds",
				    df.path(), frame_index, segment_index);
			}
			std::memcpy(decoded.data() + out, encoded.data() + in, literal_count);
			in += literal_count;
			out += literal_count;
			continue;
		}

		if (control >= -127) {
			const auto repeat_count = static_cast<std::size_t>(1 - control);
			if (in >= encoded.size() || out + repeat_count > decoded.size()) {
				diag::error_and_throw(
				    "pixel::decode_frame_into file={} frame={} reason=RLE segment {} repeat run out of bounds",
				    df.path(), frame_index, segment_index);
			}
			std::memset(decoded.data() + out, encoded[in], repeat_count);
			++in;
			out += repeat_count;
			continue;
		}

		// control == -128 : no-op
	}
}

std::vector<std::uint8_t> decode_rle_frame_to_planar(const DicomFile& df, std::size_t frame_index,
    std::span<const std::uint8_t> encoded_frame, std::size_t rows, std::size_t cols,
    std::size_t samples_per_pixel, std::size_t bytes_per_sample) {
	const auto expected_segments = samples_per_pixel * bytes_per_sample;
	if (expected_segments == 0 || expected_segments > 15) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} frame={} reason=unsupported RLE segment layout (spp={}, bytes_per_sample={})",
		    df.path(), frame_index, samples_per_pixel, bytes_per_sample);
	}

	const auto header = parse_rle_header(df, frame_index, encoded_frame);
	if (header.segment_count < expected_segments) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} frame={} reason=RLE segment count {} is smaller than expected {}",
		    df.path(), frame_index, header.segment_count, expected_segments);
	}

	const auto pixels_per_plane = rows * cols;
	const auto src_row_bytes = cols * bytes_per_sample;
	const auto src_plane_bytes = src_row_bytes * rows;
	std::vector<std::uint8_t> decoded_planar(src_plane_bytes * samples_per_pixel, 0);
	std::vector<std::uint8_t> decoded_byte_plane(pixels_per_plane, 0);

	for (std::size_t sample = 0; sample < samples_per_pixel; ++sample) {
		auto* plane_base = decoded_planar.data() + sample * src_plane_bytes;
		for (std::size_t byte_plane = 0; byte_plane < bytes_per_sample; ++byte_plane) {
			const auto segment_index = sample * bytes_per_sample + byte_plane;
			const auto segment_start = static_cast<std::size_t>(header.offsets[segment_index]);
			const auto segment_end = (segment_index + 1 < header.segment_count)
			                             ? static_cast<std::size_t>(header.offsets[segment_index + 1])
			                             : encoded_frame.size();
			const auto segment_size = segment_end - segment_start;
			const auto segment_data = encoded_frame.subspan(segment_start, segment_size);

			decode_rle_packbits_segment(df, frame_index, segment_index, segment_data,
			    std::span<std::uint8_t>(decoded_byte_plane));

			const auto byte_offset = bytes_per_sample - 1 - byte_plane;
			for (std::size_t r = 0; r < rows; ++r) {
				const auto* src_row = decoded_byte_plane.data() + r * cols;
				auto* dst_row = plane_base + r * src_row_bytes + byte_offset;
				for (std::size_t c = 0; c < cols; ++c) {
					dst_row[c * bytes_per_sample] = src_row[c];
				}
			}
		}
	}

	return decoded_planar;
}

} // namespace

void decode_rle_into(const DicomFile& df, const DicomFile::pixel_info_t& info,
    std::size_t frame_index, std::span<std::uint8_t> dst,
    const DecodeStrides& dst_strides, const DecodeOptions& opt) {
	if (!info.has_pixel_data) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=sv_dtype is unknown", df.path());
	}

	if (info.rows <= 0 || info.cols <= 0 || info.samples_per_pixel <= 0) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=invalid Rows/Columns/SamplesPerPixel",
		    df.path());
	}

	const auto samples_per_pixel_value = info.samples_per_pixel;
	if (samples_per_pixel_value != 1 && samples_per_pixel_value != 3 && samples_per_pixel_value != 4) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=only SamplesPerPixel=1/3/4 is supported in current RLE path",
		    df.path());
	}
	const auto samples_per_pixel = static_cast<std::size_t>(samples_per_pixel_value);
	if (opt.scaled && samples_per_pixel != 1) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=scaled output supports SamplesPerPixel=1 only",
		    df.path());
	}

	const auto src_bytes_per_sample = sv_dtype_bytes(info.sv_dtype);
	if (src_bytes_per_sample == 0) {
		diag::error_and_throw(
		    "pixel::decode_frame_into file={} reason=only sv_dtype=u8/s8/u16/s16/u32/s32/f32/f64 is supported in current RLE path",
		    df.path());
	}

	const auto rows = static_cast<std::size_t>(info.rows);
	const auto cols = static_cast<std::size_t>(info.cols);
	const auto dst_bytes_per_sample = opt.scaled ? sizeof(float) : src_bytes_per_sample;

	const auto dst_planar = opt.planar_out;
	const std::size_t dst_row_components =
	    (dst_planar == Planar::interleaved) ? samples_per_pixel : std::size_t{1};
	const std::size_t dst_min_row_bytes = cols * dst_row_components * dst_bytes_per_sample;
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

	if (info.frames > 0) {
		const auto declared_frames = static_cast<std::size_t>(info.frames);
		if (frame_index >= declared_frames) {
			diag::error_and_throw(
			    "pixel::decode_frame_into file={} frame={} reason=frame index out of range (frames={})",
			    df.path(), frame_index, declared_frames);
		}
	}

	const auto rle_source = load_rle_frame_buffer(df, frame_index);
	const auto decoded_planar = decode_rle_frame_to_planar(
	    df, frame_index, rle_source.view, rows, cols, samples_per_pixel, src_bytes_per_sample);
	const auto src_row_bytes = cols * src_bytes_per_sample;

	if (opt.scaled) {
		decode_mono_scaled_into_f32(
		    df, info, decoded_planar.data(), dst, dst_strides, rows, cols, src_row_bytes);
		return;
	}

	const auto transform = select_planar_transform(Planar::planar, dst_planar);
	const bool needs_swap = (src_bytes_per_sample > 1) && !endian::host_is_little_endian();
	run_planar_transform_copy(transform, src_bytes_per_sample, needs_swap,
	    decoded_planar.data(), dst.data(), rows, cols, samples_per_pixel,
	    src_row_bytes, dst_strides.row);
}

} // namespace pixel::detail
} // namespace dicom
