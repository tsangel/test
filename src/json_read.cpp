#include "dicom.h"

#include "diagnostics.h"
#include "dicom_endian.h"
#include "instream.h"

#include <yyjson.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <charconv>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fmt/format.h>
#include <limits>
#include <memory>
#include <string_view>

namespace dicom {
namespace {

void skip_json_whitespace(std::string_view text, std::size_t& pos) noexcept {
	while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) {
		++pos;
	}
}

[[nodiscard]] std::string_view trim_ascii_whitespace(std::string_view text) noexcept {
	while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
		text.remove_prefix(1);
	}
	while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
		text.remove_suffix(1);
	}
	return text;
}

[[nodiscard]] std::string_view strip_utf8_bom(std::string_view text) noexcept {
	if (text.size() >= 3u &&
	    static_cast<unsigned char>(text[0]) == 0xEFu &&
	    static_cast<unsigned char>(text[1]) == 0xBBu &&
	    static_cast<unsigned char>(text[2]) == 0xBFu) {
		text.remove_prefix(3u);
	}
	return text;
}

constexpr std::array<char, 16> kUpperHexDigits{
    '0', '1', '2', '3', '4', '5', '6', '7',
    '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};

void append_tag_hex(std::string& out, Tag tag) {
	const auto value = tag.value();
	out.push_back(kUpperHexDigits[(value >> 28) & 0x0Fu]);
	out.push_back(kUpperHexDigits[(value >> 24) & 0x0Fu]);
	out.push_back(kUpperHexDigits[(value >> 20) & 0x0Fu]);
	out.push_back(kUpperHexDigits[(value >> 16) & 0x0Fu]);
	out.push_back(kUpperHexDigits[(value >> 12) & 0x0Fu]);
	out.push_back(kUpperHexDigits[(value >> 8) & 0x0Fu]);
	out.push_back(kUpperHexDigits[(value >> 4) & 0x0Fu]);
	out.push_back(kUpperHexDigits[value & 0x0Fu]);
}

void append_decimal(std::string& out, std::size_t value) {
	char buffer[32];
	auto [ptr, ec] = std::to_chars(std::begin(buffer), std::end(buffer), value);
	if (ec != std::errc{}) {
		diag::error_and_throw("read_json reason=failed to format decimal path component");
	}
	out.append(buffer, static_cast<std::size_t>(ptr - buffer));
}

[[nodiscard]] std::string make_tag_path(std::string_view prefix, Tag tag) {
	std::string out;
	out.reserve(prefix.size() + (prefix.empty() ? 0u : 1u) + 8u);
	if (!prefix.empty()) {
		out.append(prefix);
		out.push_back('.');
	}
	append_tag_hex(out, tag);
	return out;
}

[[nodiscard]] std::string make_item_path(std::string_view prefix, std::size_t item_index) {
	std::string out;
	out.reserve(prefix.size() + 24u);
	out.append(prefix);
	out.push_back('.');
	append_decimal(out, item_index);
	return out;
}

[[nodiscard]] const std::array<std::uint8_t, 256>& hex_nibble_lut() {
	static const auto lut = []() {
		std::array<std::uint8_t, 256> table{};
		table.fill(0xF0u);
		for (std::uint8_t i = 0; i <= 9u; ++i) {
			table[static_cast<unsigned char>('0' + i)] = i;
		}
		for (std::uint8_t i = 0; i < 6u; ++i) {
			table[static_cast<unsigned char>('A' + i)] = static_cast<std::uint8_t>(10u + i);
			table[static_cast<unsigned char>('a' + i)] = static_cast<std::uint8_t>(10u + i);
		}
		return table;
	}();
	return lut;
}

[[nodiscard]] std::optional<Tag> parse_json_tag_key_fast(
    std::string_view view) {
	if (view.size() != 8u) {
		return std::nullopt;
	}
	const auto& lut = hex_nibble_lut();
	const auto b0 = lut[static_cast<unsigned char>(view[0])];
	const auto b1 = lut[static_cast<unsigned char>(view[1])];
	const auto b2 = lut[static_cast<unsigned char>(view[2])];
	const auto b3 = lut[static_cast<unsigned char>(view[3])];
	const auto b4 = lut[static_cast<unsigned char>(view[4])];
	const auto b5 = lut[static_cast<unsigned char>(view[5])];
	const auto b6 = lut[static_cast<unsigned char>(view[6])];
	const auto b7 = lut[static_cast<unsigned char>(view[7])];
	if (((b0 | b1 | b2 | b3 | b4 | b5 | b6 | b7) & 0xF0u) != 0u) {
		return std::nullopt;
	}
	const auto value =
	    (static_cast<std::uint32_t>(b0) << 28) |
	    (static_cast<std::uint32_t>(b1) << 24) |
	    (static_cast<std::uint32_t>(b2) << 20) |
	    (static_cast<std::uint32_t>(b3) << 16) |
	    (static_cast<std::uint32_t>(b4) << 12) |
	    (static_cast<std::uint32_t>(b5) << 8) |
	    (static_cast<std::uint32_t>(b6) << 4) |
	    static_cast<std::uint32_t>(b7);
	return Tag(value);
}

constexpr Tag kTransferSyntaxUidTag{0x0002u, 0x0010u};

[[nodiscard]] bool is_pixel_data_path(std::string_view path) noexcept {
	return path == "7FE00010" || path == "7FE00008" || path == "7FE00009" ||
	       path == "PixelData" || path == "FloatPixelData" || path == "DoubleFloatPixelData";
}

struct SplitBulkUri {
	std::string_view path{};
	std::string_view suffix{};
};

struct FrameBulkUriTemplate {
	std::string_view path{};
	std::string_view suffix{};
	bool path_is_frames_base{false};
};

[[nodiscard]] SplitBulkUri split_bulk_uri_suffix(std::string_view uri) noexcept {
	const auto query = uri.find('?');
	const auto fragment = uri.find('#');
	std::size_t suffix_pos = std::string_view::npos;
	if (query != std::string_view::npos && fragment != std::string_view::npos) {
		suffix_pos = std::min(query, fragment);
	} else if (query != std::string_view::npos) {
		suffix_pos = query;
	} else if (fragment != std::string_view::npos) {
		suffix_pos = fragment;
	}
	if (suffix_pos == std::string_view::npos) {
		return SplitBulkUri{uri, {}};
	}
	return SplitBulkUri{uri.substr(0u, suffix_pos), uri.substr(suffix_pos)};
}

[[nodiscard]] std::string frame_bulk_uri(std::string_view base_uri, std::size_t frame_index) {
	const auto uri = split_bulk_uri_suffix(base_uri);
	const auto frame_number = frame_index + 1u;
	std::string out;
	out.reserve(uri.path.size() + uri.suffix.size() + 24u);
	out.append(uri.path);
	if (uri.path.size() >= 7u && uri.path.substr(uri.path.size() - 7u) == "/frames") {
		out.push_back('/');
		append_decimal(out, frame_number);
		out.append(uri.suffix);
		return out;
	}
	out.append("/frames/");
	append_decimal(out, frame_number);
	out.append(uri.suffix);
	return out;
}

[[nodiscard]] FrameBulkUriTemplate make_frame_bulk_uri_template(
    std::string_view base_uri) noexcept {
	const auto uri = split_bulk_uri_suffix(base_uri);
	return FrameBulkUriTemplate{
	    .path = uri.path,
	    .suffix = uri.suffix,
	    .path_is_frames_base = uri.path.size() >= 7u &&
	                           uri.path.substr(uri.path.size() - 7u) == "/frames",
	};
}

[[nodiscard]] std::string frame_bulk_uri(
    const FrameBulkUriTemplate& base_uri, std::size_t frame_index) {
	const auto frame_number = frame_index + 1u;
	std::string out;
	out.reserve(base_uri.path.size() + base_uri.suffix.size() + 24u);
	out.append(base_uri.path);
	if (base_uri.path_is_frames_base) {
		out.push_back('/');
	} else {
		out.append("/frames/");
	}
	append_decimal(out, frame_number);
	out.append(base_uri.suffix);
	return out;
}

[[nodiscard]] bool iequals_ascii(std::string_view lhs, std::string_view rhs) noexcept {
	if (lhs.size() != rhs.size()) {
		return false;
	}
	for (std::size_t i = 0; i < lhs.size(); ++i) {
		const auto a = static_cast<unsigned char>(lhs[i]);
		const auto b = static_cast<unsigned char>(rhs[i]);
		if (std::tolower(a) != std::tolower(b)) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] bool can_synthesize_frame_bulk_uri(std::string_view uri) noexcept {
	const auto split_uri = split_bulk_uri_suffix(uri);
	if (split_uri.path.size() >= 7u &&
	    split_uri.path.substr(split_uri.path.size() - 7u) == "/frames") {
		return true;
	}
	if (!split_uri.suffix.empty()) {
		return false;
	}

	const auto slash = split_uri.path.rfind('/');
	const auto last_segment =
	    slash == std::string_view::npos ? split_uri.path : split_uri.path.substr(slash + 1u);
	return iequals_ascii(last_segment, "7FE00010");
}

[[nodiscard]] std::optional<std::size_t> parse_frame_bulk_uri_index(std::string_view uri) {
	const auto split_uri = split_bulk_uri_suffix(uri);
	const auto slash = split_uri.path.rfind('/');
	if (slash == std::string_view::npos || slash + 1u >= split_uri.path.size()) {
		return std::nullopt;
	}
	std::size_t value = 0;
	const auto index_text = split_uri.path.substr(slash + 1u);
	for (const char ch : index_text) {
		if (ch < '0' || ch > '9') {
			return std::nullopt;
		}
		value = value * 10u + static_cast<std::size_t>(ch - '0');
	}
	if (value == 0u) {
		return std::nullopt;
	}
	const auto prefix = split_uri.path.substr(0, slash);
	if (prefix.size() >= 7u && prefix.substr(prefix.size() - 7u) == "/frames") {
		return value - 1u;
	}
	return std::nullopt;
}

struct ParsedFrameBulkUriList {
	FrameBulkUriTemplate base_uri{};
	std::vector<std::size_t> frame_indices{};
};

[[nodiscard]] std::optional<ParsedFrameBulkUriList> parse_frame_bulk_uri_list(
    std::string_view uri) {
	const auto split_uri = split_bulk_uri_suffix(uri);
	static constexpr std::string_view kMarker = "/frames/";
	const auto marker = split_uri.path.rfind(kMarker);
	if (marker == std::string_view::npos || marker + kMarker.size() >= split_uri.path.size()) {
		return std::nullopt;
	}

	ParsedFrameBulkUriList parsed{};
	parsed.base_uri = FrameBulkUriTemplate{
	    .path = split_uri.path.substr(0u, marker + kMarker.size() - 1u),
	    .suffix = split_uri.suffix,
	    .path_is_frames_base = true,
	};
	const auto suffix = split_uri.path.substr(marker + kMarker.size());
	std::size_t value = 0u;
	bool saw_digit = false;
	for (std::size_t i = 0; i <= suffix.size(); ++i) {
		const char ch = i < suffix.size() ? suffix[i] : ',';
		if (ch >= '0' && ch <= '9') {
			saw_digit = true;
			value = value * 10u + static_cast<std::size_t>(ch - '0');
			continue;
		}
		if (ch != ',' || !saw_digit || value == 0u) {
			return std::nullopt;
		}
		parsed.frame_indices.push_back(value - 1u);
		value = 0u;
		saw_digit = false;
	}
	if (parsed.frame_indices.empty()) {
		return std::nullopt;
	}
	return parsed;
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

[[nodiscard]] uid::WellKnown file_transfer_syntax_uid_for_bulk(const DicomFile& file) {
	const auto& transfer_syntax_element = file.dataset().get_dataelement(kTransferSyntaxUidTag);
	if (!transfer_syntax_element.is_missing()) {
		if (const auto transfer_syntax = transfer_syntax_element.to_transfer_syntax_uid()) {
			return *transfer_syntax;
		}
	}
	return uid::WellKnown{};
}

void apply_bulk_ref_metadata(JsonBulkRef& ref, uid::WellKnown transfer_syntax) {
	const bool pixel_data = is_pixel_data_path(ref.path);
	ref.media_type = bulk_media_type_for_transfer_syntax(transfer_syntax, pixel_data);
	ref.transfer_syntax_uid =
	    pixel_data && transfer_syntax.valid() ? std::string(transfer_syntax.value()) : std::string{};
}

template <typename IntT>
[[nodiscard]] std::optional<IntT> parse_integer_literal(std::string_view token) {
	IntT value{};
	const auto* begin = token.data();
	const auto* end = token.data() + token.size();
	auto [ptr, ec] = std::from_chars(begin, end, value, 10);
	if (ec != std::errc{} || ptr != end) {
		return std::nullopt;
	}
	return value;
}

[[nodiscard]] std::optional<double> parse_double_literal(std::string_view token) {
	std::string owned(token);
	char* end_ptr = nullptr;
	errno = 0;
	const double value = std::strtod(owned.c_str(), &end_ptr);
	if (errno != 0 || end_ptr != owned.c_str() + owned.size()) {
		return std::nullopt;
	}
	return value;
}

[[nodiscard]] int base64_value(unsigned char ch) noexcept {
	if (ch >= 'A' && ch <= 'Z') return ch - 'A';
	if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
	if (ch >= '0' && ch <= '9') return ch - '0' + 52;
	if (ch == '+') return 62;
	if (ch == '/') return 63;
	return -1;
}

[[nodiscard]] std::optional<std::vector<std::uint8_t>> base64_decode(std::string_view text) {
	std::vector<std::uint8_t> out;
	out.reserve((text.size() * 3u) / 4u);
	std::array<int, 4> quartet{};
	int count = 0;
	for (char ch : text) {
		if (std::isspace(static_cast<unsigned char>(ch))) {
			continue;
		}
		if (ch == '=') {
			quartet[count++] = -2;
		} else {
			const int value = base64_value(static_cast<unsigned char>(ch));
			if (value < 0) {
				return std::nullopt;
			}
			quartet[count++] = value;
		}
		if (count != 4) {
			continue;
		}
		if (quartet[0] < 0 || quartet[1] < 0) {
			return std::nullopt;
		}
		out.push_back(static_cast<std::uint8_t>((quartet[0] << 2) | (quartet[1] >> 4)));
		if (quartet[2] == -2) {
			count = 0;
			continue;
		}
		if (quartet[2] < 0) {
			return std::nullopt;
		}
		out.push_back(static_cast<std::uint8_t>(((quartet[1] & 0x0F) << 4) | (quartet[2] >> 2)));
		if (quartet[3] == -2) {
			count = 0;
			continue;
		}
		if (quartet[3] < 0) {
			return std::nullopt;
		}
		out.push_back(static_cast<std::uint8_t>(((quartet[2] & 0x03) << 6) | quartet[3]));
		count = 0;
	}
	return count == 0 ? std::optional<std::vector<std::uint8_t>>(std::move(out)) : std::nullopt;
}

[[nodiscard]] bool vr_uses_lazy_json_value_materialization(VR vr) noexcept {
	switch (static_cast<std::uint16_t>(vr)) {
	case VR::AT_val:
	case VR::SS_val:
	case VR::US_val:
	case VR::SL_val:
	case VR::UL_val:
	case VR::SV_val:
	case VR::UV_val:
	case VR::FL_val:
	case VR::FD_val:
		return true;
	default:
		return false;
	}
}

template <typename T>
void write_integral_values(DataElement& element, std::span<const T> values) {
	element.reserve_value_bytes(values.size() * sizeof(T));
	auto dst = element.value_span();
	auto* out = const_cast<std::uint8_t*>(dst.data());
	for (std::size_t i = 0; i < values.size(); ++i) {
		if constexpr (std::is_floating_point_v<T>) {
			using Bits = std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>;
			endian::store_le<Bits>(out + (i * sizeof(T)), std::bit_cast<Bits>(values[i]));
		} else {
			endian::store_le<T>(out + (i * sizeof(T)), values[i]);
		}
	}
}

struct AttributeInfo {
	std::optional<VR> vr{};
	bool has_value{false};
	const yyjson_val* value_tree{nullptr};
	bool has_inline_binary{false};
	std::string inline_binary_storage{};
	std::string_view inline_binary{};
	bool has_bulk_data_uri{false};
	std::string bulk_data_uri_storage{};
	std::string_view bulk_data_uri{};
};

[[nodiscard]] inline yyjson_val* yyjson_mut(const yyjson_val* value) noexcept {
	return const_cast<yyjson_val*>(value);
}

[[nodiscard]] bool yyjson_value_array_all_uid_like(const yyjson_val* value) {
	if (!yyjson_is_arr(yyjson_mut(value))) {
		return false;
	}
	bool saw_value = false;
	yyjson_arr_iter iter = yyjson_arr_iter_with(yyjson_mut(value));
	yyjson_val* item = nullptr;
	while ((item = yyjson_arr_iter_next(&iter))) {
		if (!yyjson_is_str(item)) {
			return false;
		}
		const std::string_view text{yyjson_get_str(item), yyjson_get_len(item)};
		if (!uid::is_valid_uid_text_strict(text)) {
			return false;
		}
		saw_value = true;
	}
	return saw_value;
}

[[nodiscard]] bool yyjson_value_array_is_pn(const yyjson_val* value) {
	if (!yyjson_is_arr(yyjson_mut(value))) {
		return false;
	}
	auto* first = yyjson_arr_get_first(yyjson_mut(value));
	if (!yyjson_is_obj(first)) {
		return false;
	}
	yyjson_obj_iter iter = yyjson_obj_iter_with(first);
	yyjson_val* key = yyjson_obj_iter_next(&iter);
	if (!key) {
		return false;
	}
	return yyjson_equals_str(key, "Alphabetic") ||
	       yyjson_equals_str(key, "Ideographic") ||
	       yyjson_equals_str(key, "Phonetic");
}

[[nodiscard]] bool yyjson_value_array_is_sequence(const yyjson_val* value) {
	if (!yyjson_is_arr(yyjson_mut(value))) {
		return false;
	}
	auto* first = yyjson_arr_get_first(yyjson_mut(value));
	return yyjson_is_obj(first) && !yyjson_value_array_is_pn(value);
}

[[nodiscard]] bool yyjson_value_array_is_string_like(const yyjson_val* value) {
	if (!yyjson_is_arr(yyjson_mut(value))) {
		return false;
	}
	return yyjson_is_str(yyjson_arr_get_first(yyjson_mut(value)));
}

[[nodiscard]] bool yyjson_value_array_is_numeric_like(
    const yyjson_val* value, bool* out_is_floating = nullptr) {
	if (!yyjson_is_arr(yyjson_mut(value))) {
		return false;
	}
	auto* first = yyjson_arr_get_first(yyjson_mut(value));
	if (!yyjson_is_num(first)) {
		return false;
	}
	if (out_is_floating) {
		*out_is_floating = yyjson_is_real(first);
	}
	return true;
}

[[nodiscard]] std::string describe_yyjson_error(
    std::string_view name, const yyjson_read_err& err) {
	if (err.msg) {
		return fmt::format(
		    "read_json name={} reason=invalid DICOM JSON: {} at byte {}",
		    name, err.msg, err.pos);
	}
	return fmt::format(
	    "read_json name={} reason=invalid DICOM JSON at byte {}",
	    name, err.pos);
}

}  // namespace

class JsonReadParser {
public:
	JsonReadParser(
	    std::string name, const std::uint8_t* data, std::size_t size, JsonReadOptions options)
	    : name_(std::move(name)),
	      data_(data),
	      size_(size),
	      text_(strip_utf8_bom(trim_ascii_whitespace(std::string_view(
	          reinterpret_cast<const char*>(data_), size_)))),
	      options_(options) {}

	[[nodiscard]] JsonReadResult parse();

private:
	[[nodiscard]] std::string_view display_name() const noexcept {
		return name_.empty() ? std::string_view{"<memory>"} : std::string_view{name_};
	}
	[[nodiscard]] std::unique_ptr<DicomFile> make_file() const;
	[[nodiscard]] JsonReadResult parse_with_yyjson();
	void parse_top_level_array_yyjson(const yyjson_val* root, JsonReadResult& result);
	void parse_dataset_object_yyjson(DataSet& dataset,
	    std::vector<JsonBulkRef>& pending_bulk_data, std::string_view path_prefix,
	    const yyjson_val* dataset_object, bool finalize_after = true);
	void parse_attribute_object_yyjson(DataSet& dataset,
	    std::vector<JsonBulkRef>& pending_bulk_data, Tag tag, std::string_view path_prefix,
	    const yyjson_val* attr_object);
	[[nodiscard]] VR resolve_attribute_vr(Tag tag, const AttributeInfo& attr) const;
	DataElement& append_leaf_element(DataSet& dataset, Tag tag, VR vr);
	void append_json_tree_value_element(
	    DataSet& dataset, Tag tag, VR vr, const yyjson_val* value);
	void parse_sequence_value_yyjson(DataSet& dataset, Tag tag,
	    std::vector<JsonBulkRef>& pending_bulk_data, std::string_view element_path,
	    const yyjson_val* value_array);
	void assign_non_string_value_array_yyjson(DataElement& element, const yyjson_val* value) const;
	void append_bulk_ref(std::vector<JsonBulkRef>& pending_bulk_data,
	    std::string_view element_path, VR vr, std::string_view uri) const;
	void finalize_attribute(DataSet& dataset,
	    std::vector<JsonBulkRef>& pending_bulk_data, Tag tag, std::string_view path_prefix,
	    const AttributeInfo& attr);
	void finalize_dataset(DataSet& dataset) const;
	void postprocess_pending_bulk(
	    DicomFile& file, std::vector<JsonBulkRef>& pending_bulk_data) const;

	std::string name_;
	const std::uint8_t* data_{nullptr};
	std::size_t size_{0};
	std::string_view text_;
	JsonReadOptions options_{};
};

[[nodiscard]] JsonReadResult read_json_borrowed(
    std::string name, const std::uint8_t* data, std::size_t size, JsonReadOptions options);

JsonReadResult JsonReadParser::parse() {
	std::size_t pos = 0;
	skip_json_whitespace(text_, pos);
	if (pos >= text_.size()) {
		diag::error_and_throw(
		    "read_json name={} reason=input is not a DICOM JSON stream; empty input",
		    display_name());
	}
	const unsigned char first = static_cast<unsigned char>(text_[pos]);
	if ((first < 0x20u && !std::isspace(first)) || first >= 0x80u) {
		diag::error_and_throw(
		    "read_json name={} reason=input is not a DICOM JSON stream; invalid JSON byte sequence",
		    display_name());
	}
	if (text_[pos] != '[' && text_[pos] != '{') {
		diag::error_and_throw(
		    "read_json name={} reason=input is not a DICOM JSON stream; expected a top-level "
		    "JSON object or array",
		    display_name());
	}
	return parse_with_yyjson();
}

std::unique_ptr<DicomFile> JsonReadParser::make_file() const {
	auto file = std::make_unique<DicomFile>();
	file->attach_to_memory(name_, std::vector<std::uint8_t>{});
	auto& dataset = file->dataset();
	dataset.json_read_charset_errors_ = options_.charset_errors;
	dataset.last_tag_loaded_ = Tag(0xFFFFu, 0xFFFFu);
	return file;
}

JsonReadResult JsonReadParser::parse_with_yyjson() {
	JsonReadResult result{};
	yyjson_read_err err{};
	auto doc = std::shared_ptr<yyjson_doc>(
	    yyjson_read_opts(const_cast<char*>(text_.data()), text_.size(), 0, nullptr, &err),
	    yyjson_doc_free);
	if (!doc) {
		diag::error_and_throw("{}", describe_yyjson_error(display_name(), err));
	}
	auto* root = yyjson_doc_get_root(doc.get());
	if (!root) {
		diag::error_and_throw(
		    "read_json name={} reason=input is not a DICOM JSON stream; empty document",
		    display_name());
	}

	if (yyjson_is_arr(root)) {
		parse_top_level_array_yyjson(root, result);
	} else if (yyjson_is_obj(root)) {
		result.items.emplace_back();
		result.items.back().file = make_file();
		parse_dataset_object_yyjson(
		    result.items.back().file->dataset(),
		    result.items.back().pending_bulk_data,
		    "",
		    root);
	} else {
		diag::error_and_throw(
		    "read_json name={} reason=input is not a DICOM JSON stream; expected a top-level "
		    "JSON object or array",
		    display_name());
	}

	for (auto& item : result.items) {
		if (!item.file) {
			continue;
		}
		item.file->json_doc_owner_ = doc;
		postprocess_pending_bulk(*item.file, item.pending_bulk_data);
	}
	return result;
}

void JsonReadParser::parse_top_level_array_yyjson(
    const yyjson_val* root, JsonReadResult& result) {
	yyjson_arr_iter iter = yyjson_arr_iter_with(yyjson_mut(root));
	yyjson_val* item = nullptr;
	while ((item = yyjson_arr_iter_next(&iter))) {
		result.items.emplace_back();
		result.items.back().file = make_file();
		parse_dataset_object_yyjson(
		    result.items.back().file->dataset(),
		    result.items.back().pending_bulk_data,
		    "",
		    item);
	}
}

void JsonReadParser::parse_dataset_object_yyjson(DataSet& dataset,
    std::vector<JsonBulkRef>& pending_bulk_data, std::string_view path_prefix,
    const yyjson_val* dataset_object, bool finalize_after) {
	if (!yyjson_is_obj(yyjson_mut(dataset_object))) {
		diag::error_and_throw(
		    "read_json name={} reason=dataset object expected",
		    display_name());
	}
	yyjson_obj_iter iter = yyjson_obj_iter_with(yyjson_mut(dataset_object));
	yyjson_val* key = nullptr;
	while ((key = yyjson_obj_iter_next(&iter))) {
		const auto key_view =
		    std::string_view(yyjson_get_str(key), yyjson_get_len(key));
		Tag tag;
		if (const auto fast_tag = parse_json_tag_key_fast(key_view)) {
			tag = *fast_tag;
		} else {
			try {
				tag = Tag(key_view);
			} catch (...) {
				diag::error_and_throw(
				    "read_json name={} key={} reason=invalid DICOM JSON tag key",
				    display_name(), key_view);
			}
		}
		parse_attribute_object_yyjson(
		    dataset,
		    pending_bulk_data,
		    tag,
		    path_prefix,
		    yyjson_obj_iter_get_val(key));
	}
	if (finalize_after) {
		finalize_dataset(dataset);
	}
}

void JsonReadParser::parse_attribute_object_yyjson(DataSet& dataset,
    std::vector<JsonBulkRef>& pending_bulk_data, Tag tag, std::string_view path_prefix,
    const yyjson_val* attr_object) {
	if (!yyjson_is_obj(yyjson_mut(attr_object))) {
		diag::error_and_throw(
		    "read_json name={} tag={} reason=attribute must be an object",
		    display_name(), tag.to_string());
	}
	AttributeInfo attr{};
	yyjson_obj_iter iter = yyjson_obj_iter_with(yyjson_mut(attr_object));
	yyjson_val* key = nullptr;
	while ((key = yyjson_obj_iter_next(&iter))) {
		const auto* member = yyjson_obj_iter_get_val(key);
		if (yyjson_equals_str(key, "vr")) {
			if (!yyjson_is_str(yyjson_mut(member))) {
				diag::error_and_throw(
				    "read_json name={} tag={} reason=vr must be a JSON string",
				    display_name(), tag.to_string());
			}
			attr.vr = VR::from_string(
			    std::string_view(
			        yyjson_get_str(yyjson_mut(member)),
			        yyjson_get_len(yyjson_mut(member))));
		} else if (yyjson_equals_str(key, "Value")) {
			attr.has_value = true;
			attr.value_tree = member;
		} else if (yyjson_equals_str(key, "InlineBinary")) {
			if (!yyjson_is_str(yyjson_mut(member))) {
				diag::error_and_throw(
				    "read_json name={} tag={} reason=InlineBinary must be a JSON string",
				    display_name(), tag.to_string());
			}
			attr.has_inline_binary = true;
			attr.inline_binary =
			    std::string_view(
			        yyjson_get_str(yyjson_mut(member)),
			        yyjson_get_len(yyjson_mut(member)));
		} else if (yyjson_equals_str(key, "BulkDataURI")) {
			if (!yyjson_is_str(yyjson_mut(member))) {
				diag::error_and_throw(
				    "read_json name={} tag={} reason=BulkDataURI must be a JSON string",
				    display_name(), tag.to_string());
			}
			attr.has_bulk_data_uri = true;
			attr.bulk_data_uri =
			    std::string_view(
			        yyjson_get_str(yyjson_mut(member)),
			        yyjson_get_len(yyjson_mut(member)));
		}
	}
	finalize_attribute(dataset, pending_bulk_data, tag, path_prefix, attr);
}

VR JsonReadParser::resolve_attribute_vr(Tag tag, const AttributeInfo& attr) const {
	if (attr.vr && attr.vr->is_known()) {
		return *attr.vr;
	}
	const auto vr_value = lookup::tag_to_vr(tag.value());
	if (vr_value != 0) {
		return VR(vr_value);
	}

	if (attr.has_inline_binary || attr.has_bulk_data_uri) {
		return VR::UN;
	}
	if (attr.has_value) {
		if (attr.value_tree) {
			if (yyjson_value_array_all_uid_like(attr.value_tree)) {
				return VR::UI;
			}
			if (yyjson_value_array_is_pn(attr.value_tree)) {
				return VR::PN;
			}
			if (yyjson_value_array_is_sequence(attr.value_tree)) {
				return VR::SQ;
			}
			if (tag.is_private()) {
				return VR::UN;
			}
			if (yyjson_value_array_is_string_like(attr.value_tree)) {
				return VR::LO;
			}
			bool is_floating = false;
			if (yyjson_value_array_is_numeric_like(attr.value_tree, &is_floating)) {
				return is_floating ? VR::FD : VR::SL;
			}
			return VR::UN;
		}
	}
	return VR::UN;
}

DataElement& JsonReadParser::append_leaf_element(DataSet& dataset, Tag tag, VR vr) {
	dataset.elements_.emplace_back(tag, vr, 0, 0, &dataset);
	auto* element = &dataset.elements_.back();
	dataset.element_index_.emplace_back(tag, element);
	++dataset.active_element_count_;
	return *element;
}

void JsonReadParser::append_json_tree_value_element(
    DataSet& dataset, Tag tag, VR vr, const yyjson_val* value) {
	auto& element = append_leaf_element(dataset, tag, vr);
	element.storage_kind_ = DataElement::StorageKind::json_tree;
	element.storage_.ptr = const_cast<yyjson_val*>(value);
	element.length_ = 0;
}

void JsonReadParser::parse_sequence_value_yyjson(DataSet& dataset, Tag tag,
    std::vector<JsonBulkRef>& pending_bulk_data, std::string_view element_path,
    const yyjson_val* value_array) {
	auto& element = append_leaf_element(dataset, tag, VR::SQ);
	auto* sequence = element.as_sequence();
	if (!sequence) {
		diag::error_and_throw(
		    "read_json name={} tag={} reason=failed to create sequence storage",
		    display_name(), tag.to_string());
	}
	if (!yyjson_is_arr(yyjson_mut(value_array))) {
		diag::error_and_throw(
		    "read_json name={} tag={} reason=SQ Value must be a JSON array",
		    display_name(), tag.to_string());
	}
	yyjson_arr_iter iter = yyjson_arr_iter_with(yyjson_mut(value_array));
	yyjson_val* item_val = nullptr;
	std::size_t item_index = 0;
	while ((item_val = yyjson_arr_iter_next(&iter))) {
		DataSet* item = sequence->add_dataset();
		const auto child_prefix = make_item_path(element_path, item_index);
		parse_dataset_object_yyjson(*item, pending_bulk_data, child_prefix, item_val);
		++item_index;
	}
}

void JsonReadParser::assign_non_string_value_array_yyjson(
    DataElement& element, const yyjson_val* value) const {
	if (!yyjson_is_arr(yyjson_mut(value))) {
		diag::error_and_throw(
		    "read_json name={} reason=Value must be a JSON array",
		    display_name());
	}
	auto* array = yyjson_mut(value);

	switch (static_cast<std::uint16_t>(element.vr())) {
	case VR::SS_val: {
		std::vector<std::int16_t> values;
		yyjson_arr_iter iter = yyjson_arr_iter_with(array);
		yyjson_val* item = nullptr;
		while ((item = yyjson_arr_iter_next(&iter))) {
			if (!yyjson_is_int(item)) {
				diag::error_and_throw(
				    "read_json name={} reason=integral Value item must be a JSON number",
				    display_name());
			}
			const auto parsed = yyjson_get_sint(item);
			if (!std::in_range<std::int16_t>(parsed)) {
				diag::error_and_throw(
				    "read_json name={} reason=integral Value item is out of range",
				    display_name());
			}
			values.push_back(static_cast<std::int16_t>(parsed));
		}
		write_integral_values(element, std::span<const std::int16_t>(values));
		return;
	}
	case VR::US_val: {
		std::vector<std::uint16_t> values;
		yyjson_arr_iter iter = yyjson_arr_iter_with(array);
		yyjson_val* item = nullptr;
		while ((item = yyjson_arr_iter_next(&iter))) {
			if (!yyjson_is_int(item)) {
				diag::error_and_throw(
				    "read_json name={} reason=integral Value item must be a JSON number",
				    display_name());
			}
			const auto parsed = yyjson_get_uint(item);
			if (!std::in_range<std::uint16_t>(parsed)) {
				diag::error_and_throw(
				    "read_json name={} reason=integral Value item is out of range",
				    display_name());
			}
			values.push_back(static_cast<std::uint16_t>(parsed));
		}
		write_integral_values(element, std::span<const std::uint16_t>(values));
		return;
	}
	case VR::SL_val: {
		std::vector<std::int32_t> values;
		yyjson_arr_iter iter = yyjson_arr_iter_with(array);
		yyjson_val* item = nullptr;
		while ((item = yyjson_arr_iter_next(&iter))) {
			if (!yyjson_is_int(item)) {
				diag::error_and_throw(
				    "read_json name={} reason=integral Value item must be a JSON number",
				    display_name());
			}
			const auto parsed = yyjson_get_sint(item);
			if (!std::in_range<std::int32_t>(parsed)) {
				diag::error_and_throw(
				    "read_json name={} reason=integral Value item is out of range",
				    display_name());
			}
			values.push_back(static_cast<std::int32_t>(parsed));
		}
		write_integral_values(element, std::span<const std::int32_t>(values));
		return;
	}
	case VR::UL_val: {
		std::vector<std::uint32_t> values;
		yyjson_arr_iter iter = yyjson_arr_iter_with(array);
		yyjson_val* item = nullptr;
		while ((item = yyjson_arr_iter_next(&iter))) {
			if (!yyjson_is_int(item)) {
				diag::error_and_throw(
				    "read_json name={} reason=integral Value item must be a JSON number",
				    display_name());
			}
			const auto parsed = yyjson_get_uint(item);
			if (!std::in_range<std::uint32_t>(parsed)) {
				diag::error_and_throw(
				    "read_json name={} reason=integral Value item is out of range",
				    display_name());
			}
			values.push_back(static_cast<std::uint32_t>(parsed));
		}
		write_integral_values(element, std::span<const std::uint32_t>(values));
		return;
	}
	case VR::SV_val: {
		std::vector<std::int64_t> values;
		yyjson_arr_iter iter = yyjson_arr_iter_with(array);
		yyjson_val* item = nullptr;
		while ((item = yyjson_arr_iter_next(&iter))) {
			if (!yyjson_is_int(item)) {
				diag::error_and_throw(
				    "read_json name={} reason=integral Value item must be a JSON number",
				    display_name());
			}
			values.push_back(yyjson_get_sint(item));
		}
		write_integral_values(element, std::span<const std::int64_t>(values));
		return;
	}
	case VR::UV_val: {
		std::vector<std::uint64_t> values;
		yyjson_arr_iter iter = yyjson_arr_iter_with(array);
		yyjson_val* item = nullptr;
		while ((item = yyjson_arr_iter_next(&iter))) {
			if (!yyjson_is_int(item)) {
				diag::error_and_throw(
				    "read_json name={} reason=integral Value item must be a JSON number",
				    display_name());
			}
			values.push_back(yyjson_get_uint(item));
		}
		write_integral_values(element, std::span<const std::uint64_t>(values));
		return;
	}
	case VR::FL_val: {
		std::vector<float> values;
		yyjson_arr_iter iter = yyjson_arr_iter_with(array);
		yyjson_val* item = nullptr;
		while ((item = yyjson_arr_iter_next(&iter))) {
			if (!yyjson_is_num(item)) {
				diag::error_and_throw(
				    "read_json name={} reason=floating-point Value item must be a JSON number",
				    display_name());
			}
			values.push_back(static_cast<float>(yyjson_get_num(item)));
		}
		write_integral_values(element, std::span<const float>(values));
		return;
	}
	case VR::FD_val: {
		std::vector<double> values;
		yyjson_arr_iter iter = yyjson_arr_iter_with(array);
		yyjson_val* item = nullptr;
		while ((item = yyjson_arr_iter_next(&iter))) {
			if (!yyjson_is_num(item)) {
				diag::error_and_throw(
				    "read_json name={} reason=floating-point Value item must be a JSON number",
				    display_name());
			}
			values.push_back(yyjson_get_num(item));
		}
		write_integral_values(element, std::span<const double>(values));
		return;
	}
	case VR::AT_val: {
		std::vector<Tag> values;
		yyjson_arr_iter iter = yyjson_arr_iter_with(array);
		yyjson_val* item = nullptr;
		while ((item = yyjson_arr_iter_next(&iter))) {
			if (!yyjson_is_str(item)) {
				diag::error_and_throw(
				    "read_json name={} reason=AT Value item must be a JSON string",
				    display_name());
			}
			const std::string_view tag_text{yyjson_get_str(item), yyjson_get_len(item)};
			try {
				values.emplace_back(tag_text);
			} catch (...) {
				diag::error_and_throw(
				    "read_json name={} value={} reason=invalid AT Value item",
				    display_name(), tag_text);
			}
		}
		if (!element.from_tag_vector(values)) {
			diag::error_and_throw(
			    "read_json name={} reason=failed to assign parsed AT values",
			    display_name());
		}
		return;
	}
	default:
		diag::error_and_throw(
		    "read_json name={} vr={} reason=unsupported Value array for this VR",
		    display_name(), element.vr().str());
	}
}

void JsonReadParser::append_bulk_ref(
    std::vector<JsonBulkRef>& pending_bulk_data, std::string_view element_path,
    VR vr, std::string_view uri) const {
	JsonBulkRef ref{};
	ref.kind = JsonBulkTargetKind::element;
	ref.path = std::string(element_path);
	ref.uri = std::string(uri);
	ref.vr = vr;
	pending_bulk_data.push_back(std::move(ref));
}

void JsonReadParser::finalize_attribute(DataSet& dataset,
    std::vector<JsonBulkRef>& pending_bulk_data, Tag tag, std::string_view path_prefix,
    const AttributeInfo& attr) {
	const VR vr = resolve_attribute_vr(tag, attr);
	std::string element_path_storage;
	const auto element_path = [&]() -> std::string_view {
		if (element_path_storage.empty()) {
			element_path_storage = make_tag_path(path_prefix, tag);
		}
		return element_path_storage;
	};
	if (attr.has_value) {
		if (vr.is_sequence()) {
			parse_sequence_value_yyjson(
			    dataset, tag, pending_bulk_data, element_path(), attr.value_tree);
			return;
		}
		if (vr.is_string() || vr == VR::UN || vr_uses_lazy_json_value_materialization(vr)) {
			append_json_tree_value_element(dataset, tag, vr, attr.value_tree);
			return;
		}
		auto& element = append_leaf_element(dataset, tag, vr);
		assign_non_string_value_array_yyjson(element, attr.value_tree);
		return;
	}

	if (attr.has_inline_binary) {
		auto decoded = base64_decode(attr.inline_binary);
		if (!decoded) {
			diag::error_and_throw(
			    "read_json name={} tag={} reason=invalid InlineBinary payload",
			    display_name(), tag.to_string());
		}
		auto& element = append_leaf_element(dataset, tag, vr);
		element.set_value_bytes(std::move(*decoded));
		return;
	}

	if (attr.has_bulk_data_uri) {
		(void)append_leaf_element(dataset, tag, vr);
		const auto path = element_path();
		append_bulk_ref(pending_bulk_data, path, vr, attr.bulk_data_uri);
		return;
	}

	(void)append_leaf_element(dataset, tag, vr);
}

void JsonReadParser::finalize_dataset(DataSet& dataset) const {
	if (!std::is_sorted(dataset.element_index_.begin(), dataset.element_index_.end(),
	        [](const ElementRef& lhs, const ElementRef& rhs) {
		        return lhs.tag.value() < rhs.tag.value();
	        })) {
		std::stable_sort(dataset.element_index_.begin(), dataset.element_index_.end(),
		    [](const ElementRef& lhs, const ElementRef& rhs) {
			    return lhs.tag.value() < rhs.tag.value();
		    });
	}

	// Duplicate keys are non-conformant JSON, but some real-world metadata
	// producers emit them. Keep the last occurrence so callers can still
	// inspect/view the dataset instead of failing the whole read.
	if (!dataset.element_index_.empty()) {
		std::size_t write_index = 0;
		for (std::size_t read_index = 0; read_index < dataset.element_index_.size();
		     ++read_index) {
			if (write_index > 0 &&
			    dataset.element_index_[write_index - 1].tag.value() ==
			        dataset.element_index_[read_index].tag.value()) {
				dataset.element_index_[write_index - 1] =
				    dataset.element_index_[read_index];
			} else {
				dataset.element_index_[write_index++] =
				    dataset.element_index_[read_index];
			}
		}
		dataset.element_index_.resize(write_index);
	}
	dataset.active_element_count_ = dataset.element_index_.size();
	for (const auto& [_, element] : dataset.element_map_) {
		if (element.is_present()) {
			++dataset.active_element_count_;
		}
	}
	dataset.last_tag_loaded_ = Tag(0xFFFFu, 0xFFFFu);
}

void JsonReadParser::postprocess_pending_bulk(
    DicomFile& file, std::vector<JsonBulkRef>& pending_bulk_data) const {
	file.dataset().on_specific_character_set_changed();
	const auto transfer_syntax = file_transfer_syntax_uid_for_bulk(file);
	const bool pixel_data_frames_are_encapsulated = transfer_syntax.valid() &&
	                                                transfer_syntax.is_encapsulated();
	const auto number_of_frames = static_cast<std::size_t>(
	    file.dataset().get_value<long>("NumberOfFrames").value_or(1));
	const auto synthesized_frame_count = std::max<std::size_t>(number_of_frames, 1u);
	const std::string pixel_media_type =
	    bulk_media_type_for_transfer_syntax(transfer_syntax, true);
	const std::string pixel_transfer_syntax_uid =
	    transfer_syntax.valid() ? std::string(transfer_syntax.value()) : std::string{};
	std::vector<JsonBulkRef> expanded;
	expanded.reserve(
	    pending_bulk_data.size() +
	    ((pixel_data_frames_are_encapsulated && synthesized_frame_count > 0u)
	            ? (synthesized_frame_count - 1u)
	            : 0u));
	for (const auto& ref : pending_bulk_data) {
		if (ref.kind == JsonBulkTargetKind::element &&
		    ref.path == "7FE00010" &&
		    pixel_data_frames_are_encapsulated) {
			if (const auto parsed_list = parse_frame_bulk_uri_list(ref.uri)) {
				for (const auto frame_index : parsed_list->frame_indices) {
					JsonBulkRef frame_ref{};
					frame_ref.kind = JsonBulkTargetKind::pixel_frame;
					frame_ref.path = ref.path;
					frame_ref.frame_index = frame_index;
					frame_ref.uri = frame_bulk_uri(parsed_list->base_uri, frame_index);
					frame_ref.media_type = pixel_media_type;
					frame_ref.transfer_syntax_uid = pixel_transfer_syntax_uid;
					frame_ref.vr = ref.vr;
					expanded.push_back(std::move(frame_ref));
				}
				continue;
			}
			if (const auto frame_index = parse_frame_bulk_uri_index(ref.uri)) {
				JsonBulkRef frame_ref{};
				frame_ref.kind = JsonBulkTargetKind::pixel_frame;
				frame_ref.path = ref.path;
				frame_ref.frame_index = *frame_index;
				frame_ref.uri = ref.uri;
				frame_ref.media_type = pixel_media_type;
				frame_ref.transfer_syntax_uid = pixel_transfer_syntax_uid;
				frame_ref.vr = ref.vr;
				expanded.push_back(std::move(frame_ref));
				continue;
			}
			if (!can_synthesize_frame_bulk_uri(ref.uri)) {
				auto resolved_ref = ref;
				apply_bulk_ref_metadata(resolved_ref, transfer_syntax);
				expanded.push_back(std::move(resolved_ref));
				continue;
			}
			const auto base_uri = make_frame_bulk_uri_template(ref.uri);
			for (std::size_t i = 0; i < synthesized_frame_count; ++i) {
				JsonBulkRef frame_ref{};
				frame_ref.kind = JsonBulkTargetKind::pixel_frame;
				frame_ref.path = ref.path;
				frame_ref.frame_index = i;
				frame_ref.uri = frame_bulk_uri(base_uri, i);
				frame_ref.media_type = pixel_media_type;
				frame_ref.transfer_syntax_uid = pixel_transfer_syntax_uid;
				frame_ref.vr = ref.vr;
				expanded.push_back(std::move(frame_ref));
			}
			continue;
		}
		auto resolved_ref = ref;
		apply_bulk_ref_metadata(resolved_ref, transfer_syntax);
		expanded.push_back(std::move(resolved_ref));
	}
	pending_bulk_data = std::move(expanded);
}

JsonReadResult read_json_borrowed(
    std::string name, const std::uint8_t* data, std::size_t size, JsonReadOptions options) {
	JsonReadParser parser(std::move(name), data, size, options);
	return parser.parse();
}

[[nodiscard]] bool is_root_pixel_data_target(std::string_view path) noexcept {
	return path.empty() || path == "7FE00010" || path == "PixelData";
}

[[nodiscard]] std::optional<uid::WellKnown> transfer_syntax_uid_from_bulk_ref(
    const JsonBulkRef& ref) {
	if (ref.transfer_syntax_uid.empty()) {
		return std::nullopt;
	}
	return uid::from_value(ref.transfer_syntax_uid);
}

void set_encapsulated_pixel_data_from_value_field(
    DicomFile& file, uid::WellKnown transfer_syntax, std::span<const std::uint8_t> value_field) {
	InStringStream payload_stream;
	payload_stream.attach_memory(value_field.data(), value_field.size(), false);
	payload_stream.set_identifier(
	    file.path().empty() ? std::string{"<json-bulk-pixeldata>"}
	                        : fmt::format("{}#json-bulk-pixeldata", file.path()));

	PixelSequence parsed_sequence(&file.dataset(), transfer_syntax);
	parsed_sequence.attach_to_stream(&payload_stream, value_field.size());
	parsed_sequence.read_attached_stream();

	file.reset_encapsulated_pixel_data(parsed_sequence.number_of_frames());
	for (std::size_t frame_index = 0; frame_index < parsed_sequence.number_of_frames();
	     ++frame_index) {
		file.set_encoded_pixel_frame(frame_index, parsed_sequence.frame_encoded_span(frame_index));
	}
}

bool DicomFile::set_bulk_data(const JsonBulkRef& ref, std::span<const std::uint8_t> bytes) {
	switch (ref.kind) {
	case JsonBulkTargetKind::element: {
		if (is_root_pixel_data_target(ref.path)) {
			if (const auto transfer_syntax = transfer_syntax_uid_from_bulk_ref(ref);
			    transfer_syntax && transfer_syntax->is_encapsulated()) {
				set_transfer_syntax_state_only(*transfer_syntax);
				set_encapsulated_pixel_data_from_value_field(*this, *transfer_syntax, bytes);
				return true;
			}
		}
		DataElement& element = ref.vr != VR::None ? ensure_dataelement(ref.path, ref.vr)
		                                          : ensure_dataelement(ref.path);
		element.set_value_bytes(bytes);
		return true;
	}
	case JsonBulkTargetKind::pixel_frame: {
		if (!is_root_pixel_data_target(ref.path)) {
			diag::error_and_throw(
			    "DicomFile::set_bulk_data file={} reason=pixel-frame bulk refs currently "
			    "support only PixelData targets",
			    path());
		}

		const auto required_frame_count =
		    std::max<std::size_t>(
		        ref.frame_index + 1u,
		        static_cast<std::size_t>(
		            root_dataset_.get_value<long>("NumberOfFrames").value_or(
		                static_cast<long>(ref.frame_index + 1u))));

		auto& pixel_data = root_dataset_.get_dataelement("PixelData");
		if (pixel_data.is_missing() || !pixel_data.vr().is_pixel_sequence()) {
			// TODO: support assembling native multi-frame PixelData from per-frame JSON bulk refs.
			reset_encapsulated_pixel_data(required_frame_count);
		} else {
			auto* pixel_sequence = pixel_data.as_pixel_sequence();
			if (!pixel_sequence || ref.frame_index >= pixel_sequence->number_of_frames()) {
				diag::error_and_throw(
				    "DicomFile::set_bulk_data file={} reason=encapsulated PixelData does "
				    "not have a writable slot for this frame",
				    path());
			}
		}

		set_encoded_pixel_frame(ref.frame_index, bytes);
		return true;
	}
	}

	diag::error_and_throw(
	    "DicomFile::set_bulk_data file={} reason=unsupported JsonBulkTargetKind",
	    path());
}

JsonReadResult read_json(
    const std::uint8_t* data, std::size_t size, JsonReadOptions options) {
	return read_json_borrowed("<memory>", data, size, options);
}

JsonReadResult read_json(const std::string& name, const std::uint8_t* data,
    std::size_t size, JsonReadOptions options) {
	return read_json_borrowed(name, data, size, options);
}

JsonReadResult read_json(
    std::string name, std::vector<std::uint8_t>&& buffer, JsonReadOptions options) {
	return read_json_borrowed(std::move(name), buffer.data(), buffer.size(), options);
}

}  // namespace dicom
