#pragma once

#include "dicom.h"
#include "dicom_endian.h"
#include "diagnostics.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <ostream>
#include <span>
#include <string_view>
#include <vector>

namespace dicom::write_detail {
using namespace dicom::literals;

constexpr Tag kItemTag{0xFFFEu, 0xE000u};
constexpr Tag kItemDelimitationTag{0xFFFEu, 0xE00Du};
constexpr Tag kSequenceDelimitationTag{0xFFFEu, 0xE0DDu};

struct DatasetWritePlan {
	bool explicit_vr{true};
	bool convert_body_to_big_endian{false};
	bool deflate_body{false};
};


// Streams serialized bytes directly to an ostream-backed sink.
struct StreamWriter {
	explicit StreamWriter(std::ostream& out) : os(out) {}

	void append(const void* data, std::size_t size) {
		if (size == 0) {
			return;
		}
		os.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
		if (!os) {
			throw diag::DicomException(
			    "stage=ostream_write reason=failed to write output bytes");
		}
		written += size;
	}

	void append_byte(std::uint8_t value) {
		os.put(static_cast<char>(value));
		if (!os) {
			throw diag::DicomException(
			    "stage=ostream_write reason=failed to write output byte");
		}
		++written;
	}

	[[nodiscard]] std::size_t position() const noexcept { return written; }

	[[nodiscard]] bool can_overwrite() { return os.tellp() != std::streampos(-1); }

	void overwrite(std::size_t position, std::span<const std::uint8_t> bytes) {
		if (bytes.empty()) {
			return;
		}
		const auto resume = os.tellp();
		if (resume == std::streampos(-1)) {
			throw diag::DicomException(
			    "stage=ostream_backpatch reason=output stream is not seekable for backpatch");
		}
		os.seekp(static_cast<std::streamoff>(position), std::ios::beg);
		if (!os) {
			throw diag::DicomException(
			    "stage=ostream_backpatch reason=failed to seek for backpatch");
		}
		os.write(reinterpret_cast<const char*>(bytes.data()),
		    static_cast<std::streamsize>(bytes.size()));
		if (!os) {
			throw diag::DicomException(
			    "stage=ostream_backpatch reason=failed to backpatch output bytes");
		}
		os.seekp(resume, std::ios::beg);
		if (!os) {
			throw diag::DicomException(
			    "stage=ostream_backpatch reason=failed to restore stream position after backpatch");
		}
	}

	std::ostream& os;
	std::size_t written{0};
};

// Accumulates serialized bytes into an in-memory output buffer.
struct BufferWriter {
	explicit BufferWriter(std::vector<std::uint8_t>& out) : bytes(out) {}

	void append(const void* data, std::size_t size) {
		if (size == 0) {
			return;
		}
		std::memcpy(append_uninitialized(size), data, size);
	}

	void append_byte(std::uint8_t value) {
		bytes.push_back(value);
		++written;
	}

	void append_u16_le(std::uint16_t value) {
		endian::store_le<std::uint16_t>(append_uninitialized(sizeof(value)), value);
	}

	void append_u32_le(std::uint32_t value) {
		endian::store_le<std::uint32_t>(append_uninitialized(sizeof(value)), value);
	}

	void append_tag(Tag tag) {
		auto* out = append_uninitialized(4);
		endian::store_le<std::uint16_t>(out, tag.group());
		endian::store_le<std::uint16_t>(out + 2, tag.element());
	}

	[[nodiscard]] std::size_t position() const noexcept { return written; }

	[[nodiscard]] bool can_overwrite() const noexcept { return true; }

	void overwrite(std::size_t position, std::span<const std::uint8_t> data) {
		if (data.empty()) {
			return;
		}
		if (position > bytes.size() || data.size() > bytes.size() - position) {
			throw diag::DicomException(fmt::format(
			    "stage=buffer_backpatch reason=backpatch out of bounds position={} size={} buffer_size={}",
			    position, data.size(), bytes.size()));
		}
		std::memcpy(bytes.data() + position, data.data(), data.size());
	}

private:
	std::uint8_t* append_uninitialized(std::size_t size) {
		const auto old_size = bytes.size();
		bytes.resize(old_size + size);
		written += size;
		return bytes.data() + old_size;
	}

public:
	std::vector<std::uint8_t>& bytes;
	std::size_t written{0};
};

// Measures serialized sizes without producing any output bytes.
struct CountingWriter {
	void append(const void*, std::size_t size) { written += size; }
	void append_byte(std::uint8_t) { ++written; }
	std::size_t written{0};
};

enum class WriteEncoderConfigSource : std::uint8_t {
	use_plugin_defaults = 0,
	use_explicit_options,
	use_encoder_context,
};

enum class CheckedU32Label : std::uint8_t {
	element_length = 0,
	pixel_fragment_length,
	native_pixel_data_length,
	file_meta_group_length,
	basic_offset_table_length,
	extended_offset_table_length,
	extended_offset_table_lengths_length,
};

template <typename... Args>
[[noreturn]] inline void throw_write_stage_error(std::string_view stage,
    fmt::format_string<Args...> reason_format, Args&&... args) {
	throw diag::DicomException(fmt::format(
	    "stage={} reason={}", stage,
	    fmt::format(reason_format, std::forward<Args>(args)...)));
}

[[nodiscard]] inline bool write_exception_needs_boundary_prefix(
    std::string_view message) noexcept {
	return message.starts_with("stage=");
}

[[nodiscard]] inline bool write_exception_has_boundary_prefix(
    std::string_view operation, std::string_view message) noexcept {
	return message.starts_with(operation);
}

[[noreturn]] inline void rethrow_write_exception_at_boundary_or_throw(
    std::string_view operation, std::string_view file_path,
    const diag::DicomException& ex) {
	const std::string_view message = ex.what();
	if (write_exception_needs_boundary_prefix(message)) {
		diag::error_and_throw("{} file={} {}", operation, file_path, message);
	}
	if (write_exception_has_boundary_prefix(operation, message)) {
		diag::error_and_throw("{}", message);
	}
	throw;
}

[[noreturn]] inline void rethrow_write_exception_at_boundary(
    std::string_view operation, std::string_view file_path,
    const diag::DicomException& ex) {
	const std::string_view message = ex.what();
	if (write_exception_needs_boundary_prefix(message)) {
		diag::throw_exception("{} file={} {}", operation, file_path, message);
	}
	throw;
}

[[nodiscard]] inline std::string_view checked_u32_label_text(
    CheckedU32Label label) noexcept {
	switch (label) {
	case CheckedU32Label::element_length:
		return "element length";
	case CheckedU32Label::pixel_fragment_length:
		return "pixel fragment length";
	case CheckedU32Label::native_pixel_data_length:
		return "native PixelData length";
	case CheckedU32Label::file_meta_group_length:
		return "file meta group length";
	case CheckedU32Label::basic_offset_table_length:
		return "basic offset table length";
	case CheckedU32Label::extended_offset_table_length:
		return "ExtendedOffsetTable length";
	case CheckedU32Label::extended_offset_table_lengths_length:
		return "ExtendedOffsetTableLengths length";
	}
	return "length";
}

inline void write_u16(BufferWriter& writer, std::uint16_t value) {
	writer.append_u16_le(value);
}

inline void write_u32(BufferWriter& writer, std::uint32_t value) {
	writer.append_u32_le(value);
}

inline void write_tag(BufferWriter& writer, Tag tag) {
	writer.append_tag(tag);
}

// Guards size_t -> uint32_t conversions for DICOM VL fields.
[[nodiscard]] inline std::uint32_t checked_u32(
    std::size_t value, CheckedU32Label label) {
	if (value > std::numeric_limits<std::uint32_t>::max()) {
		throw_write_stage_error("checked_u32",
		    "{} exceeds 32-bit range", checked_u32_label_text(label));
	}
	return static_cast<std::uint32_t>(value);
}

[[nodiscard]] inline VR native_pixel_vr_from_bits_allocated_for_write(
    int bits_allocated) noexcept {
	return bits_allocated > 8 ? VR::OW : VR::OB;
}

[[nodiscard]] inline char ascii_upper_for_write(char value) noexcept {
	return (value >= 'a' && value <= 'z')
	           ? static_cast<char>(value - ('a' - 'A'))
	           : value;
}

[[nodiscard]] inline bool ascii_iequals_keyword(
    std::string_view lhs, std::string_view rhs) noexcept {
	if (lhs.size() != rhs.size()) {
		return false;
	}
	for (std::size_t index = 0; index < lhs.size(); ++index) {
		if (ascii_upper_for_write(lhs[index]) != ascii_upper_for_write(rhs[index])) {
			return false;
		}
	}
	return true;
}

template <typename Writer>
void write_u16(Writer& writer, std::uint16_t value) {
	std::array<std::uint8_t, 2> bytes{};
	endian::store_le<std::uint16_t>(bytes.data(), value);
	writer.append(bytes.data(), bytes.size());
}

template <typename Writer>
void write_u32(Writer& writer, std::uint32_t value) {
	std::array<std::uint8_t, 4> bytes{};
	endian::store_le<std::uint32_t>(bytes.data(), value);
	writer.append(bytes.data(), bytes.size());
}

template <typename Writer>
void write_tag(Writer& writer, Tag tag) {
	write_u16(writer, tag.group());
	write_u16(writer, tag.element());
}

// Normalizes VR choices that depend on write context instead of stored element state.
[[nodiscard]] inline VR normalize_vr_for_write(Tag tag, VR vr) {
	if (vr == VR::None) {
		const auto vr_value = lookup::tag_to_vr(tag.value());
		vr = vr_value == 0 ? VR::UN : VR(vr_value);
	}
	if (vr == VR::PX) {
		return VR::OB;
	}
	return vr;
}

template <typename Writer>
void write_data_element(const DataElement& element, Writer& writer, bool explicit_vr);

template <typename Writer>
void write_element_header(Writer& writer, Tag tag, VR vr, std::uint32_t value_length,
    bool undefined_length, bool explicit_vr) {
	std::array<std::uint8_t, 12> header{};
	endian::store_le<std::uint16_t>(header.data(), tag.group());
	endian::store_le<std::uint16_t>(header.data() + 2, tag.element());

	if (!explicit_vr) {
		endian::store_le<std::uint32_t>(
		    header.data() + 4, undefined_length ? 0xFFFFFFFFu : value_length);
		writer.append(header.data(), 8);
		return;
	}

	const std::uint16_t raw_vr = vr.raw_code();
	header[4] = static_cast<std::uint8_t>((raw_vr >> 8) & 0xFFu);
	header[5] = static_cast<std::uint8_t>(raw_vr & 0xFFu);

	if (!undefined_length && vr.uses_explicit_16bit_vl()) {
		if (value_length > std::numeric_limits<std::uint16_t>::max()) {
			throw_write_stage_error("write_element_header",
			    "16-bit VL overflow for tag={} vr={} length={}",
			    tag.to_string(), vr.str(), value_length);
		}
		endian::store_le<std::uint16_t>(
		    header.data() + 6, static_cast<std::uint16_t>(value_length));
		writer.append(header.data(), 8);
		return;
	}

	endian::store_le<std::uint16_t>(header.data() + 6, 0u);
	endian::store_le<std::uint32_t>(
	    header.data() + 8, undefined_length ? 0xFFFFFFFFu : value_length);
	writer.append(header.data(), 12);
}

template <typename Writer>
void write_item_header(Writer& writer, Tag tag, std::uint32_t value_length) {
	std::array<std::uint8_t, 8> header{};
	endian::store_le<std::uint16_t>(header.data(), tag.group());
	endian::store_le<std::uint16_t>(header.data() + 2, tag.element());
	endian::store_le<std::uint32_t>(header.data() + 4, value_length);
	writer.append(header.data(), header.size());
}

[[nodiscard]] inline std::size_t padded_length(std::size_t raw_length) {
	return raw_length + (raw_length & 1u);
}

template <typename Writer>
void append_zero_filled_bytes(Writer& writer, std::size_t count) {
	static constexpr std::array<std::uint8_t, 4096> kZeroChunk{};
	while (count != 0) {
		const auto chunk = std::min(count, kZeroChunk.size());
		writer.append(kZeroChunk.data(), chunk);
		count -= chunk;
	}
}

}  // namespace dicom::write_detail
