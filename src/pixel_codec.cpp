#include "pixel_codec.h"

#include "dicom.h"
#include <array>
#include <mutex>

using namespace dicom::literals;

namespace dicom {

// Forward declarations provided by codec units.
void register_raw_decoders();
void register_rle_decoders();

namespace {

auto& decoder_registry() {
	static std::array<DecoderFactory, kMaxTransferSyntaxIndex + 1> reg{};
	return reg;
}

void ensure_builtin_decoders_registered() {
	static std::once_flag once;
	std::call_once(once, [] {
		register_raw_decoders();
		register_rle_decoders();
	});
}

inline std::size_t align_up(std::size_t value, std::size_t alignment) {
	if (alignment <= 1) {
		return value;
	}
	const auto rem = value % alignment;
	return rem ? value + (alignment - rem) : value;
}

}  // namespace

StrideInfo FrameInfo::compute_strides(const DecodeOptions* opts) const {
	const auto fmt = opts ? resolve_output_format(*this, *opts) : PixelFormat::auto_format;
	std::size_t bps = bytes_per_sample(fmt);
	if (bps == 0) {
		// Fallback to stored size.
		bps = static_cast<std::size_t>((bits_allocated + 7) / 8);
	}
	const bool planar_out = opts && opts->output_layout == OutputLayout::planar;
	const std::size_t tight_row = static_cast<std::size_t>(cols) *
	    static_cast<std::size_t>(samples_per_pixel) * bps;
	const std::size_t effective_row = planar_out
	    ? static_cast<std::size_t>(cols) * bps
	    : tight_row;
	const std::size_t alignment = opts ? opts->output_alignment : 1;
	const std::size_t row = align_up(effective_row, alignment);
	const std::size_t frame_bytes = planar_out
	    ? row * static_cast<std::size_t>(rows) * static_cast<std::size_t>(samples_per_pixel)
	    : row * static_cast<std::size_t>(rows);
	return StrideInfo{row, frame_bytes};
}

void register_decoder(std::string_view ts_uid, DecoderFactory factory) {
	const auto ts = uid::lookup(ts_uid);
	if (!ts || ts->raw_index() > kMaxTransferSyntaxIndex) {
		return;
	}
	decoder_registry()[ts->raw_index()] = std::move(factory);
}

std::unique_ptr<PixelDecoder> make_decoder(const DataSet& ds) {
	ensure_builtin_decoders_registered();
	const auto ts = ds.transfer_syntax_uid();
	if (!ts.valid()) {
		return nullptr;
	}
	const auto idx = ts.raw_index();
	if (idx > kMaxTransferSyntaxIndex) {
		return nullptr;
	}
	const auto& factory = decoder_registry()[idx];
	return factory ? factory() : nullptr;
}

FrameInfo DataSet::frame_info(std::size_t frame) const {
	if (auto dec = make_decoder(*this)) {
		return dec->frame_info(*this, frame);
	}
	return {};
}

DecodeStatus DataSet::decode_into(std::span<std::byte> dst, std::size_t frame,
    DecodeOptions opts) const {
	auto dec = make_decoder(*this);
	if (!dec) {
		return DecodeStatus::unsupported_ts;
	}
	return dec->decode_into(*this, dst, frame, opts);
}

std::vector<std::byte> DataSet::decode_pixels(std::size_t frame, DecodeOptions opts) const {
	const auto info = frame_info(frame);
	const auto stride = info.compute_strides(&opts);
	const std::size_t bytes = opts.output_stride
	    ? opts.output_stride * static_cast<std::size_t>(info.rows) *
	          (opts.output_layout == OutputLayout::planar
	               ? static_cast<std::size_t>(info.samples_per_pixel)
	               : 1)
	    : stride.frame_bytes;
	std::vector<std::byte> buffer(bytes);
	const auto status = decode_into(buffer, frame, opts);
	if (status != DecodeStatus::ok) {
		return {};
	}
	return buffer;
}

}  // namespace dicom
