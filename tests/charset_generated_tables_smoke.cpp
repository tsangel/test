#include <bit>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>

#include "charset/generated/gb18030_tables.hpp"
#include "charset/generated/jisx0208_tables.hpp"
#include "charset/generated/jisx0212_tables.hpp"
#include "charset/generated/ksx1001_tables.hpp"
#include "charset/generated/sbcs_to_unicode_selected.hpp"

namespace {

[[noreturn]] void fail(const std::string& msg) {
	std::cerr << msg << std::endl;
	std::exit(1);
}

template <std::size_t N, std::size_t M>
std::optional<std::uint16_t> unicode_to_dense_multibyte(std::uint32_t codepoint,
    const std::uint16_t (&unicode_to_index)[N], const std::uint16_t (&index_to_multibyte)[M],
    std::uint16_t or_mask = 0) {
	if (codepoint > 0xFFFFu) {
		return std::nullopt;
	}
	const auto unicode_value = static_cast<std::uint16_t>(codepoint);
	const auto pageindex = unicode_to_index[unicode_value >> 8u];
	if (pageindex == 0u) {
		return std::nullopt;
	}

	auto codeindex = static_cast<std::size_t>(pageindex + ((unicode_value & 0xF0u) >> 3u));
	auto usebit = unicode_to_index[codeindex + 1u];
	codeindex = unicode_to_index[codeindex];
	usebit >>= (15u - (unicode_value & 0x0Fu));
	if ((usebit & 1u) == 0u) {
		return std::nullopt;
	}

	usebit >>= 1u;
	codeindex += std::popcount(static_cast<unsigned int>(usebit));
	return static_cast<std::uint16_t>(index_to_multibyte[codeindex] | or_mask);
}

template <std::size_t N>
std::optional<std::uint16_t> decode_row_offset_multibyte(
    const std::uint16_t (&lookup_table)[N], std::uint8_t b1, std::uint8_t b2,
    std::uint8_t first_base, std::uint8_t first_limit, std::uint8_t second_base,
    std::uint8_t second_limit) {
	if (b1 < first_base || b1 > first_limit || b2 < second_base || b2 > second_limit) {
		return std::nullopt;
	}
	const auto row_offset = lookup_table[b1 - first_base];
	const auto codepoint = lookup_table[row_offset + (b2 - second_base)];
	if (codepoint == 0u || codepoint == 0xFFFDu) {
		return std::nullopt;
	}
	return codepoint;
}

std::optional<std::uint16_t> decode_gbk_multibyte(std::uint8_t b1, std::uint8_t b2) {
	if (b1 < 0x81u || b1 > 0xFEu || b2 < 0x40u || b2 > 0xFEu || b2 == 0x7Fu) {
		return std::nullopt;
	}
	auto index = static_cast<std::size_t>((b1 - 0x81u) * 190u + (b2 - 0x40u));
	if (b2 > 0x7Fu) {
		--index;
	}
	const auto codepoint = dicom::charset::tables::map_gb18030_to_unicode[index];
	if (codepoint == 0xFFFDu) {
		return std::nullopt;
	}
	return codepoint;
}

bool gb18030_bmp_codepoint_to_unicode(std::uint32_t codepoint, std::uint32_t& unicode) noexcept {
	using namespace dicom::charset::tables;
	const auto range_count = sizeof(gb18030_codepoint_range) / sizeof(gb18030_codepoint_range[0]) / 2u;
	if (range_count == 0u || codepoint > gb18030_codepoint_range[range_count * 2u - 1u]) {
		return false;
	}

	std::size_t left = 0;
	std::size_t right = range_count;
	while (left < right) {
		const auto mid = (left + right) / 2u;
		const auto start = gb18030_codepoint_range[mid * 2u];
		const auto end = gb18030_codepoint_range[mid * 2u + 1u];
		if (codepoint < start) {
			right = mid;
			continue;
		}
		if (codepoint > end) {
			left = mid + 1u;
			continue;
		}
		unicode = gb18030_unicode_range[mid * 2u] + codepoint - start;
		return true;
	}
	return false;
}

bool unicode_to_gb18030_bmp_codepoint(std::uint32_t unicode, std::uint32_t& codepoint) noexcept {
	using namespace dicom::charset::tables;
	const auto range_count = sizeof(gb18030_unicode_range) / sizeof(gb18030_unicode_range[0]) / 2u;
	std::size_t left = 0;
	std::size_t right = range_count;
	while (left < right) {
		const auto mid = (left + right) / 2u;
		const auto start = gb18030_unicode_range[mid * 2u];
		const auto end = gb18030_unicode_range[mid * 2u + 1u];
		if (unicode < start) {
			right = mid;
			continue;
		}
		if (unicode > end) {
			left = mid + 1u;
			continue;
		}
		codepoint = gb18030_codepoint_range[mid * 2u] + unicode - start;
		return true;
	}
	return false;
}

}  // namespace

int main() {
	using namespace dicom::charset::tables;

	const auto gbk_multibyte =
	    unicode_to_dense_multibyte(0x4E02u, map_gb18030_unicode_to_index, map_gb18030_index_to_multibyte);
	if (!gbk_multibyte || *gbk_multibyte != 0x8140u) {
		fail("GBK generated table should map U+4E02 to 0x8140");
	}
	const auto gbk_unicode = decode_gbk_multibyte(0x81u, 0x40u);
	if (!gbk_unicode || *gbk_unicode != 0x4E02u) {
		fail("GBK generated table should map 0x8140 to U+4E02");
	}

	const auto ksx_multibyte =
	    unicode_to_dense_multibyte(0xAC00u, map_ksx1001_unicode_to_index, map_ksx1001_index_to_multibyte, 0x8080u);
	if (!ksx_multibyte || *ksx_multibyte != 0xB0A1u) {
		fail("KS X 1001 generated table should map U+AC00 to 0xB0A1");
	}
	const auto ksx_unicode =
	    decode_row_offset_multibyte(map_ksx1001_multibyte_to_unicode, 0xB0u, 0xA1u, 0xA1u, 0xFEu, 0xA1u, 0xFEu);
	if (!ksx_unicode || *ksx_unicode != 0xAC00u) {
		fail("KS X 1001 generated table should map 0xB0A1 to U+AC00");
	}

	const auto jis0208_multibyte =
	    unicode_to_dense_multibyte(0x4E9Cu, map_jisx0208_unicode_to_index, map_jisx0208_index_to_multibyte);
	if (!jis0208_multibyte || *jis0208_multibyte != 0x3021u) {
		fail("JIS X 0208 generated table should map U+4E9C to 0x3021");
	}
	const auto jis0208_unicode =
	    decode_row_offset_multibyte(map_jisx0208_multibyte_to_unicode, 0x30u, 0x21u, 0x21u, 0x7Eu, 0x21u, 0x7Eu);
	if (!jis0208_unicode || *jis0208_unicode != 0x4E9Cu) {
		fail("JIS X 0208 generated table should map 0x3021 to U+4E9C");
	}

	const auto jis0212_multibyte =
	    unicode_to_dense_multibyte(0x4E02u, map_jisx0212_unicode_to_index, map_jisx0212_index_to_multibyte);
	if (!jis0212_multibyte || *jis0212_multibyte != 0x3021u) {
		fail("JIS X 0212 generated table should map U+4E02 to 0x3021");
	}
	const auto jis0212_unicode =
	    decode_row_offset_multibyte(map_jisx0212_multibyte_to_unicode, 0x30u, 0x21u, 0x21u, 0x7Eu, 0x21u, 0x7Eu);
	if (!jis0212_unicode || *jis0212_unicode != 0x4E02u) {
		fail("JIS X 0212 generated table should map 0x3021 to U+4E02");
	}

	std::uint32_t gb18030_codepoint = 0;
	if (!unicode_to_gb18030_bmp_codepoint(0x0080u, gb18030_codepoint) || gb18030_codepoint != 0u) {
		fail("GB18030 range table should map U+0080 to codepoint 0");
	}
	std::uint32_t gb18030_unicode = 0;
	if (!gb18030_bmp_codepoint_to_unicode(0u, gb18030_unicode) || gb18030_unicode != 0x0080u) {
		fail("GB18030 range table should map codepoint 0 to U+0080");
	}

	if (map_latin2_to_unicode_g1[0xA1u - 0x80u] != 0x0104u) {
		fail("ISO_IR 101 generated table should map 0xA1 to U+0104");
	}
	if (map_latin3_to_unicode_g1[0xA1u - 0x80u] != 0x0126u) {
		fail("ISO_IR 109 generated table should map 0xA1 to U+0126");
	}
	if (map_latin4_to_unicode_g1[0xC0u - 0x80u] != 0x0100u) {
		fail("ISO_IR 110 generated table should map 0xC0 to U+0100");
	}
	if (map_thai_to_unicode_g1[0xA1u - 0x80u] != 0x0E01u) {
		fail("ISO_IR 166 generated table should map 0xA1 to U+0E01");
	}
	if (map_latin9_to_unicode_g1[0xA4u - 0x80u] != 0x20ACu) {
		fail("ISO_IR 203 generated table should map 0xA4 to U+20AC");
	}

	return 0;
}
