#pragma once

#include <array>
#include <cstddef>
#include <compare>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <iterator>
#include <iosfwd>
#include <map>
#include <memory>
#include <cstdio>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <optional>
#include <source_location>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "instream.h"
#include "dicom_endian.h"
#include "dataelement_lookup_detail.hpp"
#include "specific_character_set_registry.hpp"
#include "dicom_const.h"
#include "uid_lookup_detail.hpp"

namespace dicom {

class DataSet;
class DataElement;
enum class CharsetEncodeErrorPolicy : std::uint8_t;

namespace charset {
bool encode_utf8_for_element(DataElement& element, std::span<const std::string_view> values,
    CharsetEncodeErrorPolicy errors, std::string* out_error, bool* out_replaced);
}

namespace charset::detail {
struct ParsedSpecificCharacterSet;
using CharsetSpec = ParsedSpecificCharacterSet;
[[nodiscard]] const CharsetSpec* parse_dataset_charset(
    const DataSet& dataset, std::string* out_error);
[[nodiscard]] bool apply_declared_charset(
    DataSet& dataset, const ParsedSpecificCharacterSet& parsed, std::string* out_error);
[[nodiscard]] bool rewrite_charset_values(
    DataSet& dataset, const ParsedSpecificCharacterSet& target_charset,
    CharsetEncodeErrorPolicy errors, std::string* out_error, bool* out_replaced);
}

using std::uint8_t;
using std::uint16_t;

class InStream;

/// DICOM tag wrapper (16-bit group + 16-bit element packed into 32 bits).
struct Tag {
	std::uint32_t packed{0};

	constexpr Tag() = default;
	constexpr Tag(std::uint16_t group, std::uint16_t element)
	    : packed((static_cast<std::uint32_t>(group) << 16) | static_cast<std::uint32_t>(element)) {}
	constexpr explicit Tag(std::uint32_t value) : packed(value) {}
	constexpr explicit Tag(std::string_view text)
	    : packed(tag_value_from_text(text)) {}

	static constexpr Tag from_value(std::uint32_t value) { return Tag(value); }

	[[nodiscard]] constexpr std::uint16_t group() const {
		return static_cast<std::uint16_t>(packed >> 16);
	}

	[[nodiscard]] constexpr std::uint16_t element() const {
		return static_cast<std::uint16_t>(packed & 0xFFFFu);
	}

	[[nodiscard]] constexpr std::uint32_t value() const { return packed; }
	[[nodiscard]] inline std::string to_string() const {
		char buf[12]; // "(FFFF,FFFF)" + null
		std::snprintf(buf, sizeof(buf), "(%04X,%04X)", group(), element());
		return std::string{buf};
	}

	[[nodiscard]] constexpr bool is_private() const { return (group() & 0x0001u) != 0; }
	[[nodiscard]] constexpr explicit operator bool() const { return packed != 0; }

    // ------------------------------------------------------------
    // Comparisons and conversions
    // ------------------------------------------------------------
    constexpr explicit operator uint32_t() const noexcept { return packed; }
    constexpr auto operator<=>(const Tag&) const noexcept = default;
    constexpr bool operator==(const Tag&) const noexcept = default;

private:
	static constexpr bool is_hex_digit(char c) noexcept {
		return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
	}

	static constexpr std::uint8_t hex_digit_value(char c) noexcept {
		return (c >= '0' && c <= '9')
		           ? static_cast<std::uint8_t>(c - '0')
		           : static_cast<std::uint8_t>((c & 0xDF) - 'A' + 10);
	}

	static constexpr bool try_parse_numeric_tag(std::string_view text, std::uint32_t& value) noexcept {
		std::uint32_t accum = 0;
		int digits = 0;
		for (char c : text) {
			if (c == '(' || c == ')' || c == ',' || c == ' ' || c == '	') {
				continue;
			}
			if (!is_hex_digit(c)) {
				return false;
			}
			accum = (accum << 4) | hex_digit_value(c);
			++digits;
			if (digits > 8) {
				return false;
			}
		}
		if (digits == 8) {
			value = accum;
			return true;
		}
		return false;
	}

	static constexpr bool looks_like_numeric_tag_text(std::string_view text) noexcept {
		bool saw_delimiter = false;
		int digits = 0;
		for (char c : text) {
			if (c == '(' || c == ')' || c == ',' || c == ' ' || c == '\t') {
				saw_delimiter = true;
				continue;
			}
			if (!is_hex_digit(c)) {
				return false;
			}
			++digits;
			if (digits > 8) {
				return false;
			}
		}
		return digits == 8 && (saw_delimiter || text.size() == 8);
	}

	static constexpr bool looks_like_common_numeric_tag_text(std::string_view text) noexcept {
		return text.size() == 8 || (text.size() == 9 && text[4] == ',') ||
		       (text.size() == 11 && text.front() == '(' && text[5] == ',' &&
		        text.back() == ')');
	}

	static constexpr std::uint32_t tag_value_from_text(std::string_view text) {
		std::uint32_t numeric_value = 0;
		if (looks_like_common_numeric_tag_text(text) && try_parse_numeric_tag(text, numeric_value)) {
			return numeric_value;
		}
		if (std::is_constant_evaluated()) {
			if (const auto* entry = lookup::keyword_to_entry_chd(text)) {
				return entry->tag_value;
			}
		} else {
			if (const auto* entry = lookup::keyword_to_entry_runtime(text)) {
				return entry->tag_value;
			}
		}
		if (looks_like_numeric_tag_text(text) &&
		    try_parse_numeric_tag(text, numeric_value)) {
			return numeric_value;
		}
		throw std::invalid_argument("Unknown DICOM keyword");
	}
};

static_assert(sizeof(Tag) == 4, "Tag must remain 4 bytes");
static_assert(alignof(Tag) == alignof(std::uint32_t), "Tag alignment must match uint32_t");
static_assert(std::is_standard_layout_v<Tag>, "Tag should be standard-layout");
static_assert(std::is_trivially_copyable_v<Tag>, "Tag should be trivially copyable");
// static_assert(std::is_trivial_v<Tag>, "Tag should be trivial");

namespace endian {

// Load a Tag assuming little-endian source bytes.
inline Tag load_tag_le(const void* ptr) noexcept {
	const auto* byte_ptr = static_cast<const std::uint8_t*>(ptr);
	const auto group = load_le<std::uint16_t>(byte_ptr);
	const auto element = load_le<std::uint16_t>(byte_ptr + 2);
	return Tag(group, element);
}

}  // namespace endian

namespace uid {

struct Generated;

namespace detail {
constexpr bool is_valid_uid_char(char ch) noexcept {
	return (ch >= '0' && ch <= '9') || ch == '.';
}

constexpr bool is_valid_uid_text(std::string_view text) noexcept {
	if (text.empty() || text.size() > 64) {
		return false;
	}
	for (char ch : text) {
		if (!is_valid_uid_char(ch)) {
			return false;
		}
	}
	return true;
}

// Internal fast path for callers that already hold a strict-valid root.
[[nodiscard]] std::optional<Generated> try_generate_uid_validated_root(
    std::string_view root) noexcept;
// Internal deterministic candidate generator for callers that already hold a
// strict-valid root. Same `(root, key)` yields the same candidate UID.
[[nodiscard]] std::optional<Generated> try_generate_uid_validated_root_from(
    std::string_view root,
    std::string_view key) noexcept;
}  // namespace detail

struct WellKnown {
	std::uint16_t index{uid_lookup::kInvalidUidIndex};

	[[nodiscard]] constexpr bool valid() const noexcept {
		return index != uid_lookup::kInvalidUidIndex;
	}

	[[nodiscard]] constexpr explicit operator bool() const noexcept { return valid(); }
	[[nodiscard]] constexpr std::uint16_t raw_index() const noexcept { return index; }

	[[nodiscard]] constexpr const UidEntry* entry() const noexcept {
		return valid() ? uid_lookup::entry_from_index(index) : nullptr;
	}

	[[nodiscard]] constexpr std::string_view value() const noexcept {
		if (const auto* e = entry()) {
			return e->value;
		}
		return {};
	}

	[[nodiscard]] constexpr std::string_view keyword() const noexcept {
		if (const auto* e = entry()) {
			return e->keyword;
		}
		return {};
	}

	[[nodiscard]] constexpr std::string_view name() const noexcept {
		if (const auto* e = entry()) {
			return e->name;
		}
		return {};
	}

	[[nodiscard]] constexpr std::string_view type() const noexcept {
		if (const auto* e = entry()) {
			return e->type;
		}
		return {};
	}

	[[nodiscard]] constexpr UidType uid_type() const noexcept {
		if (const auto* e = entry()) {
			return e->uid_type;
		}
		return UidType::Other;
	}

	// Transfer Syntax classification helpers (UID index-based; no string compares).
	[[nodiscard]] constexpr bool is_jpeg_baseline() const noexcept;
	[[nodiscard]] constexpr bool is_jpeg_lossless() const noexcept;
	[[nodiscard]] constexpr bool is_jpegls() const noexcept;
	[[nodiscard]] constexpr bool is_jpeg_family() const noexcept;
	[[nodiscard]] constexpr bool is_jpeg2000() const noexcept;
	[[nodiscard]] constexpr bool is_htj2k() const noexcept;
	[[nodiscard]] constexpr bool is_jpegxl() const noexcept;
	[[nodiscard]] constexpr bool is_rle() const noexcept;
	[[nodiscard]] constexpr bool is_uncompressed() const noexcept;
	[[nodiscard]] constexpr bool is_encapsulated() const noexcept;
	[[nodiscard]] constexpr bool uses_explicit_vr() const noexcept;
	[[nodiscard]] constexpr bool is_lossless() const noexcept;
	[[nodiscard]] constexpr bool is_lossy() const noexcept;
	[[nodiscard]] constexpr bool supports_pixel_encode() const noexcept;
	[[nodiscard]] constexpr bool supports_pixel_decode() const noexcept;
	[[nodiscard]] constexpr bool is_mpeg2() const noexcept;
	[[nodiscard]] constexpr bool is_h264() const noexcept;
	[[nodiscard]] constexpr bool is_hevc() const noexcept;
	[[nodiscard]] constexpr bool ends_with_ffd9_marker() const noexcept;

	friend constexpr auto operator<=>(const WellKnown&, const WellKnown&) = default;
};

struct Generated {
	static constexpr std::size_t max_str_length = 64;
	using size_type = std::uint8_t;

	size_type length{0};
	std::array<char, max_str_length + 1> buffer{};  // null-terminated

	[[nodiscard]] std::string_view value() const noexcept {
		return std::string_view{buffer.data(), length};
	}

	// Append one numeric UID component.
	[[nodiscard]] std::optional<Generated> try_append(std::uint64_t component) const noexcept;
	[[nodiscard]] Generated append(std::uint64_t component) const;
};

inline constexpr std::optional<WellKnown> from_index(std::uint16_t idx) noexcept {
	if (idx == uid_lookup::kInvalidUidIndex) {
		return std::nullopt;
	}
	return WellKnown{idx};
}

inline constexpr std::optional<WellKnown> from_value(std::string_view value) noexcept {
	return from_index(uid_lookup::uid_index_from_value(value));
}

inline constexpr std::optional<WellKnown> from_keyword(std::string_view keyword) noexcept {
	return from_index(uid_lookup::uid_index_from_keyword(keyword));
}

inline constexpr std::optional<WellKnown> lookup(std::string_view text) noexcept {
	return from_index(uid_lookup::uid_index_from_text(text));
}

inline WellKnown lookup_or_throw(std::string_view text) {
	if (auto wk = lookup(text)) {
		return *wk;
	}
	throw std::invalid_argument("Unknown DICOM UID");
}

inline std::optional<Generated> make_generated(std::string_view value) {
	if (!detail::is_valid_uid_text(value)) {
		return std::nullopt;
	}
	Generated uid;
	uid.length = static_cast<Generated::size_type>(value.size());
	std::memcpy(uid.buffer.data(), value.data(), value.size());
	uid.buffer[uid.length] = '\0';
	return uid;
}

// UID constants are defined in dicom_const.h.
static_assert(
    kUidPrefix.size() <= Generated::max_str_length - (1 + 29),
    "kUidPrefix is too long for generate_uid() output format");

[[nodiscard]] constexpr std::string_view uid_prefix() noexcept {
	return kUidPrefix;
}

[[nodiscard]] constexpr std::string_view implementation_class_uid() noexcept {
	return kImplementationClassUid;
}

[[nodiscard]] constexpr std::string_view implementation_version_name() noexcept {
	return kImplementationVersionName;
}

// Strict UID validator for generated/written UIDs.
// Unlike detail::is_valid_uid_text, this enforces component syntax:
//  - no leading/trailing dot
//  - no empty component
//  - each component is digits only
//  - multi-digit components must not start with '0'
[[nodiscard]] bool is_valid_uid_text_strict(std::string_view text) noexcept;

// Normalize UID text by trimming DICOM UI padding:
//  - leading spaces
//  - trailing spaces / NUL bytes
[[nodiscard]] std::string normalize_uid_text(std::string_view text);
[[nodiscard]] std::string normalize_uid_bytes(std::span<const std::uint8_t> bytes);

// Build "<root>.<suffix>" while enforcing 64-byte UID length and strict syntax.
[[nodiscard]] std::optional<Generated> make_uid_with_suffix(
    std::string_view root, std::uint64_t suffix) noexcept;

// Convenience overload using DicomSDL UID prefix.
[[nodiscard]] std::optional<Generated> make_uid_with_suffix(std::uint64_t suffix) noexcept;

// Generate monotonic UIDs under DicomSDL prefix.
[[nodiscard]] std::optional<Generated> try_generate_uid(
    std::string_view root = uid_prefix()) noexcept;
[[nodiscard]] std::optional<Generated> try_generate_uid_from(
    std::string_view key,
    std::string_view root = uid_prefix()) noexcept;
[[nodiscard]] Generated generate_uid(
    std::string_view root = uid_prefix());
[[nodiscard]] Generated generate_uid_from(
    std::string_view key,
    std::string_view root = uid_prefix());
[[nodiscard]] Generated generate_sop_instance_uid();
[[nodiscard]] Generated generate_series_instance_uid();
[[nodiscard]] Generated generate_study_instance_uid();

}  // namespace uid

class UidRemapper {
public:
	/// Open a journal-backed in-memory UID remapper.
	///
	/// New mappings are appended to `journal_path`, and reopening the same journal
	/// replays prior `source_uid -> target_uid` pairs so later processes can reuse
	/// the same anonymized UIDs. When `flush_on_each_insert` is true, each new
	/// mapping is flushed immediately after append. Disabling per-insert flush can
	/// improve miss-path performance, but callers should then catch exceptions and
	/// prefer an explicit `close()` during orderly shutdown so buffered mappings
	/// are pushed to disk before process exit.
	[[nodiscard]] static UidRemapper in_memory(
	    const std::filesystem::path& journal_path,
	    std::string_view uid_root = uid::uid_prefix(),
	    bool flush_on_each_insert = true);

	UidRemapper() = default;

	/// Return the existing mapping for `source_uid`, or create and persist a new one.
	[[nodiscard]] std::string map_uid(std::string_view source_uid);

	/// Flush pending journal state and release the underlying journal handle.
	///
	/// Callers should prefer an explicit `close()` during orderly shutdown so I/O
	/// errors surface before process exit and any file/db handle is released
	/// deterministically. Abnormal termination can still lose the most recent
	/// writes. This is especially important when per-insert flush is disabled,
	/// because the most recent buffered mappings may not yet be visible in the
	/// journal unless `close()` (or an equivalent normal stream shutdown path)
	/// completes successfully.
	void close();

	[[nodiscard]] bool is_valid() const noexcept;

private:
	class Impl;
	explicit UidRemapper(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

	std::shared_ptr<Impl> impl_;
};

static_assert(uid::Generated::max_str_length <= std::numeric_limits<uid::Generated::size_type>::max(),
	"uid::Generated::size_type must fit max_str_length");

/// Packs two ASCII characters into a 16-bit value.
/// Example: 'A','E' -> 0x4145
constexpr uint16_t pack2(char a, char b) noexcept {
    return (uint16_t(uint8_t(a)) << 8) | uint16_t(uint8_t(b));
}

/// Represents a DICOM Value Representation (VR).
/// Internally stores a compact encoded form:
///  - known VR -> small integer 1..32
///  - unknown VR -> 16-bit raw two-character code ('X''Y')
struct VR {
    uint16_t value = 0;  ///< compact encoded value (0 = NONE, 1..32 = known VR, otherwise raw code)

    // ------------------------------------------------------------
    // Constructors
    // ------------------------------------------------------------
    constexpr VR() noexcept = default;
    constexpr explicit VR(uint16_t v) noexcept : value(v ? v : None_val) {}

    /// Construct from two VR characters, e.g. VR('P','N')
    constexpr VR(char a, char b) noexcept {
        const uint16_t raw = pack2(a,b);
        const uint16_t val = raw_to_val(raw);
        value = val ? val : (raw ? raw : None_val);
    }

    /// Construct from string_view (uses first two chars)
    constexpr explicit VR(std::string_view s) noexcept {
        if (s.size() >= 2) {
            const uint16_t raw = pack2(s[0], s[1]);
            const uint16_t val = raw_to_val(raw);
            value = val ? val : (raw ? raw : None_val);
        } else {
            value = None_val;
        }
    }

    // ------------------------------------------------------------
    // Comparisons and conversions
    // ------------------------------------------------------------
    constexpr explicit operator uint16_t() const noexcept { return value; }
    constexpr auto operator<=>(const VR&) const noexcept = default;
    constexpr bool operator==(const VR&) const noexcept = default;

    // ------------------------------------------------------------
    // Basic utilities
    // ------------------------------------------------------------
    constexpr bool     is_known() const noexcept { return value >= AE_val && value < Unknown_val; }
	    constexpr uint16_t val()      const noexcept { return is_known() ? value : 0; }
	    constexpr uint16_t raw_code() const noexcept { return is_known() ? val_to_raw[value] : value; }
		constexpr char first() const noexcept { return static_cast<char>(raw_code() >> 8); }
		constexpr char second() const noexcept { return static_cast<char>(raw_code() & 0xFF); }

		/// Returns the two-character VR string or "??" for unknown
		constexpr std::string_view str() const noexcept {
			switch (value) {
			case AE_val: return "AE"; case AS_val: return "AS";
			case AT_val: return "AT"; case CS_val: return "CS";
			case DA_val: return "DA"; case DS_val: return "DS";
			case DT_val: return "DT"; case FD_val: return "FD";
			case FL_val: return "FL"; case IS_val: return "IS";
			case LO_val: return "LO"; case LT_val: return "LT";
			case OB_val: return "OB"; case OD_val: return "OD";
			case OF_val: return "OF"; case OV_val: return "OV";
			case OL_val: return "OL"; case OW_val: return "OW";
			case PN_val: return "PN"; case SH_val: return "SH";
			case SL_val: return "SL"; case SQ_val: return "SQ";
			case SS_val: return "SS"; case ST_val: return "ST";
			case SV_val: return "SV"; case TM_val: return "TM";
			case UC_val: return "UC"; case UI_val: return "UI";
			case UL_val: return "UL"; case UN_val: return "UN";
			case UR_val: return "UR"; case US_val: return "US";
			case UT_val: return "UT"; case UV_val: return "UV";
			case PX_val: return "PX";
			default: return "??";
		}
	}

    // ------------------------------------------------------------
    // VR classification
    // ------------------------------------------------------------
    constexpr bool is_string() const noexcept {
        if (!is_known()) return false;
        switch (value) {
            case AE_val: case AS_val:
            case CS_val: case DA_val: case DS_val: case DT_val:
            case IS_val: case LO_val: case LT_val:
            case PN_val: case SH_val: case ST_val:
            case TM_val: case UC_val: case UI_val:
            case UR_val: case UT_val:
                return true;
            default: return false;
        }
    }

	constexpr bool is_binary() const noexcept {
		if (!is_known()) return false;
		switch (value) {
	case AT_val:
			case OB_val: case OD_val: case OF_val: case OV_val: case OL_val: case OW_val:
			case UN_val: case US_val: case SS_val: case SV_val: case UV_val: case PX_val:
			case UL_val: case SL_val: case FL_val: case FD_val:
				return true;
			default: return false;
		}
	}

	constexpr bool is_sequence() const noexcept {
		return value == SQ_val;
	}

	constexpr bool is_pixel_sequence() const noexcept {
		return value == PX_val;
	}

	constexpr bool uses_specific_character_set() const noexcept {
		if (!is_known() || !is_string()) return false;
		switch (value) {
			case LO_val:
			case LT_val:
			case PN_val:
			case SH_val:
			case ST_val:
			case UC_val:
			case UT_val:
				return true;
			default:
				return false;
		}
	}

	constexpr bool allows_multiple_text_values() const noexcept {
		if (!is_known() || !is_string()) return false;
		switch (value) {
			case LT_val:
			case ST_val:
			case UT_val:
			case UR_val:
				return false;
			default:
				return true;
		}
	}

    // ------------------------------------------------------------
    // Padding rules
    // ------------------------------------------------------------
    /// DICOM requires even-length value fields for all VRs
    static constexpr bool pad_to_even() noexcept { return true; }

    /// Returns the padding byte (0x20 for text, 0x00 for binary/UI)
	constexpr uint8_t padding_byte() const noexcept {
		if (!is_known()) return 0x00;
		switch (value) {
			case UI_val: return 0x00;
			case AT_val: case OB_val: case OD_val: case OF_val:
			case OL_val: case OW_val: case UN_val: case SQ_val: case PX_val:
				return 0x00;
			default: return 0x20;
		}
	}

    // ------------------------------------------------------------
    // Explicit VR encoding: 16-bit VL usage
    // part05. Table 7.1-2. Data Element with Explicit VR of AE, AS, AT, CS,
	// DA, DS, DT, FL, FD, IS, LO, LT, PN, SH, SL, SS, ST, TM, UI, UL and US
    //      -> use 16-bit VL field
    // ------------------------------------------------------------
    constexpr bool uses_explicit_16bit_vl() const noexcept {
        if (!is_known()) return false;
        switch (value) {
            case AE_val: case AS_val: case AT_val: case CS_val:
            case DA_val: case DS_val: case DT_val: case FL_val:
            case FD_val: case IS_val: case LO_val: case LT_val:
            case PN_val: case SH_val: case SL_val: case SS_val:
            case ST_val: case TM_val: case UI_val: case UL_val:
            case US_val:
                return true;
            default:
                return false;
        }
    }

    // ------------------------------------------------------------
    // Fixed element size (0 = variable length)
    // ------------------------------------------------------------
    constexpr int fixed_length() const noexcept {
        if (!is_known()) return 0;
        switch (value) {
            case US_val: case SS_val: return 2;
            case AT_val: case UL_val: case SL_val: case FL_val: return 4;
            case FD_val: case SV_val: case UV_val: return 8;
            default: return 0;
        }
    }

    // ------------------------------------------------------------
    // Factory helpers
    // ------------------------------------------------------------
    constexpr static VR from_string(std::string_view s) noexcept { return VR{s}; }
    constexpr static VR from_chars(char a, char b) noexcept { return VR{a,b}; }

    // ------------------------------------------------------------
    // Compact integer IDs (1..32)
    // ------------------------------------------------------------
    enum : uint16_t {
        None_val = 0, // sentinel for no VR
        AE_val=1, AS_val, AT_val, CS_val, DA_val, DS_val, DT_val, FD_val,
        FL_val, IS_val, LO_val, LT_val, OB_val, OD_val, OF_val, OV_val,
        OL_val, OW_val, PN_val, SH_val, SL_val, SQ_val, SS_val, ST_val,
        SV_val, TM_val, UC_val, UI_val, UL_val, UN_val, UR_val, US_val,
        UT_val, UV_val, PX_val, Unknown_val // sentinel upper bound for is_known()
    };

    // ------------------------------------------------------------
    // Constant VR objects
    // ------------------------------------------------------------
    static const VR AE;
    static const VR AS;
    static const VR AT;
    static const VR CS;
    static const VR DA;
    static const VR DS;
    static const VR DT;
    static const VR FD;
    static const VR FL;
    static const VR IS;
    static const VR LO;
    static const VR LT;
    static const VR OB;
    static const VR OD;
    static const VR OF;
    static const VR OV;
    static const VR OL;
    static const VR OW;
    static const VR PN;
    static const VR SH;
    static const VR SL;
    static const VR SQ;
    static const VR SS;
    static const VR ST;
    static const VR SV;
    static const VR TM;
    static const VR UC;
    static const VR UI;
    static const VR UL;
	static const VR UN;
	static const VR UR;
	static const VR US;
	static const VR UT;
    static const VR UV;
    static const VR PX;
    static const VR None;

private:
    // Mapping table from compact ID -> raw 2-char code
    inline static constexpr std::array<uint16_t, PX_val + 2> val_to_raw = {
        0,
        pack2('A','E'), pack2('A','S'), pack2('A','T'), pack2('C','S'),
        pack2('D','A'), pack2('D','S'), pack2('D','T'), pack2('F','D'),
        pack2('F','L'), pack2('I','S'), pack2('L','O'), pack2('L','T'),
        pack2('O','B'), pack2('O','D'), pack2('O','F'), pack2('O','V'),
        pack2('O','L'), pack2('O','W'), pack2('P','N'), pack2('S','H'),
        pack2('S','L'), pack2('S','Q'), pack2('S','S'), pack2('S','T'),
        pack2('S','V'), pack2('T','M'), pack2('U','C'), pack2('U','I'),
        pack2('U','L'), pack2('U','N'), pack2('U','R'), pack2('U','S'),
        pack2('U','T'), pack2('U','V'), pack2('P','X'), 0
    };

    /// Maps raw 16-bit code -> small integer (1..34) or 0 if unknown.
    static constexpr uint16_t raw_to_val(uint16_t raw) noexcept {
        switch (raw) {
            case pack2('A','E'): return AE_val; case pack2('A','S'): return AS_val;
            case pack2('A','T'): return AT_val; case pack2('C','S'): return CS_val;
            case pack2('D','A'): return DA_val; case pack2('D','S'): return DS_val;
            case pack2('D','T'): return DT_val; case pack2('F','D'): return FD_val;
            case pack2('F','L'): return FL_val; case pack2('I','S'): return IS_val;
            case pack2('L','O'): return LO_val; case pack2('L','T'): return LT_val;
            case pack2('O','B'): return OB_val; case pack2('O','D'): return OD_val;
            case pack2('O','F'): return OF_val; case pack2('O','V'): return OV_val;
            case pack2('O','L'): return OL_val; case pack2('O','W'): return OW_val;
            case pack2('P','N'): return PN_val; case pack2('S','H'): return SH_val;
            case pack2('S','L'): return SL_val; case pack2('S','Q'): return SQ_val;
            case pack2('S','S'): return SS_val; case pack2('S','T'): return ST_val;
            case pack2('S','V'): return SV_val; case pack2('T','M'): return TM_val;
            case pack2('U','C'): return UC_val; case pack2('U','I'): return UI_val;
            case pack2('U','L'): return UL_val; case pack2('U','N'): return UN_val;
            case pack2('U','R'): return UR_val; case pack2('U','S'): return US_val;
            case pack2('U','T'): return UT_val; case pack2('U','V'): return UV_val;
            case pack2('P','X'): return PX_val;
            default: return 0;
        }
    }
};

static_assert(sizeof(VR) == 2, "VR must remain 2 bytes");
static_assert(alignof(VR) == alignof(uint16_t), "VR alignment must match uint16_t");
static_assert(std::is_standard_layout_v<VR>, "VR should be standard-layout");
static_assert(std::is_trivially_copyable_v<VR>, "VR should be trivially copyable");
// static_assert(std::is_trivial_v<VR>, "VR should be trivial");

inline constexpr VR VR::AE{uint16_t(VR::AE_val)};
inline constexpr VR VR::AS{uint16_t(VR::AS_val)};
inline constexpr VR VR::AT{uint16_t(VR::AT_val)};
inline constexpr VR VR::CS{uint16_t(VR::CS_val)};
inline constexpr VR VR::DA{uint16_t(VR::DA_val)};
inline constexpr VR VR::DS{uint16_t(VR::DS_val)};
inline constexpr VR VR::DT{uint16_t(VR::DT_val)};
inline constexpr VR VR::FD{uint16_t(VR::FD_val)};
inline constexpr VR VR::FL{uint16_t(VR::FL_val)};
inline constexpr VR VR::IS{uint16_t(VR::IS_val)};
inline constexpr VR VR::LO{uint16_t(VR::LO_val)};
inline constexpr VR VR::LT{uint16_t(VR::LT_val)};
inline constexpr VR VR::OB{uint16_t(VR::OB_val)};
inline constexpr VR VR::OD{uint16_t(VR::OD_val)};
inline constexpr VR VR::OF{uint16_t(VR::OF_val)};
inline constexpr VR VR::OV{uint16_t(VR::OV_val)};
inline constexpr VR VR::OL{uint16_t(VR::OL_val)};
inline constexpr VR VR::OW{uint16_t(VR::OW_val)};
inline constexpr VR VR::PN{uint16_t(VR::PN_val)};
inline constexpr VR VR::SH{uint16_t(VR::SH_val)};
inline constexpr VR VR::SL{uint16_t(VR::SL_val)};
inline constexpr VR VR::SQ{uint16_t(VR::SQ_val)};
inline constexpr VR VR::SS{uint16_t(VR::SS_val)};
inline constexpr VR VR::ST{uint16_t(VR::ST_val)};
inline constexpr VR VR::SV{uint16_t(VR::SV_val)};
inline constexpr VR VR::TM{uint16_t(VR::TM_val)};
inline constexpr VR VR::UC{uint16_t(VR::UC_val)};
inline constexpr VR VR::UI{uint16_t(VR::UI_val)};
inline constexpr VR VR::UL{uint16_t(VR::UL_val)};
inline constexpr VR VR::UN{uint16_t(VR::UN_val)};
inline constexpr VR VR::UR{uint16_t(VR::UR_val)};
inline constexpr VR VR::US{uint16_t(VR::US_val)};
inline constexpr VR VR::UT{uint16_t(VR::UT_val)};
inline constexpr VR VR::UV{uint16_t(VR::UV_val)};
inline constexpr VR VR::PX{uint16_t(VR::PX_val)};
inline constexpr VR VR::None{uint16_t(VR::None_val)};

namespace lookup {

constexpr std::pair<Tag, VR> keyword_to_tag_vr(std::string_view keyword) {
	const auto* entry = std::is_constant_evaluated() ? keyword_to_entry_chd(keyword)
	                                                 : keyword_to_entry_runtime(keyword);
	if (entry) {
		return {Tag::from_value(entry->tag_value), VR(entry->vr_value)};
	}
	return {Tag{}, VR{}};
}

constexpr std::string_view keyword_to_tag_text(std::string_view keyword) {
	const auto* entry = std::is_constant_evaluated() ? keyword_to_entry_chd(keyword)
	                                                 : keyword_to_entry_runtime(keyword);
	if (entry) {
		return entry->tag;
	}
	return {};
}

constexpr std::string_view tag_to_keyword(std::uint32_t tag_value) {
	const auto group = static_cast<std::uint16_t>(tag_value >> 16);
	const auto element = static_cast<std::uint16_t>(tag_value & 0xFFFFu);

	if (const auto* entry = tag_to_entry(tag_value)) {
		return entry->keyword;
	}

	if (element == 0) {
		return "(Group Length)";
	}

	if (group & 0x1u) {
		if (element >= 0x10 && element <= 0xFF) {
			return "(Private Creator Data Element)";
		}
		return "(Private Data Elements)";
	}

	return {};
}

constexpr std::uint16_t tag_to_vr(std::uint32_t tag_value) {
	const auto group = static_cast<std::uint16_t>(tag_value >> 16);
	const auto element = static_cast<std::uint16_t>(tag_value & 0xFFFFu);

	if (const auto* entry = tag_to_entry(tag_value)) {
		return entry->vr_value;
	}

	if (element == 0) {
		return VR::UL_val;
	}

	if (group & 0x1u) {
		if (element >= 0x10 && element <= 0xFF) {
			return VR::LO_val;
		}
	}

	return 0;
}

} // namespace lookup

namespace literals {

consteval Tag operator""_tag(const char* text, std::size_t len) {
	return Tag(std::string_view{text, len});
}

consteval uid::WellKnown operator""_uid(const char* text, std::size_t len) {
	const auto index = uid_lookup::uid_index_from_text(std::string_view{text, len});
	if (index == uid_lookup::kInvalidUidIndex) {
		throw "Unknown DICOM UID literal";
	}
	return uid::WellKnown{index};
}

} // namespace literals
static_assert(uid_lookup::uid_index_from_text("ImplicitVRLittleEndian") ==
			uid_lookup::uid_index_from_text("1.2.840.10008.1.2"),
	"UID keyword and value literals must resolve to the same registry entry");

// -- WellKnown transfer syntax helpers (definitions placed after _uid literal).
namespace uid {
using namespace dicom::literals;

// Transfer Syntax classification compressed into a single switch.
// Order follows uid_registry transfer syntax listing (with uncompressed first).
namespace detail {
constexpr std::uint32_t kTSUncompressed = 1u << 0;
constexpr std::uint32_t kTSEncapsulated = 1u << 1;
constexpr std::uint32_t kTSJpegBaseline = 1u << 2;
constexpr std::uint32_t kTSJpegLossless = 1u << 3;
constexpr std::uint32_t kTSJpegLS = 1u << 4;
constexpr std::uint32_t kTSJpeg2000 = 1u << 5;
constexpr std::uint32_t kTSHTJ2K = 1u << 6;
constexpr std::uint32_t kTSMpeg2 = 1u << 7;
constexpr std::uint32_t kTSH264 = 1u << 8;
constexpr std::uint32_t kTSHevc = 1u << 9;
constexpr std::uint32_t kTSJpegXL = 1u << 10;
constexpr std::uint32_t kTSRle = 1u << 11;
constexpr std::uint32_t kTSFfd9 = 1u << 12;      // Codestream ends with FFD9 marker
constexpr std::uint32_t kTSJpegFamily = 1u << 13;
constexpr std::uint32_t kTSLossless = 1u << 14;
constexpr std::uint32_t kTSLossy = 1u << 15;
constexpr std::uint32_t kTSPixelEncodeSupported = 1u << 16;
constexpr std::uint32_t kTSPixelDecodeSupported = 1u << 17;

inline constexpr std::uint32_t ts_mask(std::uint16_t idx) {
	switch (idx) {
	// Uncompressed image transfer syntaxes
	case "ImplicitVRLittleEndian"_uid.raw_index():
	case "ExplicitVRLittleEndian"_uid.raw_index():
	case "DeflatedExplicitVRLittleEndian"_uid.raw_index():
	case "ExplicitVRBigEndian"_uid.raw_index():
	case "Papyrus3ImplicitVRLittleEndian"_uid.raw_index():
		return kTSUncompressed | kTSLossless | kTSPixelEncodeSupported |
		    kTSPixelDecodeSupported;
	case "EncapsulatedUncompressedExplicitVRLittleEndian"_uid.raw_index():
		return kTSUncompressed | kTSEncapsulated | kTSLossless |
		    kTSPixelEncodeSupported | kTSPixelDecodeSupported;

	// JPEG Baseline / Extended / Progressive
	case "JPEGBaseline8Bit"_uid.raw_index():
	case "JPEGExtended12Bit"_uid.raw_index():
		return kTSJpegBaseline | kTSJpegFamily | kTSFfd9 | kTSEncapsulated |
		    kTSLossy | kTSPixelEncodeSupported | kTSPixelDecodeSupported;
	case "JPEGExtended35"_uid.raw_index():
	case "JPEGSpectralSelectionNonHierarchical68"_uid.raw_index():
	case "JPEGSpectralSelectionNonHierarchical79"_uid.raw_index():
	case "JPEGFullProgressionNonHierarchical1012"_uid.raw_index():
	case "JPEGFullProgressionNonHierarchical1113"_uid.raw_index():
	case "JPEGExtendedHierarchical1618"_uid.raw_index():
	case "JPEGExtendedHierarchical1719"_uid.raw_index():
	case "JPEGSpectralSelectionHierarchical2022"_uid.raw_index():
	case "JPEGSpectralSelectionHierarchical2123"_uid.raw_index():
	case "JPEGFullProgressionHierarchical2426"_uid.raw_index():
	case "JPEGFullProgressionHierarchical2527"_uid.raw_index():
		return kTSJpegBaseline | kTSJpegFamily | kTSFfd9 | kTSEncapsulated |
		    kTSLossy | kTSPixelDecodeSupported;

	// JPEG Lossless
	case "JPEGLossless"_uid.raw_index():
	case "JPEGLosslessNonHierarchical15"_uid.raw_index():
	case "JPEGLosslessSV1"_uid.raw_index():
		return kTSJpegLossless | kTSJpegFamily | kTSFfd9 | kTSEncapsulated |
		    kTSLossless | kTSPixelEncodeSupported | kTSPixelDecodeSupported;
	case "JPEGLosslessHierarchical28"_uid.raw_index():
	case "JPEGLosslessHierarchical29"_uid.raw_index():
		return kTSJpegLossless | kTSJpegFamily | kTSFfd9 | kTSEncapsulated |
		    kTSLossless | kTSPixelDecodeSupported;

	// JPEG-LS
	case "JPEGLSLossless"_uid.raw_index():
		return kTSJpegLS | kTSJpegFamily | kTSFfd9 | kTSEncapsulated |
		    kTSLossless | kTSPixelEncodeSupported | kTSPixelDecodeSupported;
	case "JPEGLSNearLossless"_uid.raw_index():
		return kTSJpegLS | kTSJpegFamily | kTSFfd9 | kTSEncapsulated |
		    kTSLossy | kTSPixelEncodeSupported | kTSPixelDecodeSupported;

	// JPEG 2000 / JPIP Referenced
	case "JPEG2000Lossless"_uid.raw_index():
	case "JPEG2000MCLossless"_uid.raw_index():
		return kTSJpeg2000 | kTSJpegFamily | kTSEncapsulated | kTSLossless |
		    kTSPixelEncodeSupported | kTSPixelDecodeSupported;
	case "JPEG2000"_uid.raw_index():
	case "JPEG2000MC"_uid.raw_index():
		return kTSJpeg2000 | kTSJpegFamily | kTSEncapsulated | kTSLossy |
		    kTSPixelEncodeSupported | kTSPixelDecodeSupported;
	case "JPIPReferenced"_uid.raw_index():
	case "JPIPReferencedDeflate"_uid.raw_index():
		return kTSJpeg2000 | kTSJpegFamily | kTSEncapsulated;

	// MPEG-2
	case "MPEG2MPML"_uid.raw_index():
	case "MPEG2MPMLF"_uid.raw_index():
	case "MPEG2MPHL"_uid.raw_index():
	case "MPEG2MPHLF"_uid.raw_index():
		return kTSMpeg2 | kTSEncapsulated | kTSLossy;

	// H.264 / MPEG-4 AVC
	case "MPEG4HP41"_uid.raw_index():
	case "MPEG4HP41F"_uid.raw_index():
	case "MPEG4HP41BD"_uid.raw_index():
	case "MPEG4HP41BDF"_uid.raw_index():
	case "MPEG4HP422D"_uid.raw_index():
	case "MPEG4HP422DF"_uid.raw_index():
	case "MPEG4HP423D"_uid.raw_index():
	case "MPEG4HP423DF"_uid.raw_index():
	case "MPEG4HP42STEREO"_uid.raw_index():
	case "MPEG4HP42STEREOF"_uid.raw_index():
		return kTSH264 | kTSEncapsulated | kTSLossy;

	// HEVC
	case "HEVCMP51"_uid.raw_index():
	case "HEVCM10P51"_uid.raw_index():
		return kTSHevc | kTSEncapsulated | kTSLossy;

	// JPEG XL
	case "JPEGXLLossless"_uid.raw_index():
		return kTSJpegXL | kTSJpegFamily | kTSEncapsulated |
		    kTSLossless | kTSPixelEncodeSupported | kTSPixelDecodeSupported;
	case "JPEGXLJPEGRecompression"_uid.raw_index():
		return kTSJpegXL | kTSJpegFamily | kTSEncapsulated |
		    kTSLossy | kTSPixelDecodeSupported;
	case "JPEGXL"_uid.raw_index():
		return kTSJpegXL | kTSJpegFamily | kTSEncapsulated |
		    kTSLossy | kTSPixelEncodeSupported | kTSPixelDecodeSupported;

	// HTJ2K codestream transfer syntaxes
	case "HTJ2KLossless"_uid.raw_index():
	case "HTJ2KLosslessRPCL"_uid.raw_index():
		return kTSJpeg2000 | kTSHTJ2K | kTSJpegFamily | kTSFfd9 |
		    kTSEncapsulated | kTSLossless | kTSPixelEncodeSupported |
		    kTSPixelDecodeSupported;
	case "HTJ2K"_uid.raw_index():
		return kTSJpeg2000 | kTSHTJ2K | kTSJpegFamily | kTSFfd9 |
		    kTSEncapsulated | kTSLossy | kTSPixelEncodeSupported |
		    kTSPixelDecodeSupported;

	// JPIP HTJ2K referenced transfer syntaxes
	case "JPIPHTJ2KReferenced"_uid.raw_index():
	case "JPIPHTJ2KReferencedDeflate"_uid.raw_index():
		return kTSJpeg2000 | kTSHTJ2K | kTSJpegFamily | kTSEncapsulated;

	// RLE
	case "RLELossless"_uid.raw_index():
		return kTSRle | kTSEncapsulated | kTSLossless |
		    kTSPixelEncodeSupported | kTSPixelDecodeSupported;

	default:
		return 0;
	}
}

}  // namespace detail

inline constexpr bool WellKnown::is_jpeg_baseline() const noexcept {
	return detail::ts_mask(raw_index()) & detail::kTSJpegBaseline;
}

inline constexpr bool WellKnown::is_jpeg_lossless() const noexcept {
	return detail::ts_mask(raw_index()) & detail::kTSJpegLossless;
}

inline constexpr bool WellKnown::is_jpegls() const noexcept {
	return detail::ts_mask(raw_index()) & detail::kTSJpegLS;
}

inline constexpr bool WellKnown::is_jpeg_family() const noexcept {
	return detail::ts_mask(raw_index()) & detail::kTSJpegFamily;
}

inline constexpr bool WellKnown::is_jpeg2000() const noexcept {
	return detail::ts_mask(raw_index()) & detail::kTSJpeg2000;
}

inline constexpr bool WellKnown::is_htj2k() const noexcept {
	return detail::ts_mask(raw_index()) & detail::kTSHTJ2K;
}

inline constexpr bool WellKnown::is_jpegxl() const noexcept {
	return detail::ts_mask(raw_index()) & detail::kTSJpegXL;
}

inline constexpr bool WellKnown::is_rle() const noexcept {
	return detail::ts_mask(raw_index()) & detail::kTSRle;
}

inline constexpr bool WellKnown::is_uncompressed() const noexcept {
	return detail::ts_mask(raw_index()) & detail::kTSUncompressed;
}

inline constexpr bool WellKnown::is_encapsulated() const noexcept {
	return detail::ts_mask(raw_index()) & detail::kTSEncapsulated;
}

inline constexpr bool WellKnown::uses_explicit_vr() const noexcept {
	return !valid() || (*this != "ImplicitVRLittleEndian"_uid &&
	    *this != "Papyrus3ImplicitVRLittleEndian"_uid);
}

inline constexpr bool WellKnown::is_lossless() const noexcept {
	return detail::ts_mask(raw_index()) & detail::kTSLossless;
}

inline constexpr bool WellKnown::is_lossy() const noexcept {
	return detail::ts_mask(raw_index()) & detail::kTSLossy;
}

inline constexpr bool WellKnown::supports_pixel_encode() const noexcept {
	return detail::ts_mask(raw_index()) & detail::kTSPixelEncodeSupported;
}

inline constexpr bool WellKnown::supports_pixel_decode() const noexcept {
	return detail::ts_mask(raw_index()) & detail::kTSPixelDecodeSupported;
}

inline constexpr bool WellKnown::is_mpeg2() const noexcept {
	return detail::ts_mask(raw_index()) & detail::kTSMpeg2;
}

inline constexpr bool WellKnown::is_h264() const noexcept {
	return detail::ts_mask(raw_index()) & detail::kTSH264;
}

inline constexpr bool WellKnown::is_hevc() const noexcept {
	return detail::ts_mask(raw_index()) & detail::kTSHevc;
}

inline constexpr bool WellKnown::ends_with_ffd9_marker() const noexcept {
	return detail::ts_mask(raw_index()) & detail::kTSFfd9;
}

} // namespace uid

class Sequence;
class PixelFrame;
class PixelSequence;
class DicomFile;
class DataSet;
/// Options controlling how a DataSet is read from a stream.
struct ReadOptions {
	Tag load_until{Tag(0xFFFFu, 0xFFFFu)};
	bool keep_on_error{false};
	bool copy{true};
};

struct WriteOptions {
	bool include_preamble{true};
	bool write_file_meta{true};
	// When false, write_* rebuilds (0002,eeee) before serialization.
	bool keep_existing_meta{true};
};

enum class CharsetEncodeErrorPolicy : std::uint8_t {
	strict = 0,
	replace_qmark,
	replace_unicode_escape,
};

enum class CharsetDecodeErrorPolicy : std::uint8_t {
	strict = 0,
	replace_fffd,
	replace_hex_escape,
};

/// One component group of a PN value.
/// Components are stored in canonical DICOM order:
/// component 0..4 = family-name-complex, given-name-complex, middle-name,
/// name-prefix, name-suffix for human use.
/// Veterinary use assigns different semantics to the first two components,
/// so component(index) is the neutral API. The aliases below are human-use
/// convenience accessors from PS3.5 6.2 / 6.2.1.2.
struct PersonNameGroup {
	std::array<std::string, 5> components{};
	// 0 means "infer from last non-empty component". Parsed values can use this
	// to preserve explicit trailing empty PN components such as "^^^^".
	std::uint8_t explicit_component_count_{0};

	[[nodiscard]] std::string_view component(std::size_t index) const noexcept {
		return index < components.size() ? std::string_view(components[index]) : std::string_view{};
	}
	[[nodiscard]] std::string to_dicom_string() const;
	[[nodiscard]] bool empty() const noexcept;

	[[nodiscard]] std::string_view family_name() const noexcept { return components[0]; }
	[[nodiscard]] std::string_view given_name() const noexcept { return components[1]; }
	[[nodiscard]] std::string_view middle_name() const noexcept { return components[2]; }
	[[nodiscard]] std::string_view name_prefix() const noexcept { return components[3]; }
	[[nodiscard]] std::string_view name_suffix() const noexcept { return components[4]; }
};

/// Parsed Person Name (PN) value with up to three component groups:
/// alphabetic, ideographic, phonetic.
struct PersonName {
	std::optional<PersonNameGroup> alphabetic{};
	std::optional<PersonNameGroup> ideographic{};
	std::optional<PersonNameGroup> phonetic{};

	[[nodiscard]] bool empty() const noexcept;
	[[nodiscard]] std::string to_dicom_string() const;
	[[nodiscard]] static std::optional<PersonName> parse(std::string_view utf8_value);
	[[nodiscard]] static std::optional<std::vector<PersonName>> parse_many(
	    std::span<const std::string> utf8_values);
	[[nodiscard]] static std::optional<std::vector<PersonName>> parse_many(
	    std::span<const std::string_view> utf8_values);

private:
	std::uint8_t explicit_group_count_{0};
};

namespace pixel {

enum class Planar : std::uint8_t {
	interleaved = 0,
	planar = 1,
};

enum class DataType : std::uint8_t {
	unknown = 0,
	u8,
	s8,
	u16,
	s16,
	u32,
	s32,
	f32,
	f64,
};

[[nodiscard]] inline constexpr std::size_t bytes_per_sample_of(DataType dtype) noexcept {
	switch (dtype) {
	case DataType::u8:
	case DataType::s8:
		return 1;
	case DataType::u16:
	case DataType::s16:
		return 2;
	case DataType::u32:
	case DataType::s32:
	case DataType::f32:
		return 4;
	case DataType::f64:
		return 8;
	default:
		return 0;
	}
}

enum class Photometric : std::uint8_t {
	monochrome1 = 0,
	monochrome2,
	palette_color,
	rgb,
	ybr_full,
	ybr_full_422,
	ybr_rct,
	ybr_ict,
};

[[nodiscard]] inline constexpr bool is_signed_integer_dtype(DataType dtype) noexcept {
	switch (dtype) {
	case DataType::s8:
	case DataType::s16:
	case DataType::s32:
		return true;
	default:
		return false;
	}
}

[[nodiscard]] inline constexpr std::uint16_t normalized_bits_stored_of(
    DataType dtype) noexcept {
	return static_cast<std::uint16_t>(bytes_per_sample_of(dtype) * std::size_t{8});
}

namespace detail {

[[nodiscard]] inline constexpr bool checked_mul_size_t(
    std::size_t lhs, std::size_t rhs, std::size_t& out) noexcept {
	if (lhs == 0 || rhs == 0) {
		out = 0;
		return true;
	}
	if (lhs > std::numeric_limits<std::size_t>::max() / rhs) {
		return false;
	}
	out = lhs * rhs;
	return true;
}

[[nodiscard]] inline constexpr std::size_t align_up_size_t_or_zero(
    std::size_t value, std::size_t alignment) noexcept {
	if (alignment <= std::size_t{1}) {
		return value;
	}
	const auto remainder = value % alignment;
	if (remainder == 0) {
		return value;
	}
	const auto increment = alignment - remainder;
	if (value > std::numeric_limits<std::size_t>::max() - increment) {
		return 0;
	}
	return value + increment;
}

} // namespace detail

struct PixelLayout {
	DataType data_type;
	Photometric photometric;
	Planar planar;
	std::uint8_t reserved;

	std::uint32_t rows;
	std::uint32_t cols;
	std::uint32_t frames;
	std::uint16_t samples_per_pixel;
	std::uint16_t bits_stored;

	std::size_t row_stride;
	std::size_t frame_stride;

	[[nodiscard]] constexpr bool empty() const noexcept {
		return data_type == DataType::unknown || rows == 0 || cols == 0 || frames == 0 ||
		    samples_per_pixel == 0 || row_stride == 0 || frame_stride == 0;
	}

	[[nodiscard]] constexpr std::size_t bytes_per_sample() const noexcept {
		return bytes_per_sample_of(data_type);
	}

	[[nodiscard]] constexpr int bits_allocated() const noexcept {
		return static_cast<int>(bytes_per_sample() * std::size_t{8});
	}

	[[nodiscard]] constexpr int high_bit() const noexcept {
		return bits_stored > 0 ? static_cast<int>(bits_stored - 1) : bits_allocated() - 1;
	}

	[[nodiscard]] constexpr int pixel_representation() const noexcept {
		return is_signed_integer_dtype(data_type) ? 1 : 0;
	}

	[[nodiscard]] constexpr PixelLayout single_frame() const noexcept {
		auto copy = *this;
		copy.frames = 1;
		return copy;
	}

	[[nodiscard]] constexpr PixelLayout with_frames(std::uint32_t value) const noexcept {
		auto copy = *this;
		copy.frames = value;
		return copy;
	}

	[[nodiscard]] constexpr PixelLayout with_data_type(
	    DataType dt, std::uint16_t normalized_bits_stored = 0) const noexcept {
		auto copy = *this;
		copy.data_type = dt;
		copy.bits_stored = normalized_bits_stored != 0
		                       ? normalized_bits_stored
		                       : normalized_bits_stored_of(dt);
		copy.row_stride = 0;
		copy.frame_stride = 0;
		return copy;
	}

	[[nodiscard]] constexpr PixelLayout with_samples(
	    std::uint16_t spp, Photometric pm, Planar pl) const noexcept {
		auto copy = *this;
		copy.samples_per_pixel = spp;
		copy.photometric = pm;
		copy.planar = pl;
		copy.row_stride = 0;
		copy.frame_stride = 0;
		return copy;
	}

	[[nodiscard]] PixelLayout packed(std::size_t alignment = 1) const {
		// Packed/aligned layout computation only makes sense for a fully described image.
		if (data_type == DataType::unknown || rows == 0 || cols == 0 || frames == 0 ||
		    samples_per_pixel == 0) {
			throw std::invalid_argument("PixelLayout::packed requires non-empty geometry");
		}

		// Sample width drives every downstream row/frame stride calculation.
		const auto sample_bytes = bytes_per_sample();
		if (sample_bytes == 0) {
			throw std::invalid_argument("PixelLayout::packed requires a known data_type");
		}

		// In planar mode each row covers one plane, otherwise it covers all samples.
		const auto layout_planar =
		    planar == Planar::planar && samples_per_pixel > std::uint16_t{1};
		const std::size_t row_components = layout_planar
		                                       ? static_cast<std::size_t>(cols)
		                                       : static_cast<std::size_t>(cols) *
		                                             static_cast<std::size_t>(samples_per_pixel);

		// First compute the payload size of one row before any alignment padding.
		std::size_t row_packed_bytes = 0;
		if (!detail::checked_mul_size_t(row_components, sample_bytes, row_packed_bytes)) {
			throw std::overflow_error("PixelLayout::packed row bytes overflow");
		}

		// Alignment affects row starts only; 0/1 both mean "packed with no extra padding".
		const auto normalized_alignment =
		    alignment <= std::size_t{1} ? std::size_t{1} : alignment;
		const auto aligned_row_stride =
		    detail::align_up_size_t_or_zero(row_packed_bytes, normalized_alignment);
		if (aligned_row_stride == 0) {
			throw std::overflow_error("PixelLayout::packed aligned row stride overflow");
		}

		// A frame is rows worth of storage, multiplied by plane count when planar.
		std::size_t frame_stride_bytes = 0;
		if (!detail::checked_mul_size_t(
		        aligned_row_stride, static_cast<std::size_t>(rows), frame_stride_bytes)) {
			throw std::overflow_error("PixelLayout::packed frame stride overflow");
		}
		if (layout_planar &&
		    !detail::checked_mul_size_t(frame_stride_bytes,
		        static_cast<std::size_t>(samples_per_pixel), frame_stride_bytes)) {
			throw std::overflow_error("PixelLayout::packed planar frame stride overflow");
		}

		// Normalize bits_stored so the layout always carries an explicit effective precision.
		auto copy = *this;
		copy.row_stride = aligned_row_stride;
		copy.frame_stride = frame_stride_bytes;
		if (copy.bits_stored == 0) {
			copy.bits_stored = normalized_bits_stored_of(copy.data_type);
		}
		return copy;
	}
};

static_assert(std::is_trivially_copyable_v<PixelLayout>);
static_assert(std::is_standard_layout_v<PixelLayout>);
static_assert(std::is_trivial_v<PixelLayout>);

[[nodiscard]] inline constexpr bool try_pixel_storage_size(
    const PixelLayout& layout, std::size_t& out_total_bytes) noexcept {
	if (layout.empty()) {
		out_total_bytes = 0;
		return false;
	}
	return detail::checked_mul_size_t(
	    static_cast<std::size_t>(layout.frames), layout.frame_stride, out_total_bytes);
}

namespace detail {

template <typename T>
[[nodiscard]] inline bool is_typed_row_access_aligned(
    const PixelLayout& layout, const std::uint8_t* bytes) noexcept {
	// Typed row kernels need the base pointer and each row/frame step to stay aligned.
	if ((reinterpret_cast<std::uintptr_t>(bytes) % alignof(T)) != 0) {
		return false;
	}
	if ((layout.row_stride % sizeof(T)) != 0 || (layout.frame_stride % sizeof(T)) != 0) {
		return false;
	}
	return true;
}

} // namespace detail

struct ConstPixelSpan {
	PixelLayout layout{};
	std::span<const std::uint8_t> bytes{};

	[[nodiscard]] bool has_required_bytes() const noexcept {
		std::size_t required_bytes = 0;
		return try_pixel_storage_size(layout, required_bytes) && bytes.size() >= required_bytes;
	}

	template <typename T>
	[[nodiscard]] bool is_typed_row_access_aligned() const noexcept {
		return detail::is_typed_row_access_aligned<T>(layout, bytes.data());
	}

	[[nodiscard]] ConstPixelSpan frame(std::size_t frame_index) const {
		// Frame views are valid only within the layout's declared frame count.
		if (frame_index >= static_cast<std::size_t>(layout.frames)) {
			throw std::out_of_range("ConstPixelSpan::frame frame index out of range");
		}

		// Reject any request that would step outside the backing byte span.
		std::size_t offset = 0;
		if (!detail::checked_mul_size_t(frame_index, layout.frame_stride, offset) ||
		    bytes.size() < offset || bytes.size() - offset < layout.frame_stride) {
			throw std::out_of_range("ConstPixelSpan::frame frame byte range out of range");
		}
		// The returned view preserves row/frame stride semantics but narrows frames to one.
		return ConstPixelSpan{
		    .layout = layout.single_frame(),
		    .bytes = bytes.subspan(offset, layout.frame_stride),
		};
	}
};

struct PixelSpan {
	PixelLayout layout{};
	std::span<std::uint8_t> bytes{};

	[[nodiscard]] bool has_required_bytes() const noexcept {
		std::size_t required_bytes = 0;
		return try_pixel_storage_size(layout, required_bytes) && bytes.size() >= required_bytes;
	}

	template <typename T>
	[[nodiscard]] bool is_typed_row_access_aligned() const noexcept {
		return detail::is_typed_row_access_aligned<T>(layout, bytes.data());
	}

	[[nodiscard]] PixelSpan frame(std::size_t frame_index) const {
		// Frame views are valid only within the layout's declared frame count.
		if (frame_index >= static_cast<std::size_t>(layout.frames)) {
			throw std::out_of_range("PixelSpan::frame frame index out of range");
		}

		// Reject any request that would step outside the mutable backing byte span.
		std::size_t offset = 0;
		if (!detail::checked_mul_size_t(frame_index, layout.frame_stride, offset) ||
		    bytes.size() < offset || bytes.size() - offset < layout.frame_stride) {
			throw std::out_of_range("PixelSpan::frame frame byte range out of range");
		}
		// The returned view preserves row/frame stride semantics but narrows frames to one.
		return PixelSpan{
		    .layout = layout.single_frame(),
		    .bytes = bytes.subspan(offset, layout.frame_stride),
		};
	}

	[[nodiscard]] operator ConstPixelSpan() const noexcept {
		return ConstPixelSpan{.layout = layout, .bytes = bytes};
	}
};

struct PixelBuffer {
	PixelLayout layout{};
	std::vector<std::uint8_t> bytes{};

	[[nodiscard]] static PixelBuffer allocate(PixelLayout new_layout) {
		// Allocate exactly the storage span implied by the owning layout.
		std::size_t total_bytes = 0;
		if (!try_pixel_storage_size(new_layout, total_bytes)) {
			throw std::overflow_error("PixelBuffer::allocate layout storage overflow");
		}
		return PixelBuffer{
		    .layout = new_layout,
		    .bytes = std::vector<std::uint8_t>(total_bytes),
		};
	}

	void reset(PixelLayout new_layout) {
		// Reset swaps both the logical layout and the owned storage in one place.
		std::size_t total_bytes = 0;
		if (!try_pixel_storage_size(new_layout, total_bytes)) {
			throw std::overflow_error("PixelBuffer::reset layout storage overflow");
		}
		layout = new_layout;
		bytes.assign(total_bytes, std::uint8_t{0});
	}

	[[nodiscard]] ConstPixelSpan cspan() const noexcept {
		return ConstPixelSpan{.layout = layout, .bytes = bytes};
	}

	[[nodiscard]] PixelSpan span() noexcept {
		return PixelSpan{.layout = layout, .bytes = bytes};
	}

	[[nodiscard]] ConstPixelSpan frame(std::size_t frame_index) const {
		return cspan().frame(frame_index);
	}

	[[nodiscard]] PixelSpan frame(std::size_t frame_index) noexcept {
		return span().frame(frame_index);
	}
};

using CodecOptionValue = std::variant<std::int64_t, double, bool, std::string>;

struct CodecOptionKv {
	std::string_view key{};
	CodecOptionValue value{};
};

struct CodecOptionTextKv {
	std::string_view key{};
	std::string_view value{};
};

class EncoderContext;

/// Replace PixelData using the transfer syntax and codec options captured in `encoder_ctx`.
void set_pixel_data(
    DicomFile& file, ConstPixelSpan source, const EncoderContext& encoder_ctx);

class EncoderContext {
public:
	EncoderContext() = default;

	void configure(uid::WellKnown transfer_syntax);
	void configure(uid::WellKnown transfer_syntax,
	    std::span<const CodecOptionTextKv> codec_opt);
	[[nodiscard]] bool configured() const noexcept { return configured_; }
	[[nodiscard]] uid::WellKnown transfer_syntax_uid() const noexcept {
		return transfer_syntax_uid_;
	}
	[[nodiscard]] std::span<const CodecOptionKv> codec_options() const noexcept {
		return codec_options_;
	}

private:
	friend void set_pixel_data(
	    DicomFile& file, ConstPixelSpan source, const EncoderContext& encoder_ctx);
	friend class ::dicom::DicomFile;
	void set_configured_state(uid::WellKnown transfer_syntax,
	    std::vector<std::string> option_keys,
	    std::vector<CodecOptionKv> codec_options);

	uid::WellKnown transfer_syntax_uid_{};
	std::vector<std::string> option_keys_{};
	std::vector<CodecOptionKv> codec_options_{};
	bool configured_{false};
};

[[nodiscard]] EncoderContext create_encoder_context(uid::WellKnown transfer_syntax);
[[nodiscard]] EncoderContext create_encoder_context(uid::WellKnown transfer_syntax,
    std::span<const CodecOptionTextKv> codec_opt);

struct DecodeOptions {
	std::uint16_t alignment{1};  // 0/1: packed, power-of-two aligned (<= 4096)
	// 0: auto-compute from decoded geometry.
	// >0: explicit output row stride in bytes; when set, alignment is ignored.
	std::size_t row_stride{0};
	// 0: auto-compute from row_stride/geometry.
	// >0: explicit output frame stride in bytes; when set, alignment is ignored.
	std::size_t frame_stride{0};
	Planar planar_out{Planar::interleaved};
	// true: apply codestream-level MCT/color transform inverse when decoder supports it.
	// false: keep codestream component domain (for example, YBR_* domain for JPEG2000 MCT streams).
	// Note: currently honored by OpenJPEG-based decode paths; other backends may ignore it.
	bool decode_mct{true};
	// DicomSDL-managed outer worker count used by batch/multi-work-item decode paths.
	//  -1: API-specific auto scheduling policy [default], 0/1: disable outer parallelism,
	//      >1: explicit worker count.
	// Single-frame decode paths do not currently use this field.
	int worker_threads{-1};
	// Codec/backend internal thread count hint forwarded to runtime decoders when supported.
	//  -1: API-specific/backend-aware auto [default],
	//      0: library/default sequential, >0: explicit thread count.
	// Backends may ignore this option when unsupported.
	int codec_threads{-1};
};

struct ModalityLut {
	std::int64_t first_mapped{0};
	std::vector<float> values;
};

struct PaletteLut {
	std::int64_t first_mapped{0};
	std::uint16_t bits_per_entry{0};
	std::vector<std::uint16_t> red_values;
	std::vector<std::uint16_t> green_values;
	std::vector<std::uint16_t> blue_values;
	std::vector<std::uint16_t> alpha_values;
};

enum class PixelPresentation : std::uint8_t {
	monochrome = 0,
	color = 1,
	mixed = 2,
	true_color = 3,
	color_range = 4,
	color_ref = 5,
};

struct SupplementalPaletteInfo {
	PixelPresentation pixel_presentation{PixelPresentation::color};
	PaletteLut palette{};
	bool has_stored_value_range{false};
	double minimum_stored_value_mapped{0.0};
	double maximum_stored_value_mapped{0.0};
};

struct EnhancedPaletteDataPathAssignmentInfo {
	std::string data_type{};
	std::string data_path_assignment{};
	bool has_bits_mapped_to_color_lookup_table{false};
	std::uint16_t bits_mapped_to_color_lookup_table{0};
};

struct EnhancedBlendingLutInfo {
	std::string transfer_function{};
	bool has_weight_constant{false};
	double weight_constant{0.0};
	std::uint16_t bits_per_entry{0};
	std::vector<std::uint16_t> values;
};

struct EnhancedPaletteItemInfo {
	std::string data_path_id{};
	std::string rgb_lut_transfer_function{};
	std::string alpha_lut_transfer_function{};
	PaletteLut palette{};
};

struct EnhancedPaletteInfo {
	PixelPresentation pixel_presentation{PixelPresentation::color};
	std::vector<EnhancedPaletteDataPathAssignmentInfo> data_frame_assignments;
	bool has_blending_lut_1{false};
	EnhancedBlendingLutInfo blending_lut_1{};
	bool has_blending_lut_2{false};
	EnhancedBlendingLutInfo blending_lut_2{};
	std::vector<EnhancedPaletteItemInfo> palette_items;
	bool has_icc_profile{false};
	std::string color_space{};
};

struct VoiLut {
	std::int64_t first_mapped{0};
	std::uint16_t bits_per_entry{0};
	std::vector<std::uint16_t> values;
};

enum class VoiLutFunction : std::uint8_t {
	linear = 0,
	linear_exact = 1,
	sigmoid = 2,
};

struct RescaleTransform {
	float slope{1.0f};
	float intercept{0.0f};
};

struct WindowTransform {
	float center{0.0f};
	float width{0.0f};
	VoiLutFunction function{VoiLutFunction::linear};
};

/// Decode plan snapshot computed from the current file metadata and requested options.
/// Recommended usage is:
/// 1. create a plan
/// 2. allocate `plan.output_layout.frame_stride` bytes
/// 3. call `decode_frame_into()` / `DicomFile::decode_into()`
/// Recreate the plan after transfer syntax or pixel-affecting metadata changes.
/// A default-constructed plan is not valid input to `decode_frame_into()`.
struct DecodePlan {
	// Keep only caller policy and destination layout; source metadata is
	// resolved from the current file state when decode actually runs.
	DecodeOptions options{};
	PixelLayout output_layout{};
};

using ExecutionProgressCallback =
    void (*)(std::size_t completed, std::size_t total, void* user_data) noexcept;
using ExecutionCancelCallback = bool (*)(void* user_data) noexcept;

struct ExecutionObserver {
	// Called after successful work units complete.
	// For decode_all_frames_into(), completed/total are frame counts.
	// Callbacks may run on worker threads and must be thread-safe and noexcept.
	ExecutionProgressCallback on_progress{nullptr};
	// Polled cooperatively before scheduling more work.
	// Returning true cancels the operation and causes the batch API to throw.
	ExecutionCancelCallback should_cancel{nullptr};
	void* user_data{nullptr};
	// 0/1: notify every completed unit, >1: notify at this interval and on completion.
	std::size_t notify_every{8};
};

/// Compute a decode plan for the current pixel metadata snapshot.
[[nodiscard]] DecodePlan create_decode_plan(const DicomFile& df, const DecodeOptions& opt = {});

/// Decode a single frame into caller-provided buffer.
/// Current implementation supports raw(uncompressed), RLE, JPEG (via libjpeg-turbo),
/// JPEG-LS, and JPEG 2000 backends,
/// interleaved/planar layout conversion, spp=1/3/4.
/// Supported sample dtypes vary by backend:
/// - raw/RLE: u8/s8/u16/s16/u32/s32/f32/f64
/// - JPEG: integral up to 16-bit (subject to upstream libjpeg-turbo codestream support)
/// - JPEG-LS: integral up to 16-bit
/// - JPEG 2000: integral up to 32-bit
/// `dst` is expected to match the layout implied by `plan`.
/// `plan` must come from `create_decode_plan()` for the current file state.
/// @throws diag::DicomException on invalid decode plan, invalid frame index,
/// mismatched destination layout/size, unsupported decoder binding, or backend decode failure.
void decode_frame_into(const DicomFile& df, std::size_t frame_index,
    std::span<std::uint8_t> dst, const DecodePlan& plan);
/// Decode a single frame and return an owning PixelBuffer.
/// The returned buffer always uses the single-frame variant of `plan.output_layout`.
/// @throws diag::DicomException under the same conditions as decode_frame_into().
[[nodiscard]] PixelBuffer decode_frame(
    const DicomFile& df, std::size_t frame_index, const DecodePlan& plan);
/// Decode every frame into one caller-provided contiguous output buffer.
/// `dst.size()` must be at least `plan.output_layout.frames * plan.output_layout.frame_stride`.
/// Frames are written in index order, each occupying one `plan.output_layout.frame_stride` span.
/// @throws diag::DicomException on invalid decode plan, invalid frame metadata,
/// mismatched destination layout/size, unsupported decoder binding, or backend decode failure.
void decode_all_frames_into(
    const DicomFile& df, std::span<std::uint8_t> dst, const DecodePlan& plan);
/// Decode every frame and return one owning PixelBuffer for the full volume.
/// The returned buffer uses `plan.output_layout` unchanged.
/// @throws diag::DicomException under the same conditions as decode_all_frames_into().
[[nodiscard]] PixelBuffer decode_all_frames(
    const DicomFile& df, const DecodePlan& plan);
/// Decode every frame into one caller-provided contiguous output buffer.
/// When `observer` is provided, progress is reported in frames and cancellation
/// is polled cooperatively while scheduling work.
/// @throws diag::DicomException on invalid decode plan, invalid frame metadata,
/// mismatched destination layout/size, unsupported decoder binding, backend decode failure,
/// or observer-requested cancellation.
void decode_all_frames_into(const DicomFile& df, std::span<std::uint8_t> dst,
    const DecodePlan& plan, const ExecutionObserver* observer);
/// Decode every frame and return one owning PixelBuffer for the full volume.
/// When `observer` is provided, progress and cancellation behave like
/// decode_all_frames_into(..., observer).
/// @throws diag::DicomException under the same conditions as decode_all_frames_into(..., observer).
[[nodiscard]] PixelBuffer decode_all_frames(
    const DicomFile& df, const DecodePlan& plan, const ExecutionObserver* observer);

/// Build the default packed output layout for a rescale transform.
/// Geometry, sample count, and planar arrangement are preserved from `src`.
/// The default destination dtype is float32 because rescale commonly widens
/// stored integer samples into floating-point values.
[[nodiscard]] PixelLayout make_rescale_output_layout(
    PixelLayout src, DataType dst_type = DataType::f32);

/// Apply one slope/intercept pair to every sample in `src` and return a packed owner.
[[nodiscard]] PixelBuffer apply_rescale(
    ConstPixelSpan src, float slope, float intercept);

/// Apply one slope/intercept pair to every sample in `src` and write into `dst`.
/// `dst` may use a different stride/alignment policy, but its geometry and sample
/// arrangement must match `src`, and its dtype must be float32 or float64.
void apply_rescale_into(
    ConstPixelSpan src, PixelSpan dst, float slope, float intercept);

/// Apply frame-specific slope/intercept pairs to every sample in `src`.
/// `slopes.size()` and `intercepts.size()` must both match `src.layout.frames`.
void apply_rescale_frames_into(ConstPixelSpan src, PixelSpan dst,
    std::span<const float> slopes, std::span<const float> intercepts);

/// Build the default packed output layout for a Modality LUT transform.
/// Geometry is preserved and the destination dtype defaults to float32.
[[nodiscard]] PixelLayout make_modality_lut_output_layout(PixelLayout src);

/// Apply one Modality LUT to `src` and return a packed owner.
[[nodiscard]] PixelBuffer apply_modality_lut(ConstPixelSpan src, const ModalityLut& lut);

/// Apply one Modality LUT to `src` and write into `dst`.
/// `src` must carry one sample per pixel, and `dst` must match its geometry while
/// using float32 or float64 sample storage.
void apply_modality_lut_into(ConstPixelSpan src, PixelSpan dst, const ModalityLut& lut);

/// Build the default packed RGB output layout for a Palette LUT transform.
/// Geometry is preserved, samples-per-pixel becomes 3, and dtype defaults to
/// uint8 or uint16 depending on LUT entry bit depth.
[[nodiscard]] PixelLayout make_palette_output_layout(PixelLayout src, const PaletteLut& lut);

/// Apply one Palette LUT to indexed source samples and return a packed RGB owner.
[[nodiscard]] PixelBuffer apply_palette_lut(ConstPixelSpan src, const PaletteLut& lut);

/// Apply one Palette LUT to indexed source samples and write RGB values into `dst`.
/// `src` must carry one stored-value sample per pixel. `dst` must match source
/// geometry, carry three samples per pixel, and use uint8/uint16 storage that
/// matches the LUT bit depth.
void apply_palette_lut_into(ConstPixelSpan src, PixelSpan dst, const PaletteLut& lut);

/// Build the default packed monochrome output layout for a VOI LUT transform.
/// Geometry is preserved and dtype defaults to uint8 or uint16 depending on LUT bit depth.
[[nodiscard]] PixelLayout make_voi_lut_output_layout(PixelLayout src, const VoiLut& lut);

/// Apply one VOI LUT to monochrome source pixels and return a packed owner.
[[nodiscard]] PixelBuffer apply_voi_lut(ConstPixelSpan src, const VoiLut& lut);

/// Apply one VOI LUT to monochrome source pixels and write into `dst`.
/// `src` and `dst` must share geometry, carry one sample per pixel, and `dst`
/// must use uint8/uint16 storage that matches the LUT bit depth.
void apply_voi_lut_into(ConstPixelSpan src, PixelSpan dst, const VoiLut& lut);

/// Build the default packed monochrome output layout for a VOI window transform.
/// Geometry is preserved and the destination dtype defaults to uint8 display samples.
[[nodiscard]] PixelLayout make_window_output_layout(
    PixelLayout src, DataType dst_type = DataType::u8);

/// Apply one VOI window transform to monochrome source pixels and return a packed owner.
[[nodiscard]] PixelBuffer apply_window(ConstPixelSpan src, const WindowTransform& window);

/// Apply one VOI window transform to monochrome source pixels and write into `dst`.
/// `src` and `dst` must share geometry, carry one sample per pixel, and `dst`
/// must use uint8 or uint16 storage.
void apply_window_into(
    ConstPixelSpan src, PixelSpan dst, const WindowTransform& window);

enum class Htj2kDecoderBackend : std::uint8_t {
	auto_select = 0,
	openjph = 1,
	openjpeg = 2,
};

/// Configure the preferred HTJ2K decoder backend before pixel runtime initialization.
/// This must be called before the first pixel decode/encode or external plugin registration.
[[nodiscard]] bool set_htj2k_decoder_backend(Htj2kDecoderBackend backend,
    std::string* out_error = nullptr);

/// Return the currently configured HTJ2K decoder backend preference.
[[nodiscard]] Htj2kDecoderBackend get_htj2k_decoder_backend();

/// Convenience wrappers for selecting a specific HTJ2K decoder backend early in process startup.
[[nodiscard]] bool use_openjph_for_htj2k_decoding(std::string* out_error = nullptr);
[[nodiscard]] bool use_openjpeg_for_htj2k_decoding(std::string* out_error = nullptr);

/// Register external codec plugin(s) from a shared library.
/// The library may export decoder and/or encoder plugin API symbols.
/// External loadable plugins are supported only when shipped with the same
/// DicomSDL runtime release; mixed-version host/plugin deployment is not supported.
[[nodiscard]] bool register_external_codec_plugin_from_library(
    const std::filesystem::path& library_path, std::string* out_error = nullptr);

/// Remove every externally registered codec plugin and restore builtin dispatch.
[[nodiscard]] bool clear_external_codec_plugins(std::string* out_error = nullptr);

} // namespace pixel

/// Represents a collection of DICOM data elements backed by a file or memory stream.
/// The root DataSet owns the input stream; elements are parsed lazily on demand.
class DataElement {
public:
	enum class StorageKind : std::uint16_t {
		none = 0,
		stream,
		inline_bytes,
		heap,
		owned_bytes,
		sequence,
		pixel_sequence
	};
	static constexpr std::size_t kInlineStorageBytes = 8;

	constexpr DataElement() noexcept = default;
	~DataElement() { release_storage(); }

	DataElement(const DataElement&) = delete;
	DataElement& operator=(const DataElement&) = delete;
	DataElement(DataElement&&) = delete;
	DataElement& operator=(DataElement&&) = delete;

	DataElement(Tag tag, VR vr, std::size_t length, std::size_t offset,
	    DataSet* parent = nullptr) noexcept;

	/// Tag of this element.
	[[nodiscard]] constexpr Tag tag() const noexcept { return tag_; }
	/// VR of this element.
	[[nodiscard]] constexpr VR vr() const noexcept { return vr_; }
	/// Value length in bytes (may be undefined for sequences).
	[[nodiscard]] constexpr std::size_t length() const noexcept { return length_; }
	/// Absolute offset of the value within the root stream.
	/// Valid for stream-backed elements and SQ/PX elements; otherwise returns 0.
	[[nodiscard]] std::size_t offset() const noexcept;
	/// Parent dataset (if any).
	[[nodiscard]] constexpr DataSet* parent() const noexcept { return parent_; }
	/// True when lookup resolved to a real element (not the missing sentinel).
	[[nodiscard]] constexpr bool is_present() const noexcept { return vr_ != VR::None; }
	/// True when lookup resolved to the missing sentinel (VR::None).
	[[nodiscard]] constexpr bool is_missing() const noexcept { return !is_present(); }
	/// Truthy when this element is present (VR not None).
	[[nodiscard]] constexpr explicit operator bool() const noexcept { return is_present(); }
	/// Raw value as a byte span for non-sequence VRs. SQ/PX returns an empty span.
	[[nodiscard]] std::span<const std::uint8_t> value_span() const;
	/// Value multiplicity; returns -1 when not applicable or parse fails.
	[[nodiscard]] int vm() const;
	/// Current storage backing kind for this element value.
	[[nodiscard]] constexpr StorageKind storage_kind() const noexcept { return storage_kind_; }
	/// Returns the nested Sequence value if VR indicates a sequence, otherwise nullptr.
	[[nodiscard]] constexpr Sequence* sequence() const noexcept {
		return storage_kind_ == StorageKind::sequence ? storage_.seq : nullptr;
	}
	/// Returns the nested PixelSequence value if VR indicates encapsulated pixel data, otherwise nullptr.
	[[nodiscard]] constexpr PixelSequence* pixel_sequence() const noexcept {
		return storage_kind_ == StorageKind::pixel_sequence ? storage_.pixseq : nullptr;
	}
	Sequence* as_sequence();
	const Sequence* as_sequence() const;
	PixelSequence* as_pixel_sequence();
	const PixelSequence* as_pixel_sequence() const;

	/// Copy raw value bytes into inline/heap storage depending on length.
	/// For odd-length values, one VR-specific padding byte is appended automatically.
	void set_value_bytes(std::span<const std::uint8_t> bytes);
	/// Move raw value bytes into element-owned storage.
	/// For odd-length values, one VR-specific padding byte is appended automatically.
	void set_value_bytes(std::vector<std::uint8_t>&& bytes);
	/// Adopt caller-provided byte buffer without copying when possible.
	/// For odd-length values, one VR-specific padding byte is appended automatically.
	void adopt_value_bytes(std::vector<std::uint8_t>&& bytes);
	/// Reserve value storage and set length_ for non-sequence elements.
	/// The allocated storage can be observed via value_span().
	void reserve_value_bytes(std::size_t length);
	/// Encode and store a single integer value according to this element VR.
	/// Returns false when VR is unsupported for integer assignment or value is out of range.
	bool from_int(int value);
	/// Encode and store multiple integer values according to this element VR.
	/// Returns false when VR is unsupported for integer assignment or any value is out of range.
	bool from_int_vector(std::span<const int> values);
	/// Encode and store a single integer value according to this element VR.
	/// Returns false when VR is unsupported for integer assignment or value is out of range.
	bool from_long(long value);
	/// Encode and store multiple integer values according to this element VR.
	/// Returns false when VR is unsupported for integer assignment or any value is out of range.
	bool from_long_vector(std::span<const long> values);
	/// Encode and store a single integer value according to this element VR.
	/// Returns false when VR is unsupported for integer assignment or value is out of range.
	bool from_longlong(long long value);
	/// Encode and store multiple integer values according to this element VR.
	/// Returns false when VR is unsupported for integer assignment or any value is out of range.
	bool from_longlong_vector(std::span<const long long> values);
	/// Encode and store a single floating-point value according to this element VR.
	/// Returns false when VR is unsupported for floating-point assignment or value is out of range.
	bool from_double(double value);
	/// Encode and store multiple floating-point values according to this element VR.
	/// Returns false when VR is unsupported for floating-point assignment or any value is invalid.
	bool from_double_vector(std::span<const double> values);
	/// Encode and store a single tag value (AT VR only).
	bool from_tag(Tag value);
	/// Encode and store multiple tag values (AT VR only).
	bool from_tag_vector(std::span<const Tag> values);
	/// Encode and store textual bytes for string VRs.
	/// For UI VR this validates normalized UID text and applies UI padding rules.
	bool from_string_view(std::string_view value);
	/// Encode and store multiple textual values for string VRs.
	/// Values are joined with '\' for delimited string VRs.
	bool from_string_views(std::span<const std::string_view> values);
	/// Encode UTF-8 text into the current dataset charset declaration.
	/// When out_replaced is non-null, it is set when replace_* policy actually substituted characters.
	bool from_utf8_view(std::string_view value,
	    CharsetEncodeErrorPolicy errors = CharsetEncodeErrorPolicy::strict,
	    bool* out_replaced = nullptr);
	/// Encode multiple UTF-8 text values into the current dataset charset declaration.
	/// When out_replaced is non-null, it is set when replace_* policy actually substituted characters.
	bool from_utf8_views(std::span<const std::string_view> values,
	    CharsetEncodeErrorPolicy errors = CharsetEncodeErrorPolicy::strict,
	    bool* out_replaced = nullptr);
	/// Encode and store a well-known UID value (UI VR only).
	bool from_uid(uid::WellKnown uid);
	/// Encode and store a generated UID value (UI VR only).
	bool from_uid(const uid::Generated& uid);
	/// Encode and store UID text after normalization/validation (UI VR only).
	bool from_uid_string(std::string_view uid_value);
	/// Encode and store transfer syntax UID (UI VR only).
	bool from_transfer_syntax_uid(uid::WellKnown uid);
	/// Encode and store SOP class UID (UI VR only).
	bool from_sop_class_uid(uid::WellKnown uid);

	// Numeric accessors (PS3.5 6.2 Value Representation)
	/// Parse value as signed int; empty on failure.
	[[nodiscard]] std::optional<int> to_int() const;
	/// Parse value as signed long; empty on failure.
	[[nodiscard]] std::optional<long> to_long() const;
	/// Parse value as signed long long; empty on failure.
	[[nodiscard]] std::optional<long long> to_longlong() const;
	/// Parse value as vector of signed int; returns an empty vector for zero-length values and nullopt on failure.
	[[nodiscard]] std::optional<std::vector<int>> to_int_vector() const;
	/// Parse value as vector of signed long; returns an empty vector for zero-length values and nullopt on failure.
	[[nodiscard]] std::optional<std::vector<long>> to_long_vector() const;
	/// Parse value as vector of signed long long; returns an empty vector for zero-length values and nullopt on failure.
	[[nodiscard]] std::optional<std::vector<long long>> to_longlong_vector() const;
	/// Parse value as double; empty on failure.
	[[nodiscard]] std::optional<double> to_double() const;
	/// Parse value as vector of double; returns an empty vector for zero-length values and nullopt on failure.
	[[nodiscard]] std::optional<std::vector<double>> to_double_vector() const;
	/// Parse value as Tag.
	[[nodiscard]] std::optional<Tag> to_tag() const;
	/// Parse value as vector of Tag; returns an empty vector for zero-length values and nullopt on failure.
	[[nodiscard]] std::optional<std::vector<Tag>> to_tag_vector() const;
	/// Parse value as UID string (no validation beyond VR rules).
	[[nodiscard]] std::optional<std::string> to_uid_string() const;
	/// Parse value as transfer syntax UID (well-known lookup).
	[[nodiscard]] std::optional<uid::WellKnown> to_transfer_syntax_uid() const;
	/// Parse value as SOP class UID (well-known lookup).
	[[nodiscard]] std::optional<uid::WellKnown> to_sop_class_uid() const;
	/// Parse value as string_view after VR trimming/charset handling.
	[[nodiscard]] std::optional<std::string_view> to_string_view() const;
	/// Parse value as multiple string_views (backed by internal storage).
	/// For declared multibyte text charsets (for example ISO 2022 JIS, GBK, GB18030) this
	/// returns nullopt because raw byte splitting on '\' can corrupt multibyte characters
	/// before charset decode.
	[[nodiscard]] std::optional<std::vector<std::string_view>> to_string_views() const;
	/// Parse first string component as owned UTF-8 text.
	/// When out_replaced is non-null, it is set when replace_* policy actually substituted input bytes.
	[[nodiscard]] std::optional<std::string> to_utf8_string(
	    CharsetDecodeErrorPolicy errors = CharsetDecodeErrorPolicy::strict,
	    bool* out_replaced = nullptr) const;
	/// Parse all string components as owned UTF-8 text.
	/// When out_replaced is non-null, it is set when replace_* policy actually substituted input bytes.
	[[nodiscard]] std::optional<std::vector<std::string>> to_utf8_strings(
	    CharsetDecodeErrorPolicy errors = CharsetDecodeErrorPolicy::strict,
	    bool* out_replaced = nullptr) const;
	/// Parse the first PN value into structured groups/components.
	/// Returns nullopt when VR is not PN, UTF-8 decode fails, or PN syntax is invalid.
	[[nodiscard]] std::optional<PersonName> to_person_name(
	    CharsetDecodeErrorPolicy errors = CharsetDecodeErrorPolicy::strict,
	    bool* out_replaced = nullptr) const;
	/// Parse all PN values into structured groups/components.
	/// Returns nullopt when VR is not PN, UTF-8 decode fails, or any PN syntax is invalid.
	[[nodiscard]] std::optional<std::vector<PersonName>> to_person_names(
	    CharsetDecodeErrorPolicy errors = CharsetDecodeErrorPolicy::strict,
	    bool* out_replaced = nullptr) const;
	/// Serialize structured PN data and encode it into the current dataset charset declaration.
	bool from_person_name(const PersonName& value,
	    CharsetEncodeErrorPolicy errors = CharsetEncodeErrorPolicy::strict,
	    bool* out_replaced = nullptr);
	/// Serialize multiple structured PN values and encode them into the current dataset charset declaration.
	bool from_person_names(std::span<const PersonName> values,
	    CharsetEncodeErrorPolicy errors = CharsetEncodeErrorPolicy::strict,
	    bool* out_replaced = nullptr);

	/*
	VR trimming/charset rules (for string helpers like to_string_view):
	VR   | Leading | Trailing | Charset        | "\\" Delimiter
	-----+---------+----------+----------------+---------------
	AE   | trim    | trim     | default        | yes
	AS   | trim    | trim     | default        | yes
	CS   | trim    | trim     | default        | yes
	DA   | trim    | trim     | default        | yes
	DS   | trim    | trim     | default        | yes
	DT   | trim    | trim     | default        | yes
	IS   | trim    | trim     | default        | yes
	LO   | trim    | trim     | (0008,0005)    | yes
	LT   | keep    | trim     | (0008,0005)    | no
	PN   | trim    | trim     | (0008,0005)    | yes
	SH   | trim    | trim     | (0008,0005)    | yes
	ST   | keep    | trim     | (0008,0005)    | no
	TM   | trim    | trim     | default        | yes
	UC   | keep    | trim     | (0008,0005)    | yes
	UI   | trim    | trim/null pad | default   | yes
	UR   | trim    | trim     | default (RFC)  | no
	UT   | keep    | trim     | (0008,0005)    | no
	*/

	// Convenience overloads with explicit fallback values (snake_case).
	// These coexist with optional-returning to_*()/as_*() accessors.
	[[nodiscard]] inline int to_int(int default_value) const {
		return to_int().value_or(default_value);
	}
	[[nodiscard]] inline long to_long(long default_value) const {
		return to_long().value_or(default_value);
	}
	[[nodiscard]] inline long long to_longlong(long long default_value) const {
		return to_longlong().value_or(default_value);
	}
	[[nodiscard]] inline std::vector<int> to_int_vector(
	    std::vector<int> default_value) const {
		return to_int_vector().value_or(std::move(default_value));
	}
	[[nodiscard]] inline std::vector<long> to_long_vector(
	    std::vector<long> default_value) const {
		return to_long_vector().value_or(std::move(default_value));
	}
	[[nodiscard]] inline std::vector<long long> to_longlong_vector(
	    std::vector<long long> default_value) const {
		return to_longlong_vector().value_or(std::move(default_value));
	}
	[[nodiscard]] inline double to_double(double default_value) const {
		return to_double().value_or(default_value);
	}
	[[nodiscard]] inline std::vector<double> to_double_vector(
	    std::vector<double> default_value) const {
		return to_double_vector().value_or(std::move(default_value));
	}
	[[nodiscard]] inline Tag to_tag(Tag default_value) const {
		return to_tag().value_or(default_value);
	}
	[[nodiscard]] inline std::vector<Tag> to_tag_vector(
	    std::vector<Tag> default_value) const {
		return to_tag_vector().value_or(std::move(default_value));
	}
	[[nodiscard]] inline std::string to_uid_string(std::string default_value) const {
		return to_uid_string().value_or(std::move(default_value));
	}

private:
	Tag tag_{};
	VR vr_{};
	StorageKind storage_kind_{StorageKind::none};
	std::size_t length_{0};
	union Storage {
		void* ptr;
		std::vector<std::uint8_t>* vec;
		Sequence* seq;
		PixelSequence* pixseq;
		std::size_t offset_;
		std::uint8_t inline_bytes[kInlineStorageBytes];
		constexpr Storage() noexcept : ptr(nullptr) {}
	} storage_{};
	DataSet* parent_{nullptr};

	friend class DataSet;
	friend bool charset::detail::apply_declared_charset(
	    DataSet& dataset, const charset::detail::ParsedSpecificCharacterSet& parsed,
	    std::string* out_error);
	friend bool charset::encode_utf8_for_element(DataElement& element,
	    std::span<const std::string_view> values, CharsetEncodeErrorPolicy errors,
	    std::string* out_error, bool* out_replaced);
	friend bool charset::detail::rewrite_charset_values(
	    DataSet& dataset, const charset::detail::ParsedSpecificCharacterSet& target_charset,
	    CharsetEncodeErrorPolicy errors, std::string* out_error, bool* out_replaced);
	constexpr void set_length(std::size_t length) noexcept { length_ = length; }
	void release_storage() noexcept;
	void initialize_storage(std::size_t offset, bool bind_to_parent_stream) noexcept;
	void reset(Tag tag, VR vr, std::size_t length, std::size_t offset,
	    DataSet* parent, bool bind_to_parent_stream) noexcept;
	void reset_without_release(Tag tag, VR vr, std::size_t length, std::size_t offset,
	    DataSet* parent, bool bind_to_parent_stream) noexcept;
	void set_value_bytes_nocheck(std::vector<std::uint8_t>&& bytes);
	void adopt_value_bytes_nocheck(std::vector<std::uint8_t>&& bytes);
	void adopt_value_bytes_impl(
	    std::vector<std::uint8_t>&& bytes, bool notify_charset_parent);
};

namespace detail {

template <typename T>
inline constexpr bool dependent_false_v = false;

template <typename T>
[[nodiscard]] std::optional<T> dataelement_get_value_cpp(const DataElement& element);

}  // namespace detail

template <typename IndexIter, typename MapIter, typename Ref, typename Ptr>
/// Forward iterator over active deque-backed elements plus out-of-order map entries.
class DataElementIterator {
public:
	using iterator_category = std::forward_iterator_tag;
	using value_type = std::remove_reference_t<Ref>;
	using difference_type = std::ptrdiff_t;
	using reference = Ref;
	using pointer = Ptr;

	constexpr DataElementIterator() = default;

	DataElementIterator(IndexIter index_it, IndexIter index_end, MapIter map_it, MapIter map_end)
	    : index_it_(index_it), index_end_(index_end), map_it_(map_it), map_end_(map_end) {
		select_source();
	}

	reference operator*() const {
		return use_index_ ? *(index_it_->element) : map_it_->second;
	}

	pointer operator->() const {
		return &(**this);
	}

	DataElementIterator& operator++() {
		advance_active();
		select_source();
		return *this;
	}

	DataElementIterator operator++(int) {
		auto tmp = *this;
		++(*this);
		return tmp;
	}

	friend bool operator==(const DataElementIterator& lhs, const DataElementIterator& rhs) {
		return lhs.index_it_ == rhs.index_it_ && lhs.map_it_ == rhs.map_it_;
	}

	friend bool operator!=(const DataElementIterator& lhs, const DataElementIterator& rhs) {
		return !(lhs == rhs);
	}

private:
	void advance_active() {
		if (use_index_) {
			if (index_it_ != index_end_) {
				++index_it_;
			}
		} else if (map_it_ != map_end_) {
			++map_it_;
		}
	}

	void select_source() {
		while (true) {
			const bool index_done = index_it_ == index_end_;
			const bool map_done = map_it_ == map_end_;

			if (index_done && map_done) {
				use_index_ = false;
				return;
			}

			if (!index_done && (map_done || index_it_->tag.value() <= map_it_->first)) {
				const auto* element = index_it_->element;
				if (element == nullptr || element->vr() == VR::None) {
					++index_it_;
					continue;
				}
				use_index_ = true;
				return;
			}

			if (!map_done) {
				if (map_it_->second.vr() == VR::None) {
					++map_it_;
					continue;
				}
				use_index_ = false;
				return;
			}
		}
	}

	IndexIter index_it_{};
	IndexIter index_end_{};
	MapIter map_it_{};
	MapIter map_end_{};
	bool use_index_{false};
};

struct ElementRef {
	Tag tag{};
	DataElement* element{nullptr};

	constexpr ElementRef() noexcept = default;
	constexpr ElementRef(Tag tag_value, DataElement* element_ptr) noexcept
	    : tag(tag_value), element(element_ptr) {}
};


/// Root or nested DICOM dataset. Holds elements and the underlying stream identifier,
/// and supports lazy reading/iteration of elements.
class DataSet {
public:
	using iterator = DataElementIterator<std::vector<ElementRef>::iterator,
	    std::map<std::uint32_t, DataElement>::iterator, DataElement&, DataElement*>;
	using const_iterator = DataElementIterator<std::vector<ElementRef>::const_iterator,
	    std::map<std::uint32_t, DataElement>::const_iterator, const DataElement&,
	    const DataElement*>;

	/// Construct an empty root dataset.
	DataSet();
	/// Construct a child dataset inheriting parse/file context from its parent dataset.
	explicit DataSet(DataSet* parent_dataset);
	/// Construct a root dataset owned by DicomFile.
	explicit DataSet(DicomFile* root_file);
	~DataSet();
	DataSet(const DataSet&) = delete;
	DataSet& operator=(const DataSet&) = delete;
	// DataSet keeps self/root pointers, so moving would invalidate internal context.
	DataSet(DataSet&&) noexcept = delete;
	DataSet& operator=(DataSet&&) noexcept = delete;

	/// Attach the dataset to a DICOM file on disk. Lazy: parsing happens on first access.
	/// @param path Filesystem path to the DICOM file.
	void attach_to_file(const std::filesystem::path& path);

	/// Attach to an in-memory buffer.
	/// @param data Pointer to the buffer.
	/// @param size Length of the buffer in bytes.
	/// @param copy When true (default) the buffer is copied; when false it is referenced and must outlive the DataSet.
	void attach_to_memory(const std::uint8_t* data, std::size_t size, bool copy = true);

	/// Attach to an in-memory buffer with a custom identifier (used in diagnostics/path()).
	/// @param name Identifier reported by path() and logs.
	/// @param data Pointer to the buffer.
	/// @param size Length of the buffer in bytes.
	/// @param copy When true (default) the buffer is copied; when false it is referenced and must outlive the DataSet.
	void attach_to_memory(const std::string& name, const std::uint8_t* data,
	    std::size_t size, bool copy = true);

	/// Attach by taking ownership of a movable buffer.
	/// @param name Identifier reported by path() and logs.
	/// @param buffer Buffer that will be moved into the DataSet.
	/// @note DataSet keeps file/memory mappings alive for as long as the instance exists.
	/// Destroy or reset all DataSet instances that reference a path before deleting or
	/// overwriting the underlying df; some OSes (especially Windows) may refuse removal
	/// while a mapping handle is open.
	void attach_to_memory(std::string name, std::vector<std::uint8_t>&& buffer);

	/// Returns the current stream identifier (file path, provided name, or "<memory>").
	const std::string& path() const;

	/// Access the underlying input stream (root dataset only).
	InStream& stream();

	/// Access the underlying input stream (const).
	const InStream& stream() const;

	/// Attach a sub-range of another stream (used internally for sequences/pixel data).
	void attach_to_substream(InStream* base_stream, std::size_t size);

	/// Set the absolute offset of this dataset within the root stream.
	void set_offset(std::size_t offset) { offset_ = offset; }

	/// Absolute offset of this dataset within the root stream.
	[[nodiscard]] std::size_t offset() const { return offset_; }

	/// Update only the local (0008,0005) Specific Character Set metadata for this dataset.
	/// Existing text value bytes are left untouched.
	/// Prefer this API over directly editing the raw (0008,0005) element so the effective
	/// charset cache for nested sequence items stays synchronized.
	void set_declared_specific_charset(SpecificCharacterSet charset);
	void set_declared_specific_charset(std::span<const SpecificCharacterSet> charsets);
	void set_declared_specific_charset(std::initializer_list<SpecificCharacterSet> charsets) {
		set_declared_specific_charset(
		    std::span<const SpecificCharacterSet>(charsets.begin(), charsets.size()));
	}

	/// Transcode this dataset's text VR values to a new Specific Character Set and synchronize
	/// the local (0008,0005) tag for this dataset and nested item datasets in the subtree.
	/// Prefer this API over directly editing the raw (0008,0005) element so the effective
	/// charset cache for nested sequence items stays synchronized.
	void set_specific_charset(SpecificCharacterSet charset,
	    CharsetEncodeErrorPolicy errors = CharsetEncodeErrorPolicy::strict,
	    bool* out_replaced = nullptr);
	void set_specific_charset(std::span<const SpecificCharacterSet> charsets,
	    CharsetEncodeErrorPolicy errors = CharsetEncodeErrorPolicy::strict,
	    bool* out_replaced = nullptr);
	void set_specific_charset(std::initializer_list<SpecificCharacterSet> charsets,
	    CharsetEncodeErrorPolicy errors = CharsetEncodeErrorPolicy::strict,
	    bool* out_replaced = nullptr) {
		set_specific_charset(std::span<const SpecificCharacterSet>(charsets.begin(), charsets.size()),
		    errors, out_replaced);
	}

	/// Add or replace a zero-length data element owned by this DataSet.
	/// Direct edits to `(0008,0005) Specific Character Set` keep the effective charset
	/// cache synchronized, but the charset-specific setter APIs remain preferred because
	/// they make intent clearer and validate multi-term combinations explicitly.
	/// @return Reference to the inserted/replaced element.
	/// @throws Exception on validation errors (for example: VR::None with unknown tag),
	///         allocation failures, or when called on a partially loaded attached dataset
	///         for a tag beyond the current load frontier.
	DataElement& add_dataelement(Tag tag, VR vr = VR::None);
	/// Add or replace a leaf element addressed by a dotted tag path.
	/// Intermediate sequence/item components are created as needed. If an
	/// intermediate element already exists with a non-sequence VR, it is reset
	/// in place to `SQ` so the requested path can be materialized.
	DataElement& add_dataelement(std::string_view tag_path, VR vr = VR::None);

	/// Remove a data element by tag (no-op if missing).
	/// Removing `(0008,0005)` through this API also refreshes the effective charset cache,
	/// though `set_declared_specific_charset()` is still preferred for clarity.
	void remove_dataelement(Tag tag);

	/// Low-level lookup by tag (reference form). For most user-facing code, prefer operator[].
	/// Does not load implicitly; call ensure_loaded(tag) or read_attached_stream() first.
	/// Missing lookups return a falsey DataElement (VR::None).
	DataElement& get_dataelement(Tag tag);

	/// Low-level const lookup by tag (reference form). For most user-facing code, prefer operator[].
	/// Does not load implicitly; call ensure_loaded(tag) or read_attached_stream() first.
	/// Missing lookups return a falsey DataElement (VR::None).
	const DataElement& get_dataelement(Tag tag) const;

	/// Ensure that a data element exists for a tag.
	/// When the element already exists and `vr == VR::None`, it is preserved as-is.
	/// When the element already exists and `vr` is explicit and different, it is reset in place
	/// so the requested VR is guaranteed. When the element is missing, `vr` is used for
	/// insertion; if `vr == VR::None`, the dictionary VR is resolved for standard tags and
	/// unknown/private tags throw.
	/// @return Reference to the existing or inserted element.
	/// @throws Exception under the same conditions as add_dataelement (for example:
	///         VR::None with unknown/private tags), allocation failures, or when
	///         called on a partially loaded attached dataset for a tag beyond the
	///         current load frontier.
	DataElement& ensure_dataelement(Tag tag, VR vr = VR::None);
	/// Ensure that a leaf element exists for a dotted tag path.
	/// Intermediate sequence/item components are created as needed. If an
	/// intermediate element already exists with a non-sequence VR, it is reset
	/// in place to `SQ` so the requested path can be materialized.
	DataElement& ensure_dataelement(std::string_view tag_path, VR vr = VR::None);

	/// Low-level dotted tag-path resolver (e.g., "00540016.0.00181075").
	/// Caller must ensure_loaded() the needed prefixes.
	/// Missing lookups return a falsey DataElement (VR::None).
	DataElement& get_dataelement(std::string_view tag_path);

	/// Low-level const dotted tag-path resolver.
	/// Caller must ensure_loaded() the needed prefixes.
	/// Missing lookups return a falsey DataElement (VR::None).
	const DataElement& get_dataelement(std::string_view tag_path) const;

	/// Convenience typed lookup by tag.
	/// Does not implicitly continue partial loading; unread future tags behave as missing
	/// until the caller explicitly ensures the needed frontier.
	template <typename T>
	[[nodiscard]] std::optional<T> get_value(Tag tag) const;

	/// Convenience typed lookup by dotted tag-path or keyword string.
	/// Does not implicitly continue partial loading.
	template <typename T>
	[[nodiscard]] std::optional<T> get_value(std::string_view tag_path) const;

	/// Convenience typed lookup with fallback by tag.
	/// Does not implicitly continue partial loading.
	template <typename T>
	[[nodiscard]] T get_value(Tag tag, T default_value) const;

	/// Convenience typed lookup with fallback by dotted tag-path or keyword string.
	/// Does not implicitly continue partial loading.
	template <typename T>
	[[nodiscard]] T get_value(std::string_view tag_path, T default_value) const;

	/// Preferred map-style access by tag. Missing lookups return a falsey DataElement (VR::None).
	DataElement& operator[](Tag tag);

	/// Preferred map-style access by dotted tag-path or keyword string.
	/// Missing lookups return a falsey DataElement (VR::None).
	DataElement& operator[](std::string_view tag_path);

	/// Preferred const map-style access by tag. Missing lookups return a falsey DataElement (VR::None).
	const DataElement& operator[](Tag tag) const;

	/// Preferred const map-style access by dotted tag-path or keyword string.
	/// Missing lookups return a falsey DataElement (VR::None).
	const DataElement& operator[](std::string_view tag_path) const;

	/// Print elements to stdout (debug).
	void dump_elements() const;
	/// Render a human-readable dataset dump as text.
	[[nodiscard]] std::string dump(
	    std::size_t max_print_chars = 80, bool include_offset = true) const;

	/// Read all elements from the attached stream with options.
	void read_attached_stream(const ReadOptions& options);

	/// Read elements until the given tag boundary.
	void read_elements_until(Tag load_until, InStream* stream);

	/// Ensure the given tag (and preceding tags) are loaded.
	void ensure_loaded(Tag tag);

	/// Const version of ensure_loaded.
	void ensure_loaded(Tag tag) const;

	/// Transfer syntax explicit VR flag.
	[[nodiscard]] inline bool is_explicit_vr() const { return explicit_vr_; }

	/// Well-known transfer syntax UID for this dataset.
	[[nodiscard]] uid::WellKnown transfer_syntax_uid() const;
	/// Root dataset context for this dataset tree.
	[[nodiscard]] DataSet* root_dataset() const noexcept;
	/// Direct parent dataset for nested sequence items, or nullptr for the root dataset.
	[[nodiscard]] DataSet* parent_dataset() const noexcept { return parent_dataset_; }
	/// Root file/session context (nullptr for standalone DataSet).
	[[nodiscard]] DicomFile* root_file() const noexcept { return root_file_; }
	/// Number of active DataElements currently available in this dataset.
	[[nodiscard]] std::size_t size() const;

	/// Begin iterator over active elements in tag order.
	iterator begin();

	/// End iterator over active elements.
	iterator end();

	/// Const begin iterator.
	const_iterator begin() const;

	/// Const end iterator.
	const_iterator end() const;

	/// Const begin iterator (alias).
	const_iterator cbegin() const;

	/// Const end iterator (alias).
	const_iterator cend() const;

	/// One-shot typed assignment helpers.
	/// On failure these return false and leave the DataSet valid, but the destination
	/// element state is unspecified. Callers that need rollback semantics must preserve
	/// and restore the previous element value themselves. On partially loaded attached
	/// datasets, these throw when the target tag lies beyond the current loaded frontier.
	[[nodiscard]] bool set_value(Tag tag, int value);
	[[nodiscard]] bool set_value(Tag tag, long value);
	[[nodiscard]] bool set_value(Tag tag, long long value);
	[[nodiscard]] bool set_value(Tag tag, double value);
	[[nodiscard]] bool set_value(Tag tag, Tag value);
	[[nodiscard]] bool set_value(Tag tag, std::string_view value);
	[[nodiscard]] bool set_value(Tag tag, const PersonName& value);
	[[nodiscard]] bool set_value(Tag tag, std::span<const int> values);
	[[nodiscard]] bool set_value(Tag tag, std::span<const long> values);
	[[nodiscard]] bool set_value(Tag tag, std::span<const long long> values);
	[[nodiscard]] bool set_value(Tag tag, std::span<const double> values);
	[[nodiscard]] bool set_value(Tag tag, std::span<const Tag> values);
	[[nodiscard]] bool set_value(Tag tag, std::span<const std::string_view> values);
	[[nodiscard]] bool set_value(Tag tag, std::span<const PersonName> values);

	[[nodiscard]] bool set_value(std::string_view key, int value);
	[[nodiscard]] bool set_value(std::string_view key, long value);
	[[nodiscard]] bool set_value(std::string_view key, long long value);
	[[nodiscard]] bool set_value(std::string_view key, double value);
	[[nodiscard]] bool set_value(std::string_view key, Tag value);
	[[nodiscard]] bool set_value(std::string_view key, std::string_view value);
	[[nodiscard]] bool set_value(std::string_view key, const PersonName& value);
	[[nodiscard]] bool set_value(std::string_view key, std::span<const int> values);
	[[nodiscard]] bool set_value(std::string_view key, std::span<const long> values);
	[[nodiscard]] bool set_value(std::string_view key, std::span<const long long> values);
	[[nodiscard]] bool set_value(std::string_view key, std::span<const double> values);
	[[nodiscard]] bool set_value(std::string_view key, std::span<const Tag> values);
	[[nodiscard]] bool set_value(std::string_view key, std::span<const std::string_view> values);
	[[nodiscard]] bool set_value(std::string_view key, std::span<const PersonName> values);

	[[nodiscard]] bool set_value(Tag tag, VR vr, int value);
	[[nodiscard]] bool set_value(Tag tag, VR vr, long value);
	[[nodiscard]] bool set_value(Tag tag, VR vr, long long value);
	[[nodiscard]] bool set_value(Tag tag, VR vr, double value);
	[[nodiscard]] bool set_value(Tag tag, VR vr, Tag value);
	[[nodiscard]] bool set_value(Tag tag, VR vr, std::string_view value);
	[[nodiscard]] bool set_value(Tag tag, VR vr, const PersonName& value);
	[[nodiscard]] bool set_value(Tag tag, VR vr, std::span<const int> values);
	[[nodiscard]] bool set_value(Tag tag, VR vr, std::span<const long> values);
	[[nodiscard]] bool set_value(Tag tag, VR vr, std::span<const long long> values);
	[[nodiscard]] bool set_value(Tag tag, VR vr, std::span<const double> values);
	[[nodiscard]] bool set_value(Tag tag, VR vr, std::span<const Tag> values);
	[[nodiscard]] bool set_value(Tag tag, VR vr, std::span<const std::string_view> values);
	[[nodiscard]] bool set_value(Tag tag, VR vr, std::span<const PersonName> values);

	[[nodiscard]] bool set_value(std::string_view key, VR vr, int value);
	[[nodiscard]] bool set_value(std::string_view key, VR vr, long value);
	[[nodiscard]] bool set_value(std::string_view key, VR vr, long long value);
	[[nodiscard]] bool set_value(std::string_view key, VR vr, double value);
	[[nodiscard]] bool set_value(std::string_view key, VR vr, Tag value);
	[[nodiscard]] bool set_value(std::string_view key, VR vr, std::string_view value);
	[[nodiscard]] bool set_value(std::string_view key, VR vr, const PersonName& value);
	[[nodiscard]] bool set_value(std::string_view key, VR vr, std::span<const int> values);
	[[nodiscard]] bool set_value(std::string_view key, VR vr, std::span<const long> values);
	[[nodiscard]] bool set_value(std::string_view key, VR vr, std::span<const long long> values);
	[[nodiscard]] bool set_value(std::string_view key, VR vr, std::span<const double> values);
	[[nodiscard]] bool set_value(std::string_view key, VR vr, std::span<const Tag> values);
	[[nodiscard]] bool set_value(std::string_view key, VR vr, std::span<const std::string_view> values);
	[[nodiscard]] bool set_value(std::string_view key, VR vr, std::span<const PersonName> values);

private:
	struct TagPathParent {
		DataSet* parent{nullptr};
		Tag leaf_tag{};
	};

	friend class DicomFile;
	friend class DataElement;
	friend class Sequence;
	friend const charset::detail::CharsetSpec*
	charset::detail::parse_dataset_charset(const DataSet& dataset, std::string* out_error);
	friend bool charset::detail::apply_declared_charset(
	    DataSet& dataset, const charset::detail::ParsedSpecificCharacterSet& parsed,
	    std::string* out_error);
	using element_index_iterator = std::vector<ElementRef>::iterator;
	[[nodiscard]] element_index_iterator lower_bound_element_index_mutable(std::uint32_t tag_value);
	[[nodiscard]] bool should_append_to_elements(std::uint32_t tag_value) const noexcept;
	[[nodiscard]] bool is_beyond_last_loaded_tag(Tag tag) const noexcept {
		return tag > last_tag_loaded_;
	}
	[[noreturn]] void throw_beyond_last_loaded_tag(
	    Tag tag,
	    std::source_location location = std::source_location::current()) const;
	[[nodiscard]] DataElement* find_dataelement_in_elements(std::uint32_t tag_value);
	[[nodiscard]] const DataElement* find_dataelement_in_elements(std::uint32_t tag_value) const;
	[[nodiscard]] TagPathParent ensure_tag_path_parent(
	    std::string_view tag_path,
	    std::source_location location = std::source_location::current());
	DataElement& append_parsed_dataelement_nocheck(
	    Tag tag, VR vr, std::size_t offset, std::size_t length);
	DataElement& add_dataelement_nocheck(Tag tag, VR vr);
	DataElement& add_dataelement_nocheck(Tag tag, VR vr, std::size_t offset, std::size_t length);
	void remove_dataelement_nocheck(Tag tag);
	void attach_to_stream(std::string identifier, std::unique_ptr<InStream> stream);
	void set_root_file(DicomFile* root_file) noexcept { root_file_ = root_file; }
	void on_specific_character_set_changed() noexcept;
	template <typename AssignFn>
	bool set_value_impl(Tag tag, VR vr, AssignFn&& assign_fn);
	template <typename AssignFn>
	bool set_value_key_impl(std::string_view key, VR vr, AssignFn&& assign_fn);
	[[nodiscard]] bool refresh_effective_charset_cache(
	    const charset::detail::CharsetSpec* inherited, std::string* out_error = nullptr);
	[[nodiscard]] const charset::detail::CharsetSpec* effective_charset_spec(
	    std::string* out_error = nullptr) const;
	std::unique_ptr<InStream> stream_;
	DicomFile* root_file_{nullptr};
	DataSet* root_dataset_{nullptr};
	DataSet* parent_dataset_{nullptr};
	mutable const charset::detail::CharsetSpec* effective_charset_{nullptr};
	Tag last_tag_loaded_{Tag(0xFFFFu, 0xFFFFu)};
	bool explicit_vr_{true};
	std::size_t offset_{0};  // absolute offset within the root stream where this dataset starts
	std::size_t active_element_count_{0};
	std::deque<DataElement> elements_;
	std::map<std::uint32_t, DataElement> element_map_;
	std::vector<ElementRef> element_index_;
};

/// Owns root DataSet and file-level parse/decode session state.
class DicomFile {
public:
	using iterator = DataSet::iterator;
	using const_iterator = DataSet::const_iterator;

	DicomFile();
	~DicomFile();
	DicomFile(const DicomFile&) = delete;
	DicomFile& operator=(const DicomFile&) = delete;
	// DicomFile embeds the root DataSet, which keeps back-pointers into this owner.
	DicomFile(DicomFile&&) noexcept = delete;
	DicomFile& operator=(DicomFile&&) noexcept = delete;

	[[nodiscard]] DataSet& dataset();
	[[nodiscard]] const DataSet& dataset() const;
	[[nodiscard]] const std::string& path() const;
	[[nodiscard]] InStream& stream();
	[[nodiscard]] const InStream& stream() const;
	[[nodiscard]] std::size_t size() const;
	[[nodiscard]] std::string dump(
	    std::size_t max_print_chars = 80, bool include_offset = true) const;
	void rebuild_file_meta();
	void write_to_stream(std::ostream& os, const WriteOptions& options = {});
	[[nodiscard]] std::vector<std::uint8_t> write_bytes(const WriteOptions& options = {});
	void write_file(const std::filesystem::path& path, const WriteOptions& options = {});
	/// Serialize this dataset using `transfer_syntax` without mutating the source DicomFile.
	/// When the target PixelData is encapsulated and `os` is seekable, the writer backpatches
	/// ExtendedOffsetTable / ExtendedOffsetTableLengths for the streamed frames.
	/// When `os` is not seekable, the streamed encapsulated output remains valid DICOM but is
	/// written with an empty Basic Offset Table and without ExtendedOffsetTable attributes.
	/// For lossy encapsulated targets on non-seekable output streams, the writer still performs
	/// a prepass encode to compute LossyImageCompressionRatio, so those writes remain two-pass.
	/// For large pixel payloads where the goal is an output file/stream, prefer this API over
	/// `set_transfer_syntax(...)` so the target encapsulated PixelData does not need to remain
	/// materialized inside the in-memory DicomFile object.
	void write_with_transfer_syntax(std::ostream& os, uid::WellKnown transfer_syntax,
	    const WriteOptions& options = {});
	void write_with_transfer_syntax(std::ostream& os, uid::WellKnown transfer_syntax,
	    const pixel::EncoderContext& encoder_ctx, const WriteOptions& options = {});
	void write_with_transfer_syntax(std::ostream& os, uid::WellKnown transfer_syntax,
	    std::span<const pixel::CodecOptionTextKv> codec_opt,
	    const WriteOptions& options = {});
	/// Convenience overload that writes to a regular file path.
	/// The output file stream is seekable, so streamed encapsulated output can include
	/// backpatched ExtendedOffsetTable / ExtendedOffsetTableLengths when applicable.
	void write_with_transfer_syntax(const std::filesystem::path& path,
	    uid::WellKnown transfer_syntax, const WriteOptions& options = {});
	void write_with_transfer_syntax(const std::filesystem::path& path,
	    uid::WellKnown transfer_syntax, const pixel::EncoderContext& encoder_ctx,
	    const WriteOptions& options = {});
	void write_with_transfer_syntax(const std::filesystem::path& path,
	    uid::WellKnown transfer_syntax,
	    std::span<const pixel::CodecOptionTextKv> codec_opt,
	    const WriteOptions& options = {});
	/// Forwarding helper to the root DataSet::add_dataelement.
	/// @return Reference to the inserted/replaced element.
	/// @throws Exception under the same conditions as DataSet::add_dataelement.
	DataElement& add_dataelement(Tag tag, VR vr = VR::None);
	DataElement& add_dataelement(std::string_view tag_path, VR vr = VR::None);
	/// Forwarding helper to the root DataSet::ensure_dataelement.
	/// @return Reference to the existing/inserted/replaced element.
	/// @throws Exception under the same conditions as DataSet::ensure_dataelement.
	DataElement& ensure_dataelement(Tag tag, VR vr = VR::None);
	DataElement& ensure_dataelement(std::string_view tag_path, VR vr = VR::None);
	void remove_dataelement(Tag tag);
	DataElement& get_dataelement(Tag tag);
	const DataElement& get_dataelement(Tag tag) const;
	DataElement& get_dataelement(std::string_view tag_path);
	const DataElement& get_dataelement(std::string_view tag_path) const;
	void ensure_loaded(Tag tag);
	void ensure_loaded(Tag tag) const;
	template <typename T>
	[[nodiscard]] std::optional<T> get_value(Tag tag) const;
	template <typename T>
	[[nodiscard]] std::optional<T> get_value(std::string_view tag_path) const;
	template <typename T>
	[[nodiscard]] T get_value(Tag tag, T default_value) const;
	template <typename T>
	[[nodiscard]] T get_value(std::string_view tag_path, T default_value) const;
	DataElement& operator[](Tag tag);
	DataElement& operator[](std::string_view tag_path);
	const DataElement& operator[](Tag tag) const;
	const DataElement& operator[](std::string_view tag_path) const;
	/// One-shot typed assignment helpers.
	/// On failure these return false and leave the DicomFile/root DataSet valid, but the
	/// destination element state is unspecified. Callers that need rollback semantics
	/// must preserve and restore the previous element value themselves.
	[[nodiscard]] bool set_value(Tag tag, int value);
	[[nodiscard]] bool set_value(Tag tag, long value);
	[[nodiscard]] bool set_value(Tag tag, long long value);
	[[nodiscard]] bool set_value(Tag tag, double value);
	[[nodiscard]] bool set_value(Tag tag, Tag value);
	[[nodiscard]] bool set_value(Tag tag, std::string_view value);
	[[nodiscard]] bool set_value(Tag tag, const PersonName& value);
	[[nodiscard]] bool set_value(Tag tag, std::span<const int> values);
	[[nodiscard]] bool set_value(Tag tag, std::span<const long> values);
	[[nodiscard]] bool set_value(Tag tag, std::span<const long long> values);
	[[nodiscard]] bool set_value(Tag tag, std::span<const double> values);
	[[nodiscard]] bool set_value(Tag tag, std::span<const Tag> values);
	[[nodiscard]] bool set_value(Tag tag, std::span<const std::string_view> values);
	[[nodiscard]] bool set_value(Tag tag, std::span<const PersonName> values);
	[[nodiscard]] bool set_value(std::string_view key, int value);
	[[nodiscard]] bool set_value(std::string_view key, long value);
	[[nodiscard]] bool set_value(std::string_view key, long long value);
	[[nodiscard]] bool set_value(std::string_view key, double value);
	[[nodiscard]] bool set_value(std::string_view key, Tag value);
	[[nodiscard]] bool set_value(std::string_view key, std::string_view value);
	[[nodiscard]] bool set_value(std::string_view key, const PersonName& value);
	[[nodiscard]] bool set_value(std::string_view key, std::span<const int> values);
	[[nodiscard]] bool set_value(std::string_view key, std::span<const long> values);
	[[nodiscard]] bool set_value(std::string_view key, std::span<const long long> values);
	[[nodiscard]] bool set_value(std::string_view key, std::span<const double> values);
	[[nodiscard]] bool set_value(std::string_view key, std::span<const Tag> values);
	[[nodiscard]] bool set_value(std::string_view key, std::span<const std::string_view> values);
	[[nodiscard]] bool set_value(std::string_view key, std::span<const PersonName> values);
	[[nodiscard]] bool set_value(Tag tag, VR vr, int value);
	[[nodiscard]] bool set_value(Tag tag, VR vr, long value);
	[[nodiscard]] bool set_value(Tag tag, VR vr, long long value);
	[[nodiscard]] bool set_value(Tag tag, VR vr, double value);
	[[nodiscard]] bool set_value(Tag tag, VR vr, Tag value);
	[[nodiscard]] bool set_value(Tag tag, VR vr, std::string_view value);
	[[nodiscard]] bool set_value(Tag tag, VR vr, const PersonName& value);
	[[nodiscard]] bool set_value(Tag tag, VR vr, std::span<const int> values);
	[[nodiscard]] bool set_value(Tag tag, VR vr, std::span<const long> values);
	[[nodiscard]] bool set_value(Tag tag, VR vr, std::span<const long long> values);
	[[nodiscard]] bool set_value(Tag tag, VR vr, std::span<const double> values);
	[[nodiscard]] bool set_value(Tag tag, VR vr, std::span<const Tag> values);
	[[nodiscard]] bool set_value(Tag tag, VR vr, std::span<const std::string_view> values);
	[[nodiscard]] bool set_value(Tag tag, VR vr, std::span<const PersonName> values);
	[[nodiscard]] bool set_value(std::string_view key, VR vr, int value);
	[[nodiscard]] bool set_value(std::string_view key, VR vr, long value);
	[[nodiscard]] bool set_value(std::string_view key, VR vr, long long value);
	[[nodiscard]] bool set_value(std::string_view key, VR vr, double value);
	[[nodiscard]] bool set_value(std::string_view key, VR vr, Tag value);
	[[nodiscard]] bool set_value(std::string_view key, VR vr, std::string_view value);
	[[nodiscard]] bool set_value(std::string_view key, VR vr, const PersonName& value);
	[[nodiscard]] bool set_value(std::string_view key, VR vr, std::span<const int> values);
	[[nodiscard]] bool set_value(std::string_view key, VR vr, std::span<const long> values);
	[[nodiscard]] bool set_value(std::string_view key, VR vr, std::span<const long long> values);
	[[nodiscard]] bool set_value(std::string_view key, VR vr, std::span<const double> values);
	[[nodiscard]] bool set_value(std::string_view key, VR vr, std::span<const Tag> values);
	[[nodiscard]] bool set_value(std::string_view key, VR vr, std::span<const std::string_view> values);
	[[nodiscard]] bool set_value(std::string_view key, VR vr, std::span<const PersonName> values);
	iterator begin();
	iterator end();
	const_iterator begin() const;
	const_iterator end() const;
	const_iterator cbegin() const;
	const_iterator cend() const;

	void attach_to_file(const std::filesystem::path& path);
	void attach_to_memory(const std::uint8_t* data, std::size_t size, bool copy = true);
	void attach_to_memory(const std::string& name, const std::uint8_t* data,
	    std::size_t size, bool copy = true);
	void attach_to_memory(std::string name, std::vector<std::uint8_t>&& buffer);
	void read_attached_stream(const ReadOptions& options = {});
	[[nodiscard]] bool has_error() const noexcept { return has_error_; }
	[[nodiscard]] const std::string& error_message() const noexcept { return error_message_; }

	/// Set transfer syntax for subsequent write operations and synchronize (0002,0010).
	/// This updates both runtime parse/write state and file meta information.
	/// For encapsulated PixelData, encapsulated->encapsulated conversion is handled as
	/// decode-to-native + re-encode when the target transfer syntax supports pixel encode.
	/// This API mutates the in-memory DicomFile and keeps the resulting target PixelData on the
	/// object. For large files whose end goal is serialization, prefer
	/// `write_with_transfer_syntax(...)`.
	void set_transfer_syntax(uid::WellKnown transfer_syntax);
	void set_transfer_syntax(uid::WellKnown transfer_syntax,
	    const pixel::EncoderContext& encoder_ctx);
	void set_transfer_syntax(uid::WellKnown transfer_syntax,
	    std::span<const pixel::CodecOptionTextKv> codec_opt);
	/// Update only the declared (0008,0005) Specific Character Set metadata.
	/// Existing text value bytes are left untouched.
	/// Prefer this API over directly editing the raw (0008,0005) element so the effective
	/// charset cache stays synchronized.
	void set_declared_specific_charset(SpecificCharacterSet charset);
	void set_declared_specific_charset(std::span<const SpecificCharacterSet> charsets);
	void set_declared_specific_charset(std::initializer_list<SpecificCharacterSet> charsets) {
		set_declared_specific_charset(
		    std::span<const SpecificCharacterSet>(charsets.begin(), charsets.size()));
	}
	/// Transcode text VR values to a new Specific Character Set and synchronize (0008,0005)
	/// across the root dataset subtree.
	/// Prefer this API over directly editing the raw (0008,0005) element so the effective
	/// charset cache stays synchronized.
	/// When out_replaced is non-null, it is set when replace_* policy actually substituted data.
	void set_specific_charset(SpecificCharacterSet charset,
	    CharsetEncodeErrorPolicy errors = CharsetEncodeErrorPolicy::strict,
	    bool* out_replaced = nullptr);
	void set_specific_charset(std::span<const SpecificCharacterSet> charsets,
	    CharsetEncodeErrorPolicy errors = CharsetEncodeErrorPolicy::strict,
	    bool* out_replaced = nullptr);
	void set_specific_charset(std::initializer_list<SpecificCharacterSet> charsets,
	    CharsetEncodeErrorPolicy errors = CharsetEncodeErrorPolicy::strict,
	    bool* out_replaced = nullptr) {
		set_specific_charset(std::span<const SpecificCharacterSet>(charsets.begin(), charsets.size()),
		    errors, out_replaced);
	}
	[[nodiscard]] uid::WellKnown transfer_syntax_uid() const { return transfer_syntax_uid_; }
	/// Return true when the dataset carries any pixel payload element.
	[[nodiscard]] bool has_pixel_data() const;
	/// Return the normalized native stored-pixel layout when metadata is complete enough.
	[[nodiscard]] std::optional<pixel::PixelLayout> native_pixel_layout() const;
	[[nodiscard]] pixel::DecodePlan create_decode_plan(
	    const pixel::DecodeOptions& opt = {}) const {
		return pixel::create_decode_plan(*this, opt);
	}
	[[nodiscard]] std::optional<pixel::VoiLut> voi_lut() const;
	[[nodiscard]] std::optional<pixel::VoiLut> voi_lut(std::size_t frame_index) const;
	[[nodiscard]] std::optional<pixel::WindowTransform> window_transform() const;
	[[nodiscard]] std::optional<pixel::WindowTransform> window_transform(
	    std::size_t frame_index) const;
	[[nodiscard]] std::optional<pixel::RescaleTransform> rescale_transform() const;
	[[nodiscard]] std::optional<pixel::RescaleTransform> rescale_transform(
	    std::size_t frame_index) const;
	[[nodiscard]] std::optional<pixel::ModalityLut> modality_lut() const;
	[[nodiscard]] std::optional<pixel::ModalityLut> modality_lut(
	    std::size_t frame_index) const;
	/// Reads the root-level Pixel Presentation attribute when present.
	[[nodiscard]] std::optional<pixel::PixelPresentation> pixel_presentation() const;
	/// Reads the classic root-level PALETTE COLOR LUT module.
	/// This does not interpret Supplemental Palette or Enhanced Palette display-pipeline metadata.
	[[nodiscard]] std::optional<pixel::PaletteLut> palette_lut() const;
	/// Reads the root-level Supplemental Palette Color Lookup Table metadata model.
	[[nodiscard]] std::optional<pixel::SupplementalPaletteInfo> supplemental_palette() const;
	/// Reads the root-level Enhanced Palette Color Lookup Table metadata model.
	[[nodiscard]] std::optional<pixel::EnhancedPaletteInfo> enhanced_palette() const;
	/// Preferred encode entry point for native pixel bytes plus normalized layout metadata.
	void set_pixel_data(uid::WellKnown transfer_syntax,
	    pixel::ConstPixelSpan source);
	void set_pixel_data(uid::WellKnown transfer_syntax,
	    pixel::ConstPixelSpan source, const pixel::EncoderContext& encoder_ctx);
	void set_pixel_data(uid::WellKnown transfer_syntax, pixel::ConstPixelSpan source,
	    std::span<const pixel::CodecOptionTextKv> codec_opt);
	/// Replace PixelData with native bytes (OB/OW) by moving ownership from `native_pixel_data`.
	/// If `vr` is None, OB/OW is inferred from BitsAllocated (<=8 -> OB, otherwise OW).
	void set_native_pixel_data(std::vector<std::uint8_t>&& native_pixel_data, VR vr = VR::None);
	/// Replace PixelData with an encapsulated pixel sequence (VR::PX).
	/// If `frame_count` is non-zero, preallocate that many frame slots and set NumberOfFrames.
	/// This also clears FloatPixelData/DoubleFloatPixelData and invalidates pixel cache.
	void reset_encapsulated_pixel_data(std::size_t frame_count = 0);
	/// Return one encoded PixelData frame as a borrowed byte view.
	/// This may be zero-copy when the source frame is already contiguous, and may
	/// materialize/coalesce frame data when needed. The returned span remains valid
	/// only while the owning DicomFile/PixelSequence backing storage remains valid.
	[[nodiscard]] std::span<const std::uint8_t> encoded_pixel_frame_view(std::size_t frame_index);
	/// Return one encoded PixelData frame as detached owned bytes.
	[[nodiscard]] std::vector<std::uint8_t> encoded_pixel_frame_bytes(std::size_t frame_index);
	/// Copy one encoded frame payload into a preallocated encapsulated frame slot.
	/// This convenience overload copies from a borrowed byte span and forwards to
	/// the existing move-based setter.
	void set_encoded_pixel_frame(std::size_t frame_index, std::span<const std::uint8_t> encoded_frame);
	/// Move one encoded frame payload into a preallocated encapsulated frame slot.
	/// Intended for deterministic frame-index writes (e.g. multi-threaded encoders).
	void set_encoded_pixel_frame(std::size_t frame_index, std::vector<std::uint8_t>&& encoded_frame);
	/// Append one encoded frame payload into encapsulated PixelData (VR::PX).
	/// This convenience overload copies from a borrowed byte span and forwards to
	/// the existing move-based append path.
	[[nodiscard]] PixelFrame* add_encoded_pixel_frame(std::span<const std::uint8_t> encoded_frame);
	/// Append one encoded frame payload into encapsulated PixelData (VR::PX).
	/// Ownership of `encoded_frame` is moved without byte copy.
	/// NumberOfFrames is updated to match the appended frame count.
	[[nodiscard]] PixelFrame* add_encoded_pixel_frame(std::vector<std::uint8_t>&& encoded_frame);
	/// @throws diag::DicomException under the same conditions as pixel::decode_frame_into().
	void decode_into(std::size_t frame_index, std::span<std::uint8_t> dst,
	    const pixel::DecodePlan& plan) const {
		pixel::decode_frame_into(*this, frame_index, dst, plan);
	}
	/// @throws diag::DicomException under the same conditions as pixel::decode_frame().
	[[nodiscard]] pixel::PixelBuffer pixel_buffer(
	    std::size_t frame_index, const pixel::DecodePlan& plan) const {
		return pixel::decode_frame(*this, frame_index, plan);
	}
	/// @throws diag::DicomException under the same conditions as pixel::decode_frame().
	[[nodiscard]] pixel::PixelBuffer pixel_buffer(std::size_t frame_index = 0,
	    const pixel::DecodeOptions& opt = {}) const {
		return pixel_buffer(frame_index, create_decode_plan(opt));
	}
	/// @throws diag::DicomException under the same conditions as pixel::decode_frame_into().
	void decode_into(std::span<std::uint8_t> dst, const pixel::DecodePlan& plan) const {
		pixel::decode_frame_into(*this, 0, dst, plan);
	}
	/// @throws diag::DicomException under the same conditions as pixel::decode_all_frames_into().
	void decode_all_frames_into(std::span<std::uint8_t> dst,
	    const pixel::DecodePlan& plan) const {
		pixel::decode_all_frames_into(*this, dst, plan);
	}
	/// @throws diag::DicomException under the same conditions as pixel::decode_all_frames_into().
	void decode_all_frames_into(std::span<std::uint8_t> dst,
	    const pixel::DecodePlan& plan, const pixel::ExecutionObserver* observer) const {
		pixel::decode_all_frames_into(*this, dst, plan, observer);
	}
	[[nodiscard]] std::vector<std::uint8_t> pixel_data(
	    std::size_t frame_index, const pixel::DecodePlan& plan) const {
		auto decoded = pixel_buffer(frame_index, plan);
		return std::move(decoded.bytes);
	}
	[[nodiscard]] std::vector<std::uint8_t> pixel_data(std::size_t frame_index = 0,
	    const pixel::DecodeOptions& opt = {}) const {
		return pixel_data(frame_index, create_decode_plan(opt));
	}

private:
	friend class DataSet;
	friend void pixel::set_pixel_data(
	    DicomFile& file, pixel::ConstPixelSpan source,
	    const pixel::EncoderContext& encoder_ctx);
	void set_transfer_syntax_state_only(uid::WellKnown transfer_syntax);
	void apply_transfer_syntax(uid::WellKnown transfer_syntax);
	void apply_transfer_syntax(uid::WellKnown transfer_syntax,
	    const pixel::EncoderContext& encoder_ctx);
	void apply_transfer_syntax(uid::WellKnown transfer_syntax,
	    std::span<const pixel::CodecOptionTextKv> codec_opt_override);
	void clear_error_state() noexcept;
	void set_error_state(std::string message);

	DataSet root_dataset_;
	uid::WellKnown transfer_syntax_uid_{};
	bool has_error_{false};
	std::string error_message_{};
};

/// Represents a DICOM SQ element value: an ordered list of nested DataSets.
class Sequence {
public:
	/// Construct a sequence tied to the owning dataset context.
	explicit Sequence(DataSet* owner_dataset);
	~Sequence();
	Sequence(const Sequence&) = delete;
	Sequence& operator=(const Sequence&) = delete;
	Sequence(Sequence&&) noexcept = default;
	Sequence& operator=(Sequence&&) noexcept = default;

	/// Read sequence items from the current stream position.
	void read_from_stream(InStream* instream);

	/// Number of item datasets.
	[[nodiscard]] inline int size() const { return static_cast<int>(seq_.size()); }
	/// Absolute offset of this sequence value in the root stream.
	[[nodiscard]] std::size_t value_offset() const noexcept { return value_offset_; }
	/// Set absolute offset of this sequence value in the root stream.
	void set_value_offset(std::size_t value_offset) noexcept { value_offset_ = value_offset; }

	/// Create and append a new item dataset; returns the created dataset.
	DataSet* add_dataset();
	/// Get item dataset by index.
	DataSet* get_dataset(std::size_t index);
	/// Const item access by index.
	const DataSet* get_dataset(std::size_t index) const;
	/// Indexing operator (mutable).
	DataSet* operator[](std::size_t index);
	/// Indexing operator (const).
	const DataSet* operator[](std::size_t index) const;

	/// Begin iterator over item datasets.
	std::vector<std::unique_ptr<DataSet>>::iterator begin();
	/// End iterator over item datasets.
	std::vector<std::unique_ptr<DataSet>>::iterator end();
	/// Const begin iterator.
	std::vector<std::unique_ptr<DataSet>>::const_iterator begin() const;
	/// Const end iterator.
	std::vector<std::unique_ptr<DataSet>>::const_iterator end() const;
	/// Const begin iterator (alias).
	std::vector<std::unique_ptr<DataSet>>::const_iterator cbegin() const;
	/// Const end iterator (alias).
	std::vector<std::unique_ptr<DataSet>>::const_iterator cend() const;

private:
	DataSet* owner_dataset_{nullptr};
	DataSet* root_dataset_{nullptr};
	std::size_t value_offset_{0};
	std::vector<std::unique_ptr<DataSet>> seq_;
};

struct PixelFragment {
	std::size_t offset{0};  // byte offset of fragment value (relative to pixel sequence base)
	std::size_t length{0};  // byte length of fragment value
};

class PixelFrame {
public:
	PixelFrame();
	~PixelFrame();
	PixelFrame(const PixelFrame&) = delete;
	PixelFrame& operator=(const PixelFrame&) = delete;
	PixelFrame(PixelFrame&&) noexcept = default;
	PixelFrame& operator=(PixelFrame&&) noexcept = default;

	void set_encoded_data(std::vector<std::uint8_t> data);
	[[nodiscard]] std::span<const std::uint8_t> encoded_data_view() const noexcept;
	[[nodiscard]] std::size_t encoded_data_size() const noexcept { return encoded_data_.size(); }
	Tag read_from_stream(InStream* stream, std::size_t frame_length, uid::WellKnown ts, bool length_from_bot = false);
	// Clears the encoded buffer and releases its capacity.
	void discard_encoded_data();
	[[nodiscard]] const std::vector<PixelFragment>& fragments() const noexcept { return fragments_; }
	void set_fragments(std::vector<PixelFragment> fragments) { fragments_ = std::move(fragments); }
	// Returns spans for each fragment without copying (requires valid sequence stream).
	std::vector<std::span<const std::uint8_t>> fragment_views(const InStream& seq_stream) const;
	// Coalesces all fragments into a single contiguous buffer.
	std::vector<std::uint8_t> coalesce_encoded_data(const InStream& seq_stream) const;

private:
	std::vector<std::uint8_t> encoded_data_;
	std::vector<PixelFragment> fragments_;
};

/// Encapsulated pixel data as defined by PS3.5: owns fragments, offset tables, and frames.
class PixelSequence {
	public:
		/// Construct an encapsulated pixel sequence tied to the root dataset and transfer syntax.
		PixelSequence(DataSet* root_dataset, uid::WellKnown transfer_syntax);
		~PixelSequence();
		PixelSequence(const PixelSequence&) = delete;
		PixelSequence& operator=(const PixelSequence&) = delete;
		PixelSequence(PixelSequence&&) noexcept;
	PixelSequence& operator=(PixelSequence&&) noexcept;

	/// Append a new pixel frame and return it.
	PixelFrame* add_frame();
	/// Mutable frame access.
	PixelFrame* frame(std::size_t index);
	/// Const frame access.
	const PixelFrame* frame(std::size_t index) const;
	/// Number of frames contained.
	[[nodiscard]] std::size_t number_of_frames() const noexcept { return frames_.size(); }
	/// Root dataset owning this pixel sequence.
	[[nodiscard]] DataSet* root_dataset() const noexcept { return root_dataset_; }
	/// Transfer syntax associated with this pixel sequence.
	[[nodiscard]] uid::WellKnown transfer_syntax_uid() const noexcept { return transfer_syntax_; }
	/// Absolute offset of this pixel sequence value in the root stream.
	[[nodiscard]] std::size_t value_offset() const noexcept { return value_offset_; }
	/// Set absolute offset of this pixel sequence value in the root stream.
	void set_value_offset(std::size_t value_offset) noexcept { value_offset_ = value_offset; }
	/// Base offset of the pixel sequence within the root stream.
	[[nodiscard]] std::size_t base_offset() const noexcept { return base_offset_; }
	/// Offset of the basic offset table within the stream.
	[[nodiscard]] std::size_t basic_offset_table_offset() const noexcept { return basic_offset_table_offset_; }
	/// Number of entries in the basic offset table.
	[[nodiscard]] std::size_t basic_offset_table_count() const noexcept { return basic_offset_table_count_; }
	/// Offset of the extended offset table within the stream.
	[[nodiscard]] std::size_t extended_offset_table_offset() const noexcept { return extended_offset_table_offset_; }
	/// Number of entries in the extended offset table.
	[[nodiscard]] std::size_t extended_offset_table_count() const noexcept { return extended_offset_table_count_; }
	/// Encoded frame bytes as a span (requires attached stream).
	std::span<const std::uint8_t> frame_encoded_span(std::size_t index);
	/// Drop encoded data buffer for a given frame to free memory.
	void clear_frame_encoded_data(std::size_t index);

		/// Attach to a substream containing the pixel sequence payload.
		void attach_to_stream(InStream* base_stream, std::size_t size);
		/// Read the attached pixel sequence stream (fragments, offset tables).
		void read_attached_stream();
	/// Access the underlying pixel sequence stream.
	[[nodiscard]] InStream* stream() noexcept { return stream_.get(); }
	/// Const access to the underlying pixel sequence stream.
	[[nodiscard]] const InStream* stream() const noexcept { return stream_.get(); }

private:
	DataSet* root_dataset_{nullptr};
	uid::WellKnown transfer_syntax_{};
	std::unique_ptr<InStream> stream_;
	std::size_t value_offset_{0};
	std::size_t base_offset_{0};
	std::size_t basic_offset_table_offset_{0};
	std::size_t basic_offset_table_count_{0};
	std::size_t extended_offset_table_offset_{0};
	std::size_t extended_offset_table_count_{0};
	std::vector<std::unique_ptr<PixelFrame>> frames_;
};

#include "dataset_value_access.inl.hpp"


inline DataElement::DataElement(Tag tag, VR vr, std::size_t length, std::size_t offset,
    DataSet* parent) noexcept
    : tag_(tag), vr_(vr), storage_kind_(StorageKind::none), length_(length),
      storage_(), parent_(parent) {
	initialize_storage(offset, false);
}

inline void DataElement::initialize_storage(
    std::size_t offset, bool bind_to_parent_stream) noexcept {
	if (vr_.is_sequence()) {
		storage_.seq = new Sequence(parent_);
		storage_.seq->set_value_offset(offset);
		storage_kind_ = StorageKind::sequence;
	} else if (vr_.is_pixel_sequence()) {
		const auto ts = parent_ ? parent_->transfer_syntax_uid() : uid::WellKnown{};
		storage_.pixseq = new PixelSequence(parent_, ts);
		storage_.pixseq->set_value_offset(offset);
		storage_kind_ = StorageKind::pixel_sequence;
	} else if (bind_to_parent_stream && parent_) {
		storage_.offset_ = offset;
		storage_kind_ = StorageKind::stream;
	}
}

inline void DataElement::reset(Tag tag, VR vr, std::size_t length, std::size_t offset,
    DataSet* parent, bool bind_to_parent_stream) noexcept {
	release_storage();
	reset_without_release(tag, vr, length, offset, parent, bind_to_parent_stream);
}

inline void DataElement::reset_without_release(Tag tag, VR vr, std::size_t length,
    std::size_t offset, DataSet* parent, bool bind_to_parent_stream) noexcept {
	tag_ = tag;
	vr_ = vr;
	length_ = length;
	parent_ = parent;
	storage_.ptr = nullptr;
	storage_kind_ = StorageKind::none;
	initialize_storage(offset, bind_to_parent_stream);
}

inline void DataElement::release_storage() noexcept {
	switch (storage_kind_) {
	case StorageKind::heap:
		if (storage_.ptr) {
			auto* storage_base =
			    static_cast<std::uint8_t*>(storage_.ptr) - sizeof(std::size_t);
			::operator delete(storage_base);
		}
		break;
	case StorageKind::owned_bytes:
		delete storage_.vec;
		break;
	case StorageKind::sequence:
		delete storage_.seq;
		break;
	case StorageKind::pixel_sequence:
		delete storage_.pixseq;
		break;
	case StorageKind::none:
	case StorageKind::stream:
	case StorageKind::inline_bytes:
		break;
	}
	storage_.ptr = nullptr;
	storage_kind_ = StorageKind::none;
}

inline std::size_t DataElement::offset() const noexcept {
	switch (storage_kind_) {
	case StorageKind::stream:
		return storage_.offset_;
	case StorageKind::sequence:
		return storage_.seq ? storage_.seq->value_offset() : 0;
	case StorageKind::pixel_sequence:
		return storage_.pixseq ? storage_.pixseq->value_offset() : 0;
	default:
		return 0;
	}
}

inline Sequence* DataElement::as_sequence() {
	return storage_kind_ == StorageKind::sequence ? storage_.seq : nullptr;
}

inline const Sequence* DataElement::as_sequence() const {
	return storage_kind_ == StorageKind::sequence ? storage_.seq : nullptr;
}

inline PixelSequence* DataElement::as_pixel_sequence() {
	return storage_kind_ == StorageKind::pixel_sequence ? storage_.pixseq : nullptr;
}

inline const PixelSequence* DataElement::as_pixel_sequence() const {
	return storage_kind_ == StorageKind::pixel_sequence ? storage_.pixseq : nullptr;
}

/// Fast 1 KiB prefix probe for file filtering.
/// Returns true when the file prefix looks like a Part 10 stream or a raw little-endian
/// DICOM dataset. This is a sniffing heuristic, not a full validation pass.
[[nodiscard]] bool is_dicom_file(const std::filesystem::path& path);
/// Read a DICOM file/session from disk (eager up to options.load_until).
/// When options.keep_on_error is true, parse errors are captured in DicomFile::error_message().
std::unique_ptr<DicomFile> read_file(const std::filesystem::path& path, ReadOptions options = {});
/// Read from a raw memory buffer (identifier set to "<memory>"); copies unless options.copy is false.
std::unique_ptr<DicomFile> read_bytes(const std::uint8_t* data, std::size_t size,
    ReadOptions options = {});
/// Read from a named raw memory buffer.
std::unique_ptr<DicomFile> read_bytes(const std::string& name, const std::uint8_t* data,
    std::size_t size, ReadOptions options = {});
/// Read from an owning buffer moved into the dataset.
std::unique_ptr<DicomFile> read_bytes(std::string name, std::vector<std::uint8_t>&& buffer,
    ReadOptions options = {});
/// Return the current adaptive reserve hint used by root DataSet::read_attached_stream().
[[nodiscard]] std::size_t load_root_elements_reserve_hint() noexcept;
/// Reset the adaptive reserve hint used by root DataSet::read_attached_stream().
/// Intended for deterministic benchmarking and test setup.
void reset_root_elements_reserve_hint() noexcept;

} // namespace dicom
