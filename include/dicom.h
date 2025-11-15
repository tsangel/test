#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "dataelement_lookup_detail.hpp"
#include "specific_character_set_registry.hpp"
#include "version.h"
#include "uid_lookup_detail.hpp"

namespace dicom {

using std::uint8_t;
using std::uint16_t;

class InStream;

struct Tag {
	std::uint32_t packed{0};

	constexpr Tag() = default;
	constexpr Tag(std::uint16_t group, std::uint16_t element)
	    : packed((static_cast<std::uint32_t>(group) << 16) | static_cast<std::uint32_t>(element)) {}
	constexpr explicit Tag(std::uint32_t value) : packed(value) {}
	constexpr explicit Tag(std::string_view keyword)
	    : packed(tag_value_from_keyword(keyword)) {}

	static constexpr Tag from_value(std::uint32_t value) { return Tag(value); }

	[[nodiscard]] constexpr std::uint16_t group() const {
		return static_cast<std::uint16_t>(packed >> 16);
	}

	[[nodiscard]] constexpr std::uint16_t element() const {
		return static_cast<std::uint16_t>(packed & 0xFFFFu);
	}

	[[nodiscard]] constexpr std::uint32_t value() const { return packed; }

	[[nodiscard]] constexpr bool is_private() const { return (group() & 0x0001u) != 0; }
	[[nodiscard]] constexpr explicit operator bool() const { return packed != 0; }

    // ------------------------------------------------------------
    // Comparisons and conversions
    // ------------------------------------------------------------
    constexpr explicit operator uint32_t() const noexcept { return packed; }
    constexpr auto operator<=>(const Tag&) const noexcept = default;
    constexpr bool operator==(const Tag&) const noexcept = default;

private:
	static constexpr std::uint32_t tag_value_from_keyword(std::string_view keyword) {
		if (const auto* entry = lookup::keyword_to_entry_chd(keyword)) {
			return entry->tag_value;
		}
		throw std::invalid_argument("Unknown DICOM keyword");
	}
};

static_assert(sizeof(Tag) == 4, "Tag must remain 4 bytes");
static_assert(alignof(Tag) == alignof(std::uint32_t), "Tag alignment must match uint32_t");
static_assert(std::is_standard_layout_v<Tag>, "Tag should be standard-layout");
static_assert(std::is_trivially_copyable_v<Tag>, "Tag should be trivially copyable");
// static_assert(std::is_trivial_v<Tag>, "Tag should be trivial");

struct Uid {
	std::uint16_t index{uid_lookup::kInvalidUidIndex};

	constexpr Uid() noexcept = default;

	constexpr explicit Uid(std::string_view text) : index(uid_lookup::uid_index_from_text(text)) {
		if (!valid()) {
			throw std::invalid_argument("Unknown DICOM UID");
		}
	}

	static constexpr Uid from_index(std::uint16_t idx) noexcept {
		return Uid(idx, from_index_tag{});
	}

	static constexpr Uid lookup(std::string_view text) noexcept {
		return from_index(uid_lookup::uid_index_from_text(text));
	}

	static constexpr Uid from_value(std::string_view value) noexcept {
		return from_index(uid_lookup::uid_index_from_value(value));
	}

	static constexpr Uid from_keyword(std::string_view keyword) noexcept {
		return from_index(uid_lookup::uid_index_from_keyword(keyword));
	}

	[[nodiscard]] constexpr bool valid() const noexcept {
		return index != uid_lookup::kInvalidUidIndex;
	}

	[[nodiscard]] constexpr explicit operator bool() const noexcept { return valid(); }

	[[nodiscard]] constexpr std::uint16_t raw_index() const noexcept { return index; }

	[[nodiscard]] constexpr const UidEntry* entry() const noexcept {
		return uid_lookup::entry_from_index(index);
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

	constexpr auto operator<=>(const Uid&) const noexcept = default;
	constexpr bool operator==(const Uid&) const noexcept = default;

private:
	struct from_index_tag {};
	constexpr Uid(std::uint16_t idx, from_index_tag) noexcept : index(idx) {}
};

static_assert(sizeof(Uid) == sizeof(std::uint16_t), "Uid must remain a 16-bit wrapper");
static_assert(std::is_trivially_copyable_v<Uid>, "Uid should be trivially copyable");

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
    constexpr explicit VR(uint16_t v) noexcept : value(v ? v : NONE_val) {}

    /// Construct from two VR characters, e.g. VR('P','N')
    constexpr VR(char a, char b) noexcept {
        const uint16_t raw = pack2(a,b);
        const uint16_t val = raw_to_val(raw);
        value = val ? val : (raw ? raw : NONE_val);
    }

    /// Construct from string_view (uses first two chars)
    constexpr explicit VR(std::string_view s) noexcept {
        if (s.size() >= 2) {
            const uint16_t raw = pack2(s[0], s[1]);
            const uint16_t val = raw_to_val(raw);
            value = val ? val : (raw ? raw : NONE_val);
        } else {
            value = NONE_val;
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
    constexpr bool     is_known() const noexcept { return value >= AE_val && value < _UNKNOWN_val; }
    constexpr uint16_t val()      const noexcept { return is_known() ? value : 0; }
    constexpr uint16_t raw_code() const noexcept { return is_known() ? val_to_raw[value] : value; }

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
    // Explicit VR encoding: 32-bit VL usage
    // ------------------------------------------------------------
    /// Returns true if this VR uses 32-bit VL field in explicit encoding
    // Table 7.1-1. Data Element with Explicit VR other than as shown in Table 7.1-2
    //      -> use 32-bit VL field
    // Table 7.1-2. Data Element with Explicit VR of AE, AS, AT, CS, DA, DS, DT,
    // FL, FD, IS, LO, LT, PN, SH, SL, SS, ST, TM, UI, UL and US
    //      -> use 16-bit VL field
    constexpr bool uses_explicit_32bit_vl() const noexcept {
        if (!is_known()) return false;
        switch (value) {
            case AE_val: case AS_val: case AT_val: case CS_val:
            case DA_val: case DS_val: case DT_val: case FL_val:
            case FD_val: case IS_val: case LO_val: case LT_val:
            case PN_val: case SH_val: case SL_val: case SS_val:
            case ST_val: case TM_val: case UI_val: case UL_val:
            case US_val:
                return false;
            default: return true;
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
        NONE_val = 0,
        AE_val=1, AS_val, AT_val, CS_val, DA_val, DS_val, DT_val, FD_val,
        FL_val, IS_val, LO_val, LT_val, OB_val, OD_val, OF_val, OV_val,
        OL_val, OW_val, PN_val, SH_val, SL_val, SQ_val, SS_val, ST_val,
        SV_val, TM_val, UC_val, UI_val, UL_val, UN_val, UR_val, US_val,
        UT_val, UV_val, PX_val, _UNKNOWN_val
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
    static const VR NONE;

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
inline constexpr VR VR::NONE{uint16_t(VR::NONE_val)};

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
	if (const auto* entry = tag_to_entry(tag_value)) {
		return entry->keyword;
	}
	return {};
}

constexpr std::uint16_t tag_to_vr(std::uint32_t tag_value) {
	if (const auto* entry = tag_to_entry(tag_value)) {
		return entry->vr_value;
	}
	return 0;
}

} // namespace lookup

namespace literals {

inline constexpr Tag operator"" _tag(const char* text, std::size_t len) {
	return Tag(std::string_view{text, len});
}

inline constexpr Uid operator"" _uid(const char* text, std::size_t len) {
	return Uid(std::string_view{text, len});
}

} // namespace literals

class Sequence {};
class PixelSequence {};

class DataSet;
struct ReadOptions {
	Tag load_until{Tag(0xFFFFu, 0xFFFFu)};
	bool keep_on_error{false};
	bool copy{true};
};

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

	constexpr DataElement(Tag tag, VR vr, std::size_t length, std::size_t offset,
 	    DataSet* parent = nullptr) noexcept
	    : tag_(tag), vr_(vr), length_(length), offset_(offset), storage_(), parent_(parent) {}

	[[nodiscard]] constexpr Tag tag() const noexcept { return tag_; }
	[[nodiscard]] constexpr VR vr() const noexcept { return vr_; }
	[[nodiscard]] constexpr std::size_t length() const noexcept { return length_; }
	[[nodiscard]] constexpr std::size_t offset() const noexcept { return offset_; }
	[[nodiscard]] constexpr DataSet* parent() const noexcept { return parent_; }
	[[nodiscard]] std::span<const std::uint8_t> value_span() const;
	void* value_ptr() const;
	[[nodiscard]] int vm() const;
	[[nodiscard]] constexpr void* data() const noexcept { return storage_.ptr; }
	[[nodiscard]] constexpr Sequence* sequence() const noexcept {
		return vr_.is_sequence() ? storage_.seq : nullptr;
	}
	[[nodiscard]] constexpr PixelSequence* pixel_sequence() const noexcept {
		return vr_.is_pixel_sequence() ? storage_.pixseq : nullptr;
	}

	constexpr void set_tag(Tag tag) noexcept { tag_ = tag; }
	constexpr void set_vr(VR vr) noexcept { vr_ = vr; }
	constexpr void set_length(std::size_t length) noexcept { length_ = length; }
	constexpr void set_offset(std::size_t offset) noexcept { offset_ = offset; }
	constexpr void set_parent(DataSet* parent) noexcept { parent_ = parent; }

	constexpr void set_data(void* ptr) noexcept { storage_.ptr = ptr; }
	constexpr void set_sequence(Sequence* seq) noexcept { storage_.seq = seq; }
	constexpr void set_pixel_sequence(PixelSequence* pixseq) noexcept {
		storage_.pixseq = pixseq;
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

	void release_storage() noexcept {
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
				if (vec_it_->vr() == VR::NONE) {
					++vec_it_;
					continue;
				}
				use_vector_ = true;
				return;
			}

			if (!map_done) {
				if (map_it_->second.vr() == VR::NONE) {
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


class DataSet {
public:
	using iterator = DataElementIterator<std::vector<DataElement>::iterator,
	    std::map<std::uint32_t, DataElement>::iterator, DataElement&, DataElement*>;
	using const_iterator = DataElementIterator<std::vector<DataElement>::const_iterator,
	    std::map<std::uint32_t, DataElement>::const_iterator, const DataElement&, const DataElement*>;

	DataSet();
	explicit DataSet(DataSet* root_dataset);
	~DataSet();
	DataSet(const DataSet&) = delete;
	DataSet& operator=(const DataSet&) = delete;
	DataSet(DataSet&&) noexcept = default;
	DataSet& operator=(DataSet&&) noexcept = default;

	void attach_to_file(const std::string& path);
	void attach_to_memory(const std::uint8_t* data, std::size_t size, bool copy = true);
	void attach_to_memory(const std::string& name, const std::uint8_t* data,
	    std::size_t size, bool copy = true);
	void attach_to_memory(std::string name, std::vector<std::uint8_t>&& buffer);
	// NOTE: DataSet keeps file/memory mappings alive for as long as the instance exists.
	// Make sure all DataSet objects that reference a path go out of scope (or are reset)
	// before you delete or overwrite the underlying file, otherwise Windows will refuse
	// to remove it while the mapping handle is still open.

	const std::string& path() const;
	InStream& stream();
	const InStream& stream() const;
	DataElement* add_dataelement(Tag tag, VR vr=VR::NONE, std::size_t length=0, std::size_t offset=0);
	void remove_dataelement(Tag tag);
	DataElement* get_dataelement(Tag tag);
	const DataElement* get_dataelement(Tag tag) const;
	void dump_elements() const;
	void read_attached_stream(const ReadOptions& options);
	void read_elements_until(Tag load_until, InStream* stream);
	iterator begin();
	iterator end();
	const_iterator begin() const;
	const_iterator end() const;
	const_iterator cbegin() const;
	const_iterator cend() const;

private:
	void reset_stream(std::string identifier, std::unique_ptr<InStream> stream);
	std::string path_;
	std::unique_ptr<InStream> stream_;
	DataSet* root_dataset_{this};
	Tag last_tag_loaded_{Tag::from_value(0)};
	std::vector<DataElement> elements_;
	std::map<std::uint32_t, DataElement> element_map_;
};

std::unique_ptr<DataSet> read_file(const std::string& path, ReadOptions options = {});
std::unique_ptr<DataSet> read_bytes(const std::uint8_t* data, std::size_t size,
    ReadOptions options = {});
std::unique_ptr<DataSet> read_bytes(const std::string& name, const std::uint8_t* data,
    std::size_t size, ReadOptions options = {});
std::unique_ptr<DataSet> read_bytes(std::string name, std::vector<std::uint8_t>&& buffer,
    ReadOptions options = {});

} // namespace dicom
