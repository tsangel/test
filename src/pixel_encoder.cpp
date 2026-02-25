#include "dicom.h"

#include "diagnostics.h"
#include "pixel_codec_registry.hpp"
#include "pixel_encoder_detail.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dicom {
using namespace dicom::literals;

namespace {
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
	bool is_jpegxl{false};
	bool is_jpegxl_lossless{false};
	bool is_jpegxl_lossy{false};
};

[[nodiscard]] pixel_encode_target classify_pixel_encode_target(
    const pixel::detail::transfer_syntax_plugin_binding& binding) noexcept {
	pixel_encode_target target{};
	target.is_native_uncompressed =
	    binding.profile == pixel::detail::codec_profile::native_uncompressed;
	target.is_encapsulated_uncompressed =
	    binding.profile == pixel::detail::codec_profile::encapsulated_uncompressed;
	target.is_rle = binding.profile == pixel::detail::codec_profile::rle_lossless;
	target.is_j2k = binding.plugin_key == "jpeg2k";
	target.is_j2k_lossless =
	    binding.profile == pixel::detail::codec_profile::jpeg2000_lossless;
	target.is_j2k_lossy =
	    binding.profile == pixel::detail::codec_profile::jpeg2000_lossy;
	target.is_htj2k = binding.plugin_key == "htj2k";
	target.is_htj2k_lossless =
	    binding.profile == pixel::detail::codec_profile::htj2k_lossless ||
	    binding.profile == pixel::detail::codec_profile::htj2k_lossless_rpcl;
	target.is_htj2k_lossy = binding.profile == pixel::detail::codec_profile::htj2k_lossy;
	target.is_jpegls = binding.plugin_key == "jpegls";
	target.is_jpegls_lossless =
	    binding.profile == pixel::detail::codec_profile::jpegls_lossless;
	target.is_jpegls_lossy =
	    binding.profile == pixel::detail::codec_profile::jpegls_near_lossless;
	target.is_jpeg = binding.plugin_key == "jpeg";
	target.is_jpeg_lossless = binding.profile == pixel::detail::codec_profile::jpeg_lossless;
	target.is_jpeg_lossy = binding.profile == pixel::detail::codec_profile::jpeg_lossy;
	target.is_jpegxl = binding.plugin_key == "jpegxl";
	target.is_jpegxl_lossless =
	    binding.profile == pixel::detail::codec_profile::jpegxl_lossless;
	target.is_jpegxl_lossy = binding.profile == pixel::detail::codec_profile::jpegxl_lossy;
	return target;
}

void validate_target_source_constraints(const pixel_encode_target& target,
    int bits_allocated, int bits_stored, std::string_view file_path) {
	if ((target.is_j2k || target.is_htj2k || target.is_jpegls || target.is_jpeg ||
	        target.is_jpegxl) &&
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

[[nodiscard]] bool target_uses_lossy_compression(
    const pixel_encode_target& target) noexcept {
	return target.is_j2k_lossy || target.is_htj2k_lossy ||
	    target.is_jpegls_lossy || target.is_jpeg_lossy ||
	    target.is_jpegxl_lossy;
}

[[nodiscard]] std::optional<std::string_view> lossy_method_for_target(
    const pixel_encode_target& target) noexcept {
	if (target.is_j2k_lossy) {
		return std::string_view("ISO_15444_1");
	}
	if (target.is_htj2k_lossy) {
		return std::string_view("ISO_15444_15");
	}
	if (target.is_jpegls_lossy) {
		return std::string_view("ISO_14495_1");
	}
	if (target.is_jpeg_lossy) {
		return std::string_view("ISO_10918_1");
	}
	if (target.is_jpegxl_lossy) {
		return std::string_view("ISO_18181_1");
	}
	return std::nullopt;
}

[[nodiscard]] bool has_prior_lossy_history(const DataSet& dataset) {
	const auto& lossy = dataset["LossyImageCompression"_tag];
	const auto lossy_value = lossy.to_string_view();
	return lossy_value && *lossy_value == "01";
}

[[nodiscard]] std::vector<std::string> read_lossy_method_values(
    const DataSet& dataset) {
	std::vector<std::string> methods;
	const auto& method_elem = dataset["LossyImageCompressionMethod"_tag];
	const auto parsed = method_elem.to_string_views();
	if (!parsed) {
		return methods;
	}
	methods.reserve(parsed->size());
	for (const auto value : *parsed) {
		methods.emplace_back(value);
	}
	return methods;
}

[[nodiscard]] std::vector<double> read_lossy_ratio_values(const DataSet& dataset) {
	const auto& ratio_elem = dataset["LossyImageCompressionRatio"_tag];
	const auto parsed = ratio_elem.to_double_vector();
	if (!parsed) {
		return {};
	}
	return *parsed;
}

[[nodiscard]] std::size_t encoded_payload_size_from_pixel_sequence(
    const DataSet& dataset, std::string_view file_path, uid::WellKnown transfer_syntax) {
	const auto& pixel_data = dataset["PixelData"_tag];
	if (!pixel_data || !pixel_data.vr().is_pixel_sequence()) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} ts={} reason=lossy metadata update requires encapsulated PixelData sequence",
		    file_path, transfer_syntax.value());
	}
	const auto* pixel_sequence = pixel_data.as_pixel_sequence();
	if (!pixel_sequence) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} ts={} reason=lossy metadata update requires encapsulated PixelData sequence",
		    file_path, transfer_syntax.value());
	}

	std::size_t encoded_payload_bytes = 0;
	constexpr std::size_t kSizeMax = std::numeric_limits<std::size_t>::max();
	for (std::size_t frame_index = 0; frame_index < pixel_sequence->number_of_frames();
	     ++frame_index) {
		const auto* frame = pixel_sequence->frame(frame_index);
		if (!frame) {
			diag::error_and_throw(
			    "DicomFile::set_pixel_data file={} ts={} reason=encoded frame {} missing while updating lossy metadata",
			    file_path, transfer_syntax.value(), frame_index);
		}
		const auto frame_bytes = frame->encoded_data_size();
		if (encoded_payload_bytes > kSizeMax - frame_bytes) {
			diag::error_and_throw(
			    "DicomFile::set_pixel_data file={} ts={} reason=encoded payload size overflows size_t while updating lossy metadata",
			    file_path, transfer_syntax.value());
		}
		encoded_payload_bytes += frame_bytes;
	}
	return encoded_payload_bytes;
}

void update_lossy_compression_metadata_for_set_pixel_data(DataSet& dataset,
    std::string_view file_path, uid::WellKnown transfer_syntax,
    const pixel_encode_target& target, std::size_t uncompressed_payload_bytes,
    std::size_t encoded_payload_bytes) {
	const bool current_encode_is_lossy = target_uses_lossy_compression(target);
	const bool had_prior_lossy = has_prior_lossy_history(dataset);

	bool ok = true;
	if (!current_encode_is_lossy) {
		auto* lossy_elem = dataset.add_dataelement("LossyImageCompression"_tag, VR::CS);
		ok &= lossy_elem &&
		    lossy_elem->from_string_view(had_prior_lossy ? "01" : "00");
		if (!had_prior_lossy) {
			dataset.remove_dataelement("LossyImageCompressionRatio"_tag);
			dataset.remove_dataelement("LossyImageCompressionMethod"_tag);
		}
		if (!ok) {
			diag::error_and_throw(
			    "DicomFile::set_pixel_data file={} ts={} reason=failed to update LossyImageCompression metadata for non-lossy encode",
			    file_path, transfer_syntax.value());
		}
		return;
	}

	if (encoded_payload_bytes == 0 || uncompressed_payload_bytes == 0) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} ts={} reason=cannot compute lossy compression ratio from zero payload size (uncompressed={} encoded={})",
		    file_path, transfer_syntax.value(), uncompressed_payload_bytes,
		    encoded_payload_bytes);
	}
	const auto method = lossy_method_for_target(target);
	if (!method) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} ts={} reason=missing lossy compression method mapping for transfer syntax",
		    file_path, transfer_syntax.value());
	}

	const double ratio = static_cast<double>(uncompressed_payload_bytes) /
	    static_cast<double>(encoded_payload_bytes);
	if (!std::isfinite(ratio) || ratio <= 0.0) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} ts={} reason=invalid lossy compression ratio computed from uncompressed={} encoded={}",
		    file_path, transfer_syntax.value(), uncompressed_payload_bytes,
		    encoded_payload_bytes);
	}

	std::vector<std::string> methods;
	std::vector<double> ratios;
	if (had_prior_lossy) {
		methods = read_lossy_method_values(dataset);
		ratios = read_lossy_ratio_values(dataset);
		const auto paired_count = std::min(methods.size(), ratios.size());
		methods.resize(paired_count);
		ratios.resize(paired_count);
	}
	methods.emplace_back(*method);
	ratios.push_back(ratio);

	std::vector<std::string_view> method_views;
	method_views.reserve(methods.size());
	for (const auto& value : methods) {
		method_views.emplace_back(value);
	}

	auto* lossy_elem = dataset.add_dataelement("LossyImageCompression"_tag, VR::CS);
	ok &= lossy_elem && lossy_elem->from_string_view("01");

	auto* ratio_elem = dataset.add_dataelement("LossyImageCompressionRatio"_tag, VR::DS);
	ok &= ratio_elem &&
	    ratio_elem->from_double_vector(std::span<const double>(ratios.data(), ratios.size()));

	auto* method_elem = dataset.add_dataelement("LossyImageCompressionMethod"_tag, VR::CS);
	ok &= method_elem &&
	    method_elem->from_string_views(std::span<const std::string_view>(
	        method_views.data(), method_views.size()));

	if (!ok) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} ts={} reason=failed to update lossy compression metadata ratio={} method={} history_vm={}",
		    file_path, transfer_syntax.value(), ratio, *method, methods.size());
	}
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

	const auto& codec_registry = pixel::detail::global_codec_registry();
	const auto* binding = codec_registry.find_binding(transfer_syntax);
	if (!binding || !binding->encode_supported) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} ts={} reason=transfer syntax is not supported for encoding by registry binding",
		    path(), transfer_syntax.value());
	}
	const auto plugin_list = codec_registry.plugins();
	const pixel::detail::codec_plugin* encode_plugin = nullptr;
	if (binding->plugin_index < plugin_list.size()) {
		const auto& candidate = plugin_list[binding->plugin_index];
		if (candidate.key == binding->plugin_key) {
			encode_plugin = &candidate;
		}
	}
	if (!encode_plugin) {
		encode_plugin = codec_registry.find_plugin(binding->plugin_key);
	}
	if (!encode_plugin) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} ts={} plugin={} reason=registry binding references a missing plugin",
		    path(), transfer_syntax.value(), binding->plugin_key);
	}

	auto resolved_codec_opt = codec_opt;
	if (std::holds_alternative<pixel::AutoCodecOptions>(codec_opt)) {
		if (!encode_plugin->default_options) {
			diag::error_and_throw(
			    "DicomFile::set_pixel_data file={} ts={} plugin={} reason=codec plugin does not provide default options",
			    path(), transfer_syntax.value(), encode_plugin->key);
		}
		const auto default_codec_opt = encode_plugin->default_options(transfer_syntax);
		if (!default_codec_opt) {
			diag::error_and_throw(
			    "DicomFile::set_pixel_data file={} ts={} plugin={} reason=codec plugin could not resolve default options",
			    path(), transfer_syntax.value(), encode_plugin->key);
		}
		resolved_codec_opt = *default_codec_opt;
	}
	if (!encode_plugin->export_options) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} ts={} plugin={} reason=codec plugin does not provide export_options hook",
		    path(), transfer_syntax.value(), encode_plugin->key);
	}
	pixel::detail::codec_option_pairs codec_options{};
	if (const auto export_error = encode_plugin->export_options(
	        transfer_syntax, resolved_codec_opt, codec_options)) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} ts={} plugin={} reason={}",
		    path(), transfer_syntax.value(), encode_plugin->key, *export_error);
	}
	if (!encode_plugin->parse_options) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} ts={} plugin={} reason=codec plugin does not provide parse_options hook",
		    path(), transfer_syntax.value(), encode_plugin->key);
	}
	pixel::CodecOptions reparsed_codec_opt{};
	if (const auto parse_error = encode_plugin->parse_options(
	        transfer_syntax, std::span<const pixel::detail::codec_option_kv>(codec_options),
	        reparsed_codec_opt)) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} ts={} plugin={} reason={}",
		    path(), transfer_syntax.value(), encode_plugin->key, *parse_error);
	}
	resolved_codec_opt = std::move(reparsed_codec_opt);
	if (encode_plugin->validate_options) {
		const auto validation_error =
		    encode_plugin->validate_options(transfer_syntax, resolved_codec_opt);
		if (validation_error) {
			diag::error_and_throw(
			    "DicomFile::set_pixel_data file={} ts={} plugin={} reason={}",
			    path(), transfer_syntax.value(), encode_plugin->key, *validation_error);
		}
	}

	const auto target = classify_pixel_encode_target(*binding);

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
		    path());
	}
	const std::size_t row_samples = planar_source ? cols : cols * samples_per_pixel;
	if (row_samples > kSizeMax / bytes_per_sample) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} reason=row payload bytes overflows size_t",
		    path());
	}
	const std::size_t row_payload_bytes = row_samples * bytes_per_sample;

	const std::size_t source_row_stride =
	    source.row_stride == 0 ? row_payload_bytes : source.row_stride;
	if (source_row_stride < row_payload_bytes) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} reason=row_stride({}) is smaller than row payload({})",
		    path(), source_row_stride, row_payload_bytes);
	}
	if (source_row_stride > kSizeMax / rows) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} reason=source plane stride overflows size_t",
		    path());
	}
	const std::size_t source_plane_stride = source_row_stride * rows;

	std::size_t source_frame_payload = source_plane_stride;
	if (planar_source) {
		if (samples_per_pixel > kSizeMax / source_plane_stride) {
			diag::error_and_throw(
			    "DicomFile::set_pixel_data file={} reason=source frame payload bytes overflows size_t",
			    path());
		}
		source_frame_payload = source_plane_stride * samples_per_pixel;
	}
	const std::size_t source_frame_stride =
	    source.frame_stride == 0 ? source_frame_payload : source.frame_stride;
	if (source_frame_stride < source_frame_payload) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} reason=frame_stride({}) is smaller than frame payload({})",
		    path(), source_frame_stride, source_frame_payload);
	}

	if (row_payload_bytes > kSizeMax / rows) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} reason=destination plane stride overflows size_t",
		    path());
	}
	const std::size_t destination_plane_stride = row_payload_bytes * rows;
	std::size_t destination_frame_payload = destination_plane_stride;
	if (planar_source) {
		if (samples_per_pixel > kSizeMax / destination_plane_stride) {
			diag::error_and_throw(
			    "DicomFile::set_pixel_data file={} reason=destination frame payload bytes overflows size_t",
			    path());
		}
		destination_frame_payload = destination_plane_stride * samples_per_pixel;
	}
	if (frames > kSizeMax / destination_frame_payload) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} reason=destination total bytes overflows size_t",
		    path());
	}
	const std::size_t destination_total_bytes = destination_frame_payload * frames;

	if (frames > std::size_t{1} && (frames - 1) > kSizeMax / source_frame_stride) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} reason=source last frame begin overflows size_t",
		    path());
	}
	const std::size_t source_last_frame_begin = (frames - 1) * source_frame_stride;
	const std::size_t source_last_plane_used =
	    (source_plane_stride - source_row_stride) + row_payload_bytes;
	const std::size_t source_last_frame_used = planar_source
	                                               ? (source_frame_payload - source_plane_stride) +
	                                                     source_last_plane_used
	                                               : source_last_plane_used;
	if (source_last_frame_begin > kSizeMax - source_last_frame_used) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} reason=minimum source byte requirement overflows size_t",
		    path());
	}
	const std::size_t source_required_bytes = source_last_frame_begin + source_last_frame_used;
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
		const pixel::detail::CodecEncodeFnInput dispatch_input{
		    .file = *this,
		    .transfer_syntax = transfer_syntax,
		    .encode_input = encapsulated_encode_input,
		    .codec_options = std::span<const pixel::detail::codec_option_kv>(
		        codec_options),
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
		    .profile = binding->profile,
		    .plugin_key = binding->plugin_key,
		};
		pixel::detail::encode_encapsulated_pixel_data(dispatch_input);
	}

	const auto encoded_payload_bytes = target_uses_lossy_compression(target)
	    ? encoded_payload_size_from_pixel_sequence(
	          *root_dataset_, path(), transfer_syntax)
	    : std::size_t{0};
	update_lossy_compression_metadata_for_set_pixel_data(
	    *root_dataset_, path(), transfer_syntax, target,
	    destination_total_bytes, encoded_payload_bytes);

	set_transfer_syntax_state_only(transfer_syntax);
	DataElement* transfer_syntax_element = add_dataelement("(0002,0010)"_tag, VR::UI);
	if (!transfer_syntax_element || !transfer_syntax_element->from_transfer_syntax_uid(transfer_syntax)) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} reason=failed to update (0002,0010) TransferSyntaxUID",
		    path());
	}
}

} // namespace dicom
