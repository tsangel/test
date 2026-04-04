#include "dicom.h"

#include "diagnostics.h"
#include "dicom_endian.h"
#include "writing/detail/dataset_write.hpp"
#include "writing/detail/write_sinks.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <string_view>
#include <unordered_set>

#include <fmt/format.h>

namespace dicom {
namespace {

using write_detail::BufferWriter;
using write_detail::CountingWriter;
using write_detail::kItemTag;
using write_detail::kSequenceDelimitationTag;
using write_detail::throw_write_stage_error;
using write_detail::write_item_header;
using write_detail::write_u32;
constexpr Tag kLoadAllTag{0xFFFFu, 0xFFFFu};
constexpr Tag kStudyInstanceUidTag{0x0020u, 0x000Du};
constexpr Tag kSeriesInstanceUidTag{0x0020u, 0x000Eu};
constexpr Tag kSopInstanceUidTag{0x0008u, 0x0018u};
constexpr Tag kTransferSyntaxUidTag{0x0002u, 0x0010u};
constexpr Tag kFloatPixelDataTag{0x7FE0u, 0x0008u};
constexpr Tag kDoubleFloatPixelDataTag{0x7FE0u, 0x0009u};
constexpr Tag kPixelDataTag{0x7FE0u, 0x0010u};

[[nodiscard]] inline bool is_group_length_tag(Tag tag) noexcept {
	return tag.element() == 0x0000u;
}

[[nodiscard]] inline std::string tag_hex(Tag tag) {
	return fmt::format("{:08X}", tag.value());
}

void append_json_string(std::string& out, std::string_view value) {
	out.push_back('"');
	for (const unsigned char ch : value) {
		switch (ch) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\b':
			out += "\\b";
			break;
		case '\f':
			out += "\\f";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			if (ch < 0x20u) {
				out += fmt::format("\\u{:04X}", static_cast<unsigned int>(ch));
			} else {
				out.push_back(static_cast<char>(ch));
			}
			break;
		}
	}
	out.push_back('"');
}

[[nodiscard]] std::string base64_encode(std::span<const std::uint8_t> bytes) {
	static constexpr std::string_view kAlphabet =
	    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	if (bytes.empty()) {
		return {};
	}

	std::string out;
	out.reserve(((bytes.size() + 2u) / 3u) * 4u);
	std::size_t i = 0;
	for (; i + 3u <= bytes.size(); i += 3u) {
		const std::uint32_t chunk =
		    (static_cast<std::uint32_t>(bytes[i]) << 16u) |
		    (static_cast<std::uint32_t>(bytes[i + 1u]) << 8u) |
		    static_cast<std::uint32_t>(bytes[i + 2u]);
		out.push_back(kAlphabet[(chunk >> 18u) & 0x3Fu]);
		out.push_back(kAlphabet[(chunk >> 12u) & 0x3Fu]);
		out.push_back(kAlphabet[(chunk >> 6u) & 0x3Fu]);
		out.push_back(kAlphabet[chunk & 0x3Fu]);
	}

	const auto remaining = bytes.size() - i;
	if (remaining == 1u) {
		const std::uint32_t chunk = static_cast<std::uint32_t>(bytes[i]) << 16u;
		out.push_back(kAlphabet[(chunk >> 18u) & 0x3Fu]);
		out.push_back(kAlphabet[(chunk >> 12u) & 0x3Fu]);
		out += "==";
	} else if (remaining == 2u) {
		const std::uint32_t chunk =
		    (static_cast<std::uint32_t>(bytes[i]) << 16u) |
		    (static_cast<std::uint32_t>(bytes[i + 1u]) << 8u);
		out.push_back(kAlphabet[(chunk >> 18u) & 0x3Fu]);
		out.push_back(kAlphabet[(chunk >> 12u) & 0x3Fu]);
		out.push_back(kAlphabet[(chunk >> 6u) & 0x3Fu]);
		out.push_back('=');
	}
	return out;
}

template <typename T>
[[nodiscard]] std::optional<std::vector<T>> load_le_vector(std::span<const std::uint8_t> span) {
	if (span.empty()) {
		return std::vector<T>{};
	}
	if ((span.size() % sizeof(T)) != 0u) {
		return std::nullopt;
	}
	std::vector<T> out;
	out.reserve(span.size() / sizeof(T));
	for (std::size_t i = 0; i < span.size(); i += sizeof(T)) {
		if constexpr (std::is_floating_point_v<T>) {
			using Bits = std::conditional_t<sizeof(T) == 4u, std::uint32_t, std::uint64_t>;
			const Bits bits = endian::load_le<Bits>(span.data() + i);
			out.push_back(std::bit_cast<T>(bits));
		} else {
			out.push_back(endian::load_le<T>(span.data() + i));
		}
	}
	return out;
}

template <typename Writer>
void write_pixel_sequence_value_field(const PixelSequence& pixel_sequence, Writer& writer) {
	const InStream* seq_stream = pixel_sequence.stream();
	const auto offset_tables =
	    write_detail::compute_pixel_sequence_offset_tables(pixel_sequence, seq_stream);
	const auto basic_offset_table_length = write_detail::checked_u32(
	    offset_tables.basic_offsets.size() * sizeof(std::uint32_t),
	    write_detail::CheckedU32Label::basic_offset_table_length);
	write_item_header(writer, kItemTag, basic_offset_table_length);
	for (const auto offset : offset_tables.basic_offsets) {
		write_u32(writer, offset);
	}

	const auto write_fragment = [&](std::span<const std::uint8_t> fragment) {
		const auto even_value_length = write_detail::padded_length(fragment.size());
		write_item_header(
		    writer, kItemTag,
		    write_detail::checked_u32(
		        even_value_length, write_detail::CheckedU32Label::pixel_fragment_length));
		if (!fragment.empty()) {
			writer.append(fragment.data(), fragment.size());
		}
		if ((fragment.size() & 1u) != 0u) {
			writer.append_byte(0x00u);
		}
	};

	for (std::size_t frame_index = 0; frame_index < pixel_sequence.number_of_frames(); ++frame_index) {
		const PixelFrame* frame = pixel_sequence.frame(frame_index);
		if (frame == nullptr) {
			continue;
		}
		const auto encoded_data = frame->encoded_data_view();
		if (!encoded_data.empty()) {
			write_fragment(encoded_data);
			continue;
		}
		for (const auto& fragment : frame->fragments()) {
			if (!seq_stream) {
				throw_write_stage_error(
				    "write_json_pixel_data", "pixel fragment references stream but stream is null");
			}
			if (fragment.offset > seq_stream->end_offset() ||
			    fragment.length > seq_stream->end_offset() - fragment.offset) {
				throw_write_stage_error(
				    "write_json_pixel_data",
				    "pixel fragment out of bounds offset=0x{:X} length={}",
				    fragment.offset, fragment.length);
			}
			write_fragment(seq_stream->get_span(fragment.offset, fragment.length));
		}
	}

	write_item_header(writer, kSequenceDelimitationTag, 0u);
}

[[nodiscard]] std::size_t pixel_sequence_value_field_size(const PixelSequence& pixel_sequence) {
	CountingWriter writer;
	write_pixel_sequence_value_field(pixel_sequence, writer);
	return writer.written;
}

[[nodiscard]] std::vector<std::uint8_t> pixel_sequence_value_field_bytes(
    const PixelSequence& pixel_sequence) {
	std::vector<std::uint8_t> bytes;
	bytes.reserve(pixel_sequence_value_field_size(pixel_sequence));
	BufferWriter writer(bytes);
	write_pixel_sequence_value_field(pixel_sequence, writer);
	return bytes;
}

[[nodiscard]] std::string_view json_vr_text(const DataElement& element) {
	if (element.vr() == VR::PX) {
		return "OB";
	}
	const auto vr = element.vr().str();
	if (vr == "??") {
		diag::throw_exception(
		    fmt::format("DataSet::write_json reason=unsupported VR tag={}",
		        element.tag().to_string()));
	}
	return vr;
}

[[nodiscard]] bool json_inline_binary_vr(const DataElement& element) noexcept {
	switch (element.vr().value) {
	case VR::PX_val:
	case VR::OB_val:
	case VR::OD_val:
	case VR::OF_val:
	case VR::OL_val:
	case VR::OV_val:
	case VR::OW_val:
	case VR::UN_val:
		return true;
	default:
		return false;
	}
}

[[nodiscard]] bool json_bulk_uri_vr(const DataElement& element) noexcept {
	switch (element.vr().value) {
	case VR::PX_val:
	case VR::DS_val:
	case VR::FD_val:
	case VR::FL_val:
	case VR::IS_val:
	case VR::LT_val:
	case VR::OB_val:
	case VR::OD_val:
	case VR::OF_val:
	case VR::OL_val:
	case VR::OV_val:
	case VR::OW_val:
	case VR::SL_val:
	case VR::SS_val:
	case VR::ST_val:
	case VR::SV_val:
	case VR::UC_val:
	case VR::UL_val:
	case VR::UN_val:
	case VR::US_val:
	case VR::UT_val:
	case VR::UV_val:
		return true;
	default:
		return false;
	}
}

[[nodiscard]] std::size_t value_field_size_bytes(const DataElement& element) {
	if (const auto* pixel_sequence = element.as_pixel_sequence()) {
		return pixel_sequence_value_field_size(*pixel_sequence);
	}
	return element.value_span().size();
}

[[nodiscard]] std::vector<std::uint8_t> value_field_bytes(const DataElement& element) {
	if (const auto* pixel_sequence = element.as_pixel_sequence()) {
		return pixel_sequence_value_field_bytes(*pixel_sequence);
	}
	const auto span = element.value_span();
	return std::vector<std::uint8_t>(span.begin(), span.end());
}

[[nodiscard]] bool has_suffix(std::string_view text, std::string_view suffix) noexcept {
	return text.size() >= suffix.size() &&
	       text.substr(text.size() - suffix.size(), suffix.size()) == suffix;
}

[[nodiscard]] std::optional<std::string> dataset_uid_string(const DataSet& dataset, Tag tag) {
	const auto& element = dataset.get_dataelement(tag);
	if (!element) {
		return std::nullopt;
	}
	return element.to_uid_string();
}

[[nodiscard]] bool is_pixel_data_tag(Tag tag) noexcept {
	return tag == kPixelDataTag || tag == kFloatPixelDataTag || tag == kDoubleFloatPixelDataTag;
}

[[nodiscard]] uid::WellKnown dataset_transfer_syntax_uid_for_bulk(const DataSet& dataset) {
	const DataSet* root = dataset.root_dataset();
	if (root == nullptr) {
		root = &dataset;
	}
	const auto& transfer_syntax_element = root->get_dataelement(kTransferSyntaxUidTag);
	if (!transfer_syntax_element.is_missing()) {
		if (const auto transfer_syntax = transfer_syntax_element.to_transfer_syntax_uid()) {
			return *transfer_syntax;
		}
	}
	return dataset.transfer_syntax_uid();
}

[[nodiscard]] std::string bulk_media_type_for_transfer_syntax(
    uid::WellKnown transfer_syntax, bool pixel_data) {
	if (!pixel_data || !transfer_syntax.valid() || transfer_syntax.is_uncompressed()) {
		return "application/octet-stream";
	}
	if (transfer_syntax.is_rle()) {
		return "image/dicom-rle";
	}
	if (transfer_syntax.is_jpegls()) {
		return "image/jls";
	}
	if (transfer_syntax.is_htj2k()) {
		return "image/jphc";
	}
	if (transfer_syntax.is_jpegxl()) {
		return "image/jxl";
	}
	if (transfer_syntax.is_jpeg2000()) {
		const auto keyword = transfer_syntax.keyword();
		if (keyword == "JPEG2000MC" || keyword == "JPEG2000MCLossless") {
			return "image/jpx";
		}
		return "image/jp2";
	}
	if (transfer_syntax.is_jpeg_family()) {
		return "image/jpeg";
	}
	if (transfer_syntax.is_mpeg2()) {
		return "video/mpeg";
	}
	if (transfer_syntax.is_h264() || transfer_syntax.is_hevc()) {
		return "video/mp4";
	}
	return "application/octet-stream";
}

[[nodiscard]] std::pair<std::string, std::string> bulk_metadata_for_element(
    const DataSet& dataset, const DataElement& element) {
	const bool pixel_data = is_pixel_data_tag(element.tag());
	const auto transfer_syntax = pixel_data ? dataset_transfer_syntax_uid_for_bulk(dataset)
	                                        : uid::WellKnown{};
	return {
	    bulk_media_type_for_transfer_syntax(transfer_syntax, pixel_data),
	    pixel_data && transfer_syntax.valid() ? std::string(transfer_syntax.value()) : std::string{},
	};
}

[[nodiscard]] std::string resolve_bulk_data_uri(
    const DataSet& dataset, const DataElement& element, std::string_view element_path,
    const JsonWriteOptions& options) {
	const std::string* selected_template = &options.bulk_data_uri_template;
	if (element.tag() == kPixelDataTag && !options.pixel_data_uri_template.empty()) {
		selected_template = &options.pixel_data_uri_template;
	}
	if (selected_template->empty()) {
		diag::throw_exception(
		    "DataSet::write_json reason=bulk_data_uri_template is required when "
		    "bulk_data_mode=uri and a bulk value is emitted. "
		    "Example: "
		    "/dicomweb/studies/{study}/series/{series}/instances/{instance}/bulk/{tag}");
	}

	const DataSet* root = dataset.root_dataset();
	if (root == nullptr) {
		root = &dataset;
	}

	const auto study_uid = dataset_uid_string(*root, kStudyInstanceUidTag);
	const auto series_uid = dataset_uid_string(*root, kSeriesInstanceUidTag);
	const auto instance_uid = dataset_uid_string(*root, kSopInstanceUidTag);
	const auto tag_value =
	    element_path.empty() ? tag_hex(element.tag()) : std::string(element_path);

	std::string resolved;
	resolved.reserve(selected_template->size() + 64u);
	for (std::size_t i = 0; i < selected_template->size();) {
		const char ch = (*selected_template)[i];
		if (ch == '{') {
			const auto close = selected_template->find('}', i + 1u);
			if (close == std::string::npos) {
				diag::throw_exception(
				    "DataSet::write_json reason=bulk_data_uri_template has unmatched '{'");
			}
			const auto token =
			    std::string_view(*selected_template).substr(i + 1u, close - i - 1u);
			if (token == "study") {
				if (!study_uid) {
					diag::throw_exception(
					    "DataSet::write_json reason=bulk_data_uri_template requires StudyInstanceUID");
				}
				resolved += *study_uid;
			} else if (token == "series") {
				if (!series_uid) {
					diag::throw_exception(
					    "DataSet::write_json reason=bulk_data_uri_template requires SeriesInstanceUID");
				}
				resolved += *series_uid;
			} else if (token == "instance") {
				if (!instance_uid) {
					diag::throw_exception(
					    "DataSet::write_json reason=bulk_data_uri_template requires SOPInstanceUID");
				}
				resolved += *instance_uid;
			} else if (token == "tag") {
				resolved += tag_value;
			} else {
				diag::throw_exception(fmt::format(
				    "DataSet::write_json reason=unsupported bulk_data_uri_template token={}",
				    token));
			}
			i = close + 1u;
			continue;
		}
		if (ch == '}') {
			diag::throw_exception(
			    "DataSet::write_json reason=bulk_data_uri_template has unmatched '}'");
		}
		resolved.push_back(ch);
		++i;
	}
	return resolved;
}

[[nodiscard]] bool emit_bulk_uri(
    const DataElement& element, const JsonWriteOptions& options) {
	if (options.bulk_data_mode != JsonBulkDataMode::uri) {
		return false;
	}
	if (!json_bulk_uri_vr(element)) {
		return false;
	}
	return value_field_size_bytes(element) >= options.bulk_data_threshold;
}

[[nodiscard]] bool omit_bulk_value(
    const DataElement& element, const JsonWriteOptions& options) noexcept {
	return options.bulk_data_mode == JsonBulkDataMode::omit && json_bulk_uri_vr(element);
}

[[nodiscard]] std::string frame_bulk_uri(std::string_view base_uri, std::size_t frame_index) {
	const auto frame_number = frame_index + 1u;
	if (has_suffix(base_uri, "/frames")) {
		return fmt::format("{}/{}", base_uri, frame_number);
	}
	return fmt::format("{}/frames/{}", base_uri, frame_number);
}

void append_json_number(std::string& out, double value) {
	if (!std::isfinite(value)) {
		diag::throw_exception(
		    "DataSet::write_json reason=non-finite floating-point values are not supported");
	}
	out += fmt::format("{:.17g}", value);
}

template <typename T>
void append_integer_array(std::string& out, std::span<const T> values) {
	out += ",\"Value\":[";
	for (std::size_t i = 0; i < values.size(); ++i) {
		if (i != 0u) {
			out.push_back(',');
		}
		if constexpr (std::is_signed_v<T>) {
			out += fmt::format("{}", static_cast<long long>(values[i]));
		} else {
			out += fmt::format("{}", static_cast<unsigned long long>(values[i]));
		}
	}
	out.push_back(']');
}

template <typename T>
void append_float_array(std::string& out, std::span<const T> values) {
	out += ",\"Value\":[";
	for (std::size_t i = 0; i < values.size(); ++i) {
		if (i != 0u) {
			out.push_back(',');
		}
		append_json_number(out, static_cast<double>(values[i]));
	}
	out.push_back(']');
}

[[nodiscard]] std::optional<long long> parse_int_token(std::string_view token) {
	long long value = 0;
	const auto* first = token.data();
	const auto* last = token.data() + token.size();
	const auto result = std::from_chars(first, last, value);
	if (result.ec != std::errc() || result.ptr != last) {
		return std::nullopt;
	}
	return value;
}

[[nodiscard]] std::optional<double> parse_double_token(std::string_view token) {
	std::string owned(token);
	char* end = nullptr;
	errno = 0;
	const double value = std::strtod(owned.c_str(), &end);
	if (end == nullptr || *end != '\0' || errno == ERANGE || !std::isfinite(value)) {
		return std::nullopt;
	}
	return value;
}

template <typename StringRange, typename AppendFn>
void append_maybe_null_array(std::string& out, const StringRange& values, AppendFn&& append_non_empty) {
	out += ",\"Value\":[";
	for (std::size_t i = 0; i < values.size(); ++i) {
		if (i != 0u) {
			out.push_back(',');
		}
		if (values[i].empty()) {
			out += "null";
		} else {
			append_non_empty(values[i]);
		}
	}
	out.push_back(']');
}

[[nodiscard]] std::vector<std::string> string_values_for_json(
    const DataElement& element, CharsetDecodeErrorPolicy errors) {
	if (element.length() == 0u) {
		return {};
	}
	if (element.vr().uses_specific_character_set()) {
		auto values = element.to_utf8_strings(errors);
		if (!values) {
			diag::throw_exception(fmt::format(
			    "DataSet::write_json reason=failed to decode text value tag={} vr={}",
			    element.tag().to_string(), element.vr().str()));
		}
		return *values;
	}
	auto values = element.to_string_views();
	if (!values) {
		diag::throw_exception(fmt::format(
		    "DataSet::write_json reason=failed to split string value tag={} vr={}",
		    element.tag().to_string(), element.vr().str()));
	}
	std::vector<std::string> out;
	out.reserve(values->size());
	for (const auto value : *values) {
		out.emplace_back(value);
	}
	return out;
}

void append_pn_value_array(
    std::string& out, const DataElement& element, CharsetDecodeErrorPolicy errors) {
	const auto values = string_values_for_json(element, errors);
	if (values.empty()) {
		return;
	}
	append_maybe_null_array(out, values, [&](const std::string& raw_value) {
		const auto parsed = PersonName::parse(raw_value);
		if (!parsed) {
			diag::throw_exception(fmt::format(
			    "DataSet::write_json reason=failed to parse PN value tag={}",
			    element.tag().to_string()));
		}
		out.push_back('{');
		bool first = true;
		const auto append_group = [&](std::string_view key,
		                              const std::optional<PersonNameGroup>& group) {
			if (!group || group->empty()) {
				return;
			}
			if (!first) {
				out.push_back(',');
			}
			first = false;
			append_json_string(out, key);
			out.push_back(':');
			append_json_string(out, group->to_dicom_string());
		};
		append_group("Alphabetic", parsed->alphabetic);
		append_group("Ideographic", parsed->ideographic);
		append_group("Phonetic", parsed->phonetic);
		out.push_back('}');
	});
}

void append_string_value_array(
    std::string& out, const DataElement& element, CharsetDecodeErrorPolicy errors) {
	const auto values = string_values_for_json(element, errors);
	if (values.empty()) {
		return;
	}
	append_maybe_null_array(out, values, [&](const std::string& value) {
		append_json_string(out, value);
	});
}

void append_tag_value_array(std::string& out, const DataElement& element) {
	const auto values = element.to_tag_vector();
	if (!values) {
		diag::throw_exception(fmt::format(
		    "DataSet::write_json reason=failed to read AT value tag={}",
		    element.tag().to_string()));
	}
	if (values->empty()) {
		return;
	}
	out += ",\"Value\":[";
	for (std::size_t i = 0; i < values->size(); ++i) {
		if (i != 0u) {
			out.push_back(',');
		}
		append_json_string(out, tag_hex((*values)[i]));
	}
	out.push_back(']');
}

void append_ds_or_is_value_array(std::string& out, const DataElement& element, bool decimal) {
	const auto values = element.to_string_views();
	if (!values) {
		diag::throw_exception(fmt::format(
		    "DataSet::write_json reason=failed to split {} value tag={}",
		    decimal ? "DS" : "IS", element.tag().to_string()));
	}
	if (values->empty()) {
		return;
	}
	out += ",\"Value\":[";
	for (std::size_t i = 0; i < values->size(); ++i) {
		if (i != 0u) {
			out.push_back(',');
		}
		const auto token = (*values)[i];
		if (token.empty()) {
			out += "null";
			continue;
		}
		if (decimal) {
			if (const auto parsed = parse_double_token(token)) {
				append_json_number(out, *parsed);
			} else {
				append_json_string(out, token);
			}
		} else {
			if (const auto parsed = parse_int_token(token)) {
				out += fmt::format("{}", *parsed);
			} else {
				append_json_string(out, token);
			}
		}
	}
	out.push_back(']');
}

void append_numeric_value_array(std::string& out, const DataElement& element) {
	const auto span = element.value_span();
	switch (element.vr().value) {
	case VR::SS_val: {
		const auto values = load_le_vector<std::int16_t>(span);
		if (!values) break;
		append_integer_array(out, std::span<const std::int16_t>(*values));
		return;
	}
	case VR::US_val: {
		const auto values = load_le_vector<std::uint16_t>(span);
		if (!values) break;
		append_integer_array(out, std::span<const std::uint16_t>(*values));
		return;
	}
	case VR::SL_val: {
		const auto values = load_le_vector<std::int32_t>(span);
		if (!values) break;
		append_integer_array(out, std::span<const std::int32_t>(*values));
		return;
	}
	case VR::UL_val: {
		const auto values = load_le_vector<std::uint32_t>(span);
		if (!values) break;
		append_integer_array(out, std::span<const std::uint32_t>(*values));
		return;
	}
	case VR::SV_val: {
		const auto values = load_le_vector<std::int64_t>(span);
		if (!values) break;
		append_integer_array(out, std::span<const std::int64_t>(*values));
		return;
	}
	case VR::UV_val: {
		const auto values = load_le_vector<std::uint64_t>(span);
		if (!values) break;
		append_integer_array(out, std::span<const std::uint64_t>(*values));
		return;
	}
	case VR::FL_val: {
		const auto values = load_le_vector<float>(span);
		if (!values) break;
		append_float_array(out, std::span<const float>(*values));
		return;
	}
	case VR::FD_val: {
		const auto values = load_le_vector<double>(span);
		if (!values) break;
		append_float_array(out, std::span<const double>(*values));
		return;
	}
	default:
		break;
	}
	diag::throw_exception(fmt::format(
	    "DataSet::write_json reason=failed to serialize numeric value tag={} vr={}",
	    element.tag().to_string(), element.vr().str()));
}

void append_regular_value_member(
    std::string& out, const DataElement& element, const JsonWriteOptions& options) {
	if (json_inline_binary_vr(element)) {
		const auto bytes = value_field_bytes(element);
		if (bytes.empty()) {
			return;
		}
		out += ",\"InlineBinary\":";
		append_json_string(out, base64_encode(bytes));
		return;
	}

	switch (element.vr().value) {
	case VR::PN_val:
		append_pn_value_array(out, element, options.charset_errors);
		return;
	case VR::AE_val:
	case VR::AS_val:
	case VR::CS_val:
	case VR::DA_val:
	case VR::DT_val:
	case VR::LO_val:
	case VR::LT_val:
	case VR::SH_val:
	case VR::ST_val:
	case VR::TM_val:
	case VR::UC_val:
	case VR::UI_val:
	case VR::UR_val:
	case VR::UT_val:
		append_string_value_array(out, element, options.charset_errors);
		return;
	case VR::AT_val:
		append_tag_value_array(out, element);
		return;
	case VR::DS_val:
		append_ds_or_is_value_array(out, element, true);
		return;
	case VR::IS_val:
		append_ds_or_is_value_array(out, element, false);
		return;
	case VR::SS_val:
	case VR::US_val:
	case VR::SL_val:
	case VR::UL_val:
	case VR::SV_val:
	case VR::UV_val:
	case VR::FL_val:
	case VR::FD_val:
		append_numeric_value_array(out, element);
		return;
	default:
		diag::throw_exception(fmt::format(
		    "DataSet::write_json reason=unsupported value serialization tag={} vr={}",
		    element.tag().to_string(), element.vr().str()));
	}
}

struct JsonEmitContext {
	const JsonWriteOptions& options;
	JsonWriteResult* result{nullptr};
	std::unordered_set<std::string> seen_bulk_uris{};
};

void append_bulk_parts_for_uri(
    const DataSet& dataset, const DataElement& element, std::string_view base_uri,
    JsonEmitContext& context) {
	const auto [media_type, transfer_syntax_uid] = bulk_metadata_for_element(dataset, element);
	if (const auto* pixel_sequence = element.as_pixel_sequence()) {
		const auto frame_count = pixel_sequence->number_of_frames();
		if (frame_count >= 1u) {
			if (!context.seen_bulk_uris.insert(std::string(base_uri)).second) {
				diag::throw_exception(fmt::format(
				    "DataSet::write_json reason=duplicate BulkDataURI generated uri={}. "
				    "The bulk_data_uri_template likely resolves to the same URI for multiple "
				    "bulk elements. Keep {{tag}} in bulk_data_uri_template; nested bulk "
				    "elements expand it to dotted paths such as "
				    "22002200.0.12340012. Example: "
				    "/dicomweb/studies/{{study}}/series/{{series}}/instances/{{instance}}/bulk/{{tag}}. "
				    "If PixelData should use a /frames route, set pixel_data_uri_template separately.",
				    base_uri));
			}
			DicomFile* root_file = dataset.root_file();
			for (std::size_t frame_index = 0; frame_index < frame_count; ++frame_index) {
				const auto part_uri = frame_bulk_uri(base_uri, frame_index);
				if (!context.seen_bulk_uris.insert(part_uri).second) {
					diag::throw_exception(fmt::format(
					    "DataSet::write_json reason=duplicate frame BulkDataURI generated uri={}. "
					    "When using frame-style PixelData URIs, keep a distinct "
					    "bulk_data_uri_template for non-PixelData elements, e.g. "
					    "/dicomweb/studies/{{study}}/series/{{series}}/instances/{{instance}}/bulk/{{tag}}, "
					    "and use pixel_data_uri_template for /frames routes.",
					    part_uri));
				}
				if (root_file != nullptr) {
					context.result->bulk_parts.emplace_back(
					    part_uri, root_file->encoded_pixel_frame_view(frame_index),
					    media_type, transfer_syntax_uid);
				} else {
					auto* mutable_pixel_sequence = const_cast<PixelSequence*>(pixel_sequence);
					context.result->bulk_parts.emplace_back(
					    part_uri, mutable_pixel_sequence->frame_encoded_span(frame_index),
					    media_type, transfer_syntax_uid);
				}
			}
			return;
		}
	}

	if (!context.seen_bulk_uris.insert(std::string(base_uri)).second) {
		diag::throw_exception(fmt::format(
		    "DataSet::write_json reason=duplicate BulkDataURI generated uri={}. "
		    "The bulk_data_uri_template likely resolves to the same URI for multiple "
		    "bulk elements. Keep {{tag}} in bulk_data_uri_template; nested bulk "
		    "elements expand it to dotted paths such as "
		    "22002200.0.12340012. Example: "
		    "/dicomweb/studies/{{study}}/series/{{series}}/instances/{{instance}}/bulk/{{tag}}. "
		    "If PixelData should use a /frames route, set pixel_data_uri_template separately.",
		    base_uri));
	}
	context.result->bulk_parts.emplace_back(
	    std::string(base_uri), element.value_span(), media_type, transfer_syntax_uid);
}

void append_dataset_json_object(
    std::string& out, const DataSet& dataset, std::string_view path_prefix,
    JsonEmitContext& context);

void append_sequence_value_array(
    std::string& out, const Sequence& sequence, std::string_view sequence_path,
    JsonEmitContext& context) {
	out += ",\"Value\":[";
	for (int item_index = 0; item_index < sequence.size(); ++item_index) {
		if (item_index != 0) {
			out.push_back(',');
		}
		const DataSet* item = sequence.get_dataset(static_cast<std::size_t>(item_index));
		if (item == nullptr) {
			out += "{}";
		} else {
			const auto child_path = fmt::format("{}.{}", sequence_path, item_index);
			append_dataset_json_object(out, *item, child_path, context);
		}
	}
	out.push_back(']');
}

void append_attribute_json(
    std::string& out, const DataSet& dataset, const DataElement& element,
    std::string_view element_path, JsonEmitContext& context) {
	out.push_back('{');
	append_json_string(out, "vr");
	out.push_back(':');
	append_json_string(out, json_vr_text(element));

	if (element.vr().is_sequence()) {
		const Sequence* sequence = element.as_sequence();
		if (!sequence) {
			diag::throw_exception(fmt::format(
			    "DataSet::write_json reason=SQ element has null sequence pointer tag={}",
			    element.tag().to_string()));
		}
		append_sequence_value_array(out, *sequence, element_path, context);
		out.push_back('}');
		return;
	}

	if (omit_bulk_value(element, context.options)) {
		out.push_back('}');
		return;
	}

	if (emit_bulk_uri(element, context.options)) {
		const auto uri = resolve_bulk_data_uri(dataset, element, element_path, context.options);
		out += ",\"BulkDataURI\":";
		append_json_string(out, uri);
		append_bulk_parts_for_uri(dataset, element, uri, context);
		out.push_back('}');
		return;
	}

	append_regular_value_member(out, element, context.options);
	out.push_back('}');
}

void append_dataset_json_object(
    std::string& out, const DataSet& dataset, std::string_view path_prefix,
    JsonEmitContext& context) {
	out.push_back('{');
	bool first = true;
	for (const auto& element : dataset) {
		if (!element.is_present()) {
			continue;
		}
		if (is_group_length_tag(element.tag())) {
			continue;
		}
		if (!context.options.include_group_0002 && element.tag().group() == 0x0002u) {
			continue;
		}
		if (!first) {
			out.push_back(',');
		}
		first = false;
		append_json_string(out, tag_hex(element.tag()));
		out.push_back(':');
		const auto element_path = path_prefix.empty()
		    ? tag_hex(element.tag())
		    : fmt::format("{}.{}", path_prefix, tag_hex(element.tag()));
		append_attribute_json(out, dataset, element, element_path, context);
	}
	out.push_back('}');
}

}  // namespace

JsonWriteResult DataSet::write_json(const JsonWriteOptions& options) const {
	if (const DataSet* root = root_dataset(); root != nullptr) {
		root->ensure_loaded(kLoadAllTag);
	} else {
		ensure_loaded(kLoadAllTag);
	}

	JsonWriteResult result{};
	JsonEmitContext context{
	    .options = options,
	    .result = &result,
	};
	result.json.reserve(size() * 48u);
	append_dataset_json_object(result.json, *this, "", context);
	return result;
}

JsonWriteResult DicomFile::write_json(const JsonWriteOptions& options) const {
	return dataset().write_json(options);
}

}  // namespace dicom
