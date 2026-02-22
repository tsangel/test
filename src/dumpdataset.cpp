#include <dicom.h>

#include <algorithm>
#include <fmt/format.h>
#include <iterator>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace dicom::literals;

namespace dicom {

namespace {

constexpr std::size_t kDumpBinaryPreviewBytes = 64;
constexpr std::size_t kDumpMinValueChars = 80;
constexpr std::size_t kFragmentPreviewHeadBytes = 16;
constexpr std::size_t kFragmentPreviewTailBytes = 8;

std::string dump_tag_token(Tag tag) {
	return fmt::format("{:04x}{:04x}", tag.group(), tag.element());
}

std::string dump_escape_text(std::string_view text) {
	std::string out;
	out.reserve(text.size() + 8);
	for (unsigned char ch : text) {
		if (ch == '\\') {
			out += "\\\\";
		} else if (ch == '\'') {
			out += "\\'";
		} else if (ch >= 0x20 && ch <= 0x7e) {
			out.push_back(static_cast<char>(ch));
		} else {
			out += fmt::format("\\x{:02x}", static_cast<unsigned int>(ch));
		}
	}
	return out;
}

std::string dump_quote_text(std::string_view text) {
	return fmt::format("'{}'", dump_escape_text(text));
}

std::string truncate_dump_value_for_print_width(std::string value,
    std::size_t max_print_chars, std::size_t prefix_chars, std::size_t suffix_chars) {
	if (max_print_chars == 0) {
		return value;
	}
	if (prefix_chars >= max_print_chars) {
		return {};
	}
	const auto remaining_after_prefix = max_print_chars - prefix_chars;
	if (suffix_chars >= remaining_after_prefix) {
		return {};
	}
	const auto max_value_chars = remaining_after_prefix - suffix_chars;
	if (value.size() <= max_value_chars) {
		return value;
	}
	const bool quoted =
	    value.size() >= 2 && value.front() == '\'' && value.back() == '\'';
	if (quoted && max_value_chars >= 6) {
		const auto keep_inner_chars = max_value_chars - 5;  // opening+closing quote and "..."
		std::string out;
		out.reserve(max_value_chars);
		out.push_back('\'');
		out.append(value.data() + 1, keep_inner_chars);
		out += "...";
		out.push_back('\'');
		return out;
	}
	if (max_value_chars <= 3) {
		return std::string(max_value_chars, '.');
	}
	value.resize(max_value_chars - 3);
	value += "...";
	return value;
}

std::string pad_dump_value_min_width(std::string value, std::size_t min_width) {
	if (value.size() < min_width) {
		value.append(min_width - value.size(), ' ');
	}
	return value;
}

template <typename T>
std::string dump_join_values(const std::vector<T>& values) {
	if (values.empty()) {
		return "(no value)";
	}
	std::ostringstream oss;
	for (std::size_t i = 0; i < values.size(); ++i) {
		if (i) {
			oss << '\\';
		}
		oss << values[i];
	}
	return oss.str();
}

std::optional<std::string> dump_numeric_value(const DataElement& element) {
	switch (static_cast<std::uint16_t>(element.vr())) {
	case VR::SS_val:
	case VR::US_val:
	case VR::SL_val:
	case VR::UL_val: {
		const auto vm = element.vm();
		if (vm > 1) {
			auto values = element.to_long_vector();
			if (!values) {
				return std::nullopt;
			}
			return dump_join_values(*values);
		}
		auto value = element.to_long();
		if (!value) {
			return std::nullopt;
		}
		return std::to_string(*value);
	}
	case VR::SV_val:
	case VR::UV_val: {
		const auto vm = element.vm();
		if (vm > 1) {
			auto values = element.to_longlong_vector();
			if (!values) {
				return std::nullopt;
			}
			return dump_join_values(*values);
		}
		auto value = element.to_longlong();
		if (!value) {
			return std::nullopt;
		}
		return std::to_string(*value);
	}
	case VR::FL_val:
	case VR::FD_val: {
		const auto vm = element.vm();
		if (vm > 1) {
			auto values = element.to_double_vector();
			if (!values) {
				return std::nullopt;
			}
			return dump_join_values(*values);
		}
		auto value = element.to_double();
		if (!value) {
			return std::nullopt;
		}
		std::ostringstream oss;
		oss << *value;
		return oss.str();
	}
	default:
		return std::nullopt;
	}
}

std::string dump_tag_values(const DataElement& element) {
	const auto vm = element.vm();
	if (vm > 1) {
		auto values = element.to_tag_vector();
		if (!values) {
			return "(no value)";
		}
		std::ostringstream oss;
		for (std::size_t i = 0; i < values->size(); ++i) {
			if (i) {
				oss << '\\';
			}
			oss << dump_tag_token((*values)[i]);
		}
		return dump_quote_text(oss.str());
	}
	auto value = element.to_tag();
	if (!value) {
		return "(no value)";
	}
	return dump_quote_text(dump_tag_token(*value));
}

std::string dump_string_values(const DataElement& element) {
	auto values = element.to_string_views();
	if (!values || values->empty()) {
		return "(no value)";
	}
	std::string joined;
	for (std::size_t i = 0; i < values->size(); ++i) {
		if (i) {
			joined.push_back('\\');
		}
		joined.append((*values)[i].data(), (*values)[i].size());
	}
	if (joined.empty()) {
		return "(no value)";
	}
	return dump_quote_text(joined);
}

std::string dump_binary_value_preview(const DataElement& element) {
	const auto span = element.value_span();
	if (span.empty()) {
		return "(no value)";
	}

	std::string out;
	out.reserve(kDumpBinaryPreviewBytes * 4 + 8);
	out.push_back('\'');
	const auto preview_size = std::min<std::size_t>(span.size(), kDumpBinaryPreviewBytes);
	for (std::size_t i = 0; i < preview_size; ++i) {
		out += fmt::format("\\x{:02x}", static_cast<unsigned int>(span[i]));
	}
	if (preview_size < span.size()) {
		out += "...";
	}
	out.push_back('\'');
	return out;
}

void append_fragment_hex_bytes(std::string& out, std::span<const std::uint8_t> bytes) {
	for (std::size_t i = 0; i < bytes.size(); ++i) {
		if (!out.empty()) {
			out.push_back('\\');
		}
		fmt::format_to(std::back_inserter(out), "{:02x}",
		    static_cast<unsigned int>(bytes[i]));
	}
}

std::string format_fragment_preview(std::span<const std::uint8_t> bytes) {
	if (bytes.empty()) {
		return "(empty)";
	}
	const auto total = bytes.size();
	const auto edge_total = kFragmentPreviewHeadBytes + kFragmentPreviewTailBytes;
	std::string out;
	out.reserve(edge_total * 3 + 8);

	if (total <= edge_total) {
		append_fragment_hex_bytes(out, bytes);
		return out;
	}

	append_fragment_hex_bytes(out, bytes.first(kFragmentPreviewHeadBytes));
	std::string tail;
	append_fragment_hex_bytes(tail, bytes.last(kFragmentPreviewTailBytes));
	out += "...";
	out += tail;
	return out;
}

std::string dump_element_value(const DataElement& element) {
	if (element.vr().is_sequence()) {
		const auto* seq = element.sequence();
		const int dataset_count = seq ? seq->size() : 0;
		return fmt::format("SEQUENCE WITH {} DATASET(s)", dataset_count);
	}
	if (element.vr().is_pixel_sequence()) {
		const auto* pixseq = element.pixel_sequence();
		const auto frame_count = pixseq ? pixseq->number_of_frames() : 0;
		return fmt::format("PIXEL SEQUENCE WITH {} FRAME(S)", frame_count);
	}
	if (element.vr() == VR::AT) {
		return dump_tag_values(element);
	}
	if (element.vr() == VR::UI) {
		auto uid_value = element.to_uid_string();
		if (!uid_value || uid_value->empty()) {
			return "(no value)";
		}
		if (auto wk = uid::from_value(*uid_value)) {
			return fmt::format("{} = {}", dump_quote_text(*uid_value), wk->name());
		}
		return dump_quote_text(*uid_value);
	}
	if (auto numeric = dump_numeric_value(element)) {
		return *numeric;
	}
	if (element.vr().is_string()) {
		return dump_string_values(element);
	}
	if (element.vr().is_binary()) {
		return dump_binary_value_preview(element);
	}
	return "(no value)";
}

void append_dump_pixel_sequence_lines(std::string& out, const PixelSequence& pixseq) {
	const auto frame_count = pixseq.number_of_frames();
	const InStream* seq_stream = pixseq.stream();
	for (std::size_t frame_index = 0; frame_index < frame_count; ++frame_index) {
		const auto* frame = pixseq.frame(frame_index);
		if (!frame) {
			continue;
		}
		const auto& fragments = frame->fragments();
		std::size_t frame_size = 0;
		for (const auto& frag : fragments) {
			frame_size += frag.length;
		}

		const auto frame_begin = fragments.empty()
		                             ? 0u
		                             : (fragments.front().offset >= 8 ? fragments.front().offset - 8 : 0);
		const auto frame_end = fragments.empty()
		                           ? frame_begin
		                           : fragments.back().offset + fragments.back().length + 8;
		fmt::format_to(std::back_inserter(out),
		    "\tFRAME #{} ({} BYTES) WITH {} FRAGMENTS {{0x{:x} - 0x{:x}}}\n",
		    frame_index + 1, frame_size, fragments.size(), frame_begin, frame_end);

		for (std::size_t fragment_index = 0; fragment_index < fragments.size(); ++fragment_index) {
			const auto& frag = fragments[fragment_index];
			std::string preview = "(unavailable)";
			if (seq_stream &&
			    frag.offset <= seq_stream->end_offset() &&
			    frag.length <= seq_stream->end_offset() - frag.offset) {
				preview = format_fragment_preview(
				    seq_stream->get_span(frag.offset, frag.length));
			}
			fmt::format_to(std::back_inserter(out),
			    "\t\tFRAGMENT #{} {{0x{:x} - 0x{:x}}} len={} \"{}\"\n",
			    fragment_index, frag.offset, frag.offset + frag.length, frag.length, preview);
		}
	}
}

void append_dump_dataset_lines(std::string& out, const DataSet* dataset,
    std::string_view prefix, std::size_t max_print_chars, bool include_offset) {
	if (!dataset) {
		return;
	}
	for (const auto& element : *dataset) {
		const auto tag_token = dump_tag_token(element.tag());
		std::string tag_path;
		if (prefix.empty()) {
			tag_path = tag_token;
		} else {
			tag_path = fmt::format("{}.{}", prefix, tag_token);
		}

		const auto keyword_sv = lookup::tag_to_keyword(element.tag().value());
		const std::string keyword =
		    keyword_sv.empty() ? std::string{"-"} : std::string(keyword_sv.data(), keyword_sv.size());
		const int vm = element.vm();
		const int vm_print = vm < 0 ? 0 : vm;
		const auto prefix_text = include_offset
		                             ? fmt::format(
		                                   "'{}'\t{}\t{}\t{}\t0x{:x}\t",
		                                   tag_path, element.vr().str(),
		                                   element.length(), vm_print, element.offset())
		                             : fmt::format(
		                                   "'{}'\t{}\t{}\t{}\t",
		                                   tag_path, element.vr().str(),
		                                   element.length(), vm_print);
		const auto suffix_text = fmt::format("\t# {}\n", keyword);
		std::size_t effective_max_print_chars = max_print_chars;
		if (effective_max_print_chars != 0) {
			std::size_t min_required = prefix_text.size();
			if (std::numeric_limits<std::size_t>::max() - min_required > suffix_text.size()) {
				min_required += suffix_text.size();
			} else {
				min_required = std::numeric_limits<std::size_t>::max();
			}
			if (std::numeric_limits<std::size_t>::max() - min_required > kDumpMinValueChars) {
				min_required += kDumpMinValueChars;
			} else {
				min_required = std::numeric_limits<std::size_t>::max();
			}
			if (effective_max_print_chars < min_required) {
				effective_max_print_chars = min_required;
			}
		}
		const auto value = truncate_dump_value_for_print_width(
		    dump_element_value(element), effective_max_print_chars,
		    prefix_text.size(), suffix_text.size());
		out += prefix_text;
		out += pad_dump_value_min_width(value, kDumpMinValueChars);
		out += suffix_text;

		if (const auto* seq = element.sequence()) {
			for (int item_index = 0; item_index < seq->size(); ++item_index) {
				append_dump_dataset_lines(
				    out, seq->get_dataset(static_cast<std::size_t>(item_index)),
				    fmt::format("{}.{}", tag_path, item_index), max_print_chars, include_offset);
			}
			continue;
		}

		if (const auto* pixseq = element.pixel_sequence()) {
			append_dump_pixel_sequence_lines(out, *pixseq);
		}
	}
}

std::string dump_dataset(
    const DataSet* dataset, std::size_t max_print_chars, bool include_offset) {
	std::string out = include_offset
	                      ? "TAG\tVR\tLEN\tVM\tOFFSET\tVALUE\tKEYWORD\n"
	                      : "TAG\tVR\tLEN\tVM\tVALUE\tKEYWORD\n";
	append_dump_dataset_lines(out, dataset, {}, max_print_chars, include_offset);
	return out;
}

}  // namespace

std::string DataSet::dump(std::size_t max_print_chars, bool include_offset) const {
	if (this == root_dataset_ && stream_ && stream_->is_valid() && last_tag_loaded_ != "ffff,ffff"_tag) {
		const_cast<DataSet*>(this)->ensure_loaded("ffff,ffff"_tag);
	}
	return dump_dataset(this, max_print_chars, include_offset);
}

std::string DicomFile::dump(std::size_t max_print_chars, bool include_offset) const {
	return root_dataset_->dump(max_print_chars, include_offset);
}

}  // namespace dicom
