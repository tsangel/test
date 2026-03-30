#include <algorithm>
#include <cstdint>
#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <iostream>
#include <sstream>
#include <exception>
#include <stdexcept>
#include <cstdlib>
#include <cstdio>
#include <atomic>
#include <mutex>
#include <thread>

#include <dicom.h>
#include <diagnostics.h>
#include <instream.h>
#include "codec_builtin_flags.hpp"

int main() {
	using dicom::lookup::keyword_to_tag_vr;
	using dicom::lookup::tag_to_keyword;
	using dicom::DicomFile;
	using dicom::DataSet;
	using dicom::read_bytes;
	using dicom::read_file;
	using namespace dicom::literals;

	auto fail = [](const std::string& msg) {
		std::cerr << msg << std::endl;
		std::exit(1);
	};

	const auto [tag, vr] = keyword_to_tag_vr("PatientName");
	if (!tag) fail("keyword_to_tag_vr returned null tag");
	if (tag.value() != 0x00100010u) fail("tag value mismatch");
	if (vr.str() != std::string_view("PN")) fail("vr mismatch");
	if (tag_to_keyword(tag.value()) != std::string_view("PatientName")) fail("keyword roundtrip mismatch");

	const auto seq_vr = dicom::VR::SQ;
	if (!seq_vr.is_sequence()) fail("SQ should be sequence");
	if (seq_vr.is_pixel_sequence()) fail("SQ should not be pixel sequence");

	const auto px_vr = dicom::VR::PX;
	if (px_vr.is_sequence()) fail("PX should not be sequence");
	if (!px_vr.is_pixel_sequence()) fail("PX should be pixel sequence");
	if (!px_vr.is_binary()) fail("PX should be binary");
	if (px_vr.uses_specific_character_set()) fail("PX should not use specific character set");
	if (px_vr.allows_multiple_text_values()) fail("PX should not allow multiple text values");
	if (px_vr.str() != std::string_view("PX")) fail("PX string mismatch");

	const auto pn_vr = dicom::VR::PN;
	if (!pn_vr.uses_specific_character_set()) fail("PN should use specific character set");
	if (!pn_vr.allows_multiple_text_values()) fail("PN should allow multiple text values");

	const auto ut_vr = dicom::VR::UT;
	if (!ut_vr.uses_specific_character_set()) fail("UT should use specific character set");
	if (ut_vr.allows_multiple_text_values()) fail("UT should not allow multiple text values");

	const dicom::Tag literal_tag = "Rows"_tag;
	if (literal_tag.value() != 0x00280010u) fail("literal tag mismatch");
	const dicom::Tag numeric_tag("(0010,0010)");
	if (numeric_tag.value() != 0x00100010u) fail("numeric tag text mismatch");
	const dicom::Tag packed_numeric_tag("00100010");
	if (packed_numeric_tag.value() != 0x00100010u) fail("packed numeric tag text mismatch");

	{
		dicom::InFileStream direct_stream;
		const auto fixture_dir = std::filesystem::path(__FILE__).parent_path();
		const auto raw_path = (fixture_dir / "." / "test_le.dcm").string();
		direct_stream.attach_file(raw_path);
		const auto expected_path =
		    (fixture_dir / "test_le.dcm").lexically_normal().string();
		if (direct_stream.identifier() != expected_path) {
			fail("InFileStream identifier should store normalized file path");
		}
		if (!direct_stream.is_valid()) {
			fail("InFileStream attach_file should expose mapped file data");
		}
	}

	{
		const auto log_path = std::filesystem::temp_directory_path() / "dicomsdl_basic_smoke.log";
		const auto reporter_path = log_path.parent_path() / "." / log_path.filename();
		std::filesystem::remove(log_path);
		std::string line;
		{
			auto reporter = std::make_shared<dicom::diag::FileReporter>(reporter_path);
			dicom::diag::set_thread_reporter(reporter);
			dicom::diag::warn("basic-smoke-file-reporter");
			dicom::diag::set_thread_reporter(nullptr);
		}
		{
			std::ifstream log_file(log_path);
			if (!log_file.is_open()) {
				fail("FileReporter log file should open for reading");
			}
			if (!std::getline(log_file, line)) {
				fail("FileReporter should append a log line");
			}
		}
		if (line.find("basic-smoke-file-reporter") == std::string::npos) {
			fail("FileReporter log line mismatch");
		}
		std::error_code remove_error;
		const bool removed = std::filesystem::remove(log_path, remove_error);
		if (!removed || remove_error) {
			fail("FileReporter cleanup should release the log file before removal");
		}
	}

	if (!dicom::uid::is_valid_uid_text_strict(
	        dicom::uid::uid_prefix())) {
		fail("uid_prefix should be a strict-valid UID");
	}
	if (!dicom::uid::is_valid_uid_text_strict(
	        dicom::uid::implementation_class_uid())) {
		fail("implementation_class_uid should be a strict-valid UID");
	}
	if (dicom::uid::implementation_version_name() !=
	    std::string_view(DICOMSDL_IMPLEMENTATION_VERSION_NAME)) {
		fail("implementation_version_name mismatch");
	}

	const auto suffixed_uid = dicom::uid::make_uid_with_suffix(42u);
	if (!suffixed_uid) fail("make_uid_with_suffix should return a value");
	if (suffixed_uid->value() != std::string_view("1.3.6.1.4.1.56559.42")) {
		fail("make_uid_with_suffix value mismatch");
	}

	const auto generated_uid = dicom::uid::generate_uid();
	if (!dicom::uid::is_valid_uid_text_strict(generated_uid.value())) {
		fail("generate_uid should return strict-valid UID text");
	}
	if (generated_uid.value().rfind(dicom::uid::uid_prefix(), 0) != 0) {
		fail("generate_uid should use DICOMSDL UID prefix");
	}
	const auto generated_uid_nothrow = dicom::uid::try_generate_uid();
	if (!generated_uid_nothrow) {
		fail("try_generate_uid should return a value");
	}
	if (!dicom::uid::is_valid_uid_text_strict(generated_uid_nothrow->value())) {
		fail("try_generate_uid should return strict-valid UID text");
	}
	const auto generated_custom_root =
	    dicom::uid::generate_uid("1.2.826.0.1.3680043.10.543");
	if (!generated_custom_root.value().starts_with(
	        "1.2.826.0.1.3680043.10.543.")) {
		fail("generate_uid(root) should use the requested root");
	}
	if (!dicom::uid::is_valid_uid_text_strict(generated_custom_root.value())) {
		fail("generate_uid(root) should remain strict-valid");
	}
	const auto from_uid_a = dicom::uid::generate_uid_from(
	    "same-key", "1.2.826.0.1.3680043.10.543");
	const auto from_uid_b = dicom::uid::generate_uid_from(
	    "same-key", "1.2.826.0.1.3680043.10.543");
	const auto from_uid_c = dicom::uid::generate_uid_from(
	    "different-key", "1.2.826.0.1.3680043.10.543");
	if (from_uid_a.value() != from_uid_b.value()) {
		fail("generate_uid_from(key, root) should be deterministic for the same key");
	}
	if (from_uid_a.value() == from_uid_c.value()) {
		fail("generate_uid_from(key, root) should vary across different keys");
	}
	if (!dicom::uid::is_valid_uid_text_strict(from_uid_a.value())) {
		fail("generate_uid_from(key, root) should remain strict-valid");
	}

	{
		const auto temp_root =
		    std::filesystem::temp_directory_path() / "dicomsdl_uid_remapper_smoke";
		const auto default_journal = temp_root / "default.tsv";
		const auto buffered_journal = temp_root / "buffered.tsv";
		const auto custom_journal = temp_root / "custom.tsv";
		std::error_code cleanup_ec;
		std::filesystem::remove_all(temp_root, cleanup_ec);
		std::filesystem::create_directories(temp_root, cleanup_ec);
		if (cleanup_ec) {
			fail("UidRemapper smoke setup should create temporary journal directory");
		}

		const std::string source_uid_a = "1.2.840.113619.2.55.3.604688435.123.456";
		const std::string source_uid_b = "1.2.840.113619.2.55.3.604688435.123.457";
		std::string mapped_a;
		std::string mapped_b;

		{
			auto remapper = dicom::UidRemapper::in_memory(default_journal);
			if (!remapper.is_valid()) {
				fail("UidRemapper::in_memory should create a valid remapper");
			}
			bool threw_duplicate_open = false;
			try {
				auto duplicate = dicom::UidRemapper::in_memory(default_journal);
				(void)duplicate;
			} catch (const std::exception&) {
				threw_duplicate_open = true;
			}
			if (!threw_duplicate_open) {
				fail("UidRemapper should reject opening the same journal twice at once");
			}
			mapped_a = remapper.map_uid(source_uid_a);
			if (mapped_a.rfind(dicom::uid::uid_prefix(), 0) != 0) {
				fail("UidRemapper default root should use dicomsdl uid prefix");
			}
			if (!dicom::uid::is_valid_uid_text_strict(mapped_a)) {
				fail("UidRemapper mapped UID should be strict-valid");
			}
			if (remapper.map_uid(source_uid_a) != mapped_a) {
				fail("UidRemapper should reuse existing mapping for same source UID");
			}
			mapped_b = remapper.map_uid(source_uid_b);
			if (mapped_b == mapped_a) {
				fail("UidRemapper should create different mappings for different source UIDs");
			}
		}

		{
			auto replayed = dicom::UidRemapper::in_memory(default_journal);
			if (replayed.map_uid(source_uid_a) != mapped_a) {
				fail("UidRemapper should replay existing mapping from journal");
			}
			if (replayed.map_uid(source_uid_b) != mapped_b) {
				fail("UidRemapper should replay second mapping from journal");
			}
		}

		{
			auto buffered = dicom::UidRemapper::in_memory(
			    buffered_journal, dicom::uid::uid_prefix(), false);
			const auto buffered_target = buffered.map_uid(source_uid_a);
			buffered.close();
			auto replayed = dicom::UidRemapper::in_memory(buffered_journal);
			if (replayed.map_uid(source_uid_a) != buffered_target) {
				fail("UidRemapper buffered mode should persist mappings after explicit close");
			}
		}

		{
			auto remapper = dicom::UidRemapper::in_memory(
			    custom_journal, "1.2.826.0.1.3680043.10.543");
			const auto custom_target = remapper.map_uid(source_uid_a);
			if (!custom_target.starts_with("1.2.826.0.1.3680043.10.543.")) {
				fail("UidRemapper custom root should use requested root");
			}
			if (!dicom::uid::is_valid_uid_text_strict(custom_target)) {
				fail("UidRemapper custom root mapping should be strict-valid");
			}
			remapper.close();
			if (remapper.is_valid()) {
				fail("UidRemapper should become invalid after close");
			}
			bool threw_after_close = false;
			try {
				(void)remapper.map_uid(source_uid_b);
			} catch (const std::exception&) {
				threw_after_close = true;
			}
			if (!threw_after_close) {
				fail("UidRemapper should reject map_uid after close");
			}
		}

		std::filesystem::remove_all(temp_root, cleanup_ec);
		if (cleanup_ec) {
			fail("UidRemapper smoke cleanup should remove temporary journal files");
		}
	}

	{
		const auto temp_root =
		    std::filesystem::temp_directory_path() / "dicomsdl_rewrite_uids_smoke";
		const auto file_journal = temp_root / "file.tsv";
		const auto dataset_journal = temp_root / "dataset.tsv";
		std::error_code cleanup_ec;
		std::filesystem::remove_all(temp_root, cleanup_ec);
		std::filesystem::create_directories(temp_root, cleanup_ec);
		if (cleanup_ec) {
			fail("rewrite_uids smoke setup should create temporary journal directory");
		}

		{
			DicomFile file;
			auto& dataset = file.dataset();
			const std::string study_uid = "1.2.840.113619.2.55.3.604688435.100";
			const std::string series_uid = "1.2.840.113619.2.55.3.604688435.101";
			const std::string sop_uid = "1.2.840.113619.2.55.3.604688435.102";
			const std::string rtv_sop_uid = "1.2.840.113619.2.55.3.604688435.1021";
			const std::string referenced_uid = "1.2.840.113619.2.55.3.604688435.103";
			const std::string referenced_in_file_uid =
			    "1.2.840.113619.2.55.3.604688435.104";
			const std::string frame_uid = "1.2.840.113619.2.55.3.604688435.105";
			if (!dataset.set_value("StudyInstanceUID"_tag, study_uid) ||
			    !dataset.set_value("SeriesInstanceUID"_tag, series_uid) ||
			    !dataset.set_value("SOPInstanceUID"_tag, sop_uid) ||
			    !dataset.set_value("MediaStorageSOPInstanceUID"_tag, sop_uid) ||
			    !dataset.set_value("RTVCommunicationSOPInstanceUID"_tag, rtv_sop_uid) ||
			    !dataset.set_value("FrameOfReferenceUID"_tag, frame_uid) ||
			    !dataset.set_value("PatientName"_tag, "Rewrite^Smoke")) {
				fail("rewrite_uids smoke should populate root DicomFile dataset");
			}
			auto* sequence =
			    dataset.ensure_dataelement("ReferencedStudySequence"_tag, dicom::VR::SQ)
			        .as_sequence();
			if (sequence == nullptr) {
				fail("rewrite_uids smoke should create nested sequence");
			}
			auto* item = sequence->add_dataset();
			if (item == nullptr ||
			    !item->set_value("ReferencedSOPInstanceUID"_tag, referenced_uid) ||
			    !item->set_value(
			        "ReferencedSOPInstanceUIDInFile"_tag, referenced_in_file_uid)) {
				fail("rewrite_uids smoke should populate nested referenced UID elements");
			}

			auto remapper = dicom::UidRemapper::in_memory(
			    file_journal, "1.2.826.0.1.3680043.10.543", false);
			const auto rewritten = dicom::rewrite_uids(file, remapper);
			if (rewritten != 7) {
				fail("rewrite_uids(DicomFile) should rewrite the default Study/Series/SOP targets");
			}

			const auto expected_study = remapper.map_uid(study_uid);
			const auto expected_series = remapper.map_uid(series_uid);
			const auto expected_sop = remapper.map_uid(sop_uid);
			const auto expected_rtv_sop = remapper.map_uid(rtv_sop_uid);
			const auto expected_referenced = remapper.map_uid(referenced_uid);
			const auto expected_referenced_in_file =
			    remapper.map_uid(referenced_in_file_uid);

			const auto actual_study =
			    dataset.get_dataelement("StudyInstanceUID"_tag).to_uid_string();
			const auto actual_series =
			    dataset.get_dataelement("SeriesInstanceUID"_tag).to_uid_string();
			const auto actual_sop =
			    dataset.get_dataelement("SOPInstanceUID"_tag).to_uid_string();
			const auto actual_meta_sop =
			    dataset.get_dataelement("MediaStorageSOPInstanceUID"_tag).to_uid_string();
			const auto actual_rtv_sop =
			    dataset.get_dataelement("RTVCommunicationSOPInstanceUID"_tag).to_uid_string();
			const auto actual_referenced =
			    dataset.get_dataelement(
			               "ReferencedStudySequence.0.ReferencedSOPInstanceUID")
			        .to_uid_string();
			const auto actual_referenced_in_file =
			    dataset.get_dataelement(
			               "ReferencedStudySequence.0.ReferencedSOPInstanceUIDInFile")
			        .to_uid_string();
			const auto actual_frame =
			    dataset.get_dataelement("FrameOfReferenceUID"_tag).to_uid_string();
			if (!actual_study || *actual_study != expected_study ||
			    !actual_series || *actual_series != expected_series ||
			    !actual_sop || *actual_sop != expected_sop ||
			    !actual_meta_sop || *actual_meta_sop != expected_sop ||
			    !actual_rtv_sop || *actual_rtv_sop != expected_rtv_sop ||
			    !actual_referenced || *actual_referenced != expected_referenced ||
			    !actual_referenced_in_file ||
			        *actual_referenced_in_file != expected_referenced_in_file) {
				fail("rewrite_uids(DicomFile) should rewrite selected UID elements consistently");
			}
			if (!actual_frame || *actual_frame != frame_uid) {
				fail("rewrite_uids(DicomFile) should leave FrameOfReferenceUID unchanged by default");
			}
			if (dataset.get_dataelement("PatientName"_tag).to_string_view() !=
			    std::optional<std::string_view>("Rewrite^Smoke")) {
				fail("rewrite_uids(DicomFile) should not touch non-UI elements");
			}
			if (!dicom::uid::is_valid_uid_text_strict(expected_study) ||
			    !dicom::uid::is_valid_uid_text_strict(expected_series) ||
			    !dicom::uid::is_valid_uid_text_strict(expected_sop) ||
			    !dicom::uid::is_valid_uid_text_strict(expected_rtv_sop) ||
			    !dicom::uid::is_valid_uid_text_strict(expected_referenced) ||
			    !dicom::uid::is_valid_uid_text_strict(expected_referenced_in_file)) {
				fail("rewrite_uids(DicomFile) should produce strict-valid mapped UIDs");
			}
		}

		{
			DataSet dataset;
			const std::string study_uid = "1.2.840.113619.2.55.3.604688435.200";
			const std::string series_uid = "1.2.840.113619.2.55.3.604688435.201";
			const std::string sop_uid = "1.2.840.113619.2.55.3.604688435.202";
			const std::string frame_uid = "1.2.840.113619.2.55.3.604688435.203";
			const std::string referenced_uid = "1.2.840.113619.2.55.3.604688435.204";
			if (!dataset.set_value("StudyInstanceUID"_tag, study_uid) ||
			    !dataset.set_value("SeriesInstanceUID"_tag, series_uid) ||
			    !dataset.set_value("SOPInstanceUID"_tag, sop_uid) ||
			    !dataset.set_value("FrameOfReferenceUID"_tag, frame_uid)) {
				fail("rewrite_uids(DataSet) smoke should populate root UID elements");
			}
			auto* sequence =
			    dataset.ensure_dataelement("ReferencedStudySequence"_tag, dicom::VR::SQ)
			        .as_sequence();
			if (sequence == nullptr) {
				fail("rewrite_uids(DataSet) smoke should create nested sequence");
			}
			auto* item = sequence->add_dataset();
			if (item == nullptr ||
			    !item->set_value("ReferencedSOPInstanceUID"_tag, referenced_uid)) {
				fail("rewrite_uids(DataSet) smoke should populate nested referenced UID");
			}

			dicom::RewriteUidOptions options;
			options.rewrite_study_instance_uid = false;
			options.rewrite_sop_instance_uids = false;
			options.rewrite_frame_of_reference_uids = true;

			auto remapper = dicom::UidRemapper::in_memory(
			    dataset_journal, "1.2.826.0.1.3680043.10.544", false);
			const auto rewritten = dicom::rewrite_uids(dataset, remapper, options);
			if (rewritten != 2) {
				fail("rewrite_uids(DataSet) should honor RewriteUidOptions");
			}

			const auto expected_series = remapper.map_uid(series_uid);
			const auto expected_frame = remapper.map_uid(frame_uid);
			const auto actual_study =
			    dataset.get_dataelement("StudyInstanceUID"_tag).to_uid_string();
			const auto actual_series =
			    dataset.get_dataelement("SeriesInstanceUID"_tag).to_uid_string();
			const auto actual_sop =
			    dataset.get_dataelement("SOPInstanceUID"_tag).to_uid_string();
			const auto actual_frame =
			    dataset.get_dataelement("FrameOfReferenceUID"_tag).to_uid_string();
			const auto actual_referenced =
			    dataset.get_dataelement(
			               "ReferencedStudySequence.0.ReferencedSOPInstanceUID")
			        .to_uid_string();
			if (!actual_study || *actual_study != study_uid ||
			    !actual_sop || *actual_sop != sop_uid ||
			    !actual_referenced || *actual_referenced != referenced_uid) {
				fail("rewrite_uids(DataSet) should leave disabled UID categories unchanged");
			}
			if (!actual_series || *actual_series != expected_series ||
			    !actual_frame || *actual_frame != expected_frame) {
				fail("rewrite_uids(DataSet) should rewrite the enabled UID categories");
			}
		}

		std::filesystem::remove_all(temp_root, cleanup_ec);
		if (cleanup_ec) {
			fail("rewrite_uids smoke cleanup should remove temporary journal files");
		}
	}

	{
		DicomFile source;
		source.set_declared_specific_charset(dicom::SpecificCharacterSet::ISO_IR_100);
		auto& source_root = source.dataset();
		const dicom::Tag top_private_tag(0x0009, 0x1001);
		const dicom::Tag top_unknown_tag(0x7777, 0x0010);
		const dicom::Tag nested_private_tag(0x0009, 0x1002);
		if (!source_root.set_value("StudyInstanceUID"_tag, "1.2.840.10008.100")) {
			fail("selected read smoke should set StudyInstanceUID");
		}
		if (!source_root.set_value("SOPInstanceUID"_tag, "1.2.840.10008.200")) {
			fail("selected read smoke should set top-level SOPInstanceUID");
		}
		if (!source_root.set_value("PatientName"_tag, "Test^Patient")) {
			fail("selected read smoke should set top-level PatientName");
		}
		if (!source_root.set_value(top_private_tag, dicom::VR::LO, "Top^Private")) {
			fail("selected read smoke should set top-level private element");
		}
		if (!source_root.set_value(top_unknown_tag, dicom::VR::LO, "Top^Unknown")) {
			fail("selected read smoke should set top-level unknown element");
		}
		auto* referenced_series =
		    source_root.ensure_dataelement("ReferencedSeriesSequence"_tag, dicom::VR::SQ)
		        .as_sequence();
		if (referenced_series == nullptr) {
			fail("selected read smoke should create ReferencedSeriesSequence");
		}
		auto* series_item0 = referenced_series->add_dataset();
		auto* series_item1 = referenced_series->add_dataset();
		if (series_item0 == nullptr || series_item1 == nullptr) {
			fail("selected read smoke should create ReferencedSeriesSequence items");
		}
		if (!series_item0->set_value("SeriesInstanceUID"_tag, "1.2.840.10008.301") ||
		    !series_item0->set_value("ReferencedSOPInstanceUID"_tag, "1.2.840.10008.401") ||
		    !series_item0->set_value("PatientName"_tag, "Nested^Ignored") ||
		    !series_item0->set_value(nested_private_tag, dicom::VR::LO, "Nested^Private0")) {
			fail("selected read smoke should populate first sequence item");
		}
		if (!series_item1->set_value("SeriesInstanceUID"_tag, "1.2.840.10008.302") ||
		    !series_item1->set_value("ReferencedSOPInstanceUID"_tag, "1.2.840.10008.402") ||
		    !series_item1->set_value("PatientName"_tag, "Nested^Ignored2") ||
		    !series_item1->set_value(nested_private_tag, dicom::VR::LO, "Nested^Private1")) {
			fail("selected read smoke should populate second sequence item");
		}

		auto source_bytes = source.write_bytes();

		dicom::DataSetSelection selection{
		    "SOPInstanceUID"_tag,
		    {"ReferencedSeriesSequence"_tag, {
		        "SeriesInstanceUID"_tag,
		        nested_private_tag,
		    }},
		    "StudyInstanceUID"_tag,
		    top_private_tag,
		    top_unknown_tag,
		    {"ReferencedSeriesSequence"_tag, {
		        "ReferencedSOPInstanceUID"_tag,
		    }},
		};
		if (selection.nodes().size() != 7) {
			fail("DataSetSelection should inject default root metadata and keep private/unknown top-level nodes");
		}
		if (selection.nodes()[0].tag != "TransferSyntaxUID"_tag ||
		    selection.nodes()[1].tag != "SpecificCharacterSet"_tag ||
		    selection.nodes()[2].tag != "SOPInstanceUID"_tag ||
		    selection.nodes()[3].tag != "ReferencedSeriesSequence"_tag ||
		    selection.nodes()[4].tag != top_private_tag ||
		    selection.nodes()[5].tag != "StudyInstanceUID"_tag ||
		    selection.nodes()[6].tag != top_unknown_tag) {
			fail("DataSetSelection should sort top-level nodes, inject default metadata, and preserve private/unknown tags");
		}
		if (selection.nodes()[3].children.size() != 3 ||
		    selection.nodes()[3].children[0].tag != "ReferencedSOPInstanceUID"_tag ||
		    selection.nodes()[3].children[1].tag != nested_private_tag ||
		    selection.nodes()[3].children[2].tag != "SeriesInstanceUID"_tag) {
			fail("DataSetSelection should sort and merge nested children including private tags");
		}
		dicom::DataSetSelection default_selection;
		if (default_selection.nodes().size() != 2 ||
		    default_selection.nodes()[0].tag != "TransferSyntaxUID"_tag ||
		    default_selection.nodes()[1].tag != "SpecificCharacterSet"_tag) {
			fail("default DataSetSelection should canonicalize to the implicit root metadata selection");
		}

		dicom::ReadOptions selected_options;
		selected_options.load_until = dicom::Tag::from_value(0);
		auto selected = read_bytes_selected(
		    source_bytes.data(), source_bytes.size(), selection, selected_options);
		if (selected->transfer_syntax_uid() != source.transfer_syntax_uid()) {
			fail("read_bytes_selected should preserve transfer syntax state even when not selected");
		}
		if (!selected->get_dataelement("(0008,0005)"_tag)) {
			fail("read_bytes_selected should include SpecificCharacterSet even when not selected");
		}
		if (!selected->get_dataelement("StudyInstanceUID"_tag) ||
		    !selected->get_dataelement("SOPInstanceUID"_tag) ||
		    !selected->get_dataelement(top_private_tag) ||
		    !selected->get_dataelement(top_unknown_tag)) {
			fail("read_bytes_selected should keep explicitly selected top-level elements");
		}
		if (selected->get_dataelement("PatientName"_tag)) {
			fail("read_bytes_selected should omit unselected top-level elements");
		}

		const auto& selected_series_sequence =
		    selected->get_dataelement("ReferencedSeriesSequence"_tag);
		if (!selected_series_sequence || selected_series_sequence.vr() != dicom::VR::SQ) {
			fail("read_bytes_selected should keep selected SQ elements");
		}
		const auto* selected_sequence = selected_series_sequence.as_sequence();
		if (selected_sequence == nullptr || selected_sequence->size() != 2) {
			fail("read_bytes_selected should preserve selected sequence item count");
		}
		const auto* selected_item0 = selected_sequence->get_dataset(0);
		const auto* selected_item1 = selected_sequence->get_dataset(1);
		if (selected_item0 == nullptr || selected_item1 == nullptr) {
			fail("read_bytes_selected should materialize selected sequence item datasets");
		}
		if (!selected_item0->get_dataelement("SeriesInstanceUID"_tag) ||
		    !selected_item0->get_dataelement("ReferencedSOPInstanceUID"_tag) ||
		    !selected_item0->get_dataelement(nested_private_tag) ||
		    selected_item0->get_dataelement("PatientName"_tag)) {
			fail("read_bytes_selected should project only the selected children for each sequence item");
		}
		if (!selected_item1->get_dataelement("SeriesInstanceUID"_tag) ||
		    !selected_item1->get_dataelement("ReferencedSOPInstanceUID"_tag) ||
		    !selected_item1->get_dataelement(nested_private_tag) ||
		    selected_item1->get_dataelement("PatientName"_tag)) {
			fail("read_bytes_selected should project selected children for later sequence items");
		}

		dicom::DataSetSelection sequence_only_selection{
		    "ReferencedSeriesSequence"_tag,
		};
		auto sequence_only = read_bytes_selected(
		    source_bytes.data(), source_bytes.size(), sequence_only_selection);
		const auto& sequence_only_element =
		    sequence_only->get_dataelement("ReferencedSeriesSequence"_tag);
		const auto* sequence_only_value = sequence_only_element.as_sequence();
		if (!sequence_only_element || sequence_only_value == nullptr ||
		    sequence_only_value->size() != 2) {
			fail("read_bytes_selected should keep a present SQ even when no child selection is provided");
		}
		const auto* empty_item0 = sequence_only_value->get_dataset(0);
		const auto* empty_item1 = sequence_only_value->get_dataset(1);
		if (empty_item0 == nullptr || empty_item1 == nullptr ||
		    empty_item0->size() != 0 || empty_item1->size() != 0) {
			fail("read_bytes_selected should keep sequence items empty when only the SQ itself is selected");
		}

		const auto malformed_selected = [] {
			std::vector<std::uint8_t> bytes(128, 0);
			bytes.insert(bytes.end(), {'D', 'I', 'C', 'M'});
			bytes.insert(bytes.end(), {0x10, 0x00, 0x10, 0x00, 'P', 'N', 0x08, 0x00});
			bytes.insert(bytes.end(), {'A', 'B'});
			return bytes;
		}();
		bool malformed_selected_threw = false;
		try {
			[[maybe_unused]] auto should_throw = read_bytes_selected(
			    "malformed-selected-default", malformed_selected.data(), malformed_selected.size(),
			    dicom::DataSetSelection{"PatientName"_tag});
		} catch (const std::exception&) {
			malformed_selected_threw = true;
		}
		if (!malformed_selected_threw) {
			fail("selected read should throw for malformed input when keep_on_error is false");
		}

		dicom::ReadOptions malformed_selected_keep_options;
		malformed_selected_keep_options.keep_on_error = true;
		const auto malformed_selected_keep = read_bytes_selected(
		    "malformed-selected-keep", malformed_selected.data(), malformed_selected.size(),
		    dicom::DataSetSelection{"PatientName"_tag}, malformed_selected_keep_options);
		if (!malformed_selected_keep) {
			fail("selected read keep_on_error should still return a DicomFile");
		}
		if (!malformed_selected_keep->has_error()) {
			fail("selected read keep_on_error should record has_error=true");
		}
		if (malformed_selected_keep->error_message().empty()) {
			fail("selected read keep_on_error should keep error_message");
		}
		if (malformed_selected_keep->size() == 0) {
			fail("selected read keep_on_error should preserve partially parsed elements");
		}
	}

	{
		const auto repo_root = std::filesystem::path(__FILE__).parent_path().parent_path();
		const auto tutorial_root = repo_root / "tutorials";
		const auto tutorial_ct1_unc = tutorial_root / "CT1_UNC";
		const auto tutorial_ct2_jlsn = tutorial_root / "CT2_JLSN";

		auto require_selected_uid = [&](const dicom::DicomFile& full, const dicom::DicomFile& selected,
		                                dicom::Tag tag, const std::string& context) {
			const auto full_uid = full.get_dataelement(tag).to_uid_string();
			const auto selected_uid = selected.get_dataelement(tag).to_uid_string();
			if (!full_uid || !selected_uid || *full_uid != *selected_uid) {
				fail(context);
			}
		};
		auto require_selected_string =
		    [&](const dicom::DicomFile& full, const dicom::DicomFile& selected,
		        std::string_view tag_path, const std::string& context) {
			    const auto full_text = full.get_value<std::string>(tag_path);
			    const auto selected_text = selected.get_value<std::string>(tag_path);
			    if (!full_text || !selected_text || *full_text != *selected_text) {
				    fail(context);
			    }
		    };
		auto require_selected_long =
		    [&](const dicom::DicomFile& full, const dicom::DicomFile& selected,
		        std::string_view tag_path, const std::string& context) {
			    const auto full_value = full.get_value<long long>(tag_path);
			    const auto selected_value = selected.get_value<long long>(tag_path);
			    if (!full_value || !selected_value || *full_value != *selected_value) {
				    fail(context);
			    }
		    };

		if (std::filesystem::exists(tutorial_ct1_unc)) {
			dicom::DataSetSelection selection{
			    "StudyInstanceUID"_tag,
			    "SOPInstanceUID"_tag,
			    "Rows"_tag,
			    "Columns"_tag,
			};
			dicom::ReadOptions selected_options;
			selected_options.load_until = dicom::Tag::from_value(0);

			auto full = read_file(tutorial_ct1_unc);
			auto selected = read_file_selected(tutorial_ct1_unc, selection, selected_options);
			if (selected->transfer_syntax_uid() != full->transfer_syntax_uid() ||
			    selected->transfer_syntax_uid() != "ExplicitVRLittleEndian"_uid) {
				fail("selected read should preserve uncompressed tutorial transfer syntax state");
			}
			if (!selected->get_dataelement("TransferSyntaxUID"_tag)) {
				fail("selected read should keep TransferSyntaxUID in tutorial CT1_UNC sample");
			}
			if (selected->get_dataelement("FileMetaInformationVersion"_tag) ||
			    selected->get_dataelement("MediaStorageSOPClassUID"_tag) ||
			    selected->get_dataelement("ImplementationClassUID"_tag)) {
				fail("selected read should omit unselected file meta elements and keep only TransferSyntaxUID");
			}
			if (!selected->get_dataelement("SpecificCharacterSet"_tag)) {
				fail("selected read should keep SpecificCharacterSet in tutorial CT1_UNC sample");
			}
			require_selected_uid(
			    *full, *selected, "StudyInstanceUID"_tag,
			    "selected read should preserve StudyInstanceUID from tutorial CT1_UNC sample");
			require_selected_uid(
			    *full, *selected, "SOPInstanceUID"_tag,
			    "selected read should preserve SOPInstanceUID from tutorial CT1_UNC sample");
			require_selected_long(
			    *full, *selected, "Rows",
			    "selected read should preserve Rows from tutorial CT1_UNC sample");
			require_selected_long(
			    *full, *selected, "Columns",
			    "selected read should preserve Columns from tutorial CT1_UNC sample");
			if (selected->get_dataelement("PatientName"_tag)) {
				fail("selected read should omit unselected top-level PatientName in tutorial CT1_UNC sample");
			}
			if (selected->get_dataelement("SeriesInstanceUID"_tag)) {
				fail("selected read should omit unselected SeriesInstanceUID in tutorial CT1_UNC sample");
			}
		}

		if (std::filesystem::exists(tutorial_ct2_jlsn)) {
			dicom::DataSetSelection selection{
			    "StudyInstanceUID"_tag,
			    {"SourceImageSequence"_tag, {
			        "ReferencedSOPClassUID"_tag,
			        "ReferencedSOPInstanceUID"_tag,
			        {"PurposeOfReferenceCodeSequence"_tag, {
			            "CodeValue"_tag,
			            "CodingSchemeDesignator"_tag,
			            "CodeMeaning"_tag,
			        }},
			    }},
			    {"DerivationCodeSequence"_tag, {
			        "CodeValue"_tag,
			        "CodingSchemeDesignator"_tag,
			        "CodeMeaning"_tag,
			    }},
			};
			dicom::ReadOptions selected_options;
			selected_options.load_until = dicom::Tag::from_value(0);

			auto full = read_file(tutorial_ct2_jlsn);
			auto selected = read_file_selected(tutorial_ct2_jlsn, selection, selected_options);
			if (selected->transfer_syntax_uid() != full->transfer_syntax_uid() ||
			    selected->transfer_syntax_uid() != "JPEGLSNearLossless"_uid) {
				fail("selected read should preserve tutorial CT2_JLSN transfer syntax state");
			}
			if (!selected->get_dataelement("TransferSyntaxUID"_tag)) {
				fail("selected read should keep TransferSyntaxUID in tutorial CT2_JLSN sample");
			}
			if (selected->get_dataelement("SpecificCharacterSet"_tag)) {
				fail("selected read should not materialize absent SpecificCharacterSet in tutorial CT2_JLSN sample");
			}
			require_selected_uid(
			    *full, *selected, "StudyInstanceUID"_tag,
			    "selected read should preserve StudyInstanceUID from tutorial CT2_JLSN sample");
			require_selected_string(
			    *full, *selected, "SourceImageSequence.0.ReferencedSOPClassUID",
			    "selected read should preserve ReferencedSOPClassUID from tutorial CT2_JLSN sample");
			require_selected_string(
			    *full, *selected, "SourceImageSequence.0.ReferencedSOPInstanceUID",
			    "selected read should preserve ReferencedSOPInstanceUID from tutorial CT2_JLSN sample");
			require_selected_string(
			    *full, *selected, "SourceImageSequence.0.PurposeOfReferenceCodeSequence.0.CodeValue",
			    "selected read should preserve nested PurposeOfReference CodeValue in tutorial CT2_JLSN sample");
			require_selected_string(
			    *full, *selected, "SourceImageSequence.0.PurposeOfReferenceCodeSequence.0.CodingSchemeDesignator",
			    "selected read should preserve nested PurposeOfReference CodingSchemeDesignator in tutorial CT2_JLSN sample");
			require_selected_string(
			    *full, *selected, "SourceImageSequence.0.PurposeOfReferenceCodeSequence.0.CodeMeaning",
			    "selected read should preserve nested PurposeOfReference CodeMeaning in tutorial CT2_JLSN sample");
			require_selected_string(
			    *full, *selected, "DerivationCodeSequence.0.CodeValue",
			    "selected read should preserve nested DerivationCodeSequence CodeValue in tutorial CT2_JLSN sample");
			require_selected_string(
			    *full, *selected, "DerivationCodeSequence.0.CodingSchemeDesignator",
			    "selected read should preserve nested DerivationCodeSequence CodingSchemeDesignator in tutorial CT2_JLSN sample");
			require_selected_string(
			    *full, *selected, "DerivationCodeSequence.0.CodeMeaning",
			    "selected read should preserve nested DerivationCodeSequence CodeMeaning in tutorial CT2_JLSN sample");
			if (selected->get_dataelement("SOPInstanceUID"_tag)) {
				fail("selected read should omit unselected SOPInstanceUID in tutorial CT2_JLSN sample");
			}
			if (selected->get_dataelement("PatientName"_tag)) {
				fail("selected read should omit unselected PatientName in tutorial CT2_JLSN sample");
			}
			const auto* full_source_image =
			    full->get_dataelement("SourceImageSequence"_tag).as_sequence();
			const auto* selected_source_image =
			    selected->get_dataelement("SourceImageSequence"_tag).as_sequence();
			if (full_source_image == nullptr || selected_source_image == nullptr ||
			    full_source_image->size() != selected_source_image->size()) {
				fail("selected read should preserve SourceImageSequence item count in tutorial CT2_JLSN sample");
			}
			const auto* full_derivation =
			    full->get_dataelement("DerivationCodeSequence"_tag).as_sequence();
			const auto* selected_derivation =
			    selected->get_dataelement("DerivationCodeSequence"_tag).as_sequence();
			if (full_derivation == nullptr || selected_derivation == nullptr ||
			    full_derivation->size() != selected_derivation->size()) {
				fail("selected read should preserve DerivationCodeSequence item count in tutorial CT2_JLSN sample");
			}
		}
	}

	{
		DataSet dataset;
		if (!dataset.set_value("SOPInstanceUID"_tag, "1.2.840.10008.1")) {
			fail("DataSet walk smoke should set top-level SOPInstanceUID");
		}
		if (!dataset.set_value("SeriesInstanceUID"_tag, "1.2.840.10008.10")) {
			fail("DataSet walk smoke should set trailing top-level SeriesInstanceUID");
		}
		auto* sequence = dataset.ensure_dataelement("ReferencedStudySequence"_tag, dicom::VR::SQ)
		                     .as_sequence();
		if (sequence == nullptr) {
			fail("DataSet walk smoke should create sequence element");
		}
		auto* item0 = sequence->add_dataset();
		auto* item1 = sequence->add_dataset();
		if (item0 == nullptr || item1 == nullptr) {
			fail("DataSet walk smoke should create sequence item datasets");
		}
		if (!item0->set_value("ReferencedSOPInstanceUID"_tag, "1.2.3")) {
			fail("DataSet walk smoke should set item 0 referenced SOP UID");
		}
		if (!item0->set_value("SeriesInstanceUID"_tag, "1.2.30")) {
			fail("DataSet walk smoke should set item 0 SeriesInstanceUID");
		}
		if (!item1->set_value("ReferencedSOPInstanceUID"_tag, "1.2.4")) {
			fail("DataSet walk smoke should set item 1 referenced SOP UID");
		}
		if (!item1->set_value("SeriesInstanceUID"_tag, "1.2.40")) {
			fail("DataSet walk smoke should set item 1 SeriesInstanceUID");
		}

		std::vector<std::uint32_t> walked_tags;
		std::vector<std::string> walked_paths;
		for (auto entry : dataset.walk()) {
			walked_tags.push_back(entry.element.tag().value());
			walked_paths.push_back(entry.path.to_string());
		}
		if (walked_tags.size() != 7) {
			fail("DataSet::walk should visit top-level and nested sequence elements");
		}
		if (walked_tags[0] != "SOPInstanceUID"_tag.value() ||
		    walked_paths[0] != std::string()) {
			fail("DataSet::walk should start with top-level elements and empty path");
		}
		if (walked_tags[1] != "ReferencedStudySequence"_tag.value() ||
		    walked_paths[1] != std::string()) {
			fail("DataSet::walk should include SQ elements before their children");
		}
		if (walked_tags[2] != "ReferencedSOPInstanceUID"_tag.value() ||
		    walked_paths[2] != std::string("00081110.0")) {
			fail("DataSet::walk should report ancestors-only path for first nested item");
		}
		if (walked_tags[3] != "SeriesInstanceUID"_tag.value() ||
		    walked_paths[3] != std::string("00081110.0")) {
			fail("DataSet::walk should keep the same ancestors-only path within one nested item");
		}
		if (walked_tags[4] != "ReferencedSOPInstanceUID"_tag.value() ||
		    walked_paths[4] != std::string("00081110.1")) {
			fail("DataSet::walk should report ancestors-only path for later nested items");
		}
		if (walked_tags[5] != "SeriesInstanceUID"_tag.value() ||
		    walked_paths[5] != std::string("00081110.1")) {
			fail("DataSet::walk should keep the same ancestors-only path for later nested items");
		}
		if (walked_tags[6] != "SeriesInstanceUID"_tag.value() ||
		    walked_paths[6] != std::string()) {
			fail("DataSet::walk should continue with later top-level elements after nested items");
		}

		std::vector<std::uint32_t> visited_tags;
		std::vector<std::string> visited_paths;
		dataset.visit([&](auto path, auto& element) {
			visited_tags.push_back(element.tag().value());
			visited_paths.push_back(path.to_string());
		});
		if (visited_tags != walked_tags || visited_paths != walked_paths) {
			fail("DataSet::visit should match DataSet::walk traversal order and path semantics");
		}

		std::vector<std::uint32_t> visit_pruned_tags;
		dataset.visit([&](auto path, auto& element) -> dicom::DataSetVisitControl {
			visit_pruned_tags.push_back(element.tag().value());
			if (element.tag() == "ReferencedStudySequence"_tag) {
				if (!path.empty()) {
					fail("DataSet::visit sequence path should be empty at the root dataset");
				}
				if (path.contains_sequence("ReferencedStudySequence"_tag)) {
					fail("DataSet::visit path should not include the current leaf sequence");
				}
				return dicom::DataSetVisitControl::skip_sequence;
			}
			return dicom::DataSetVisitControl::continue_;
		});
		if (std::find(
		        visit_pruned_tags.begin(),
		        visit_pruned_tags.end(),
		        "ReferencedSOPInstanceUID"_tag.value()) != visit_pruned_tags.end()) {
			fail("DataSet::visit skip_sequence should prune the current SQ subtree");
		}

		std::size_t visit_stop_count = 0;
		dataset.visit([&](auto, auto& element) -> dicom::DataSetVisitControl {
			++visit_stop_count;
			if (element.tag() == "ReferencedStudySequence"_tag) {
				return dicom::DataSetVisitControl::stop;
			}
			return dicom::DataSetVisitControl::continue_;
		});
		if (visit_stop_count != 2) {
			fail("DataSet::visit stop should end traversal immediately after the current callback");
		}

		bool visit_saw_item0_series = false;
		bool visit_saw_item1_referenced = false;
		bool visit_saw_top_level_series = false;
		dataset.visit([&](auto path, auto& element) -> dicom::DataSetVisitControl {
			const auto path_text = path.to_string();
			if (path_text == "00081110.0" &&
			    element.tag() == "ReferencedSOPInstanceUID"_tag) {
				return dicom::DataSetVisitControl::skip_current_dataset;
			}
			if (path_text == "00081110.0" && element.tag() == "SeriesInstanceUID"_tag) {
				visit_saw_item0_series = true;
			}
			if (path_text == "00081110.1" &&
			    element.tag() == "ReferencedSOPInstanceUID"_tag) {
				visit_saw_item1_referenced = true;
			}
			if (path_text.empty() && element.tag() == "SeriesInstanceUID"_tag) {
				visit_saw_top_level_series = true;
			}
			return dicom::DataSetVisitControl::continue_;
		});
		if (visit_saw_item0_series) {
			fail("DataSet::visit skip_current_dataset should prune the rest of the current nested dataset");
		}
		if (!visit_saw_item1_referenced || !visit_saw_top_level_series) {
			fail("DataSet::visit skip_current_dataset should continue with sibling items and parent elements");
		}

		const DataSet& const_dataset = dataset;
		std::size_t const_visit_count = 0;
		const_dataset.visit([&](auto path, const auto& element) {
			++const_visit_count;
			if (element.tag() == "ReferencedSOPInstanceUID"_tag &&
			    !path.contains_sequence("ReferencedStudySequence"_tag)) {
				fail("const DataSet::visit should preserve borrowed path helpers");
			}
		});
		if (const_visit_count != walked_tags.size()) {
			fail("const DataSet::visit should traverse the same loaded elements");
		}

		auto walker = dataset.walk();
		std::vector<std::uint32_t> pruned_tags;
		for (auto it = walker.begin(); it != walker.end(); ++it) {
			pruned_tags.push_back(it->element.tag().value());
			if (it->element.tag() == "ReferencedStudySequence"_tag) {
				if (!it->path.empty()) {
					fail("SQ walk entry path should be empty at the root dataset");
				}
				if (it->path.contains_sequence("ReferencedStudySequence"_tag)) {
					fail("SQ walk entry path should not include the current leaf sequence");
				}
				it->skip_sequence();
			}
		}
		if (std::find(
		        pruned_tags.begin(),
		        pruned_tags.end(),
		        "ReferencedSOPInstanceUID"_tag.value()) != pruned_tags.end()) {
			fail("DataSetWalkIterator::skip_sequence should prune the current SQ subtree");
		}
		if (std::find(pruned_tags.begin(), pruned_tags.end(), "SeriesInstanceUID"_tag.value()) ==
		    pruned_tags.end()) {
			fail("DataSetWalkIterator::skip_sequence should continue with later top-level elements");
		}

		auto equality_walker = dataset.walk();
		auto equality_it = equality_walker.begin();
		++equality_it;
		if (equality_it == equality_walker.end() ||
		    equality_it->element.tag() != "ReferencedStudySequence"_tag) {
			fail("DataSet walk equality smoke should reach the sequence element");
		}
		auto equality_copy = equality_it;
		equality_it.skip_sequence();
		if (!(equality_it == equality_copy)) {
			fail("DataSetWalkIterator equality should not depend on pending skip_sequence state");
		}
		equality_copy.skip_current_dataset();
		if (!(equality_it == equality_copy)) {
			fail("DataSetWalkIterator equality should not depend on pending skip_current_dataset state");
		}

		auto root_skip_walker = dataset.walk();
		std::vector<std::uint32_t> root_skip_tags;
		for (auto it = root_skip_walker.begin(); it != root_skip_walker.end(); ++it) {
			auto entry = *it;
			root_skip_tags.push_back(entry.element.tag().value());
			if (entry.element.tag() == "ReferencedStudySequence"_tag) {
				entry.skip_current_dataset();
			}
		}
		if (root_skip_tags.size() != 2 ||
		    root_skip_tags[0] != "SOPInstanceUID"_tag.value() ||
		    root_skip_tags[1] != "ReferencedStudySequence"_tag.value()) {
			fail("DataSetWalkIterator::skip_current_dataset should end the root dataset walk");
		}

		auto nested_skip_walker = dataset.walk();
		bool saw_item0_series = false;
		bool saw_item1_referenced = false;
		bool saw_top_level_series = false;
		for (auto it = nested_skip_walker.begin(); it != nested_skip_walker.end(); ++it) {
			auto entry = *it;
			const auto path = entry.path.to_string();
			if (path == "00081110.0" && entry.element.tag() == "ReferencedSOPInstanceUID"_tag) {
				entry.skip_current_dataset();
			}
			if (path == "00081110.0" && entry.element.tag() == "SeriesInstanceUID"_tag) {
				saw_item0_series = true;
			}
			if (path == "00081110.1" &&
			    entry.element.tag() == "ReferencedSOPInstanceUID"_tag) {
				saw_item1_referenced = true;
			}
			if (path.empty() && entry.element.tag() == "SeriesInstanceUID"_tag) {
				saw_top_level_series = true;
			}
		}
		if (saw_item0_series) {
			fail("DataSetWalkIterator::skip_current_dataset should prune the rest of the current nested dataset");
		}
		if (!saw_item1_referenced || !saw_top_level_series) {
			fail("DataSetWalkIterator::skip_current_dataset should continue with sibling items and parent elements");
		}

		DicomFile file;
		auto& root = file.dataset();
		if (!root.set_value("SOPInstanceUID"_tag, "1.2.840.10008.2")) {
			fail("DicomFile walk smoke should set top-level SOPInstanceUID");
		}
		auto* file_sequence =
		    root.ensure_dataelement("ReferencedStudySequence"_tag, dicom::VR::SQ).as_sequence();
		if (file_sequence == nullptr) {
			fail("DicomFile walk smoke should create sequence element");
		}
		auto* file_item = file_sequence->add_dataset();
		if (file_item == nullptr ||
		    !file_item->set_value("ReferencedSOPInstanceUID"_tag, "9.8.7")) {
			fail("DicomFile walk smoke should set nested referenced SOP UID");
		}

		std::size_t file_walk_count = 0;
		for (auto entry : file.walk()) {
			++file_walk_count;
			if (entry.element.tag() == "ReferencedSOPInstanceUID"_tag &&
			    entry.path.to_string() != std::string("00081110.0")) {
				fail("DicomFile::walk should forward dataset walk paths");
			}
		}
		if (file_walk_count != 3) {
			fail("DicomFile::walk should visit root and nested elements");
		}

		std::size_t file_visit_count = 0;
		file.visit([&](auto path, auto& element) {
			++file_visit_count;
			if (element.tag() == "ReferencedSOPInstanceUID"_tag &&
			    path.to_string() != std::string("00081110.0")) {
				fail("DicomFile::visit should forward dataset visit paths");
			}
		});
		if (file_visit_count != 3) {
			fail("DicomFile::visit should visit root and nested elements");
		}
	}

	const auto sop_uid = dicom::uid::generate_sop_instance_uid();
	if (!dicom::uid::is_valid_uid_text_strict(sop_uid.value())) {
		fail("generate_sop_instance_uid should return strict-valid UID text");
	}

	const auto base_uid = dicom::uid::make_generated("1.2.840.10008");
	if (!base_uid) fail("make_generated should build base UID");
	const auto composed_uid = base_uid->append(11u).append(22u).append(33u);
	if (composed_uid.value() != std::string_view("1.2.840.10008.11.22.33")) {
		fail("Generated::append should append all components");
	}
	const auto zero_component_uid = dicom::uid::make_generated("1.2.3");
	if (!zero_component_uid) fail("make_generated should build base UID with small root");
	if (zero_component_uid->append(7u).append(0u).value() != std::string_view("1.2.3.7.0")) {
		fail("Generated::append should treat 0 as valid component");
	}

	std::string long_base{"1"};
	while (long_base.size() + 2 <= 61) {
		long_base += ".1";
	}
	const auto long_base_uid = dicom::uid::make_generated(long_base);
	if (!long_base_uid) fail("make_generated should build long base UID");
	const auto compact_uid = long_base_uid->append(1234567890123456789ULL);
	if (compact_uid.value().size() > 64) fail("Generated::append compacted UID should be <= 64 chars");
	std::string truncated_prefix = long_base.substr(0, std::min<std::size_t>(30, long_base.size()));
	if (!truncated_prefix.empty() && truncated_prefix.back() != '.') {
		truncated_prefix.push_back('.');
	}
	if (compact_uid.value().rfind(truncated_prefix, 0) != 0) {
		fail("Generated::append compacted UID should keep 30-char base prefix");
	}
	if (!dicom::uid::is_valid_uid_text_strict(compact_uid.value())) {
		fail("Generated::append compacted UID should remain strict-valid");
	}

	const auto my_study_uid = dicom::uid::generate_uid();
	const auto series_uid = my_study_uid.append(23u);
	const auto instance_uid = series_uid.append(34u);
	if (!(series_uid.value().starts_with(my_study_uid.value()) &&
	        series_uid.value().ends_with(".23"))) {
		fail("Generated::append should append series component");
	}
	if (!(instance_uid.value().starts_with(series_uid.value()) &&
	        instance_uid.value().ends_with(".34"))) {
		fail("Generated::append should append instance component");
	}
	if (!dicom::uid::is_valid_uid_text_strict(instance_uid.value())) {
		fail("Generated::append chain should remain strict-valid");
	}

		{
			dicom::DataElement signed_long_elem("Rows"_tag, dicom::VR::SL, 0, 0, nullptr);
			if (!signed_long_elem.from_long(123456789L)) {
				fail("DataElement::from_long should encode SL");
			}
		if (signed_long_elem.length() != 4) {
			fail("DataElement::from_long SL length should be 4");
		}
			if (signed_long_elem.to_long().value_or(0) != 123456789L) {
				fail("DataElement::from_long SL roundtrip mismatch");
			}
		}
		{
			dicom::DataElement signed_int_elem("Rows"_tag, dicom::VR::SL, 0, 0, nullptr);
			if (!signed_int_elem.from_int(12345)) {
				fail("DataElement::from_int should encode SL");
			}
			if (signed_int_elem.to_int().value_or(0) != 12345) {
				fail("DataElement::from_int SL roundtrip mismatch");
			}
		}
		{
			dicom::DataElement inline_elem("Rows"_tag, dicom::VR::OB, 0, 0, nullptr);
			inline_elem.reserve_value_bytes(3);
		if (inline_elem.length() != 3) {
			fail("DataElement::reserve_value_bytes should update length");
		}
		if (inline_elem.storage_kind() != dicom::DataElement::StorageKind::inline_bytes) {
			fail("DataElement::reserve_value_bytes should use inline storage for small values");
		}
		if (inline_elem.value_span().size() != 3) {
			fail("DataElement::reserve_value_bytes should expose reserved span size");
		}

		dicom::DataElement heap_elem("Rows"_tag, dicom::VR::OB, 0, 0, nullptr);
		heap_elem.reserve_value_bytes(dicom::DataElement::kInlineStorageBytes + 1);
		if (heap_elem.storage_kind() != dicom::DataElement::StorageKind::heap) {
			fail("DataElement::reserve_value_bytes should use heap storage for large values");
		}
		auto first_heap_ptr = heap_elem.value_span().data();
		heap_elem.reserve_value_bytes(dicom::DataElement::kInlineStorageBytes + 1);
			if (heap_elem.value_span().data() != first_heap_ptr) {
				fail("DataElement::reserve_value_bytes should reuse heap when capacity is sufficient");
			}
		}
		{
			dicom::DataElement bytes_elem("Rows"_tag, dicom::VR::OB, 0, 0, nullptr);
			const std::array<std::uint8_t, 8> raw{
			    0x01u, 0x00u, 0xFFu, 0x7Fu, 0x78u, 0x56u, 0x34u, 0x12u};
			bytes_elem.set_value_bytes(raw);
			auto bytes = bytes_elem.value_span();
			if (bytes.size() != raw.size()) {
				fail("DataElement::value_span should expose raw bytes");
			}
			for (std::size_t i = 0; i < raw.size(); ++i) {
				if (bytes[i] != raw[i]) {
					fail("DataElement::value_span value mismatch");
				}
			}
		}
		{
			dicom::DataElement moved_bytes_elem("Rows"_tag, dicom::VR::OB, 0, 0, nullptr);
			std::vector<std::uint8_t> raw(1024);
			for (std::size_t i = 0; i < raw.size(); ++i) {
				raw[i] = static_cast<std::uint8_t>(i & 0xFFu);
			}
			moved_bytes_elem.set_value_bytes(std::move(raw));
			if (moved_bytes_elem.storage_kind() != dicom::DataElement::StorageKind::owned_bytes) {
				fail("DataElement::set_value_bytes(move) should adopt vector storage");
			}
			const auto bytes = moved_bytes_elem.value_span();
			if (bytes.size() != 1024) {
				fail("DataElement::set_value_bytes(move) size mismatch");
			}
			if (bytes[0] != 0x00u || bytes[1] != 0x01u || bytes[255] != 0xFFu) {
				fail("DataElement::set_value_bytes(move) value mismatch");
			}
		}
		{
			DataSet ds;
			auto& inserted = ds.add_dataelement("Rows"_tag, dicom::VR::US);
			if (inserted.storage_kind() != dicom::DataElement::StorageKind::none) {
				fail("DataSet::add_dataelement(tag, vr) should keep storage_kind none");
			}
			auto& replaced = ds.add_dataelement("Rows"_tag, dicom::VR::UL);
			if (&replaced != &inserted || replaced.vr() != dicom::VR::UL ||
			    replaced.storage_kind() != dicom::DataElement::StorageKind::none ||
			    replaced.length() != 0) {
				fail("DataSet::add_dataelement(tag, vr) should replace an existing element in place");
			}
			auto& preserved_vr = ds.add_dataelement("Rows"_tag);
			if (&preserved_vr != &inserted || preserved_vr.vr() != dicom::VR::UL ||
			    preserved_vr.storage_kind() != dicom::DataElement::StorageKind::none) {
				fail("DataSet::add_dataelement(tag) should preserve the existing VR when omitted");
			}
		}
		{
			DataSet ds;
			auto& inserted = ds.add_dataelement("Rows"_tag, dicom::VR::US);
			auto& preserved = ds.ensure_dataelement("Rows"_tag);
			if (&preserved != &inserted ||
			    preserved.storage_kind() != dicom::DataElement::StorageKind::none) {
				fail("DataSet::ensure_dataelement(tag) should preserve an existing element");
			}

			auto& overridden = ds.ensure_dataelement("Rows"_tag, dicom::VR::UL);
			if (&overridden != &inserted || overridden.vr() != dicom::VR::UL ||
			    overridden.storage_kind() != dicom::DataElement::StorageKind::none ||
			    overridden.length() != 0) {
				fail("DataSet::ensure_dataelement(tag, vr) should reset an existing element to the requested VR");
			}

			auto& inserted_missing = ds.ensure_dataelement("Columns"_tag);
			if (!inserted_missing || inserted_missing.vr() != dicom::VR::US ||
			    inserted_missing.storage_kind() != dicom::DataElement::StorageKind::none) {
				fail("DataSet::ensure_dataelement(tag) should insert missing standard tags using dictionary VR");
			}

			try {
				(void)ds.ensure_dataelement(dicom::Tag(0x0009, 0x1030));
				fail("DataSet::ensure_dataelement(private tag) without VR should throw");
			} catch (const std::exception& ex) {
				const std::string what = ex.what();
				if (what.find("&dicom::DataSet::ensure_dataelement") != std::string::npos) {
					fail("DataSet::ensure_dataelement error prefix should not include a leading '&'");
				}
				if (what.find("ensure_dataelement") == std::string::npos) {
					fail("DataSet::ensure_dataelement error prefix should include the operation name");
				}
			}
		}
		{
			DataSet ds;
			auto& ensured = ds.ensure_dataelement(
			    "ReferencedStudySequence.0.ReferencedSOPInstanceUID", dicom::VR::UI);
			if (!ensured || ensured.vr() != dicom::VR::UI) {
				fail("DataSet::ensure_dataelement(tag_path, vr) should create nested sequence parents");
			}
			if (!ds.set_value(
			        "ReferencedStudySequence.0.ReferencedSOPInstanceUID",
			        std::string_view("1.2.3.4"))) {
				fail("DataSet::set_value(tag_path, value) should assign nested leaf elements");
			}
			const auto nested_uid = ds.get_value<std::string>(
			    "ReferencedStudySequence.0.ReferencedSOPInstanceUID");
			if (!nested_uid || *nested_uid != "1.2.3.4") {
				fail("DataSet::get_value(tag_path) should read nested leaf elements after assignment");
			}
			const auto nested_uid_via_index =
			    ds["ReferencedStudySequence.0.ReferencedSOPInstanceUID"].to_string_view();
			if (!nested_uid_via_index || *nested_uid_via_index != std::string_view("1.2.3.4")) {
				fail("DataSet::operator[](tag_path) should read nested leaf elements");
			}
			auto& reset_leaf = ds.add_dataelement(
			    "ReferencedStudySequence.0.ReferencedSOPInstanceUID", dicom::VR::LO);
			if (&reset_leaf != &ensured || reset_leaf.vr() != dicom::VR::LO) {
				fail("DataSet::add_dataelement(tag_path, vr) should reset the nested leaf in place");
			}

			ds.add_dataelement("Rows"_tag, dicom::VR::US);
			auto& nested_from_overwrite =
			    ds.ensure_dataelement("Rows.0.Columns", dicom::VR::US);
			if (nested_from_overwrite.vr() != dicom::VR::US) {
				fail("Nested ensure_dataelement should create the requested leaf VR");
			}
			const auto& rows_element = ds.get_dataelement("Rows"_tag);
			const auto* rows_seq = rows_element.as_sequence();
			if (!rows_seq || rows_element.vr() != dicom::VR::SQ) {
				fail("Nested ensure_dataelement should reset a non-sequence intermediate element to SQ");
			}
			if (rows_seq->size() != 1) {
				fail("Nested ensure_dataelement should create the first missing sequence item");
			}
			const auto* row_item = rows_seq->get_dataset(0);
			if (!row_item) {
				fail("Nested ensure_dataelement should materialize the requested sequence item");
			}
			const auto& nested_columns = row_item->get_dataelement("Columns"_tag);
			if (nested_columns.is_missing() || nested_columns.vr() != dicom::VR::US) {
				fail("Nested ensure_dataelement should create the requested leaf inside the new item");
			}
			if (ds["Rows.0.Columns"].vr() != dicom::VR::US) {
				fail("DataSet::operator[](tag_path) should resolve nested keyword paths");
			}
		}
		{
			const auto fixture_dir = std::filesystem::path(__FILE__).parent_path();
			const auto fixture_path = fixture_dir / "test_le.dcm";
			dicom::ReadOptions opts{};
			opts.load_until = "StudyTime"_tag;

			auto partial_read = read_file(fixture_path, opts);
			if (!partial_read) {
				fail("partial read_file should succeed");
			}
			if (partial_read->get_value<long>("Rows"_tag).has_value()) {
				fail("get_value should not implicitly continue partial loading");
			}
			const auto& partial_read_const = *partial_read;
			partial_read_const.ensure_loaded("Rows"_tag);
			if (!partial_read_const.get_value<long>("Rows"_tag).has_value()) {
				fail("DicomFile::ensure_loaded should forward to the root dataset frontier");
			}

			auto partial_set = read_file(fixture_path, opts);
			if (!partial_set) {
				fail("partial read_file for set_value should succeed");
			}
			bool set_rows_threw = false;
			try {
				(void)partial_set->set_value("Rows"_tag, 1024L);
			} catch (const std::exception&) {
				set_rows_threw = true;
			}
			if (!set_rows_threw) {
				fail("set_value should throw beyond the current load frontier");
			}
			bool set_repr_threw = false;
			try {
				(void)partial_set->set_value("PixelRepresentation"_tag, 0L);
			} catch (const std::exception&) {
				set_repr_threw = true;
			}
			if (!set_repr_threw) {
				fail("set_value should throw for later root tags beyond the current load frontier");
			}

			auto partial_add = read_file(fixture_path, opts);
			bool add_threw = false;
			try {
				(void)partial_add->add_dataelement("Rows"_tag, dicom::VR::US);
			} catch (const std::exception&) {
				add_threw = true;
			}
			if (!add_threw) {
				fail("add_dataelement should throw beyond the current load frontier");
			}

			auto partial_ensure = read_file(fixture_path, opts);
			bool ensure_threw = false;
			try {
				(void)partial_ensure->ensure_dataelement("Rows"_tag, dicom::VR::US);
			} catch (const std::exception&) {
				ensure_threw = true;
			}
			if (!ensure_threw) {
				fail("ensure_dataelement should throw beyond the current load frontier");
			}
		}
		{
			DataSet ds;
			auto& rows = ds.add_dataelement("Rows"_tag, dicom::VR::US);
			if (!rows.from_long(512)) {
				fail("DataSet::add_dataelement should allow assigning a standard element");
			}
			auto& columns = ds.add_dataelement("Columns"_tag, dicom::VR::US);
			if (!columns.from_long(256)) {
				fail("DataSet::add_dataelement should allow assigning a second standard element");
			}
			auto& private_mid = ds.add_dataelement(dicom::Tag(0x0009, 0x1030), dicom::VR::US);
			if (!private_mid.from_long(16)) {
				fail("DataSet::add_dataelement should allow assigning an out-of-order private element");
			}
			auto& private_late =
			    ds.add_dataelement(dicom::Tag(0x0009, 0x1031), dicom::VR::LO);
			if (private_late.storage_kind() != dicom::DataElement::StorageKind::none) {
				fail("DataSet::add_dataelement should keep public inserts unbound from stream storage");
			}
			const std::vector<std::uint32_t> expected_tags = {
			    dicom::Tag(0x0009, 0x1030).value(),
			    dicom::Tag(0x0009, 0x1031).value(),
			    "Rows"_tag.value(),
			    "Columns"_tag.value(),
			};
			std::vector<std::uint32_t> iterated_tags;
			for (const auto& element : ds) {
				iterated_tags.push_back(element.tag().value());
			}
			if (iterated_tags != expected_tags) {
				fail("DataSet iteration should remain sorted after out-of-order inserts");
			}
			ds.remove_dataelement(dicom::Tag(0x0009, 0x1030));
			if (ds.get_dataelement(dicom::Tag(0x0009, 0x1030))) {
				fail("DataSet::remove_dataelement should erase out-of-order private elements");
			}
			if (ds.get_dataelement(dicom::Tag(0x0009, 0x1031)).length() != 0) {
				fail("Removing one map-backed element should not disturb neighboring map-backed elements");
			}
		}
		{
			DataSet ds;
			if (!ds.set_value("Rows"_tag, 512L)) {
				fail("DataSet::set_value should encode scalar long");
			}
			auto rows = ds.get_value<long>("Rows"_tag);
			if (!rows || *rows != 512L) {
				fail("DataSet::get_value<long> should decode scalar US");
			}
			if (ds.get_value<long>("Columns"_tag, -1L) != -1L) {
				fail("DataSet::get_value<long>(default) should use fallback for missing tag");
			}

			const std::array<double, 2> window_centers{40.5, 80.25};
			if (!ds.set_value("WindowCenter"_tag, std::span<const double>(window_centers))) {
				fail("DataSet::set_value should encode vector<double>");
			}
			auto decoded_window_centers = ds.get_value<std::vector<double>>("WindowCenter"_tag);
			if (!decoded_window_centers ||
			    *decoded_window_centers != std::vector<double>{40.5, 80.25}) {
				fail("DataSet::get_value<vector<double>> should decode DS values");
			}

			if (!ds.set_value("PatientName", std::string_view("DOE^JOHN"))) {
				fail("DataSet::set_value should accept keyword string keys");
			}
			auto patient_name_view = ds.get_value<std::string_view>("PatientName");
			if (!patient_name_view || *patient_name_view != std::string_view("DOE^JOHN")) {
				fail("DataSet::get_value<string_view> should expose zero-copy string access");
			}
			auto patient_name = ds.get_value<std::string>("PatientName");
			if (!patient_name || *patient_name != "DOE^JOHN") {
				fail("DataSet::get_value<string> should decode keyword string lookups");
			}
			const std::array<std::string_view, 2> image_type_values{"ORIGINAL", "PRIMARY"};
			if (!ds.set_value("ImageType"_tag, std::span<const std::string_view>(image_type_values))) {
				fail("DataSet::set_value should encode string_view vectors");
			}
			auto image_type_views = ds.get_value<std::vector<std::string_view>>("ImageType"_tag);
			if (!image_type_views ||
			    *image_type_views != std::vector<std::string_view>{"ORIGINAL", "PRIMARY"}) {
				fail("DataSet::get_value<vector<string_view>> should expose zero-copy multi-value access");
			}

			const dicom::Tag private_tag(0x0009, 0x0030);
			if (!ds.set_value(private_tag, dicom::VR::US, 16LL)) {
				fail("DataSet::set_value(tag, vr, value) should create private elements");
			}
			if (ds.get_dataelement(private_tag).vr() != dicom::VR::US ||
			    ds.get_value<long long>(private_tag).value_or(0) != 16LL) {
				fail("DataSet explicit-VR private assignment mismatch");
			}
			if (!ds.set_value(private_tag, dicom::VR::UL, 17LL)) {
				fail("DataSet::set_value(tag, vr, value) should override non-SQ/non-PX VR");
			}
			if (ds.get_dataelement(private_tag).vr() != dicom::VR::UL ||
			    ds.get_value<long long>(private_tag).value_or(0) != 17LL) {
				fail("DataSet explicit-VR override mismatch");
			}
			if (ds.set_value(private_tag, dicom::VR::SQ, 18LL)) {
				fail("DataSet::set_value(tag, vr, value) should fail when assigning a scalar to SQ");
			}
			if (ds.get_dataelement(private_tag).vr() != dicom::VR::SQ ||
			    ds.get_dataelement(private_tag).length() != 0) {
				fail("DataSet explicit-VR SQ failure should leave the target reset to SQ");
			}

			const std::array<long long, 0> empty_rows{};
			if (!ds.set_value("Rows"_tag, std::span<const long long>(empty_rows))) {
				fail("DataSet::set_value should encode zero-length integer vectors");
			}
			auto decoded_empty_rows = ds.get_value<std::vector<long long>>("Rows"_tag);
			if (!decoded_empty_rows || !decoded_empty_rows->empty()) {
				fail("DataSet::get_value<vector<long long>> should preserve zero-length values");
			}
			if (ds.set_value("Rows"_tag, -1L)) {
				fail("DataSet::set_value should report false for out-of-range US assignment");
			}
			if (!ds.set_value("Columns"_tag, 128L) ||
			    ds.get_value<long>("Columns"_tag).value_or(0L) != 128L) {
				fail("DataSet should remain usable after a failed write");
			}
		}
		{
			DicomFile df;
			if (!df.set_value("Rows"_tag, 1024L)) {
				fail("DicomFile::set_value should forward to the root dataset");
			}
			auto& ensured = df.ensure_dataelement("Columns"_tag);
			if (!ensured || ensured.vr() != dicom::VR::US) {
				fail("DicomFile::ensure_dataelement should forward to the root dataset");
			}
			auto rows = df.get_value<long>("Rows"_tag);
			if (!rows || *rows != 1024L) {
				fail("DicomFile::get_value<long> should forward to the root dataset");
			}
			if (df["Rows"].to_long().value_or(0) != 1024L) {
				fail("DicomFile::operator[](tag_path) should forward string lookups to the root dataset");
			}
			if (df["Columns"].vr() != dicom::VR::US) {
				fail("DicomFile::operator[](tag_path) should resolve keyword strings");
			}
		}
		{
			dicom::DataElement padded_move_elem("Rows"_tag, dicom::VR::OB, 0, 0, nullptr);
			std::vector<std::uint8_t> odd_bytes{0x11u, 0x22u, 0x33u};
			padded_move_elem.adopt_value_bytes(std::move(odd_bytes));
			const auto bytes = padded_move_elem.value_span();
			if (bytes.size() != 4) {
				fail("DataElement::adopt_value_bytes should pad odd-length values");
			}
			if (bytes[0] != 0x11u || bytes[1] != 0x22u || bytes[2] != 0x33u) {
				fail("DataElement::adopt_value_bytes should preserve original payload");
			}
		}
		{
			dicom::DataElement unsigned_short_elem("Rows"_tag, dicom::VR::US, 0, 0, nullptr);
			if (unsigned_short_elem.from_long(-1)) {
				fail("DataElement::from_long should reject negative value for US");
		}
	}
	{
		dicom::DataElement integer_string_elem("Rows"_tag, dicom::VR::IS, 0, 0, nullptr);
		if (!integer_string_elem.from_long(42L)) {
			fail("DataElement::from_long should encode IS");
		}
		const auto text = integer_string_elem.to_string_view();
		if (!text || *text != "42") {
			fail("DataElement::from_long IS string mismatch");
		}
	}
		{
			dicom::DataElement vector_elem("Rows"_tag, dicom::VR::US, 0, 0, nullptr);
			const std::array<long, 3> values{1, 2, 3};
			if (!vector_elem.from_long_vector(values)) {
				fail("DataElement::from_long_vector should encode US");
		}
		auto decoded = vector_elem.to_long_vector();
		if (!decoded || *decoded != std::vector<long>{1, 2, 3}) {
			fail("DataElement::from_long_vector US roundtrip mismatch");
		}
		const std::array<long, 2> invalid_values{-1, 2};
			if (vector_elem.from_long_vector(invalid_values)) {
				fail("DataElement::from_long_vector should reject out-of-range value for US");
			}
		}
		{
			dicom::DataElement vector_elem("Rows"_tag, dicom::VR::US, 0, 0, nullptr);
			const std::array<int, 3> values{1, 2, 3};
			if (!vector_elem.from_int_vector(values)) {
				fail("DataElement::from_int_vector should encode US");
			}
			auto decoded = vector_elem.to_int_vector();
			if (!decoded || *decoded != std::vector<int>{1, 2, 3}) {
				fail("DataElement::from_int_vector US roundtrip mismatch");
			}
			const std::array<int, 2> invalid_values{-1, 2};
			if (vector_elem.from_int_vector(invalid_values)) {
				fail("DataElement::from_int_vector should reject out-of-range value for US");
			}
		}
		{
			dicom::DataElement signed_very_long_elem("Rows"_tag, dicom::VR::SV, 0, 0, nullptr);
			constexpr long long kValue = 4294967296LL;
			if (!signed_very_long_elem.from_longlong(kValue)) {
				fail("DataElement::from_longlong should encode SV");
			}
			if (signed_very_long_elem.to_longlong().value_or(0) != kValue) {
				fail("DataElement::from_longlong SV roundtrip mismatch");
			}
			dicom::DataElement signed_long_elem("Rows"_tag, dicom::VR::SL, 0, 0, nullptr);
			if (signed_long_elem.from_longlong(kValue)) {
				fail("DataElement::from_longlong should reject out-of-range value for SL");
			}
		}
		{
			dicom::DataElement vector_elem("Rows"_tag, dicom::VR::SV, 0, 0, nullptr);
			const std::array<long long, 2> values{4294967296LL, 7LL};
			if (!vector_elem.from_longlong_vector(values)) {
				fail("DataElement::from_longlong_vector should encode SV");
			}
			auto decoded = vector_elem.to_longlong_vector();
			if (!decoded || *decoded != std::vector<long long>{4294967296LL, 7LL}) {
				fail("DataElement::from_longlong_vector SV roundtrip mismatch");
			}
			dicom::DataElement narrow_elem("Rows"_tag, dicom::VR::SL, 0, 0, nullptr);
			if (narrow_elem.from_longlong_vector(values)) {
				fail("DataElement::from_longlong_vector should reject out-of-range value for SL");
			}

			dicom::DataElement empty_vector_elem("Rows"_tag, dicom::VR::US, 0, 0, nullptr);
			const std::array<long long, 0> empty_values{};
			if (!empty_vector_elem.from_longlong_vector(empty_values)) {
				fail("DataElement::from_longlong_vector should encode zero-length US");
			}
			auto empty_ints = empty_vector_elem.to_int_vector();
			if (!empty_ints || !empty_ints->empty()) {
				fail("DataElement::to_int_vector should return empty vector for zero-length US");
			}
			auto empty_longs = empty_vector_elem.to_long_vector();
			if (!empty_longs || !empty_longs->empty()) {
				fail("DataElement::to_long_vector should return empty vector for zero-length US");
			}
			auto empty_longlongs = empty_vector_elem.to_longlong_vector();
			if (!empty_longlongs || !empty_longlongs->empty()) {
				fail("DataElement::to_longlong_vector should return empty vector for zero-length US");
			}
		}
		{
			dicom::DataElement fd_elem("SliceThickness"_tag, dicom::VR::FD, 0, 0, nullptr);
			if (!fd_elem.from_double(12.5)) {
				fail("DataElement::from_double should encode FD");
			}
			if (fd_elem.to_double().value_or(0.0) != 12.5) {
				fail("DataElement::from_double FD roundtrip mismatch");
			}

			dicom::DataElement fl_elem("SliceThickness"_tag, dicom::VR::FL, 0, 0, nullptr);
			if (!fl_elem.from_double(3.25)) {
				fail("DataElement::from_double should encode FL");
			}
			if (fl_elem.to_double().value_or(0.0) != 3.25) {
				fail("DataElement::from_double FL roundtrip mismatch");
			}

			dicom::DataElement ds_elem("SliceThickness"_tag, dicom::VR::DS, 0, 0, nullptr);
			if (!ds_elem.from_double(1.5)) {
				fail("DataElement::from_double should encode DS");
			}
			if (ds_elem.to_double().value_or(0.0) != 1.5) {
				fail("DataElement::from_double DS roundtrip mismatch");
			}

			dicom::DataElement unsupported_elem("Rows"_tag, dicom::VR::US, 0, 0, nullptr);
			if (unsupported_elem.from_double(1.0)) {
				fail("DataElement::from_double should reject unsupported VR");
			}
		}
		{
			dicom::DataElement fd_vec_elem("SliceThickness"_tag, dicom::VR::FD, 0, 0, nullptr);
			const std::array<double, 3> values{1.5, 2.25, 3.75};
			if (!fd_vec_elem.from_double_vector(values)) {
				fail("DataElement::from_double_vector should encode FD");
			}
			auto decoded = fd_vec_elem.to_double_vector();
			if (!decoded || *decoded != std::vector<double>{1.5, 2.25, 3.75}) {
				fail("DataElement::from_double_vector FD roundtrip mismatch");
			}

			dicom::DataElement ds_vec_elem("SliceThickness"_tag, dicom::VR::DS, 0, 0, nullptr);
			if (!ds_vec_elem.from_double_vector(values)) {
				fail("DataElement::from_double_vector should encode DS");
			}
			auto ds_decoded = ds_vec_elem.to_double_vector();
			if (!ds_decoded || *ds_decoded != std::vector<double>{1.5, 2.25, 3.75}) {
				fail("DataElement::from_double_vector DS roundtrip mismatch");
			}

			dicom::DataElement empty_ds_elem("SliceThickness"_tag, dicom::VR::DS, 0, 0, nullptr);
			const std::array<double, 0> empty_values{};
			if (!empty_ds_elem.from_double_vector(empty_values)) {
				fail("DataElement::from_double_vector should encode zero-length DS");
			}
			auto empty_ds_decoded = empty_ds_elem.to_double_vector();
			if (!empty_ds_decoded || !empty_ds_decoded->empty()) {
				fail("DataElement::to_double_vector should return empty vector for zero-length DS");
			}
		}
		{
			const dicom::Tag offending_tag(0x0000, 0x0901);
			dicom::DataElement tag_elem(offending_tag, dicom::VR::AT, 0, 0, nullptr);
			const dicom::Tag expected(0x0010, 0x0020);
			if (!tag_elem.from_tag(expected)) {
				fail("DataElement::from_tag should encode AT");
			}
			if (tag_elem.to_tag().value_or(dicom::Tag()) != expected) {
				fail("DataElement::from_tag AT roundtrip mismatch");
			}

			dicom::DataElement tag_vec_elem(offending_tag, dicom::VR::AT, 0, 0, nullptr);
			const std::array<dicom::Tag, 3> tags{
			    dicom::Tag(0x0010, 0x0010),
			    dicom::Tag(0x0010, 0x0020),
			    dicom::Tag(0x0008, 0x0018)};
			if (!tag_vec_elem.from_tag_vector(tags)) {
				fail("DataElement::from_tag_vector should encode AT");
			}
			auto decoded = tag_vec_elem.to_tag_vector();
			if (!decoded ||
			    *decoded != std::vector<dicom::Tag>{dicom::Tag(0x0010, 0x0010),
			        dicom::Tag(0x0010, 0x0020), dicom::Tag(0x0008, 0x0018)}) {
				fail("DataElement::from_tag_vector AT roundtrip mismatch");
			}

			dicom::DataElement empty_tag_vec_elem(offending_tag, dicom::VR::AT, 0, 0, nullptr);
			const std::array<dicom::Tag, 0> empty_tags{};
			if (!empty_tag_vec_elem.from_tag_vector(empty_tags)) {
				fail("DataElement::from_tag_vector should encode zero-length AT");
			}
			auto empty_decoded = empty_tag_vec_elem.to_tag_vector();
			if (!empty_decoded || !empty_decoded->empty()) {
				fail("DataElement::to_tag_vector should return empty vector for zero-length AT");
			}

			dicom::DataElement unsupported_elem("Rows"_tag, dicom::VR::US, 0, 0, nullptr);
			if (unsupported_elem.from_tag(expected)) {
				fail("DataElement::from_tag should reject non-AT VR");
			}
		}
		{
			dicom::DataElement patient_name("PatientName"_tag, dicom::VR::PN, 0, 0, nullptr);
			if (!patient_name.from_string_view("DOE^JOHN")) {
				fail("DataElement::from_string_view should encode PN");
			}
			if ((patient_name.length() & 1u) != 0u) {
				fail("DataElement::from_string_view should store even-length value");
			}
			const auto text = patient_name.to_string_view();
			if (!text || *text != "DOE^JOHN") {
				fail("DataElement::from_string_view PN roundtrip mismatch");
			}
			if (!patient_name.from_string_view("A")) {
				fail("DataElement::from_string_view should encode odd-length PN");
			}
			if (patient_name.length() != 2) {
				fail("DataElement::from_string_view should pad odd-length PN to even");
			}
		}
		{
			dicom::DataElement patient_names("PatientName"_tag, dicom::VR::PN, 0, 0, nullptr);
			const std::array<std::string_view, 2> names{"DOE^JOHN", "SMITH^ALICE"};
			if (!patient_names.from_string_views(names)) {
				fail("DataElement::from_string_views should encode PN");
			}
			auto decoded = patient_names.to_string_views();
			if (!decoded ||
			    *decoded != std::vector<std::string_view>{"DOE^JOHN", "SMITH^ALICE"}) {
				fail("DataElement::from_string_views PN roundtrip mismatch");
			}

			dicom::DataElement ui_values("SOPInstanceUID"_tag, dicom::VR::UI, 0, 0, nullptr);
			const std::array<std::string_view, 2> uids{"1.2.3", "1.2.840.10008.1.2"};
			if (!ui_values.from_string_views(uids)) {
				fail("DataElement::from_string_views should encode multi-value UI");
			}
			auto ui_decoded = ui_values.to_string_views();
			if (!ui_decoded ||
			    *ui_decoded != std::vector<std::string_view>{"1.2.3", "1.2.840.10008.1.2"}) {
				fail("DataElement::from_string_views UI roundtrip mismatch");
			}

			dicom::DataElement url_elem("RetrieveURL"_tag, dicom::VR::UR, 0, 0, nullptr);
			const std::array<std::string_view, 2> urls{"https://a", "https://b"};
			if (url_elem.from_string_views(urls)) {
				fail("DataElement::from_string_views should reject multi-value UR");
			}
		}
		{
			const std::string utf8_name("\xED\x99\x8D\xEA\xB8\xB8\xEB\x8F\x99", 9);
			dicom::DicomFile utf8_file;
			utf8_file.set_declared_specific_charset(dicom::SpecificCharacterSet::ISO_IR_192);
			auto& utf8_patient_name = utf8_file.add_dataelement("PatientName"_tag, dicom::VR::PN);
			if (!utf8_patient_name.from_utf8_view(utf8_name)) {
				fail("DataElement::from_utf8_view should encode UTF-8 text using current charset");
			}
			if (utf8_patient_name.value_span().empty()) {
				fail("from_utf8_view should materialize raw bytes immediately");
			}
			const auto utf8_owned = utf8_patient_name.to_utf8_string();
			if (!utf8_owned || *utf8_owned != utf8_name) {
				fail("DataElement::to_utf8_string should return owned UTF-8 text");
			}
			auto raw_utf8 = utf8_patient_name.to_string_view();
			if (!raw_utf8 || *raw_utf8 != utf8_name) {
				fail("UTF-8 raw storage should remain accessible through to_string_view");
			}

			auto& utf8_patient_names = utf8_file.add_dataelement("OtherPatientNames"_tag, dicom::VR::PN);
			const std::array<std::string_view, 2> utf8_names{utf8_name, "DOE^JOHN"};
			if (!utf8_patient_names.from_utf8_views(utf8_names)) {
				fail("DataElement::from_utf8_views should encode multi-value UTF-8 text");
			}
			const auto utf8_owned_values = utf8_patient_names.to_utf8_strings();
			if (!utf8_owned_values || utf8_owned_values->size() != 2 ||
			    (*utf8_owned_values)[0] != utf8_name || (*utf8_owned_values)[1] != "DOE^JOHN") {
				fail("DataElement::to_utf8_strings should return owned UTF-8 values");
			}
		}
		{
			auto ts_uid = dicom::uid::from_keyword("ImplicitVRLittleEndian");
			if (!ts_uid) {
				fail("uid::from_keyword should resolve transfer syntax UID");
			}
			dicom::DataElement ts_elem("TransferSyntaxUID"_tag, dicom::VR::UI, 0, 0, nullptr);
			if (!ts_elem.from_uid(*ts_uid)) {
				fail("DataElement::from_uid should encode well-known UID");
			}
			if (!ts_elem.from_transfer_syntax_uid(*ts_uid)) {
				fail("DataElement::from_transfer_syntax_uid should encode transfer syntax UID");
			}
			auto roundtrip = ts_elem.to_transfer_syntax_uid();
			if (!roundtrip || roundtrip->value() != ts_uid->value()) {
				fail("DataElement::from_transfer_syntax_uid roundtrip mismatch");
			}
			auto generated = dicom::uid::generate_uid();
			if (!ts_elem.from_uid(generated)) {
				fail("DataElement::from_uid(Generated) should encode generated UID");
			}
			auto generated_roundtrip = ts_elem.to_uid_string();
			if (!generated_roundtrip || *generated_roundtrip != std::string(generated.value())) {
				fail("DataElement::from_uid(Generated) roundtrip mismatch");
			}
		}
		{
			const auto implicit_le = dicom::uid::from_keyword("ImplicitVRLittleEndian");
			const auto deflated_le = dicom::uid::from_keyword("DeflatedExplicitVRLittleEndian");
			const auto encapsulated_uncompressed =
			    dicom::uid::from_keyword("EncapsulatedUncompressedExplicitVRLittleEndian");
			const auto jpeg_baseline = dicom::uid::from_keyword("JPEGBaseline8Bit");
			const auto jpeg_lossless_sv1 = dicom::uid::from_keyword("JPEGLosslessSV1");
			const auto jpegls_lossless = dicom::uid::from_keyword("JPEGLSLossless");
			const auto jpeg2000_lossless = dicom::uid::from_keyword("JPEG2000Lossless");
			const auto jpeg2000_lossy = dicom::uid::from_keyword("JPEG2000");
			const auto htj2k_lossless = dicom::uid::from_keyword("HTJ2KLossless");
			const auto jpegxl = dicom::uid::from_keyword("JPEGXL");
			const auto rle_lossless = dicom::uid::from_keyword("RLELossless");
			if (!implicit_le || !deflated_le || !encapsulated_uncompressed || !jpeg_baseline ||
			    !jpeg_lossless_sv1 || !jpegls_lossless || !jpeg2000_lossless ||
			    !jpeg2000_lossy || !htj2k_lossless || !jpegxl || !rle_lossless) {
				fail("uid::from_keyword should resolve transfer syntax classification fixtures");
			}

			if (!implicit_le->is_uncompressed() || implicit_le->is_encapsulated()) {
				fail("ImplicitVRLittleEndian classification mismatch");
			}
			if (!implicit_le->is_lossless() || implicit_le->is_lossy() ||
			    !implicit_le->supports_pixel_encode() || !implicit_le->supports_pixel_decode()) {
				fail("ImplicitVRLittleEndian capability mismatch");
			}
			if (!deflated_le->is_uncompressed() || deflated_le->is_encapsulated()) {
				fail("DeflatedExplicitVRLittleEndian classification mismatch");
			}
			if (!encapsulated_uncompressed->is_uncompressed() ||
			    !encapsulated_uncompressed->is_encapsulated()) {
				fail("EncapsulatedUncompressedExplicitVRLittleEndian classification mismatch");
			}
			if (!encapsulated_uncompressed->is_lossless() || encapsulated_uncompressed->is_lossy() ||
			    !encapsulated_uncompressed->supports_pixel_encode() ||
			    !encapsulated_uncompressed->supports_pixel_decode()) {
				fail("EncapsulatedUncompressedExplicitVRLittleEndian capability mismatch");
			}
			if (jpeg_baseline->is_uncompressed() || !jpeg_baseline->is_encapsulated()) {
				fail("JPEGBaseline8Bit classification mismatch");
			}
			if (jpeg_baseline->is_lossless() || !jpeg_baseline->is_lossy() ||
			    !jpeg_baseline->supports_pixel_encode() ||
			    !jpeg_baseline->supports_pixel_decode()) {
				fail("JPEGBaseline8Bit capability mismatch");
			}
			if (!jpeg_lossless_sv1->is_jpeg_lossless() || !jpeg_lossless_sv1->is_lossless() ||
			    jpeg_lossless_sv1->is_lossy() || !jpeg_lossless_sv1->supports_pixel_encode() ||
			    !jpeg_lossless_sv1->supports_pixel_decode()) {
				fail("JPEGLosslessSV1 capability mismatch");
			}
			if (!jpegls_lossless->is_jpegls() || !jpegls_lossless->is_lossless() ||
			    jpegls_lossless->is_lossy() || !jpegls_lossless->supports_pixel_encode() ||
			    !jpegls_lossless->supports_pixel_decode()) {
				fail("JPEGLSLossless capability mismatch");
			}
			if (!jpeg2000_lossless->is_jpeg2000() || jpeg2000_lossless->is_htj2k() ||
			    !jpeg2000_lossless->is_lossless() || jpeg2000_lossless->is_lossy() ||
			    !jpeg2000_lossless->supports_pixel_encode() ||
			    !jpeg2000_lossless->supports_pixel_decode()) {
				fail("JPEG2000Lossless capability mismatch");
			}
			if (!jpeg2000_lossy->is_jpeg2000() || jpeg2000_lossy->is_htj2k() ||
			    jpeg2000_lossy->is_lossless() || !jpeg2000_lossy->is_lossy() ||
			    !jpeg2000_lossy->supports_pixel_encode() ||
			    !jpeg2000_lossy->supports_pixel_decode()) {
				fail("JPEG2000 capability mismatch");
			}
			if (!htj2k_lossless->is_htj2k() || !htj2k_lossless->is_lossless() ||
			    htj2k_lossless->is_lossy() || !htj2k_lossless->supports_pixel_encode() ||
			    !htj2k_lossless->supports_pixel_decode()) {
				fail("HTJ2KLossless capability mismatch");
			}
			if (!jpegxl->is_jpegxl() || jpegxl->is_lossless() || !jpegxl->is_lossy() ||
			    !jpegxl->supports_pixel_encode() || !jpegxl->supports_pixel_decode()) {
				fail("JPEGXL capability mismatch");
			}
			if (!rle_lossless->is_rle() || !rle_lossless->is_encapsulated()) {
				fail("RLELossless classification mismatch");
			}
			if (!rle_lossless->is_lossless() || rle_lossless->is_lossy() ||
			    !rle_lossless->supports_pixel_encode() ||
			    !rle_lossless->supports_pixel_decode()) {
				fail("RLELossless capability mismatch");
			}
		}
	{
		auto sop_uid = dicom::uid::from_keyword("SecondaryCaptureImageStorage");
		if (!sop_uid) {
			fail("uid::from_keyword should resolve SOP class UID");
		}
		dicom::DataElement sop_elem("SOPClassUID"_tag, dicom::VR::UI, 0, 0, nullptr);
		if (!sop_elem.from_sop_class_uid(*sop_uid)) {
			fail("DataElement::from_sop_class_uid should encode SOP class UID");
		}
		auto roundtrip = sop_elem.to_sop_class_uid();
		if (!roundtrip || roundtrip->value() != sop_uid->value()) {
			fail("DataElement::from_sop_class_uid roundtrip mismatch");
		}
		auto ts_uid = dicom::uid::from_keyword("ImplicitVRLittleEndian");
		if (!ts_uid) {
			fail("uid::from_keyword should resolve transfer syntax UID");
		}
		if (sop_elem.from_transfer_syntax_uid(*sop_uid)) {
			fail("DataElement::from_transfer_syntax_uid should reject non-transfer-syntax UID");
		}
		if (sop_elem.from_sop_class_uid(*ts_uid)) {
			fail("DataElement::from_sop_class_uid should reject transfer syntax UID");
		}
	}
	{
		dicom::DataElement uid_elem("SOPInstanceUID"_tag, dicom::VR::UI, 0, 0, nullptr);
		if (!uid_elem.from_uid_string("1.2.3")) {
			fail("DataElement::from_uid_string should accept valid UID text");
		}
		if (uid_elem.length() != 6) {
			fail("DataElement::from_uid_string should pad odd length to even");
		}
		auto uid_text = uid_elem.to_uid_string();
		if (!uid_text || *uid_text != "1.2.3") {
			fail("DataElement::from_uid_string roundtrip mismatch");
		}
		if (uid_elem.from_uid_string("1..2")) {
			fail("DataElement::from_uid_string should reject invalid UID text");
		}
	}
	{
		dicom::DataElement missing_elem;
		if (missing_elem.is_present()) {
			fail("default DataElement should be missing");
		}
		if (!missing_elem.is_missing()) {
			fail("default DataElement should report is_missing");
		}
		if (static_cast<bool>(missing_elem)) {
			fail("default DataElement bool() should be false");
		}
		dicom::DataElement present_elem("Rows"_tag, dicom::VR::US, 0, 0, nullptr);
		if (!present_elem.is_present()) {
			fail("non-None VR DataElement should be present");
		}
		if (present_elem.is_missing()) {
			fail("non-None VR DataElement should not be missing");
		}
		if (!static_cast<bool>(present_elem)) {
			fail("present DataElement bool() should be true");
		}
	}


    return 0;
}
