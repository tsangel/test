#include <dicom.h>
#include <dicom_endian.h>
#include <diagnostics.h>

#include <cctype>
#include <charconv>
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

inline bool element_value_is_little_endian(const DataElement& elem) noexcept {
	const auto* parent = elem.parent();
	return parent ? parent->is_little_endian() : true;
}

template <typename T>
std::optional<std::vector<T>> load_numeric_vector(const DataElement& elem) {
    static_assert(std::is_integral_v<T> || std::is_floating_point_v<T>, "numeric load requires arithmetic type");
	const auto span = elem.value_span();
	if (span.empty()) return std::nullopt;
	if (span.size() % sizeof(T) != 0) return std::nullopt;

	const bool little_endian = element_value_is_little_endian(elem);
	const auto count = span.size() / sizeof(T);
	std::vector<T> out;
	out.reserve(count);
	for (std::size_t i = 0; i < count; ++i) {
		const auto* ptr = span.data() + i * sizeof(T);
        if constexpr (std::is_floating_point_v<T>) {
            using Bits = std::conditional_t<sizeof(T)==4, std::uint32_t, std::uint64_t>;
            const Bits bits = endian::load_value<Bits>(ptr, little_endian);
            out.push_back(std::bit_cast<T>(bits));
        } else {
		    out.push_back(endian::load_value<T>(ptr, little_endian));
        }
	}
	return out;
}

template <typename Parser>
std::optional<std::vector<typename Parser::result_type>> parse_string_numbers(const DataElement& elem,
    Parser parser) {
	const auto span = elem.value_span();
	if (span.empty()) return std::nullopt;
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
	if (storage_.ptr) {
		return std::span<const std::uint8_t>(
		    static_cast<const std::uint8_t*>(storage_.ptr), length_);
	}
	if (!parent_) {
		return {};
	}
	return parent_->stream().get_span(offset_, length_);
}

void* DataElement::value_ptr() const {
	if (storage_.ptr) {
		return storage_.ptr;
	}
	if (!parent_) {
		return nullptr;
	}
	return parent_->stream().get_pointer(offset_, length_);
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

std::optional<std::vector<std::uint16_t>> DataElement::as_uint16_vector() const {
	if (vr_ == VR::None) return std::nullopt;
	const auto vec = load_numeric_vector<std::uint16_t>(*this);
	return vec;
}

std::optional<std::vector<std::uint8_t>> DataElement::as_uint8_vector() const {
	if (vr_ == VR::None) return std::nullopt;
	const auto span = value_span();
	std::vector<std::uint8_t> out;
	out.reserve(span.size());
	for (auto b : span) out.push_back(static_cast<std::uint8_t>(b));
	return out;
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
	const bool little_endian = element_value_is_little_endian(elem);
	const auto* ptr = span.data();
	if constexpr (std::is_floating_point_v<T>) {
		using Bits = std::conditional_t<sizeof(T)==4, std::uint32_t, std::uint64_t>;
		const Bits bits = endian::load_value<Bits>(ptr, little_endian);
		return std::bit_cast<T>(bits);
	} else {
		return endian::load_value<T>(ptr, little_endian);
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
	if (span.empty() || span.size() % 4 != 0) return std::nullopt;
	const bool little_endian = element_value_is_little_endian(*this);
	const auto count = span.size() / 4;
	std::vector<Tag> out;
	out.reserve(count);
	for (std::size_t i = 0; i < count; ++i) {
		const auto* ptr = span.data() + i * 4;
		const std::uint16_t g = endian::load_value<std::uint16_t>(ptr, little_endian);
		const std::uint16_t e = endian::load_value<std::uint16_t>(ptr + 2, little_endian);
		out.emplace_back(g, e);
	}
	return out;
}

std::optional<Tag> DataElement::to_tag() const {
	if (vr_ != dicom::VR::AT) return std::nullopt;
	const auto span = value_span();
	if (span.size() < 4) return std::nullopt;
	const bool little_endian = element_value_is_little_endian(*this);
	const std::uint16_t g = endian::load_value<std::uint16_t>(span.data(), little_endian);
	const std::uint16_t e = endian::load_value<std::uint16_t>(span.data() + 2, little_endian);
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

std::optional<std::string_view> DataElement::to_utf8_view() const {
	return to_string_view();
}

std::optional<std::vector<std::string_view>> DataElement::to_utf8_views() const {
	return to_string_views();
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
