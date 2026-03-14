#include "serialization/detail/write_core.hpp"
#include "serialization/transfer_syntax_write.hpp"

#include <fstream>
#include <vector>

namespace dicom {
using namespace dicom::literals;

namespace {

template <typename Writer>
void write_impl(DicomFile& file, Writer& writer, const WriteOptions& options) {
	if (!options.keep_existing_meta) {
		file.rebuild_file_meta();
	}

	const DataSet& dataset = file.dataset();
	dataset.ensure_loaded(Tag(0xFFFFu, 0xFFFFu));
	const auto target_transfer_syntax =
	    write_detail::determine_target_transfer_syntax(file, dataset, options);
	const auto encoding =
	    write_detail::determine_dataset_encoding(target_transfer_syntax, dataset);

	if (options.include_preamble) {
		write_detail::write_preamble(writer);
	}

	if (options.write_file_meta) {
		write_detail::write_file_meta_group(writer, dataset);
	}

	write_detail::write_dataset_body(writer, dataset, encoding, file.path());
}

}  // namespace

void DicomFile::rebuild_file_meta() {
	DataSet& dataset = this->dataset();
	dataset.ensure_loaded(Tag(0xFFFFu, 0xFFFFu));

	std::string sop_class_uid;
	if (auto value = dataset[write_detail::kSopClassUidTag].to_uid_string();
	    value && !value->empty()) {
		sop_class_uid = uid::normalize_uid_text(*value);
	} else if (auto value =
	               dataset[write_detail::kMediaStorageSopClassUidTag].to_uid_string();
	           value && !value->empty()) {
		sop_class_uid = uid::normalize_uid_text(*value);
	} else {
		sop_class_uid = std::string("SecondaryCaptureImageStorage"_uid.value());
	}
	if (!uid::is_valid_uid_text_strict(sop_class_uid)) {
		sop_class_uid = std::string("SecondaryCaptureImageStorage"_uid.value());
	}

	std::string sop_instance_uid;
	if (auto value = dataset[write_detail::kSopInstanceUidTag].to_uid_string();
	    value && !value->empty()) {
		sop_instance_uid = uid::normalize_uid_text(*value);
	} else if (auto value =
	               dataset[write_detail::kMediaStorageSopInstanceUidTag].to_uid_string();
	           value && !value->empty()) {
		sop_instance_uid = uid::normalize_uid_text(*value);
	} else {
		const auto generated = uid::generate_sop_instance_uid();
		const auto generated_value = generated.value();
		sop_instance_uid.assign(generated_value.data(), generated_value.size());
	}
	if (!uid::is_valid_uid_text_strict(sop_instance_uid)) {
		const auto generated = uid::generate_sop_instance_uid();
		const auto generated_value = generated.value();
		sop_instance_uid.assign(generated_value.data(), generated_value.size());
	}

	std::string transfer_syntax_uid =
	    write_detail::determine_transfer_syntax_uid_for_rebuild(*this, dataset);

	write_detail::clear_existing_meta_group(dataset);
	dataset.add_dataelement(write_detail::kFileMetaGroupLengthTag, VR::UL);
	const std::array<std::uint8_t, 2> meta_version{{0x00u, 0x01u}};
	dataset.add_dataelement(write_detail::kFileMetaInformationVersionTag, VR::OB)
	    .set_value_bytes(meta_version);
	write_detail::set_dataelement_uid(
	    dataset, write_detail::kMediaStorageSopClassUidTag, sop_class_uid);
	write_detail::set_dataelement_uid(
	    dataset, write_detail::kMediaStorageSopInstanceUidTag, sop_instance_uid);
	write_detail::set_dataelement_uid(
	    dataset, write_detail::kTransferSyntaxUidTag, transfer_syntax_uid);
	write_detail::set_dataelement_uid(
	    dataset, write_detail::kImplementationClassUidTag,
	    uid::implementation_class_uid());
	dataset.add_dataelement(write_detail::kImplementationVersionNameTag, VR::SH)
	    .from_string_view(uid::implementation_version_name());

	const auto meta_group_length = write_detail::measure_meta_group_length(dataset);
	dataset.get_dataelement(write_detail::kFileMetaGroupLengthTag)
	    .from_long(meta_group_length);
}

void DicomFile::write_to_stream(std::ostream& os, const WriteOptions& options) {
	write_detail::StreamWriter writer(os);
	write_impl(*this, writer, options);
}

std::vector<std::uint8_t> DicomFile::write_bytes(const WriteOptions& options) {
	std::vector<std::uint8_t> output;
	std::size_t reserve_hint = 4096;
	if (!this->path().empty()) {
		reserve_hint = std::max(reserve_hint, this->stream().attached_size());
	}
	if (options.include_preamble) {
		reserve_hint += 132;
	}
	output.reserve(reserve_hint);

	write_detail::VectorWriter writer(output);
	write_impl(*this, writer, options);
	return output;
}

void DicomFile::write_file(const std::string& path, const WriteOptions& options) {
	std::ofstream os(path, std::ios::binary | std::ios::trunc);
	if (!os) {
		diag::error_and_throw("write_file path={} reason=failed to open output file", path);
	}

	std::vector<char> file_buffer(1 << 20);
	os.rdbuf()->pubsetbuf(file_buffer.data(), static_cast<std::streamsize>(file_buffer.size()));

	this->write_to_stream(os, options);
	os.flush();
	if (!os) {
		diag::error_and_throw("write_file path={} reason=failed to flush output file", path);
	}
}

void DicomFile::write_with_transfer_syntax(std::ostream& os,
    uid::WellKnown transfer_syntax, const WriteOptions& options) {
	write_detail::StreamWriter writer(os);
	write_detail::write_with_transfer_syntax_to_stream_writer(*this, writer,
	    transfer_syntax, write_detail::StreamingWriteEncodeMode::use_plugin_defaults,
	    std::span<const pixel::CodecOptionTextKv>{}, nullptr, options);
}

void DicomFile::write_with_transfer_syntax(std::ostream& os,
    uid::WellKnown transfer_syntax, const pixel::EncoderContext& encoder_ctx,
    const WriteOptions& options) {
	write_detail::StreamWriter writer(os);
	write_detail::write_with_transfer_syntax_to_stream_writer(*this, writer,
	    transfer_syntax, write_detail::StreamingWriteEncodeMode::use_encoder_context,
	    std::span<const pixel::CodecOptionTextKv>{}, &encoder_ctx, options);
}

void DicomFile::write_with_transfer_syntax(std::ostream& os,
    uid::WellKnown transfer_syntax,
    std::span<const pixel::CodecOptionTextKv> codec_opt,
    const WriteOptions& options) {
	write_detail::StreamWriter writer(os);
	write_detail::write_with_transfer_syntax_to_stream_writer(*this, writer,
	    transfer_syntax, write_detail::StreamingWriteEncodeMode::use_explicit_options,
	    codec_opt, nullptr, options);
}

void DicomFile::write_with_transfer_syntax(const std::string& path,
    uid::WellKnown transfer_syntax, const WriteOptions& options) {
	std::ofstream os(path, std::ios::binary | std::ios::trunc);
	if (!os) {
		diag::error_and_throw(
		    "write_with_transfer_syntax path={} reason=failed to open output file", path);
	}

	std::vector<char> file_buffer(1 << 20);
	os.rdbuf()->pubsetbuf(file_buffer.data(), static_cast<std::streamsize>(file_buffer.size()));

	write_with_transfer_syntax(os, transfer_syntax, options);
	os.flush();
	if (!os) {
		diag::error_and_throw(
		    "write_with_transfer_syntax path={} reason=failed to flush output file", path);
	}
}

void DicomFile::write_with_transfer_syntax(const std::string& path,
    uid::WellKnown transfer_syntax, const pixel::EncoderContext& encoder_ctx,
    const WriteOptions& options) {
	std::ofstream os(path, std::ios::binary | std::ios::trunc);
	if (!os) {
		diag::error_and_throw(
		    "write_with_transfer_syntax path={} reason=failed to open output file", path);
	}

	std::vector<char> file_buffer(1 << 20);
	os.rdbuf()->pubsetbuf(file_buffer.data(), static_cast<std::streamsize>(file_buffer.size()));

	write_with_transfer_syntax(os, transfer_syntax, encoder_ctx, options);
	os.flush();
	if (!os) {
		diag::error_and_throw(
		    "write_with_transfer_syntax path={} reason=failed to flush output file", path);
	}
}

void DicomFile::write_with_transfer_syntax(const std::string& path,
    uid::WellKnown transfer_syntax,
    std::span<const pixel::CodecOptionTextKv> codec_opt,
    const WriteOptions& options) {
	std::ofstream os(path, std::ios::binary | std::ios::trunc);
	if (!os) {
		diag::error_and_throw(
		    "write_with_transfer_syntax path={} reason=failed to open output file", path);
	}

	std::vector<char> file_buffer(1 << 20);
	os.rdbuf()->pubsetbuf(file_buffer.data(), static_cast<std::streamsize>(file_buffer.size()));

	write_with_transfer_syntax(os, transfer_syntax, codec_opt, options);
	os.flush();
	if (!os) {
		diag::error_and_throw(
		    "write_with_transfer_syntax path={} reason=failed to flush output file", path);
	}
}

}  // namespace dicom
