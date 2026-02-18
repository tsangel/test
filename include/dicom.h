#pragma once

#include <array>
#include <cstddef>
#include <compare>
#include <cstdint>
#include <cstring>
#include <limits>
#include <iterator>
#include <map>
#include <memory>
#include <cstdio>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#include "instream.h"
#include "dicom_endian.h"
#include "dataelement_lookup_detail.hpp"
#include "specific_character_set_registry.hpp"
#include "version.h"
#include "uid_lookup_detail.hpp"

namespace dicom {

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

	static constexpr std::uint32_t tag_value_from_text(std::string_view text) {
		if (const auto* entry = lookup::keyword_to_entry_chd(text)) {
			return entry->tag_value;
		}
		std::uint32_t numeric_value = 0;
		if (try_parse_numeric_tag(text, numeric_value)) {
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

// Load a Tag from two consecutive uint16 fields, honoring the source endianness.
inline Tag load_tag(const void* ptr, bool little_endian_source) noexcept {
	const auto* byte_ptr = static_cast<const std::uint8_t*>(ptr);
	const auto group = load_value<std::uint16_t>(byte_ptr, little_endian_source);
	const auto element = load_value<std::uint16_t>(byte_ptr + 2, little_endian_source);
	return Tag(group, element);
}

// Load a Tag assuming little-endian source bytes.
inline Tag load_tag_le(const void* ptr) noexcept {
	const auto* byte_ptr = static_cast<const std::uint8_t*>(ptr);
	const auto group = load_le<std::uint16_t>(byte_ptr);
	const auto element = load_le<std::uint16_t>(byte_ptr + 2);
	return Tag(group, element);
}

}  // namespace endian

namespace uid {

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
	[[nodiscard]] constexpr bool is_jpegxl() const noexcept;
	[[nodiscard]] constexpr bool is_rle() const noexcept;
	[[nodiscard]] constexpr bool is_uncompressed() const noexcept;
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

inline WellKnown require(std::string_view text) {
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

}  // namespace uid

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
		return is_known() && value == SQ_val;
	}

	constexpr bool is_pixel_sequence() const noexcept {
		return is_known() && value == PX_val;
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
	if (const auto* entry = keyword_to_entry_chd(keyword)) {
		return {Tag::from_value(entry->tag_value), VR(entry->vr_value)};
	}
	return {Tag{}, VR{}};
}

constexpr std::string_view keyword_to_tag(std::string_view keyword) {
	if (const auto* entry = keyword_to_entry_chd(keyword)) {
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

consteval Tag operator"" _tag(const char* text, std::size_t len) {
	return Tag(std::string_view{text, len});
}

consteval uid::WellKnown operator"" _uid(const char* text, std::size_t len) {
	return uid::WellKnown{uid_lookup::uid_index_from_text(std::string_view{text, len})};
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
constexpr std::uint32_t kTSJpegBaseline = 1u << 1;
constexpr std::uint32_t kTSJpegLossless = 1u << 2;
constexpr std::uint32_t kTSJpegLS = 1u << 3;
constexpr std::uint32_t kTSJpeg2000 = 1u << 4;
constexpr std::uint32_t kTSMpeg2 = 1u << 5;
constexpr std::uint32_t kTSH264 = 1u << 6;
constexpr std::uint32_t kTSHevc = 1u << 7;
constexpr std::uint32_t kTSJpegXL = 1u << 8;
constexpr std::uint32_t kTSRle = 1u << 9;
constexpr std::uint32_t kTSFfd9 = 1u << 10;      // Codestream ends with FFD9 marker
constexpr std::uint32_t kTSJpegFamily = 1u << 11;

inline constexpr std::uint32_t ts_mask(std::uint16_t idx) {
	switch (idx) {
		// Uncompressed image transfer syntaxes
	case "ImplicitVRLittleEndian"_uid.raw_index():
	case "ExplicitVRLittleEndian"_uid.raw_index():
	case "EncapsulatedUncompressedExplicitVRLittleEndian"_uid.raw_index():
	case "DeflatedExplicitVRLittleEndian"_uid.raw_index():
	case "ExplicitVRBigEndian"_uid.raw_index():
	case "Papyrus3ImplicitVRLittleEndian"_uid.raw_index():
		return kTSUncompressed;

		// JPEG Baseline / Extended / Progressive
	case "JPEGBaseline8Bit"_uid.raw_index():
	case "JPEGExtended12Bit"_uid.raw_index():
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
		return kTSJpegBaseline | kTSJpegFamily | kTSFfd9;

		// JPEG Lossless
	case "JPEGLossless"_uid.raw_index():
	case "JPEGLosslessNonHierarchical15"_uid.raw_index():
	case "JPEGLosslessHierarchical28"_uid.raw_index():
	case "JPEGLosslessHierarchical29"_uid.raw_index():
	case "JPEGLosslessSV1"_uid.raw_index():
		return kTSJpegLossless | kTSJpegFamily | kTSFfd9;

		// JPEG-LS
	case "JPEGLSLossless"_uid.raw_index():
	case "JPEGLSNearLossless"_uid.raw_index():
		return kTSJpegLS | kTSJpegFamily | kTSFfd9;

		// JPEG 2000 / JPIP Referenced
	case "JPEG2000Lossless"_uid.raw_index():
	case "JPEG2000"_uid.raw_index():
	case "JPEG2000MCLossless"_uid.raw_index():
	case "JPEG2000MC"_uid.raw_index():
	case "JPIPReferenced"_uid.raw_index():
	case "JPIPReferencedDeflate"_uid.raw_index():
		return kTSJpeg2000 | kTSJpegFamily;

		// MPEG-2
	case "MPEG2MPML"_uid.raw_index():
	case "MPEG2MPMLF"_uid.raw_index():
	case "MPEG2MPHL"_uid.raw_index():
	case "MPEG2MPHLF"_uid.raw_index():
		return kTSMpeg2;

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
		return kTSH264;

		// HEVC
	case "HEVCMP51"_uid.raw_index():
	case "HEVCM10P51"_uid.raw_index():
		return kTSHevc;

		// JPEG XL
	case "JPEGXLLossless"_uid.raw_index():
	case "JPEGXLJPEGRecompression"_uid.raw_index():
	case "JPEGXL"_uid.raw_index():
		return kTSJpegXL | kTSJpegFamily | kTSFfd9;

		// HTJ2K / JPIP HTJ2K Referenced
	case "HTJ2KLossless"_uid.raw_index():
	case "HTJ2KLosslessRPCL"_uid.raw_index():
	case "HTJ2K"_uid.raw_index():
	case "JPIPHTJ2KReferenced"_uid.raw_index():
	case "JPIPHTJ2KReferencedDeflate"_uid.raw_index():
		return kTSJpeg2000 | kTSJpegFamily;

		// RLE
	case "RLELossless"_uid.raw_index():
		return kTSRle;

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

inline constexpr bool WellKnown::is_jpegxl() const noexcept {
	return detail::ts_mask(raw_index()) & detail::kTSJpegXL;
}

inline constexpr bool WellKnown::is_rle() const noexcept {
	return detail::ts_mask(raw_index()) & detail::kTSRle;
}

inline constexpr bool WellKnown::is_uncompressed() const noexcept {
	return detail::ts_mask(raw_index()) & detail::kTSUncompressed;
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
class PixelSequence;

class DataSet;
/// Options controlling how a DataSet is read from a stream.
struct ReadOptions {
	Tag load_until{Tag(0xFFFFu, 0xFFFFu)};
	bool keep_on_error{false};
	bool copy{true};
};

/// Represents a collection of DICOM data elements backed by a file or memory stream.
/// The root DataSet owns the input stream; elements are parsed lazily on demand.
class DataElement {
public:
	constexpr DataElement() noexcept = default;
	~DataElement() { release_storage(); }

	DataElement(const DataElement&) = delete;
	DataElement(const DataElement&&) = delete;
	DataElement& operator=(const DataElement&) = delete;

	DataElement(DataElement&& other) noexcept { move_from(std::move(other)); }
	DataElement& operator=(DataElement&& other) noexcept {
		if (this != &other) {
			release_storage();
			move_from(std::move(other));
		}
		return *this;
	}

	DataElement(Tag tag, VR vr, std::size_t length, std::size_t offset,
	    DataSet* parent = nullptr) noexcept;

	/// Tag of this element.
	[[nodiscard]] constexpr Tag tag() const noexcept { return tag_; }
	/// VR of this element.
	[[nodiscard]] constexpr VR vr() const noexcept { return vr_; }
	/// Value length in bytes (may be undefined for sequences).
	[[nodiscard]] constexpr std::size_t length() const noexcept { return length_; }
	/// Absolute offset of the value within the root stream.
	[[nodiscard]] constexpr std::size_t offset() const noexcept { return offset_; }
	/// Parent dataset (if any).
	[[nodiscard]] constexpr DataSet* parent() const noexcept { return parent_; }
	/// Truthy when this element is present (VR not None).
	[[nodiscard]] constexpr explicit operator bool() const noexcept { return vr_ != VR::None; }
	/// Raw value as a byte span; for SQ/PixelData this is the encoded payload.
	[[nodiscard]] std::span<const std::uint8_t> value_span() const;
	/// Pointer to stored value or nested sequence/pixel sequence.
	void* value_ptr() const;
	/// Value multiplicity; returns -1 when not applicable or parse fails.
	[[nodiscard]] int vm() const;
	/// Raw storage pointer for non-sequence VRs.
	[[nodiscard]] constexpr void* data() const noexcept { return storage_.ptr; }
	/// Returns the nested Sequence value if VR indicates a sequence, otherwise nullptr.
	[[nodiscard]] constexpr Sequence* sequence() const noexcept {
		return vr_.is_sequence() ? storage_.seq : nullptr;
	}
	/// Returns the nested PixelSequence value if VR indicates encapsulated pixel data, otherwise nullptr.
	[[nodiscard]] constexpr PixelSequence* pixel_sequence() const noexcept {
		return vr_.is_pixel_sequence() ? storage_.pixseq : nullptr;
	}
	Sequence* as_sequence();
	const Sequence* as_sequence() const;
	PixelSequence* as_pixel_sequence();
	const PixelSequence* as_pixel_sequence() const;

	constexpr void set_tag(Tag tag) noexcept { tag_ = tag; }
	constexpr void set_vr(VR vr) noexcept { vr_ = vr; }
	constexpr void set_length(std::size_t length) noexcept { length_ = length; }
	constexpr void set_offset(std::size_t offset) noexcept { offset_ = offset; }
	constexpr void set_parent(DataSet* parent) noexcept { parent_ = parent; }

	/// Set raw storage pointer (binary/string VRs).
	constexpr void set_data(void* ptr) noexcept { storage_.ptr = ptr; }
	/// Set nested sequence pointer (SQ VR only).
	constexpr void set_sequence(Sequence* seq) noexcept { storage_.seq = seq; }
	/// Set nested pixel sequence pointer (encapsulated pixel data).
	constexpr void set_pixel_sequence(PixelSequence* pixseq) noexcept {
		storage_.pixseq = pixseq;
	}

	// Numeric accessors (PS3.5 6.2 Value Representation)
	/// Parse value as signed int; empty on failure.
	[[nodiscard]] std::optional<int> to_int() const;
	/// Parse value as signed long; empty on failure.
	[[nodiscard]] std::optional<long> to_long() const;
	/// Parse value as signed long long; empty on failure.
	[[nodiscard]] std::optional<long long> to_longlong() const;
	/// Parse value as vector of signed int; empty on failure.
	[[nodiscard]] std::optional<std::vector<int>> to_int_vector() const;
	/// Treat underlying value bytes as little/endian-aware uint16 vector; empty on failure.
	[[nodiscard]] std::optional<std::vector<std::uint16_t>> as_uint16_vector() const;
	/// Treat underlying value bytes as uint8 vector; empty on failure.
	[[nodiscard]] std::optional<std::vector<std::uint8_t>> as_uint8_vector() const;
	/// Parse value as vector of signed long; empty on failure.
	[[nodiscard]] std::optional<std::vector<long>> to_long_vector() const;
	/// Parse value as vector of signed long long; empty on failure.
	[[nodiscard]] std::optional<std::vector<long long>> to_longlong_vector() const;
	/// Parse value as double; empty on failure.
	[[nodiscard]] std::optional<double> to_double() const;
	/// Parse value as vector of double; empty on failure.
	[[nodiscard]] std::optional<std::vector<double>> to_double_vector() const;
	/// Parse value as Tag.
	[[nodiscard]] std::optional<Tag> to_tag() const;
	/// Parse value as vector of Tag.
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
	[[nodiscard]] std::optional<std::vector<std::string_view>> to_string_views() const;
	/// Parse value as UTF-8 string_view (using Specific Character Set when applicable).
	[[nodiscard]] std::optional<std::string_view> to_utf8_view() const;
	/// Parse value as multiple UTF-8 string_views.
	[[nodiscard]] std::optional<std::vector<std::string_view>> to_utf8_views() const;

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

	// Convenience wrappers with default values
	[[nodiscard]] inline int toInt(int default_value = 0) const {
		return to_int().value_or(default_value);
	}
	[[nodiscard]] inline long toLong(long default_value = 0) const {
		return to_long().value_or(default_value);
	}
	[[nodiscard]] inline long long toLongLong(long long default_value = 0) const {
		return to_longlong().value_or(default_value);
	}
	[[nodiscard]] inline std::vector<int> toIntVector(std::vector<int> default_value = {}) const {
		return to_int_vector().value_or(std::move(default_value));
	}
	[[nodiscard]] inline std::vector<std::uint16_t> asUint16Vector(std::vector<std::uint16_t> default_value = {}) const {
		return as_uint16_vector().value_or(std::move(default_value));
	}
	[[nodiscard]] inline std::vector<std::uint8_t> asUint8Vector(std::vector<std::uint8_t> default_value = {}) const {
		return as_uint8_vector().value_or(std::move(default_value));
	}
	[[nodiscard]] inline std::vector<long> toLongVector(std::vector<long> default_value = {}) const {
		return to_long_vector().value_or(std::move(default_value));
	}
	[[nodiscard]] inline std::vector<long long> toLongLongVector(std::vector<long long> default_value = {}) const {
		return to_longlong_vector().value_or(std::move(default_value));
	}
	[[nodiscard]] inline double toDouble(double default_value = 0.0) const {
		return to_double().value_or(default_value);
	}
	[[nodiscard]] inline std::vector<double> toDoubleVector(std::vector<double> default_value = {}) const {
		return to_double_vector().value_or(std::move(default_value));
	}
	[[nodiscard]] inline Tag toTag(Tag default_value = Tag{}) const {
		return to_tag().value_or(default_value);
	}
	[[nodiscard]] inline std::vector<Tag> toTagVector(std::vector<Tag> default_value = {}) const {
		return to_tag_vector().value_or(std::move(default_value));
	}

private:
	Tag tag_{};
	VR vr_{};
	std::size_t length_{0};
	std::size_t offset_{0};
	union Storage {
		void* ptr;
		Sequence* seq;
		PixelSequence* pixseq;
		constexpr Storage() noexcept : ptr(nullptr) {}
	} storage_{};
	DataSet* parent_{nullptr};

	void release_storage() noexcept;

	void move_from(DataElement&& other) noexcept {
		tag_ = other.tag_;
		vr_ = other.vr_;
		length_ = other.length_;
		offset_ = other.offset_;
		storage_.ptr = other.storage_.ptr;
		parent_ = other.parent_;
		other.storage_.ptr = nullptr;
	}
};

DataElement* NullElement();


template <typename VecIter, typename MapIter, typename Ref, typename Ptr>
/// Forward iterator that merges vector- and map-backed element storage.
class DataElementIterator {
public:
	using iterator_category = std::forward_iterator_tag;
	using value_type = std::remove_reference_t<Ref>;
	using difference_type = std::ptrdiff_t;
	using reference = Ref;
	using pointer = Ptr;

	constexpr DataElementIterator() = default;

	DataElementIterator(VecIter vec_it, VecIter vec_end, MapIter map_it, MapIter map_end)
	    : vec_it_(vec_it), vec_end_(vec_end), map_it_(map_it), map_end_(map_end) {
		select_source();
	}

	reference operator*() const {
		return use_vector_ ? *vec_it_ : map_it_->second;
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
		return lhs.vec_it_ == rhs.vec_it_ && lhs.map_it_ == rhs.map_it_;
	}

	friend bool operator!=(const DataElementIterator& lhs, const DataElementIterator& rhs) {
		return !(lhs == rhs);
	}

private:
	void advance_active() {
		if (use_vector_) {
			if (vec_it_ != vec_end_) {
				++vec_it_;
			}
		} else if (map_it_ != map_end_) {
			++map_it_;
		}
	}

	void select_source() {
		while (true) {
			const bool vec_done = vec_it_ == vec_end_;
			const bool map_done = map_it_ == map_end_;

			if (vec_done && map_done) {
				use_vector_ = false;
				return;
			}

			if (!vec_done && (map_done || vec_it_->tag().value() <= map_it_->first)) {
				if (vec_it_->vr() == VR::None) {
					++vec_it_;
					continue;
				}
				use_vector_ = true;
				return;
			}

			if (!map_done) {
				if (map_it_->second.vr() == VR::None) {
					++map_it_;
					continue;
				}
				use_vector_ = false;
				return;
			}
		}
	}

	VecIter vec_it_{};
	VecIter vec_end_{};
	MapIter map_it_{};
	MapIter map_end_{};
	bool use_vector_{false};
};


	/// Root or nested DICOM dataset. Holds elements and the underlying stream identifier,
	/// and supports lazy reading/iteration of elements.
	class DataSet {
	public:
		using iterator = DataElementIterator<std::vector<DataElement>::iterator,
		    std::map<std::uint32_t, DataElement>::iterator, DataElement&, DataElement*>;
		using const_iterator = DataElementIterator<std::vector<DataElement>::const_iterator,
		    std::map<std::uint32_t, DataElement>::const_iterator, const DataElement&, const DataElement*>;

		/// Construct an empty root dataset.
		DataSet();
		/// Construct a child dataset sharing the same root stream.
		explicit DataSet(DataSet* root_dataset);
		~DataSet();
		DataSet(const DataSet&) = delete;
		DataSet& operator=(const DataSet&) = delete;
		DataSet(DataSet&&) noexcept = default;
	DataSet& operator=(DataSet&&) noexcept = default;

	/// Attach the dataset to a DICOM file on disk. Lazy: parsing happens on first access.
	/// @param path Filesystem path to the DICOM file.
	void attach_to_file(const std::string& path);

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
		/// overwriting the underlying file; some OSes (especially Windows) may refuse removal
		/// while a mapping handle is open.
		void attach_to_memory(std::string name, std::vector<std::uint8_t>&& buffer);

		/// Returns the current stream identifier (file path, provided name, or "<memory>").
		const std::string& path() const;

	/// Access the underlying input stream (root dataset only).
	InStream& stream();

	/// Access the underlying input stream (const).
	const InStream& stream() const;

	/// Attach a sub-range of another stream (used internally for sequences/pixel data).
	void attach_to_substream(InStream* basestream, std::size_t size);

	/// Set the absolute offset of this dataset within the root stream.
	void set_offset(std::size_t offset) { offset_ = offset; }

	/// Absolute offset of this dataset within the root stream.
	[[nodiscard]] std::size_t offset() const { return offset_; }

	/// Add or replace a data element (vector-backed, preserves insertion order).
	/// @return Pointer to the inserted element.
	DataElement* add_dataelement(Tag tag, VR vr=VR::None, std::size_t offset=0, std::size_t length=0);

	/// Remove a data element by tag (no-op if missing).
	void remove_dataelement(Tag tag);

	/// Lookup a data element by tag. Does not load implicitly; call ensure_loaded(tag) or read_attached_stream() first.
	DataElement* get_dataelement(Tag tag);

	/// Const lookup by tag. Does not load implicitly; call ensure_loaded(tag) or read_attached_stream() first.
	const DataElement* get_dataelement(Tag tag) const;

	/// Resolve a dotted tag path (e.g., "00540016.0.00181075"). Caller must ensure_loaded() the needed prefixes.
	DataElement* get_dataelement(std::string_view tag_path);

	/// Const resolve of a dotted tag path. Caller must ensure_loaded() the needed prefixes.
	const DataElement* get_dataelement(std::string_view tag_path) const;

	/// Map-style access; throws if missing.
	DataElement& operator[](Tag tag);

	/// Const map-style access; throws if missing.
	const DataElement& operator[](Tag tag) const;

	/// Print elements to stdout (debug).
	void dump_elements() const;

	/// Read all elements from the attached stream with options.
		void read_attached_stream(const ReadOptions& options);

		/// Read elements until the given tag boundary.
		void read_elements_until(Tag load_until, InStream* stream);

		/// Ensure the given tag (and preceding tags) are loaded.
		void ensure_loaded(Tag tag);

	/// Const version of ensure_loaded.
	void ensure_loaded(Tag tag) const;

	/// Transfer syntax endianness flag.
	[[nodiscard]] inline bool is_little_endian() const { return little_endian_; }

	/// Transfer syntax explicit VR flag.
	[[nodiscard]] inline bool is_explicit_vr() const { return explicit_vr_; }

	/// Well-known transfer syntax UID for this dataset.
	[[nodiscard]] inline uid::WellKnown transfer_syntax_uid() const { return transfer_syntax_uid_; }

	/// Begin iterator over active elements (vector + map merge).
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

private:
	void attach_to_stream(std::string identifier, std::unique_ptr<InStream> stream);
	void set_transfer_syntax(uid::WellKnown transfer_syntax);
	std::unique_ptr<InStream> stream_;
	DataSet* root_dataset_{this};
	Tag last_tag_loaded_{Tag::from_value(0)};
	uid::WellKnown transfer_syntax_uid_{};
	bool little_endian_{true};
	bool explicit_vr_{true};
	alignas(std::uint64_t) std::uint8_t buf8_[8];  // temporary buffer for tag, vr and length
	std::size_t offset_{0};  // absolute offset within the root stream where this dataset starts
	std::vector<DataElement> elements_;
	std::map<std::uint32_t, DataElement> element_map_;
};

/// Represents a DICOM SQ element value: an ordered list of nested DataSets.
	class Sequence {
	public:
		/// Construct a sequence tied to the given root dataset.
		explicit Sequence(DataSet* root_dataset);
		~Sequence();
		Sequence(const Sequence&) = delete;
		Sequence& operator=(const Sequence&) = delete;
		Sequence(Sequence&&) noexcept = default;
	Sequence& operator=(Sequence&&) noexcept = default;

	/// Read sequence items from the current stream position.
	void read_from_stream(InStream* instream);

	/// Number of item datasets.
	[[nodiscard]] inline int size() const { return static_cast<int>(seq_.size()); }

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
	DataSet* root_dataset_{nullptr};
	[[maybe_unused]] uid::WellKnown transfer_syntax_uid_{};
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
		void attach_to_stream(InStream* basestream, std::size_t size);
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
	std::size_t base_offset_{0};
	std::size_t basic_offset_table_offset_{0};
	std::size_t basic_offset_table_count_{0};
	std::size_t extended_offset_table_offset_{0};
	std::size_t extended_offset_table_count_{0};
	std::vector<std::unique_ptr<PixelFrame>> frames_;
};


inline DataElement::DataElement(Tag tag, VR vr, std::size_t length, std::size_t offset,
    DataSet* parent) noexcept
    : tag_(tag), vr_(vr), length_(length), offset_(offset), storage_(), parent_(parent) {
	if (vr_.is_sequence()) {
		storage_.seq = new Sequence(parent_);
	} else if (vr_.is_pixel_sequence()) {
		const auto ts = parent_ ? parent_->transfer_syntax_uid() : uid::WellKnown{};
		storage_.pixseq = new PixelSequence(parent_, ts);
	}
}

inline void DataElement::release_storage() noexcept {
	if (!storage_.ptr) {
		return;
	}
	if (vr_.is_sequence()) {
		delete storage_.seq;
	} else if (vr_.is_pixel_sequence()) {
		delete storage_.pixseq;
	} else {
		::operator delete(storage_.ptr);
	}
	storage_.ptr = nullptr;
}

inline Sequence* DataElement::as_sequence() {
	return vr_.is_sequence() ? storage_.seq : nullptr;
}

inline const Sequence* DataElement::as_sequence() const {
	return vr_.is_sequence() ? storage_.seq : nullptr;
}

inline PixelSequence* DataElement::as_pixel_sequence() {
	return vr_.is_pixel_sequence() ? storage_.pixseq : nullptr;
}

inline const PixelSequence* DataElement::as_pixel_sequence() const {
	return vr_.is_pixel_sequence() ? storage_.pixseq : nullptr;
}

/// Read a DICOM dataset from disk (eager up to options.load_until).
std::unique_ptr<DataSet> read_file(const std::string& path, ReadOptions options = {});
/// Read from a raw memory buffer (identifier set to "<memory>"); copies unless options.copy is false.
std::unique_ptr<DataSet> read_bytes(const std::uint8_t* data, std::size_t size,
    ReadOptions options = {});
/// Read from a named raw memory buffer.
std::unique_ptr<DataSet> read_bytes(const std::string& name, const std::uint8_t* data,
    std::size_t size, ReadOptions options = {});
/// Read from an owning buffer moved into the dataset.
std::unique_ptr<DataSet> read_bytes(std::string name, std::vector<std::uint8_t>&& buffer,
    ReadOptions options = {});

} // namespace dicom
