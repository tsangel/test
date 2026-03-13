#include "dicom.h"

#include "dicom_endian.h"
#include "diagnostics.h"
#include "pixel/host/decode/decode_plan_compute.hpp"
#include "pixel/host/decode/decode_modality_value_transform.hpp"
#include "pixel/host/encode/encode_set_pixel_data_runner.hpp"

#include <exception>
#include <cstddef>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

#include "charset/charset_mutation.hpp"

namespace dicom {
using namespace dicom::literals;

namespace {

[[nodiscard]] bool transfer_syntax_uses_explicit_vr(uid::WellKnown transfer_syntax) noexcept {
	if (!transfer_syntax.valid()) {
		return true;
	}
	return transfer_syntax != "ImplicitVRLittleEndian"_uid &&
	    transfer_syntax != "Papyrus3ImplicitVRLittleEndian"_uid;
}

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

[[nodiscard]] VR native_pixel_vr_from_bits_allocated(int bits_allocated) noexcept {
	return bits_allocated > 8 ? VR::OW : VR::OB;
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

[[nodiscard]] std::optional<pixel::Photometric> parse_photometric_from_text(
    std::string_view text) noexcept {
	if (ascii_iequals(text, "MONOCHROME1")) {
		return pixel::Photometric::monochrome1;
	}
	if (ascii_iequals(text, "MONOCHROME2")) {
		return pixel::Photometric::monochrome2;
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

struct DecodedNativePixelBuffer {
	std::vector<std::uint8_t> bytes;
	pixel::DecodeStrides strides{};
	std::size_t frame_count{0};
};

[[nodiscard]] DecodedNativePixelBuffer decode_all_frames_to_native(DicomFile& file,
    const pixel::PixelDataInfo& info, std::string_view context) {
	if (!info.has_pixel_data) {
		diag::error_and_throw(
		    "{} file={} reason=PixelData exists but pixel metadata is not decodable",
		    context, file.path());
	}
	if (info.frames <= 0) {
		diag::error_and_throw(
		    "{} file={} reason=invalid NumberOfFrames for pixel conversion",
		    context, file.path());
	}

	pixel::DecodeOptions decode_options{};
	decode_options.planar_out = info.planar_configuration;
	decode_options.alignment = 1;
	decode_options.to_modality_value = false;
	// Keep codestream component domain during transfer-syntax transcoding.
	decode_options.decode_mct = false;
	const auto decode_plan = file.create_decode_plan(decode_options);
	if (decode_plan.strides.frame == 0) {
		diag::error_and_throw(
		    "{} file={} reason=calculated native frame size is zero",
		    context, file.path());
	}

	DecodedNativePixelBuffer decoded{};
	decoded.frame_count = static_cast<std::size_t>(info.frames);
	decoded.strides = decode_plan.strides;
	if (decoded.frame_count != 0 &&
	    decode_plan.strides.frame > std::numeric_limits<std::size_t>::max() / decoded.frame_count) {
		diag::error_and_throw(
		    "{} file={} reason=decoded native buffer size overflows size_t",
		    context, file.path());
	}
	decoded.bytes.resize(decode_plan.strides.frame * decoded.frame_count);

	for (std::size_t frame_index = 0; frame_index < decoded.frame_count; ++frame_index) {
		auto frame_span = std::span<std::uint8_t>(
		    decoded.bytes.data() + frame_index * decode_plan.strides.frame,
		    decode_plan.strides.frame);
		file.decode_into(frame_index, frame_span, decode_plan);
	}
	return decoded;
}

[[nodiscard]] pixel::PixelSource build_set_pixel_source_from_native_pixel_data(
    DicomFile& file, uid::WellKnown target_ts) {
	DataSet& dataset = file.dataset();
	const auto& pixel_data = dataset["PixelData"_tag];
	if (!pixel_data || pixel_data.vr().is_pixel_sequence() || !pixel_data.vr().is_binary()) {
		diag::error_and_throw(
		    "DicomFile::apply_transfer_syntax file={} target_ts={} reason=PixelData must be native binary for encoding path",
		    file.path(), target_ts.value());
	}

	const auto info = file.pixeldata_info();
	if (!info.has_pixel_data || info.rows <= 0 || info.cols <= 0 ||
	    info.frames <= 0 || info.samples_per_pixel <= 0) {
		diag::error_and_throw(
		    "DicomFile::apply_transfer_syntax file={} target_ts={} reason=invalid native pixel metadata rows={} cols={} frames={} samples_per_pixel={}",
		    file.path(), target_ts.value(), info.rows, info.cols,
		    info.frames, info.samples_per_pixel);
	}

	const auto bytes_per_sample = bytes_per_sample_of(info.sv_dtype);
	if (bytes_per_sample == 0) {
		diag::error_and_throw(
		    "DicomFile::apply_transfer_syntax file={} target_ts={} reason=failed to resolve bytes per sample for dtype={}",
		    file.path(), target_ts.value(), static_cast<int>(info.sv_dtype));
	}

	const auto rows = static_cast<std::size_t>(info.rows);
	const auto cols = static_cast<std::size_t>(info.cols);
	const auto frames = static_cast<std::size_t>(info.frames);
	const auto samples_per_pixel = static_cast<std::size_t>(info.samples_per_pixel);
	const bool source_is_planar =
	    info.planar_configuration == pixel::Planar::planar &&
	    samples_per_pixel > std::size_t{1};
	constexpr std::size_t kMaxRowsOrColumns = 65535;
	constexpr std::size_t kMaxSamplesPerPixel = 4;
	if (rows > kMaxRowsOrColumns || cols > kMaxRowsOrColumns) {
		diag::error_and_throw(
		    "DicomFile::apply_transfer_syntax file={} target_ts={} reason=Rows/Columns must be <= 65535 for native source path",
		    file.path(), target_ts.value());
	}
	if (samples_per_pixel > kMaxSamplesPerPixel) {
		diag::error_and_throw(
		    "DicomFile::apply_transfer_syntax file={} target_ts={} reason=SamplesPerPixel must be <= 4 for native source path",
		    file.path(), target_ts.value());
	}

	const std::uint64_t row_components_u64 =
	    source_is_planar ? static_cast<std::uint64_t>(cols)
	                     : static_cast<std::uint64_t>(cols) *
	                           static_cast<std::uint64_t>(samples_per_pixel);
	const std::uint64_t row_stride_u64 =
	    row_components_u64 * static_cast<std::uint64_t>(bytes_per_sample);
	const std::uint64_t plane_stride_u64 =
	    row_stride_u64 * static_cast<std::uint64_t>(rows);
	const std::uint64_t frame_stride_u64 = source_is_planar
	                                           ? plane_stride_u64 *
	                                                 static_cast<std::uint64_t>(samples_per_pixel)
	                                           : plane_stride_u64;

	const auto to_size_checked = [&](std::uint64_t value, std::string_view label) -> std::size_t {
		if (value > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
			diag::error_and_throw(
			    "DicomFile::apply_transfer_syntax file={} target_ts={} reason={} overflows size_t",
			    file.path(), target_ts.value(), label);
		}
		return static_cast<std::size_t>(value);
	};
	const auto row_stride = to_size_checked(row_stride_u64, "native source row stride");
	const auto plane_stride = to_size_checked(plane_stride_u64, "native source plane stride");
	const auto frame_stride = to_size_checked(frame_stride_u64, "native source frame stride");
	if (frame_stride == 0) {
		diag::error_and_throw(
		    "DicomFile::apply_transfer_syntax file={} target_ts={} reason=native source frame stride is zero",
		    file.path(), target_ts.value());
	}
	if (frames > std::numeric_limits<std::size_t>::max() / frame_stride) {
		diag::error_and_throw(
		    "DicomFile::apply_transfer_syntax file={} target_ts={} reason=native source total bytes overflow (frames={}, frame_stride={})",
		    file.path(), target_ts.value(), frames, frame_stride);
	}
	const auto required_bytes = frame_stride * frames;

	const auto source_bytes = pixel_data.value_span();
	if (source_bytes.size() < required_bytes) {
		diag::error_and_throw(
		    "DicomFile::apply_transfer_syntax file={} target_ts={} reason=PixelData bytes({}) are shorter than required native frame payload({})",
		    file.path(), target_ts.value(), source_bytes.size(), required_bytes);
	}

	const auto photometric_text = dataset["PhotometricInterpretation"_tag].to_string_view();
	if (!photometric_text) {
		diag::error_and_throw(
		    "DicomFile::apply_transfer_syntax file={} target_ts={} reason=missing or invalid PhotometricInterpretation",
		    file.path(), target_ts.value());
	}
	const auto photometric = parse_photometric_from_text(*photometric_text);
	if (!photometric) {
		diag::error_and_throw(
		    "DicomFile::apply_transfer_syntax file={} target_ts={} reason=unsupported PhotometricInterpretation='{}' for set_pixel_data path",
		    file.path(), target_ts.value(), *photometric_text);
	}

	const auto parse_optional_int_tag = [&](Tag tag, std::string_view name) -> std::optional<int> {
		const auto value = dataset[tag].to_long();
		if (!value) {
			return std::nullopt;
		}
		if (*value < static_cast<long>(std::numeric_limits<int>::min()) ||
		    *value > static_cast<long>(std::numeric_limits<int>::max())) {
			diag::error_and_throw(
			    "DicomFile::apply_transfer_syntax file={} target_ts={} reason={} value({}) is outside int range",
			    file.path(), target_ts.value(), name, *value);
		}
		return static_cast<int>(*value);
	};

	pixel::PixelSource source{};
	source.bytes = source_bytes.subspan(0, required_bytes);
	source.data_type = info.sv_dtype;
	source.rows = info.rows;
	source.cols = info.cols;
	source.frames = info.frames;
	source.samples_per_pixel = info.samples_per_pixel;
	source.planar = info.planar_configuration;
	source.row_stride = row_stride;
	source.frame_stride = frame_stride;
	source.photometric = *photometric;
	source.bits_stored = parse_optional_int_tag("BitsStored"_tag, "BitsStored").value_or(info.bits_stored);
	return source;
}

[[nodiscard]] pixel::PixelSource build_set_pixel_source_from_decoded_native_frames(
    DicomFile& file, uid::WellKnown target_ts, const pixel::PixelDataInfo& info,
    const pixel::DecodePlan& decode_plan) {
	if (!info.has_pixel_data || info.rows <= 0 || info.cols <= 0 ||
	    info.frames <= 0 || info.samples_per_pixel <= 0) {
		diag::error_and_throw(
		    "DicomFile::apply_transfer_syntax file={} target_ts={} reason=invalid decoded pixel metadata rows={} cols={} frames={} samples_per_pixel={}",
		    file.path(), target_ts.value(), info.rows, info.cols,
		    info.frames, info.samples_per_pixel);
	}
	if (decode_plan.strides.row == 0 || decode_plan.strides.frame == 0) {
		diag::error_and_throw(
		    "DicomFile::apply_transfer_syntax file={} target_ts={} reason=decoded frame layout is empty",
		    file.path(), target_ts.value());
	}

	const auto photometric_text = file.dataset()["PhotometricInterpretation"_tag].to_string_view();
	if (!photometric_text) {
		diag::error_and_throw(
		    "DicomFile::apply_transfer_syntax file={} target_ts={} reason=missing or invalid PhotometricInterpretation",
		    file.path(), target_ts.value());
	}
	const auto photometric = parse_photometric_from_text(*photometric_text);
	if (!photometric) {
		diag::error_and_throw(
		    "DicomFile::apply_transfer_syntax file={} target_ts={} reason=unsupported PhotometricInterpretation='{}' for streaming transcode path",
		    file.path(), target_ts.value(), *photometric_text);
	}

	const auto parse_optional_int_tag = [&](Tag tag, std::string_view name) -> std::optional<int> {
		const auto value = file.dataset()[tag].to_long();
		if (!value) {
			return std::nullopt;
		}
		if (*value < static_cast<long>(std::numeric_limits<int>::min()) ||
		    *value > static_cast<long>(std::numeric_limits<int>::max())) {
			diag::error_and_throw(
			    "DicomFile::apply_transfer_syntax file={} target_ts={} reason={} value({}) is outside int range",
			    file.path(), target_ts.value(), name, *value);
		}
		return static_cast<int>(*value);
	};

	pixel::PixelSource source{};
	source.data_type = info.sv_dtype;
	source.rows = info.rows;
	source.cols = info.cols;
	source.frames = info.frames;
	source.samples_per_pixel = info.samples_per_pixel;
	source.planar = info.planar_configuration;
	source.row_stride = decode_plan.strides.row;
	source.frame_stride = decode_plan.strides.frame;
	source.photometric = *photometric;
	source.bits_stored =
	    parse_optional_int_tag("BitsStored"_tag, "BitsStored").value_or(info.bits_stored);
	return source;
}

void convert_encapsulated_pixel_data_to_native(DicomFile& file) {
	DataSet& dataset = file.dataset();
	const auto& pixel_data = dataset["PixelData"_tag];
	if (!pixel_data || !pixel_data.vr().is_pixel_sequence()) {
		return;
	}

	const auto info = file.pixeldata_info();
	auto decoded =
	    decode_all_frames_to_native(file, info, "DicomFile::apply_transfer_syntax");
	const auto storage_bits =
	    static_cast<int>(bytes_per_sample_of(info.sv_dtype) * std::size_t{8});
	if (storage_bits <= 0) {
		diag::error_and_throw(
		    "DicomFile::apply_transfer_syntax file={} reason=failed to resolve storage bits from sv_dtype={}",
		    file.path(), static_cast<int>(info.sv_dtype));
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

void transcode_encapsulated_pixel_data_to_encapsulated(DicomFile& file,
    uid::WellKnown target_transfer_syntax,
    ApplyTransferSyntaxEncodeMode encode_mode,
    std::span<const pixel::CodecOptionTextKv> codec_opt_override,
    const pixel::EncoderContext* encoder_ctx) {
	const auto info = file.pixeldata_info();
	pixel::DecodeOptions decode_options{};
	decode_options.planar_out = info.planar_configuration;
	decode_options.alignment = 1;
	decode_options.to_modality_value = false;
	decode_options.decode_mct = false;
	const auto decode_plan = file.create_decode_plan(decode_options);
	if (decode_plan.strides.frame == 0) {
		diag::error_and_throw(
		    "DicomFile::apply_transfer_syntax file={} target_ts={} reason=calculated native frame size is zero for streaming transcode",
		    file.path(), target_transfer_syntax.value());
	}

	auto source_descriptor = build_set_pixel_source_from_decoded_native_frames(
	    file, target_transfer_syntax, info, decode_plan);
	std::vector<std::uint8_t> decoded_frame(decode_plan.strides.frame);

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
		diag::error_and_throw(
		    "DicomFile::apply_transfer_syntax file={} target_ts={} reason=encoder context is missing for streaming transcode",
		    file.path(), target_transfer_syntax.value());
	}

	pixel::detail::run_set_pixel_data_from_frame_provider_with_computed_codec_options(
	    file, target_transfer_syntax, source_descriptor,
	    active_encoder_ctx->codec_options(),
	    [&](std::size_t frame_index) -> std::span<const std::uint8_t> {
		    auto frame_span = std::span<std::uint8_t>(
		        decoded_frame.data(), decoded_frame.size());
		    file.decode_into(frame_index, frame_span, decode_plan);
		    return std::span<const std::uint8_t>(frame_span.data(), frame_span.size());
	    });
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
				diag::error_and_throw(
				    "DicomFile::set_transfer_syntax file={} source_ts={} target_ts={} reason=target encapsulated transfer syntax does not support native pixel encoding",
				    file.path(), source_transfer_syntax.value(), transfer_syntax.value());
			}
				auto source =
				    build_set_pixel_source_from_native_pixel_data(file, transfer_syntax);
				if (encode_mode == ApplyTransferSyntaxEncodeMode::use_plugin_defaults) {
					file.set_pixel_data(transfer_syntax, source);
				} else if (encode_mode == ApplyTransferSyntaxEncodeMode::use_encoder_context) {
					file.set_pixel_data(transfer_syntax, source, *encoder_ctx);
				} else {
					file.set_pixel_data(transfer_syntax, source, codec_opt_override);
				}
			return false;
		} else if (!transfer_syntax.supports_pixel_encode()) {
			diag::error_and_throw(
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

	return true;
}

struct LutDescriptorValues {
	std::size_t entry_count{0};
	std::int64_t first_mapped{0};
	std::uint32_t bits_per_entry{0};
};

bool try_parse_lut_descriptor(const DataElement& descriptor_elem,
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
		raw_entries = static_cast<long>(endian::load_le<std::uint16_t>(span.data()));
		raw_first_mapped = static_cast<long>(endian::load_le<std::uint16_t>(span.data() + 2));
		raw_bits = static_cast<long>(endian::load_le<std::uint16_t>(span.data() + 4));
	} else if (descriptor_vr == VR::SS) {
		raw_entries = static_cast<long>(endian::load_le<std::int16_t>(span.data()));
		raw_first_mapped = static_cast<long>(endian::load_le<std::int16_t>(span.data() + 2));
		raw_bits = static_cast<long>(endian::load_le<std::int16_t>(span.data() + 4));
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

DataElement& DicomFile::add_dataelement(
    Tag tag, VR vr, std::size_t offset, std::size_t length) {
	return root_dataset_.add_dataelement(tag, vr, offset, length);
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

DataElement& DicomFile::operator[](Tag tag) {
	return root_dataset_[tag];
}

const DataElement& DicomFile::operator[](Tag tag) const {
	return root_dataset_[tag];
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

void DicomFile::attach_to_file(const std::string& path) {
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

pixel::PixelDataInfo DicomFile::pixeldata_info() const {
	root_dataset_.ensure_loaded("PixelData"_tag);

	pixel::PixelDataInfo info{};
	info.ts = transfer_syntax_uid();
	info.rows = static_cast<int>(root_dataset_["Rows"_tag].to_long().value_or(0));
	info.cols = static_cast<int>(root_dataset_["Columns"_tag].to_long().value_or(0));
	info.samples_per_pixel =
	    static_cast<int>(root_dataset_["SamplesPerPixel"_tag].to_long().value_or(1));
	const auto bits_allocated =
	    static_cast<int>(root_dataset_["BitsAllocated"_tag].to_long().value_or(0));
	const auto bits_stored =
	    static_cast<int>(root_dataset_["BitsStored"_tag].to_long().value_or(0));
	const auto pixel_representation =
	    root_dataset_["PixelRepresentation"_tag].to_long().value_or(0);
	info.frames = static_cast<int>(root_dataset_["NumberOfFrames"_tag].to_long().value_or(1));
	info.planar_configuration =
	    (root_dataset_["PlanarConfiguration"_tag].to_long().value_or(0) == 1)
	        ? pixel::Planar::planar
	        : pixel::Planar::interleaved;
	if (const auto photometric_text =
	        root_dataset_["PhotometricInterpretation"_tag].to_string_view();
	    photometric_text.has_value()) {
		info.photometric = parse_photometric_from_text(*photometric_text);
	}
	if (const auto& double_float_pixel = root_dataset_["DoubleFloatPixelData"_tag];
	    double_float_pixel) {
		info.sv_dtype = pixel::DataType::f64;
		info.bits_stored = 64;
	} else if (const auto& float_pixel = root_dataset_["FloatPixelData"_tag]; float_pixel) {
		info.sv_dtype = pixel::DataType::f32;
		info.bits_stored = 32;
	} else {
		switch (bits_allocated) {
		case 8:
			info.sv_dtype =
			    (pixel_representation == 0) ? pixel::DataType::u8 : pixel::DataType::s8;
			break;
		case 16:
			info.sv_dtype =
			    (pixel_representation == 0) ? pixel::DataType::u16 : pixel::DataType::s16;
			break;
		case 32:
			info.sv_dtype =
			    (pixel_representation == 0) ? pixel::DataType::u32 : pixel::DataType::s32;
			break;
		default:
			info.sv_dtype = pixel::DataType::unknown;
			break;
		}
		const auto storage_bits =
		    static_cast<int>(bytes_per_sample_of(info.sv_dtype) * std::size_t{8});
		info.bits_stored = bits_stored > 0
		                       ? bits_stored
		                       : (bits_allocated > 0 ? bits_allocated : storage_bits);
	}
	info.has_pixel_data = (info.sv_dtype != pixel::DataType::unknown);
	return info;
}

std::optional<pixel::ModalityLut> DicomFile::modality_lut() const {
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
	if (!try_parse_lut_descriptor(descriptor_elem, descriptor)) {
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
	lut.values.resize(descriptor.entry_count);

	const std::uint32_t value_mask =
	    (descriptor.bits_per_entry == 16)
	        ? 0xFFFFu
	        : ((1u << descriptor.bits_per_entry) - 1u);
	const std::size_t entry_count = descriptor.entry_count;

	if (descriptor.bits_per_entry <= 8 && lut_data.size() >= entry_count * sizeof(std::uint16_t)) {
		// Some datasets store 8-bit LUT values in 16-bit containers.
			for (std::size_t i = 0; i < entry_count; ++i) {
				const auto v = endian::load_le<std::uint16_t>(
				    lut_data.data() + i * sizeof(std::uint16_t));
				lut.values[i] = static_cast<float>(v & value_mask);
			}
		return lut;
	}

	if (descriptor.bits_per_entry <= 8 && lut_data.size() >= entry_count) {
		for (std::size_t i = 0; i < entry_count; ++i) {
			const auto v = static_cast<std::uint32_t>(lut_data[i]);
			lut.values[i] = static_cast<float>(v & value_mask);
		}
		return lut;
	}

	const auto required_u16_bytes = entry_count * sizeof(std::uint16_t);
	if (lut_data.size() < required_u16_bytes) {
		diag::error_and_throw(
		    "DicomFile::modality_lut file={} reason=LUTData is shorter than LUTDescriptor entry count",
		    path());
	}
	for (std::size_t i = 0; i < entry_count; ++i) {
		const auto v = endian::load_le<std::uint16_t>(
		    lut_data.data() + i * sizeof(std::uint16_t));
		lut.values[i] = static_cast<float>(v & value_mask);
	}
	return lut;
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
	root_dataset_.explicit_vr_ = transfer_syntax_uses_explicit_vr(transfer_syntax_uid_);
}

void DicomFile::apply_transfer_syntax(uid::WellKnown transfer_syntax) {
	if (!apply_transfer_syntax_impl(*this, transfer_syntax,
	        ApplyTransferSyntaxEncodeMode::use_plugin_defaults,
	        std::span<const pixel::CodecOptionTextKv>{}, nullptr)) {
		return;
	}
	set_transfer_syntax_state_only(transfer_syntax);
	auto& transfer_syntax_element = add_dataelement("(0002,0010)"_tag, VR::UI);
	if (!transfer_syntax_element.from_transfer_syntax_uid(transfer_syntax)) {
		diag::error_and_throw(
		    "DicomFile::apply_transfer_syntax reason=failed to update (0002,0010) TransferSyntaxUID");
	}
}

void DicomFile::apply_transfer_syntax(uid::WellKnown transfer_syntax,
    const pixel::EncoderContext& encoder_ctx) {
	if (!apply_transfer_syntax_impl(*this, transfer_syntax,
	        ApplyTransferSyntaxEncodeMode::use_encoder_context,
	        std::span<const pixel::CodecOptionTextKv>{}, &encoder_ctx)) {
		return;
	}
	set_transfer_syntax_state_only(transfer_syntax);
	auto& transfer_syntax_element = add_dataelement("(0002,0010)"_tag, VR::UI);
	if (!transfer_syntax_element.from_transfer_syntax_uid(transfer_syntax)) {
		diag::error_and_throw(
		    "DicomFile::apply_transfer_syntax reason=failed to update (0002,0010) TransferSyntaxUID");
	}
}

void DicomFile::apply_transfer_syntax(uid::WellKnown transfer_syntax,
    std::span<const pixel::CodecOptionTextKv> codec_opt_override) {
	if (!apply_transfer_syntax_impl(*this, transfer_syntax,
	        ApplyTransferSyntaxEncodeMode::use_explicit_options,
	        codec_opt_override, nullptr)) {
		return;
	}
	set_transfer_syntax_state_only(transfer_syntax);
	auto& transfer_syntax_element = add_dataelement("(0002,0010)"_tag, VR::UI);
	if (!transfer_syntax_element.from_transfer_syntax_uid(transfer_syntax)) {
		diag::error_and_throw(
		    "DicomFile::apply_transfer_syntax reason=failed to update (0002,0010) TransferSyntaxUID");
	}
}

void DicomFile::set_transfer_syntax(uid::WellKnown transfer_syntax) {
	if (!transfer_syntax.valid() || transfer_syntax.uid_type() != UidType::TransferSyntax) {
		diag::error_and_throw(
		    "DicomFile::set_transfer_syntax reason=uid must be a valid Transfer Syntax UID");
	}
	apply_transfer_syntax(transfer_syntax);
}

void DicomFile::set_transfer_syntax(uid::WellKnown transfer_syntax,
    const pixel::EncoderContext& encoder_ctx) {
	if (!transfer_syntax.valid() || transfer_syntax.uid_type() != UidType::TransferSyntax) {
		diag::error_and_throw(
		    "DicomFile::set_transfer_syntax reason=uid must be a valid Transfer Syntax UID");
	}
	if (!encoder_ctx.configured()) {
		diag::error_and_throw(
		    "DicomFile::set_transfer_syntax file={} ts={} reason=encoder context is not configured",
		    path(), transfer_syntax.value());
	}
	if (!encoder_ctx.transfer_syntax_uid().valid() ||
	    encoder_ctx.transfer_syntax_uid() != transfer_syntax) {
		diag::error_and_throw(
		    "DicomFile::set_transfer_syntax file={} ts={} ctx_ts={} reason=encoder context transfer syntax mismatch",
		    path(), transfer_syntax.value(), encoder_ctx.transfer_syntax_uid().value());
	}
	apply_transfer_syntax(transfer_syntax, encoder_ctx);
}

void DicomFile::set_transfer_syntax(uid::WellKnown transfer_syntax,
    std::span<const pixel::CodecOptionTextKv> codec_opt) {
	if (!transfer_syntax.valid() || transfer_syntax.uid_type() != UidType::TransferSyntax) {
		diag::error_and_throw(
		    "DicomFile::set_transfer_syntax reason=uid must be a valid Transfer Syntax UID");
	}
	apply_transfer_syntax(transfer_syntax, codec_opt);
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

std::unique_ptr<DicomFile> read_file(const std::string& path, ReadOptions options) {
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
