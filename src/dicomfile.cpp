#include "dicom.h"

#include "dicom_endian.h"
#include "diagnostics.h"
#include "pixel/host/decode/decode_frame_dispatch.hpp"
#include "pixel/host/decode/decode_plan_compute.hpp"
#include "pixel/host/encode/encode_set_pixel_data_runner.hpp"
#include "pixel/host/support/dicom_pixel_support.hpp"

#include <array>
#include <cmath>
#include <exception>
#include <cstddef>
#include <limits>
#include <memory>
#include <optional>
#include <source_location>
#include <utility>

#include "charset/charset_mutation.hpp"

namespace dicom {
using namespace dicom::literals;

namespace {

class LastErrorCapturingReporter final : public diag::Reporter {
public:
	explicit LastErrorCapturingReporter(std::shared_ptr<diag::Reporter> downstream)
	    : downstream_(std::move(downstream)) {}

	void report(diag::LogLevel level, std::string_view message) override {
		if (level == diag::LogLevel::Error) {
			has_error_ = true;
			last_error_message_.assign(message);
		}
		if (downstream_) {
			downstream_->report(level, message);
		}
	}

	[[nodiscard]] bool has_error() const noexcept { return has_error_; }
	[[nodiscard]] const std::string& last_error_message() const noexcept {
		return last_error_message_;
	}

private:
	std::shared_ptr<diag::Reporter> downstream_;
	bool has_error_{false};
	std::string last_error_message_{};
};

class ThreadReporterGuard {
public:
	explicit ThreadReporterGuard(std::shared_ptr<diag::Reporter> reporter)
	    : previous_(diag::thread_reporter_slot()) {
		diag::set_thread_reporter(std::move(reporter));
	}

	~ThreadReporterGuard() {
		diag::set_thread_reporter(previous_);
	}

	ThreadReporterGuard(const ThreadReporterGuard&) = delete;
	ThreadReporterGuard& operator=(const ThreadReporterGuard&) = delete;

private:
	std::shared_ptr<diag::Reporter> previous_;
};

[[nodiscard]] std::string_view tidy_fn_name(
    std::source_location location) noexcept {
	auto name = std::string_view(location.function_name());
	const auto open_paren = name.find('(');
	if (open_paren != std::string_view::npos) {
		name = name.substr(0, open_paren);
	}
	const auto last_space = name.rfind(' ');
	if (last_space != std::string_view::npos) {
		name.remove_prefix(last_space + 1);
	}
	while (!name.empty() && (name.front() == '&' || name.front() == '*')) {
		name.remove_prefix(1);
	}
	return name;
}

[[nodiscard]] std::string sequence_name_from_tag(Tag sequence_tag) {
	const auto keyword = lookup::tag_to_keyword(sequence_tag.value());
	if (!keyword.empty()) {
		return std::string(keyword);
	}
	return sequence_tag.to_string();
}

[[nodiscard]] const std::string& dataset_file_path(const DataSet& dataset) noexcept {
	return dataset.root_dataset()->path();
}

[[nodiscard]] bool dataset_uses_little_endian(const DataSet& dataset) noexcept {
	return dataset.transfer_syntax_uid() != "ExplicitVRBigEndian"_uid;
}

[[nodiscard]] VR native_pixel_vr_from_bits_allocated(int bits_allocated) noexcept {
	return bits_allocated > 8 ? VR::OW : VR::OB;
}

[[nodiscard]] bool file_uses_little_endian(const DicomFile& file) noexcept {
	return file.transfer_syntax_uid() != "ExplicitVRBigEndian"_uid;
}

template <typename T>
[[nodiscard]] T load_dataset_endian_value(const std::uint8_t* ptr, bool little_endian) noexcept {
	return little_endian ? endian::load_le<T>(ptr) : endian::load_be<T>(ptr);
}

[[nodiscard]] char ascii_upper(char value) noexcept {
	return (value >= 'a' && value <= 'z')
	           ? static_cast<char>(value - ('a' - 'A'))
	           : value;
}

[[nodiscard]] bool ascii_iequals(std::string_view lhs, std::string_view rhs) noexcept {
	if (lhs.size() != rhs.size()) {
		return false;
	}
	for (std::size_t index = 0; index < lhs.size(); ++index) {
		if (ascii_upper(lhs[index]) != ascii_upper(rhs[index])) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] bool transfer_syntax_exception_has_boundary_prefix(
    std::string_view message) noexcept {
	return message.starts_with("DicomFile::set_transfer_syntax") ||
	    message.starts_with("DicomFile::apply_transfer_syntax");
}

[[noreturn]] void rethrow_transfer_syntax_exception_at_boundary_or_throw(
    const diag::DicomException& ex) {
	const std::string_view message = ex.what();
	if (transfer_syntax_exception_has_boundary_prefix(message)) {
		diag::error_and_throw("{}", message);
	}
	throw;
}

[[nodiscard]] std::optional<pixel::Photometric> parse_photometric_from_text(
    std::string_view text) noexcept {
	if (ascii_iequals(text, "MONOCHROME1")) {
		return pixel::Photometric::monochrome1;
	}
	if (ascii_iequals(text, "MONOCHROME2")) {
		return pixel::Photometric::monochrome2;
	}
	if (ascii_iequals(text, "PALETTE COLOR")) {
		return pixel::Photometric::palette_color;
	}
	if (ascii_iequals(text, "RGB")) {
		return pixel::Photometric::rgb;
	}
	if (ascii_iequals(text, "YBR_FULL")) {
		return pixel::Photometric::ybr_full;
	}
	if (ascii_iequals(text, "YBR_FULL_422")) {
		return pixel::Photometric::ybr_full_422;
	}
	if (ascii_iequals(text, "YBR_RCT")) {
		return pixel::Photometric::ybr_rct;
	}
	if (ascii_iequals(text, "YBR_ICT")) {
		return pixel::Photometric::ybr_ict;
	}
	return std::nullopt;
}

[[nodiscard]] std::optional<pixel::PixelPresentation> parse_pixel_presentation_from_text(
    std::string_view text) noexcept {
	if (ascii_iequals(text, "MONOCHROME")) {
		return pixel::PixelPresentation::monochrome;
	}
	if (ascii_iequals(text, "COLOR")) {
		return pixel::PixelPresentation::color;
	}
	if (ascii_iequals(text, "MIXED")) {
		return pixel::PixelPresentation::mixed;
	}
	if (ascii_iequals(text, "TRUE_COLOR")) {
		return pixel::PixelPresentation::true_color;
	}
	if (ascii_iequals(text, "COLOR_RANGE")) {
		return pixel::PixelPresentation::color_range;
	}
	if (ascii_iequals(text, "COLOR_REF")) {
		return pixel::PixelPresentation::color_ref;
	}
	return std::nullopt;
}

[[nodiscard]] std::optional<pixel::VoiLutFunction> parse_voi_lut_function_from_text(
    std::string_view text) noexcept {
	if (ascii_iequals(text, "LINEAR")) {
		return pixel::VoiLutFunction::linear;
	}
	if (ascii_iequals(text, "LINEAR_EXACT")) {
		return pixel::VoiLutFunction::linear_exact;
	}
	if (ascii_iequals(text, "SIGMOID")) {
		return pixel::VoiLutFunction::sigmoid;
	}
	return std::nullopt;
}

void validate_transform_metadata_frame_index_or_throw(const DataSet& dataset,
    std::size_t frame_index,
    std::source_location location = std::source_location::current()) {
	std::size_t frame_count = 1;
	if (const auto parsed_frames = dataset["NumberOfFrames"_tag].to_long();
	    parsed_frames.has_value()) {
		if (*parsed_frames <= 0) {
			diag::error_and_throw(
			    "{} file={} reason=NumberOfFrames must be >= 1 when present",
			    tidy_fn_name(location), dataset_file_path(dataset));
		}
		frame_count = static_cast<std::size_t>(*parsed_frames);
	}
	if (frame_index >= frame_count) {
		diag::error_and_throw(
		    "{} file={} frame_index={} frame_count={} reason=frame index out of range",
		    tidy_fn_name(location), dataset_file_path(dataset), frame_index,
		    frame_count);
	}
}

[[nodiscard]] const Sequence* lookup_sequence_or_throw(const DataSet& dataset,
    Tag sequence_tag,
    std::source_location location = std::source_location::current()) {
	const auto& sequence_elem = dataset[sequence_tag];
	if (!sequence_elem) {
		return nullptr;
	}

	const auto* sequence = sequence_elem.as_sequence();
	if (!sequence) {
		diag::error_and_throw(
		    "{} file={} reason={} is not a valid sequence",
		    tidy_fn_name(location), dataset_file_path(dataset),
		    sequence_name_from_tag(sequence_tag));
	}
	if (sequence->size() <= 0) {
		diag::error_and_throw(
		    "{} file={} reason={} is empty", tidy_fn_name(location),
		    dataset_file_path(dataset), sequence_name_from_tag(sequence_tag));
	}
	return sequence;
}

[[nodiscard]] const DataSet* lookup_first_nested_sequence_item_or_throw(
    const DataSet& dataset, Tag sequence_tag,
    std::source_location location = std::source_location::current()) {
	const auto* sequence = lookup_sequence_or_throw(dataset, sequence_tag, location);
	if (!sequence) {
		return nullptr;
	}

	const auto* item = sequence->get_dataset(0);
	if (!item) {
		diag::error_and_throw(
		    "{} file={} reason={} item #0 is missing",
		    tidy_fn_name(location), dataset_file_path(dataset),
		    sequence_name_from_tag(sequence_tag));
	}
	return item;
}

[[nodiscard]] const DataSet* resolve_frame_functional_group_macro_item_or_throw(
    const DataSet& dataset, Tag macro_sequence_tag, std::size_t frame_index,
    std::source_location location = std::source_location::current()) {
	// Frame-specific Functional Group items override shared entries when both
	// are available for the requested frame.
	if (const auto* per_frame_seq = lookup_sequence_or_throw(
	        dataset, "PerFrameFunctionalGroupsSequence"_tag, location)) {
		const auto* per_frame_item =
		    frame_index < static_cast<std::size_t>(per_frame_seq->size())
		        ? per_frame_seq->get_dataset(frame_index)
		        : nullptr;
		if (!per_frame_item) {
			diag::error_and_throw(
			    "{} file={} frame_index={} reason=PerFrameFunctionalGroupsSequence item is missing",
			    tidy_fn_name(location), dataset_file_path(dataset), frame_index);
		}
		if (const auto* per_frame_macro =
		        lookup_first_nested_sequence_item_or_throw(
		            *per_frame_item, macro_sequence_tag, location)) {
			return per_frame_macro;
		}
	}

	// Shared Functional Groups provide the fallback for every frame in the SOP
	// Instance when no per-frame override is present.
	if (const auto* shared_seq = lookup_sequence_or_throw(
	        dataset, "SharedFunctionalGroupsSequence"_tag, location)) {
		const auto* shared_item = shared_seq->get_dataset(0);
		if (!shared_item) {
			diag::error_and_throw(
			    "{} file={} reason=SharedFunctionalGroupsSequence item #0 is missing",
			    tidy_fn_name(location), dataset_file_path(dataset));
		}
		if (const auto* shared_macro = lookup_first_nested_sequence_item_or_throw(
		        *shared_item, macro_sequence_tag, location)) {
			return shared_macro;
		}
	}

	return nullptr;
}

[[nodiscard]] pixel::PixelBuffer decode_all_frames_to_native(DicomFile& file,
    const pixel::PixelLayout& source_layout,
    std::source_location location = std::source_location::current()) {
	// This helper materializes the full native pixel payload so transfer-syntax
	// mutations can reuse the normal encode path afterwards.
	if (source_layout.empty()) {
		diag::throw_exception(
		    "{} file={} reason=PixelData exists but pixel metadata is not decodable",
		    tidy_fn_name(location), file.path());
	}

	pixel::DecodeOptions decode_options{};
	decode_options.alignment = 1;
	decode_options.planar_out = source_layout.planar;
	// Keep codestream component domain during transfer-syntax transcoding.
	decode_options.decode_mct = false;
	const auto decode_plan = file.create_decode_plan(decode_options);
	if (decode_plan.output_layout.empty()) {
		diag::throw_exception(
		    "{} file={} reason=calculated native frame size is zero",
		    tidy_fn_name(location), file.path());
	}

	// Reuse the shared owning pixel container so decode helpers and transforms
	// can move the same type through the pipeline.
	auto decoded = pixel::PixelBuffer::allocate(decode_plan.output_layout);
	const auto frame_count = static_cast<std::size_t>(source_layout.frames);

	// Decode into one contiguous native buffer so downstream callers can treat it
	// exactly like ordinary native PixelData storage.
	auto decoded_view = decoded.span();
	for (std::size_t frame_index = 0; frame_index < frame_count; ++frame_index) {
		// Frame subviews preserve the planned single-frame layout automatically.
		auto frame_view = decoded_view.frame(frame_index);
		file.decode_into(frame_index, frame_view.bytes, decode_plan);
	}
	return decoded;
}

[[nodiscard]] pixel::ConstPixelSpan build_native_source_span_from_native_pixel_data(
    DicomFile& file, uid::WellKnown target_ts) {
	// Reconstruct a normalized native pixel span directly from on-disk PixelData.
	DataSet& dataset = file.dataset();
	const auto& pixel_data = dataset["PixelData"_tag];
	if (!pixel_data || pixel_data.vr().is_pixel_sequence() || !pixel_data.vr().is_binary()) {
		diag::throw_exception(
		    "DicomFile::apply_transfer_syntax file={} target_ts={} reason=PixelData must be native binary for encoding path",
		    file.path(), target_ts.value());
	}

	const auto layout = file.native_pixel_layout();
	if (!layout.has_value()) {
		diag::throw_exception(
		    "DicomFile::apply_transfer_syntax file={} target_ts={} reason=invalid native pixel metadata for normalized layout reconstruction",
		    file.path(), target_ts.value());
	}

	// Clamp the source span to the exact native payload size implied by the metadata.
	const auto source_bytes = pixel_data.value_span();

	std::size_t required_bytes = 0;
	if (!pixel::try_pixel_storage_size(*layout, required_bytes) ||
	    source_bytes.size() < required_bytes) {
		diag::throw_exception(
		    "DicomFile::apply_transfer_syntax file={} target_ts={} reason=PixelData bytes({}) are shorter than required native frame payload({})",
		    file.path(), target_ts.value(), source_bytes.size(), required_bytes);
	}

	return pixel::ConstPixelSpan{
	    .layout = *layout,
	    .bytes = source_bytes.first(required_bytes),
	};
}

[[nodiscard]] pixel::PixelLayout resolve_decoded_source_layout_or_throw(
    DicomFile& file, uid::WellKnown target_ts, const pixel::DecodePlan& decode_plan) {
	// This path describes a decode-on-demand source for re-encode without first
	// materializing a monolithic native PixelData element on the file.
	if (decode_plan.output_layout.empty()) {
		diag::throw_exception(
		    "DicomFile::apply_transfer_syntax file={} target_ts={} reason=decoded frame layout is empty",
		    file.path(), target_ts.value());
	}

	// The decode plan already carries the fully normalized native frame layout.
	return decode_plan.output_layout;
}

void convert_encapsulated_pixel_data_to_native(DicomFile& file) {
	DataSet& dataset = file.dataset();
	const auto& pixel_data = dataset["PixelData"_tag];
	if (!pixel_data || !pixel_data.vr().is_pixel_sequence()) {
		return;
	}

	const auto source_layout = pixel::support_detail::compute_decode_source_layout(file);
	// Decode every encapsulated frame and replace PixelData with one native binary blob.
	auto decoded = decode_all_frames_to_native(file, source_layout);
	const auto storage_bits = source_layout.bits_allocated();
	if (storage_bits <= 0) {
		diag::throw_exception(
		    "DicomFile::apply_transfer_syntax file={} reason=failed to resolve storage bits from source layout dtype={}",
		    file.path(), static_cast<int>(source_layout.data_type));
	}
	file.set_native_pixel_data(
	    std::move(decoded.bytes),
	    native_pixel_vr_from_bits_allocated(storage_bits));
}

enum class ApplyTransferSyntaxEncodeMode : std::uint8_t {
	use_plugin_defaults = 0,
	use_explicit_options,
	use_encoder_context,
};

void restore_encapsulated_pixel_sequence_after_failed_transcode_or_throw(
    DicomFile& file, std::unique_ptr<PixelSequence>&& snapshot,
    std::optional<long> original_number_of_frames) {
	if (!snapshot) {
		return;
	}

	auto& dataset = file.dataset();
	auto& pixel_data = dataset.add_dataelement("PixelData"_tag, VR::PX);
	auto* pixel_sequence = pixel_data.as_pixel_sequence();
	if (pixel_sequence == nullptr) {
		diag::throw_exception(
		    "DicomFile::apply_transfer_syntax file={} reason=failed to recreate encapsulated PixelData while restoring failed transcode",
		    file.path());
	}
	*pixel_sequence = std::move(*snapshot);

	if (original_number_of_frames.has_value()) {
		auto& number_of_frames = dataset.add_dataelement("NumberOfFrames"_tag, VR::IS);
		if (!number_of_frames.from_long(*original_number_of_frames)) {
			diag::throw_exception(
			    "DicomFile::apply_transfer_syntax file={} reason=failed to restore NumberOfFrames after failed encapsulated transcode",
			    file.path());
		}
	} else {
		dataset.remove_dataelement("NumberOfFrames"_tag);
	}
}

void transcode_encapsulated_pixel_data_to_encapsulated(DicomFile& file,
    uid::WellKnown target_transfer_syntax,
    ApplyTransferSyntaxEncodeMode encode_mode,
    std::span<const pixel::CodecOptionTextKv> codec_opt_override,
    const pixel::EncoderContext* encoder_ctx) {
	// Stream each source frame through decode -> encode so large encapsulated inputs
	// do not need an intermediate full-volume native allocation.
	const auto source_decode_layout =
	    pixel::support_detail::compute_decode_source_layout(file);
	pixel::DecodeOptions decode_options{};
	decode_options.alignment = 1;
	decode_options.planar_out = source_decode_layout.planar;
	decode_options.decode_mct = false;
	const auto decode_plan = file.create_decode_plan(decode_options);
	if (decode_plan.output_layout.empty()) {
		diag::throw_exception(
		    "DicomFile::apply_transfer_syntax file={} target_ts={} reason=calculated native frame size is zero for streaming transcode",
		    file.path(), target_transfer_syntax.value());
	}
	auto& source_pixel_data = file.dataset().get_dataelement("PixelData"_tag);
	if (source_pixel_data.is_missing() || !source_pixel_data.vr().is_pixel_sequence()) {
		diag::throw_exception(
		    "DicomFile::apply_transfer_syntax file={} target_ts={} reason=streaming transcode requires encapsulated PixelData source",
		    file.path(), target_transfer_syntax.value());
	}
	auto* source_pixel_sequence = source_pixel_data.as_pixel_sequence();
	if (source_pixel_sequence == nullptr) {
		diag::throw_exception(
		    "DicomFile::apply_transfer_syntax file={} target_ts={} reason=encapsulated PixelData sequence is missing during streaming transcode",
		    file.path(), target_transfer_syntax.value());
	}
	const auto original_number_of_frames =
	    file.dataset()["NumberOfFrames"_tag].to_long();

	auto source_layout = resolve_decoded_source_layout_or_throw(
	    file, target_transfer_syntax, decode_plan);
	std::vector<std::uint8_t> decoded_frame(decode_plan.output_layout.frame_stride);

	pixel::EncoderContext staged_encoder_ctx{};
	const pixel::EncoderContext* active_encoder_ctx = encoder_ctx;
	if (encode_mode == ApplyTransferSyntaxEncodeMode::use_plugin_defaults) {
		staged_encoder_ctx.configure(target_transfer_syntax);
		active_encoder_ctx = &staged_encoder_ctx;
	} else if (encode_mode == ApplyTransferSyntaxEncodeMode::use_explicit_options) {
		staged_encoder_ctx.configure(target_transfer_syntax, codec_opt_override);
		active_encoder_ctx = &staged_encoder_ctx;
	}
	if (active_encoder_ctx == nullptr) {
		diag::throw_exception(
		    "DicomFile::apply_transfer_syntax file={} target_ts={} reason=encoder context is missing for streaming transcode",
		    file.path(), target_transfer_syntax.value());
	}

	auto source_pixel_sequence_snapshot =
	    std::make_unique<PixelSequence>(std::move(*source_pixel_sequence));
	try {
		// The frame provider decodes one source frame on demand into a reusable buffer.
		pixel::detail::run_set_pixel_data_from_frame_provider_streaming_with_computed_codec_options(
		    file, target_transfer_syntax, source_layout,
		    active_encoder_ctx->codec_options(),
		    [&](std::size_t frame_index) -> std::span<const std::uint8_t> {
			    auto frame_span = std::span<std::uint8_t>(
			        decoded_frame.data(), decoded_frame.size());
			    auto prepared_source =
			        pixel::support_detail::prepare_decode_frame_source_or_throw(
			            *source_pixel_sequence_snapshot, frame_index);
			    pixel::detail::dispatch_decode_prepared_frame(
			        file, source_layout, frame_index, prepared_source.bytes, frame_span,
			        decode_plan);
			    source_pixel_sequence_snapshot->clear_frame_encoded_data(frame_index);
			    return std::span<const std::uint8_t>(
			        frame_span.data(), frame_span.size());
		    });
		source_pixel_sequence_snapshot.reset();
	} catch (...) {
		restore_encapsulated_pixel_sequence_after_failed_transcode_or_throw(
		    file, std::move(source_pixel_sequence_snapshot), original_number_of_frames);
		throw;
	}
}

[[nodiscard]] bool apply_transfer_syntax_impl(DicomFile& file,
    uid::WellKnown transfer_syntax,
    ApplyTransferSyntaxEncodeMode encode_mode,
    std::span<const pixel::CodecOptionTextKv> codec_opt_override,
    const pixel::EncoderContext* encoder_ctx) {
	const auto source_transfer_syntax = file.transfer_syntax_uid();
	DataSet& dataset = file.dataset();
	dataset.ensure_loaded(Tag(0xFFFFu, 0xFFFFu));
	const auto& pixel_data = dataset.get_dataelement("PixelData"_tag);
	const bool has_float_pixel_data =
	    dataset["FloatPixelData"_tag].is_present() ||
	    dataset["DoubleFloatPixelData"_tag].is_present();
	const bool has_pixel_data_element = !pixel_data.is_missing();
	const bool has_encapsulated_pixel_data = pixel_data.vr().is_pixel_sequence();
	const bool target_uses_encapsulated_pixel_data = transfer_syntax.is_encapsulated();

	if (target_uses_encapsulated_pixel_data && has_pixel_data_element) {
		const bool already_target_transfer_syntax = has_encapsulated_pixel_data &&
		    source_transfer_syntax.valid() && source_transfer_syntax == transfer_syntax;
		if (already_target_transfer_syntax) {
			// Keep existing encoded PixelData as-is.
		} else if (!has_encapsulated_pixel_data) {
			if (!transfer_syntax.supports_pixel_encode()) {
				diag::throw_exception(
				    "DicomFile::set_transfer_syntax file={} source_ts={} target_ts={} reason=target encapsulated transfer syntax does not support native pixel encoding",
				    file.path(), source_transfer_syntax.value(), transfer_syntax.value());
			}
				auto source =
				    build_native_source_span_from_native_pixel_data(
				        file, transfer_syntax);
				if (encode_mode == ApplyTransferSyntaxEncodeMode::use_plugin_defaults) {
					file.set_pixel_data(transfer_syntax, source);
				} else if (encode_mode == ApplyTransferSyntaxEncodeMode::use_encoder_context) {
					file.set_pixel_data(transfer_syntax, source, *encoder_ctx);
				} else {
					file.set_pixel_data(transfer_syntax, source, codec_opt_override);
				}
			return false;
		} else if (!transfer_syntax.supports_pixel_encode()) {
			diag::throw_exception(
			    "DicomFile::set_transfer_syntax file={} source_ts={} target_ts={} reason=target encapsulated transfer syntax does not support encapsulated-to-encapsulated transcoding",
			    file.path(), source_transfer_syntax.value(), transfer_syntax.value());
			} else {
				transcode_encapsulated_pixel_data_to_encapsulated(
				    file, transfer_syntax, encode_mode, codec_opt_override,
				    encoder_ctx);
				return true;
			}
		} else if (has_encapsulated_pixel_data && !target_uses_encapsulated_pixel_data) {
		convert_encapsulated_pixel_data_to_native(file);
	}
	if (target_uses_encapsulated_pixel_data && has_float_pixel_data) {
		diag::throw_exception(
		    "DicomFile::set_transfer_syntax file={} target_ts={} reason=FloatPixelData/DoubleFloatPixelData cannot be written with encapsulated transfer syntaxes",
		    file.path(), transfer_syntax.value());
	}

	return true;
}

struct LutDescriptorValues {
	std::size_t entry_count{0};
	std::int64_t first_mapped{0};
	std::uint32_t bits_per_entry{0};
};

bool try_parse_lut_descriptor(const DataElement& descriptor_elem, bool little_endian,
    LutDescriptorValues& out) noexcept {
	const auto span = descriptor_elem.value_span();
	if (span.size() < 6) {
		return false;
	}

	long raw_entries = 0;
	long raw_first_mapped = 0;
	long raw_bits = 0;
	const auto descriptor_vr = descriptor_elem.vr();
	if (descriptor_vr == VR::US) {
		raw_entries = static_cast<long>(
		    load_dataset_endian_value<std::uint16_t>(span.data(), little_endian));
		raw_first_mapped = static_cast<long>(
		    load_dataset_endian_value<std::uint16_t>(span.data() + 2, little_endian));
		raw_bits = static_cast<long>(
		    load_dataset_endian_value<std::uint16_t>(span.data() + 4, little_endian));
	} else if (descriptor_vr == VR::SS) {
		raw_entries = static_cast<long>(
		    load_dataset_endian_value<std::int16_t>(span.data(), little_endian));
		raw_first_mapped = static_cast<long>(
		    load_dataset_endian_value<std::int16_t>(span.data() + 2, little_endian));
		raw_bits = static_cast<long>(
		    load_dataset_endian_value<std::int16_t>(span.data() + 4, little_endian));
	} else {
		const auto descriptor = descriptor_elem.to_long_vector();
		if (!descriptor || descriptor->size() < 3) {
			return false;
		}
		raw_entries = (*descriptor)[0];
		raw_first_mapped = (*descriptor)[1];
		raw_bits = (*descriptor)[2];
	}

	if (raw_entries < 0 || raw_bits <= 0 || raw_bits > 16) {
		return false;
	}

	out.entry_count = (raw_entries == 0) ? std::size_t{65536} : static_cast<std::size_t>(raw_entries);
	out.first_mapped = static_cast<std::int64_t>(raw_first_mapped);
	out.bits_per_entry = static_cast<std::uint32_t>(raw_bits);
	return out.entry_count > 0;
}

[[nodiscard]] std::vector<std::uint16_t> load_discrete_lut_data_or_throw(
    const DataSet& dataset, const LutDescriptorValues& descriptor,
    const DataElement& lut_data_elem,
    std::source_location location = std::source_location::current()) {
	const bool little_endian = dataset_uses_little_endian(dataset);
	const auto lut_data = lut_data_elem.value_span();
	if (lut_data.empty()) {
		diag::error_and_throw(
		    "{} file={} reason=empty LUT data", tidy_fn_name(location),
		    dataset_file_path(dataset));
	}

	std::vector<std::uint16_t> values(descriptor.entry_count, std::uint16_t{0});
	const std::uint32_t value_mask =
	    descriptor.bits_per_entry == 16
	        ? 0xFFFFu
	        : ((1u << descriptor.bits_per_entry) - 1u);
	const auto entry_count = descriptor.entry_count;

	// Accept both byte-packed and 16-bit container forms for <=8-bit LUT payloads.
	if (descriptor.bits_per_entry <= 8 &&
	    lut_data.size() >= entry_count * sizeof(std::uint16_t)) {
		for (std::size_t index = 0; index < entry_count; ++index) {
			const auto value = load_dataset_endian_value<std::uint16_t>(
			    lut_data.data() + index * sizeof(std::uint16_t), little_endian);
			values[index] = static_cast<std::uint16_t>(value & value_mask);
		}
		return values;
	}
	if (descriptor.bits_per_entry <= 8 && lut_data.size() >= entry_count) {
		for (std::size_t index = 0; index < entry_count; ++index) {
			values[index] = static_cast<std::uint16_t>(
			    static_cast<std::uint32_t>(lut_data[index]) & value_mask);
		}
		return values;
	}

	const auto required_u16_bytes = entry_count * sizeof(std::uint16_t);
	if (lut_data.size() < required_u16_bytes) {
		diag::error_and_throw(
		    "{} file={} reason=LUTData is shorter than LUTDescriptor entry count",
		    tidy_fn_name(location), dataset_file_path(dataset));
	}
	for (std::size_t index = 0; index < entry_count; ++index) {
		const auto value = load_dataset_endian_value<std::uint16_t>(
		    lut_data.data() + index * sizeof(std::uint16_t), little_endian);
		values[index] = static_cast<std::uint16_t>(value & value_mask);
	}
	return values;
}

[[nodiscard]] std::vector<std::uint16_t> expand_segmented_lut_data_or_throw(
    const DataSet& dataset, std::span<const std::uint32_t> data,
    const LutDescriptorValues& descriptor, bool little_endian,
    std::optional<std::size_t> segment_limit = std::nullopt,
    std::optional<std::uint16_t> previous_value = std::nullopt,
    std::source_location location = std::source_location::current()) {
	std::vector<std::uint16_t> values;
	std::size_t offset = 0;
	std::size_t segments_read = 0;
	const auto value_mask = descriptor.bits_per_entry == 16
	                            ? std::uint32_t{0xFFFFu}
	                            : ((std::uint32_t{1u} << descriptor.bits_per_entry) - 1u);
	const bool byte_entries = descriptor.bits_per_entry <= 8;

	// Segments are at least two entries long (opcode + length), so offset+1 is
	// enough to tolerate a trailing pad byte in 8-bit payloads.
	while ((offset + 1) < data.size()) {
		const auto opcode = data[offset];
		const auto length = static_cast<std::size_t>(data[offset + 1]);
		offset += 2;

		if (opcode == 0u) {
			// Discrete segment: copy `length` explicit values.
			if ((offset + length) > data.size()) {
				diag::error_and_throw(
				    "{} file={} reason=segmented palette discrete segment is truncated",
				    tidy_fn_name(location), dataset_file_path(dataset));
			}
			for (std::size_t index = 0; index < length; ++index) {
				values.push_back(static_cast<std::uint16_t>(data[offset + index] & value_mask));
			}
			offset += length;
		} else if (opcode == 1u) {
			// Linear segment: interpolate `length` values from the previous sample.
			const std::uint32_t y0 = !values.empty()
			                             ? values.back()
			                             : (previous_value.has_value()
			                                    ? static_cast<std::uint32_t>(*previous_value)
			                                    : std::numeric_limits<std::uint32_t>::max());
			if (y0 == std::numeric_limits<std::uint32_t>::max()) {
				diag::error_and_throw(
				    "{} file={} reason=segmented palette linear segment has no previous sample",
				    tidy_fn_name(location), dataset_file_path(dataset));
			}
			if (offset >= data.size()) {
				diag::error_and_throw(
				    "{} file={} reason=segmented palette linear segment is truncated",
				    tidy_fn_name(location), dataset_file_path(dataset));
			}
			const auto y1 = data[offset] & value_mask;
			++offset;
			if (length == 0) {
				// Zero-length segments are a no-op but still count as one segment.
			} else if (y0 == y1) {
				values.insert(values.end(), length, static_cast<std::uint16_t>(y1));
			} else {
				const double step =
				    (static_cast<double>(y1) - static_cast<double>(y0)) /
				    static_cast<double>(length);
				for (std::size_t index = 1; index <= length; ++index) {
					const auto interpolated = static_cast<std::uint32_t>(
					    std::lround(static_cast<double>(y0) + (step * static_cast<double>(index))));
					values.push_back(static_cast<std::uint16_t>(interpolated & value_mask));
				}
			}
		} else if (opcode == 2u) {
			// Indirect segment: jump to another segment stream and expand `length`
			// segments from there, using the current tail sample as the prior value.
			if (values.empty()) {
				diag::error_and_throw(
				    "{} file={} reason=segmented palette indirect segment has no previous sample",
				    tidy_fn_name(location), dataset_file_path(dataset));
			}

			std::size_t segment_offset = 0;
			if (byte_entries) {
				if ((offset + 4) > data.size()) {
					diag::error_and_throw(
					    "{} file={} reason=segmented palette indirect segment is truncated",
					    tidy_fn_name(location), dataset_file_path(dataset));
				}
				const auto b0 = data[offset + 0];
				const auto b1 = data[offset + 1];
				const auto b2 = data[offset + 2];
				const auto b3 = data[offset + 3];
				segment_offset = little_endian
				                     ? static_cast<std::size_t>(
				                           b0 | (b1 << 8) | (b2 << 16) | (b3 << 24))
				                     : static_cast<std::size_t>(
				                           (b0 << 24) | (b1 << 16) | (b2 << 8) | b3);
				offset += 4;
			} else {
				if ((offset + 2) > data.size()) {
					diag::error_and_throw(
					    "{} file={} reason=segmented palette indirect segment is truncated",
					    tidy_fn_name(location), dataset_file_path(dataset));
				}
				segment_offset = static_cast<std::size_t>(
				    data[offset] | (data[offset + 1] << 16));
				offset += 2;
			}
			if (segment_offset >= data.size()) {
				diag::error_and_throw(
				    "{} file={} reason=segmented palette indirect segment offset is out of range",
				    tidy_fn_name(location), dataset_file_path(dataset));
			}
			auto nested = expand_segmented_lut_data_or_throw(
			    dataset, data.subspan(segment_offset), descriptor, little_endian, length,
			    values.back(), location);
			values.insert(values.end(), nested.begin(), nested.end());
		} else {
			diag::error_and_throw(
			    "{} file={} reason=segmented palette opcode {} is not supported",
			    tidy_fn_name(location), dataset_file_path(dataset), opcode);
		}

		++segments_read;
		if (segment_limit.has_value() && segments_read == *segment_limit) {
			return values;
		}
	}

	return values;
}

[[nodiscard]] std::vector<std::uint16_t> load_segmented_lut_data_or_throw(
    const DataSet& dataset, const LutDescriptorValues& descriptor,
    const DataElement& lut_data_elem,
    std::source_location location = std::source_location::current()) {
	const auto lut_data = lut_data_elem.value_span();
	if (lut_data.empty()) {
		diag::error_and_throw(
		    "{} file={} reason=empty segmented LUT data",
		    tidy_fn_name(location),
		    dataset_file_path(dataset));
	}

	const bool little_endian = dataset_uses_little_endian(dataset);
	std::vector<std::uint32_t> packed_values{};
	if (descriptor.bits_per_entry <= 8) {
		packed_values.reserve(lut_data.size());
		for (std::uint8_t value : lut_data) {
			packed_values.push_back(static_cast<std::uint32_t>(value));
		}
	} else if (descriptor.bits_per_entry == 16) {
		if ((lut_data.size() % sizeof(std::uint16_t)) != 0) {
			diag::error_and_throw(
			    "{} file={} reason=16-bit segmented LUT data has odd byte count",
			    tidy_fn_name(location), dataset_file_path(dataset));
		}
		packed_values.reserve(lut_data.size() / sizeof(std::uint16_t));
		for (std::size_t index = 0; index < lut_data.size(); index += sizeof(std::uint16_t)) {
			packed_values.push_back(static_cast<std::uint32_t>(
			    load_dataset_endian_value<std::uint16_t>(lut_data.data() + index, little_endian)));
		}
	} else {
		diag::error_and_throw(
		    "{} file={} reason=segmented LUT supports only 8-bit or 16-bit entries",
		    tidy_fn_name(location), dataset_file_path(dataset));
	}

	auto values = expand_segmented_lut_data_or_throw(
	    dataset, packed_values, descriptor, little_endian, std::nullopt, std::nullopt,
	    location);
	if (values.size() != descriptor.entry_count) {
		diag::error_and_throw(
		    "{} file={} reason=segmented LUT expanded {} entries but LUTDescriptor requires {}",
		    tidy_fn_name(location), dataset_file_path(dataset), values.size(),
		    descriptor.entry_count);
	}
	return values;
}

struct ParsedPaletteChannels {
	pixel::PaletteLut palette{};
};

struct StoredValueColorRangeValues {
	double minimum_stored_value_mapped{0.0};
	double maximum_stored_value_mapped{0.0};
};

[[nodiscard]] std::optional<pixel::Photometric> parse_photometric_from_dataset_or_throw(
    const DataSet& dataset,
    std::source_location location = std::source_location::current()) {
	const auto& photometric_elem = dataset["PhotometricInterpretation"_tag];
	if (!photometric_elem) {
		return std::nullopt;
	}

	const auto photometric_text = photometric_elem.to_string_view();
	if (!photometric_text || photometric_text->empty()) {
		diag::error_and_throw(
		    "{} file={} reason=invalid PhotometricInterpretation",
		    tidy_fn_name(location), dataset_file_path(dataset));
	}
	const auto parsed = parse_photometric_from_text(*photometric_text);
	if (!parsed) {
		diag::error_and_throw(
		    "{} file={} reason=unsupported PhotometricInterpretation",
		    tidy_fn_name(location), dataset_file_path(dataset));
	}
	return parsed;
}

[[nodiscard]] std::optional<pixel::PixelPresentation> parse_pixel_presentation_from_dataset_or_throw(
    const DataSet& dataset,
    std::source_location location = std::source_location::current()) {
	const auto& presentation_elem = dataset["PixelPresentation"_tag];
	if (!presentation_elem) {
		return std::nullopt;
	}

	const auto presentation_text = presentation_elem.to_string_view();
	if (!presentation_text || presentation_text->empty()) {
		diag::error_and_throw(
		    "{} file={} reason=invalid PixelPresentation",
		    tidy_fn_name(location), dataset_file_path(dataset));
	}
	const auto parsed = parse_pixel_presentation_from_text(*presentation_text);
	if (!parsed) {
		diag::error_and_throw(
		    "{} file={} reason=unsupported PixelPresentation",
		    tidy_fn_name(location), dataset_file_path(dataset));
	}
	return parsed;
}

[[nodiscard]] std::optional<StoredValueColorRangeValues>
parse_stored_value_color_range_from_dataset_or_throw(
    const DataSet& dataset,
    std::source_location location = std::source_location::current()) {
	const auto* item = lookup_first_nested_sequence_item_or_throw(
	    dataset, "StoredValueColorRangeSequence"_tag, location);
	if (!item) {
		return std::nullopt;
	}

	const auto& minimum_elem = (*item)["MinimumStoredValueMapped"_tag];
	const auto& maximum_elem = (*item)["MaximumStoredValueMapped"_tag];
	if (!minimum_elem || !maximum_elem) {
		diag::error_and_throw(
		    "{} file={} reason=StoredValueColorRangeSequence item #0 is incomplete",
		    tidy_fn_name(location), dataset_file_path(dataset));
	}

	const auto minimum_value = minimum_elem.to_double();
	const auto maximum_value = maximum_elem.to_double();
	if (!minimum_value || !maximum_value ||
	    !std::isfinite(*minimum_value) || !std::isfinite(*maximum_value)) {
		diag::error_and_throw(
		    "{} file={} reason=invalid StoredValueColorRangeSequence values",
		    tidy_fn_name(location), dataset_file_path(dataset));
	}

	return StoredValueColorRangeValues{
	    .minimum_stored_value_mapped = *minimum_value,
	    .maximum_stored_value_mapped = *maximum_value,
	};
}

[[nodiscard]] bool dataset_has_enhanced_palette_metadata(const DataSet& dataset) noexcept {
	return dataset["DataFrameAssignmentSequence"_tag] ||
	    dataset["BlendingLUT1Sequence"_tag] ||
	    dataset["BlendingLUT2Sequence"_tag] ||
	    dataset["EnhancedPaletteColorLookupTableSequence"_tag];
}

[[nodiscard]] std::optional<ParsedPaletteChannels> parse_palette_channels_from_dataset_or_throw(
    const DataSet& dataset,
    std::source_location location = std::source_location::current()) {
	const auto& red_descriptor_elem = dataset["RedPaletteColorLookupTableDescriptor"_tag];
	const auto& green_descriptor_elem = dataset["GreenPaletteColorLookupTableDescriptor"_tag];
	const auto& blue_descriptor_elem = dataset["BluePaletteColorLookupTableDescriptor"_tag];
	const auto& alpha_descriptor_elem = dataset["AlphaPaletteColorLookupTableDescriptor"_tag];
	const auto& red_data_elem = dataset["RedPaletteColorLookupTableData"_tag];
	const auto& green_data_elem = dataset["GreenPaletteColorLookupTableData"_tag];
	const auto& blue_data_elem = dataset["BluePaletteColorLookupTableData"_tag];
	const auto& alpha_data_elem = dataset["AlphaPaletteColorLookupTableData"_tag];
	const auto& segmented_red_data_elem =
	    dataset["SegmentedRedPaletteColorLookupTableData"_tag];
	const auto& segmented_green_data_elem =
	    dataset["SegmentedGreenPaletteColorLookupTableData"_tag];
	const auto& segmented_blue_data_elem =
	    dataset["SegmentedBluePaletteColorLookupTableData"_tag];
	const auto& segmented_alpha_data_elem =
	    dataset["SegmentedAlphaPaletteColorLookupTableData"_tag];

	const bool has_rgb_descriptors =
	    red_descriptor_elem && green_descriptor_elem && blue_descriptor_elem;
	const bool has_discrete_palette = has_rgb_descriptors &&
	    red_data_elem && green_data_elem && blue_data_elem;
	const bool has_segmented_palette = has_rgb_descriptors &&
	    segmented_red_data_elem && segmented_green_data_elem && segmented_blue_data_elem;
	const bool has_any_rgb_tag = red_descriptor_elem || green_descriptor_elem || blue_descriptor_elem ||
	    red_data_elem || green_data_elem || blue_data_elem ||
	    segmented_red_data_elem || segmented_green_data_elem || segmented_blue_data_elem;
	const bool has_discrete_alpha = alpha_descriptor_elem && alpha_data_elem;
	const bool has_segmented_alpha = alpha_descriptor_elem && segmented_alpha_data_elem;
	const bool has_any_alpha_tag =
	    alpha_descriptor_elem || alpha_data_elem || segmented_alpha_data_elem;
	if (!has_any_rgb_tag && !has_any_alpha_tag) {
		return std::nullopt;
	}
	if (!has_discrete_palette && !has_segmented_palette) {
		diag::error_and_throw(
		    "{} file={} reason=palette LUT metadata is incomplete",
		    tidy_fn_name(location),
		    dataset_file_path(dataset));
	}
	if (has_discrete_palette &&
	    (segmented_red_data_elem || segmented_green_data_elem || segmented_blue_data_elem)) {
		diag::error_and_throw(
		    "{} file={} reason=discrete and segmented palette data must not both be present",
		    tidy_fn_name(location), dataset_file_path(dataset));
	}
	if (has_any_alpha_tag && !has_discrete_alpha && !has_segmented_alpha) {
		diag::error_and_throw(
		    "{} file={} reason=alpha palette metadata is incomplete",
		    tidy_fn_name(location), dataset_file_path(dataset));
	}
	if (has_discrete_alpha && segmented_alpha_data_elem) {
		diag::error_and_throw(
		    "{} file={} reason=discrete and segmented alpha palette data must not both be present",
		    tidy_fn_name(location), dataset_file_path(dataset));
	}

	LutDescriptorValues red_descriptor{};
	LutDescriptorValues green_descriptor{};
	LutDescriptorValues blue_descriptor{};
	const bool little_endian = dataset_uses_little_endian(dataset);
	if (!try_parse_lut_descriptor(red_descriptor_elem, little_endian, red_descriptor) ||
	    !try_parse_lut_descriptor(green_descriptor_elem, little_endian, green_descriptor) ||
	    !try_parse_lut_descriptor(blue_descriptor_elem, little_endian, blue_descriptor)) {
		diag::error_and_throw(
		    "{} file={} reason=invalid palette LUT descriptor",
		    tidy_fn_name(location),
		    dataset_file_path(dataset));
	}
	if (red_descriptor.entry_count != green_descriptor.entry_count ||
	    red_descriptor.entry_count != blue_descriptor.entry_count ||
	    red_descriptor.first_mapped != green_descriptor.first_mapped ||
	    red_descriptor.first_mapped != blue_descriptor.first_mapped ||
	    red_descriptor.bits_per_entry != green_descriptor.bits_per_entry ||
	    red_descriptor.bits_per_entry != blue_descriptor.bits_per_entry) {
		diag::error_and_throw(
		    "{} file={} reason=palette LUT descriptors are inconsistent",
		    tidy_fn_name(location), dataset_file_path(dataset));
	}

	ParsedPaletteChannels parsed{};
	parsed.palette.first_mapped = red_descriptor.first_mapped;
	parsed.palette.bits_per_entry = static_cast<std::uint16_t>(red_descriptor.bits_per_entry);
	if (has_discrete_palette) {
		parsed.palette.red_values = load_discrete_lut_data_or_throw(
		    dataset, red_descriptor, red_data_elem, location);
		parsed.palette.green_values = load_discrete_lut_data_or_throw(
		    dataset, green_descriptor, green_data_elem, location);
		parsed.palette.blue_values = load_discrete_lut_data_or_throw(
		    dataset, blue_descriptor, blue_data_elem, location);
	} else {
		parsed.palette.red_values = load_segmented_lut_data_or_throw(
		    dataset, red_descriptor, segmented_red_data_elem, location);
		parsed.palette.green_values = load_segmented_lut_data_or_throw(
		    dataset, green_descriptor, segmented_green_data_elem, location);
		parsed.palette.blue_values = load_segmented_lut_data_or_throw(
		    dataset, blue_descriptor, segmented_blue_data_elem, location);
	}

	if (has_discrete_alpha || has_segmented_alpha) {
		LutDescriptorValues alpha_descriptor{};
		if (!try_parse_lut_descriptor(alpha_descriptor_elem, little_endian, alpha_descriptor)) {
			diag::error_and_throw(
			    "{} file={} reason=invalid alpha palette LUT descriptor",
			    tidy_fn_name(location), dataset_file_path(dataset));
		}
		if (alpha_descriptor.entry_count != red_descriptor.entry_count ||
		    alpha_descriptor.first_mapped != red_descriptor.first_mapped ||
		    alpha_descriptor.bits_per_entry != red_descriptor.bits_per_entry) {
			diag::error_and_throw(
			    "{} file={} reason=alpha palette LUT descriptor is inconsistent with RGB descriptors",
			    tidy_fn_name(location), dataset_file_path(dataset));
		}
		if (has_discrete_alpha) {
			parsed.palette.alpha_values = load_discrete_lut_data_or_throw(
			    dataset, alpha_descriptor, alpha_data_elem, location);
		} else {
			parsed.palette.alpha_values = load_segmented_lut_data_or_throw(
			    dataset, alpha_descriptor, segmented_alpha_data_elem, location);
		}
	}

	return parsed;
}

[[nodiscard]] std::vector<pixel::EnhancedPaletteDataPathAssignmentInfo>
parse_enhanced_data_frame_assignments_or_throw(
    const DataSet& dataset,
    std::source_location location = std::source_location::current()) {
	const auto* sequence = lookup_sequence_or_throw(
	    dataset, "DataFrameAssignmentSequence"_tag, location);
	if (!sequence) {
		return {};
	}

	std::vector<pixel::EnhancedPaletteDataPathAssignmentInfo> assignments{};
	assignments.reserve(static_cast<std::size_t>(sequence->size()));
	for (int item_index = 0; item_index < sequence->size(); ++item_index) {
		const auto* item = sequence->get_dataset(static_cast<std::size_t>(item_index));
		if (!item) {
			diag::error_and_throw(
			    "{} file={} reason=DataFrameAssignmentSequence item #{} is missing",
			    tidy_fn_name(location), dataset_file_path(dataset),
			    item_index);
		}

		pixel::EnhancedPaletteDataPathAssignmentInfo assignment{};
		if (const auto& data_type_elem = (*item)["DataType"_tag]; data_type_elem) {
			const auto data_type_text = data_type_elem.to_string_view();
			if (!data_type_text || data_type_text->empty()) {
				diag::error_and_throw(
				    "{} file={} reason=DataFrameAssignmentSequence item #{} has invalid DataType",
				    tidy_fn_name(location), dataset_file_path(dataset),
				    item_index);
			}
			assignment.data_type.assign(data_type_text->data(), data_type_text->size());
		}

		const auto& data_path_assignment_elem = (*item)["DataPathAssignment"_tag];
		if (!data_path_assignment_elem) {
			diag::error_and_throw(
			    "{} file={} reason=DataFrameAssignmentSequence item #{} missing DataPathAssignment",
			    tidy_fn_name(location), dataset_file_path(dataset),
			    item_index);
		}
		const auto data_path_assignment_text = data_path_assignment_elem.to_string_view();
		if (!data_path_assignment_text || data_path_assignment_text->empty()) {
			diag::error_and_throw(
			    "{} file={} reason=DataFrameAssignmentSequence item #{} has invalid DataPathAssignment",
			    tidy_fn_name(location), dataset_file_path(dataset),
			    item_index);
		}
		assignment.data_path_assignment.assign(
		    data_path_assignment_text->data(), data_path_assignment_text->size());

		if (const auto& bits_elem = (*item)["BitsMappedToColorLookupTable"_tag]; bits_elem) {
			const auto bits_value = bits_elem.to_int();
			if (!bits_value || *bits_value < 0 || *bits_value > 65535) {
				diag::error_and_throw(
				    "{} file={} reason=DataFrameAssignmentSequence item #{} has invalid BitsMappedToColorLookupTable",
				    tidy_fn_name(location), dataset_file_path(dataset),
				    item_index);
			}
			assignment.has_bits_mapped_to_color_lookup_table = true;
			assignment.bits_mapped_to_color_lookup_table =
			    static_cast<std::uint16_t>(*bits_value);
		}

		assignments.push_back(std::move(assignment));
	}
	return assignments;
}

[[nodiscard]] std::optional<pixel::EnhancedBlendingLutInfo>
parse_enhanced_blending_lut_or_throw(const DataSet& dataset,
    Tag sequence_tag, Tag transfer_function_tag,
    std::source_location location = std::source_location::current()) {
	const auto* item = lookup_first_nested_sequence_item_or_throw(
	    dataset, sequence_tag, location);
	if (!item) {
		return std::nullopt;
	}

	pixel::EnhancedBlendingLutInfo info{};
	const auto& transfer_elem = (*item)[transfer_function_tag];
	if (!transfer_elem) {
		diag::error_and_throw(
		    "{} file={} reason={} item #0 missing transfer function",
		    tidy_fn_name(location), dataset_file_path(dataset),
		    sequence_name_from_tag(sequence_tag));
	}
	const auto transfer_text = transfer_elem.to_string_view();
	if (!transfer_text || transfer_text->empty()) {
		diag::error_and_throw(
		    "{} file={} reason={} item #0 has invalid transfer function",
		    tidy_fn_name(location), dataset_file_path(dataset),
		    sequence_name_from_tag(sequence_tag));
	}
	info.transfer_function.assign(transfer_text->data(), transfer_text->size());

	if (const auto& weight_elem = (*item)["BlendingWeightConstant"_tag]; weight_elem) {
		const auto weight_value = weight_elem.to_double();
		if (!weight_value || !std::isfinite(*weight_value)) {
			diag::error_and_throw(
			    "{} file={} reason={} item #0 has invalid BlendingWeightConstant",
			    tidy_fn_name(location), dataset_file_path(dataset),
			    sequence_name_from_tag(sequence_tag));
		}
		info.has_weight_constant = true;
		info.weight_constant = *weight_value;
	}

	if (ascii_iequals(info.transfer_function, "TABLE")) {
		const auto& descriptor_elem = (*item)["BlendingLookupTableDescriptor"_tag];
		const auto& data_elem = (*item)["BlendingLookupTableData"_tag];
		if (!descriptor_elem || !data_elem) {
			diag::error_and_throw(
			    "{} file={} reason={} item #0 missing blending LUT table metadata",
			    tidy_fn_name(location), dataset_file_path(dataset),
			    sequence_name_from_tag(sequence_tag));
		}

		LutDescriptorValues descriptor{};
		if (!try_parse_lut_descriptor(
		        descriptor_elem, dataset_uses_little_endian(dataset), descriptor)) {
			diag::error_and_throw(
			    "{} file={} reason={} item #0 has invalid blending LUT descriptor",
			    tidy_fn_name(location), dataset_file_path(dataset),
			    sequence_name_from_tag(sequence_tag));
		}
		info.bits_per_entry = static_cast<std::uint16_t>(descriptor.bits_per_entry);
		info.values =
		    load_discrete_lut_data_or_throw(*item, descriptor, data_elem, location);
	}

	return info;
}

[[nodiscard]] std::vector<pixel::EnhancedPaletteItemInfo>
parse_enhanced_palette_items_or_throw(
    const DataSet& dataset,
    std::source_location location = std::source_location::current()) {
	const auto* sequence = lookup_sequence_or_throw(
	    dataset, "EnhancedPaletteColorLookupTableSequence"_tag, location);
	if (!sequence) {
		return {};
	}

	std::vector<pixel::EnhancedPaletteItemInfo> items{};
	items.reserve(static_cast<std::size_t>(sequence->size()));
	for (int item_index = 0; item_index < sequence->size(); ++item_index) {
		const auto* item = sequence->get_dataset(static_cast<std::size_t>(item_index));
		if (!item) {
			diag::error_and_throw(
			    "{} file={} reason=EnhancedPaletteColorLookupTableSequence item #{} is missing",
			    tidy_fn_name(location), dataset_file_path(dataset),
			    item_index);
		}

		const auto parsed_channels = parse_palette_channels_from_dataset_or_throw(
		    *item, location);
		if (!parsed_channels) {
			diag::error_and_throw(
			    "{} file={} reason=EnhancedPaletteColorLookupTableSequence item #{} missing palette LUT data",
			    tidy_fn_name(location), dataset_file_path(dataset),
			    item_index);
		}

		pixel::EnhancedPaletteItemInfo info{};
		const auto& data_path_id_elem = (*item)["DataPathID"_tag];
		if (!data_path_id_elem) {
			diag::error_and_throw(
			    "{} file={} reason=EnhancedPaletteColorLookupTableSequence item #{} missing DataPathID",
			    tidy_fn_name(location), dataset_file_path(dataset),
			    item_index);
		}
		const auto data_path_id_text = data_path_id_elem.to_string_view();
		if (!data_path_id_text || data_path_id_text->empty()) {
			diag::error_and_throw(
			    "{} file={} reason=EnhancedPaletteColorLookupTableSequence item #{} has invalid DataPathID",
			    tidy_fn_name(location), dataset_file_path(dataset),
			    item_index);
		}
		info.data_path_id.assign(data_path_id_text->data(), data_path_id_text->size());

		if (const auto& rgb_tf_elem = (*item)["RGBLUTTransferFunction"_tag]; rgb_tf_elem) {
			const auto rgb_tf_text = rgb_tf_elem.to_string_view();
			if (!rgb_tf_text || rgb_tf_text->empty()) {
				diag::error_and_throw(
				    "{} file={} reason=EnhancedPaletteColorLookupTableSequence item #{} has invalid RGBLUTTransferFunction",
				    tidy_fn_name(location), dataset_file_path(dataset),
				    item_index);
			}
			info.rgb_lut_transfer_function.assign(rgb_tf_text->data(), rgb_tf_text->size());
		}
		if (const auto& alpha_tf_elem = (*item)["AlphaLUTTransferFunction"_tag]; alpha_tf_elem) {
			const auto alpha_tf_text = alpha_tf_elem.to_string_view();
			if (!alpha_tf_text || alpha_tf_text->empty()) {
				diag::error_and_throw(
				    "{} file={} reason=EnhancedPaletteColorLookupTableSequence item #{} has invalid AlphaLUTTransferFunction",
				    tidy_fn_name(location), dataset_file_path(dataset),
				    item_index);
			}
			info.alpha_lut_transfer_function.assign(
			    alpha_tf_text->data(), alpha_tf_text->size());
		}

		info.palette = parsed_channels->palette;
		items.push_back(std::move(info));
	}
	return items;
}

[[nodiscard]] std::optional<pixel::VoiLut> parse_voi_lut_from_dataset_or_throw(
    const DataSet& dataset,
    std::source_location location = std::source_location::current()) {
	if (!dataset["VOILUTSequence"_tag]) {
		return std::nullopt;
	}

	const auto* item = lookup_first_nested_sequence_item_or_throw(
	    dataset, "VOILUTSequence"_tag, location);
	if (!item) {
		return std::nullopt;
	}

	const auto& descriptor_elem = (*item)["LUTDescriptor"_tag];
	if (!descriptor_elem) {
		diag::error_and_throw(
		    "{} file={} reason=VOILUTSequence item #0 missing LUTDescriptor",
		    tidy_fn_name(location), dataset_file_path(dataset));
	}

	LutDescriptorValues descriptor{};
	if (!try_parse_lut_descriptor(
	        descriptor_elem, dataset_uses_little_endian(dataset), descriptor)) {
		diag::error_and_throw(
		    "{} file={} reason=invalid LUTDescriptor",
		    tidy_fn_name(location),
		    dataset_file_path(dataset));
	}

	const auto& lut_data_elem = (*item)["LUTData"_tag];
	if (!lut_data_elem) {
		diag::error_and_throw(
		    "{} file={} reason=VOILUTSequence item #0 missing LUTData",
		    tidy_fn_name(location), dataset_file_path(dataset));
	}

	pixel::VoiLut lut{};
	lut.first_mapped = descriptor.first_mapped;
	lut.bits_per_entry = static_cast<std::uint16_t>(descriptor.bits_per_entry);
	lut.values =
	    load_discrete_lut_data_or_throw(*item, descriptor, lut_data_elem, location);
	return lut;
}

[[nodiscard]] std::optional<pixel::WindowTransform> parse_window_transform_from_dataset_or_throw(
    const DataSet& dataset,
    std::source_location location = std::source_location::current()) {
	const auto& center_elem = dataset["WindowCenter"_tag];
	const auto& width_elem = dataset["WindowWidth"_tag];
	if (!center_elem && !width_elem) {
		return std::nullopt;
	}
	if (!center_elem || !width_elem) {
		diag::error_and_throw(
		    "{} file={} reason=WindowCenter/WindowWidth metadata is incomplete",
		    tidy_fn_name(location), dataset_file_path(dataset));
	}

	// DICOM allows multiple alternative windows; expose the first pair for now.
	const auto parsed_centers = center_elem.to_double_vector();
	if (!parsed_centers || parsed_centers->empty()) {
		diag::error_and_throw(
		    "{} file={} reason=invalid WindowCenter",
		    tidy_fn_name(location),
		    dataset_file_path(dataset));
	}
	const auto parsed_widths = width_elem.to_double_vector();
	if (!parsed_widths || parsed_widths->empty()) {
		diag::error_and_throw(
		    "{} file={} reason=invalid WindowWidth",
		    tidy_fn_name(location),
		    dataset_file_path(dataset));
	}

	const float center = static_cast<float>((*parsed_centers)[0]);
	const float width = static_cast<float>((*parsed_widths)[0]);
	if (!std::isfinite(center) || !std::isfinite(width) || width <= 0.0f) {
		diag::error_and_throw(
		    "{} file={} reason=WindowCenter/WindowWidth must be finite and width > 0",
		    tidy_fn_name(location), dataset_file_path(dataset));
	}

	pixel::VoiLutFunction function = pixel::VoiLutFunction::linear;
	const auto& function_elem = dataset["VOILUTFunction"_tag];
	if (function_elem) {
		const auto function_text = function_elem.to_string_view();
		if (!function_text || function_text->empty()) {
			diag::error_and_throw(
			    "{} file={} reason=invalid VOILUTFunction",
			    tidy_fn_name(location),
			    dataset_file_path(dataset));
		}
		const auto parsed_function = parse_voi_lut_function_from_text(*function_text);
		if (!parsed_function) {
			diag::error_and_throw(
			    "{} file={} reason=unsupported VOILUTFunction",
			    tidy_fn_name(location),
			    dataset_file_path(dataset));
		}
		function = *parsed_function;
	}

	return pixel::WindowTransform{
	    .center = center,
	    .width = width,
	    .function = function,
	};
}

[[nodiscard]] std::optional<pixel::RescaleTransform> parse_rescale_transform_from_dataset_or_throw(
    const DataSet& dataset,
    std::source_location location = std::source_location::current()) {
	const auto& slope_elem = dataset["RescaleSlope"_tag];
	const auto& intercept_elem = dataset["RescaleIntercept"_tag];
	if (!slope_elem && !intercept_elem) {
		return std::nullopt;
	}

	// Parse slope/intercept independently so missing tags still fall back to
	// the DICOM identity defaults.
	float slope = 1.0f;
	if (slope_elem) {
		const auto parsed_slope = slope_elem.to_double();
		if (!parsed_slope) {
			diag::error_and_throw(
			    "{} file={} reason=invalid RescaleSlope",
			    tidy_fn_name(location),
			    dataset_file_path(dataset));
		}
		slope = static_cast<float>(*parsed_slope);
	}

	float intercept = 0.0f;
	if (intercept_elem) {
		const auto parsed_intercept = intercept_elem.to_double();
		if (!parsed_intercept) {
			diag::error_and_throw(
			    "{} file={} reason=invalid RescaleIntercept",
			    tidy_fn_name(location),
			    dataset_file_path(dataset));
		}
		intercept = static_cast<float>(*parsed_intercept);
	}

	return pixel::RescaleTransform{
	    .slope = slope,
	    .intercept = intercept,
	};
}

}  // namespace

DicomFile::DicomFile() : root_dataset_(this) {
	set_transfer_syntax_state_only("ExplicitVRLittleEndian"_uid);
}

DicomFile::~DicomFile() = default;

DataSet& DicomFile::dataset() {
	return root_dataset_;
}

const DataSet& DicomFile::dataset() const {
	return root_dataset_;
}

const std::string& DicomFile::path() const {
	return root_dataset_.path();
}

InStream& DicomFile::stream() {
	return root_dataset_.stream();
}

const InStream& DicomFile::stream() const {
	return root_dataset_.stream();
}

std::size_t DicomFile::size() const {
	return root_dataset_.size();
}

DataElement& DicomFile::add_dataelement(Tag tag, VR vr) {
	return root_dataset_.add_dataelement(tag, vr);
}

DataElement& DicomFile::add_dataelement(std::string_view tag_path, VR vr) {
	return root_dataset_.add_dataelement(tag_path, vr);
}

DataElement& DicomFile::ensure_dataelement(Tag tag, VR vr) {
	return root_dataset_.ensure_dataelement(tag, vr);
}

DataElement& DicomFile::ensure_dataelement(std::string_view tag_path, VR vr) {
	return root_dataset_.ensure_dataelement(tag_path, vr);
}

void DicomFile::remove_dataelement(Tag tag) {
	root_dataset_.remove_dataelement(tag);
}

DataElement& DicomFile::get_dataelement(Tag tag) {
	return root_dataset_.get_dataelement(tag);
}

const DataElement& DicomFile::get_dataelement(Tag tag) const {
	return root_dataset_.get_dataelement(tag);
}

DataElement& DicomFile::get_dataelement(std::string_view tag_path) {
	return root_dataset_.get_dataelement(tag_path);
}

const DataElement& DicomFile::get_dataelement(std::string_view tag_path) const {
	return root_dataset_.get_dataelement(tag_path);
}

void DicomFile::ensure_loaded(Tag tag) {
	root_dataset_.ensure_loaded(tag);
}

void DicomFile::ensure_loaded(Tag tag) const {
	root_dataset_.ensure_loaded(tag);
}

DataElement& DicomFile::operator[](Tag tag) {
	return root_dataset_[tag];
}

DataElement& DicomFile::operator[](std::string_view tag_path) {
	return root_dataset_[tag_path];
}

const DataElement& DicomFile::operator[](Tag tag) const {
	return root_dataset_[tag];
}

const DataElement& DicomFile::operator[](std::string_view tag_path) const {
	return root_dataset_[tag_path];
}

DicomFile::iterator DicomFile::begin() {
	return root_dataset_.begin();
}

DicomFile::iterator DicomFile::end() {
	return root_dataset_.end();
}

DicomFile::const_iterator DicomFile::begin() const {
	return dataset().begin();
}

DicomFile::const_iterator DicomFile::end() const {
	return dataset().end();
}

DicomFile::const_iterator DicomFile::cbegin() const {
	return dataset().cbegin();
}

DicomFile::const_iterator DicomFile::cend() const {
	return dataset().cend();
}

void DicomFile::attach_to_file(const std::filesystem::path& path) {
	root_dataset_.attach_to_file(path);
}

void DicomFile::attach_to_memory(const std::uint8_t* data, std::size_t size, bool copy) {
	root_dataset_.attach_to_memory(data, size, copy);
}

void DicomFile::attach_to_memory(const std::string& name, const std::uint8_t* data,
    std::size_t size, bool copy) {
	root_dataset_.attach_to_memory(name, data, size, copy);
}

void DicomFile::attach_to_memory(std::string name, std::vector<std::uint8_t>&& buffer) {
	root_dataset_.attach_to_memory(std::move(name), std::move(buffer));
}

void DicomFile::clear_error_state() noexcept {
	has_error_ = false;
	error_message_.clear();
}

void DicomFile::set_error_state(std::string message) {
	has_error_ = true;
	error_message_ = std::move(message);
}

void DicomFile::read_attached_stream(const ReadOptions& options) {
	clear_error_state();

	std::shared_ptr<diag::Reporter> downstream = diag::thread_reporter_slot();
	if (!downstream) {
		downstream = diag::default_reporter();
	}
	auto capturing_reporter = std::make_shared<LastErrorCapturingReporter>(downstream);
	ThreadReporterGuard reporter_scope(capturing_reporter);

	try {
		root_dataset_.read_attached_stream(options);
	} catch (const std::exception& ex) {
		const std::string_view what = ex.what();
		if (!what.empty()) {
			set_error_state(std::string(what));
		} else if (capturing_reporter->has_error()) {
			set_error_state(capturing_reporter->last_error_message());
		} else {
			set_error_state("DicomFile::read_attached_stream reason=unknown read error");
		}

		if (!options.keep_on_error) {
			throw;
		}
	} catch (...) {
		if (capturing_reporter->has_error()) {
			set_error_state(capturing_reporter->last_error_message());
		} else {
			set_error_state("DicomFile::read_attached_stream reason=unknown non-std exception");
		}

		if (!options.keep_on_error) {
			throw;
		}
	}

	if (!has_error_ && capturing_reporter->has_error()) {
		set_error_state(capturing_reporter->last_error_message());
	}
}

bool DicomFile::has_pixel_data() const {
	// Pixel payload may live in one of the three standard pixel value elements.
	root_dataset_.ensure_loaded("PixelData"_tag);
	return root_dataset_["PixelData"_tag].is_present() ||
	    root_dataset_["FloatPixelData"_tag].is_present() ||
	    root_dataset_["DoubleFloatPixelData"_tag].is_present();
}

std::optional<pixel::PixelLayout> DicomFile::native_pixel_layout() const {
	// Reuse the internal source-layout helper so native PixelData and decode share
	// the same normalized packed layout interpretation.
	if (!has_pixel_data()) {
		return std::nullopt;
	}

	const auto source_layout = pixel::support_detail::compute_decode_source_layout(*this);
	if (source_layout.empty()) {
		return std::nullopt;
	}
	return source_layout;
}

std::optional<pixel::ModalityLut> DicomFile::modality_lut() const {
	return modality_lut(std::size_t{0});
}

std::optional<pixel::ModalityLut> DicomFile::modality_lut(
    std::size_t frame_index) const {
	validate_transform_metadata_frame_index_or_throw(root_dataset_, frame_index);

	// Enhanced multi-frame instances describe frame-specific modality-equivalent
	// transforms through Pixel Value Transformation Sequence. Root-level
	// ModalityLUTSequence remains a shared legacy fallback for every frame.
	const auto& modality_lut_seq_elem = root_dataset_["ModalityLUTSequence"_tag];
	if (!modality_lut_seq_elem) {
		return std::nullopt;
	}

	const auto* modality_lut_seq = modality_lut_seq_elem.as_sequence();
	if (!modality_lut_seq || modality_lut_seq->size() <= 0) {
		diag::error_and_throw(
		    "DicomFile::modality_lut file={} reason=ModalityLUTSequence is empty or invalid",
		    path());
	}

	const auto* item = modality_lut_seq->get_dataset(0);
	if (!item) {
		diag::error_and_throw(
		    "DicomFile::modality_lut file={} reason=ModalityLUTSequence item #0 is missing",
		    path());
	}

	const auto& descriptor_elem = (*item)["LUTDescriptor"_tag];
	if (!descriptor_elem) {
		diag::error_and_throw(
		    "DicomFile::modality_lut file={} reason=ModalityLUTSequence item #0 missing LUTDescriptor",
		    path());
	}

	LutDescriptorValues descriptor{};
	if (!try_parse_lut_descriptor(descriptor_elem, file_uses_little_endian(*this), descriptor)) {
		diag::error_and_throw(
		    "DicomFile::modality_lut file={} reason=invalid LUTDescriptor",
		    path());
	}

	const auto& lut_data_elem = (*item)["LUTData"_tag];
	if (!lut_data_elem) {
		diag::error_and_throw(
		    "DicomFile::modality_lut file={} reason=ModalityLUTSequence item #0 missing LUTData",
		    path());
	}

	const auto lut_data = lut_data_elem.value_span();
	if (lut_data.empty()) {
		diag::error_and_throw(
		    "DicomFile::modality_lut file={} reason=empty LUTData",
		    path());
	}

	pixel::ModalityLut lut{};
	lut.first_mapped = descriptor.first_mapped;
	const auto discrete_values = load_discrete_lut_data_or_throw(
	    *item, descriptor, lut_data_elem);
	lut.values.resize(discrete_values.size());
	for (std::size_t index = 0; index < discrete_values.size(); ++index) {
		lut.values[index] = static_cast<float>(discrete_values[index]);
	}
	return lut;
}

std::optional<pixel::PixelPresentation> DicomFile::pixel_presentation() const {
	return parse_pixel_presentation_from_dataset_or_throw(root_dataset_);
}

std::optional<pixel::PaletteLut> DicomFile::palette_lut() const {
	// This accessor intentionally models only the classic root-level PALETTE COLOR LUT.
	// Supplemental Palette and Enhanced Palette metadata describe different display
	// pipelines and are intentionally exposed through separate metadata accessors.
	if (dataset_has_enhanced_palette_metadata(root_dataset_)) {
		return std::nullopt;
	}

	const auto presentation = parse_pixel_presentation_from_dataset_or_throw(root_dataset_);
	const auto photometric = parse_photometric_from_dataset_or_throw(root_dataset_);
	if (presentation && *presentation == pixel::PixelPresentation::color &&
	    photometric && *photometric != pixel::Photometric::palette_color) {
		return std::nullopt;
	}

	const auto parsed = parse_palette_channels_from_dataset_or_throw(root_dataset_);
	if (!parsed) {
		return std::nullopt;
	}
	return parsed->palette;
}

std::optional<pixel::SupplementalPaletteInfo> DicomFile::supplemental_palette() const {
	if (dataset_has_enhanced_palette_metadata(root_dataset_)) {
		return std::nullopt;
	}

	const auto presentation = parse_pixel_presentation_from_dataset_or_throw(root_dataset_);
	if (!presentation || *presentation != pixel::PixelPresentation::color) {
		return std::nullopt;
	}

	const auto parsed = parse_palette_channels_from_dataset_or_throw(root_dataset_);
	if (!parsed) {
		return std::nullopt;
	}

	const auto photometric = parse_photometric_from_dataset_or_throw(root_dataset_);
	if (photometric && *photometric == pixel::Photometric::palette_color) {
		return std::nullopt;
	}

	pixel::SupplementalPaletteInfo info{};
	info.pixel_presentation = *presentation;
	info.palette = parsed->palette;
	if (const auto stored_value_range =
	        parse_stored_value_color_range_from_dataset_or_throw(root_dataset_)) {
		info.has_stored_value_range = true;
		info.minimum_stored_value_mapped =
		    stored_value_range->minimum_stored_value_mapped;
		info.maximum_stored_value_mapped =
		    stored_value_range->maximum_stored_value_mapped;
	}
	return info;
}

std::optional<pixel::EnhancedPaletteInfo> DicomFile::enhanced_palette() const {
	if (!dataset_has_enhanced_palette_metadata(root_dataset_)) {
		return std::nullopt;
	}

	pixel::EnhancedPaletteInfo info{};
	if (const auto presentation = parse_pixel_presentation_from_dataset_or_throw(
	        root_dataset_)) {
		info.pixel_presentation = *presentation;
	}
	info.data_frame_assignments =
	    parse_enhanced_data_frame_assignments_or_throw(root_dataset_);
	if (const auto blending_lut_1 = parse_enhanced_blending_lut_or_throw(
	        root_dataset_, "BlendingLUT1Sequence"_tag,
	        "BlendingLUT1TransferFunction"_tag)) {
		info.has_blending_lut_1 = true;
		info.blending_lut_1 = *blending_lut_1;
	}
	if (const auto blending_lut_2 = parse_enhanced_blending_lut_or_throw(
	        root_dataset_, "BlendingLUT2Sequence"_tag,
	        "BlendingLUT2TransferFunction"_tag)) {
		info.has_blending_lut_2 = true;
		info.blending_lut_2 = *blending_lut_2;
	}
	info.palette_items = parse_enhanced_palette_items_or_throw(root_dataset_);
	info.has_icc_profile = root_dataset_["ICCProfile"_tag].is_present();
	if (const auto& color_space_elem = root_dataset_["ColorSpace"_tag]; color_space_elem) {
		const auto color_space_text = color_space_elem.to_string_view();
		if (!color_space_text || color_space_text->empty()) {
			diag::error_and_throw(
			    "DicomFile::enhanced_palette file={} reason=invalid ColorSpace",
			    path());
		}
		info.color_space.assign(color_space_text->data(), color_space_text->size());
	}
	return info;
}

std::optional<pixel::VoiLut> DicomFile::voi_lut() const {
	return voi_lut(std::size_t{0});
}

std::optional<pixel::VoiLut> DicomFile::voi_lut(std::size_t frame_index) const {
	validate_transform_metadata_frame_index_or_throw(root_dataset_, frame_index);

	// Frame VOI LUT metadata overrides shared entries, which in turn override
	// root-level legacy VOILUTSequence fallback.
	if (const auto* frame_voi_item = resolve_frame_functional_group_macro_item_or_throw(
	        root_dataset_, "FrameVOILUTSequence"_tag, frame_index)) {
		if (const auto lut = parse_voi_lut_from_dataset_or_throw(*frame_voi_item)) {
			return lut;
		}
	}

	return parse_voi_lut_from_dataset_or_throw(root_dataset_);
}

std::optional<pixel::WindowTransform> DicomFile::window_transform() const {
	return window_transform(std::size_t{0});
}

std::optional<pixel::WindowTransform> DicomFile::window_transform(
    std::size_t frame_index) const {
	validate_transform_metadata_frame_index_or_throw(root_dataset_, frame_index);

	// Frame VOI LUT window metadata overrides shared entries, which in turn
	// override root-level WindowCenter/WindowWidth fallback.
	if (const auto* frame_voi_item = resolve_frame_functional_group_macro_item_or_throw(
	        root_dataset_, "FrameVOILUTSequence"_tag, frame_index)) {
		if (const auto window = parse_window_transform_from_dataset_or_throw(
		        *frame_voi_item)) {
			return window;
		}
	}

	return parse_window_transform_from_dataset_or_throw(root_dataset_);
}

std::optional<pixel::RescaleTransform> DicomFile::rescale_transform() const {
	return rescale_transform(std::size_t{0});
}

std::optional<pixel::RescaleTransform> DicomFile::rescale_transform(
    std::size_t frame_index) const {
	validate_transform_metadata_frame_index_or_throw(root_dataset_, frame_index);

	// Per-frame Pixel Value Transformation metadata overrides shared entries,
	// which in turn override root-level legacy RescaleSlope/RescaleIntercept.
	if (const auto* pixel_value_tx_item = resolve_frame_functional_group_macro_item_or_throw(
	        root_dataset_, "PixelValueTransformationSequence"_tag, frame_index)) {
		if (const auto rescale = parse_rescale_transform_from_dataset_or_throw(
		        *pixel_value_tx_item)) {
			return rescale;
		}
	}

	return parse_rescale_transform_from_dataset_or_throw(root_dataset_);
}

void DicomFile::set_native_pixel_data(std::vector<std::uint8_t>&& native_pixel_data, VR vr) {
	root_dataset_.remove_dataelement("FloatPixelData"_tag);
	root_dataset_.remove_dataelement("DoubleFloatPixelData"_tag);
	root_dataset_.remove_dataelement("ExtendedOffsetTable"_tag);
	root_dataset_.remove_dataelement("ExtendedOffsetTableLengths"_tag);

	VR native_vr = vr;
	if (native_vr == VR::None) {
		const auto bits_allocated =
		    static_cast<int>(root_dataset_["BitsAllocated"_tag].to_long().value_or(0));
		native_vr = native_pixel_vr_from_bits_allocated(bits_allocated);
	}
	if (native_vr != VR::OB && native_vr != VR::OW) {
		diag::error_and_throw(
		    "DicomFile::set_native_pixel_data file={} reason=PixelData VR must be OB or OW",
		    path());
	}

	auto& pixel_data = root_dataset_.add_dataelement("PixelData"_tag, native_vr);
	pixel_data.set_value_bytes(std::move(native_pixel_data));
}

void DicomFile::reset_encapsulated_pixel_data(std::size_t frame_count) {
	root_dataset_.remove_dataelement("FloatPixelData"_tag);
	root_dataset_.remove_dataelement("DoubleFloatPixelData"_tag);
	root_dataset_.remove_dataelement("ExtendedOffsetTable"_tag);
	root_dataset_.remove_dataelement("ExtendedOffsetTableLengths"_tag);

	auto& pixel_data = root_dataset_.add_dataelement("PixelData"_tag, VR::PX);
	auto* pixel_sequence = pixel_data.as_pixel_sequence();
	if (!pixel_sequence) {
		diag::error_and_throw(
		    "DicomFile::reset_encapsulated_pixel_data file={} reason=PixelData is not an encapsulated sequence",
		    path());
	}

	if (frame_count > 0) {
		for (std::size_t frame_index = 0; frame_index < frame_count; ++frame_index) {
			PixelFrame* frame = pixel_sequence->add_frame();
			if (!frame) {
				diag::error_and_throw(
				    "DicomFile::reset_encapsulated_pixel_data file={} reason=failed to preallocate encapsulated frame index={}",
				    path(), frame_index);
			}
		}
		if (frame_count > static_cast<std::size_t>(std::numeric_limits<long>::max())) {
			diag::error_and_throw(
			    "DicomFile::reset_encapsulated_pixel_data file={} reason=frame count exceeds long range",
			    path());
		}
		auto& frame_count_element =
		    root_dataset_.add_dataelement("NumberOfFrames"_tag, VR::IS);
		if (!frame_count_element.from_long(static_cast<long>(frame_count))) {
			diag::error_and_throw(
			    "DicomFile::reset_encapsulated_pixel_data file={} reason=failed to set NumberOfFrames",
			    path());
		}
	} else {
		root_dataset_.remove_dataelement("NumberOfFrames"_tag);
	}
}

void DicomFile::set_encoded_pixel_frame(std::size_t frame_index,
    std::vector<std::uint8_t>&& encoded_frame) {
	auto& pixel_data = root_dataset_.get_dataelement("PixelData"_tag);
	if (pixel_data.is_missing() || !pixel_data.vr().is_pixel_sequence()) {
		diag::error_and_throw(
		    "DicomFile::set_encoded_pixel_frame file={} frame_index={} reason=PixelData is not an encapsulated sequence; call reset_encapsulated_pixel_data(frame_count) first",
		    path(), frame_index);
	}
	auto* pixel_sequence = pixel_data.as_pixel_sequence();
	if (!pixel_sequence) {
		diag::error_and_throw(
		    "DicomFile::set_encoded_pixel_frame file={} frame_index={} reason=PixelData is not an encapsulated sequence",
		    path(), frame_index);
	}
	PixelFrame* frame = pixel_sequence->frame(frame_index);
	if (!frame) {
		diag::error_and_throw(
		    "DicomFile::set_encoded_pixel_frame file={} frame_index={} frame_count={} reason=frame index out of range; call reset_encapsulated_pixel_data(frame_count) with enough slots",
		    path(), frame_index, pixel_sequence->number_of_frames());
	}

	frame->set_fragments({});
	frame->set_encoded_data(std::move(encoded_frame));
}

PixelFrame* DicomFile::add_encoded_pixel_frame(std::vector<std::uint8_t>&& encoded_frame) {
	auto& pixel_data_before = root_dataset_.get_dataelement("PixelData"_tag);
	if (pixel_data_before.is_missing() || !pixel_data_before.vr().is_pixel_sequence()) {
		reset_encapsulated_pixel_data(0);
	}
	auto& pixel_data = root_dataset_.get_dataelement("PixelData"_tag);
	if (pixel_data.is_missing()) {
		diag::error_and_throw(
		    "DicomFile::add_encoded_pixel_frame file={} reason=PixelData is missing after reset",
		    path());
	}
	auto* pixel_sequence = pixel_data.as_pixel_sequence();
	if (!pixel_sequence) {
		diag::error_and_throw(
		    "DicomFile::add_encoded_pixel_frame file={} reason=PixelData is not an encapsulated sequence",
		    path());
	}

	PixelFrame* frame = pixel_sequence->add_frame();
	if (!frame) {
		diag::error_and_throw(
		    "DicomFile::add_encoded_pixel_frame file={} reason=failed to append pixel frame",
		    path());
	}
	frame->set_fragments({});
	frame->set_encoded_data(std::move(encoded_frame));

	const auto frame_count = pixel_sequence->number_of_frames();
	if (frame_count > static_cast<std::size_t>(std::numeric_limits<long>::max())) {
		diag::error_and_throw(
		    "DicomFile::add_encoded_pixel_frame file={} reason=frame count exceeds long range",
		    path());
	}
	auto& frame_count_element =
	    root_dataset_.add_dataelement("NumberOfFrames"_tag, VR::IS);
	if (!frame_count_element.from_long(static_cast<long>(frame_count))) {
		diag::error_and_throw(
		    "DicomFile::add_encoded_pixel_frame file={} reason=failed to update NumberOfFrames",
		    path());
	}
	return frame;
}

void DicomFile::set_transfer_syntax_state_only(uid::WellKnown transfer_syntax) {
	transfer_syntax_uid_ = transfer_syntax.valid() ? transfer_syntax : uid::WellKnown{};
	root_dataset_.explicit_vr_ = transfer_syntax_uid_.uses_explicit_vr();
}

void DicomFile::apply_transfer_syntax(uid::WellKnown transfer_syntax) {
	try {
		if (!apply_transfer_syntax_impl(*this, transfer_syntax,
		        ApplyTransferSyntaxEncodeMode::use_plugin_defaults,
		        std::span<const pixel::CodecOptionTextKv>{}, nullptr)) {
			return;
		}
		set_transfer_syntax_state_only(transfer_syntax);
		auto& transfer_syntax_element = add_dataelement("(0002,0010)"_tag, VR::UI);
		if (!transfer_syntax_element.from_transfer_syntax_uid(transfer_syntax)) {
			diag::throw_exception(
			    "DicomFile::apply_transfer_syntax reason=failed to update (0002,0010) TransferSyntaxUID");
		}
	} catch (const diag::DicomException& ex) {
		rethrow_transfer_syntax_exception_at_boundary_or_throw(ex);
	}
}

void DicomFile::apply_transfer_syntax(uid::WellKnown transfer_syntax,
    const pixel::EncoderContext& encoder_ctx) {
	try {
		if (!apply_transfer_syntax_impl(*this, transfer_syntax,
		        ApplyTransferSyntaxEncodeMode::use_encoder_context,
		        std::span<const pixel::CodecOptionTextKv>{}, &encoder_ctx)) {
			return;
		}
		set_transfer_syntax_state_only(transfer_syntax);
		auto& transfer_syntax_element = add_dataelement("(0002,0010)"_tag, VR::UI);
		if (!transfer_syntax_element.from_transfer_syntax_uid(transfer_syntax)) {
			diag::throw_exception(
			    "DicomFile::apply_transfer_syntax reason=failed to update (0002,0010) TransferSyntaxUID");
		}
	} catch (const diag::DicomException& ex) {
		rethrow_transfer_syntax_exception_at_boundary_or_throw(ex);
	}
}

void DicomFile::apply_transfer_syntax(uid::WellKnown transfer_syntax,
    std::span<const pixel::CodecOptionTextKv> codec_opt_override) {
	try {
		if (!apply_transfer_syntax_impl(*this, transfer_syntax,
		        ApplyTransferSyntaxEncodeMode::use_explicit_options,
		        codec_opt_override, nullptr)) {
			return;
		}
		set_transfer_syntax_state_only(transfer_syntax);
		auto& transfer_syntax_element = add_dataelement("(0002,0010)"_tag, VR::UI);
		if (!transfer_syntax_element.from_transfer_syntax_uid(transfer_syntax)) {
			diag::throw_exception(
			    "DicomFile::apply_transfer_syntax reason=failed to update (0002,0010) TransferSyntaxUID");
		}
	} catch (const diag::DicomException& ex) {
		rethrow_transfer_syntax_exception_at_boundary_or_throw(ex);
	}
}

void DicomFile::set_transfer_syntax(uid::WellKnown transfer_syntax) {
	try {
		if (!transfer_syntax.valid() ||
		    transfer_syntax.uid_type() != UidType::TransferSyntax) {
			diag::throw_exception(
			    "DicomFile::set_transfer_syntax reason=uid must be a valid Transfer Syntax UID");
		}
		if (!apply_transfer_syntax_impl(*this, transfer_syntax,
		        ApplyTransferSyntaxEncodeMode::use_plugin_defaults,
		        std::span<const pixel::CodecOptionTextKv>{}, nullptr)) {
			return;
		}
		set_transfer_syntax_state_only(transfer_syntax);
		auto& transfer_syntax_element = add_dataelement("(0002,0010)"_tag, VR::UI);
		if (!transfer_syntax_element.from_transfer_syntax_uid(transfer_syntax)) {
			diag::throw_exception(
			    "DicomFile::apply_transfer_syntax reason=failed to update (0002,0010) TransferSyntaxUID");
		}
	} catch (const diag::DicomException& ex) {
		rethrow_transfer_syntax_exception_at_boundary_or_throw(ex);
	}
}

void DicomFile::set_transfer_syntax(uid::WellKnown transfer_syntax,
    const pixel::EncoderContext& encoder_ctx) {
	try {
		if (!transfer_syntax.valid() ||
		    transfer_syntax.uid_type() != UidType::TransferSyntax) {
			diag::throw_exception(
			    "DicomFile::set_transfer_syntax reason=uid must be a valid Transfer Syntax UID");
		}
		if (!encoder_ctx.configured()) {
			diag::throw_exception(
			    "DicomFile::set_transfer_syntax file={} ts={} reason=encoder context is not configured",
			    path(), transfer_syntax.value());
		}
		if (!encoder_ctx.transfer_syntax_uid().valid() ||
		    encoder_ctx.transfer_syntax_uid() != transfer_syntax) {
			diag::throw_exception(
			    "DicomFile::set_transfer_syntax file={} ts={} ctx_ts={} reason=encoder context transfer syntax mismatch",
			    path(), transfer_syntax.value(), encoder_ctx.transfer_syntax_uid().value());
		}
		if (!apply_transfer_syntax_impl(*this, transfer_syntax,
		        ApplyTransferSyntaxEncodeMode::use_encoder_context,
		        std::span<const pixel::CodecOptionTextKv>{}, &encoder_ctx)) {
			return;
		}
		set_transfer_syntax_state_only(transfer_syntax);
		auto& transfer_syntax_element = add_dataelement("(0002,0010)"_tag, VR::UI);
		if (!transfer_syntax_element.from_transfer_syntax_uid(transfer_syntax)) {
			diag::throw_exception(
			    "DicomFile::apply_transfer_syntax reason=failed to update (0002,0010) TransferSyntaxUID");
		}
	} catch (const diag::DicomException& ex) {
		rethrow_transfer_syntax_exception_at_boundary_or_throw(ex);
	}
}

void DicomFile::set_transfer_syntax(uid::WellKnown transfer_syntax,
    std::span<const pixel::CodecOptionTextKv> codec_opt) {
	try {
		if (!transfer_syntax.valid() ||
		    transfer_syntax.uid_type() != UidType::TransferSyntax) {
			diag::throw_exception(
			    "DicomFile::set_transfer_syntax reason=uid must be a valid Transfer Syntax UID");
		}
		if (!apply_transfer_syntax_impl(*this, transfer_syntax,
		        ApplyTransferSyntaxEncodeMode::use_explicit_options,
		        codec_opt, nullptr)) {
			return;
		}
		set_transfer_syntax_state_only(transfer_syntax);
		auto& transfer_syntax_element = add_dataelement("(0002,0010)"_tag, VR::UI);
		if (!transfer_syntax_element.from_transfer_syntax_uid(transfer_syntax)) {
			diag::throw_exception(
			    "DicomFile::apply_transfer_syntax reason=failed to update (0002,0010) TransferSyntaxUID");
		}
	} catch (const diag::DicomException& ex) {
		rethrow_transfer_syntax_exception_at_boundary_or_throw(ex);
	}
}

void DicomFile::set_declared_specific_charset(SpecificCharacterSet charset) {
	const std::array<SpecificCharacterSet, 1> charsets{charset};
	set_declared_specific_charset(std::span<const SpecificCharacterSet>(charsets));
}

void DicomFile::set_declared_specific_charset(std::span<const SpecificCharacterSet> charsets) {
	root_dataset_.set_declared_specific_charset(charsets);
}

void DicomFile::set_specific_charset(
    SpecificCharacterSet charset, CharsetEncodeErrorPolicy errors, bool* out_replaced) {
	const std::array<SpecificCharacterSet, 1> charsets{charset};
	set_specific_charset(std::span<const SpecificCharacterSet>(charsets), errors, out_replaced);
}

void DicomFile::set_specific_charset(
    std::span<const SpecificCharacterSet> charsets, CharsetEncodeErrorPolicy errors,
    bool* out_replaced) {
	root_dataset_.set_specific_charset(charsets, errors, out_replaced);
}

std::unique_ptr<DicomFile> read_file(const std::filesystem::path& path, ReadOptions options) {
	auto dicom_file = std::make_unique<DicomFile>();
	dicom_file->attach_to_file(path);
	dicom_file->read_attached_stream(options);
	return dicom_file;
}

std::unique_ptr<DicomFile> read_bytes(const std::uint8_t* data, std::size_t size,
    ReadOptions options) {
	return read_bytes(std::string{"<memory>"}, data, size, options);
}

std::unique_ptr<DicomFile> read_bytes(const std::string& name, const std::uint8_t* data,
    std::size_t size, ReadOptions options) {
	auto dicom_file = std::make_unique<DicomFile>();
	dicom_file->attach_to_memory(name, data, size, options.copy);
	dicom_file->read_attached_stream(options);
	return dicom_file;
}

std::unique_ptr<DicomFile> read_bytes(std::string name, std::vector<std::uint8_t>&& buffer,
    ReadOptions options) {
	auto dicom_file = std::make_unique<DicomFile>();
	dicom_file->attach_to_memory(std::move(name), std::move(buffer));
	dicom_file->read_attached_stream(options);
	return dicom_file;
}

}  // namespace dicom
