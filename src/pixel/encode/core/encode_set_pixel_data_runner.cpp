#include "pixel/encode/core/encode_set_pixel_data_runner.hpp"

#include "pixel/encode/core/encode_metadata_updater.hpp"
#include "pixel/encode/core/encode_source_layout_resolver.hpp"
#include "pixel/encode/core/encode_target_resolver.hpp"
#include "pixel/encode/core/multicomponent_option_resolver.hpp"
#include "pixel/encode/core/native_pixel_copy.hpp"
#include "pixel/encode/core/encode_codec_impl_detail.hpp"

#include <cstddef>

namespace dicom::pixel::detail {

void run_set_pixel_data_with_resolved_codec_options(DicomFile& file,
    uid::WellKnown transfer_syntax, const pixel::PixelSource& source,
    const TransferSyntaxPluginBinding& binding,
    std::span<const CodecOptionKv> codec_options) {
	const auto target = classify_pixel_encode_target(binding);
	const auto file_path = file.path();
	const auto source_layout =
	    resolve_encode_source_layout_or_throw(source, file_path);
	validate_target_source_constraints(
	    target, source_layout.bits_allocated, source_layout.bits_stored, file_path);
	const bool use_multicomponent_transform =
	    resolve_use_multicomponent_transform(transfer_syntax, target.is_j2k,
	        target.is_htj2k, codec_options, source_layout.samples_per_pixel,
	        file_path);
	const pixel::Photometric output_photometric = resolve_output_photometric(
	    target, use_multicomponent_transform, source.photometric);

	auto& dataset = file.dataset();
	dataset.ensure_loaded(Tag(0xFFFFu, 0xFFFFu));
	update_pixel_metadata_for_set_pixel_data(dataset, file_path, transfer_syntax, source,
	    target.is_rle, output_photometric, source_layout.bits_allocated,
	    source_layout.bits_stored, source_layout.high_bit,
	    source_layout.pixel_representation, source_layout.source_row_stride,
	    source_layout.source_frame_stride);

	// set_transfer_syntax(native->encapsulated) can pass a span that aliases current
	// native PixelData bytes. We must preserve those bytes until all frames are encoded.
	const bool source_aliases_current_native_pixel_data =
	    source_aliases_native_pixel_data(dataset, source.bytes);
	const EncapsulatedEncodeInput encapsulated_encode_input{
	    .source_base = source.bytes.data(),
	    .frame_count = source_layout.frames,
	    .source_frame_stride = source_layout.source_frame_stride,
	    .source_frame_size_bytes = source_layout.source_frame_size_bytes,
	    .source_aliases_current_native_pixel_data = source_aliases_current_native_pixel_data,
	};

	if (target.is_native_uncompressed) {
		const NativePixelCopyInput native_copy_input{
		    .source_bytes = source.bytes,
		    .rows = source_layout.rows,
		    .frames = source_layout.frames,
		    .samples_per_pixel = source_layout.samples_per_pixel,
		    .planar_source = source_layout.planar_source,
		    .row_payload_bytes = source_layout.row_payload_bytes,
		    .source_row_stride = source_layout.source_row_stride,
		    .source_plane_stride = source_layout.source_plane_stride,
		    .source_frame_stride = source_layout.source_frame_stride,
		    .destination_frame_payload = source_layout.destination_frame_payload,
		    .destination_total_bytes = source_layout.destination_total_bytes,
		};
		auto native_pixel_data = build_native_pixel_payload(native_copy_input);
		file.set_native_pixel_data(std::move(native_pixel_data));
	} else {
		const CodecEncodeFnInput dispatch_input{
		    .file = file,
		    .transfer_syntax = transfer_syntax,
		    .encode_input = encapsulated_encode_input,
		    .codec_options = codec_options,
		    .rows = source_layout.rows,
		    .cols = source_layout.cols,
		    .samples_per_pixel = source_layout.samples_per_pixel,
		    .bytes_per_sample = source_layout.bytes_per_sample,
		    .bits_allocated = source_layout.bits_allocated,
		    .bits_stored = source_layout.bits_stored,
		    .pixel_representation = source_layout.pixel_representation,
		    .use_multicomponent_transform = use_multicomponent_transform,
		    .source_planar = source.planar,
		    .planar_source = source_layout.planar_source,
		    .row_payload_bytes = source_layout.row_payload_bytes,
		    .source_row_stride = source_layout.source_row_stride,
		    .source_plane_stride = source_layout.source_plane_stride,
		    .source_frame_size_bytes = source_layout.source_frame_size_bytes,
		    .destination_frame_payload = source_layout.destination_frame_payload,
		    .profile = binding.profile,
		    .plugin_key = binding.plugin_key,
		};
		encode_encapsulated_pixel_data(dispatch_input);
	}

	const auto encoded_payload_bytes = target_uses_lossy_compression(target)
	    ? encoded_payload_size_from_pixel_sequence(dataset, file_path, transfer_syntax)
	    : std::size_t{0};
	update_lossy_compression_metadata_for_set_pixel_data(dataset, file_path,
	    transfer_syntax, target, source_layout.destination_total_bytes,
	    encoded_payload_bytes);
}

} // namespace dicom::pixel::detail
