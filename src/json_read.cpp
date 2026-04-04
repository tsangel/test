#include "dicom.h"

#include "diagnostics.h"
#include "dicom_endian.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <charconv>
#include <cerrno>
#include <cstdlib>
#include <fmt/format.h>
#include <limits>
#include <memory>
#include <string_view>

namespace dicom {
namespace {

struct JsonStringToken {
	std::size_t token_begin{0};
	std::size_t token_end{0};
	std::size_t content_begin{0};
	std::size_t content_end{0};
	bool has_escape{false};
};

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

[[nodiscard]] bool validate_json_stream_ascii_envelope(std::string_view text) noexcept {
	bool in_string = false;
	bool escaping = false;
	for (std::size_t i = 0; i < text.size(); ++i) {
		const unsigned char ch = static_cast<unsigned char>(text[i]);
		if (!in_string) {
			if (ch >= 0x80u) {
				return false;
			}
			if (ch == '"') {
				in_string = true;
				continue;
			}
			if (ch < 0x20u && !std::isspace(ch)) {
				return false;
			}
			continue;
		}

		if (escaping) {
			if (ch == 'u') {
				if (i + 4u >= text.size()) {
					return false;
				}
				for (std::size_t j = 1; j <= 4u; ++j) {
					const unsigned char hex = static_cast<unsigned char>(text[i + j]);
					if (!std::isxdigit(hex)) {
						return false;
					}
				}
				i += 4u;
			}
			escaping = false;
			continue;
		}
		if (ch == '\\') {
			escaping = true;
			continue;
		}
		if (ch == '"') {
			in_string = false;
			continue;
		}
		if (ch < 0x20u) {
			return false;
		}
	}
	return !in_string && !escaping;
}

[[nodiscard]] std::optional<std::uint32_t> parse_hex4(std::string_view text, std::size_t pos) {
	if (pos + 4u > text.size()) {
		return std::nullopt;
	}
	std::uint32_t value = 0;
	for (std::size_t i = 0; i < 4u; ++i) {
		value <<= 4u;
		const char ch = text[pos + i];
		if (ch >= '0' && ch <= '9') {
			value |= static_cast<std::uint32_t>(ch - '0');
			continue;
		}
		if (ch >= 'a' && ch <= 'f') {
			value |= static_cast<std::uint32_t>(10 + ch - 'a');
			continue;
		}
		if (ch >= 'A' && ch <= 'F') {
			value |= static_cast<std::uint32_t>(10 + ch - 'A');
			continue;
		}
		return std::nullopt;
	}
	return value;
}

void append_utf8_codepoint(std::string& out, std::uint32_t codepoint) {
	if (codepoint <= 0x7Fu) {
		out.push_back(static_cast<char>(codepoint));
		return;
	}
	if (codepoint <= 0x7FFu) {
		out.push_back(static_cast<char>(0xC0u | (codepoint >> 6u)));
		out.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
		return;
	}
	if (codepoint <= 0xFFFFu) {
		out.push_back(static_cast<char>(0xE0u | (codepoint >> 12u)));
		out.push_back(static_cast<char>(0x80u | ((codepoint >> 6u) & 0x3Fu)));
		out.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
		return;
	}
	out.push_back(static_cast<char>(0xF0u | (codepoint >> 18u)));
	out.push_back(static_cast<char>(0x80u | ((codepoint >> 12u) & 0x3Fu)));
	out.push_back(static_cast<char>(0x80u | ((codepoint >> 6u) & 0x3Fu)));
	out.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
}

[[nodiscard]] bool parse_json_string_token(
    std::string_view text, std::size_t& pos, JsonStringToken& out_token) {
	skip_json_whitespace(text, pos);
	if (pos >= text.size() || text[pos] != '"') {
		return false;
	}
	out_token = JsonStringToken{};
	out_token.token_begin = pos;
	out_token.content_begin = pos + 1u;
	++pos;
	while (pos < text.size()) {
		const char ch = text[pos];
		if (ch == '"') {
			out_token.content_end = pos;
			out_token.token_end = pos + 1u;
			++pos;
			return true;
		}
		if (ch == '\\') {
			out_token.has_escape = true;
			++pos;
			if (pos >= text.size()) {
				return false;
			}
			if (text[pos] == 'u') {
				pos += 5u;
				if (pos > text.size()) {
					return false;
				}
				continue;
			}
			++pos;
			continue;
		}
		++pos;
	}
	return false;
}

[[nodiscard]] std::optional<std::string> decode_json_string_token(
    std::string_view text, const JsonStringToken& token) {
	if (!token.has_escape) {
		return std::string(text.substr(
		    token.content_begin, token.content_end - token.content_begin));
	}

	std::string out;
	out.reserve(token.content_end - token.content_begin);
	for (std::size_t pos = token.content_begin; pos < token.content_end; ++pos) {
		const char ch = text[pos];
		if (ch != '\\') {
			out.push_back(ch);
			continue;
		}
		++pos;
		if (pos >= token.content_end) {
			return std::nullopt;
		}
		switch (text[pos]) {
		case '"':
		case '\\':
		case '/':
			out.push_back(text[pos]);
			break;
		case 'b':
			out.push_back('\b');
			break;
		case 'f':
			out.push_back('\f');
			break;
		case 'n':
			out.push_back('\n');
			break;
		case 'r':
			out.push_back('\r');
			break;
		case 't':
			out.push_back('\t');
			break;
		case 'u': {
			auto codepoint = parse_hex4(text, pos + 1u);
			if (!codepoint) {
				return std::nullopt;
			}
			pos += 4u;
			if (*codepoint >= 0xD800u && *codepoint <= 0xDBFFu) {
				if (pos + 6u >= token.content_end || text[pos + 1u] != '\\' ||
				    text[pos + 2u] != 'u') {
					return std::nullopt;
				}
				auto low = parse_hex4(text, pos + 3u);
				if (!low || *low < 0xDC00u || *low > 0xDFFFu) {
					return std::nullopt;
				}
				*codepoint =
				    0x10000u + (((*codepoint - 0xD800u) << 10u) | (*low - 0xDC00u));
				pos += 6u;
			}
			append_utf8_codepoint(out, *codepoint);
			break;
		}
		default:
			return std::nullopt;
		}
	}
	return out;
}

[[nodiscard]] bool skip_json_number_token(
    std::string_view text, std::size_t& pos, std::string_view& out_token) {
	skip_json_whitespace(text, pos);
	const std::size_t begin = pos;
	if (pos < text.size() && text[pos] == '-') {
		++pos;
	}
	bool saw_digit = false;
	while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]))) {
		saw_digit = true;
		++pos;
	}
	if (pos < text.size() && text[pos] == '.') {
		++pos;
		while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]))) {
			saw_digit = true;
			++pos;
		}
	}
	if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E')) {
		++pos;
		if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) {
			++pos;
		}
		bool saw_exp_digit = false;
		while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]))) {
			saw_exp_digit = true;
			++pos;
		}
		if (!saw_exp_digit) {
			return false;
		}
	}
	if (!saw_digit) {
		return false;
	}
	out_token = text.substr(begin, pos - begin);
	return true;
}

[[nodiscard]] bool skip_json_literal(
    std::string_view text, std::size_t& pos, std::string_view literal) {
	skip_json_whitespace(text, pos);
	if (text.substr(pos, literal.size()) != literal) {
		return false;
	}
	pos += literal.size();
	return true;
}

[[nodiscard]] bool skip_json_value(std::string_view text, std::size_t& pos);

[[nodiscard]] bool skip_json_array(std::string_view text, std::size_t& pos);
[[nodiscard]] bool skip_json_object(std::string_view text, std::size_t& pos);

[[nodiscard]] bool skip_json_array(std::string_view text, std::size_t& pos) {
	skip_json_whitespace(text, pos);
	if (pos >= text.size() || text[pos] != '[') {
		return false;
	}
	++pos;
	skip_json_whitespace(text, pos);
	if (pos < text.size() && text[pos] == ']') {
		++pos;
		return true;
	}
	while (pos < text.size()) {
		if (!skip_json_value(text, pos)) {
			return false;
		}
		skip_json_whitespace(text, pos);
		if (pos < text.size() && text[pos] == ',') {
			++pos;
			continue;
		}
		if (pos < text.size() && text[pos] == ']') {
			++pos;
			return true;
		}
		return false;
	}
	return false;
}

[[nodiscard]] bool skip_json_object(std::string_view text, std::size_t& pos) {
	skip_json_whitespace(text, pos);
	if (pos >= text.size() || text[pos] != '{') {
		return false;
	}
	++pos;
	skip_json_whitespace(text, pos);
	if (pos < text.size() && text[pos] == '}') {
		++pos;
		return true;
	}
	while (pos < text.size()) {
		JsonStringToken key{};
		if (!parse_json_string_token(text, pos, key)) {
			return false;
		}
		skip_json_whitespace(text, pos);
		if (pos >= text.size() || text[pos] != ':') {
			return false;
		}
		++pos;
		if (!skip_json_value(text, pos)) {
			return false;
		}
		skip_json_whitespace(text, pos);
		if (pos < text.size() && text[pos] == ',') {
			++pos;
			continue;
		}
		if (pos < text.size() && text[pos] == '}') {
			++pos;
			return true;
		}
		return false;
	}
	return false;
}

[[nodiscard]] bool skip_json_value(std::string_view text, std::size_t& pos) {
	skip_json_whitespace(text, pos);
	if (pos >= text.size()) {
		return false;
	}
	if (text[pos] == '"') {
		JsonStringToken token{};
		return parse_json_string_token(text, pos, token);
	}
	if (text[pos] == '{') {
		return skip_json_object(text, pos);
	}
	if (text[pos] == '[') {
		return skip_json_array(text, pos);
	}
	std::string_view number{};
	if (skip_json_number_token(text, pos, number)) {
		return true;
	}
	return skip_json_literal(text, pos, "null") || skip_json_literal(text, pos, "true") ||
	       skip_json_literal(text, pos, "false");
}

[[nodiscard]] std::string tag_hex(Tag tag) {
	return fmt::format("{:04X}{:04X}", tag.group(), tag.element());
}

constexpr Tag kTransferSyntaxUidTag{0x0002u, 0x0010u};

[[nodiscard]] bool is_pixel_data_path(std::string_view path) noexcept {
	return path == "7FE00010" || path == "7FE00008" || path == "7FE00009" ||
	       path == "PixelData" || path == "FloatPixelData" || path == "DoubleFloatPixelData";
}

[[nodiscard]] std::string frame_bulk_uri(std::string_view base_uri, std::size_t frame_index) {
	const auto frame_number = frame_index + 1u;
	if (base_uri.size() >= 7u && base_uri.substr(base_uri.size() - 7u) == "/frames") {
		return fmt::format("{}/{}", base_uri, frame_number);
	}
	return fmt::format("{}/frames/{}", base_uri, frame_number);
}

[[nodiscard]] std::optional<std::size_t> parse_frame_bulk_uri_index(std::string_view uri) {
	const auto slash = uri.rfind('/');
	if (slash == std::string_view::npos || slash + 1u >= uri.size()) {
		return std::nullopt;
	}
	std::size_t value = 0;
	const auto index_text = uri.substr(slash + 1u);
	for (const char ch : index_text) {
		if (ch < '0' || ch > '9') {
			return std::nullopt;
		}
		value = value * 10u + static_cast<std::size_t>(ch - '0');
	}
	if (value == 0u) {
		return std::nullopt;
	}
	const auto prefix = uri.substr(0, slash);
	if (prefix.size() >= 7u && prefix.substr(prefix.size() - 7u) == "/frames") {
		return value - 1u;
	}
	return std::nullopt;
}

struct ParsedFrameBulkUriList {
	std::string_view base_uri{};
	std::vector<std::size_t> frame_indices{};
};

[[nodiscard]] std::optional<ParsedFrameBulkUriList> parse_frame_bulk_uri_list(
    std::string_view uri) {
	static constexpr std::string_view kMarker = "/frames/";
	const auto marker = uri.rfind(kMarker);
	if (marker == std::string_view::npos || marker + kMarker.size() >= uri.size()) {
		return std::nullopt;
	}

	ParsedFrameBulkUriList parsed{};
	parsed.base_uri = uri.substr(0u, marker + kMarker.size() - 1u);
	const auto suffix = uri.substr(marker + kMarker.size());
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

[[nodiscard]] bool json_value_array_all_uid_like(std::string_view fragment) {
	std::size_t pos = 0;
	skip_json_whitespace(fragment, pos);
	if (pos >= fragment.size() || fragment[pos] != '[') {
		return false;
	}
	++pos;
	skip_json_whitespace(fragment, pos);
	if (pos < fragment.size() && fragment[pos] == ']') {
		return false;
	}
	bool saw_value = false;
	while (pos < fragment.size()) {
		JsonStringToken token{};
		if (!parse_json_string_token(fragment, pos, token)) {
			return false;
		}
		auto decoded = decode_json_string_token(fragment, token);
		if (!decoded || !uid::is_valid_uid_text_strict(*decoded)) {
			return false;
		}
		saw_value = true;
		skip_json_whitespace(fragment, pos);
		if (pos < fragment.size() && fragment[pos] == ',') {
			++pos;
			continue;
		}
		if (pos < fragment.size() && fragment[pos] == ']') {
			return saw_value;
		}
		return false;
	}
	return false;
}

[[nodiscard]] bool json_value_array_is_pn(std::string_view fragment) {
	std::size_t pos = 0;
	skip_json_whitespace(fragment, pos);
	if (pos >= fragment.size() || fragment[pos] != '[') {
		return false;
	}
	++pos;
	skip_json_whitespace(fragment, pos);
	if (pos >= fragment.size() || fragment[pos] != '{') {
		return false;
	}
	++pos;
	skip_json_whitespace(fragment, pos);
	if (pos < fragment.size() && fragment[pos] == '}') {
		return false;
	}
	JsonStringToken token{};
	if (!parse_json_string_token(fragment, pos, token)) {
		return false;
	}
	auto decoded = decode_json_string_token(fragment, token);
	if (!decoded) {
		return false;
	}
	return *decoded == "Alphabetic" || *decoded == "Ideographic" || *decoded == "Phonetic";
}

[[nodiscard]] bool json_value_array_is_sequence(std::string_view fragment) {
	std::size_t pos = 0;
	skip_json_whitespace(fragment, pos);
	if (pos >= fragment.size() || fragment[pos] != '[') {
		return false;
	}
	++pos;
	skip_json_whitespace(fragment, pos);
	return pos < fragment.size() && fragment[pos] == '{' && !json_value_array_is_pn(fragment);
}

[[nodiscard]] bool json_value_array_is_string_like(std::string_view fragment) {
	std::size_t pos = 0;
	skip_json_whitespace(fragment, pos);
	if (pos >= fragment.size() || fragment[pos] != '[') {
		return false;
	}
	++pos;
	skip_json_whitespace(fragment, pos);
	return pos < fragment.size() && fragment[pos] == '"';
}

[[nodiscard]] bool json_value_array_is_numeric_like(
    std::string_view fragment, bool* out_is_floating = nullptr) {
	std::size_t pos = 0;
	skip_json_whitespace(fragment, pos);
	if (pos >= fragment.size() || fragment[pos] != '[') {
		return false;
	}
	++pos;
	skip_json_whitespace(fragment, pos);
	std::string_view token{};
	if (!skip_json_number_token(fragment, pos, token)) {
		return false;
	}
	if (out_is_floating) {
		*out_is_floating = token.find_first_of(".eE") != std::string_view::npos;
	}
	return true;
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
	std::size_t value_offset{0};
	std::size_t value_length{0};
	std::optional<std::string> inline_binary{};
	std::optional<std::string> bulk_data_uri{};
};

}  // namespace

class JsonReadParser {
public:
	JsonReadParser(std::string name,
	    std::shared_ptr<const std::vector<std::uint8_t>> bytes,
	    JsonReadOptions options)
	    : name_(std::move(name)),
	      bytes_(std::move(bytes)),
	      text_(strip_utf8_bom(trim_ascii_whitespace(std::string_view(
	          reinterpret_cast<const char*>(bytes_->data()), bytes_->size())))),
	      base_offset_(static_cast<std::size_t>(
	          text_.data() - reinterpret_cast<const char*>(bytes_->data()))),
	      options_(options) {}

	[[nodiscard]] JsonReadResult parse();

private:
	[[nodiscard]] std::string_view display_name() const noexcept {
		return name_.empty() ? std::string_view{"<memory>"} : std::string_view{name_};
	}
	[[nodiscard]] std::size_t absolute_offset(std::size_t pos) const noexcept {
		return base_offset_ + pos;
	}
	[[nodiscard]] std::unique_ptr<DicomFile> make_file() const;
	void parse_top_level_array(JsonReadResult& result, std::size_t& pos);
	void parse_dataset_object(DataSet& dataset, std::vector<JsonBulkRef>& pending_bulk_data,
	    std::string_view path_prefix, std::size_t& pos, bool finalize_after = true);
	void parse_attribute_object(DataSet& dataset,
	    std::vector<JsonBulkRef>& pending_bulk_data, Tag tag, std::string_view element_path,
	    std::size_t& pos);
	[[nodiscard]] VR resolve_attribute_vr(Tag tag, const AttributeInfo& attr) const;
	DataElement& append_leaf_element(DataSet& dataset, Tag tag, VR vr);
	void append_json_stream_string_element(
	    DataSet& dataset, Tag tag, VR vr, std::size_t offset, std::size_t length);
	void parse_sequence_value(DataSet& dataset, Tag tag,
	    std::vector<JsonBulkRef>& pending_bulk_data, std::string_view element_path,
	    std::size_t offset, std::size_t length);
	template <typename T>
	void parse_integral_array_into(std::string_view fragment, std::vector<T>& out_values) const;
	template <typename T>
	void parse_floating_array_into(std::string_view fragment, std::vector<T>& out_values) const;
	void parse_at_array_into(std::string_view fragment, std::vector<Tag>& out_values) const;
	void assign_non_string_value_array(DataElement& element, std::string_view fragment) const;
	void append_bulk_ref(std::vector<JsonBulkRef>& pending_bulk_data,
	    std::string_view element_path, VR vr, const std::string& uri) const;
	void finalize_attribute(DataSet& dataset,
	    std::vector<JsonBulkRef>& pending_bulk_data, Tag tag, std::string_view element_path,
	    const AttributeInfo& attr);
	void finalize_dataset(DataSet& dataset) const;
	void postprocess_pending_bulk(
	    DicomFile& file, std::vector<JsonBulkRef>& pending_bulk_data) const;

	std::string name_;
	std::shared_ptr<const std::vector<std::uint8_t>> bytes_;
	std::string_view text_;
	std::size_t base_offset_{0};
	JsonReadOptions options_{};
};

[[nodiscard]] JsonReadResult read_json_from_shared(
    std::string name, std::shared_ptr<const std::vector<std::uint8_t>> bytes,
    JsonReadOptions options);

JsonReadResult JsonReadParser::parse() {
	JsonReadResult result{};
	std::size_t pos = 0;
	skip_json_whitespace(text_, pos);
	if (pos >= text_.size()) {
		diag::error_and_throw(
		    "read_json name={} reason=input is not a DICOM JSON stream; empty input",
		    display_name());
	}
	if (!validate_json_stream_ascii_envelope(text_)) {
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
	if (text_[pos] == '[') {
		parse_top_level_array(result, pos);
		skip_json_whitespace(text_, pos);
		if (pos != text_.size()) {
			diag::error_and_throw(
			    "read_json name={} reason=unexpected trailing content after top-level array",
			    display_name());
		}
		return result;
	}

	result.items.emplace_back();
	result.items.back().file = make_file();
	parse_dataset_object(
	    result.items.back().file->dataset(), result.items.back().pending_bulk_data, "", pos);
	postprocess_pending_bulk(*result.items.back().file, result.items.back().pending_bulk_data);
	skip_json_whitespace(text_, pos);
	if (pos != text_.size()) {
		diag::error_and_throw(
		    "read_json name={} reason=unexpected trailing content after top-level object",
		    display_name());
	}
	return result;
}

std::unique_ptr<DicomFile> JsonReadParser::make_file() const {
	auto file = std::make_unique<DicomFile>();
	file->attach_to_memory(name_, bytes_->data(), bytes_->size(), false);
	auto& dataset = file->dataset();
	dataset.attached_memory_owner_ = bytes_;
	dataset.json_read_charset_errors_ = options_.charset_errors;
	dataset.last_tag_loaded_ = Tag(0xFFFFu, 0xFFFFu);
	return file;
}

void JsonReadParser::parse_top_level_array(JsonReadResult& result, std::size_t& pos) {
	++pos;  // '['
	skip_json_whitespace(text_, pos);
	if (pos < text_.size() && text_[pos] == ']') {
		++pos;
		return;
	}
	while (pos < text_.size()) {
		result.items.emplace_back();
		result.items.back().file = make_file();
		parse_dataset_object(
		    result.items.back().file->dataset(), result.items.back().pending_bulk_data, "", pos);
		postprocess_pending_bulk(*result.items.back().file, result.items.back().pending_bulk_data);
		skip_json_whitespace(text_, pos);
		if (pos < text_.size() && text_[pos] == ',') {
			++pos;
			continue;
		}
		if (pos < text_.size() && text_[pos] == ']') {
			++pos;
			return;
		}
		diag::error_and_throw(
		    "read_json name={} reason=malformed top-level array",
		    display_name());
	}
	diag::error_and_throw(
	    "read_json name={} reason=unterminated top-level array",
	    display_name());
}

void JsonReadParser::parse_dataset_object(DataSet& dataset,
    std::vector<JsonBulkRef>& pending_bulk_data, std::string_view path_prefix,
    std::size_t& pos, bool finalize_after) {
	skip_json_whitespace(text_, pos);
	if (pos >= text_.size() || text_[pos] != '{') {
		diag::error_and_throw(
		    "read_json name={} reason=dataset object expected",
		    display_name());
	}
	++pos;
	skip_json_whitespace(text_, pos);
	if (pos < text_.size() && text_[pos] == '}') {
		++pos;
		if (finalize_after) {
			finalize_dataset(dataset);
		}
		return;
	}
	while (pos < text_.size()) {
		JsonStringToken key_token{};
		if (!parse_json_string_token(text_, pos, key_token)) {
			diag::error_and_throw(
			    "read_json name={} reason=dataset key must be a JSON string",
			    display_name());
		}
		auto key = decode_json_string_token(text_, key_token);
		if (!key) {
			diag::error_and_throw(
			    "read_json name={} reason=failed to decode dataset key",
			    display_name());
		}
		Tag tag;
		try {
			tag = Tag(*key);
		} catch (...) {
			diag::error_and_throw(
			    "read_json name={} key={} reason=invalid DICOM JSON tag key",
			    display_name(), *key);
		}
		skip_json_whitespace(text_, pos);
		if (pos >= text_.size() || text_[pos] != ':') {
			diag::error_and_throw(
			    "read_json name={} key={} reason=expected ':' after dataset key",
			    display_name(), *key);
		}
		++pos;
		const auto element_path = path_prefix.empty()
		    ? tag_hex(tag)
		    : fmt::format("{}.{}", path_prefix, tag_hex(tag));
		parse_attribute_object(dataset, pending_bulk_data, tag, element_path, pos);
		skip_json_whitespace(text_, pos);
		if (pos < text_.size() && text_[pos] == ',') {
			++pos;
			continue;
		}
		if (pos < text_.size() && text_[pos] == '}') {
			++pos;
			if (finalize_after) {
				finalize_dataset(dataset);
			}
			return;
		}
		diag::error_and_throw(
		    "read_json name={} reason=malformed dataset object",
		    display_name());
	}
	diag::error_and_throw(
	    "read_json name={} reason=unterminated dataset object",
	    display_name());
}

void JsonReadParser::parse_attribute_object(DataSet& dataset,
    std::vector<JsonBulkRef>& pending_bulk_data, Tag tag, std::string_view element_path,
    std::size_t& pos) {
	skip_json_whitespace(text_, pos);
	if (pos >= text_.size() || text_[pos] != '{') {
		diag::error_and_throw(
		    "read_json name={} tag={} reason=attribute must be an object",
		    display_name(), tag.to_string());
	}
	++pos;
	AttributeInfo attr{};
	skip_json_whitespace(text_, pos);
	if (pos < text_.size() && text_[pos] == '}') {
		++pos;
		finalize_attribute(dataset, pending_bulk_data, tag, element_path, attr);
		return;
	}
	while (pos < text_.size()) {
		JsonStringToken key_token{};
		if (!parse_json_string_token(text_, pos, key_token)) {
			diag::error_and_throw(
			    "read_json name={} tag={} reason=attribute member key must be a JSON string",
			    display_name(), tag.to_string());
		}
		auto key = decode_json_string_token(text_, key_token);
		if (!key) {
			diag::error_and_throw(
			    "read_json name={} tag={} reason=failed to decode attribute member key",
			    display_name(), tag.to_string());
		}
		skip_json_whitespace(text_, pos);
		if (pos >= text_.size() || text_[pos] != ':') {
			diag::error_and_throw(
			    "read_json name={} tag={} reason=expected ':' in attribute object",
			    display_name(), tag.to_string());
		}
		++pos;
		if (*key == "vr") {
			JsonStringToken vr_token{};
			if (!parse_json_string_token(text_, pos, vr_token)) {
				diag::error_and_throw(
				    "read_json name={} tag={} reason=vr must be a JSON string",
				    display_name(), tag.to_string());
			}
			auto vr_text = decode_json_string_token(text_, vr_token);
			if (!vr_text) {
				diag::error_and_throw(
				    "read_json name={} tag={} reason=failed to decode vr string",
				    display_name(), tag.to_string());
			}
			attr.vr = VR::from_string(*vr_text);
		} else if (*key == "Value") {
			skip_json_whitespace(text_, pos);
			const auto begin = pos;
			if (!skip_json_value(text_, pos)) {
				diag::error_and_throw(
				    "read_json name={} tag={} reason=malformed Value property",
				    display_name(), tag.to_string());
			}
			attr.has_value = true;
			attr.value_offset = absolute_offset(begin);
			attr.value_length = pos - begin;
		} else if (*key == "InlineBinary") {
			JsonStringToken token{};
			if (!parse_json_string_token(text_, pos, token)) {
				diag::error_and_throw(
				    "read_json name={} tag={} reason=InlineBinary must be a JSON string",
				    display_name(), tag.to_string());
			}
			attr.inline_binary = decode_json_string_token(text_, token);
		} else if (*key == "BulkDataURI") {
			JsonStringToken token{};
			if (!parse_json_string_token(text_, pos, token)) {
				diag::error_and_throw(
				    "read_json name={} tag={} reason=BulkDataURI must be a JSON string",
				    display_name(), tag.to_string());
			}
			attr.bulk_data_uri = decode_json_string_token(text_, token);
		} else if (!skip_json_value(text_, pos)) {
			diag::error_and_throw(
			    "read_json name={} tag={} reason=failed to skip unsupported attribute member",
			    display_name(), tag.to_string());
		}
		skip_json_whitespace(text_, pos);
		if (pos < text_.size() && text_[pos] == ',') {
			++pos;
			continue;
		}
		if (pos < text_.size() && text_[pos] == '}') {
			++pos;
			finalize_attribute(dataset, pending_bulk_data, tag, element_path, attr);
			return;
		}
		diag::error_and_throw(
		    "read_json name={} tag={} reason=malformed attribute object",
		    display_name(), tag.to_string());
	}
	diag::error_and_throw(
	    "read_json name={} tag={} reason=unterminated attribute object",
	    display_name(), tag.to_string());
}

VR JsonReadParser::resolve_attribute_vr(Tag tag, const AttributeInfo& attr) const {
	if (attr.vr && attr.vr->is_known()) {
		return *attr.vr;
	}
	const auto vr_value = lookup::tag_to_vr(tag.value());
	if (vr_value != 0) {
		return VR(vr_value);
	}

	if (attr.inline_binary || attr.bulk_data_uri) {
		return VR::UN;
	}
	if (attr.has_value) {
		const auto local_offset = attr.value_offset - base_offset_;
		const auto fragment = text_.substr(local_offset, attr.value_length);
		if (json_value_array_all_uid_like(fragment)) {
			return VR::UI;
		}
		if (json_value_array_is_pn(fragment)) {
			return VR::PN;
		}
		if (json_value_array_is_sequence(fragment)) {
			return VR::SQ;
		}
		if (tag.is_private()) {
			return VR::UN;
		}
		if (json_value_array_is_string_like(fragment)) {
			return VR::LO;
		}
		bool is_floating = false;
		if (json_value_array_is_numeric_like(fragment, &is_floating)) {
			return is_floating ? VR::FD : VR::SL;
		}
	}
	return VR::UN;
}

DataElement& JsonReadParser::append_leaf_element(DataSet& dataset, Tag tag, VR vr) {
	dataset.elements_.emplace_back();
	auto* element = &dataset.elements_.back();
	element->reset_without_release(tag, vr, 0, 0, &dataset, false);
	dataset.element_index_.emplace_back(tag, element);
	++dataset.active_element_count_;
	return *element;
}

void JsonReadParser::append_json_stream_string_element(
    DataSet& dataset, Tag tag, VR vr, std::size_t offset, std::size_t length) {
	auto& element = append_leaf_element(dataset, tag, vr);
	element.storage_kind_ = DataElement::StorageKind::json_stream;
	element.storage_.offset_ = offset;
	element.length_ = length;
}

void JsonReadParser::parse_sequence_value(DataSet& dataset, Tag tag,
    std::vector<JsonBulkRef>& pending_bulk_data, std::string_view element_path,
    std::size_t offset, std::size_t length) {
	auto& element = append_leaf_element(dataset, tag, VR::SQ);
	auto* sequence = element.as_sequence();
	if (!sequence) {
		diag::error_and_throw(
		    "read_json name={} tag={} reason=failed to create sequence storage",
		    display_name(), tag.to_string());
	}
	std::size_t pos = offset - base_offset_;
	const std::size_t end = pos + length;
	skip_json_whitespace(text_, pos);
	if (pos >= end || text_[pos] != '[') {
		diag::error_and_throw(
		    "read_json name={} tag={} reason=SQ Value must be a JSON array",
		    display_name(), tag.to_string());
	}
	++pos;
	skip_json_whitespace(text_, pos);
	if (pos < end && text_[pos] == ']') {
		return;
	}
	std::size_t item_index = 0;
	while (pos < end) {
		DataSet* item = sequence->add_dataset();
		const auto child_prefix = fmt::format("{}.{}", element_path, item_index);
		parse_dataset_object(*item, pending_bulk_data, child_prefix, pos);
		++item_index;
		skip_json_whitespace(text_, pos);
		if (pos < end && text_[pos] == ',') {
			++pos;
			continue;
		}
		if (pos < end && text_[pos] == ']') {
			return;
		}
		diag::error_and_throw(
		    "read_json name={} tag={} reason=malformed SQ Value array",
		    display_name(), tag.to_string());
	}
	diag::error_and_throw(
	    "read_json name={} tag={} reason=unterminated SQ Value array",
	    display_name(), tag.to_string());
}

template <typename T>
void JsonReadParser::parse_integral_array_into(
    std::string_view fragment, std::vector<T>& out_values) const {
	std::size_t pos = 0;
	skip_json_whitespace(fragment, pos);
	if (pos >= fragment.size() || fragment[pos] != '[') {
		diag::error_and_throw(
		    "read_json name={} reason=numeric Value must be a JSON array",
		    display_name());
	}
	++pos;
	skip_json_whitespace(fragment, pos);
	if (pos < fragment.size() && fragment[pos] == ']') {
		return;
	}
	while (pos < fragment.size()) {
		std::string_view token{};
		if (!skip_json_number_token(fragment, pos, token)) {
			diag::error_and_throw(
			    "read_json name={} reason=integral Value item must be a JSON number",
			    display_name());
		}
		auto parsed = parse_integer_literal<T>(token);
		if (!parsed) {
			diag::error_and_throw(
			    "read_json name={} value={} reason=integral Value item is out of range",
			    display_name(), token);
		}
		out_values.push_back(*parsed);
		skip_json_whitespace(fragment, pos);
		if (pos < fragment.size() && fragment[pos] == ',') {
			++pos;
			continue;
		}
		if (pos < fragment.size() && fragment[pos] == ']') {
			return;
		}
		diag::error_and_throw(
		    "read_json name={} reason=malformed numeric Value array",
		    display_name());
	}
}

template <typename T>
void JsonReadParser::parse_floating_array_into(
    std::string_view fragment, std::vector<T>& out_values) const {
	std::size_t pos = 0;
	skip_json_whitespace(fragment, pos);
	if (pos >= fragment.size() || fragment[pos] != '[') {
		diag::error_and_throw(
		    "read_json name={} reason=floating-point Value must be a JSON array",
		    display_name());
	}
	++pos;
	skip_json_whitespace(fragment, pos);
	if (pos < fragment.size() && fragment[pos] == ']') {
		return;
	}
	while (pos < fragment.size()) {
		std::string_view token{};
		if (!skip_json_number_token(fragment, pos, token)) {
			diag::error_and_throw(
			    "read_json name={} reason=floating-point Value item must be a JSON number",
			    display_name());
		}
		auto parsed = parse_double_literal(token);
		if (!parsed) {
			diag::error_and_throw(
			    "read_json name={} value={} reason=failed to parse floating-point Value item",
			    display_name(), token);
		}
		out_values.push_back(static_cast<T>(*parsed));
		skip_json_whitespace(fragment, pos);
		if (pos < fragment.size() && fragment[pos] == ',') {
			++pos;
			continue;
		}
		if (pos < fragment.size() && fragment[pos] == ']') {
			return;
		}
		diag::error_and_throw(
		    "read_json name={} reason=malformed floating-point Value array",
		    display_name());
	}
}

void JsonReadParser::parse_at_array_into(
    std::string_view fragment, std::vector<Tag>& out_values) const {
	std::size_t pos = 0;
	skip_json_whitespace(fragment, pos);
	if (pos >= fragment.size() || fragment[pos] != '[') {
		diag::error_and_throw(
		    "read_json name={} reason=AT Value must be a JSON array",
		    display_name());
	}
	++pos;
	skip_json_whitespace(fragment, pos);
	if (pos < fragment.size() && fragment[pos] == ']') {
		return;
	}
	while (pos < fragment.size()) {
		JsonStringToken token{};
		if (!parse_json_string_token(fragment, pos, token)) {
			diag::error_and_throw(
			    "read_json name={} reason=AT Value item must be a JSON string",
			    display_name());
		}
		auto tag_text = decode_json_string_token(fragment, token);
		if (!tag_text) {
			diag::error_and_throw(
			    "read_json name={} reason=failed to decode AT Value item",
			    display_name());
		}
		try {
			out_values.emplace_back(*tag_text);
		} catch (...) {
			diag::error_and_throw(
			    "read_json name={} value={} reason=invalid AT Value item",
			    display_name(), *tag_text);
		}
		skip_json_whitespace(fragment, pos);
		if (pos < fragment.size() && fragment[pos] == ',') {
			++pos;
			continue;
		}
		if (pos < fragment.size() && fragment[pos] == ']') {
			return;
		}
		diag::error_and_throw(
		    "read_json name={} reason=malformed AT Value array",
		    display_name());
	}
}

void JsonReadParser::assign_non_string_value_array(
    DataElement& element, std::string_view fragment) const {
	switch (static_cast<std::uint16_t>(element.vr())) {
	case VR::SS_val: {
		std::vector<std::int16_t> values;
		parse_integral_array_into(fragment, values);
		write_integral_values(element, std::span<const std::int16_t>(values));
		return;
	}
	case VR::US_val: {
		std::vector<std::uint16_t> values;
		parse_integral_array_into(fragment, values);
		write_integral_values(element, std::span<const std::uint16_t>(values));
		return;
	}
	case VR::SL_val: {
		std::vector<std::int32_t> values;
		parse_integral_array_into(fragment, values);
		write_integral_values(element, std::span<const std::int32_t>(values));
		return;
	}
	case VR::UL_val: {
		std::vector<std::uint32_t> values;
		parse_integral_array_into(fragment, values);
		write_integral_values(element, std::span<const std::uint32_t>(values));
		return;
	}
	case VR::SV_val: {
		std::vector<std::int64_t> values;
		parse_integral_array_into(fragment, values);
		write_integral_values(element, std::span<const std::int64_t>(values));
		return;
	}
	case VR::UV_val: {
		std::vector<std::uint64_t> values;
		parse_integral_array_into(fragment, values);
		write_integral_values(element, std::span<const std::uint64_t>(values));
		return;
	}
	case VR::FL_val: {
		std::vector<float> values;
		parse_floating_array_into(fragment, values);
		write_integral_values(element, std::span<const float>(values));
		return;
	}
	case VR::FD_val: {
		std::vector<double> values;
		parse_floating_array_into(fragment, values);
		write_integral_values(element, std::span<const double>(values));
		return;
	}
	case VR::AT_val: {
		std::vector<Tag> values;
		parse_at_array_into(fragment, values);
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
    VR vr, const std::string& uri) const {
	JsonBulkRef ref{};
	ref.kind = JsonBulkTargetKind::element;
	ref.path = std::string(element_path);
	ref.uri = uri;
	ref.vr = vr;
	pending_bulk_data.push_back(std::move(ref));
}

void JsonReadParser::finalize_attribute(DataSet& dataset,
    std::vector<JsonBulkRef>& pending_bulk_data, Tag tag, std::string_view element_path,
    const AttributeInfo& attr) {
	const VR vr = resolve_attribute_vr(tag, attr);
	if (attr.has_value) {
		if (vr.is_sequence()) {
			parse_sequence_value(
			    dataset, tag, pending_bulk_data, element_path,
			    attr.value_offset, attr.value_length);
			return;
		}
		if (vr.is_string() || vr == VR::UN) {
			append_json_stream_string_element(
			    dataset, tag, vr, attr.value_offset, attr.value_length);
			return;
		}
		auto& element = append_leaf_element(dataset, tag, vr);
		const auto local_offset = attr.value_offset - base_offset_;
		assign_non_string_value_array(element, text_.substr(local_offset, attr.value_length));
		return;
	}

	if (attr.inline_binary) {
		auto decoded = base64_decode(*attr.inline_binary);
		if (!decoded) {
			diag::error_and_throw(
			    "read_json name={} tag={} reason=invalid InlineBinary payload",
			    display_name(), tag.to_string());
		}
		auto& element = append_leaf_element(dataset, tag, vr);
		element.set_value_bytes(std::move(*decoded));
		return;
	}

	if (attr.bulk_data_uri) {
		(void)append_leaf_element(dataset, tag, vr);
		append_bulk_ref(pending_bulk_data, element_path, vr, *attr.bulk_data_uri);
		return;
	}

	(void)append_leaf_element(dataset, tag, vr);
}

void JsonReadParser::finalize_dataset(DataSet& dataset) const {
	std::sort(dataset.element_index_.begin(), dataset.element_index_.end(),
	    [](const ElementRef& lhs, const ElementRef& rhs) {
		    return lhs.tag.value() < rhs.tag.value();
	    });
	for (std::size_t i = 1; i < dataset.element_index_.size(); ++i) {
		if (dataset.element_index_[i - 1].tag.value() ==
		    dataset.element_index_[i].tag.value()) {
			diag::error_and_throw(
			    "read_json name={} tag={} reason=duplicate tag in one dataset is not yet supported",
			    display_name(), dataset.element_index_[i].tag.to_string());
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
	std::vector<JsonBulkRef> expanded;
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
					frame_ref.uri = fmt::format("{}/{}", parsed_list->base_uri, frame_index + 1u);
					frame_ref.vr = ref.vr;
					apply_bulk_ref_metadata(frame_ref, transfer_syntax);
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
				frame_ref.vr = ref.vr;
				apply_bulk_ref_metadata(frame_ref, transfer_syntax);
				expanded.push_back(std::move(frame_ref));
				continue;
			}
			const auto number_of_frames =
			    static_cast<std::size_t>(
			        file.dataset().get_value<long>("NumberOfFrames").value_or(1));
			for (std::size_t i = 0; i < std::max<std::size_t>(number_of_frames, 1u); ++i) {
				JsonBulkRef frame_ref{};
				frame_ref.kind = JsonBulkTargetKind::pixel_frame;
				frame_ref.path = ref.path;
				frame_ref.frame_index = i;
				frame_ref.uri = frame_bulk_uri(ref.uri, i);
				frame_ref.vr = ref.vr;
				apply_bulk_ref_metadata(frame_ref, transfer_syntax);
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

JsonReadResult read_json_from_shared(
    std::string name, std::shared_ptr<const std::vector<std::uint8_t>> bytes,
    JsonReadOptions options) {
	JsonReadParser parser(std::move(name), std::move(bytes), options);
	return parser.parse();
}

bool DicomFile::set_bulk_data(const JsonBulkRef& ref, std::span<const std::uint8_t> bytes) {
	switch (ref.kind) {
	case JsonBulkTargetKind::element: {
		DataElement& element = ref.vr != VR::None ? ensure_dataelement(ref.path, ref.vr)
		                                          : ensure_dataelement(ref.path);
		element.set_value_bytes(bytes);
		return true;
	}
	case JsonBulkTargetKind::pixel_frame: {
		if (!ref.path.empty() && ref.path != "7FE00010" && ref.path != "PixelData") {
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
	auto bytes = std::make_shared<std::vector<std::uint8_t>>(data, data + size);
	return read_json_from_shared("<memory>", std::move(bytes), options);
}

JsonReadResult read_json(const std::string& name, const std::uint8_t* data,
    std::size_t size, JsonReadOptions options) {
	auto bytes = std::make_shared<std::vector<std::uint8_t>>(data, data + size);
	return read_json_from_shared(name, std::move(bytes), options);
}

JsonReadResult read_json(
    std::string name, std::vector<std::uint8_t>&& buffer, JsonReadOptions options) {
	auto bytes =
	    std::static_pointer_cast<const std::vector<std::uint8_t>>(
	        std::make_shared<std::vector<std::uint8_t>>(std::move(buffer)));
	return read_json_from_shared(std::move(name), std::move(bytes), options);
}

}  // namespace dicom
