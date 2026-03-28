#include "writing/detail/write_metadata.hpp"
#include "writing/transcoded_write.hpp"
#include "../stream_path_detail.hpp"

#include <filesystem>
#include <fstream>
#include <utility>
#include <vector>

namespace dicom {
using namespace dicom::literals;

namespace {
namespace fs = std::filesystem;

fs::path normalize_output_path(const fs::path& raw_path) {
	if (raw_path.empty()) {
		return {};
	}
	const fs::path normalized_path = raw_path.lexically_normal();
	return normalized_path.empty() ? raw_path : normalized_path;
}

[[nodiscard]] bool rebuild_file_meta_exception_has_boundary_prefix(
    std::string_view message) noexcept {
	return message.starts_with("rebuild_file_meta");
}

[[noreturn]] void rethrow_rebuild_file_meta_exception_at_public_boundary_or_throw(
    const diag::DicomException& ex) {
	const std::string_view message = ex.what();
	if (rebuild_file_meta_exception_has_boundary_prefix(message)) {
		diag::error_and_throw("{}", message);
	}
	throw;
}

[[noreturn]] void rethrow_rebuild_file_meta_exception_at_write_boundary_or_throw(
    const DicomFile& file, const diag::DicomException& ex) {
	const std::string_view message = ex.what();
	if (rebuild_file_meta_exception_has_boundary_prefix(message)) {
		diag::error_and_throw("write_to_stream file={} {}", file.path(), message);
	}
	throw;
}

void rebuild_file_meta_or_throw(DicomFile& file) {
	DataSet& dataset = file.dataset();
	dataset.ensure_loaded(Tag(0xFFFFu, 0xFFFFu));

	std::string sop_class_uid;
	if (auto value = dataset.get_value<std::string>("SOPClassUID"_tag);
	    value && !value->empty()) {
		sop_class_uid = uid::normalize_uid_text(*value);
	} else if (auto value =
	               dataset.get_value<std::string>("MediaStorageSOPClassUID"_tag);
	           value && !value->empty()) {
		sop_class_uid = uid::normalize_uid_text(*value);
	} else {
		sop_class_uid = std::string("SecondaryCaptureImageStorage"_uid.value());
	}
	if (!uid::is_valid_uid_text_strict(sop_class_uid)) {
		sop_class_uid = std::string("SecondaryCaptureImageStorage"_uid.value());
	}

	std::string sop_instance_uid;
	if (auto value = dataset.get_value<std::string>("SOPInstanceUID"_tag);
	    value && !value->empty()) {
		sop_instance_uid = uid::normalize_uid_text(*value);
	} else if (auto value =
	               dataset.get_value<std::string>("MediaStorageSOPInstanceUID"_tag);
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
	    write_detail::determine_transfer_syntax_uid_for_rebuild(file, dataset);

	write_detail::clear_existing_meta_group(dataset);
	const std::array<std::uint8_t, 2> meta_version{{0x00u, 0x01u}};
	dataset.add_dataelement("FileMetaInformationVersion"_tag, VR::OB)
	    .set_value_bytes(meta_version);
	if (!dataset.set_value("MediaStorageSOPClassUID"_tag, VR::UI, sop_class_uid)) {
		diag::throw_exception(
		    "rebuild_file_meta reason=invalid UID tag={} value={}",
		    "MediaStorageSOPClassUID"_tag.to_string(), sop_class_uid);
	}
	if (!dataset.set_value("MediaStorageSOPInstanceUID"_tag, VR::UI, sop_instance_uid)) {
		diag::throw_exception(
		    "rebuild_file_meta reason=invalid UID tag={} value={}",
		    "MediaStorageSOPInstanceUID"_tag.to_string(), sop_instance_uid);
	}
	if (!dataset.set_value("TransferSyntaxUID"_tag, VR::UI, transfer_syntax_uid)) {
		diag::throw_exception(
		    "rebuild_file_meta reason=invalid UID tag={} value={}",
		    "TransferSyntaxUID"_tag.to_string(), transfer_syntax_uid);
	}
	if (!dataset.set_value(
	        "ImplementationClassUID"_tag, VR::UI, uid::implementation_class_uid())) {
		diag::throw_exception(
		    "rebuild_file_meta reason=invalid UID tag={} value={}",
		    "ImplementationClassUID"_tag.to_string(), uid::implementation_class_uid());
	}
	if (!dataset.set_value(
	        "ImplementationVersionName"_tag, uid::implementation_version_name())) {
		diag::throw_exception(
		    "rebuild_file_meta reason=failed to set tag={} value={}",
		    "ImplementationVersionName"_tag.to_string(),
		    uid::implementation_version_name());
	}

	const auto meta_group_length = write_detail::measure_meta_group_length(dataset);
	if (!dataset.set_value(
	        "FileMetaInformationGroupLength"_tag, static_cast<long>(meta_group_length))) {
		diag::throw_exception(
		    "rebuild_file_meta reason=failed to set tag={} value={}",
		    "FileMetaInformationGroupLength"_tag.to_string(), meta_group_length);
	}
}

template <typename Fn>
void with_output_file_stream(const fs::path& raw_path, const char* operation_name, Fn&& fn) {
	const fs::path normalized_path = normalize_output_path(raw_path);
	const std::string path_text =
	    detail::normalize_stream_identifier_path(
	        normalized_path.empty() ? raw_path : normalized_path);

	std::ofstream os(normalized_path, std::ios::binary | std::ios::trunc);
	if (!os) {
		diag::error_and_throw("{} path={} reason=failed to open output file",
		    operation_name, path_text);
	}

	std::vector<char> file_buffer(1 << 20);
	os.rdbuf()->pubsetbuf(file_buffer.data(), static_cast<std::streamsize>(file_buffer.size()));

	std::forward<Fn>(fn)(os);
	os.flush();
	if (!os) {
		diag::error_and_throw("{} path={} reason=failed to flush output file",
		    operation_name, path_text);
	}
}

template <typename Writer>
void write_impl(DicomFile& file, Writer& writer, const WriteOptions& options) {
	try {
		if (!options.keep_existing_meta) {
			rebuild_file_meta_or_throw(file);
		}

		const DataSet& dataset = file.dataset();
		dataset.ensure_loaded(Tag(0xFFFFu, 0xFFFFu));
		const auto target_transfer_syntax =
		    write_detail::determine_target_transfer_syntax(file, dataset, options);
		const auto write_plan =
		    write_detail::determine_dataset_write_plan(target_transfer_syntax, dataset);

		if (options.include_preamble) {
			write_detail::write_preamble(writer);
		}

		if (options.write_file_meta) {
			write_detail::write_file_meta_group(writer, dataset);
		}

		write_detail::write_root_dataset_body(writer, dataset, write_plan);
	} catch (const diag::DicomException& ex) {
		if (rebuild_file_meta_exception_has_boundary_prefix(ex.what())) {
			rethrow_rebuild_file_meta_exception_at_write_boundary_or_throw(file, ex);
		}
		write_detail::rethrow_write_exception_at_boundary_or_throw(
		    "write_to_stream", file.path(), ex);
	}
}

}  // namespace

void DicomFile::rebuild_file_meta() {
	try {
		rebuild_file_meta_or_throw(*this);
	} catch (const diag::DicomException& ex) {
		rethrow_rebuild_file_meta_exception_at_public_boundary_or_throw(ex);
	}
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

	write_detail::BufferWriter writer(output);
	write_impl(*this, writer, options);
	return output;
}

void DicomFile::write_file(const std::filesystem::path& path, const WriteOptions& options) {
	with_output_file_stream(path, "write_file",
	    [&](std::ostream& os) { this->write_to_stream(os, options); });
}

void DicomFile::write_with_transfer_syntax(std::ostream& os,
    uid::WellKnown transfer_syntax, const WriteOptions& options) {
	write_detail::StreamWriter writer(os);
	write_detail::write_with_transfer_syntax_to_stream_writer(*this, writer,
	    transfer_syntax, write_detail::WriteEncoderConfigSource::use_plugin_defaults,
	    std::span<const pixel::CodecOptionTextKv>{}, nullptr, options);
}

void DicomFile::write_with_transfer_syntax(std::ostream& os,
    uid::WellKnown transfer_syntax, const pixel::EncoderContext& encoder_ctx,
    const WriteOptions& options) {
	write_detail::StreamWriter writer(os);
	write_detail::write_with_transfer_syntax_to_stream_writer(*this, writer,
	    transfer_syntax, write_detail::WriteEncoderConfigSource::use_encoder_context,
	    std::span<const pixel::CodecOptionTextKv>{}, &encoder_ctx, options);
}

void DicomFile::write_with_transfer_syntax(std::ostream& os,
    uid::WellKnown transfer_syntax,
    std::span<const pixel::CodecOptionTextKv> codec_opt,
    const WriteOptions& options) {
	write_detail::StreamWriter writer(os);
	write_detail::write_with_transfer_syntax_to_stream_writer(*this, writer,
	    transfer_syntax, write_detail::WriteEncoderConfigSource::use_explicit_options,
	    codec_opt, nullptr, options);
}

void DicomFile::write_with_transfer_syntax(const std::filesystem::path& path,
    uid::WellKnown transfer_syntax, const WriteOptions& options) {
	with_output_file_stream(path, "write_with_transfer_syntax",
	    [&](std::ostream& os) { write_with_transfer_syntax(os, transfer_syntax, options); });
}

void DicomFile::write_with_transfer_syntax(const std::filesystem::path& path,
    uid::WellKnown transfer_syntax, const pixel::EncoderContext& encoder_ctx,
    const WriteOptions& options) {
	with_output_file_stream(path, "write_with_transfer_syntax", [&](std::ostream& os) {
		write_with_transfer_syntax(os, transfer_syntax, encoder_ctx, options);
	});
}

void DicomFile::write_with_transfer_syntax(const std::filesystem::path& path,
    uid::WellKnown transfer_syntax,
    std::span<const pixel::CodecOptionTextKv> codec_opt,
    const WriteOptions& options) {
	with_output_file_stream(path, "write_with_transfer_syntax", [&](std::ostream& os) {
		write_with_transfer_syntax(os, transfer_syntax, codec_opt, options);
	});
}

}  // namespace dicom
