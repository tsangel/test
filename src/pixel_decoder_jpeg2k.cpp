#include "pixel_decoder_detail.hpp"

#include "diagnostics.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>
#include <openjpeg.h>

using namespace dicom::literals;
namespace diag = dicom::diag;

namespace dicom {
namespace pixel::detail {

namespace {

struct jpeg2k_frame_source {
	const PixelFrame* frame{nullptr};
	const InStream* stream{nullptr};
	std::span<const std::uint8_t> contiguous{};
	const std::vector<PixelFragment>* fragments{nullptr};
	std::vector<std::size_t> fragment_offsets{};
	std::size_t total_size{0};
	std::size_t position{0};
};

struct openjpeg_log_sink {
	std::string warning{};
	std::string error{};
};

class opj_stream_deleter {
public:
	void operator()(opj_stream_t* stream) const noexcept {
		if (stream) {
			opj_stream_destroy(stream);
		}
	}
};

class opj_codec_deleter {
public:
	void operator()(opj_codec_t* codec) const noexcept {
		if (codec) {
			opj_destroy_codec(codec);
		}
	}
};

class opj_image_deleter {
public:
	void operator()(opj_image_t* image) const noexcept {
		if (image) {
			opj_image_destroy(image);
		}
	}
};

using opj_stream_ptr = std::unique_ptr<opj_stream_t, opj_stream_deleter>;
using opj_codec_ptr = std::unique_ptr<opj_codec_t, opj_codec_deleter>;
using opj_image_ptr = std::unique_ptr<opj_image_t, opj_image_deleter>;

bool sv_dtype_is_signed(dtype sv_dtype) noexcept {
	switch (sv_dtype) {
	case dtype::s8:
	case dtype::s16:
	case dtype::s32:
		return true;
	default:
		return false;
	}
}

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

const char* openjpeg_format_name(OPJ_CODEC_FORMAT format) noexcept {
	return (format == OPJ_CODEC_JP2) ? "JP2" : "J2K";
}

std::string trimmed_message(std::string message) {
	while (!message.empty() &&
	       (message.back() == '\n' || message.back() == '\r' || message.back() == ' ' ||
	           message.back() == '\t')) {
		message.pop_back();
	}
	return message;
}

std::size_t fragment_index_for_position(const jpeg2k_frame_source& source, std::size_t position) {
	const auto& offsets = source.fragment_offsets;
	const auto it = std::upper_bound(offsets.begin(), offsets.end(), position);
	if (it == offsets.begin()) {
		return 0;
	}
	return static_cast<std::size_t>(std::distance(offsets.begin(), it) - 1);
}

OPJ_SIZE_T OPJ_CALLCONV opj_read_from_frame_stream(void* p_buffer, OPJ_SIZE_T p_nb_bytes,
    void* p_user_data) {
	auto* source = static_cast<jpeg2k_frame_source*>(p_user_data);
	if (!source || p_nb_bytes == 0) {
		return 0;
	}
	if (source->position >= source->total_size) {
		return static_cast<OPJ_SIZE_T>(-1);
	}

	const std::size_t remaining = source->total_size - source->position;
	const std::size_t requested = static_cast<std::size_t>(p_nb_bytes);
	const std::size_t to_copy = std::min(remaining, requested);
	if (to_copy == 0) {
		return static_cast<OPJ_SIZE_T>(-1);
	}

	auto* out = static_cast<std::uint8_t*>(p_buffer);
	if (!source->contiguous.empty()) {
		std::memcpy(out, source->contiguous.data() + source->position, to_copy);
		source->position += to_copy;
		return static_cast<OPJ_SIZE_T>(to_copy);
	}

	try {
		std::size_t copied = 0;
		while (copied < to_copy) {
			const auto frag_index = fragment_index_for_position(*source, source->position);
			const auto& frag = (*source->fragments)[frag_index];
			const auto frag_start = source->fragment_offsets[frag_index];
			const auto frag_offset = source->position - frag_start;
			const auto frag_remaining = frag.length - frag_offset;
			const auto chunk = std::min(to_copy - copied, frag_remaining);
			const auto chunk_span = source->stream->get_span(frag.offset + frag_offset, chunk);
			std::memcpy(out + copied, chunk_span.data(), chunk);
			copied += chunk;
			source->position += chunk;
		}
		return static_cast<OPJ_SIZE_T>(to_copy);
	} catch (...) {
		return static_cast<OPJ_SIZE_T>(-1);
	}
}

OPJ_OFF_T OPJ_CALLCONV opj_skip_in_frame_stream(OPJ_OFF_T p_nb_bytes, void* p_user_data) {
	auto* source = static_cast<jpeg2k_frame_source*>(p_user_data);
	if (!source || p_nb_bytes < 0) {
		return static_cast<OPJ_OFF_T>(-1);
	}
	const auto requested = static_cast<std::size_t>(p_nb_bytes);
	const auto remaining = source->total_size - source->position;
	const auto skipped = std::min(requested, remaining);
	source->position += skipped;
	return static_cast<OPJ_OFF_T>(skipped);
}

OPJ_BOOL OPJ_CALLCONV opj_seek_in_frame_stream(OPJ_OFF_T p_nb_bytes, void* p_user_data) {
	auto* source = static_cast<jpeg2k_frame_source*>(p_user_data);
	if (!source || p_nb_bytes < 0) {
		return OPJ_FALSE;
	}
	const auto target = static_cast<std::size_t>(p_nb_bytes);
	if (target > source->total_size) {
		return OPJ_FALSE;
	}
	source->position = target;
	return OPJ_TRUE;
}

void OPJ_CALLCONV opj_warning_handler(const char* message, void* user_data) {
	auto* sink = static_cast<openjpeg_log_sink*>(user_data);
	if (!sink || !message) {
		return;
	}
	sink->warning.append(message);
}

void OPJ_CALLCONV opj_error_handler(const char* message, void* user_data) {
	auto* sink = static_cast<openjpeg_log_sink*>(user_data);
	if (!sink || !message) {
		return;
	}
	sink->error.append(message);
}

bool frame_looks_like_jp2(const jpeg2k_frame_source& source) {
	constexpr std::array<std::uint8_t, 12> kJp2Signature = {
	    0x00, 0x00, 0x00, 0x0C, 0x6A, 0x50, 0x20, 0x20, 0x0D, 0x0A, 0x87, 0x0A};
	if (source.total_size < kJp2Signature.size()) {
		return false;
	}

	std::array<std::uint8_t, 12> prefix{};
	if (!source.contiguous.empty()) {
		std::memcpy(prefix.data(), source.contiguous.data(), prefix.size());
		return prefix == kJp2Signature;
	}

	try {
		std::size_t copied = 0;
		for (const auto& frag : *source.fragments) {
			if (copied >= prefix.size()) {
				break;
			}
			const auto chunk = std::min(prefix.size() - copied, frag.length);
			if (chunk == 0) {
				continue;
			}
			const auto span = source.stream->get_span(frag.offset, chunk);
			std::memcpy(prefix.data() + copied, span.data(), chunk);
			copied += chunk;
		}
		if (copied < prefix.size()) {
			return false;
		}
		return prefix == kJp2Signature;
	} catch (...) {
		return false;
	}
}

std::string decode_failure_message(OPJ_CODEC_FORMAT format, const openjpeg_log_sink& sink,
    std::string_view fallback) {
	const auto err = trimmed_message(sink.error);
	if (!err.empty()) {
		return err;
	}
	const auto warn = trimmed_message(sink.warning);
	if (!warn.empty()) {
		return warn;
	}
	return fmt::format("{} decode failed ({})", openjpeg_format_name(format), fallback);
}

OPJ_UINT32 resolve_openjpeg_thread_count(const DataSet& ds, const decode_opts& opt) {
	int configured_threads = opt.decoder_threads;
	if (configured_threads < -1) {
		diag::error_and_throw(
		    "pixel::decode_into file={} reason=invalid decoder_threads {} (expected -1, 0, or positive)",
		    ds.path(), configured_threads);
	}
	if (configured_threads == 0) {
		return 0;
	}
	if (configured_threads == -1) {
		configured_threads = opj_get_num_cpus();
	}
	if (configured_threads <= 0) {
		return 0;
	}
	if (static_cast<unsigned long long>(configured_threads) >
	    static_cast<unsigned long long>(std::numeric_limits<OPJ_UINT32>::max())) {
		diag::error_and_throw(
		    "pixel::decode_into file={} reason=decoder_threads {} exceeds OpenJPEG limit {}",
		    ds.path(), configured_threads, std::numeric_limits<OPJ_UINT32>::max());
	}
	return static_cast<OPJ_UINT32>(configured_threads);
}

jpeg2k_frame_source load_jpeg2k_frame_source(const DataSet& ds, std::size_t frame_index) {
	const auto& pixel_data = ds["PixelData"_tag];
	if (!pixel_data || !pixel_data.vr().is_pixel_sequence()) {
		diag::error_and_throw(
		    "pixel::decode_into file={} reason=JPEG2000 requires encapsulated PixelData",
		    ds.path());
	}

	const auto* pixel_sequence = pixel_data.as_pixel_sequence();
	if (!pixel_sequence) {
		diag::error_and_throw(
		    "pixel::decode_into file={} reason=JPEG2000 pixel sequence is missing",
		    ds.path());
	}

	const auto frame_count = pixel_sequence->number_of_frames();
	if (frame_index >= frame_count) {
		diag::error_and_throw(
		    "pixel::decode_into file={} frame={} reason=JPEG2000 frame index out of range (frames={})",
		    ds.path(), frame_index, frame_count);
	}

	const auto* frame = pixel_sequence->frame(frame_index);
	if (!frame) {
		diag::error_and_throw(
		    "pixel::decode_into file={} frame={} reason=JPEG2000 frame is missing",
		    ds.path(), frame_index);
	}

	jpeg2k_frame_source source{};
	source.frame = frame;
	if (frame->encoded_data_size() != 0) {
		source.contiguous = frame->encoded_data_view();
		source.total_size = source.contiguous.size();
		return source;
	}

	const auto& fragments = frame->fragments();
	if (fragments.empty()) {
		diag::error_and_throw(
		    "pixel::decode_into file={} frame={} reason=JPEG2000 frame has no fragments",
		    ds.path(), frame_index);
	}
	for (const auto& fragment : fragments) {
		if (fragment.length == 0) {
			diag::error_and_throw(
			    "pixel::decode_into file={} frame={} reason=JPEG2000 zero-length fragment is not supported",
			    ds.path(), frame_index);
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
		    "pixel::decode_into file={} frame={} reason=JPEG2000 pixel sequence stream is missing",
		    ds.path(), frame_index);
	}
	source.stream = stream;

	if (fragments.size() == 1) {
		const auto& frag = fragments.front();
		source.contiguous = stream->get_span(frag.offset, frag.length);
		source.total_size = source.contiguous.size();
		return source;
	}

	source.fragments = &fragments;
	source.fragment_offsets.reserve(fragments.size());
	std::size_t total_size = 0;
	for (const auto& frag : fragments) {
		if (frag.length == 0) {
			diag::error_and_throw(
			    "pixel::decode_into file={} frame={} reason=JPEG2000 zero-length fragment is not supported",
			    ds.path(), frame_index);
		}
		source.fragment_offsets.push_back(total_size);
		if (frag.length > std::numeric_limits<std::size_t>::max() - total_size) {
			diag::error_and_throw(
			    "pixel::decode_into file={} frame={} reason=JPEG2000 frame size overflow",
			    ds.path(), frame_index);
		}
		total_size += frag.length;
	}
	source.total_size = total_size;
	return source;
}

opj_stream_ptr create_openjpeg_stream(jpeg2k_frame_source& source) {
	constexpr OPJ_SIZE_T kStreamBufferSize = 64 * 1024;
	opj_stream_ptr stream(opj_stream_create(kStreamBufferSize, OPJ_STREAM_READ));
	if (!stream) {
		return {};
	}
	source.position = 0;
	opj_stream_set_user_data(stream.get(), &source, nullptr);
	opj_stream_set_user_data_length(stream.get(), static_cast<OPJ_UINT64>(source.total_size));
	opj_stream_set_read_function(stream.get(), opj_read_from_frame_stream);
	opj_stream_set_skip_function(stream.get(), opj_skip_in_frame_stream);
	opj_stream_set_seek_function(stream.get(), opj_seek_in_frame_stream);
	return stream;
}

opj_image_ptr decode_openjpeg_image_with_format(const DataSet& ds,
    jpeg2k_frame_source& source, OPJ_CODEC_FORMAT format, const decode_opts& opt,
    std::string& failure) {
	opj_dparameters_t parameters{};
	opj_set_default_decoder_parameters(&parameters);

	opj_codec_ptr codec(opj_create_decompress(format));
	if (!codec) {
		failure = fmt::format("{} codec creation failed", openjpeg_format_name(format));
		return {};
	}

	openjpeg_log_sink sink{};
	opj_set_warning_handler(codec.get(), opj_warning_handler, &sink);
	opj_set_error_handler(codec.get(), opj_error_handler, &sink);

	if (!opj_setup_decoder(codec.get(), &parameters)) {
		failure = decode_failure_message(format, sink, "setup");
		return {};
	}
	const auto thread_count = resolve_openjpeg_thread_count(ds, opt);
	if (thread_count > 0 && !opj_codec_set_threads(codec.get(), thread_count)) {
		failure = fmt::format(
		    "{} failed to set decode threads ({})", openjpeg_format_name(format), thread_count);
		return {};
	}

	opj_stream_ptr stream = create_openjpeg_stream(source);
	if (!stream) {
		failure = fmt::format("{} stream creation failed", openjpeg_format_name(format));
		return {};
	}

	opj_image_t* raw_image = nullptr;
	if (!opj_read_header(stream.get(), codec.get(), &raw_image)) {
		if (raw_image) {
			opj_image_destroy(raw_image);
		}
		failure = decode_failure_message(format, sink, "read_header");
		return {};
	}

	opj_image_ptr image(raw_image);
	if (!image) {
		failure = fmt::format("{} read_header returned null image", openjpeg_format_name(format));
		return {};
	}
	if (!opj_decode(codec.get(), stream.get(), image.get())) {
		failure = decode_failure_message(format, sink, "decode");
		return {};
	}
	if (!opj_end_decompress(codec.get(), stream.get())) {
		failure = decode_failure_message(format, sink, "end_decompress");
		return {};
	}
	return image;
}

opj_image_ptr decode_openjpeg_image(const DataSet& ds, std::size_t frame_index,
    jpeg2k_frame_source& source, const decode_opts& opt) {
	const auto prefer_jp2 = frame_looks_like_jp2(source);

	std::string first_failure{};
	std::string second_failure{};
	const auto first_format = prefer_jp2 ? OPJ_CODEC_JP2 : OPJ_CODEC_J2K;
	const auto second_format = prefer_jp2 ? OPJ_CODEC_J2K : OPJ_CODEC_JP2;

	if (auto image = decode_openjpeg_image_with_format(
	        ds, source, first_format, opt, first_failure)) {
		return image;
	}
	if (auto image = decode_openjpeg_image_with_format(
	        ds, source, second_format, opt, second_failure)) {
		return image;
	}

	diag::error_and_throw(
	    "pixel::decode_into file={} frame={} reason=JPEG2000 decode failed ({}: {}; {}: {})",
	    ds.path(), frame_index,
	    openjpeg_format_name(first_format), trimmed_message(first_failure),
	    openjpeg_format_name(second_format), trimmed_message(second_failure));
	return {};
}

void validate_destination(const DataSet& ds, std::span<std::uint8_t> dst,
    const strides& dst_strides, planar dst_planar, std::size_t rows, std::size_t cols,
    std::size_t samples_per_pixel, std::size_t bytes_per_sample) {
	const std::size_t dst_row_components =
	    (dst_planar == planar::interleaved) ? samples_per_pixel : std::size_t{1};
	const std::size_t dst_min_row_bytes = cols * dst_row_components * bytes_per_sample;
	if (dst_strides.row < dst_min_row_bytes) {
		diag::error_and_throw(
		    "pixel::decode_into file={} reason=row stride too small (need>={}, got={})",
		    ds.path(), dst_min_row_bytes, dst_strides.row);
	}

	std::size_t min_frame_bytes = dst_strides.row * rows;
	if (dst_planar == planar::planar) {
		min_frame_bytes *= samples_per_pixel;
	}
	if (dst_strides.frame < min_frame_bytes) {
		diag::error_and_throw(
		    "pixel::decode_into file={} reason=frame stride too small (need>={}, got={})",
		    ds.path(), min_frame_bytes, dst_strides.frame);
	}
	if (dst.size() < dst_strides.frame) {
		diag::error_and_throw(
		    "pixel::decode_into file={} reason=destination too small (need={}, got={})",
		    ds.path(), dst_strides.frame, dst.size());
	}
}

void validate_decoded_image(const DataSet& ds, std::size_t frame_index, const DataSet::pixel_info_t& info,
    const opj_image_t& image, std::size_t rows, std::size_t cols, std::size_t samples_per_pixel) {
	const auto decoded_rows = (image.y1 >= image.y0)
	                              ? static_cast<std::size_t>(image.y1 - image.y0)
	                              : std::size_t{0};
	const auto decoded_cols = (image.x1 >= image.x0)
	                              ? static_cast<std::size_t>(image.x1 - image.x0)
	                              : std::size_t{0};
	if (decoded_rows != rows || decoded_cols != cols) {
		diag::error_and_throw(
		    "pixel::decode_into file={} frame={} reason=JPEG2000 decoded dimensions mismatch (decoded={}x{}, expected={}x{})",
		    ds.path(), frame_index, decoded_rows, decoded_cols, rows, cols);
	}
	if (image.numcomps != samples_per_pixel) {
		diag::error_and_throw(
		    "pixel::decode_into file={} frame={} reason=JPEG2000 component count mismatch (decoded={}, expected={})",
		    ds.path(), frame_index, image.numcomps, samples_per_pixel);
	}

	const auto bytes_per_sample = sv_dtype_bytes(info.sv_dtype);
	if (bytes_per_sample == 0 || bytes_per_sample > 4 || !sv_dtype_is_integral(info.sv_dtype)) {
		diag::error_and_throw(
		    "pixel::decode_into file={} frame={} reason=JPEG2000 supports integral sv_dtype up to 32-bit only",
		    ds.path(), frame_index);
	}

	const bool expected_signed = sv_dtype_is_signed(info.sv_dtype);
	const auto max_precision = static_cast<OPJ_UINT32>(bytes_per_sample * 8);
	if (!expected_signed && max_precision == 32) {
		diag::error_and_throw(
		    "pixel::decode_into file={} frame={} reason=JPEG2000 unsigned 32-bit samples are not supported",
		    ds.path(), frame_index);
	}

	for (std::size_t c = 0; c < samples_per_pixel; ++c) {
		const auto& comp = image.comps[c];
		if (!comp.data) {
			diag::error_and_throw(
			    "pixel::decode_into file={} frame={} reason=JPEG2000 component {} has no decoded data",
			    ds.path(), frame_index, c);
		}
		if (comp.w != cols || comp.h != rows) {
			diag::error_and_throw(
			    "pixel::decode_into file={} frame={} reason=JPEG2000 component {} dimensions mismatch (decoded={}x{}, expected={}x{})",
			    ds.path(), frame_index, c, comp.h, comp.w, rows, cols);
		}
		if (comp.sgnd != static_cast<OPJ_UINT32>(expected_signed ? 1 : 0)) {
			diag::error_and_throw(
			    "pixel::decode_into file={} frame={} reason=JPEG2000 component {} signedness mismatch (decoded={}, expected={})",
			    ds.path(), frame_index, c, comp.sgnd, expected_signed ? 1 : 0);
		}
		if (comp.prec == 0 || comp.prec > max_precision) {
			diag::error_and_throw(
			    "pixel::decode_into file={} frame={} reason=JPEG2000 component {} precision {} exceeds output {} bits",
			    ds.path(), frame_index, c, comp.prec, max_precision);
		}
	}
}

template <typename DstT>
inline void store_value(std::uint8_t* dst, DstT value) {
	std::memcpy(dst, &value, sizeof(DstT));
}

template <typename DstT>
void write_unscaled_to_dst(const opj_image_t& image, std::span<std::uint8_t> dst,
    const strides& dst_strides, planar dst_planar, std::size_t rows, std::size_t cols,
    std::size_t samples_per_pixel) {
	if (dst_planar == planar::planar && samples_per_pixel > 1) {
		const std::size_t plane_bytes = dst_strides.row * rows;
		for (std::size_t comp = 0; comp < samples_per_pixel; ++comp) {
			const auto* src_comp = image.comps[comp].data;
			auto* dst_plane = dst.data() + comp * plane_bytes;
			for (std::size_t r = 0; r < rows; ++r) {
				const auto src_index = r * cols;
				auto* dst_row = dst_plane + r * dst_strides.row;
				for (std::size_t c = 0; c < cols; ++c) {
					const auto sample = static_cast<DstT>(src_comp[src_index + c]);
					store_value(dst_row + c * sizeof(DstT), sample);
				}
			}
		}
		return;
	}

	const std::size_t pixel_stride = samples_per_pixel * sizeof(DstT);
	for (std::size_t r = 0; r < rows; ++r) {
		auto* dst_row = dst.data() + r * dst_strides.row;
		const auto src_index = r * cols;
		for (std::size_t c = 0; c < cols; ++c) {
			auto* dst_pixel = dst_row + c * pixel_stride;
			for (std::size_t comp = 0; comp < samples_per_pixel; ++comp) {
				const auto sample =
				    static_cast<DstT>(image.comps[comp].data[src_index + c]);
				store_value(dst_pixel + comp * sizeof(DstT), sample);
			}
		}
	}
}

template <typename SampleT>
void write_scaled_mono_to_dst(const DataSet& ds, const opj_image_t& image, std::span<std::uint8_t> dst,
    const strides& dst_strides, std::size_t rows, std::size_t cols) {
	const auto* src = image.comps[0].data;
	if (!src) {
		diag::error_and_throw(
		    "pixel::decode_into file={} reason=JPEG2000 decoded component has no data",
		    ds.path());
	}

	const auto modality_lut = ds.modality_lut();
	if (modality_lut) {
		const auto last_index = static_cast<std::int64_t>(modality_lut->values.size() - 1);
		for (std::size_t r = 0; r < rows; ++r) {
			const auto src_index = r * cols;
			auto* dst_row = dst.data() + r * dst_strides.row;
			for (std::size_t c = 0; c < cols; ++c) {
				const auto sv = static_cast<SampleT>(src[src_index + c]);
				std::int64_t lut_index = static_cast<std::int64_t>(sv) - modality_lut->first_mapped;
				if (lut_index < 0) {
					lut_index = 0;
				} else if (lut_index > last_index) {
					lut_index = last_index;
				}
				const auto value = modality_lut->values[static_cast<std::size_t>(lut_index)];
				store_value(dst_row + c * sizeof(float), value);
			}
		}
		return;
	}

	const auto slope = ds["RescaleSlope"_tag].toDouble(1.0);
	const auto intercept = ds["RescaleIntercept"_tag].toDouble(0.0);
	if (!std::isfinite(slope) || !std::isfinite(intercept)) {
		diag::error_and_throw(
		    "pixel::decode_into file={} reason=RescaleSlope/RescaleIntercept must be finite",
		    ds.path());
	}

	for (std::size_t r = 0; r < rows; ++r) {
		const auto src_index = r * cols;
		auto* dst_row = dst.data() + r * dst_strides.row;
		for (std::size_t c = 0; c < cols; ++c) {
			const auto sv = static_cast<SampleT>(src[src_index + c]);
			const auto value = static_cast<float>(static_cast<double>(sv) * slope + intercept);
			store_value(dst_row + c * sizeof(float), value);
		}
	}
}

} // namespace

void decode_jpeg2k_into(const DataSet& ds, const DataSet::pixel_info_t& info,
    std::size_t frame_index, std::span<std::uint8_t> dst,
    const strides& dst_strides, const decode_opts& opt) {
	if (!info.has_pixel_data) {
		diag::error_and_throw(
		    "pixel::decode_into file={} reason=sv_dtype is unknown", ds.path());
	}
	if (!info.ts.is_jpeg2000()) {
		diag::error_and_throw(
		    "pixel::decode_into file={} reason=transfer syntax is not JPEG2000 ({})",
		    ds.path(), ds.transfer_syntax_uid().value());
	}
	if (info.rows <= 0 || info.cols <= 0 || info.samples_per_pixel <= 0) {
		diag::error_and_throw(
		    "pixel::decode_into file={} reason=invalid Rows/Columns/SamplesPerPixel",
		    ds.path());
	}
	if (info.frames <= 0) {
		diag::error_and_throw(
		    "pixel::decode_into file={} reason=invalid NumberOfFrames",
		    ds.path());
	}

	const auto samples_per_pixel_value = info.samples_per_pixel;
	if (samples_per_pixel_value != 1 && samples_per_pixel_value != 3 &&
	    samples_per_pixel_value != 4) {
		diag::error_and_throw(
		    "pixel::decode_into file={} reason=only SamplesPerPixel=1/3/4 is supported in current JPEG2000 path",
		    ds.path());
	}
	const auto samples_per_pixel = static_cast<std::size_t>(samples_per_pixel_value);
	if (opt.scaled && samples_per_pixel != 1) {
		diag::error_and_throw(
		    "pixel::decode_into file={} reason=scaled output supports SamplesPerPixel=1 only",
		    ds.path());
	}
	if (!sv_dtype_is_integral(info.sv_dtype)) {
		diag::error_and_throw(
		    "pixel::decode_into file={} reason=JPEG2000 supports integral sv_dtype only",
		    ds.path());
	}

	const auto src_bytes_per_sample = sv_dtype_bytes(info.sv_dtype);
	if (src_bytes_per_sample == 0 || src_bytes_per_sample > 4) {
		diag::error_and_throw(
		    "pixel::decode_into file={} reason=JPEG2000 supports integral sv_dtype up to 32-bit only",
		    ds.path());
	}

	const auto frame_count = static_cast<std::size_t>(info.frames);
	if (frame_index >= frame_count) {
		diag::error_and_throw(
		    "pixel::decode_into file={} frame={} reason=frame index out of range (frames={})",
		    ds.path(), frame_index, frame_count);
	}

	const auto rows = static_cast<std::size_t>(info.rows);
	const auto cols = static_cast<std::size_t>(info.cols);
	const auto dst_bytes_per_sample = opt.scaled ? sizeof(float) : src_bytes_per_sample;
	validate_destination(
	    ds, dst, dst_strides, opt.planar_out, rows, cols, samples_per_pixel, dst_bytes_per_sample);

	auto frame_source = load_jpeg2k_frame_source(ds, frame_index);
	if (frame_source.total_size == 0) {
		diag::error_and_throw(
		    "pixel::decode_into file={} frame={} reason=JPEG2000 frame has empty codestream",
		    ds.path(), frame_index);
	}

	auto image = decode_openjpeg_image(ds, frame_index, frame_source, opt);
	validate_decoded_image(ds, frame_index, info, *image, rows, cols, samples_per_pixel);

	if (opt.scaled) {
		switch (info.sv_dtype) {
		case dtype::u8:
			write_scaled_mono_to_dst<std::uint8_t>(ds, *image, dst, dst_strides, rows, cols);
			return;
		case dtype::s8:
			write_scaled_mono_to_dst<std::int8_t>(ds, *image, dst, dst_strides, rows, cols);
			return;
		case dtype::u16:
			write_scaled_mono_to_dst<std::uint16_t>(ds, *image, dst, dst_strides, rows, cols);
			return;
		case dtype::s16:
			write_scaled_mono_to_dst<std::int16_t>(ds, *image, dst, dst_strides, rows, cols);
			return;
		case dtype::u32:
			write_scaled_mono_to_dst<std::uint32_t>(ds, *image, dst, dst_strides, rows, cols);
			return;
		case dtype::s32:
			write_scaled_mono_to_dst<std::int32_t>(ds, *image, dst, dst_strides, rows, cols);
			return;
		default:
			diag::error_and_throw(
			    "pixel::decode_into file={} reason=scaled output does not support sv_dtype={}",
			    ds.path(), static_cast<int>(info.sv_dtype));
			return;
		}
	}

	switch (info.sv_dtype) {
	case dtype::u8:
		write_unscaled_to_dst<std::uint8_t>(
		    *image, dst, dst_strides, opt.planar_out, rows, cols, samples_per_pixel);
		return;
	case dtype::s8:
		write_unscaled_to_dst<std::int8_t>(
		    *image, dst, dst_strides, opt.planar_out, rows, cols, samples_per_pixel);
		return;
	case dtype::u16:
		write_unscaled_to_dst<std::uint16_t>(
		    *image, dst, dst_strides, opt.planar_out, rows, cols, samples_per_pixel);
		return;
	case dtype::s16:
		write_unscaled_to_dst<std::int16_t>(
		    *image, dst, dst_strides, opt.planar_out, rows, cols, samples_per_pixel);
		return;
	case dtype::u32:
		write_unscaled_to_dst<std::uint32_t>(
		    *image, dst, dst_strides, opt.planar_out, rows, cols, samples_per_pixel);
		return;
	case dtype::s32:
		write_unscaled_to_dst<std::int32_t>(
		    *image, dst, dst_strides, opt.planar_out, rows, cols, samples_per_pixel);
		return;
	default:
		diag::error_and_throw(
		    "pixel::decode_into file={} reason=JPEG2000 output does not support sv_dtype={}",
		    ds.path(), static_cast<int>(info.sv_dtype));
		return;
	}
}

} // namespace pixel::detail
} // namespace dicom
