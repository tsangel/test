#include "dicom.h"

#include "dicom_endian.h"
#include "diagnostics.h"

#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

namespace dicom {
using namespace dicom::literals;

namespace {

void apply_transfer_syntax_flags_for_file(uid::WellKnown transfer_syntax, bool& little_endian,
    bool& explicit_vr) {
	little_endian = true;
	explicit_vr = true;

	if (!transfer_syntax.valid()) {
		return;
	}
	if (transfer_syntax == "ExplicitVRBigEndian"_uid) {
		little_endian = false;
		return;
	}
	if (transfer_syntax == "ImplicitVRLittleEndian"_uid ||
	    transfer_syntax == "Papyrus3ImplicitVRLittleEndian"_uid) {
		explicit_vr = false;
	}
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
	    : previous_(diag::tls_reporter) {
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

bool is_valid_alignment(std::uint16_t alignment) noexcept {
	constexpr std::uint16_t kMaxAlignment = 4096;
	if (alignment == 0 || alignment == 1) {
		return true;
	}
	return alignment <= kMaxAlignment &&
	    (alignment & static_cast<std::uint16_t>(alignment - 1)) == 0;
}

pixel::DataType dtype_from_bits_and_representation(long bits_allocated,
    long pixel_representation) noexcept {
	switch (bits_allocated) {
	case 8:
		return (pixel_representation == 0) ? pixel::DataType::u8 : pixel::DataType::s8;
	case 16:
		return (pixel_representation == 0) ? pixel::DataType::u16 : pixel::DataType::s16;
	case 32:
		return (pixel_representation == 0) ? pixel::DataType::u32 : pixel::DataType::s32;
	default:
		return pixel::DataType::unknown;
	}
}

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

struct lut_descriptor_values {
	std::size_t entry_count{0};
	std::int64_t first_mapped{0};
	std::uint32_t bits_per_entry{0};
};

bool try_parse_lut_descriptor(const DataElement& descriptor_elem,
    lut_descriptor_values& out) noexcept {
	const auto span = descriptor_elem.value_span();
	if (span.size() < 6) {
		return false;
	}

	const bool little_endian =
	    descriptor_elem.parent() ? descriptor_elem.parent()->is_little_endian() : true;

	long raw_entries = 0;
	long raw_first_mapped = 0;
	long raw_bits = 0;
	const auto descriptor_vr = descriptor_elem.vr();
	if (descriptor_vr == VR::US) {
		raw_entries = static_cast<long>(endian::load_value<std::uint16_t>(span.data(), little_endian));
		raw_first_mapped = static_cast<long>(endian::load_value<std::uint16_t>(span.data() + 2, little_endian));
		raw_bits = static_cast<long>(endian::load_value<std::uint16_t>(span.data() + 4, little_endian));
	} else if (descriptor_vr == VR::SS) {
		raw_entries = static_cast<long>(endian::load_value<std::int16_t>(span.data(), little_endian));
		raw_first_mapped = static_cast<long>(endian::load_value<std::int16_t>(span.data() + 2, little_endian));
		raw_bits = static_cast<long>(endian::load_value<std::int16_t>(span.data() + 4, little_endian));
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

DicomFile::DicomFile() : root_dataset_(std::make_unique<DataSet>(this)) {
	set_transfer_syntax("ExplicitVRLittleEndian"_uid);
}

DicomFile::~DicomFile() = default;

DataSet& DicomFile::dataset() {
	return *root_dataset_;
}

const DataSet& DicomFile::dataset() const {
	return *root_dataset_;
}

const std::string& DicomFile::path() const {
	return root_dataset_->path();
}

InStream& DicomFile::stream() {
	return root_dataset_->stream();
}

const InStream& DicomFile::stream() const {
	return root_dataset_->stream();
}

std::size_t DicomFile::size() const {
	return root_dataset_->size();
}

DataElement* DicomFile::add_dataelement(Tag tag, VR vr) {
	return root_dataset_->add_dataelement(tag, vr);
}

DataElement* DicomFile::add_dataelement(Tag tag, VR vr, std::size_t offset, std::size_t length) {
	return root_dataset_->add_dataelement(tag, vr, offset, length);
}

void DicomFile::remove_dataelement(Tag tag) {
	root_dataset_->remove_dataelement(tag);
}

DataElement* DicomFile::get_dataelement(Tag tag) {
	return root_dataset_->get_dataelement(tag);
}

const DataElement* DicomFile::get_dataelement(Tag tag) const {
	return root_dataset_->get_dataelement(tag);
}

DataElement* DicomFile::get_dataelement(std::string_view tag_path) {
	return root_dataset_->get_dataelement(tag_path);
}

const DataElement* DicomFile::get_dataelement(std::string_view tag_path) const {
	return root_dataset_->get_dataelement(tag_path);
}

DataElement& DicomFile::operator[](Tag tag) {
	return (*root_dataset_)[tag];
}

const DataElement& DicomFile::operator[](Tag tag) const {
	return (*root_dataset_)[tag];
}

DicomFile::iterator DicomFile::begin() {
	return root_dataset_->begin();
}

DicomFile::iterator DicomFile::end() {
	return root_dataset_->end();
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
	root_dataset_->attach_to_file(path);
}

void DicomFile::attach_to_memory(const std::uint8_t* data, std::size_t size, bool copy) {
	root_dataset_->attach_to_memory(data, size, copy);
}

void DicomFile::attach_to_memory(const std::string& name, const std::uint8_t* data,
    std::size_t size, bool copy) {
	root_dataset_->attach_to_memory(name, data, size, copy);
}

void DicomFile::attach_to_memory(std::string name, std::vector<std::uint8_t>&& buffer) {
	root_dataset_->attach_to_memory(std::move(name), std::move(buffer));
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

	std::shared_ptr<diag::Reporter> downstream = diag::tls_reporter;
	if (!downstream) {
		downstream = diag::default_reporter();
	}
	auto capturing_reporter = std::make_shared<LastErrorCapturingReporter>(downstream);
	ThreadReporterGuard reporter_scope(capturing_reporter);

	try {
		root_dataset_->read_attached_stream(options);
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

const DicomFile::pixel_info_t& DicomFile::pixel_info(bool recalc) const {
	const auto build_pixel_info = [&]() {
		root_dataset_->ensure_loaded("PixelData"_tag);

		DicomFile::pixel_info_t info{};
		info.ts = transfer_syntax_uid();
		info.rows = static_cast<int>((*root_dataset_)["Rows"_tag].to_long().value_or(0));
		info.cols = static_cast<int>((*root_dataset_)["Columns"_tag].to_long().value_or(0));
		info.samples_per_pixel =
		    static_cast<int>((*root_dataset_)["SamplesPerPixel"_tag].to_long().value_or(1));
		info.bits_allocated =
		    static_cast<int>((*root_dataset_)["BitsAllocated"_tag].to_long().value_or(0));
		const auto pixel_representation =
		    (*root_dataset_)["PixelRepresentation"_tag].to_long().value_or(0);
		info.frames = static_cast<int>((*root_dataset_)["NumberOfFrames"_tag].to_long().value_or(1));
		info.planar_configuration =
		    ((*root_dataset_)["PlanarConfiguration"_tag].to_long().value_or(0) == 1)
		        ? pixel::Planar::planar
		        : pixel::Planar::interleaved;
		if (const auto& double_float_pixel = (*root_dataset_)["DoubleFloatPixelData"_tag];
		    double_float_pixel) {
			info.sv_dtype = pixel::DataType::f64;
		} else if (const auto& float_pixel = (*root_dataset_)["FloatPixelData"_tag]; float_pixel) {
			info.sv_dtype = pixel::DataType::f32;
		} else {
			info.sv_dtype = dtype_from_bits_and_representation(
			    static_cast<long>(info.bits_allocated), pixel_representation);
		}
		info.has_pixel_data = (info.sv_dtype != pixel::DataType::unknown);
		return info;
	};

	if (recalc || !pixel_info_cache_.has_value()) {
		pixel_info_cache_ = build_pixel_info();
	}
	return *pixel_info_cache_;
}

pixel::DecodeStrides DicomFile::calc_decode_strides(const pixel::DecodeOptions& opt) const {
	if (!is_valid_alignment(opt.alignment)) {
		diag::error_and_throw(
		    "DicomFile::calc_decode_strides file={} reason=alignment must be 0/1 or power-of-two <= 4096",
		    path());
	}

	const auto& info = pixel_info();
	const auto rows_value = info.rows;
	const auto cols_value = info.cols;
	const auto spp_value = info.samples_per_pixel;
	constexpr int kMaxRowsOrColumns = 65535;
	constexpr int kMaxSamplesPerPixel = 4;

	if (rows_value <= 0 || cols_value <= 0 || spp_value <= 0) {
		diag::error_and_throw(
		    "DicomFile::calc_decode_strides file={} reason=invalid Rows/Columns/SamplesPerPixel",
		    path());
	}
	if (rows_value > kMaxRowsOrColumns || cols_value > kMaxRowsOrColumns) {
		diag::error_and_throw(
		    "DicomFile::calc_decode_strides file={} reason=Rows/Columns must be <= 65535",
		    path());
	}
	if (spp_value > kMaxSamplesPerPixel) {
		diag::error_and_throw(
		    "DicomFile::calc_decode_strides file={} reason=SamplesPerPixel must be <= 4",
		    path());
	}

	const auto rows = static_cast<std::size_t>(rows_value);
	const auto cols = static_cast<std::size_t>(cols_value);
	const auto samples_per_pixel = static_cast<std::size_t>(spp_value);
	const auto scaled_output = pixel::should_use_scaled_output(*this, opt);

	const auto bytes_per_sample = scaled_output
	                                  ? sizeof(float)
	                                  : bytes_per_sample_of(info.sv_dtype);
	if (bytes_per_sample == 0) {
		diag::error_and_throw(
		    "DicomFile::calc_decode_strides file={} reason=unsupported or unknown sv_dtype",
		    path());
	}

	const auto planar_out = opt.planar_out;
	const auto row_components = (planar_out == pixel::Planar::planar)
	                                ? std::size_t{1}
	                                : samples_per_pixel;

	std::size_t row_stride = cols * row_components * bytes_per_sample;
	const std::size_t alignment = (opt.alignment <= 1)
	                                  ? std::size_t{1}
	                                  : static_cast<std::size_t>(opt.alignment);
	if (alignment > 1) {
		row_stride = ((row_stride + alignment - 1) / alignment) * alignment;
	}

	std::size_t frame_stride = row_stride * rows;
	if (planar_out == pixel::Planar::planar) {
		frame_stride *= samples_per_pixel;
	}

	return pixel::DecodeStrides{row_stride, frame_stride};
}

std::optional<pixel::ModalityLut> DicomFile::modality_lut() const {
	const auto& modality_lut_seq_elem = (*root_dataset_)["ModalityLUTSequence"_tag];
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

	lut_descriptor_values descriptor{};
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
	const bool source_little_endian = item->is_little_endian();
	const std::size_t entry_count = descriptor.entry_count;

	if (descriptor.bits_per_entry <= 8 && lut_data.size() >= entry_count * sizeof(std::uint16_t)) {
		// Some datasets store 8-bit LUT values in 16-bit containers.
		for (std::size_t i = 0; i < entry_count; ++i) {
			const auto v = endian::load_value<std::uint16_t>(
			    lut_data.data() + i * sizeof(std::uint16_t), source_little_endian);
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
		const auto v = endian::load_value<std::uint16_t>(
		    lut_data.data() + i * sizeof(std::uint16_t), source_little_endian);
		lut.values[i] = static_cast<float>(v & value_mask);
	}
	return lut;
}

void DicomFile::set_transfer_syntax(uid::WellKnown transfer_syntax) {
	invalidate_pixel_info_cache();
	transfer_syntax_uid_ = transfer_syntax.valid() ? transfer_syntax : uid::WellKnown{};
	if (root_dataset_) {
		apply_transfer_syntax_flags_for_file(transfer_syntax_uid_, root_dataset_->little_endian_,
		    root_dataset_->explicit_vr_);
	}
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
