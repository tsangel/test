#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>

namespace dicom {

using std::uint8_t;
using std::uint16_t;

struct Tag {
	std::uint32_t packed{0};

	constexpr Tag() = default;
	constexpr Tag(std::uint16_t group, std::uint16_t element)
	    : packed((static_cast<std::uint32_t>(group) << 16) | static_cast<std::uint32_t>(element)) {}
	constexpr explicit Tag(std::uint32_t value) : packed(value) {}

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
};

static_assert(sizeof(Tag) == 4, "Tag must remain 4 bytes");
static_assert(alignof(Tag) == alignof(std::uint32_t), "Tag alignment must match uint32_t");
static_assert(std::is_standard_layout_v<Tag>, "Tag should be standard-layout");
static_assert(std::is_trivially_copyable_v<Tag>, "Tag should be trivially copyable");
// static_assert(std::is_trivial_v<Tag>, "Tag should be trivial");

/// Packs two ASCII characters into a 16-bit value.
/// Example: 'A','E' -> 0x4145
constexpr uint16_t pack2(char a, char b) noexcept {
    return (uint16_t(uint8_t(a)) << 8) | uint16_t(uint8_t(b));
}

/// Represents a DICOM Value Representation (VR).
/// Internally stores a compact encoded form:
///  - known VR → small integer 1..32
///  - unknown VR → 16-bit raw two-character code ('X''Y')
struct VR {
    uint16_t value = 0;  ///< compact encoded value (1..32 or raw 16-bit code)

    // ------------------------------------------------------------
    // Constructors
    // ------------------------------------------------------------
    constexpr VR() noexcept = default;
    constexpr explicit VR(uint16_t v) noexcept : value(v) {}

    /// Construct from two VR characters, e.g. VR('P','N')
    constexpr VR(char a, char b) noexcept {
        const uint16_t raw = pack2(a,b);
        const uint16_t val = raw_to_val(raw);
        value = val ? val : raw;
    }

    /// Construct from string_view (uses first two chars)
    constexpr explicit VR(std::string_view s) noexcept {
        if (s.size() >= 2) {
            const uint16_t raw = pack2(s[0], s[1]);
            const uint16_t val = raw_to_val(raw);
            value = val ? val : raw;
        } else value = 0;
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
    constexpr bool     is_known() const noexcept { return value >= 1 && value <= 32; }
    constexpr uint16_t val()      const noexcept { return is_known() ? value : 0; }
    constexpr uint16_t raw_code() const noexcept { return is_known() ? val_to_raw[value] : value; }
    constexpr char     first()    const noexcept { return char(raw_code() >> 8); }
    constexpr char     second()   const noexcept { return char(raw_code() & 0xFF); }

    /// Returns the two-character VR string or "??" for unknown
    constexpr std::string_view str() const noexcept {
        if (!is_known()) return "??";
        switch (val_to_raw[value]) {
            case pack2('A','E'): return "AE"; case pack2('A','S'): return "AS";
            case pack2('A','T'): return "AT"; case pack2('C','S'): return "CS";
            case pack2('D','A'): return "DA"; case pack2('D','S'): return "DS";
            case pack2('D','T'): return "DT"; case pack2('F','D'): return "FD";
            case pack2('F','L'): return "FL"; case pack2('I','S'): return "IS";
            case pack2('L','O'): return "LO"; case pack2('L','T'): return "LT";
            case pack2('O','B'): return "OB"; case pack2('O','D'): return "OD";
            case pack2('O','F'): return "OF"; case pack2('O','L'): return "OL";
            case pack2('O','W'): return "OW"; case pack2('P','N'): return "PN";
            case pack2('S','H'): return "SH"; case pack2('S','L'): return "SL";
            case pack2('S','Q'): return "SQ"; case pack2('S','S'): return "SS";
            case pack2('S','T'): return "ST"; case pack2('T','M'): return "TM";
            case pack2('U','C'): return "UC"; case pack2('U','I'): return "UI";
            case pack2('U','L'): return "UL"; case pack2('U','N'): return "UN";
            case pack2('U','R'): return "UR"; case pack2('U','S'): return "US";
            case pack2('U','T'): return "UT";
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
            case OB_val: case OD_val: case OF_val: case OL_val: case OW_val:
            case UN_val: case US_val: case SS_val:
            case UL_val: case SL_val: case FL_val: case FD_val:
                return true;
            default: return false;
        }
    }

    constexpr bool is_sequence() const noexcept {
        return is_known() && value == SQ_val;
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
            case OL_val: case OW_val: case UN_val: case SQ_val:
                return 0x00;
            default: return 0x20;
        }
    }

    // ------------------------------------------------------------
    // Explicit VR encoding: 32-bit VL usage
    // ------------------------------------------------------------
    /// Returns true if this VR uses 32-bit VL field in explicit encoding
    constexpr bool uses_explicit_32bit_vl() const noexcept {
        if (!is_known()) return false;
        switch (value) {
            case OB_val: case OD_val: case OF_val:
            case OL_val: case OW_val: case SQ_val:
            case UC_val: case UR_val: case UN_val: case UT_val:
                return true;
            default: return false;
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
            case FD_val: return 8;
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
        AE_val=1, AS_val, AT_val, CS_val, DA_val, DS_val, DT_val, FD_val,
        FL_val, IS_val, LO_val, LT_val, OB_val, OD_val, OF_val, OL_val,
        OW_val, PN_val, SH_val, SL_val, SQ_val, SS_val, ST_val, TM_val,
        UC_val, UI_val, UL_val, UN_val, UR_val, US_val, UT_val, _UNKNOWN_val
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
    static const VR OL;
    static const VR OW;
    static const VR PN;
    static const VR SH;
    static const VR SL;
    static const VR SQ;
    static const VR SS;
    static const VR ST;
    static const VR TM;
    static const VR UC;
    static const VR UI;
    static const VR UL;
    static const VR UN;
    static const VR UR;
    static const VR US;
    static const VR UT;

private:
    // Mapping table from compact ID -> raw 2-char code
    inline static constexpr std::array<uint16_t, 33> val_to_raw = {
        0,
        pack2('A','E'), pack2('A','S'), pack2('A','T'), pack2('C','S'),
        pack2('D','A'), pack2('D','S'), pack2('D','T'), pack2('F','D'),
        pack2('F','L'), pack2('I','S'), pack2('L','O'), pack2('L','T'),
        pack2('O','B'), pack2('O','D'), pack2('O','F'), pack2('O','L'),
        pack2('O','W'), pack2('P','N'), pack2('S','H'), pack2('S','L'),
        pack2('S','Q'), pack2('S','S'), pack2('S','T'), pack2('T','M'),
        pack2('U','C'), pack2('U','I'), pack2('U','L'), pack2('U','N'),
        pack2('U','R'), pack2('U','S'), pack2('U','T'), 0
    };

    /// Maps raw 16-bit code -> small integer (1..32) or 0 if unknown.
    static constexpr uint16_t raw_to_val(uint16_t raw) noexcept {
        switch (raw) {
            case pack2('A','E'): return AE_val; case pack2('A','S'): return AS_val;
            case pack2('A','T'): return AT_val; case pack2('C','S'): return CS_val;
            case pack2('D','A'): return DA_val; case pack2('D','S'): return DS_val;
            case pack2('D','T'): return DT_val; case pack2('F','D'): return FD_val;
            case pack2('F','L'): return FL_val; case pack2('I','S'): return IS_val;
            case pack2('L','O'): return LO_val; case pack2('L','T'): return LT_val;
            case pack2('O','B'): return OB_val; case pack2('O','D'): return OD_val;
            case pack2('O','F'): return OF_val; case pack2('O','L'): return OL_val;
            case pack2('O','W'): return OW_val; case pack2('P','N'): return PN_val;
            case pack2('S','H'): return SH_val; case pack2('S','L'): return SL_val;
            case pack2('S','Q'): return SQ_val; case pack2('S','S'): return SS_val;
            case pack2('S','T'): return ST_val; case pack2('T','M'): return TM_val;
            case pack2('U','C'): return UC_val; case pack2('U','I'): return UI_val;
            case pack2('U','L'): return UL_val; case pack2('U','N'): return UN_val;
            case pack2('U','R'): return UR_val; case pack2('U','S'): return US_val;
            case pack2('U','T'): return UT_val;
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
inline constexpr VR VR::OL{uint16_t(VR::OL_val)};
inline constexpr VR VR::OW{uint16_t(VR::OW_val)};
inline constexpr VR VR::PN{uint16_t(VR::PN_val)};
inline constexpr VR VR::SH{uint16_t(VR::SH_val)};
inline constexpr VR VR::SL{uint16_t(VR::SL_val)};
inline constexpr VR VR::SQ{uint16_t(VR::SQ_val)};
inline constexpr VR VR::SS{uint16_t(VR::SS_val)};
inline constexpr VR VR::ST{uint16_t(VR::ST_val)};
inline constexpr VR VR::TM{uint16_t(VR::TM_val)};
inline constexpr VR VR::UC{uint16_t(VR::UC_val)};
inline constexpr VR VR::UI{uint16_t(VR::UI_val)};
inline constexpr VR VR::UL{uint16_t(VR::UL_val)};
inline constexpr VR VR::UN{uint16_t(VR::UN_val)};
inline constexpr VR VR::UR{uint16_t(VR::UR_val)};
inline constexpr VR VR::US{uint16_t(VR::US_val)};
inline constexpr VR VR::UT{uint16_t(VR::UT_val)};


class DicomFile {
public:
	explicit DicomFile(const std::string& path);
	DicomFile(const DicomFile&) = delete;
	DicomFile& operator=(const DicomFile&) = delete;
	DicomFile(DicomFile&&) noexcept = default;
	DicomFile& operator=(DicomFile&&) noexcept = default;

	static std::unique_ptr<DicomFile> attach(const std::string& path);
	const std::string& path() const;

private:
	std::string path_;
};

} // namespace dicom
