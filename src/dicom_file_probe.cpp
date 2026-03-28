#include "dicom.h"

#include "dicom_endian.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <fstream>

namespace dicom {

namespace {

constexpr std::size_t kDicomFileProbeBytes = 1024;
constexpr std::size_t kDicomFileProbeEvenElementTarget = 3;
constexpr std::size_t kDicomPreambleSize = 128;
constexpr std::size_t kDicomMarkerSize = 4;
constexpr std::size_t kDicomDatasetOffsetWithMarker = kDicomPreambleSize + kDicomMarkerSize;

struct PrefixProbeState {
	std::span<const std::uint8_t> bytes{};
	std::size_t offset{0};
	std::uint32_t last_tag_value{0};
	bool has_last_tag{false};
	std::size_t even_group_count{0};
};

[[nodiscard]] bool probe_has_bytes(
    std::span<const std::uint8_t> bytes, std::size_t offset, std::size_t count) noexcept {
	return offset <= bytes.size() && count <= (bytes.size() - offset);
}

[[nodiscard]] bool probe_value_fits(
    const PrefixProbeState& state, std::size_t header_size, std::uint32_t length) noexcept {
	if (!probe_has_bytes(state.bytes, state.offset, header_size)) {
		return false;
	}
	return static_cast<std::size_t>(length) <=
	    (state.bytes.size() - state.offset - header_size);
}

[[nodiscard]] bool advance_prefix_probe(
    PrefixProbeState& state, bool explicit_vr) noexcept {
	if (!probe_has_bytes(state.bytes, state.offset, 8)) {
		return false;
	}

	const auto* const ptr = state.bytes.data() + state.offset;
	const auto group = endian::load_le<std::uint16_t>(ptr);
	const auto element = endian::load_le<std::uint16_t>(ptr + 2);
	if (group == 0u || group == 0xFFFEu || group == 0xFFFFu) {
		return false;
	}

	const auto tag_value =
	    (static_cast<std::uint32_t>(group) << 16) | static_cast<std::uint32_t>(element);
	if (state.has_last_tag && tag_value < state.last_tag_value) {
		return false;
	}

	std::size_t header_size = 8;
	std::uint32_t length = 0;
	if (explicit_vr) {
		const VR vr(static_cast<char>(ptr[4]), static_cast<char>(ptr[5]));
		if (!vr.is_known() || vr == VR::PX) {
			return false;
		}
		if (vr.uses_explicit_16bit_vl()) {
			length = endian::load_le<std::uint16_t>(ptr + 6);
		} else {
			if (!probe_has_bytes(state.bytes, state.offset, 12)) {
				return false;
			}
			if (ptr[6] != 0 || ptr[7] != 0) {
				return false;
			}
			header_size = 12;
			length = endian::load_le<std::uint32_t>(ptr + 8);
		}
	} else {
		length = endian::load_le<std::uint32_t>(ptr + 4);
	}

	if (length == 0xFFFFFFFFu || !probe_value_fits(state, header_size, length)) {
		return false;
	}

	state.offset += header_size + static_cast<std::size_t>(length);
	state.last_tag_value = tag_value;
	state.has_last_tag = true;
	if ((group & 0x1u) == 0) {
		++state.even_group_count;
	}
	return true;
}

[[nodiscard]] bool sniff_raw_dicom_prefix(
    std::span<const std::uint8_t> bytes, std::size_t start_offset, bool explicit_vr) noexcept {
	if (!probe_has_bytes(bytes, start_offset, 8)) {
		return false;
	}

	PrefixProbeState state{bytes, start_offset};
	while (state.even_group_count < kDicomFileProbeEvenElementTarget) {
		if (!advance_prefix_probe(state, explicit_vr)) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] bool has_dicom_marker(std::span<const std::uint8_t> bytes) noexcept {
	if (!probe_has_bytes(bytes, kDicomPreambleSize, kDicomMarkerSize)) {
		return false;
	}

	const auto* const ptr = bytes.data() + kDicomPreambleSize;
	return std::memcmp(ptr, "DICM", kDicomMarkerSize) == 0;
}

}  // namespace

bool is_dicom_file(const std::filesystem::path& path) {
	try {
		std::ifstream stream(path, std::ios::binary);
		if (!stream) {
			return false;
		}

		std::array<std::uint8_t, kDicomFileProbeBytes> buffer{};
		stream.read(reinterpret_cast<char*>(buffer.data()),
		    static_cast<std::streamsize>(buffer.size()));
		const auto bytes_read = static_cast<std::size_t>(stream.gcount());
		if (bytes_read == 0) {
			return false;
		}

		const std::span<const std::uint8_t> prefix(buffer.data(), bytes_read);
		const auto start_offset = has_dicom_marker(prefix) ? kDicomDatasetOffsetWithMarker : 0u;
		return sniff_raw_dicom_prefix(prefix, start_offset, true) ||
		    sniff_raw_dicom_prefix(prefix, start_offset, false);
	} catch (...) {
		return false;
	}
}

}  // namespace dicom
