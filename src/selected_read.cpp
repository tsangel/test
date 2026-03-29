#include "dicom.h"

#include "dataset_deflate_codec.h"
#include "dataset_endian_converter.h"
#include "diagnostics.h"
#include "stream_path_detail.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <iterator>
#include <utility>

using namespace dicom::literals;
namespace diag = dicom::diag;

namespace dicom {

namespace {

constexpr Tag kSpecificCharacterSetTag = "(0008,0005)"_tag;
constexpr Tag kTransferSyntaxUidTag = "(0002,0010)"_tag;
constexpr Tag kFileMetaReadUntilTag = "(0002,ffff)"_tag;

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

[[nodiscard]] std::vector<DataSetSelectionNode> normalize_selection_nodes(
    std::vector<DataSetSelectionNode> nodes, bool inject_default_root_metadata = false) {
	for (auto& node : nodes) {
		if (!node.tag) {
			diag::error_and_throw(
			    "DataSetSelection reason=selection nodes must use non-zero tags");
		}
		if (!node.children.empty()) {
			node.children = normalize_selection_nodes(std::move(node.children), false);
		}
	}

	if (inject_default_root_metadata) {
		nodes.emplace_back(kTransferSyntaxUidTag);
		nodes.emplace_back(kSpecificCharacterSetTag);
	}

	std::sort(nodes.begin(), nodes.end(),
	    [](const DataSetSelectionNode& lhs, const DataSetSelectionNode& rhs) {
			return lhs.tag.value() < rhs.tag.value();
		});

	std::vector<DataSetSelectionNode> normalized;
	normalized.reserve(nodes.size());
	for (auto& node : nodes) {
		if (normalized.empty() || normalized.back().tag != node.tag) {
			normalized.push_back(std::move(node));
			continue;
		}

		auto& merged_children = normalized.back().children;
		merged_children.insert(
		    merged_children.end(),
		    std::make_move_iterator(node.children.begin()),
		    std::make_move_iterator(node.children.end()));
	}

	for (auto& node : normalized) {
		if (node.children.size() > 1) {
			node.children = normalize_selection_nodes(std::move(node.children));
		}
	}

	return normalized;
}

class SelectionCursor {
public:
	explicit SelectionCursor(std::span<const DataSetSelectionNode> nodes) : nodes_(nodes) {}

	[[nodiscard]] const DataSetSelectionNode* match(Tag tag) {
		while (index_ < nodes_.size() && nodes_[index_].tag.value() < tag.value()) {
			++index_;
		}
		if (index_ < nodes_.size() && nodes_[index_].tag == tag) {
			return &nodes_[index_];
		}
		return nullptr;
	}

	[[nodiscard]] Tag max_tag() const noexcept {
		return nodes_.empty() ? Tag{} : nodes_.back().tag;
	}

private:
	std::span<const DataSetSelectionNode> nodes_{};
	std::size_t index_{0};
};

[[nodiscard]] std::array<DataSetSelectionNode, 2> file_meta_selection_nodes() {
	return {
	    DataSetSelectionNode(kTransferSyntaxUidTag),
	    DataSetSelectionNode(kFileMetaReadUntilTag),
	};
}

[[nodiscard]] std::string_view tidy_fn_name(
    std::source_location location = std::source_location::current()) noexcept {
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

}  // namespace

class SelectedReadParser {
public:
	static void read_attached_stream(
	    DicomFile& file, const DataSetSelection& selection, const ReadOptions& options);

private:
	static void read_attached_stream_impl(
	    DicomFile& file, const DataSetSelection& selection, const ReadOptions& options);
	static void read_elements_selected(DataSet& dataset,
	    std::span<const DataSetSelectionNode> nodes, InStream* stream, bool allow_early_stop);
	static void read_sequence_selected(Sequence& sequence,
	    std::span<const DataSetSelectionNode> nodes, InStream* stream);

	static void skip_undefined_length_sequence(DataSet& dataset, InStream* stream);
	static void skip_undefined_length_dataset(DataSet& dataset, InStream* stream);

	static void read_header(DataSet& dataset, InStream* stream, bool explicit_vr,
	    std::array<std::uint8_t, 8>& buf8, std::array<std::uint8_t, 4>& buf4,
	    Tag& tag, VR& vr, std::size_t& length, std::size_t& offset);
};

void SelectedReadParser::read_header(DataSet& dataset, InStream* stream, bool explicit_vr,
    std::array<std::uint8_t, 8>& buf8, std::array<std::uint8_t, 4>& buf4, Tag& tag, VR& vr,
    std::size_t& length, std::size_t& offset) {
	const std::uint16_t gggg = endian::load_le<std::uint16_t>(buf8.data());
	const std::uint16_t eeee = endian::load_le<std::uint16_t>(buf8.data() + 2);
	tag = Tag{gggg, eeee};
	vr = VR::None;
	length = 0;

	if (explicit_vr) {
		const char vr_char0 = static_cast<char>(buf8[4]);
		const char vr_char1 = static_cast<char>(buf8[5]);
		vr = VR(vr_char0, vr_char1);

		if (vr.is_known()) {
			if (vr.uses_explicit_16bit_vl()) {
				length = endian::load_le<std::uint16_t>(buf8.data() + 6);
			} else {
				if (stream->read_4bytes(buf4) != 4) {
					diag::error_and_throw(
					    "{} file={} offset=0x{:X} tag={} vr={} reason=failed to read 4-byte length",
					    tidy_fn_name(), dataset.path(), stream->tell(), tag.to_string(), vr.str());
				}
				length = endian::load_le<std::uint32_t>(buf4.data());
				if (length != 0xffffffff && length > stream->bytes_remaining()) {
					stream->unread(4);
					length = endian::load_le<std::uint16_t>(buf8.data() + 6);
				}
			}
		} else {
			if (vr == VR('P', 'X')) {
				length = endian::load_le<std::uint16_t>(buf8.data() + 6);
			} else {
				length = endian::load_le<std::uint32_t>(buf8.data() + 4);
				const auto vr_value = lookup::tag_to_vr(tag.value());
				vr = vr_value ? VR(vr_value) : VR::UN;
			}
		}
	} else {
		const auto vr_value = lookup::tag_to_vr(tag.value());
		vr = vr_value ? VR(vr_value) : VR::UN;
		length = endian::load_le<std::uint32_t>(buf8.data() + 4);
	}

	offset = stream->tell();
}

void SelectedReadParser::skip_undefined_length_sequence(DataSet& dataset, InStream* stream) {
	std::array<std::uint8_t, 8> buf8{};

	while (!stream->is_eof()) {
		if (stream->read_8bytes(buf8) != 8) {
			diag::error_and_throw(
			    "SelectedRead skip_undefined_length_sequence file={} offset=0x{:X} reason=failed to read 8-byte item header",
			    dataset.path(), stream->tell());
		}

		const Tag tag = endian::load_tag_le(buf8.data());
		const std::size_t length = endian::load_le<std::uint32_t>(buf8.data() + 4);

		if (tag == "(fffe,e0dd)"_tag) {
			break;
		}
		if (tag != "(fffe,e000)"_tag) {
			diag::error_and_throw(
			    "SelectedRead skip_undefined_length_sequence file={} offset=0x{:X} tag={} reason=expected (FFFE,E000) item tag",
			    dataset.path(), stream->tell() - 8, tag.to_string());
		}

		if (length == 0xffffffffu) {
			skip_undefined_length_dataset(dataset, stream);
			continue;
		}
		if (stream->skip(length) != length) {
			diag::error_and_throw(
			    "SelectedRead skip_undefined_length_sequence file={} offset=0x{:X} length={} reason=failed to skip item bytes",
			    dataset.path(), stream->tell(), length);
		}
	}
}

void SelectedReadParser::skip_undefined_length_dataset(DataSet& dataset, InStream* stream) {
	std::array<std::uint8_t, 8> buf8{};
	std::array<std::uint8_t, 4> buf4{};
	const bool explicit_vr = dataset.is_explicit_vr();

	while (!stream->is_eof()) {
		if (stream->read_8bytes(buf8) != 8) {
			diag::error_and_throw(
			    "SelectedRead skip_undefined_length_dataset file={} offset=0x{:X} reason=failed to read 8-byte element header",
			    dataset.path(), stream->tell());
		}

		Tag tag = endian::load_tag_le(buf8.data());
		if (tag == "(fffe,e00d)"_tag) {
			break;
		}
		if (tag == "(fffe,e0dd)"_tag) {
			stream->unread(8);
			break;
		}

		VR vr;
		std::size_t length = 0;
		std::size_t offset = 0;
		read_header(dataset, stream, explicit_vr, buf8, buf4, tag, vr, length, offset);
		(void)offset;

		if (vr == VR::SQ || length == 0xffffffffu) {
			skip_undefined_length_sequence(dataset, stream);
			continue;
		}
		if (stream->skip(length) != length) {
			diag::error_and_throw(
			    "SelectedRead skip_undefined_length_dataset file={} offset=0x{:X} tag={} vr={} length={} reason=failed to skip value bytes",
			    dataset.path(), stream->tell(), tag.to_string(), vr.str(), length);
		}
	}
}

void SelectedReadParser::read_elements_selected(DataSet& dataset,
    std::span<const DataSetSelectionNode> nodes, InStream* stream, bool allow_early_stop) {
	std::array<std::uint8_t, 8> buf8{};
	std::array<std::uint8_t, 4> buf4{};

	if (!stream) {
		stream = dataset.stream_.get();
		if (!stream) {
			diag::error_and_throw(
			    "SelectedRead read_elements_selected file={} reason=no valid attached stream",
			    dataset.path());
		}
	}

	const bool explicit_vr = dataset.is_explicit_vr();
	SelectionCursor cursor(nodes);
	const Tag max_selected_tag = cursor.max_tag();

	while (!stream->is_eof()) {
		if (stream->read_8bytes(buf8) != 8) {
			stream->skip_to_end();
			break;
		}
		if (std::bit_cast<std::uint64_t>(buf8) == 0) {
			stream->skip_to_end();
			break;
		}

		const Tag tag = endian::load_tag_le(buf8.data());

		if (tag.group() == 0xfffeu) {
			const auto item_offset = stream->tell() - 8;
			if (tag == "(fffe,e00d)"_tag) {
				if (&dataset != dataset.root_dataset_) {
					break;
				}
				diag::error(
				    "SelectedRead read_elements_selected file={} offset=0x{:X} tag={} reason=item delim encountered out of sequence",
				    dataset.path(), item_offset, tag.to_string());
				continue;
			}
			if (tag == "(fffe,e0dd)"_tag) {
				if (&dataset != dataset.root_dataset_) {
					break;
				}
				diag::error(
				    "SelectedRead read_elements_selected file={} offset=0x{:X} tag={} reason=sequence delim encountered while parsing root dataset",
				    dataset.path(), item_offset, tag.to_string());
				continue;
			}
		}

		if (allow_early_stop && &dataset == dataset.root_dataset_ &&
		    tag.value() > max_selected_tag.value()) {
			stream->unread(8);
			break;
		}
		if (allow_early_stop && &dataset != dataset.root_dataset_ &&
		    tag.value() > max_selected_tag.value()) {
			stream->unread(8);
			break;
		}

		if (dataset.last_tag_loaded_ > tag && &dataset != dataset.root_dataset_) {
			diag::error(
			    "SelectedRead read_elements_selected file={} offset=0x{:X} tag={} last_tag={} reason=tag order decreased",
			    dataset.path(), stream->tell() - 8, tag.to_string(), dataset.last_tag_loaded_.to_string());
			stream->unread(8);
			break;
		}

		Tag parsed_tag;
		VR vr;
		std::size_t length = 0;
		std::size_t offset = 0;
		read_header(dataset, stream, explicit_vr, buf8, buf4, parsed_tag, vr, length, offset);

		const auto* matched_node = cursor.match(parsed_tag);
		const bool keep_element = (parsed_tag == kSpecificCharacterSetTag) || (matched_node != nullptr);

		if (vr == VR::SQ) {
			std::size_t sequence_length = length;
			if (sequence_length == 0xffffffffu) {
				sequence_length = stream->bytes_remaining();
			}

			if (keep_element) {
				DataElement& elem =
				    dataset.append_parsed_dataelement_nocheck(parsed_tag, VR::SQ, offset, sequence_length);
				auto* sequence = elem.as_sequence();
				InSubStream subs(stream, sequence_length);
				read_sequence_selected(*sequence,
				    matched_node ? std::span<const DataSetSelectionNode>(matched_node->children)
				                 : std::span<const DataSetSelectionNode>{},
				    &subs);
				const auto consumed = subs.tell() - offset;
				elem.set_length(consumed);
				stream->skip(consumed);
				dataset.last_tag_loaded_ = parsed_tag;
				continue;
			}

			if (length == 0xffffffffu) {
				skip_undefined_length_sequence(dataset, stream);
			} else if (stream->skip(length) != length) {
				diag::error_and_throw(
				    "SelectedRead read_elements_selected file={} offset=0x{:X} tag={} vr={} length={} reason=failed to skip SQ bytes",
				    dataset.path(), offset, parsed_tag.to_string(), vr.str(), length);
			}
			dataset.last_tag_loaded_ = parsed_tag;
			continue;
		}

		if (parsed_tag == "7fe0,0010"_tag && length == 0xffffffffu) {
			if (keep_element) {
				DataElement& elem =
				    dataset.append_parsed_dataelement_nocheck(parsed_tag, VR::PX, offset, length);
				auto* pixseq = elem.as_pixel_sequence();
				pixseq->attach_to_stream(stream, stream->bytes_remaining());
				pixseq->read_attached_stream();
				const auto consumed = pixseq->stream()->tell() - offset;
				elem.set_length(consumed);
				stream->skip(consumed);
				dataset.last_tag_loaded_ = parsed_tag;
				continue;
			}

			skip_undefined_length_sequence(dataset, stream);
			dataset.last_tag_loaded_ = parsed_tag;
			continue;
		}

		if (length == 0xffffffffu) {
			if (keep_element) {
				std::size_t inferred_length = stream->bytes_remaining();
				DataElement& elem =
				    dataset.append_parsed_dataelement_nocheck(parsed_tag, VR::SQ, offset, inferred_length);
				auto* sequence = elem.as_sequence();
				InSubStream subs(stream, inferred_length);
				read_sequence_selected(*sequence,
				    matched_node ? std::span<const DataSetSelectionNode>(matched_node->children)
				                 : std::span<const DataSetSelectionNode>{},
				    &subs);
				const auto consumed = subs.tell() - offset;
				elem.set_length(consumed);
				stream->skip(consumed);
				dataset.last_tag_loaded_ = parsed_tag;
				continue;
			}

			skip_undefined_length_sequence(dataset, stream);
			dataset.last_tag_loaded_ = parsed_tag;
			continue;
		}

		if (keep_element) {
			dataset.append_parsed_dataelement_nocheck(parsed_tag, vr, offset, length);
		}
		if (stream->skip(length) != length) {
			diag::error_and_throw(
			    "SelectedRead read_elements_selected file={} offset=0x{:X} tag={} vr={} length={} reason=value length exceeds remaining bytes",
			    dataset.path(), offset, parsed_tag.to_string(), vr.str(), length);
		}
		dataset.last_tag_loaded_ = parsed_tag;
	}

	dataset.last_tag_loaded_ = allow_early_stop ? max_selected_tag : dataset.last_tag_loaded_;
	if (stream->is_eof()) {
		dataset.last_tag_loaded_ = "ffff,ffff"_tag;
	}
}

void SelectedReadParser::read_sequence_selected(
    Sequence& sequence, std::span<const DataSetSelectionNode> nodes, InStream* stream) {
	std::array<std::uint8_t, 8> buf8{};
	auto* owner_dataset = sequence.owner_dataset_;
	if (!owner_dataset) {
		diag::error_and_throw(
		    "SelectedRead read_sequence_selected reason=no owning dataset context");
	}

	while (!stream->is_eof()) {
		if (stream->read_8bytes(buf8) != 8) {
			diag::error_and_throw(
			    "SelectedRead read_sequence_selected stream={} offset=0x{:X} reason=failed to read 8-byte item header",
			    stream->identifier(), stream->tell());
		}

		const Tag tag = endian::load_tag_le(buf8.data());
		const std::size_t length = endian::load_le<std::uint32_t>(buf8.data() + 4);

		if (tag == "(fffe,e0dd)"_tag) {
			break;
		}
		if (tag != "(fffe,e000)"_tag) {
			stream->unread(8);
			diag::error(
			    "SelectedRead read_sequence_selected stream={} offset=0x{:X} reason=expected (FFFE,E000) item tag but found {}",
			    stream->identifier(), stream->tell(), tag.to_string());
			break;
		}

		const auto item_offset = stream->tell();
		DataSet* item_dataset = sequence.add_dataset();
		if (!item_dataset) {
			diag::error_and_throw(
			    "SelectedRead read_sequence_selected reason=failed to append item dataset");
		}
		item_dataset->set_offset(item_offset);

		if (length == 0xffffffffu) {
			item_dataset->attach_to_substream(stream, stream->bytes_remaining());
			auto& subs = item_dataset->stream();
			const auto offset_start = subs.tell();
			read_elements_selected(*item_dataset, nodes, &subs, false);
			const auto offset_end = subs.tell();
			stream->skip(offset_end - offset_start);
			continue;
		}

		if (length == 0) {
			continue;
		}

		item_dataset->attach_to_substream(stream, length);
		auto& subs = item_dataset->stream();
		read_elements_selected(*item_dataset, nodes, &subs, true);
		stream->skip(length);
	}
}

void SelectedReadParser::read_attached_stream(
    DicomFile& file, const DataSetSelection& selection, const ReadOptions& options) {
	file.clear_error_state();

	std::shared_ptr<diag::Reporter> downstream = diag::thread_reporter_slot();
	if (!downstream) {
		downstream = diag::default_reporter();
	}
	auto capturing_reporter = std::make_shared<LastErrorCapturingReporter>(downstream);
	ThreadReporterGuard reporter_scope(capturing_reporter);

	try {
		read_attached_stream_impl(file, selection, options);
	} catch (const std::exception& ex) {
		const std::string_view what = ex.what();
		if (!what.empty()) {
			file.set_error_state(std::string(what));
		} else if (capturing_reporter->has_error()) {
			file.set_error_state(capturing_reporter->last_error_message());
		} else {
			file.set_error_state("read_*_selected reason=unknown read error");
		}

		if (!options.keep_on_error) {
			throw;
		}
	} catch (...) {
		if (capturing_reporter->has_error()) {
			file.set_error_state(capturing_reporter->last_error_message());
		} else {
			file.set_error_state("read_*_selected reason=unknown non-std exception");
		}

		if (!options.keep_on_error) {
			throw;
		}
	}

	if (!file.has_error() && capturing_reporter->has_error()) {
		file.set_error_state(capturing_reporter->last_error_message());
	}
}

void SelectedReadParser::read_attached_stream_impl(
    DicomFile& file, const DataSetSelection& selection, const ReadOptions& options) {
	auto& root = file.root_dataset_;
	root.elements_.clear();
	root.element_map_.clear();
	root.element_index_.clear();
	root.element_index_.reserve(load_root_elements_reserve_hint());
	root.active_element_count_ = 0;
	root.effective_charset_ = nullptr;
	root.last_tag_loaded_ = Tag::from_value(0);
	file.set_transfer_syntax_state_only("ExplicitVRLittleEndian"_uid);

	if (!root.stream_ || !root.stream_->is_valid()) {
		diag::error_and_throw(
		    "read_*_selected file={} reason=no valid attached stream", root.path());
	}

	root.stream_->rewind();
	root.stream_->skip(128);

	std::array<std::uint8_t, 4> magic{};
	if (root.stream_->read_4bytes(magic) != 4 || std::memcmp(magic.data(), "DICM", 4) != 0) {
		root.stream_->rewind();
	}

	const auto meta_selection = file_meta_selection_nodes();
	read_elements_selected(root, meta_selection, root.stream_.get(), true);

	const auto& transfer_syntax = root.get_dataelement("(0002,0010)"_tag);
	if (auto well_known = transfer_syntax.to_transfer_syntax_uid()) {
		file.set_transfer_syntax_state_only(*well_known);
	} else if (auto uid_value = transfer_syntax.to_uid_string()) {
		diag::error(
		    "read_*_selected file={} transfer_syntax_uid={} reason=unknown transfer syntax UID",
		    root.path(), *uid_value);
	}

	if (root.transfer_syntax_uid() == "DeflatedExplicitVRLittleEndian"_uid ||
	    root.transfer_syntax_uid() == "ExplicitVRBigEndian"_uid) {
		std::size_t dataset_start_offset = root.stream_->tell();
		const auto& meta_group_length = root.get_dataelement("(0002,0000)"_tag);
		if (auto group_length = meta_group_length.to_long(); group_length && *group_length >= 0) {
			const auto offset_candidate =
			    meta_group_length.offset() + meta_group_length.length() +
			    static_cast<std::size_t>(*group_length);
			if (offset_candidate <= root.stream_->end_offset()) {
				dataset_start_offset = offset_candidate;
			}
		}

		const auto full_size = root.stream_->end_offset();
		const auto full_span = root.stream_->get_span(0, full_size);
		if (dataset_start_offset > full_span.size()) {
			diag::error_and_throw(
			    "read_*_selected file={} offset=0x{:X} reason=invalid dataset body start offset",
			    root.path(), dataset_start_offset);
		}

		std::vector<std::uint8_t> normalized_image;
		if (root.transfer_syntax_uid() == "DeflatedExplicitVRLittleEndian"_uid) {
			normalized_image = inflate_deflated_dataset(full_span, dataset_start_offset);
		} else {
			normalized_image = normalize_big_endian_dataset(full_span, dataset_start_offset);
		}

		const std::string stream_identifier = root.path();
		root.attach_to_memory(stream_identifier, std::move(normalized_image));
		root.stream_->seek(dataset_start_offset);
		root.explicit_vr_ = true;
	}

	(void)options;
	read_elements_selected(root, selection.nodes(), root.stream_.get(), true);
	(void)root.refresh_effective_charset_cache(nullptr, nullptr);
}

DataSetSelection::DataSetSelection() {
	nodes_ = normalize_selection_nodes({}, true);
}

DataSetSelection::DataSetSelection(std::initializer_list<DataSetSelectionNode> init) {
	nodes_ = normalize_selection_nodes(std::vector<DataSetSelectionNode>(init), true);
}

DataSetSelection::DataSetSelection(std::vector<DataSetSelectionNode> nodes) {
	nodes_ = normalize_selection_nodes(std::move(nodes), true);
}

std::unique_ptr<DicomFile> read_file_selected(
    const std::filesystem::path& path, const DataSetSelection& selection,
    ReadOptions options) {
	auto dicom_file = std::make_unique<DicomFile>();
	dicom_file->attach_to_file(path);
	SelectedReadParser::read_attached_stream(*dicom_file, selection, options);
	return dicom_file;
}

std::unique_ptr<DicomFile> read_bytes_selected(
    const std::uint8_t* data, std::size_t size, const DataSetSelection& selection,
    ReadOptions options) {
	return read_bytes_selected(std::string{"<memory>"}, data, size, selection, options);
}

std::unique_ptr<DicomFile> read_bytes_selected(
    const std::string& name, const std::uint8_t* data, std::size_t size,
    const DataSetSelection& selection, ReadOptions options) {
	auto dicom_file = std::make_unique<DicomFile>();
	dicom_file->attach_to_memory(name, data, size, options.copy);
	SelectedReadParser::read_attached_stream(*dicom_file, selection, options);
	return dicom_file;
}

std::unique_ptr<DicomFile> read_bytes_selected(
    std::string name, std::vector<std::uint8_t>&& buffer,
    const DataSetSelection& selection, ReadOptions options) {
	auto dicom_file = std::make_unique<DicomFile>();
	dicom_file->attach_to_memory(std::move(name), std::move(buffer));
	SelectedReadParser::read_attached_stream(*dicom_file, selection, options);
	return dicom_file;
}

}  // namespace dicom
