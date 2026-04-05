#include <dicom.h>
#include <dicom_endian.h>
#include "charset/charset_decode.hpp"
#include "charset/charset_detail.hpp"
#include "charset/charset_mutation.hpp"
#include "charset/text_validation.hpp"
#include <diagnostics.h>
#include <yyjson.h>

#include <cctype>
#include <charconv>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <fmt/format.h>
#include <limits>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <vector>
#include <instream.h>

namespace dicom {

namespace {

[[nodiscard]] inline yyjson_val* yyjson_mut(const yyjson_val* value) noexcept {
	return const_cast<yyjson_val*>(value);
}

inline std::string_view trim(std::string_view s) {
	while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
	while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
	return s;
}

[[nodiscard]] std::string_view tidy_fn_name(
    std::source_location location) noexcept {
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
	if (pos < text.size() && (text[pos] == '-' || text[pos] == '+')) {
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
		if (pos < text.size() && (text[pos] == '-' || text[pos] == '+')) {
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

[[nodiscard]] bool skip_json_literal(std::string_view text, std::size_t& pos, std::string_view literal) {
	skip_json_whitespace(text, pos);
	if (text.substr(pos, literal.size()) != literal) {
		return false;
	}
	pos += literal.size();
	return true;
}

[[nodiscard]] bool skip_json_value(std::string_view text, std::size_t& pos);

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

[[nodiscard]] std::optional<std::string> parse_json_person_name_object(
    std::string_view text, std::size_t& pos) {
	skip_json_whitespace(text, pos);
	if (pos >= text.size() || text[pos] != '{') {
		return std::nullopt;
	}
	++pos;
	bool have_alphabetic = false;
	bool have_ideographic = false;
	bool have_phonetic = false;
	std::string alphabetic;
	std::string ideographic;
	std::string phonetic;
	skip_json_whitespace(text, pos);
	if (pos < text.size() && text[pos] == '}') {
		++pos;
		return std::string{};
	}
	while (pos < text.size()) {
		JsonStringToken key_token{};
		if (!parse_json_string_token(text, pos, key_token)) {
			return std::nullopt;
		}
		auto key = decode_json_string_token(text, key_token);
		if (!key) {
			return std::nullopt;
		}
		skip_json_whitespace(text, pos);
		if (pos >= text.size() || text[pos] != ':') {
			return std::nullopt;
		}
		++pos;
		skip_json_whitespace(text, pos);
		if (*key == "Alphabetic" || *key == "Ideographic" || *key == "Phonetic") {
			JsonStringToken value_token{};
			if (!parse_json_string_token(text, pos, value_token)) {
				return std::nullopt;
			}
			auto value = decode_json_string_token(text, value_token);
			if (!value) {
				return std::nullopt;
			}
			if (*key == "Alphabetic") {
				have_alphabetic = true;
				alphabetic = std::move(*value);
			} else if (*key == "Ideographic") {
				have_ideographic = true;
				ideographic = std::move(*value);
			} else {
				have_phonetic = true;
				phonetic = std::move(*value);
			}
		} else if (!skip_json_value(text, pos)) {
			return std::nullopt;
		}
		skip_json_whitespace(text, pos);
		if (pos < text.size() && text[pos] == ',') {
			++pos;
			continue;
		}
		if (pos < text.size() && text[pos] == '}') {
			++pos;
			break;
		}
		return std::nullopt;
	}

	std::string out;
	if (have_alphabetic) {
		out += alphabetic;
	}
	if (have_ideographic || have_phonetic) {
		out.push_back('=');
		if (have_ideographic) {
			out += ideographic;
		}
	}
	if (have_phonetic) {
		out.push_back('=');
		out += phonetic;
	}
	return out;
}

[[nodiscard]] std::optional<std::vector<std::string>> parse_json_utf8_values_from_fragment(
    const DataElement& element, std::string_view fragment) {
	std::size_t pos = 0;
	skip_json_whitespace(fragment, pos);
	if (pos >= fragment.size() || fragment[pos] != '[') {
		return std::nullopt;
	}
	++pos;
	std::vector<std::string> values;
	skip_json_whitespace(fragment, pos);
	if (pos < fragment.size() && fragment[pos] == ']') {
		++pos;
		return values;
	}

	while (pos < fragment.size()) {
		skip_json_whitespace(fragment, pos);
		if (skip_json_literal(fragment, pos, "null")) {
			values.emplace_back();
		} else if (element.vr() == VR::PN) {
			auto pn = parse_json_person_name_object(fragment, pos);
			if (!pn) {
				return std::nullopt;
			}
			if (!charset::validate_utf8(*pn)) {
				return std::nullopt;
			}
			values.push_back(std::move(*pn));
		} else if (pos < fragment.size() && fragment[pos] == '"') {
			JsonStringToken token{};
			if (!parse_json_string_token(fragment, pos, token)) {
				return std::nullopt;
			}
			auto decoded = decode_json_string_token(fragment, token);
			if (!decoded) {
				return std::nullopt;
			}
			if (!charset::validate_utf8(*decoded)) {
				return std::nullopt;
			}
			values.push_back(std::move(*decoded));
		} else if ((element.vr() == VR::DS || element.vr() == VR::IS || element.vr() == VR::UN) &&
		    pos < fragment.size()) {
			std::string_view number{};
			if (!skip_json_number_token(fragment, pos, number)) {
				return std::nullopt;
			}
			values.emplace_back(number);
		} else {
			return std::nullopt;
		}

		skip_json_whitespace(fragment, pos);
		if (pos < fragment.size() && fragment[pos] == ',') {
			++pos;
			continue;
		}
		if (pos < fragment.size() && fragment[pos] == ']') {
			++pos;
			return values;
		}
		return std::nullopt;
	}
	return std::nullopt;
}

[[nodiscard]] std::optional<std::string> parse_json_tree_person_name_object(
    const yyjson_val* object_value) {
	if (!object_value || !yyjson_is_obj(yyjson_mut(object_value))) {
		return std::nullopt;
	}

	bool have_alphabetic = false;
	bool have_ideographic = false;
	bool have_phonetic = false;
	std::string alphabetic;
	std::string ideographic;
	std::string phonetic;

	yyjson_obj_iter iter = yyjson_obj_iter_with(yyjson_mut(object_value));
	yyjson_val* key = nullptr;
	while ((key = yyjson_obj_iter_next(&iter))) {
		auto* value = yyjson_obj_iter_get_val(key);
		if (!yyjson_is_str(key) || !yyjson_is_str(value)) {
			continue;
		}
		const std::string_view key_text{yyjson_get_str(key), yyjson_get_len(key)};
		const std::string_view value_text{yyjson_get_str(value), yyjson_get_len(value)};
		if (key_text == "Alphabetic") {
			have_alphabetic = true;
			alphabetic.assign(value_text);
		} else if (key_text == "Ideographic") {
			have_ideographic = true;
			ideographic.assign(value_text);
		} else if (key_text == "Phonetic") {
			have_phonetic = true;
			phonetic.assign(value_text);
		}
	}

	std::string out;
	if (have_alphabetic) {
		out += alphabetic;
	}
	if (have_ideographic || have_phonetic) {
		out.push_back('=');
		if (have_ideographic) {
			out += ideographic;
		}
	}
	if (have_phonetic) {
		out.push_back('=');
		out += phonetic;
	}
	return out;
}

[[nodiscard]] std::optional<std::string> yyjson_scalar_to_text(const yyjson_val* value) {
	if (!value) {
		return std::nullopt;
	}
	if (yyjson_is_str(yyjson_mut(value))) {
		return std::string(yyjson_get_str(yyjson_mut(value)), yyjson_get_len(yyjson_mut(value)));
	}
	if (yyjson_is_num(yyjson_mut(value))) {
		char* text = yyjson_val_write(value, 0, nullptr);
		if (!text) {
			return std::nullopt;
		}
		std::string out{text};
		std::free(text);
		return out;
	}
	return std::nullopt;
}

template <typename IntT>
[[nodiscard]] std::optional<IntT> yyjson_integral_to_value(const yyjson_val* value) {
	if (!value) {
		return std::nullopt;
	}
	auto* item = yyjson_mut(value);
	if (!yyjson_is_num(item) || yyjson_is_real(item)) {
		return std::nullopt;
	}
	if constexpr (std::is_signed_v<IntT>) {
		if (yyjson_is_uint(item)) {
			const auto raw = yyjson_get_uint(item);
			if (raw > static_cast<std::uint64_t>(std::numeric_limits<IntT>::max())) {
				return std::nullopt;
			}
			return static_cast<IntT>(raw);
		}
		const auto raw = yyjson_get_sint(item);
		if (raw < static_cast<std::int64_t>(std::numeric_limits<IntT>::min()) ||
		    raw > static_cast<std::int64_t>(std::numeric_limits<IntT>::max())) {
			return std::nullopt;
		}
		return static_cast<IntT>(raw);
	}
	if (yyjson_is_uint(item)) {
		const auto raw = yyjson_get_uint(item);
		if (raw > static_cast<std::uint64_t>(std::numeric_limits<IntT>::max())) {
			return std::nullopt;
		}
		return static_cast<IntT>(raw);
	}
	const auto raw = yyjson_get_sint(item);
	if (raw < 0 ||
	    static_cast<std::uint64_t>(raw) > static_cast<std::uint64_t>(std::numeric_limits<IntT>::max())) {
		return std::nullopt;
	}
	return static_cast<IntT>(raw);
}

template <typename FloatT>
[[nodiscard]] std::optional<FloatT> yyjson_floating_to_value(const yyjson_val* value) {
	if (!value) {
		return std::nullopt;
	}
	auto* item = yyjson_mut(value);
	if (!yyjson_is_num(item)) {
		return std::nullopt;
	}
	if (yyjson_is_real(item)) {
		return static_cast<FloatT>(yyjson_get_real(item));
	}
	if (yyjson_is_uint(item)) {
		return static_cast<FloatT>(yyjson_get_uint(item));
	}
	return static_cast<FloatT>(yyjson_get_sint(item));
}

template <typename T, typename ConvertFn>
[[nodiscard]] bool materialize_json_tree_numeric_array(
    DataElement& element, const yyjson_val* value, ConvertFn&& convert) {
	if (!value || !yyjson_is_arr(yyjson_mut(value))) {
		return false;
	}
	const auto count = yyjson_arr_size(yyjson_mut(value));
	element.reserve_value_bytes(count * sizeof(T));
	auto dst = element.value_span();
	auto* out = const_cast<std::uint8_t*>(dst.data());
	yyjson_arr_iter iter = yyjson_arr_iter_with(yyjson_mut(value));
	yyjson_val* item = nullptr;
	std::size_t index = 0;
	while ((item = yyjson_arr_iter_next(&iter))) {
		auto parsed = convert(item);
		if (!parsed) {
			return false;
		}
		if constexpr (std::is_floating_point_v<T>) {
			using Bits = std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>;
			endian::store_le<Bits>(
			    out + index * sizeof(T), std::bit_cast<Bits>(*parsed));
		} else {
			endian::store_le<T>(out + index * sizeof(T), *parsed);
		}
		++index;
	}
	return true;
}

[[nodiscard]] bool collect_json_tree_string_views(const yyjson_val* value,
    std::vector<std::string_view>& out_views, bool* out_all_ascii = nullptr) {
	if (!value || !yyjson_is_arr(yyjson_mut(value))) {
		return false;
	}
	out_views.clear();
	out_views.reserve(yyjson_arr_size(yyjson_mut(value)));
	bool all_ascii = true;
	yyjson_arr_iter iter = yyjson_arr_iter_with(yyjson_mut(value));
	yyjson_val* item = nullptr;
	while ((item = yyjson_arr_iter_next(&iter))) {
		if (yyjson_is_null(item)) {
			out_views.emplace_back();
			continue;
		}
		if (!yyjson_is_str(item)) {
			return false;
		}
		const std::string_view text{yyjson_get_str(item), yyjson_get_len(item)};
		if (!charset::validate_ascii(text)) {
			all_ascii = false;
		}
		out_views.push_back(text);
	}
	if (out_all_ascii) {
		*out_all_ascii = all_ascii;
	}
	return true;
}

[[nodiscard]] bool materialize_json_tree_string_views(
    DataElement& element, const yyjson_val* value) {
	std::vector<std::string_view> views;
	bool all_ascii = true;
	if (!collect_json_tree_string_views(value, views, &all_ascii)) {
		return false;
	}
	if (element.vr() == VR::UN) {
		std::string raw_text;
		std::size_t total_length = views.empty() ? 0u : views.size() - 1u;
		for (const auto view : views) {
			total_length += view.size();
		}
		raw_text.reserve(total_length);
		for (std::size_t i = 0; i < views.size(); ++i) {
			if (i != 0u) {
				raw_text.push_back('\\');
			}
			raw_text.append(views[i].data(), views[i].size());
		}
		const auto* ptr = reinterpret_cast<const std::uint8_t*>(raw_text.data());
		element.set_value_bytes(std::span<const std::uint8_t>(ptr, raw_text.size()));
		return true;
	}
	if (!element.vr().uses_specific_character_set() || all_ascii) {
		return element.from_string_views(views);
	}
	const auto* root = element.parent() ? element.parent()->root_dataset() : nullptr;
	const auto charset_errors =
	    root ? root->json_read_charset_errors_policy() : CharsetEncodeErrorPolicy::strict;
	return element.from_utf8_views(views, charset_errors, nullptr);
}

[[nodiscard]] bool append_json_tree_number_text(
    std::string& out, const yyjson_val* value, VR vr) {
	if (!value) {
		return false;
	}
	auto* item = yyjson_mut(value);
	if (yyjson_is_null(item)) {
		return true;
	}
	if (yyjson_is_str(item)) {
		out.append(yyjson_get_str(item), yyjson_get_len(item));
		return true;
	}
	if (!yyjson_is_num(item)) {
		return false;
	}
	char buffer[64];
	if (yyjson_is_real(item)) {
		if (vr == VR::IS) {
			return false;
		}
		const auto number = yyjson_get_real(item);
		const auto result = std::to_chars(
		    buffer, buffer + sizeof(buffer), number, std::chars_format::general);
		if (result.ec != std::errc()) {
			return false;
		}
		out.append(buffer, result.ptr);
		return true;
	}
	if (yyjson_is_uint(item)) {
		const auto result =
		    std::to_chars(buffer, buffer + sizeof(buffer), yyjson_get_uint(item), 10);
		if (result.ec != std::errc()) {
			return false;
		}
		out.append(buffer, result.ptr);
		return true;
	}
	const auto result =
	    std::to_chars(buffer, buffer + sizeof(buffer), yyjson_get_sint(item), 10);
	if (result.ec != std::errc()) {
		return false;
	}
	out.append(buffer, result.ptr);
	return true;
}

[[nodiscard]] bool materialize_json_tree_delimited_text(
    DataElement& element, const yyjson_val* value) {
	if (!value || !yyjson_is_arr(yyjson_mut(value))) {
		return false;
	}
	const auto count = yyjson_arr_size(yyjson_mut(value));
	std::string text;
	text.reserve(count * 24);
	yyjson_arr_iter iter = yyjson_arr_iter_with(yyjson_mut(value));
	yyjson_val* item = nullptr;
	std::size_t index = 0;
	while ((item = yyjson_arr_iter_next(&iter))) {
		if (index++ != 0u) {
			text.push_back('\\');
		}
		if (!append_json_tree_number_text(text, item, element.vr())) {
			return false;
		}
	}
	const auto* ptr = reinterpret_cast<const std::uint8_t*>(text.data());
	element.set_value_bytes(std::span<const std::uint8_t>(ptr, text.size()));
	return true;
}

[[nodiscard]] bool materialize_json_tree_direct(
    DataElement& element, const yyjson_val* value) {
	switch (static_cast<std::uint16_t>(element.vr())) {
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
	case VR::UN_val:
		return materialize_json_tree_string_views(element, value);
	case VR::DS_val:
	case VR::IS_val:
		return materialize_json_tree_delimited_text(element, value);
	case VR::SS_val:
		return materialize_json_tree_numeric_array<std::int16_t>(
		    element, value, [](const yyjson_val* item) {
			    return yyjson_integral_to_value<std::int16_t>(item);
		    });
	case VR::US_val:
		return materialize_json_tree_numeric_array<std::uint16_t>(
		    element, value, [](const yyjson_val* item) {
			    return yyjson_integral_to_value<std::uint16_t>(item);
		    });
	case VR::SL_val:
		return materialize_json_tree_numeric_array<std::int32_t>(
		    element, value, [](const yyjson_val* item) {
			    return yyjson_integral_to_value<std::int32_t>(item);
		    });
	case VR::UL_val:
		return materialize_json_tree_numeric_array<std::uint32_t>(
		    element, value, [](const yyjson_val* item) {
			    return yyjson_integral_to_value<std::uint32_t>(item);
		    });
	case VR::SV_val:
		return materialize_json_tree_numeric_array<std::int64_t>(
		    element, value, [](const yyjson_val* item) {
			    return yyjson_integral_to_value<std::int64_t>(item);
		    });
	case VR::UV_val:
		return materialize_json_tree_numeric_array<std::uint64_t>(
		    element, value, [](const yyjson_val* item) {
			    return yyjson_integral_to_value<std::uint64_t>(item);
		    });
	case VR::FL_val:
		return materialize_json_tree_numeric_array<float>(
		    element, value, [](const yyjson_val* item) {
			    return yyjson_floating_to_value<float>(item);
		    });
	case VR::FD_val:
		return materialize_json_tree_numeric_array<double>(
		    element, value, [](const yyjson_val* item) {
			    return yyjson_floating_to_value<double>(item);
		    });
	default:
		return false;
	}
}

template <typename T>
std::optional<std::vector<T>> parse_json_integral_array(std::string_view fragment);
template <typename T>
std::optional<std::vector<T>> parse_json_floating_array(std::string_view fragment);
std::optional<std::vector<Tag>> parse_json_at_array(std::string_view fragment);
template <typename T>
void write_numeric_values_le(DataElement& element, std::span<const T> values);

[[nodiscard]] bool materialize_json_element_from_fragment(
    DataElement& element, std::string_view fragment) {
	switch (static_cast<std::uint16_t>(element.vr())) {
	case VR::AT_val: {
		auto values = parse_json_at_array(fragment);
		if (values && element.from_tag_vector(*values)) {
			return true;
		}
		return false;
	}
	case VR::SS_val: {
		auto values = parse_json_integral_array<std::int16_t>(fragment);
		if (values) {
			write_numeric_values_le(element, std::span<const std::int16_t>(*values));
			return true;
		}
		return false;
	}
	case VR::US_val: {
		auto values = parse_json_integral_array<std::uint16_t>(fragment);
		if (values) {
			write_numeric_values_le(element, std::span<const std::uint16_t>(*values));
			return true;
		}
		return false;
	}
	case VR::SL_val: {
		auto values = parse_json_integral_array<std::int32_t>(fragment);
		if (values) {
			write_numeric_values_le(element, std::span<const std::int32_t>(*values));
			return true;
		}
		return false;
	}
	case VR::UL_val: {
		auto values = parse_json_integral_array<std::uint32_t>(fragment);
		if (values) {
			write_numeric_values_le(element, std::span<const std::uint32_t>(*values));
			return true;
		}
		return false;
	}
	case VR::SV_val: {
		auto values = parse_json_integral_array<std::int64_t>(fragment);
		if (values) {
			write_numeric_values_le(element, std::span<const std::int64_t>(*values));
			return true;
		}
		return false;
	}
	case VR::UV_val: {
		auto values = parse_json_integral_array<std::uint64_t>(fragment);
		if (values) {
			write_numeric_values_le(element, std::span<const std::uint64_t>(*values));
			return true;
		}
		return false;
	}
	case VR::FL_val: {
		auto values = parse_json_floating_array<float>(fragment);
		if (values) {
			write_numeric_values_le(element, std::span<const float>(*values));
			return true;
		}
		return false;
	}
	case VR::FD_val: {
		auto values = parse_json_floating_array<double>(fragment);
		if (values) {
			write_numeric_values_le(element, std::span<const double>(*values));
			return true;
		}
		return false;
	}
	default:
		break;
	}

	auto values = parse_json_utf8_values_from_fragment(element, fragment);
	if (!values) {
		return false;
	}

	std::vector<std::string_view> value_views;
	value_views.reserve(values->size());
	for (const auto& value : *values) {
		value_views.push_back(value);
	}
	if (element.vr() == VR::UN) {
		std::string raw_text;
		std::size_t total_length = values->empty() ? 0u : values->size() - 1u;
		for (const auto& value : *values) {
			total_length += value.size();
		}
		raw_text.reserve(total_length);
		for (std::size_t i = 0; i < values->size(); ++i) {
			if (i != 0u) {
				raw_text.push_back('\\');
			}
			raw_text += (*values)[i];
		}
		const auto* ptr = reinterpret_cast<const std::uint8_t*>(raw_text.data());
		element.set_value_bytes(std::span<const std::uint8_t>(ptr, raw_text.size()));
		return true;
	}

	const auto* root = element.parent() ? element.parent()->root_dataset() : nullptr;
	const auto charset_errors =
	    root ? root->json_read_charset_errors_policy() : CharsetEncodeErrorPolicy::strict;
	return element.vr().uses_specific_character_set()
	    ? element.from_utf8_views(value_views, charset_errors, nullptr)
	    : element.from_string_views(value_views);
}

bool report_from_assignment_failure(const DataElement& element, std::string_view reason,
    std::source_location location = std::source_location::current()) {
	diag::error("{} tag={} vr={} reason={}",
	    tidy_fn_name(location), element.tag().to_string(), element.vr().str(), reason);
	return false;
}

bool raw_string_splitting_is_safe(const DataElement& element) {
	if (!element.vr().uses_specific_character_set()) {
		return true;
	}
	const auto* parent = element.parent();
	if (!parent) {
		return true;
	}
	auto parsed = charset::detail::parse_dataset_charset(*parent, nullptr);
	if (!parsed) {
		return true;
	}
	if (parsed->is_multi_term()) {
		return false;
	}
	switch (parsed->primary) {
	case SpecificCharacterSet::GBK:
	case SpecificCharacterSet::GB18030:
	case SpecificCharacterSet::ISO_2022_IR_149:
	case SpecificCharacterSet::ISO_2022_IR_58:
	case SpecificCharacterSet::ISO_2022_IR_87:
	case SpecificCharacterSet::ISO_2022_IR_159:
		return false;
	default:
		return true;
	}
}

template <typename T>
std::optional<std::vector<T>> load_numeric_vector(const DataElement& elem) {
    static_assert(std::is_integral_v<T> || std::is_floating_point_v<T>, "numeric load requires arithmetic type");
	const auto span = elem.value_span();
	if (span.empty()) return std::vector<T>{};
	if (span.size() % sizeof(T) != 0) return std::nullopt;

	const auto count = span.size() / sizeof(T);
	std::vector<T> out;
	out.reserve(count);
	for (std::size_t i = 0; i < count; ++i) {
		const auto* ptr = span.data() + i * sizeof(T);
        if constexpr (std::is_floating_point_v<T>) {
            using Bits = std::conditional_t<sizeof(T)==4, std::uint32_t, std::uint64_t>;
            const Bits bits = endian::load_le<Bits>(ptr);
            out.push_back(std::bit_cast<T>(bits));
        } else {
		    out.push_back(endian::load_le<T>(ptr));
        }
	}
	return out;
}

template <typename Parser>
std::optional<std::vector<typename Parser::result_type>> parse_string_numbers(const DataElement& elem,
    Parser parser) {
	const auto span = elem.value_span();
	if (span.empty()) return std::vector<typename Parser::result_type>{};
	std::string_view raw{reinterpret_cast<const char*>(span.data()), span.size()};
	std::vector<typename Parser::result_type> out;
	std::size_t start = 0;
	while (start <= raw.size()) {
		const auto pos = raw.find_first_of("\\/", start);
		const auto len = (pos == std::string_view::npos) ? raw.size() - start : pos - start;
		auto token = trim(raw.substr(start, len));
		if (!token.empty()) {
			auto parsed = parser(token);
			if (!parsed.has_value()) return std::nullopt;
			out.push_back(*parsed);
		}
		if (pos == std::string_view::npos) break;
		start = pos + 1;
	}
	if (out.empty()) return std::nullopt;
	return out;
}

template <typename T, typename Source>
bool assign_integral_from_integer(DataElement& element, Source value) {
	static_assert(std::is_integral_v<T>, "assign_integral_from_integer requires integral target type");
	static_assert(
	    std::is_integral_v<Source>,
	    "assign_integral_from_integer requires integral source type");

	T encoded{};
	if constexpr (std::is_signed_v<T>) {
		if constexpr (std::is_signed_v<Source>) {
			const auto signed_value = static_cast<std::intmax_t>(value);
			if (signed_value < static_cast<std::intmax_t>(std::numeric_limits<T>::min()) ||
			    signed_value > static_cast<std::intmax_t>(std::numeric_limits<T>::max())) {
				return false;
			}
		} else {
			const auto unsigned_value = static_cast<std::uintmax_t>(value);
			if (unsigned_value > static_cast<std::uintmax_t>(std::numeric_limits<T>::max())) {
				return false;
			}
		}
		encoded = static_cast<T>(value);
	} else {
		if constexpr (std::is_signed_v<Source>) {
			if (value < 0) {
				return false;
			}
		}
		const auto unsigned_value = static_cast<std::uintmax_t>(value);
		if (unsigned_value > static_cast<std::uintmax_t>(std::numeric_limits<T>::max())) {
			return false;
		}
		encoded = static_cast<T>(unsigned_value);
	}

	element.reserve_value_bytes(sizeof(T));
	auto dst = element.value_span();
	endian::store_le<T>(const_cast<std::uint8_t*>(dst.data()), encoded);
	return true;
}

void store_padded_value_bytes(DataElement& element, std::span<const std::uint8_t> bytes) {
	const bool needs_padding = VR::pad_to_even() && ((bytes.size() & 1u) != 0u);
	const std::size_t stored_length = bytes.size() + (needs_padding ? 1u : 0u);
	element.reserve_value_bytes(stored_length);
	auto dst = element.value_span();
	auto* writable = const_cast<std::uint8_t*>(dst.data());
	if (!bytes.empty()) {
		std::memcpy(writable, bytes.data(), bytes.size());
	}
	if (needs_padding) {
		writable[bytes.size()] = element.vr().padding_byte();
	}
}

template <typename Source>
bool assign_integer_string_from_value(DataElement& element, Source value) {
	static_assert(
	    std::is_integral_v<Source>,
	    "assign_integer_string_from_value requires integral source type");
	const std::string text = std::to_string(value);
	const auto* ptr = reinterpret_cast<const std::uint8_t*>(text.data());
	store_padded_value_bytes(element, std::span<const std::uint8_t>(ptr, text.size()));
	return true;
}

template <typename T, typename Source>
bool assign_integral_vector_from_integer(
    DataElement& element, std::span<const Source> values) {
	static_assert(
	    std::is_integral_v<T>,
	    "assign_integral_vector_from_integer requires integral target type");
	static_assert(
	    std::is_integral_v<Source>,
	    "assign_integral_vector_from_integer requires integral source type");

	if (values.empty()) {
		element.reserve_value_bytes(0);
		return true;
	}
	if (values.size() > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
		return false;
	}

	const std::size_t total_bytes = values.size() * sizeof(T);
	element.reserve_value_bytes(total_bytes);
	auto dst = element.value_span();
	auto* writable = const_cast<std::uint8_t*>(dst.data());
	for (std::size_t i = 0; i < values.size(); ++i) {
		T encoded{};
		const Source value = values[i];
		if constexpr (std::is_signed_v<T>) {
			if constexpr (std::is_signed_v<Source>) {
				const auto signed_value = static_cast<std::intmax_t>(value);
				if (signed_value < static_cast<std::intmax_t>(std::numeric_limits<T>::min()) ||
				    signed_value > static_cast<std::intmax_t>(std::numeric_limits<T>::max())) {
					return false;
				}
			} else {
				const auto unsigned_value = static_cast<std::uintmax_t>(value);
				if (unsigned_value > static_cast<std::uintmax_t>(std::numeric_limits<T>::max())) {
					return false;
				}
			}
			encoded = static_cast<T>(value);
		} else {
			if constexpr (std::is_signed_v<Source>) {
				if (value < 0) {
					return false;
				}
			}
			const auto unsigned_value = static_cast<std::uintmax_t>(value);
			if (unsigned_value > static_cast<std::uintmax_t>(std::numeric_limits<T>::max())) {
				return false;
			}
			encoded = static_cast<T>(unsigned_value);
		}
		endian::store_le<T>(writable + (i * sizeof(T)), encoded);
	}
	return true;
}

template <typename Source>
bool assign_integer_string_from_values(DataElement& element, std::span<const Source> values) {
	static_assert(
	    std::is_integral_v<Source>,
	    "assign_integer_string_from_values requires integral source type");

	if (values.empty()) {
		element.reserve_value_bytes(0);
		return true;
	}

	std::string text;
	text.reserve(values.size() * 21);
	for (std::size_t i = 0; i < values.size(); ++i) {
		if (i != 0) {
			text.push_back('\\');
		}
		text += std::to_string(values[i]);
	}
	const auto* ptr = reinterpret_cast<const std::uint8_t*>(text.data());
	store_padded_value_bytes(element, std::span<const std::uint8_t>(ptr, text.size()));
	return true;
}

template <typename T>
bool assign_floating_from_double(DataElement& element, double value) {
	static_assert(
	    std::is_floating_point_v<T>,
	    "assign_floating_from_double requires floating-point target type");
	if (!std::isfinite(value)) {
		return false;
	}
	if constexpr (sizeof(T) < sizeof(double)) {
		constexpr double kMax = static_cast<double>(std::numeric_limits<T>::max());
		if (value < -kMax || value > kMax) {
			return false;
		}
	}

	const auto encoded = static_cast<T>(value);
	if (!std::isfinite(encoded)) {
		return false;
	}

	using Bits = std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>;
	const Bits bits = std::bit_cast<Bits>(encoded);
	element.reserve_value_bytes(sizeof(T));
	auto dst = element.value_span();
	endian::store_le<Bits>(const_cast<std::uint8_t*>(dst.data()), bits);
	return true;
}

template <typename T>
bool assign_floating_vector_from_double(
    DataElement& element, std::span<const double> values) {
	static_assert(
	    std::is_floating_point_v<T>,
	    "assign_floating_vector_from_double requires floating-point target type");

	if (values.empty()) {
		element.reserve_value_bytes(0);
		return true;
	}
	if (values.size() > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
		return false;
	}

	const std::size_t total_bytes = values.size() * sizeof(T);
	element.reserve_value_bytes(total_bytes);
	auto dst = element.value_span();
	auto* writable = const_cast<std::uint8_t*>(dst.data());
	using Bits = std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>;
	for (std::size_t i = 0; i < values.size(); ++i) {
		const double value = values[i];
		if (!std::isfinite(value)) {
			return false;
		}
		if constexpr (sizeof(T) < sizeof(double)) {
			constexpr double kMax = static_cast<double>(std::numeric_limits<T>::max());
			if (value < -kMax || value > kMax) {
				return false;
			}
		}
			const auto encoded = static_cast<T>(value);
			if (!std::isfinite(encoded)) {
				return false;
			}
			const Bits bits = std::bit_cast<Bits>(encoded);
			endian::store_le<Bits>(writable + (i * sizeof(T)), bits);
		}
	return true;
}

bool assign_decimal_string_from_double(DataElement& element, double value) {
	if (!std::isfinite(value)) {
		return false;
	}
	const std::string text = fmt::format("{:.17g}", value);
	const auto* ptr = reinterpret_cast<const std::uint8_t*>(text.data());
	store_padded_value_bytes(element, std::span<const std::uint8_t>(ptr, text.size()));
	return true;
}

bool assign_decimal_string_vector_from_double(
    DataElement& element, std::span<const double> values) {
	if (values.empty()) {
		element.reserve_value_bytes(0);
		return true;
	}

	std::string text;
	text.reserve(values.size() * 24);
	for (std::size_t i = 0; i < values.size(); ++i) {
		const double value = values[i];
		if (!std::isfinite(value)) {
			return false;
		}
		if (i != 0) {
			text.push_back('\\');
		}
		text += fmt::format("{:.17g}", value);
	}
	const auto* ptr = reinterpret_cast<const std::uint8_t*>(text.data());
	store_padded_value_bytes(element, std::span<const std::uint8_t>(ptr, text.size()));
	return true;
}

bool assign_uid_string(DataElement& element, std::string_view uid_text) {
	if (element.vr() != dicom::VR::UI) {
		return false;
	}

	std::string normalized = uid::normalize_uid_text(uid_text);
	if (!uid::is_valid_uid_text_strict(normalized)) {
		return false;
	}

	const auto* ptr = reinterpret_cast<const std::uint8_t*>(normalized.data());
	store_padded_value_bytes(
	    element, std::span<const std::uint8_t>(ptr, normalized.size()));
	return true;
}

struct IntParser {
	using result_type = long long;
	std::optional<long long> operator()(std::string_view s) const {
		long long value = 0;
		auto res = std::from_chars(s.data(), s.data() + s.size(), value, 10);
		if (res.ec == std::errc()) return value;
		return std::nullopt;
	}
};

struct DoubleParser {
	using result_type = double;
	std::optional<double> operator()(std::string_view s) const {
		try {
			size_t idx = 0;
			double v = std::stod(std::string{s}, &idx);
			if (idx != s.size()) return std::nullopt;
			return v;
		} catch (...) {
			return std::nullopt;
		}
	}
};

std::optional<std::vector<long>> make_long_vector_from_numbers(const std::vector<long long>& src) {
	std::vector<long> out;
	out.reserve(src.size());
	for (auto v : src) {
		if (v < std::numeric_limits<long>::min() || v > std::numeric_limits<long>::max()) {
			return std::nullopt;
		}
	out.push_back(static_cast<long>(v));
	}
	return out;
}

std::optional<std::vector<int>> make_int_vector_from_numbers(const std::vector<long long>& src) {
	std::vector<int> out;
	out.reserve(src.size());
	for (auto v : src) {
		if (v < std::numeric_limits<int>::min() || v > std::numeric_limits<int>::max()) {
			return std::nullopt;
		}
		out.push_back(static_cast<int>(v));
	}
	return out;
}

template <typename IntT>
std::optional<IntT> parse_integer_literal(std::string_view token) {
	IntT value = 0;
	const auto result = std::from_chars(token.data(), token.data() + token.size(), value, 10);
	if (result.ec == std::errc() && result.ptr == token.data() + token.size()) {
		return value;
	}
	return std::nullopt;
}

std::optional<double> parse_double_literal(std::string_view token) {
	try {
		size_t idx = 0;
		double value = std::stod(std::string{token}, &idx);
		if (idx == token.size()) {
			return value;
		}
	} catch (...) {
	}
	return std::nullopt;
}

template <typename T>
void write_numeric_values_le(DataElement& element, std::span<const T> values) {
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

template <typename T>
std::optional<std::vector<T>> parse_json_integral_array(std::string_view fragment) {
	std::size_t pos = 0;
	skip_json_whitespace(fragment, pos);
	if (pos >= fragment.size() || fragment[pos] != '[') {
		return std::nullopt;
	}
	++pos;
	skip_json_whitespace(fragment, pos);
	std::vector<T> values;
	if (pos < fragment.size() && fragment[pos] == ']') {
		return values;
	}
	while (pos < fragment.size()) {
		std::string_view token{};
		if (!skip_json_number_token(fragment, pos, token)) {
			return std::nullopt;
		}
		auto parsed = parse_integer_literal<T>(token);
		if (!parsed) {
			return std::nullopt;
		}
		values.push_back(*parsed);
		skip_json_whitespace(fragment, pos);
		if (pos < fragment.size() && fragment[pos] == ',') {
			++pos;
			continue;
		}
		if (pos < fragment.size() && fragment[pos] == ']') {
			return values;
		}
		return std::nullopt;
	}
	return std::nullopt;
}

template <typename T>
std::optional<std::vector<T>> parse_json_floating_array(std::string_view fragment) {
	std::size_t pos = 0;
	skip_json_whitespace(fragment, pos);
	if (pos >= fragment.size() || fragment[pos] != '[') {
		return std::nullopt;
	}
	++pos;
	skip_json_whitespace(fragment, pos);
	std::vector<T> values;
	if (pos < fragment.size() && fragment[pos] == ']') {
		return values;
	}
	while (pos < fragment.size()) {
		std::string_view token{};
		if (!skip_json_number_token(fragment, pos, token)) {
			return std::nullopt;
		}
		auto parsed = parse_double_literal(token);
		if (!parsed) {
			return std::nullopt;
		}
		values.push_back(static_cast<T>(*parsed));
		skip_json_whitespace(fragment, pos);
		if (pos < fragment.size() && fragment[pos] == ',') {
			++pos;
			continue;
		}
		if (pos < fragment.size() && fragment[pos] == ']') {
			return values;
		}
		return std::nullopt;
	}
	return std::nullopt;
}

std::optional<std::vector<Tag>> parse_json_at_array(std::string_view fragment) {
	std::size_t pos = 0;
	skip_json_whitespace(fragment, pos);
	if (pos >= fragment.size() || fragment[pos] != '[') {
		return std::nullopt;
	}
	++pos;
	skip_json_whitespace(fragment, pos);
	std::vector<Tag> values;
	if (pos < fragment.size() && fragment[pos] == ']') {
		return values;
	}
	while (pos < fragment.size()) {
		JsonStringToken token{};
		if (!parse_json_string_token(fragment, pos, token)) {
			return std::nullopt;
		}
		auto tag_text = decode_json_string_token(fragment, token);
		if (!tag_text) {
			return std::nullopt;
		}
		try {
			values.emplace_back(*tag_text);
		} catch (...) {
			return std::nullopt;
		}
		skip_json_whitespace(fragment, pos);
		if (pos < fragment.size() && fragment[pos] == ',') {
			++pos;
			continue;
		}
		if (pos < fragment.size() && fragment[pos] == ']') {
			return values;
		}
		return std::nullopt;
	}
	return std::nullopt;
}

constexpr Tag kTransferSyntaxUidTag{0x0002u, 0x0010u};
constexpr Tag kSopClassUidTag{0x0008u, 0x0016u};

namespace {

constexpr bool is_trim_char(char ch) {
	return ch == ' ' || ch == '\0';
}

inline void trim_leading_spaces(std::string_view& view) {
	while (!view.empty() && is_trim_char(view.front())) {
		view.remove_prefix(1);
	}
}

inline void trim_trailing_spaces(std::string_view& view) {
	while (!view.empty() && is_trim_char(view.back())) {
		view.remove_suffix(1);
	}
}

template <bool TrimLeading>
inline std::string_view to_string_view_apply_trim(std::string_view component) {
	if constexpr (TrimLeading) {
		trim_leading_spaces(component);
	}
	trim_trailing_spaces(component);
	return component;
}

template <bool TrimLeading, bool UseDelim>
inline std::optional<std::string_view> to_string_view_normalize(std::string_view raw) {
	if constexpr (UseDelim) {
		const auto pos = raw.find('\\');
		if (pos != std::string_view::npos) {
			raw = raw.substr(0, pos);
		}
	}
	return to_string_view_apply_trim<TrimLeading>(raw);
}

template <bool TrimLeading, bool UseDelim>
inline std::optional<std::vector<std::string_view>> to_string_views_normalize(std::string_view raw) {
	std::vector<std::string_view> values;
	if constexpr (!UseDelim) {
		values.push_back(to_string_view_apply_trim<TrimLeading>(raw));
		return values;
	}
	size_t start = 0;
	while (start <= raw.size()) {
		const auto next = raw.find('\\', start);
		const auto len = (next == std::string_view::npos) ? (raw.size() - start) : (next - start);
		auto token = raw.substr(start, len);
		values.push_back(to_string_view_apply_trim<TrimLeading>(token));
		if (next == std::string_view::npos) {
			break;
		}
		start = next + 1;
	}
	return values;
}

std::optional<uid::WellKnown> well_known_uid_from_element_value(const DataElement& elem) {
	if (elem.vr() != dicom::VR::UI) {
		return std::nullopt;
	}
	const auto normalized = elem.to_string_view();
	if (!normalized) {
		return std::nullopt;
	}
	return uid::from_value(*normalized);
}

}  // namespace

}  // namespace

std::optional<std::vector<std::string>> DataElement::parse_json_tree_utf8_values() const {
	if (storage_kind_ != StorageKind::json_tree) {
		return std::nullopt;
	}
	const auto* value = reinterpret_cast<const yyjson_val*>(storage_.ptr);
	if (!value || !yyjson_is_arr(yyjson_mut(value))) {
		return std::nullopt;
	}

	std::vector<std::string> values;
	yyjson_arr_iter iter = yyjson_arr_iter_with(yyjson_mut(value));
	yyjson_val* item = nullptr;
	while ((item = yyjson_arr_iter_next(&iter))) {
		if (yyjson_is_null(item)) {
			values.emplace_back();
		} else if (vr_ == VR::PN) {
			auto pn = parse_json_tree_person_name_object(item);
			if (!pn) {
				return std::nullopt;
			}
			values.push_back(std::move(*pn));
		} else if (yyjson_is_str(item)) {
			values.emplace_back(yyjson_get_str(item), yyjson_get_len(item));
		} else if ((vr_ == VR::DS || vr_ == VR::IS || vr_ == VR::UN) && yyjson_is_num(item)) {
			auto number = yyjson_scalar_to_text(item);
			if (!number) {
				return std::nullopt;
			}
			values.push_back(std::move(*number));
		} else {
			return std::nullopt;
		}
	}
	return values;
}

void DataElement::materialize_json_tree() {
	if (storage_kind_ != StorageKind::json_tree) {
		return;
	}
	const auto* value = reinterpret_cast<const yyjson_val*>(storage_.ptr);
	if (!value) {
		diag::error_and_throw(
		    "json_tree tag={} vr={} reason=missing yyjson value tree",
		    tag_.to_string(), vr_.str());
	}
	if (materialize_json_tree_direct(*this, value)) {
		return;
	}
	char* fragment = yyjson_val_write(value, 0, nullptr);
	if (!fragment) {
		diag::error_and_throw(
		    "json_tree tag={} vr={} reason=failed to serialize JSON Value tree",
		    tag_.to_string(), vr_.str());
	}
	const std::string_view fragment_view{fragment, std::strlen(fragment)};
	const bool ok = materialize_json_element_from_fragment(*this, fragment_view);
	std::free(fragment);
	if (!ok) {
		diag::error_and_throw(
		    "json_tree tag={} vr={} reason=failed to materialize raw DICOM bytes from yyjson Value tree",
		    tag_.to_string(), vr_.str());
	}
}

DataElement* NullElement() {
	static DataElement null(Tag(0x0000, 0x0000), VR::None, 0, 0, nullptr);
	return &null;
}

std::span<const std::uint8_t> DataElement::value_span() const {
	switch (storage_kind_) {
	case StorageKind::inline_bytes:
		return std::span<const std::uint8_t>(
		    storage_.inline_bytes, std::min(length_, kInlineStorageBytes));
	case StorageKind::heap:
		if (!storage_.ptr) {
			return {};
		}
		return std::span<const std::uint8_t>(
		    static_cast<const std::uint8_t*>(storage_.ptr), length_);
	case StorageKind::owned_bytes:
		if (!storage_.vec) {
			return {};
		}
		return std::span<const std::uint8_t>(storage_.vec->data(), length_);
	case StorageKind::stream:
		if (!parent_) {
			return {};
		}
		return parent_->stream().get_span(storage_.offset_, length_);
	case StorageKind::json_tree: {
		auto* self = const_cast<DataElement*>(this);
		self->materialize_json_tree();
		return self->value_span();
	}
	case StorageKind::none:
	case StorageKind::sequence:
	case StorageKind::pixel_sequence:
		return {};
	}
	return {};
}

std::size_t DataElement::length() const {
	if (storage_kind_ == StorageKind::json_tree) {
		auto* self = const_cast<DataElement*>(this);
		self->materialize_json_tree();
	}
	return length_;
}

void DataElement::reserve_value_bytes(std::size_t length) {
	if (vr_ == dicom::VR::SQ || vr_ == dicom::VR::PX || vr_ == dicom::VR::None) {
		diag::error_and_throw(
		    "DataElement::reserve_value_bytes reason=cannot reserve raw value bytes for sequence storage vr={}",
		    vr_.str());
	}

	if (length == 0) {
		release_storage();
		length_ = 0;
		return;
	}

	if (length <= kInlineStorageBytes) {
		if (storage_kind_ != StorageKind::inline_bytes) {
			release_storage();
		}
		length_ = length;
		std::memset(storage_.inline_bytes, 0, kInlineStorageBytes);
		storage_kind_ = StorageKind::inline_bytes;
		return;
	}

	if (storage_kind_ == StorageKind::owned_bytes && storage_.vec) {
		storage_.vec->resize(length);
		length_ = length;
		return;
	}

	if (storage_kind_ == StorageKind::heap && storage_.ptr) {
		std::size_t capacity = 0;
		const auto* storage_base =
		    static_cast<const std::uint8_t*>(storage_.ptr) - sizeof(std::size_t);
		std::memcpy(&capacity, storage_base, sizeof(capacity));
		if (length <= capacity) {
			length_ = length;
			return;
		}
	}

	release_storage();
	length_ = length;
	if (length_ > std::numeric_limits<std::size_t>::max() - sizeof(std::size_t)) {
		diag::error_and_throw(
		    "DataElement::reserve_value_bytes reason=length overflow length={}",
		    length_);
	}
	const auto allocation_size = sizeof(std::size_t) + length_;
	auto* storage_base = static_cast<std::uint8_t*>(::operator new(allocation_size));
	std::memcpy(storage_base, &length_, sizeof(length_));
	storage_.ptr = storage_base + sizeof(std::size_t);
	storage_kind_ = StorageKind::heap;
}

void DataElement::set_value_bytes(std::span<const std::uint8_t> bytes) {
	if (vr_ == dicom::VR::SQ || vr_ == dicom::VR::PX || vr_ == dicom::VR::None) {
		diag::error_and_throw(
		    "DataElement::set_value_bytes reason=cannot assign raw bytes to sequence storage vr={}",
		    vr_.str());
	}
	store_padded_value_bytes(*this, bytes);
	if (tag_ == charset::detail::kSpecificCharacterSetTag && parent_) {
		parent_->on_specific_character_set_changed();
	}
}

void DataElement::set_value_bytes(std::vector<std::uint8_t>&& bytes) {
	adopt_value_bytes_impl(std::move(bytes), true);
}

void DataElement::set_value_bytes_nocheck(std::vector<std::uint8_t>&& bytes) {
	adopt_value_bytes_nocheck(std::move(bytes));
}

void DataElement::adopt_value_bytes(std::vector<std::uint8_t>&& bytes) {
	adopt_value_bytes_impl(std::move(bytes), true);
}

void DataElement::adopt_value_bytes_nocheck(std::vector<std::uint8_t>&& bytes) {
	adopt_value_bytes_impl(std::move(bytes), false);
}

void DataElement::adopt_value_bytes_impl(
    std::vector<std::uint8_t>&& bytes, bool notify_charset_parent) {
	if (vr_ == dicom::VR::SQ || vr_ == dicom::VR::PX || vr_ == dicom::VR::None) {
		diag::error_and_throw(
		    "DataElement::adopt_value_bytes reason=cannot assign raw bytes to sequence storage vr={}",
		    vr_.str());
	}

	const bool needs_padding = VR::pad_to_even() && ((bytes.size() & 1u) != 0u);
	if (needs_padding) {
		if (bytes.size() == bytes.max_size()) {
			diag::error_and_throw(
			    "DataElement::adopt_value_bytes reason=length overflow length={}",
			    bytes.size());
		}
		bytes.push_back(vr_.padding_byte());
	}

	if (bytes.empty()) {
		reserve_value_bytes(0);
		if (notify_charset_parent && tag_ == charset::detail::kSpecificCharacterSetTag && parent_) {
			parent_->on_specific_character_set_changed();
		}
		return;
	}

	if (bytes.size() <= kInlineStorageBytes) {
		store_padded_value_bytes(*this,
		    std::span<const std::uint8_t>(bytes.data(), bytes.size()));
		if (notify_charset_parent && tag_ == charset::detail::kSpecificCharacterSetTag && parent_) {
			parent_->on_specific_character_set_changed();
		}
		return;
	}

	release_storage();
	length_ = bytes.size();
	storage_.vec = new std::vector<std::uint8_t>(std::move(bytes));
	storage_kind_ = StorageKind::owned_bytes;
	if (notify_charset_parent && tag_ == charset::detail::kSpecificCharacterSetTag && parent_) {
		parent_->on_specific_character_set_changed();
	}
}

bool DataElement::from_int(int value) {
	bool ok = false;
	switch (static_cast<std::uint16_t>(vr_)) {
	case VR::SS_val:
		ok = assign_integral_from_integer<std::int16_t>(*this, value);
		break;
	case VR::US_val:
		ok = assign_integral_from_integer<std::uint16_t>(*this, value);
		break;
	case VR::SL_val:
		ok = assign_integral_from_integer<std::int32_t>(*this, value);
		break;
	case VR::UL_val:
		ok = assign_integral_from_integer<std::uint32_t>(*this, value);
		break;
	case VR::SV_val:
		ok = assign_integral_from_integer<std::int64_t>(*this, value);
		break;
	case VR::UV_val:
		ok = assign_integral_from_integer<std::uint64_t>(*this, value);
		break;
	case VR::IS_val:
	case VR::DS_val:
		ok = assign_integer_string_from_value(*this, value);
		break;
	default:
		return report_from_assignment_failure(*this, "unsupported VR for from_int");
	}

	if (!ok) {
		return report_from_assignment_failure(*this, "value out of range for VR");
	}
	return true;
}

bool DataElement::from_int_vector(std::span<const int> values) {
	bool ok = false;
	switch (static_cast<std::uint16_t>(vr_)) {
	case VR::SS_val:
		ok = assign_integral_vector_from_integer<std::int16_t>(*this, values);
		break;
	case VR::US_val:
		ok = assign_integral_vector_from_integer<std::uint16_t>(*this, values);
		break;
	case VR::SL_val:
		ok = assign_integral_vector_from_integer<std::int32_t>(*this, values);
		break;
	case VR::UL_val:
		ok = assign_integral_vector_from_integer<std::uint32_t>(*this, values);
		break;
	case VR::SV_val:
		ok = assign_integral_vector_from_integer<std::int64_t>(*this, values);
		break;
	case VR::UV_val:
		ok = assign_integral_vector_from_integer<std::uint64_t>(*this, values);
		break;
	case VR::IS_val:
	case VR::DS_val:
		ok = assign_integer_string_from_values(*this, values);
		break;
	default:
		return report_from_assignment_failure(*this, "unsupported VR for from_int_vector");
	}

	if (!ok) {
		return report_from_assignment_failure(*this, "one or more values are out of range for VR");
	}
	return true;
}

bool DataElement::from_long(long value) {
	bool ok = false;
	switch (static_cast<std::uint16_t>(vr_)) {
	case VR::SS_val:
		ok = assign_integral_from_integer<std::int16_t>(*this, value);
		break;
	case VR::US_val:
		ok = assign_integral_from_integer<std::uint16_t>(*this, value);
		break;
	case VR::SL_val:
		ok = assign_integral_from_integer<std::int32_t>(*this, value);
		break;
	case VR::UL_val:
		ok = assign_integral_from_integer<std::uint32_t>(*this, value);
		break;
	case VR::SV_val:
		ok = assign_integral_from_integer<std::int64_t>(*this, value);
		break;
	case VR::UV_val:
		ok = assign_integral_from_integer<std::uint64_t>(*this, value);
		break;
	case VR::IS_val:
	case VR::DS_val:
		ok = assign_integer_string_from_value(*this, value);
		break;
	default:
		return report_from_assignment_failure(*this, "unsupported VR for from_long");
	}

	if (!ok) {
		return report_from_assignment_failure(*this, "value out of range for VR");
	}
	return true;
}

bool DataElement::from_long_vector(std::span<const long> values) {
	bool ok = false;
	switch (static_cast<std::uint16_t>(vr_)) {
	case VR::SS_val:
		ok = assign_integral_vector_from_integer<std::int16_t>(*this, values);
		break;
	case VR::US_val:
		ok = assign_integral_vector_from_integer<std::uint16_t>(*this, values);
		break;
	case VR::SL_val:
		ok = assign_integral_vector_from_integer<std::int32_t>(*this, values);
		break;
	case VR::UL_val:
		ok = assign_integral_vector_from_integer<std::uint32_t>(*this, values);
		break;
	case VR::SV_val:
		ok = assign_integral_vector_from_integer<std::int64_t>(*this, values);
		break;
	case VR::UV_val:
		ok = assign_integral_vector_from_integer<std::uint64_t>(*this, values);
		break;
	case VR::IS_val:
	case VR::DS_val:
		ok = assign_integer_string_from_values(*this, values);
		break;
	default:
		return report_from_assignment_failure(*this, "unsupported VR for from_long_vector");
	}

	if (!ok) {
		return report_from_assignment_failure(*this, "one or more values are out of range for VR");
	}
	return true;
}

bool DataElement::from_longlong(long long value) {
	bool ok = false;
	switch (static_cast<std::uint16_t>(vr_)) {
	case VR::SS_val:
		ok = assign_integral_from_integer<std::int16_t>(*this, value);
		break;
	case VR::US_val:
		ok = assign_integral_from_integer<std::uint16_t>(*this, value);
		break;
	case VR::SL_val:
		ok = assign_integral_from_integer<std::int32_t>(*this, value);
		break;
	case VR::UL_val:
		ok = assign_integral_from_integer<std::uint32_t>(*this, value);
		break;
	case VR::SV_val:
		ok = assign_integral_from_integer<std::int64_t>(*this, value);
		break;
	case VR::UV_val:
		ok = assign_integral_from_integer<std::uint64_t>(*this, value);
		break;
	case VR::IS_val:
	case VR::DS_val:
		ok = assign_integer_string_from_value(*this, value);
		break;
	default:
		return report_from_assignment_failure(*this, "unsupported VR for from_longlong");
	}

	if (!ok) {
		return report_from_assignment_failure(*this, "value out of range for VR");
	}
	return true;
}

bool DataElement::from_longlong_vector(std::span<const long long> values) {
	bool ok = false;
	switch (static_cast<std::uint16_t>(vr_)) {
	case VR::SS_val:
		ok = assign_integral_vector_from_integer<std::int16_t>(*this, values);
		break;
	case VR::US_val:
		ok = assign_integral_vector_from_integer<std::uint16_t>(*this, values);
		break;
	case VR::SL_val:
		ok = assign_integral_vector_from_integer<std::int32_t>(*this, values);
		break;
	case VR::UL_val:
		ok = assign_integral_vector_from_integer<std::uint32_t>(*this, values);
		break;
	case VR::SV_val:
		ok = assign_integral_vector_from_integer<std::int64_t>(*this, values);
		break;
	case VR::UV_val:
		ok = assign_integral_vector_from_integer<std::uint64_t>(*this, values);
		break;
	case VR::IS_val:
	case VR::DS_val:
		ok = assign_integer_string_from_values(*this, values);
		break;
	default:
		return report_from_assignment_failure(*this, "unsupported VR for from_longlong_vector");
	}

	if (!ok) {
		return report_from_assignment_failure(*this, "one or more values are out of range for VR");
	}
	return true;
}

bool DataElement::from_double(double value) {
	bool ok = false;
	switch (static_cast<std::uint16_t>(vr_)) {
	case VR::FL_val:
		ok = assign_floating_from_double<float>(*this, value);
		break;
	case VR::FD_val:
		ok = assign_floating_from_double<double>(*this, value);
		break;
	case VR::DS_val:
		ok = assign_decimal_string_from_double(*this, value);
		break;
	default:
		return report_from_assignment_failure(*this, "unsupported VR for from_double");
	}

	if (!ok) {
		return report_from_assignment_failure(*this, "value out of range for VR");
	}
	return true;
}

bool DataElement::from_double_vector(std::span<const double> values) {
	bool ok = false;
	switch (static_cast<std::uint16_t>(vr_)) {
	case VR::FL_val:
		ok = assign_floating_vector_from_double<float>(*this, values);
		break;
	case VR::FD_val:
		ok = assign_floating_vector_from_double<double>(*this, values);
		break;
	case VR::DS_val:
		ok = assign_decimal_string_vector_from_double(*this, values);
		break;
	default:
		return report_from_assignment_failure(*this, "unsupported VR for from_double_vector");
	}

	if (!ok) {
		return report_from_assignment_failure(*this, "one or more values are out of range for VR");
	}
	return true;
}

bool DataElement::from_tag(Tag value) {
	if (vr_ != dicom::VR::AT) {
		return report_from_assignment_failure(*this, "AT VR required for from_tag");
	}

	reserve_value_bytes(4);
	auto dst = value_span();
	auto* writable = const_cast<std::uint8_t*>(dst.data());
	endian::store_le<std::uint16_t>(writable, value.group());
	endian::store_le<std::uint16_t>(writable + 2, value.element());
	return true;
}

bool DataElement::from_tag_vector(std::span<const Tag> values) {
	if (vr_ != dicom::VR::AT) {
		return report_from_assignment_failure(*this, "AT VR required for from_tag_vector");
	}
	if (values.empty()) {
		reserve_value_bytes(0);
		return true;
	}
	if (values.size() > std::numeric_limits<std::size_t>::max() / 4) {
		return report_from_assignment_failure(*this, "too many tag values for AT element");
	}

	const std::size_t total_bytes = values.size() * 4;
	reserve_value_bytes(total_bytes);
	auto dst = value_span();
	auto* writable = const_cast<std::uint8_t*>(dst.data());
	for (std::size_t i = 0; i < values.size(); ++i) {
		const auto& tag = values[i];
		const auto offset = i * 4;
		endian::store_le<std::uint16_t>(writable + offset, tag.group());
		endian::store_le<std::uint16_t>(writable + offset + 2, tag.element());
	}
	return true;
}

bool DataElement::from_string_view(std::string_view value) {
	if (vr_ == dicom::VR::UI) {
		return from_uid_string(value);
	}
	if (!vr_.is_string()) {
		return report_from_assignment_failure(*this, "unsupported VR for from_string_view");
	}

	const auto* ptr = reinterpret_cast<const std::uint8_t*>(value.data());
	store_padded_value_bytes(*this, std::span<const std::uint8_t>(ptr, value.size()));
	if (tag_ == charset::detail::kSpecificCharacterSetTag && parent_) {
		parent_->on_specific_character_set_changed();
	}
	return true;
}

bool DataElement::from_string_views(std::span<const std::string_view> values) {
	if (values.empty()) {
		reserve_value_bytes(0);
		if (tag_ == charset::detail::kSpecificCharacterSetTag && parent_) {
			parent_->on_specific_character_set_changed();
		}
		return true;
	}
	if (vr_ == dicom::VR::UI) {
		std::string text;
		text.reserve(values.size() * 66);
		for (std::size_t i = 0; i < values.size(); ++i) {
			std::string normalized = uid::normalize_uid_text(values[i]);
			if (!uid::is_valid_uid_text_strict(normalized)) {
				return report_from_assignment_failure(*this, "invalid UID text in one or more values");
			}
			if (i != 0) {
				text.push_back('\\');
			}
			text += normalized;
		}
		const auto* ptr = reinterpret_cast<const std::uint8_t*>(text.data());
		store_padded_value_bytes(*this, std::span<const std::uint8_t>(ptr, text.size()));
		if (tag_ == charset::detail::kSpecificCharacterSetTag && parent_) {
			parent_->on_specific_character_set_changed();
		}
		return true;
	}
	if (!vr_.is_string()) {
		return report_from_assignment_failure(*this, "unsupported VR for from_string_views");
	}

	switch (static_cast<std::uint16_t>(vr_)) {
	case VR::LT_val:
	case VR::ST_val:
	case VR::UT_val:
	case VR::UR_val:
		if (values.size() != 1) {
			return report_from_assignment_failure(*this, "VR requires a single value for from_string_views");
		}
		return from_string_view(values.front());
	default:
		break;
	}

	std::string text;
	std::size_t total_length = values.size() > 1 ? values.size() - 1 : 0;
	for (const auto value : values) {
		total_length += value.size();
	}
	text.reserve(total_length);
	for (std::size_t i = 0; i < values.size(); ++i) {
		if (i != 0) {
			text.push_back('\\');
		}
		text.append(values[i].data(), values[i].size());
	}
	const auto* ptr = reinterpret_cast<const std::uint8_t*>(text.data());
	store_padded_value_bytes(*this, std::span<const std::uint8_t>(ptr, text.size()));
	if (tag_ == charset::detail::kSpecificCharacterSetTag && parent_) {
		parent_->on_specific_character_set_changed();
	}
	return true;
}

bool DataElement::from_utf8_view(
    std::string_view value, CharsetEncodeErrorPolicy errors, bool* out_replaced) {
	return from_utf8_views(std::span<const std::string_view>(&value, 1), errors, out_replaced);
}

bool DataElement::from_utf8_views(
    std::span<const std::string_view> values, CharsetEncodeErrorPolicy errors,
    bool* out_replaced) {
	if (values.empty()) {
		if (out_replaced) {
			*out_replaced = false;
		}
		return from_string_views(values);
	}
	if (!vr_.is_string()) {
		return report_from_assignment_failure(*this, "unsupported VR for from_utf8_views");
	}
	for (const auto value : values) {
		if (!charset::validate_utf8(value)) {
			return report_from_assignment_failure(*this, "input is not valid UTF-8");
		}
	}
	if (!vr_.uses_specific_character_set()) {
		for (const auto value : values) {
			if (!charset::validate_ascii(value)) {
				return report_from_assignment_failure(*this, "VR requires ASCII-compatible text");
			}
		}
		if (out_replaced) {
			*out_replaced = false;
		}
		return from_string_views(values);
	}
	std::string error;
	if (charset::encode_utf8_for_element(
	        *this, values, errors, &error, out_replaced)) {
		return true;
	}
	return report_from_assignment_failure(*this, error.empty() ? "failed to encode UTF-8 text" : error);
}

bool DataElement::from_uid(uid::WellKnown uid) {
	if (!uid.valid()) {
		return report_from_assignment_failure(*this, "invalid uid::WellKnown value");
	}
	return from_uid_string(uid.value());
}

bool DataElement::from_uid(const uid::Generated& uid) {
	return from_uid_string(uid.value());
}

bool DataElement::from_uid_string(std::string_view uid_value) {
	if (!assign_uid_string(*this, uid_value)) {
		if (vr_ != dicom::VR::UI) {
			return report_from_assignment_failure(*this, "UI VR required");
		}
		return report_from_assignment_failure(*this, "invalid UID text");
	}
	return true;
}

bool DataElement::from_transfer_syntax_uid(uid::WellKnown uid) {
	if (!uid.valid() || uid.uid_type() != UidType::TransferSyntax) {
		return report_from_assignment_failure(*this, "uid must be a valid Transfer Syntax UID");
	}
	return from_uid(uid);
}

bool DataElement::from_sop_class_uid(uid::WellKnown uid) {
	if (!uid.valid()) {
		return report_from_assignment_failure(*this, "uid must be valid");
	}
	const auto type = uid.uid_type();
	if (type != UidType::SopClass && type != UidType::MetaSopClass) {
		return report_from_assignment_failure(*this, "uid must be SOP Class or Meta SOP Class");
	}
	return from_uid(uid);
}

int DataElement::vm() const {
	if (storage_kind_ == StorageKind::json_tree) {
		(void)value_span();
	}
	// PS 3.5, 6.4 VALUE MULTIPLICITY (VM) AND DELIMITATION
	if (length_ == 0) {
		return 0;
	}

	const auto vr_value = static_cast<std::uint16_t>(vr_);
	switch (vr_value) {
	case VR::FD_val:
	case VR::SV_val:
	case VR::UV_val:
		return static_cast<int>(length_ / 8);
	case VR::AT_val:
	case VR::FL_val:
	case VR::UL_val:
	case VR::SL_val:
		return static_cast<int>(length_ / 4);
	case VR::US_val:
	case VR::SS_val:
		return static_cast<int>(length_ / 2);
	case VR::AE_val: case VR::AS_val: case VR::CS_val: case VR::DA_val:
	case VR::DS_val: case VR::DT_val: case VR::IS_val: case VR::LO_val:
	case VR::PN_val: case VR::SH_val: case VR::TM_val: case VR::UC_val:
	case VR::UI_val: {
		std::span<const std::uint8_t> data;
		data = value_span();
		if (data.empty()) {
			return 0;
		}
		int delims = 0;
		for (auto byte : data) {
			if (byte == '\\') {
				++delims;
			}
		}
		return delims + 1;
	}
	// LT, OB, OD, OF, OL, OW, SQ, ST, UN, UR or UT -> always 1
	default:
		return 1;
	}
}

std::optional<std::vector<long long>> DataElement::to_longlong_vector() const {
	switch (static_cast<std::uint16_t>(vr_)) {
	case VR::SS_val: {
		auto vec = load_numeric_vector<std::int16_t>(*this);
		if (!vec) return std::nullopt;
		std::vector<long long> out(vec->begin(), vec->end());
		return out;
	}
	case VR::US_val: {
		auto vec = load_numeric_vector<std::uint16_t>(*this);
		if (!vec) return std::nullopt;
		std::vector<long long> out;
		out.reserve(vec->size());
		for (auto v : *vec) out.push_back(static_cast<long long>(v));
		return out;
	}
	case VR::SL_val: {
		auto vec = load_numeric_vector<std::int32_t>(*this);
		if (!vec) return std::nullopt;
		std::vector<long long> out(vec->begin(), vec->end());
		return out;
	}
	case VR::UL_val: {
		auto vec = load_numeric_vector<std::uint32_t>(*this);
		if (!vec) return std::nullopt;
		std::vector<long long> out;
		out.reserve(vec->size());
		for (auto v : *vec) out.push_back(static_cast<long long>(v));
		return out;
	}
	case VR::SV_val: {
		auto vec = load_numeric_vector<std::int64_t>(*this);
		if (!vec) return std::nullopt;
		std::vector<long long> out(vec->begin(), vec->end());
		return out;
	}
	case VR::UV_val: {
		auto vec = load_numeric_vector<std::uint64_t>(*this);
		if (!vec) return std::nullopt;
		std::vector<long long> out;
		out.reserve(vec->size());
		for (auto v : *vec) {
			if (v > static_cast<std::uint64_t>(std::numeric_limits<long long>::max())) {
				return std::nullopt;
			}
			out.push_back(static_cast<long long>(v));
		}
		return out;
	}
	case VR::IS_val: {
		auto vec = parse_string_numbers(*this, IntParser{});
		if (!vec) return std::nullopt;
		return vec;
	}
	case VR::DS_val: {
		// DS is decimal string; cast to integer if integral
		auto vec = parse_string_numbers(*this, DoubleParser{});
		if (!vec) return std::nullopt;
		std::vector<long long> out;
		out.reserve(vec->size());
		for (auto v : *vec) {
			auto rounded = std::llround(v);
			if (std::fabs(v - rounded) > 1e-9) return std::nullopt; // not integral
			out.push_back(rounded);
		}
		return out;
	}
	default:
		return std::nullopt;
	}
}

std::optional<std::vector<int>> DataElement::to_int_vector() const {
	switch (static_cast<std::uint16_t>(vr_)) {
	case VR::SS_val: {
		auto vec = load_numeric_vector<std::int16_t>(*this);
		if (!vec) return std::nullopt;
		std::vector<int> out;
		out.reserve(vec->size());
		for (auto v : *vec) {
			if constexpr (sizeof(int) < sizeof(std::int16_t)) {
				if (v < std::numeric_limits<int>::min() || v > std::numeric_limits<int>::max()) return std::nullopt;
			}
			out.push_back(static_cast<int>(v));
		}
		return out;
	}
	case VR::US_val: {
		auto vec = load_numeric_vector<std::uint16_t>(*this);
		if (!vec) return std::nullopt;
		std::vector<int> out;
		out.reserve(vec->size());
		for (auto v : *vec) {
			if (v > static_cast<std::uint16_t>(std::numeric_limits<int>::max())) return std::nullopt;
			out.push_back(static_cast<int>(v));
		}
		return out;
	}
	case VR::UL_val: {
		auto vec = load_numeric_vector<std::uint32_t>(*this);
		if (!vec) return std::nullopt;
		std::vector<int> out;
		out.reserve(vec->size());
		for (auto v : *vec) {
			if (v > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) return std::nullopt;
			out.push_back(static_cast<int>(v));
		}
		return out;
	}
	case VR::SL_val: {
		auto vec = load_numeric_vector<std::int32_t>(*this);
		if (!vec) return std::nullopt;
		std::vector<int> out;
		out.reserve(vec->size());
		for (auto v : *vec) {
			if (v < std::numeric_limits<int>::min() || v > std::numeric_limits<int>::max()) return std::nullopt;
			out.push_back(static_cast<int>(v));
		}
		return out;
	}
	case VR::SV_val: {
		auto vec = load_numeric_vector<std::int64_t>(*this);
		if (!vec) return std::nullopt;
		std::vector<int> out;
		out.reserve(vec->size());
		for (auto v : *vec) {
			if (v < std::numeric_limits<int>::min() || v > std::numeric_limits<int>::max()) {
				diag::warn("DataElement::to_int tag={} vr=SV value too wide for int; use to_long() or to_longlong()", tag_.to_string());
				return std::nullopt;
			}
			out.push_back(static_cast<int>(v));
		}
		return out;
	}
	case VR::UV_val: {
		auto vec = load_numeric_vector<std::uint64_t>(*this);
		if (!vec) return std::nullopt;
		std::vector<int> out;
		out.reserve(vec->size());
		for (auto v : *vec) {
			if (v > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
				diag::warn("DataElement::to_int tag={} vr=UV value too wide for int; use to_long() or to_longlong()", tag_.to_string());
				return std::nullopt;
			}
			out.push_back(static_cast<int>(v));
		}
		return out;
	}
	case VR::IS_val: {
		auto vec = parse_string_numbers(*this, IntParser{});
		if (!vec) return std::nullopt;
		return make_int_vector_from_numbers(*vec);
	}
	case VR::DS_val: {
		auto vec = parse_string_numbers(*this, DoubleParser{});
		if (!vec) return std::nullopt;
		std::vector<int> out;
		out.reserve(vec->size());
		for (auto v : *vec) {
			auto rounded = std::llround(v);
			if (std::fabs(v - rounded) > 1e-9) return std::nullopt;
			if (rounded < std::numeric_limits<int>::min() || rounded > std::numeric_limits<int>::max()) return std::nullopt;
			out.push_back(static_cast<int>(rounded));
		}
		return out;
	}
	default:
		return std::nullopt;
	}
}

std::optional<std::vector<long>> DataElement::to_long_vector() const {
	switch (static_cast<std::uint16_t>(vr_)) {
	case VR::SS_val: {
		auto vec = load_numeric_vector<std::int16_t>(*this);
		if (!vec) return std::nullopt;
		std::vector<long> out;
		out.reserve(vec->size());
		for (auto v : *vec) out.push_back(static_cast<long>(v));
		return out;
	}
	case VR::US_val: {
		auto vec = load_numeric_vector<std::uint16_t>(*this);
		if (!vec) return std::nullopt;
		std::vector<long> out;
		out.reserve(vec->size());
		for (auto v : *vec) out.push_back(static_cast<long>(v));
		return out;
	}
	case VR::UL_val: {
		auto vec = load_numeric_vector<std::uint32_t>(*this);
		if (!vec) return std::nullopt;
		std::vector<long> out;
		out.reserve(vec->size());
		for (auto v : *vec) out.push_back(static_cast<long>(v));
		return out;
	}
	case VR::SL_val: {
		auto vec = load_numeric_vector<std::int32_t>(*this);
		if (!vec) return std::nullopt;
		std::vector<long> out;
		out.reserve(vec->size());
		for (auto v : *vec) out.push_back(static_cast<long>(v));
		return out;
	}
	case VR::SV_val: {
		auto vec = load_numeric_vector<std::int64_t>(*this);
		if (!vec) return std::nullopt;
		std::vector<long> out;
		out.reserve(vec->size());
		for (auto v : *vec) {
			if constexpr (sizeof(long) < sizeof(std::int64_t)) {
				if (v < std::numeric_limits<long>::min() || v > std::numeric_limits<long>::max()) return std::nullopt;
			}
			out.push_back(static_cast<long>(v));
		}
		return out;
	}
	case VR::UV_val: {
		auto vec = load_numeric_vector<std::uint64_t>(*this);
		if (!vec) return std::nullopt;
		std::vector<long> out;
		out.reserve(vec->size());
		for (auto v : *vec) {
			if constexpr (sizeof(long) < sizeof(std::uint64_t)) {
				if (v > static_cast<std::uint64_t>(std::numeric_limits<long>::max())) return std::nullopt;
			}
			out.push_back(static_cast<long>(v));
		}
		return out;
	}
	case VR::IS_val: {
		auto vec = parse_string_numbers(*this, IntParser{});
		if (!vec) return std::nullopt;
		std::vector<long> out;
		out.reserve(vec->size());
		for (auto v : *vec) {
			if (v < std::numeric_limits<long>::min() || v > std::numeric_limits<long>::max()) return std::nullopt;
			out.push_back(static_cast<long>(v));
		}
		return out;
	}
	case VR::DS_val: {
		auto vec = parse_string_numbers(*this, DoubleParser{});
		if (!vec) return std::nullopt;
		std::vector<long> out;
		out.reserve(vec->size());
		for (auto v : *vec) {
			auto rounded = std::llround(v);
			if (std::fabs(v - rounded) > 1e-9) return std::nullopt;
			if (rounded < std::numeric_limits<long>::min() || rounded > std::numeric_limits<long>::max()) return std::nullopt;
			out.push_back(static_cast<long>(rounded));
		}
		return out;
	}
	default:
		return std::nullopt;
	}
}

// Fast scalar paths to avoid intermediate vectors
template <typename T>
static std::optional<T> load_numeric_scalar(const DataElement& elem) {
	const auto span = elem.value_span();
	if (span.size() < sizeof(T)) return std::nullopt;
	const auto* ptr = span.data();
	if constexpr (std::is_floating_point_v<T>) {
		using Bits = std::conditional_t<sizeof(T)==4, std::uint32_t, std::uint64_t>;
		const Bits bits = endian::load_le<Bits>(ptr);
		return std::bit_cast<T>(bits);
	} else {
		return endian::load_le<T>(ptr);
	}
}

std::optional<long long> DataElement::to_longlong() const {
	switch (static_cast<std::uint16_t>(vr_)) {
	case VR::SS_val: {
		auto v = load_numeric_scalar<std::int16_t>(*this);
		if (!v) return std::nullopt;
		return static_cast<long long>(*v);
	}
	case VR::US_val: {
		auto v = load_numeric_scalar<std::uint16_t>(*this);
		if (!v) return std::nullopt;
		return static_cast<long long>(*v);
	}
	case VR::SL_val: {
		auto v = load_numeric_scalar<std::int32_t>(*this);
		if (!v) return std::nullopt;
		return static_cast<long long>(*v);
	}
	case VR::UL_val: {
		auto v = load_numeric_scalar<std::uint32_t>(*this);
		if (!v) return std::nullopt;
		return static_cast<long long>(*v);
	}
	case VR::SV_val: {
		auto v = load_numeric_scalar<std::int64_t>(*this);
		if (!v) return std::nullopt;
		return static_cast<long long>(*v);
	}
	case VR::UV_val: {
		auto v = load_numeric_scalar<std::uint64_t>(*this);
		if (!v) return std::nullopt;
		if (*v > static_cast<std::uint64_t>(std::numeric_limits<long long>::max())) return std::nullopt;
		return static_cast<long long>(*v);
	}
	case VR::IS_val: {
		auto nums = parse_string_numbers(*this, IntParser{});
		if (!nums || nums->empty()) return std::nullopt;
		return nums->front();
	}
	case VR::DS_val: {
		auto nums = parse_string_numbers(*this, DoubleParser{});
		if (!nums || nums->empty()) return std::nullopt;
		auto v = nums->front();
		auto rounded = std::llround(v);
		if (std::fabs(v - rounded) > 1e-9) return std::nullopt;
		return rounded;
	}
	default:
		return std::nullopt;
	}
}

std::optional<int> DataElement::to_int() const {
	switch (static_cast<std::uint16_t>(vr_)) {
	case VR::SS_val: {
		auto v = load_numeric_scalar<std::int16_t>(*this);
		if (!v) return std::nullopt;
		if constexpr (sizeof(int) < sizeof(std::int16_t)) {
			if (*v < std::numeric_limits<int>::min() || *v > std::numeric_limits<int>::max()) return std::nullopt;
		}
		return static_cast<int>(*v);
	}
	case VR::US_val: {
		auto v = load_numeric_scalar<std::uint16_t>(*this);
		if (!v) return std::nullopt;
		if (*v > static_cast<std::uint16_t>(std::numeric_limits<int>::max())) return std::nullopt;
		return static_cast<int>(*v);
	}
	case VR::SL_val: {
		auto v = load_numeric_scalar<std::int32_t>(*this);
		if (!v) return std::nullopt;
		if (*v < std::numeric_limits<int>::min() || *v > std::numeric_limits<int>::max()) return std::nullopt;
		return static_cast<int>(*v);
	}
	case VR::UL_val: {
		auto v = load_numeric_scalar<std::uint32_t>(*this);
		if (!v) return std::nullopt;
		if (*v > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) return std::nullopt;
		return static_cast<int>(*v);
	}
	case VR::SV_val: {
		auto v = load_numeric_scalar<std::int64_t>(*this);
		if (!v) return std::nullopt;
		if (*v < std::numeric_limits<int>::min() || *v > std::numeric_limits<int>::max()) {
			diag::warn("DataElement::to_int tag={} vr=SV value too wide for int; use to_long() or to_longlong()", tag_.to_string());
			return std::nullopt;
		}
		return static_cast<int>(*v);
	}
	case VR::UV_val: {
		auto v = load_numeric_scalar<std::uint64_t>(*this);
		if (!v) return std::nullopt;
		if (*v > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
			diag::warn("DataElement::to_int tag={} vr=UV value too wide for int; use to_long() or to_longlong()", tag_.to_string());
			return std::nullopt;
		}
		return static_cast<int>(*v);
	}
	case VR::IS_val: {
		auto nums = parse_string_numbers(*this, IntParser{});
		if (!nums || nums->empty()) return std::nullopt;
		auto v = nums->front();
		if (v < std::numeric_limits<int>::min() || v > std::numeric_limits<int>::max()) return std::nullopt;
		return static_cast<int>(v);
	}
	case VR::DS_val: {
		auto nums = parse_string_numbers(*this, DoubleParser{});
		if (!nums || nums->empty()) return std::nullopt;
		auto v = nums->front();
		auto rounded = std::llround(v);
		if (std::fabs(v - rounded) > 1e-9) return std::nullopt;
		if (rounded < std::numeric_limits<int>::min() || rounded > std::numeric_limits<int>::max()) return std::nullopt;
		return static_cast<int>(rounded);
	}
	default:
		return std::nullopt;
	}
}



std::optional<long> DataElement::to_long() const {
	switch (static_cast<std::uint16_t>(vr_)) {
	case VR::SS_val: {
		auto v = load_numeric_scalar<std::int16_t>(*this);
		if (!v) return std::nullopt;
		return static_cast<long>(*v);
	}
	case VR::US_val: {
		auto v = load_numeric_scalar<std::uint16_t>(*this);
		if (!v) return std::nullopt;
		if constexpr (sizeof(long) < sizeof(std::uint16_t)) {
			if (*v > static_cast<std::uint16_t>(std::numeric_limits<long>::max())) return std::nullopt;
		}
		return static_cast<long>(*v);
	}
	case VR::SL_val: {
		auto v = load_numeric_scalar<std::int32_t>(*this);
		if (!v) return std::nullopt;
		if constexpr (sizeof(long) < sizeof(std::int32_t)) {
			if (*v < std::numeric_limits<long>::min() || *v > std::numeric_limits<long>::max()) return std::nullopt;
		}
		return static_cast<long>(*v);
	}
	case VR::UL_val: {
		auto v = load_numeric_scalar<std::uint32_t>(*this);
		if (!v) return std::nullopt;
		if constexpr (sizeof(long) < sizeof(std::uint32_t)) {
			if (*v > static_cast<std::uint32_t>(std::numeric_limits<long>::max())) return std::nullopt;
		}
		return static_cast<long>(*v);
	}
	case VR::SV_val: {
		auto v = load_numeric_scalar<std::int64_t>(*this);
		if (!v) return std::nullopt;
		if constexpr (sizeof(long) < sizeof(std::int64_t)) {
			if (*v < std::numeric_limits<long>::min() || *v > std::numeric_limits<long>::max()) {
				diag::warn("DataElement::to_long tag={} vr=SV value too wide for long; use to_longlong()", tag_.to_string());
				return std::nullopt;
			}
		}
		return static_cast<long>(*v);
	}
	case VR::UV_val: {
		auto v = load_numeric_scalar<std::uint64_t>(*this);
		if (!v) return std::nullopt;
		if constexpr (sizeof(long) < sizeof(std::uint64_t)) {
			if (*v > static_cast<std::uint64_t>(std::numeric_limits<long>::max())) {
				diag::warn("DataElement::to_long tag={} vr=UV value too wide for long; use to_longlong()", tag_.to_string());
				return std::nullopt;
			}
		}
		return static_cast<long>(*v);
	}
	case VR::IS_val: {
		auto nums = parse_string_numbers(*this, IntParser{});
		if (!nums || nums->empty()) return std::nullopt;
		auto v = nums->front();
		if (v < std::numeric_limits<long>::min() || v > std::numeric_limits<long>::max()) return std::nullopt;
		return static_cast<long>(v);
	}
	case VR::DS_val: {
		auto nums = parse_string_numbers(*this, DoubleParser{});
		if (!nums || nums->empty()) return std::nullopt;
		auto v = nums->front();
		auto rounded = std::llround(v);
		if (std::fabs(v - rounded) > 1e-9) return std::nullopt;
		if (rounded < std::numeric_limits<long>::min() || rounded > std::numeric_limits<long>::max()) return std::nullopt;
		return static_cast<long>(rounded);
	}
	default:
		return std::nullopt;
	}
}

std::optional<std::vector<double>> DataElement::to_double_vector() const {
	switch (static_cast<std::uint16_t>(vr_)) {
	case VR::FL_val: {
		auto vec = load_numeric_vector<float>(*this);
		if (!vec) return std::nullopt;
		std::vector<double> out;
		out.reserve(vec->size());
		for (auto v : *vec) out.push_back(static_cast<double>(v));
		return out;
	}
	case VR::FD_val: {
		auto vec = load_numeric_vector<double>(*this);
		if (!vec) return std::nullopt;
		return vec;
	}
	case VR::SS_val:
	case VR::US_val:
	case VR::SL_val:
	case VR::UL_val:
	case VR::SV_val:
	case VR::UV_val: {
		if constexpr (sizeof(long) == 4) {
			if (vr_ == dicom::VR::SV || vr_ == dicom::VR::UV || vr_ == dicom::VR::UL) {
				auto ll = to_longlong_vector();
				if (!ll) return std::nullopt;
				std::vector<double> out;
				out.reserve(ll->size());
				for (auto v : *ll) out.push_back(static_cast<double>(v));
				return out;
			}
		}
		auto lv = to_long_vector();
		if (!lv) return std::nullopt;
		std::vector<double> out;
		out.reserve(lv->size());
		for (auto v : *lv) out.push_back(static_cast<double>(v));
		return out;
	}
	case VR::DS_val: {
		auto vec = parse_string_numbers(*this, DoubleParser{});
		if (!vec) return std::nullopt;
		return vec;
	}
	case VR::IS_val: {
		auto vec = parse_string_numbers(*this, IntParser{});
		if (!vec) return std::nullopt;
		std::vector<double> out;
		out.reserve(vec->size());
		for (auto v : *vec) out.push_back(static_cast<double>(v));
		return out;
	}
	default:
		return std::nullopt;
	}
}

std::optional<double> DataElement::to_double() const {
	switch (static_cast<std::uint16_t>(vr_)) {
	case VR::FL_val: {
		auto v = load_numeric_scalar<float>(*this);
		if (!v) return std::nullopt;
		return static_cast<double>(*v);
	}
	case VR::FD_val: {
		return load_numeric_scalar<double>(*this);
	}
	case VR::DS_val: {
		auto nums = parse_string_numbers(*this, DoubleParser{});
		if (!nums || nums->empty()) return std::nullopt;
		return nums->front();
	}
	case VR::IS_val: {
		auto nums = parse_string_numbers(*this, IntParser{});
		if (!nums || nums->empty()) return std::nullopt;
		return static_cast<double>(nums->front());
	}
	case VR::SS_val:
	case VR::US_val:
	case VR::SL_val:
	case VR::UL_val:
	case VR::SV_val:
	case VR::UV_val: {
		if constexpr (sizeof(long) == 4) {
			if (vr_ == dicom::VR::SV || vr_ == dicom::VR::UV || vr_ == dicom::VR::UL) {
				auto v = to_longlong();
				if (!v) return std::nullopt;
				return static_cast<double>(*v);
			}
		}
		auto v = to_long();
		if (!v) return std::nullopt;
		return static_cast<double>(*v);
	}
	default:
		return std::nullopt;
	}
}

// AT -> Tag helpers
std::optional<std::vector<Tag>> DataElement::to_tag_vector() const {
	if (vr_ != dicom::VR::AT) return std::nullopt;
	const auto span = value_span();
	if (span.empty()) return std::vector<Tag>{};
	if (span.size() % 4 != 0) return std::nullopt;
	const auto count = span.size() / 4;
	std::vector<Tag> out;
	out.reserve(count);
	for (std::size_t i = 0; i < count; ++i) {
		const auto* ptr = span.data() + i * 4;
		const std::uint16_t g = endian::load_le<std::uint16_t>(ptr);
		const std::uint16_t e = endian::load_le<std::uint16_t>(ptr + 2);
		out.emplace_back(g, e);
	}
	return out;
}

std::optional<Tag> DataElement::to_tag() const {
	if (vr_ != dicom::VR::AT) return std::nullopt;
	const auto span = value_span();
	if (span.size() < 4) return std::nullopt;
	const std::uint16_t g = endian::load_le<std::uint16_t>(span.data());
	const std::uint16_t e = endian::load_le<std::uint16_t>(span.data() + 2);
	return Tag(g, e);
}

std::optional<std::string> DataElement::to_uid_string() const {
	if (vr_ != dicom::VR::UI) {
		return std::nullopt;
	}
	if (auto normalized = to_string_view()) {
		return std::string(*normalized);
	}
	return std::nullopt;
}

std::optional<std::string_view> DataElement::to_string_view() const {
	if (!vr_.is_string()) {
		return std::nullopt;
	}
	const auto span = value_span();
	std::string_view raw(reinterpret_cast<const char*>(span.data()), span.size());
	switch (static_cast<std::uint16_t>(vr_)) {
	case VR::AE_val:
	case VR::AS_val:
	case VR::CS_val:
	case VR::DA_val:
	case VR::DS_val:
	case VR::DT_val:
	case VR::IS_val:
	case VR::LO_val:
	case VR::PN_val:
	case VR::SH_val:
	case VR::TM_val:
	case VR::UI_val:
		return to_string_view_normalize<true, true>(raw);
	case VR::UR_val:
		return to_string_view_normalize<true, false>(raw);
	case VR::UC_val:
		return to_string_view_normalize<false, true>(raw);
	case VR::LT_val:
	case VR::ST_val:
	case VR::UT_val:
		return to_string_view_normalize<false, false>(raw);
	default:
		return std::nullopt;
	}
}

std::optional<std::vector<std::string_view>> DataElement::to_string_views() const {
	if (!vr_.is_string()) {
		return std::nullopt;
	}
	if (!raw_string_splitting_is_safe(*this)) {
		return std::nullopt;
	}
	const auto span = value_span();
	std::string_view raw(reinterpret_cast<const char*>(span.data()), span.size());
	switch (static_cast<std::uint16_t>(vr_)) {
	case VR::AE_val:
	case VR::AS_val:
	case VR::CS_val:
	case VR::DA_val:
	case VR::DS_val:
	case VR::DT_val:
	case VR::IS_val:
	case VR::LO_val:
	case VR::PN_val:
	case VR::SH_val:
	case VR::TM_val:
	case VR::UI_val:
		return to_string_views_normalize<true, true>(raw);
	case VR::UR_val:
		return to_string_views_normalize<true, false>(raw);
	case VR::UC_val:
		return to_string_views_normalize<false, true>(raw);
	case VR::LT_val:
	case VR::ST_val:
	case VR::UT_val:
		return to_string_views_normalize<false, false>(raw);
	default:
		return std::nullopt;
	}
}

std::optional<std::string> DataElement::to_utf8_string(
    CharsetDecodeErrorPolicy errors, bool* out_replaced) const {
	if (storage_kind_ == StorageKind::json_tree) {
		if (out_replaced) {
			*out_replaced = false;
		}
		auto values = parse_json_tree_utf8_values();
		if (!values) {
			return std::nullopt;
		}
		return values->empty() ? std::optional<std::string>(std::string{}) : (*values)[0];
	}
	return charset::raw_element_as_owned_utf8_value(*this, errors, nullptr, out_replaced);
}

std::optional<std::vector<std::string>> DataElement::to_utf8_strings(
    CharsetDecodeErrorPolicy errors, bool* out_replaced) const {
	if (storage_kind_ == StorageKind::json_tree) {
		if (out_replaced) {
			*out_replaced = false;
		}
		return parse_json_tree_utf8_values();
	}
	if (!vr_.is_string()) {
		if (out_replaced) {
			*out_replaced = false;
		}
		return std::nullopt;
	}
	return charset::raw_element_as_owned_utf8_values(*this, errors, nullptr, out_replaced);
}

std::optional<PersonName> DataElement::to_person_name(
    CharsetDecodeErrorPolicy errors, bool* out_replaced) const {
	if (vr_ != dicom::VR::PN) {
		if (out_replaced) {
			*out_replaced = false;
		}
		return std::nullopt;
	}
	auto utf8_value = to_utf8_string(errors, out_replaced);
	if (!utf8_value) {
		return std::nullopt;
	}
	return PersonName::parse(*utf8_value);
}

std::optional<std::vector<PersonName>> DataElement::to_person_names(
    CharsetDecodeErrorPolicy errors, bool* out_replaced) const {
	if (vr_ != dicom::VR::PN) {
		if (out_replaced) {
			*out_replaced = false;
		}
		return std::nullopt;
	}
	auto utf8_values = to_utf8_strings(errors, out_replaced);
	if (!utf8_values) {
		return std::nullopt;
	}
	return PersonName::parse_many(*utf8_values);
}

bool DataElement::from_person_name(
    const PersonName& value, CharsetEncodeErrorPolicy errors, bool* out_replaced) {
	if (vr_ != dicom::VR::PN) {
		return report_from_assignment_failure(*this, "PN VR required for from_person_name");
	}
	return from_utf8_view(value.to_dicom_string(), errors, out_replaced);
}

bool DataElement::from_person_names(
    std::span<const PersonName> values, CharsetEncodeErrorPolicy errors,
    bool* out_replaced) {
	if (vr_ != dicom::VR::PN) {
		return report_from_assignment_failure(*this, "PN VR required for from_person_names");
	}
	std::vector<std::string> encoded_values;
	std::vector<std::string_view> encoded_views;
	encoded_values.reserve(values.size());
	encoded_views.reserve(values.size());
	for (const auto& value : values) {
		encoded_values.push_back(value.to_dicom_string());
		encoded_views.push_back(encoded_values.back());
	}
	return from_utf8_views(encoded_views, errors, out_replaced);
}


std::optional<uid::WellKnown> DataElement::to_transfer_syntax_uid() const {
	auto uid = well_known_uid_from_element_value(*this);
	if (!uid) {
		return std::nullopt;
	}
	if (uid->uid_type() != UidType::TransferSyntax) {
		return std::nullopt;
	}
	return uid;
}

std::optional<uid::WellKnown> DataElement::to_sop_class_uid() const {
	auto uid = well_known_uid_from_element_value(*this);
	if (!uid) {
		return std::nullopt;
	}
	const auto type = uid->uid_type();
	if (type != UidType::SopClass && type != UidType::MetaSopClass) {
		return std::nullopt;
	}
	return uid;
}

}  // namespace dicom
