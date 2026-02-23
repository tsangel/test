#include "dataset_endian_converter.h"

#include <dicom.h>
#include <diagnostics.h>

#include <algorithm>
#include <cstring>
#include <fmt/format.h>

namespace diag = dicom::diag;

namespace dicom {
using namespace dicom::literals;

namespace {

constexpr std::uint16_t kGroupItem = 0xFFFEu;
constexpr std::uint16_t kElemItem = 0xE000u;
constexpr std::uint16_t kElemItemDelim = 0xE00Du;
constexpr std::uint16_t kElemSequenceDelim = 0xE0DDu;
constexpr std::uint16_t kPixelDataGroup = 0x7FE0u;
constexpr std::uint16_t kPixelDataElement = 0x0010u;

[[noreturn]] void throw_normalizer_error(const std::string& file_path, std::size_t offset,
    std::string_view reason) {
	diag::error_and_throw(
	    fmt::format(
	        "DataSet::read_attached_stream file={} offset=0x{:X} reason={}",
	        file_path, offset, reason));
}

[[nodiscard]] inline std::uint16_t load_u16_le(const std::uint8_t* ptr) noexcept {
	return static_cast<std::uint16_t>((static_cast<std::uint16_t>(ptr[1]) << 8) |
	                                  static_cast<std::uint16_t>(ptr[0]));
}

[[nodiscard]] inline std::uint16_t load_u16_be(const std::uint8_t* ptr) noexcept {
	return static_cast<std::uint16_t>((static_cast<std::uint16_t>(ptr[0]) << 8) |
	                                  static_cast<std::uint16_t>(ptr[1]));
}

[[nodiscard]] inline std::uint32_t load_u32_le(const std::uint8_t* ptr) noexcept {
	return static_cast<std::uint32_t>(ptr[0]) |
	       (static_cast<std::uint32_t>(ptr[1]) << 8) |
	       (static_cast<std::uint32_t>(ptr[2]) << 16) |
	       (static_cast<std::uint32_t>(ptr[3]) << 24);
}

[[nodiscard]] inline std::uint32_t load_u32_be(const std::uint8_t* ptr) noexcept {
	return (static_cast<std::uint32_t>(ptr[0]) << 24) |
	       (static_cast<std::uint32_t>(ptr[1]) << 16) |
	       (static_cast<std::uint32_t>(ptr[2]) << 8) |
	       static_cast<std::uint32_t>(ptr[3]);
}

inline void store_u16_be(std::uint8_t* ptr, std::uint16_t value) noexcept {
	ptr[0] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
	ptr[1] = static_cast<std::uint8_t>(value & 0xFFu);
}

inline void store_u16_le(std::uint8_t* ptr, std::uint16_t value) noexcept {
	ptr[0] = static_cast<std::uint8_t>(value & 0xFFu);
	ptr[1] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
}

inline void store_u32_be(std::uint8_t* ptr, std::uint32_t value) noexcept {
	ptr[0] = static_cast<std::uint8_t>((value >> 24) & 0xFFu);
	ptr[1] = static_cast<std::uint8_t>((value >> 16) & 0xFFu);
	ptr[2] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
	ptr[3] = static_cast<std::uint8_t>(value & 0xFFu);
}

inline void store_u32_le(std::uint8_t* ptr, std::uint32_t value) noexcept {
	ptr[0] = static_cast<std::uint8_t>(value & 0xFFu);
	ptr[1] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
	ptr[2] = static_cast<std::uint8_t>((value >> 16) & 0xFFu);
	ptr[3] = static_cast<std::uint8_t>((value >> 24) & 0xFFu);
}

[[nodiscard]] bool eight_bytes_are_zero(std::span<const std::uint8_t> bytes,
    std::size_t offset) noexcept {
	if (offset + 8 > bytes.size()) {
		return false;
	}
	std::uint64_t v = 0;
	std::memcpy(&v, bytes.data() + offset, sizeof(v));
	return v == 0;
}

[[nodiscard]] std::size_t lane_width_for_vr(VR vr) noexcept {
	if (vr == VR::US || vr == VR::SS || vr == VR::OW || vr == VR::AT) {
		return 2;
	}
	if (vr == VR::UL || vr == VR::SL || vr == VR::FL || vr == VR::OL || vr == VR::OF) {
		return 4;
	}
	if (vr == VR::FD || vr == VR::SV || vr == VR::UV || vr == VR::OV || vr == VR::OD) {
		return 8;
	}
	return 1;
}

void swap_lane_bytes(std::span<const std::uint8_t> src, std::span<std::uint8_t> dst,
    std::size_t lane_width, const std::string& file_path, std::size_t offset) {
	if (src.size() != dst.size()) {
		throw_normalizer_error(file_path, offset, "internal lane swap size mismatch");
	}
	if (lane_width <= 1) {
		if (src.data() != dst.data()) {
			std::memcpy(dst.data(), src.data(), src.size());
		}
		return;
	}
	if ((src.size() % lane_width) != 0) {
		throw_normalizer_error(file_path, offset,
		    "value length is not aligned to element byte width for dataset endian conversion");
	}
	for (std::size_t i = 0; i < src.size(); i += lane_width) {
		for (std::size_t b = 0; b < lane_width; ++b) {
			dst[i + b] = src[i + lane_width - 1 - b];
		}
	}
}

class dataset_endian_converter {
public:
	enum class direction : std::uint8_t {
		be_to_le = 0,
		le_to_be = 1,
	};

	dataset_endian_converter(std::span<const std::uint8_t> src,
	    std::span<std::uint8_t> dst, const std::string& file_path,
	    direction conversion_direction) noexcept
	    : src_(src), dst_(dst), file_path_(file_path), direction_(conversion_direction) {}

	void convert(std::size_t offset, std::size_t end) {
		auto pos = offset;
		normalize_dataset_elements(pos, end, false);
	}

private:
	enum class stop_reason {
		reached_end,
		item_delim,
		sequence_delim,
	};

	struct element_header {
		std::uint16_t group{0};
		std::uint16_t element{0};
		VR vr{VR::None};
		bool known_vr{false};
		bool explicit_16bit_vl{true};
		std::size_t header_size{8};
		std::uint32_t length{0};
	};

	void ensure_available(std::size_t offset, std::size_t bytes) const {
		if (offset > src_.size() || bytes > src_.size() - offset) {
			throw_normalizer_error(file_path_, offset,
			    "unexpected end of input while converting dataset endianness");
		}
	}

	[[nodiscard]] std::uint16_t load_u16(const std::uint8_t* ptr) const noexcept {
		if (direction_ == direction::be_to_le) {
			return load_u16_be(ptr);
		}
		return load_u16_le(ptr);
	}

	[[nodiscard]] std::uint32_t load_u32(const std::uint8_t* ptr) const noexcept {
		if (direction_ == direction::be_to_le) {
			return load_u32_be(ptr);
		}
		return load_u32_le(ptr);
	}

	void store_u16(std::uint8_t* ptr, std::uint16_t value) const noexcept {
		if (direction_ == direction::be_to_le) {
			store_u16_le(ptr, value);
			return;
		}
		store_u16_be(ptr, value);
	}

	void store_u32(std::uint8_t* ptr, std::uint32_t value) const noexcept {
		if (direction_ == direction::be_to_le) {
			store_u32_le(ptr, value);
			return;
		}
		store_u32_be(ptr, value);
	}

	[[nodiscard]] element_header parse_and_convert_element_header(std::size_t offset,
	    std::size_t end) {
		ensure_available(offset, 8);

		element_header header{};
		header.group = load_u16(src_.data() + offset);
		header.element = load_u16(src_.data() + offset + 2);

		store_u16(dst_.data() + offset, header.group);
		store_u16(dst_.data() + offset + 2, header.element);

		if (header.group == kGroupItem) {
			header.known_vr = false;
			header.header_size = 8;
			header.length = load_u32(src_.data() + offset + 4);
			store_u32(dst_.data() + offset + 4, header.length);
			return header;
		}

		header.vr = VR(
		    static_cast<char>(src_[offset + 4]),
		    static_cast<char>(src_[offset + 5]));
		header.known_vr = header.vr.is_known();
		if (!header.known_vr) {
			// Unknown VR bytes: keep parser-compatible behavior (8-byte header + 32-bit VL).
			header.header_size = 8;
			header.length = load_u32(src_.data() + offset + 4);
			store_u32(dst_.data() + offset + 4, header.length);
			return header;
		}

		header.explicit_16bit_vl = header.vr.uses_explicit_16bit_vl();
		if (header.explicit_16bit_vl) {
			header.header_size = 8;
			header.length = load_u16(src_.data() + offset + 6);
			store_u16(dst_.data() + offset + 6, static_cast<std::uint16_t>(header.length));
			return header;
		}

		// Explicit VR with reserved + 32-bit VL (normal form).
		ensure_available(offset, 12);
		const auto long_length = load_u32(src_.data() + offset + 8);
		const auto remaining_after_12 = end - (offset + 12);
		if (long_length != 0xFFFFFFFFu && long_length > remaining_after_12) {
			// Salvage malformed streams that used 16-bit VL despite long-form VR.
			header.header_size = 8;
			header.length = load_u16(src_.data() + offset + 6);
			store_u16(dst_.data() + offset + 6, static_cast<std::uint16_t>(header.length));
			return header;
		}

		header.header_size = 12;
		header.length = long_length;
		store_u32(dst_.data() + offset + 8, header.length);
		return header;
	}

	stop_reason normalize_dataset_elements(std::size_t& pos, std::size_t end,
	    bool stop_on_item_delim) {
		while (pos < end) {
			if (end - pos < 8 || eight_bytes_are_zero(src_, pos)) {
				pos = end;
				return stop_reason::reached_end;
			}

			const auto header = parse_and_convert_element_header(pos, end);
			const auto element_start = pos;
			pos += header.header_size;

			if (header.group == kGroupItem) {
				if (header.element == kElemItemDelim) {
					if (stop_on_item_delim) {
						return stop_reason::item_delim;
					}
					continue;
				}
				if (header.element == kElemSequenceDelim) {
					if (stop_on_item_delim) {
						return stop_reason::sequence_delim;
					}
					continue;
				}
				if (header.element != kElemItem) {
					throw_normalizer_error(file_path_, element_start,
					    "unexpected (FFFE,xxxx) item tag while normalizing dataset");
				}
				normalize_sequence_item_payload(pos, end, header.length);
				continue;
			}

			if (header.length == 0xFFFFFFFFu) {
				if (header.known_vr && header.vr == VR::SQ) {
					normalize_sequence_value_undefined(pos, end);
					continue;
				}
				if (header.group == kPixelDataGroup && header.element == kPixelDataElement) {
					normalize_pixel_sequence_undefined(pos, end);
					continue;
				}
				// For non-conforming undefined-length non-SQ values, fall back to
				// item-stream normalization (same as encapsulated containers).
				normalize_pixel_sequence_undefined(pos, end);
				continue;
			}

			if (header.length > end - pos) {
				throw_normalizer_error(file_path_, element_start,
				    "value length exceeds remaining bytes during dataset endian conversion");
			}

			const auto value_offset = pos;
			const auto value_end = pos + header.length;

			if (header.known_vr && header.vr == VR::SQ) {
				normalize_sequence_value_defined(pos, value_end);
				pos = value_end;
				continue;
			}

			const std::size_t lane_width = lane_width_for_vr(header.vr);
			swap_lane_bytes(src_.subspan(value_offset, header.length),
			    dst_.subspan(value_offset, header.length), lane_width, file_path_, value_offset);
			pos = value_end;
		}
		return stop_reason::reached_end;
	}

	void normalize_sequence_item_payload(std::size_t& pos, std::size_t end,
	    std::uint32_t item_length) {
		if (item_length == 0xFFFFFFFFu) {
			const auto stop = normalize_dataset_elements(pos, end, true);
			if (stop == stop_reason::sequence_delim) {
				// Missing Item Delimitation; tolerate and let parent consume the sequence end.
				return;
			}
			return;
		}

		if (item_length > end - pos) {
			throw_normalizer_error(file_path_, pos,
			    "item length exceeds remaining bytes during dataset endian conversion");
		}

		const auto item_end = pos + static_cast<std::size_t>(item_length);
		auto item_pos = pos;
		(void)normalize_dataset_elements(item_pos, item_end, false);
		pos = item_end;
	}

	void normalize_sequence_value_defined(std::size_t& pos, std::size_t value_end) {
		while (pos < value_end) {
			if (value_end - pos < 8) {
				pos = value_end;
				return;
			}
			const auto header = parse_and_convert_element_header(pos, value_end);
			if (header.group != kGroupItem) {
				throw_normalizer_error(file_path_, pos,
				    "SQ value contains a non-item tag during dataset endian conversion");
			}

			pos += header.header_size;
			if (header.element == kElemSequenceDelim || header.element == kElemItemDelim) {
				continue;
			}
			if (header.element != kElemItem) {
				throw_normalizer_error(file_path_, pos - header.header_size,
				    "SQ value contains unknown item marker during dataset endian conversion");
			}
			normalize_sequence_item_payload(pos, value_end, header.length);
		}
	}

	void normalize_sequence_value_undefined(std::size_t& pos, std::size_t end) {
		while (pos < end) {
			ensure_available(pos, 8);
			const auto header = parse_and_convert_element_header(pos, end);
			if (header.group != kGroupItem) {
				throw_normalizer_error(file_path_, pos,
				    "undefined-length SQ contains a non-item tag during dataset endian conversion");
			}

			pos += header.header_size;
			if (header.element == kElemSequenceDelim) {
				return;
			}
			if (header.element == kElemItemDelim) {
				continue;
			}
			if (header.element != kElemItem) {
				throw_normalizer_error(file_path_, pos - header.header_size,
				    "undefined-length SQ contains unknown marker during dataset endian conversion");
			}
			normalize_sequence_item_payload(pos, end, header.length);
		}
		throw_normalizer_error(file_path_, end,
		    "missing sequence delimitation item in undefined-length SQ during dataset endian conversion");
	}

	void normalize_pixel_sequence_undefined(std::size_t& pos, std::size_t end) {
		while (pos < end) {
			ensure_available(pos, 8);
			const auto header = parse_and_convert_element_header(pos, end);
			if (header.group != kGroupItem) {
				throw_normalizer_error(file_path_, pos,
				    "encapsulated PixelData contains a non-item tag during dataset endian conversion");
			}

			pos += header.header_size;
			if (header.element == kElemSequenceDelim) {
				return;
			}
			if (header.element != kElemItem) {
				throw_normalizer_error(file_path_, pos - header.header_size,
				    "encapsulated PixelData contains unknown marker during dataset endian conversion");
			}
			if (header.length == 0xFFFFFFFFu) {
				throw_normalizer_error(file_path_, pos - header.header_size,
				    "encapsulated PixelData item has undefined length");
			}
			if (header.length > end - pos) {
				throw_normalizer_error(file_path_, pos,
				    "encapsulated PixelData item exceeds remaining bytes during dataset endian conversion");
			}
			pos += static_cast<std::size_t>(header.length);
		}
		throw_normalizer_error(file_path_, end,
		    "missing sequence delimitation item in encapsulated PixelData during dataset endian conversion");
	}

	std::span<const std::uint8_t> src_;
	std::span<std::uint8_t> dst_;
	const std::string& file_path_;
	direction direction_{direction::be_to_le};
};

} // namespace

std::vector<std::uint8_t> normalize_big_endian_dataset(
    std::span<const std::uint8_t> full_input, std::size_t dataset_start_offset,
    const std::string& file_path) {
	if (dataset_start_offset > full_input.size()) {
		throw_normalizer_error(file_path, dataset_start_offset,
		    "invalid big-endian dataset start offset");
	}

	std::vector<std::uint8_t> output(full_input.begin(), full_input.end());
	if (dataset_start_offset == full_input.size()) {
		return output;
	}

	dataset_endian_converter normalizer(full_input, std::span<std::uint8_t>(output),
	    file_path, dataset_endian_converter::direction::be_to_le);
	normalizer.convert(dataset_start_offset, full_input.size());
	return output;
}

std::vector<std::uint8_t> convert_little_endian_dataset_to_big_endian(
    std::span<const std::uint8_t> full_input, std::size_t dataset_start_offset,
    const std::string& file_path) {
	if (dataset_start_offset > full_input.size()) {
		throw_normalizer_error(file_path, dataset_start_offset,
		    "invalid little-endian dataset start offset");
	}

	std::vector<std::uint8_t> output(full_input.begin(), full_input.end());
	if (dataset_start_offset == full_input.size()) {
		return output;
	}

	dataset_endian_converter converter(full_input, std::span<std::uint8_t>(output),
	    file_path, dataset_endian_converter::direction::le_to_be);
	converter.convert(dataset_start_offset, full_input.size());
	return output;
}

} // namespace dicom
