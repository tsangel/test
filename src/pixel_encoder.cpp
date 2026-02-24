#include "dicom.h"

#include "diagnostics.h"
#include "pixel_encoder_detail.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

namespace dicom {
using namespace dicom::literals;

namespace {
constexpr double kDefaultLossyJ2kTargetPsnr = 45.0;
constexpr int kDefaultNearLosslessJpegLsError = 2;

std::size_t bytes_per_sample_of(pixel::DataType dtype) noexcept {
	switch (dtype) {
	case pixel::DataType::u8:
	case pixel::DataType::s8:
		return 1;
	case pixel::DataType::u16:
	case pixel::DataType::s16:
		return 2;
	case pixel::DataType::u32:
	case pixel::DataType::s32:
	case pixel::DataType::f32:
		return 4;
	case pixel::DataType::f64:
		return 8;
	default:
		return 0;
	}
}

struct native_source_layout {
	int bits_allocated{0};
	int pixel_representation{0}; // 0 unsigned, 1 signed
};

[[nodiscard]] std::optional<native_source_layout> native_source_layout_of(
    pixel::DataType data_type) noexcept {
	switch (data_type) {
	case pixel::DataType::u8:
		return native_source_layout{8, 0};
	case pixel::DataType::s8:
		return native_source_layout{8, 1};
	case pixel::DataType::u16:
		return native_source_layout{16, 0};
	case pixel::DataType::s16:
		return native_source_layout{16, 1};
	case pixel::DataType::u32:
		return native_source_layout{32, 0};
	case pixel::DataType::s32:
		return native_source_layout{32, 1};
	default:
		return std::nullopt;
	}
}

[[nodiscard]] std::string_view to_photometric_text(pixel::Photometric photometric) noexcept {
	switch (photometric) {
	case pixel::Photometric::monochrome1:
		return "MONOCHROME1";
	case pixel::Photometric::monochrome2:
		return "MONOCHROME2";
	case pixel::Photometric::rgb:
		return "RGB";
	case pixel::Photometric::ybr_full:
		return "YBR_FULL";
	case pixel::Photometric::ybr_full_422:
		return "YBR_FULL_422";
	case pixel::Photometric::ybr_rct:
		return "YBR_RCT";
	case pixel::Photometric::ybr_ict:
		return "YBR_ICT";
	default:
		return "MONOCHROME2";
	}
}

[[nodiscard]] bool is_jpeg2000_mc_transfer_syntax(uid::WellKnown transfer_syntax) noexcept {
	return transfer_syntax == "JPEG2000MCLossless"_uid ||
	    transfer_syntax == "JPEG2000MC"_uid;
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

[[nodiscard]] bool source_aliases_native_pixel_data(
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

struct pixel_encode_target {
	bool is_native_uncompressed{false};
	bool is_encapsulated_uncompressed{false};
	bool is_rle{false};
	bool is_j2k{false};
	bool is_j2k_lossless{false};
	bool is_j2k_lossy{false};
	bool is_htj2k{false};
	bool is_htj2k_lossless{false};
	bool is_htj2k_lossy{false};
	bool is_jpegls{false};
	bool is_jpegls_lossless{false};
	bool is_jpegls_lossy{false};
	bool is_jpeg{false};
	bool is_jpeg_lossless{false};
	bool is_jpeg_lossy{false};
};

[[nodiscard]] pixel_encode_target classify_pixel_encode_target(uid::WellKnown transfer_syntax) noexcept {
	pixel_encode_target target{};
	target.is_native_uncompressed =
	    transfer_syntax.is_uncompressed() && !transfer_syntax.is_encapsulated();
	target.is_encapsulated_uncompressed =
	    transfer_syntax.is_uncompressed() && transfer_syntax.is_encapsulated();
	target.is_rle = transfer_syntax.is_rle();
	target.is_j2k = transfer_syntax.is_jpeg2000() && !transfer_syntax.is_htj2k();
	target.is_j2k_lossless = target.is_j2k && transfer_syntax.is_lossless();
	target.is_j2k_lossy = target.is_j2k && transfer_syntax.is_lossy();
	target.is_htj2k = transfer_syntax.is_htj2k();
	target.is_htj2k_lossless = target.is_htj2k && transfer_syntax.is_lossless();
	target.is_htj2k_lossy = target.is_htj2k && transfer_syntax.is_lossy();
	target.is_jpegls = transfer_syntax.is_jpegls();
	target.is_jpegls_lossless = target.is_jpegls && transfer_syntax.is_lossless();
	target.is_jpegls_lossy = target.is_jpegls && transfer_syntax.is_lossy();
	target.is_jpeg = transfer_syntax.is_jpeg_family() &&
	    !transfer_syntax.is_jpeg2000() &&
	    !transfer_syntax.is_jpegls() &&
	    !transfer_syntax.is_jpegxl();
	target.is_jpeg_lossless = target.is_jpeg && transfer_syntax.is_lossless();
	target.is_jpeg_lossy = target.is_jpeg && transfer_syntax.is_lossy();
	return target;
}

[[nodiscard]] pixel::CodecOptions resolve_codec_options_for_target(
    uid::WellKnown transfer_syntax, const pixel_encode_target& target,
    pixel::CodecOptions codec_opt) {
	if (!std::holds_alternative<pixel::AutoCodecOptions>(codec_opt)) {
		return codec_opt;
	}

	if (target.is_j2k) {
		pixel::J2kOptions default_j2k{};
		if (target.is_j2k_lossy) {
			default_j2k.target_psnr = kDefaultLossyJ2kTargetPsnr;
		}
		return default_j2k;
	}
	if (target.is_htj2k) {
		pixel::Htj2kOptions default_htj2k{};
		if (target.is_htj2k_lossy) {
			default_htj2k.target_psnr = kDefaultLossyJ2kTargetPsnr;
		}
		return default_htj2k;
	}
	if (target.is_jpegls) {
		pixel::JpegLsOptions default_jpegls{};
		if (target.is_jpegls_lossy) {
			default_jpegls.near_lossless_error = kDefaultNearLosslessJpegLsError;
		}
		return default_jpegls;
	}
	if (target.is_jpeg) {
		return pixel::JpegOptions{};
	}
	if (target.is_rle) {
		return pixel::RleOptions{};
	}

	(void)transfer_syntax;
	return pixel::NoCompression{};
}

void validate_codec_option_for_target(uid::WellKnown transfer_syntax,
    const pixel_encode_target& target, const pixel::CodecOptions& codec_opt,
    std::string_view file_path) {
	if (target.is_j2k) {
		if (!std::holds_alternative<pixel::J2kOptions>(codec_opt)) {
			diag::error_and_throw(
			    "DicomFile::set_pixel_data file={} ts={} reason=JPEG2000 transfer syntax requires J2kOptions codec",
			    file_path, transfer_syntax.value());
		}
		const auto& options = std::get<pixel::J2kOptions>(codec_opt);
		if (options.threads < -1) {
			diag::error_and_throw(
			    "DicomFile::set_pixel_data file={} ts={} reason=J2kOptions.threads must be -1, 0, or positive",
			    file_path, transfer_syntax.value());
		}
		return;
	}
	if (target.is_htj2k) {
		if (!std::holds_alternative<pixel::Htj2kOptions>(codec_opt)) {
			diag::error_and_throw(
			    "DicomFile::set_pixel_data file={} ts={} reason=HTJ2K transfer syntax requires Htj2kOptions codec",
			    file_path, transfer_syntax.value());
		}
		const auto& options = std::get<pixel::Htj2kOptions>(codec_opt);
		if (options.threads < -1) {
			diag::error_and_throw(
			    "DicomFile::set_pixel_data file={} ts={} reason=Htj2kOptions.threads must be -1, 0, or positive",
			    file_path, transfer_syntax.value());
		}
		return;
	}
	if (target.is_jpegls) {
		if (!std::holds_alternative<pixel::JpegLsOptions>(codec_opt)) {
			diag::error_and_throw(
			    "DicomFile::set_pixel_data file={} ts={} reason=JPEG-LS transfer syntax requires JpegLsOptions codec",
			    file_path, transfer_syntax.value());
		}
		const auto& options = std::get<pixel::JpegLsOptions>(codec_opt);
		if (target.is_jpegls_lossless && options.near_lossless_error != 0) {
			diag::error_and_throw(
			    "DicomFile::set_pixel_data file={} ts={} reason=JPEG-LS lossless transfer syntax requires near_lossless_error=0",
			    file_path, transfer_syntax.value());
		}
		if (target.is_jpegls_lossy && options.near_lossless_error <= 0) {
			diag::error_and_throw(
			    "DicomFile::set_pixel_data file={} ts={} reason=JPEG-LS near-lossless transfer syntax requires near_lossless_error>0",
			    file_path, transfer_syntax.value());
		}
		return;
	}
	if (target.is_jpeg) {
		if (!std::holds_alternative<pixel::JpegOptions>(codec_opt)) {
			diag::error_and_throw(
			    "DicomFile::set_pixel_data file={} ts={} reason=JPEG transfer syntax requires JpegOptions codec",
			    file_path, transfer_syntax.value());
		}
		return;
	}
	if (target.is_rle) {
		if (!std::holds_alternative<pixel::RleOptions>(codec_opt)) {
			diag::error_and_throw(
			    "DicomFile::set_pixel_data file={} ts={} reason=RLELossless transfer syntax requires RleOptions codec",
			    file_path, transfer_syntax.value());
		}
		return;
	}
	if (!std::holds_alternative<pixel::NoCompression>(codec_opt)) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} ts={} reason=native uncompressed transfer syntax requires NoCompression codec",
		    file_path, transfer_syntax.value());
	}
}

void validate_target_source_constraints(const pixel_encode_target& target,
    int bits_allocated, int bits_stored, std::string_view file_path) {
	if ((target.is_j2k || target.is_htj2k || target.is_jpegls || target.is_jpeg) &&
	    bits_allocated > 16) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} reason=selected encoder currently supports bits_allocated <= 16",
		    file_path);
	}
	if (target.is_jpeg_lossy && bits_stored > 12) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} reason=lossy JPEG transfer syntax requires bits_stored <= 12",
		    file_path);
	}
}

[[nodiscard]] bool resolve_use_multicomponent_transform(uid::WellKnown transfer_syntax,
    const pixel_encode_target& target, const pixel::CodecOptions& resolved_codec_opt,
    std::size_t samples_per_pixel, std::string_view file_path) {
	if (target.is_j2k) {
		const auto& options = std::get<pixel::J2kOptions>(resolved_codec_opt);
		if (is_jpeg2000_mc_transfer_syntax(transfer_syntax)) {
			if (!options.use_color_transform) {
				diag::error_and_throw(
				    "DicomFile::set_pixel_data file={} ts={} reason=JPEG2000 MC transfer syntax requires color transform enabled",
				    file_path, transfer_syntax.value());
			}
			if (samples_per_pixel != std::size_t{3}) {
				diag::error_and_throw(
				    "DicomFile::set_pixel_data file={} ts={} reason=JPEG2000 MC transfer syntax requires samples_per_pixel=3",
				    file_path, transfer_syntax.value());
			}
			return true;
		}
		return options.use_color_transform && samples_per_pixel == std::size_t{3};
	}
	if (target.is_htj2k) {
		const auto& options = std::get<pixel::Htj2kOptions>(resolved_codec_opt);
		return options.use_color_transform && samples_per_pixel == std::size_t{3};
	}
	return false;
}

[[nodiscard]] pixel::Photometric resolve_output_photometric(
    const pixel_encode_target& target, bool use_multicomponent_transform,
    pixel::Photometric source_photometric) noexcept {
	if (!use_multicomponent_transform) {
		return source_photometric;
	}
	if (target.is_j2k_lossless || target.is_htj2k_lossless) {
		return pixel::Photometric::ybr_rct;
	}
	return pixel::Photometric::ybr_ict;
}

void update_pixel_metadata_for_set_pixel_data(DataSet& dataset, std::string_view file_path,
    uid::WellKnown transfer_syntax, const pixel::PixelSource& source, bool target_is_rle,
    pixel::Photometric output_photometric, int bits_allocated, int bits_stored,
    int high_bit, int pixel_representation,
    std::size_t source_row_stride, std::size_t source_frame_stride) {
	bool ok = true;
	ok &= dataset.add_dataelement("Rows"_tag, VR::US)
	          ->from_long(static_cast<long>(source.rows));
	ok &= dataset.add_dataelement("Columns"_tag, VR::US)
	          ->from_long(static_cast<long>(source.cols));
	ok &= dataset.add_dataelement("SamplesPerPixel"_tag, VR::US)
	          ->from_long(static_cast<long>(source.samples_per_pixel));
	ok &= dataset.add_dataelement("BitsAllocated"_tag, VR::US)
	          ->from_long(static_cast<long>(bits_allocated));
	ok &= dataset.add_dataelement("BitsStored"_tag, VR::US)
	          ->from_long(static_cast<long>(bits_stored));
	ok &= dataset.add_dataelement("HighBit"_tag, VR::US)
	          ->from_long(static_cast<long>(high_bit));
	ok &= dataset.add_dataelement("PixelRepresentation"_tag, VR::US)
	          ->from_long(static_cast<long>(pixel_representation));

	const auto photometric_text = to_photometric_text(output_photometric);
	ok &= dataset.add_dataelement("PhotometricInterpretation"_tag, VR::CS)
	          ->from_string_view(photometric_text);

	if (source.frames > 1) {
		ok &= dataset.add_dataelement("NumberOfFrames"_tag, VR::IS)
		          ->from_long(static_cast<long>(source.frames));
	} else {
		dataset.remove_dataelement("NumberOfFrames"_tag);
	}
	if (source.samples_per_pixel > 1) {
		const long planar_configuration = target_is_rle
		                                      ? 1L
		                                      : (source.planar == pixel::Planar::planar ? 1L : 0L);
		ok &= dataset.add_dataelement("PlanarConfiguration"_tag, VR::US)
		          ->from_long(planar_configuration);
	} else {
		dataset.remove_dataelement("PlanarConfiguration"_tag);
	}
	if (!ok) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} ts={} reason=failed to update one or more pixel metadata tags requested rows={} cols={} frames={} spp={} bits_allocated={} bits_stored={} high_bit={} pixel_representation={} photometric={} planar={} row_stride={} frame_stride={}",
		    file_path, transfer_syntax.value(), source.rows, source.cols, source.frames,
		    source.samples_per_pixel, bits_allocated, bits_stored, high_bit,
		    pixel_representation, photometric_text,
		    source.planar == pixel::Planar::planar ? "planar" : "interleaved",
		    source_row_stride, source_frame_stride);
	}
}

struct NativePixelCopyInput {
	std::span<const std::uint8_t> source_bytes{};
	std::size_t rows{0};
	std::size_t frames{0};
	std::size_t samples_per_pixel{0};
	bool planar_source{false};
	std::size_t row_payload_bytes{0};
	std::size_t source_row_stride{0};
	std::size_t source_plane_stride{0};
	std::size_t source_frame_stride{0};
	std::size_t destination_frame_payload{0};
	std::size_t destination_total_bytes{0};
};

[[nodiscard]] std::vector<std::uint8_t> build_native_pixel_payload(
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

} // namespace

void DicomFile::set_pixel_data(uid::WellKnown transfer_syntax, const pixel::PixelSource& source,
    pixel::CodecOptions codec_opt) {
	if (!transfer_syntax.valid() || transfer_syntax.uid_type() != UidType::TransferSyntax) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data reason=transfer_syntax must be a valid Transfer Syntax UID");
	}

	const auto target = classify_pixel_encode_target(transfer_syntax);
	if (!transfer_syntax.supports_pixel_encode()) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} ts={} reason=transfer syntax is not supported yet in set_pixel_data (supported: native uncompressed, EncapsulatedUncompressedExplicitVRLittleEndian, RLELossless, JPEG2000*, HTJ2K*, JPEG-LS, JPEG Baseline/Extended/Lossless)",
		    path(), transfer_syntax.value());
	}
	auto resolved_codec_opt = resolve_codec_options_for_target(transfer_syntax, target, codec_opt);
	validate_codec_option_for_target(transfer_syntax, target, resolved_codec_opt, path());

	const auto native_layout = native_source_layout_of(source.data_type);
	if (!native_layout) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} reason=source.data_type must be one of u8/s8/u16/s16/u32/s32 for current implementation",
		    path());
	}
	const auto bytes_per_sample = bytes_per_sample_of(source.data_type);
	if (bytes_per_sample == 0) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} reason=invalid source.data_type",
		    path());
	}

	if (source.rows <= 0 || source.cols <= 0 || source.frames <= 0 || source.samples_per_pixel <= 0) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} reason=rows/cols/frames/samples_per_pixel must be positive",
		    path());
	}
	if (source.rows > 65535 || source.cols > 65535) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} reason=rows/cols must be <= 65535",
		    path());
	}

	const auto checked_mul = [&](std::size_t a, std::size_t b, std::string_view label) {
		if (a != 0 && b > std::numeric_limits<std::size_t>::max() / a) {
			diag::error_and_throw(
			    "DicomFile::set_pixel_data file={} reason={} overflows size_t",
			    path(), label);
		}
		return a * b;
	};
	const auto checked_add = [&](std::size_t a, std::size_t b, std::string_view label) {
		if (b > std::numeric_limits<std::size_t>::max() - a) {
			diag::error_and_throw(
			    "DicomFile::set_pixel_data file={} reason={} overflows size_t",
			    path(), label);
		}
		return a + b;
	};

	const auto rows = static_cast<std::size_t>(source.rows);
	const auto cols = static_cast<std::size_t>(source.cols);
	const auto frames = static_cast<std::size_t>(source.frames);
	const auto samples_per_pixel = static_cast<std::size_t>(source.samples_per_pixel);
	const bool planar_source =
	    source.planar == pixel::Planar::planar && samples_per_pixel > std::size_t{1};

	const std::size_t row_samples =
	    planar_source ? cols : checked_mul(cols, samples_per_pixel, "row samples");
	const std::size_t row_payload_bytes =
	    checked_mul(row_samples, bytes_per_sample, "row payload bytes");

	const std::size_t source_row_stride =
	    source.row_stride == 0 ? row_payload_bytes : source.row_stride;
	if (source_row_stride < row_payload_bytes) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} reason=row_stride({}) is smaller than row payload({})",
		    path(), source_row_stride, row_payload_bytes);
	}

	const std::size_t source_plane_stride =
	    checked_mul(source_row_stride, rows, "source plane stride");
	const std::size_t source_frame_payload = planar_source
	                                             ? checked_mul(source_plane_stride, samples_per_pixel,
	                                                   "source frame payload bytes")
	                                             : source_plane_stride;
	const std::size_t source_frame_stride =
	    source.frame_stride == 0 ? source_frame_payload : source.frame_stride;
	if (source_frame_stride < source_frame_payload) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} reason=frame_stride({}) is smaller than frame payload({})",
		    path(), source_frame_stride, source_frame_payload);
	}

	const std::size_t destination_frame_payload = planar_source
	                                                  ? checked_mul(
	                                                        checked_mul(row_payload_bytes, rows,
	                                                            "destination plane stride"),
	                                                        samples_per_pixel,
	                                                        "destination frame payload bytes")
	                                                  : checked_mul(row_payload_bytes, rows,
	                                                        "destination frame payload bytes");
	const std::size_t destination_total_bytes =
	    checked_mul(destination_frame_payload, frames, "destination total bytes");

	const std::size_t source_last_frame_begin =
	    checked_mul(frames - 1, source_frame_stride, "source last frame begin");
	const std::size_t source_last_frame_used = planar_source
	                                               ? checked_add(
	                                                     checked_add(
	                                                         checked_mul(samples_per_pixel - 1,
	                                                             source_plane_stride,
	                                                             "source last plane offset"),
	                                                         checked_mul(rows - 1, source_row_stride,
	                                                             "source last row offset"),
	                                                         "source planar offsets"),
	                                                     row_payload_bytes,
	                                                     "source last planar row payload")
	                                               : checked_add(
	                                                     checked_mul(rows - 1, source_row_stride,
	                                                         "source last row offset"),
	                                                     row_payload_bytes,
	                                                     "source last row payload");
	const std::size_t source_required_bytes =
	    checked_add(source_last_frame_begin, source_last_frame_used,
	        "minimum source byte requirement");
	if (source.bytes.size() < source_required_bytes) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} reason=source bytes({}) are shorter than required({})",
		    path(), source.bytes.size(), source_required_bytes);
	}

	const int bits_allocated = native_layout->bits_allocated;
	int bits_stored = source.bits_stored > 0 ? source.bits_stored : bits_allocated;
	const int pixel_representation = native_layout->pixel_representation;
	if (bits_stored <= 0 || bits_stored > bits_allocated) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} reason=bits_stored({}) must be in [1, bits_allocated({})]",
		    path(), bits_stored, bits_allocated);
	}
	const int high_bit = bits_stored - 1;
	validate_target_source_constraints(target, bits_allocated, bits_stored, path());
	const bool use_multicomponent_transform = resolve_use_multicomponent_transform(
	    transfer_syntax, target, resolved_codec_opt, samples_per_pixel, path());
	const pixel::Photometric output_photometric = resolve_output_photometric(
	    target, use_multicomponent_transform, source.photometric);

	root_dataset_->ensure_loaded(Tag(0xFFFFu, 0xFFFFu));
	update_pixel_metadata_for_set_pixel_data(
	    *root_dataset_, path(), transfer_syntax, source, target.is_rle,
	    output_photometric,
	    bits_allocated, bits_stored, high_bit, pixel_representation,
	    source_row_stride, source_frame_stride);

	// set_transfer_syntax(native->encapsulated) can pass a span that aliases current
	// native PixelData bytes. We must preserve those bytes until all frames are encoded.
	const bool source_aliases_current_native_pixel_data =
	    source_aliases_native_pixel_data(*root_dataset_, source.bytes);
	const pixel::detail::EncapsulatedEncodeInput encapsulated_encode_input{
	    .source_base = source.bytes.data(),
	    .frame_count = frames,
	    .source_frame_stride = source_frame_stride,
	    .source_frame_payload = source_frame_payload,
	    .source_aliases_current_native_pixel_data = source_aliases_current_native_pixel_data,
	};

	if (target.is_native_uncompressed) {
		const NativePixelCopyInput native_copy_input{
		    .source_bytes = source.bytes,
		    .rows = rows,
		    .frames = frames,
		    .samples_per_pixel = samples_per_pixel,
		    .planar_source = planar_source,
		    .row_payload_bytes = row_payload_bytes,
		    .source_row_stride = source_row_stride,
		    .source_plane_stride = source_plane_stride,
		    .source_frame_stride = source_frame_stride,
		    .destination_frame_payload = destination_frame_payload,
		    .destination_total_bytes = destination_total_bytes,
		};
		auto native_pixel_data = build_native_pixel_payload(native_copy_input);
		set_native_pixel_data(std::move(native_pixel_data));
	} else {
		const pixel::detail::EncapsulatedPixelEncodeDispatchInput dispatch_input{
		    .file = *this,
		    .transfer_syntax = transfer_syntax,
		    .resolved_codec_opt = resolved_codec_opt,
		    .encode_input = encapsulated_encode_input,
		    .rows = rows,
		    .cols = cols,
		    .samples_per_pixel = samples_per_pixel,
		    .bytes_per_sample = bytes_per_sample,
		    .bits_allocated = bits_allocated,
		    .bits_stored = bits_stored,
		    .pixel_representation = pixel_representation,
		    .use_multicomponent_transform = use_multicomponent_transform,
		    .source_planar = source.planar,
		    .planar_source = planar_source,
		    .row_payload_bytes = row_payload_bytes,
		    .source_row_stride = source_row_stride,
		    .source_plane_stride = source_plane_stride,
		    .source_frame_payload = source_frame_payload,
		    .destination_frame_payload = destination_frame_payload,
		    .is_encapsulated_uncompressed = target.is_encapsulated_uncompressed,
		    .is_j2k = target.is_j2k,
		    .is_j2k_lossless = target.is_j2k_lossless,
		    .is_htj2k = target.is_htj2k,
		    .is_htj2k_lossless = target.is_htj2k_lossless,
		    .is_jpegls = target.is_jpegls,
		    .is_jpeg = target.is_jpeg,
		    .is_jpeg_lossless = target.is_jpeg_lossless,
		    .is_rle = target.is_rle,
		    .file_path = path(),
		};
		pixel::detail::encode_encapsulated_pixel_data(dispatch_input);
	}

	set_transfer_syntax_state_only(transfer_syntax);
	DataElement* transfer_syntax_element = add_dataelement("(0002,0010)"_tag, VR::UI);
	if (!transfer_syntax_element || !transfer_syntax_element->from_transfer_syntax_uid(transfer_syntax)) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} reason=failed to update (0002,0010) TransferSyntaxUID",
		    path());
	}
}

} // namespace dicom
