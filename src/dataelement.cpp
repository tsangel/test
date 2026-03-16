#include <dicom.h>
#include <dicom_endian.h>
#include "charset/charset_decode.hpp"
#include "charset/charset_detail.hpp"
#include "charset/charset_mutation.hpp"
#include "charset/text_validation.hpp"
#include <diagnostics.h>

#include <cctype>
#include <charconv>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <fmt/format.h>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <instream.h>

namespace dicom {

namespace {

inline std::string_view trim(std::string_view s) {
	while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
	while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
	return s;
}

bool report_from_assignment_failure(
    std::string_view function_name, const DataElement& element, std::string_view reason) {
	diag::error("{} tag={} vr={} reason={}",
	    function_name, element.tag().to_string(), element.vr().str(), reason);
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
	return uid::from_value(std::string(*normalized));
}

}  // namespace

}  // namespace

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
	case StorageKind::none:
	case StorageKind::sequence:
	case StorageKind::pixel_sequence:
		return {};
	}
	return {};
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
		return report_from_assignment_failure(
		    "DataElement::from_int", *this, "unsupported VR for from_int");
	}

	if (!ok) {
		return report_from_assignment_failure(
		    "DataElement::from_int", *this, "value out of range for VR");
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
		return report_from_assignment_failure(
		    "DataElement::from_int_vector", *this, "unsupported VR for from_int_vector");
	}

	if (!ok) {
		return report_from_assignment_failure(
		    "DataElement::from_int_vector", *this, "one or more values are out of range for VR");
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
		return report_from_assignment_failure(
		    "DataElement::from_long", *this, "unsupported VR for from_long");
	}

	if (!ok) {
		return report_from_assignment_failure(
		    "DataElement::from_long", *this, "value out of range for VR");
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
		return report_from_assignment_failure(
		    "DataElement::from_long_vector", *this, "unsupported VR for from_long_vector");
	}

	if (!ok) {
		return report_from_assignment_failure(
		    "DataElement::from_long_vector", *this, "one or more values are out of range for VR");
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
		return report_from_assignment_failure(
		    "DataElement::from_longlong", *this, "unsupported VR for from_longlong");
	}

	if (!ok) {
		return report_from_assignment_failure(
		    "DataElement::from_longlong", *this, "value out of range for VR");
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
		return report_from_assignment_failure(
		    "DataElement::from_longlong_vector", *this, "unsupported VR for from_longlong_vector");
	}

	if (!ok) {
		return report_from_assignment_failure(
		    "DataElement::from_longlong_vector", *this, "one or more values are out of range for VR");
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
		return report_from_assignment_failure(
		    "DataElement::from_double", *this, "unsupported VR for from_double");
	}

	if (!ok) {
		return report_from_assignment_failure(
		    "DataElement::from_double", *this, "value out of range for VR");
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
		return report_from_assignment_failure(
		    "DataElement::from_double_vector", *this, "unsupported VR for from_double_vector");
	}

	if (!ok) {
		return report_from_assignment_failure(
		    "DataElement::from_double_vector", *this,
		    "one or more values are out of range for VR");
	}
	return true;
}

bool DataElement::from_tag(Tag value) {
	if (vr_ != dicom::VR::AT) {
		return report_from_assignment_failure(
		    "DataElement::from_tag", *this, "AT VR required for from_tag");
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
		return report_from_assignment_failure(
		    "DataElement::from_tag_vector", *this, "AT VR required for from_tag_vector");
	}
	if (values.empty()) {
		reserve_value_bytes(0);
		return true;
	}
	if (values.size() > std::numeric_limits<std::size_t>::max() / 4) {
		return report_from_assignment_failure(
		    "DataElement::from_tag_vector", *this, "too many tag values for AT element");
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
		return report_from_assignment_failure(
		    "DataElement::from_string_view", *this, "unsupported VR for from_string_view");
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
				return report_from_assignment_failure(
				    "DataElement::from_string_views", *this,
				    "invalid UID text in one or more values");
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
		return report_from_assignment_failure(
		    "DataElement::from_string_views", *this, "unsupported VR for from_string_views");
	}

	switch (static_cast<std::uint16_t>(vr_)) {
	case VR::LT_val:
	case VR::ST_val:
	case VR::UT_val:
	case VR::UR_val:
		if (values.size() != 1) {
			return report_from_assignment_failure(
			    "DataElement::from_string_views", *this,
			    "VR requires a single value for from_string_views");
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
		return report_from_assignment_failure(
		    "DataElement::from_utf8_views", *this, "unsupported VR for from_utf8_views");
	}
	for (const auto value : values) {
		if (!charset::validate_utf8(value)) {
			return report_from_assignment_failure(
			    "DataElement::from_utf8_views", *this, "input is not valid UTF-8");
		}
	}
	if (!vr_.uses_specific_character_set()) {
		for (const auto value : values) {
			if (!charset::validate_ascii(value)) {
				return report_from_assignment_failure(
				    "DataElement::from_utf8_views", *this, "VR requires ASCII-compatible text");
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
	return report_from_assignment_failure(
	    "DataElement::from_utf8_views", *this, error.empty() ? "failed to encode UTF-8 text" : error);
}

bool DataElement::from_uid(uid::WellKnown uid) {
	if (!uid.valid()) {
		return report_from_assignment_failure(
		    "DataElement::from_uid", *this, "invalid uid::WellKnown value");
	}
	return from_uid_string(uid.value());
}

bool DataElement::from_uid(const uid::Generated& uid) {
	return from_uid_string(uid.value());
}

bool DataElement::from_uid_string(std::string_view uid_value) {
	if (!assign_uid_string(*this, uid_value)) {
		if (vr_ != dicom::VR::UI) {
			return report_from_assignment_failure(
			    "DataElement::from_uid_string", *this, "UI VR required");
		}
		return report_from_assignment_failure(
		    "DataElement::from_uid_string", *this, "invalid UID text");
	}
	return true;
}

bool DataElement::from_transfer_syntax_uid(uid::WellKnown uid) {
	if (!uid.valid() || uid.uid_type() != UidType::TransferSyntax) {
		return report_from_assignment_failure(
		    "DataElement::from_transfer_syntax_uid", *this,
		    "uid must be a valid Transfer Syntax UID");
	}
	return from_uid(uid);
}

bool DataElement::from_sop_class_uid(uid::WellKnown uid) {
	if (!uid.valid()) {
		return report_from_assignment_failure(
		    "DataElement::from_sop_class_uid", *this, "uid must be valid");
	}
	const auto type = uid.uid_type();
	if (type != UidType::SopClass && type != UidType::MetaSopClass) {
		return report_from_assignment_failure(
		    "DataElement::from_sop_class_uid", *this,
		    "uid must be SOP Class or Meta SOP Class");
	}
	return from_uid(uid);
}

int DataElement::vm() const {
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
	return charset::raw_element_as_owned_utf8_value(*this, errors, nullptr, out_replaced);
}

std::optional<std::vector<std::string>> DataElement::to_utf8_strings(
    CharsetDecodeErrorPolicy errors, bool* out_replaced) const {
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
		return report_from_assignment_failure(
		    "DataElement::from_person_name", *this, "PN VR required for from_person_name");
	}
	return from_utf8_view(value.to_dicom_string(), errors, out_replaced);
}

bool DataElement::from_person_names(
    std::span<const PersonName> values, CharsetEncodeErrorPolicy errors,
    bool* out_replaced) {
	if (vr_ != dicom::VR::PN) {
		return report_from_assignment_failure(
		    "DataElement::from_person_names", *this, "PN VR required for from_person_names");
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
