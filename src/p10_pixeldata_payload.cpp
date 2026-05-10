#include "dicom.h"

#include "dicom_endian.h"
#include "diagnostics.h"
#include "pixeldata_payload_placeholder.hpp"

#include <algorithm>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dicom {
using namespace dicom::literals;

namespace {

void append_u16_le(std::vector<std::uint8_t>& out, std::uint16_t value) {
	const auto offset = out.size();
	out.resize(offset + sizeof(value));
	endian::store_le(out.data() + offset, value);
}

void append_u32_le(std::vector<std::uint8_t>& out, std::uint32_t value) {
	const auto offset = out.size();
	out.resize(offset + sizeof(value));
	endian::store_le(out.data() + offset, value);
}

void append_pixeldata_payload_placeholder_element(
    std::vector<std::uint8_t>& out, bool explicit_vr,
    VR original_vr, std::uint32_t original_vl,
    std::uint64_t payload_length) {
	const auto placeholder_value =
	    detail::make_pixeldata_payload_placeholder_value(
	        original_vr, original_vl, payload_length);
	append_u16_le(out, 0x7FE0u);
	append_u16_le(out, 0x0010u);
	if (explicit_vr) {
		out.push_back(static_cast<std::uint8_t>('O'));
		out.push_back(static_cast<std::uint8_t>('B'));
		append_u16_le(out, 0u);
		append_u32_le(out,
		    static_cast<std::uint32_t>(placeholder_value.size()));
	} else {
		append_u32_le(out,
		    static_cast<std::uint32_t>(placeholder_value.size()));
	}
	out.insert(out.end(), placeholder_value.begin(), placeholder_value.end());
}

[[nodiscard]] DataSetSelection split_pixeldata_augmented_selection(
    const DataSetSelection& selection) {
	return selection.extended({
	    "PixelData"_tag,
	    "FloatPixelData"_tag,
	    "DoubleFloatPixelData"_tag,
	    "Rows"_tag,
	    "Columns"_tag,
	    "SamplesPerPixel"_tag,
	    "BitsAllocated"_tag,
	    "BitsStored"_tag,
	    "PixelRepresentation"_tag,
	    "NumberOfFrames"_tag,
	    "PlanarConfiguration"_tag,
	    "PhotometricInterpretation"_tag,
	});
}

[[nodiscard]] VR native_pixel_vr_from_bits_allocated_for_split(
    int bits_allocated) noexcept {
	return bits_allocated > 8 ? VR::OW : VR::OB;
}

[[nodiscard]] VR implicit_pixel_payload_metadata_vr(const DicomFile& file) {
	const auto bits_allocated =
	    static_cast<int>(file.dataset()["BitsAllocated"_tag].to_long().value_or(16));
	return native_pixel_vr_from_bits_allocated_for_split(bits_allocated);
}

[[nodiscard]] detail::PixelDataPayloadPlaceholderMetadata
original_pixel_payload_metadata_or_throw(
    const DicomFile& file, std::span<const std::uint8_t> source,
    std::size_t element_offset, std::size_t value_offset,
    std::size_t value_length, bool explicit_vr) {
	detail::PixelDataPayloadPlaceholderMetadata metadata{};
	metadata.payload_length = static_cast<std::uint64_t>(value_length);
	if (explicit_vr) {
		if (element_offset + 12u > source.size()) {
			diag::error_and_throw(
			    "split_pixeldata_payload file={} offset=0x{:X} reason=explicit PixelData header exceeds source",
			    file.path(), element_offset);
		}
		metadata.original_vr = VR(static_cast<char>(source[element_offset + 4u]),
		    static_cast<char>(source[element_offset + 5u]));
		if (!detail::is_supported_pixeldata_payload_header_vr(metadata.original_vr)) {
			diag::error_and_throw(
			    "split_pixeldata_payload file={} vr={} reason=unsupported PixelData header VR",
			    file.path(), metadata.original_vr.str());
		}
		const auto reserved =
		    endian::load_le<std::uint16_t>(source.data() + element_offset + 6u);
		if (reserved != 0) {
			diag::error_and_throw(
			    "split_pixeldata_payload file={} offset=0x{:X} reason=explicit PixelData reserved bytes must be zero",
			    file.path(), element_offset);
		}
		metadata.original_vl =
		    endian::load_le<std::uint32_t>(source.data() + element_offset + 8u);
		(void)value_offset;
		return metadata;
	}

	if (element_offset + 8u > source.size()) {
		diag::error_and_throw(
		    "split_pixeldata_payload file={} offset=0x{:X} reason=implicit PixelData header exceeds source",
		    file.path(), element_offset);
	}
	metadata.original_vr = implicit_pixel_payload_metadata_vr(file);
	metadata.original_vl =
	    endian::load_le<std::uint32_t>(source.data() + element_offset + 4u);
	return metadata;
}

	void set_split_pixeldata_payload_error(
	    SplitPixelDataPayloadResult& result, std::string message) {
		result.error_message = std::move(message);
		result.main_bytes.clear();
		result.pixel_payload.clear();
		result.decode_descriptor = pixel::PixelPayloadDecodeDescriptor{};
	}

	[[nodiscard]] SplitPixelDataPayloadResult split_pixeldata_payload_error_result(
	    std::string message) {
		SplitPixelDataPayloadResult result{};
		set_split_pixeldata_payload_error(result, std::move(message));
		return result;
	}

	[[nodiscard]] std::optional<std::string> split_pixeldata_payload_preflight_error(
	    const DicomFile& df) {
		if (df.has_error()) {
			return df.error_message();
		}
		const auto& transfer_syntax_element = df.dataset()["TransferSyntaxUID"_tag];
		if (!transfer_syntax_element.is_present()) {
			return std::nullopt;
		}
		const auto transfer_syntax = transfer_syntax_element.to_transfer_syntax_uid();
		if (!transfer_syntax) {
			return fmt::format(
			    "split_pixeldata_payload file={} reason=invalid transfer syntax",
			    df.path());
		}
		if (*transfer_syntax == "DeflatedExplicitVRLittleEndian"_uid ||
		    *transfer_syntax == "ExplicitVRBigEndian"_uid) {
			return fmt::format(
			    "split_pixeldata_payload file={} transfer_syntax_uid={} reason=unsupported transfer syntax for byte-preserving split",
			    df.path(), transfer_syntax->value());
		}
		return std::nullopt;
	}

[[nodiscard]] SplitPixelDataPayloadResult
split_pixeldata_payload_loaded_or_empty(
    std::unique_ptr<DicomFile> file) {
	SplitPixelDataPayloadResult result{};
	auto& df = *file;

	if (df.has_error()) {
		set_split_pixeldata_payload_error(result, df.error_message());
		return result;
	}
	if (!df.dataset()["TransferSyntaxUID"_tag].is_present()) {
		set_split_pixeldata_payload_error(result,
		    fmt::format(
		        "split_pixeldata_payload file={} reason=TransferSyntaxUID is missing",
		        df.path()));
		return result;
	}
	const auto transfer_syntax = df.transfer_syntax_uid();
	if (!transfer_syntax.valid() ||
	    transfer_syntax.uid_type() != UidType::TransferSyntax) {
		set_split_pixeldata_payload_error(result,
		    fmt::format(
		        "split_pixeldata_payload file={} reason=invalid transfer syntax",
		        df.path()));
		return result;
	}
	if (transfer_syntax == "DeflatedExplicitVRLittleEndian"_uid ||
	    transfer_syntax == "ExplicitVRBigEndian"_uid) {
		set_split_pixeldata_payload_error(result,
		    fmt::format(
		        "split_pixeldata_payload file={} transfer_syntax_uid={} reason=unsupported transfer syntax for byte-preserving split",
		        df.path(), transfer_syntax.value()));
		return result;
	}

	const auto& pixel_data = df.dataset()["PixelData"_tag];
	if (!pixel_data || pixel_data.is_missing()) {
		set_split_pixeldata_payload_error(result,
		    fmt::format("split_pixeldata_payload file={} reason=PixelData is missing",
		        df.path()));
		return result;
	}
	if (df.dataset()["FloatPixelData"_tag].is_present() ||
	    df.dataset()["DoubleFloatPixelData"_tag].is_present()) {
		set_split_pixeldata_payload_error(result,
		    fmt::format(
		        "split_pixeldata_payload file={} reason=FloatPixelData and DoubleFloatPixelData are not supported",
		        df.path()));
		return result;
	}

	try {
		const auto& stream = df.stream();
		const auto source = stream.get_span(0, stream.end_offset());
		const bool explicit_vr = df.dataset().is_explicit_vr();
		const std::size_t header_size = explicit_vr ? 12u : 8u;
		const auto value_offset = pixel_data.offset();
		const auto value_length = pixel_data.length();
		if (value_offset < header_size) {
			diag::error_and_throw(
			    "split_pixeldata_payload file={} offset=0x{:X} reason=PixelData value offset is before expected header",
			    df.path(), value_offset);
		}
		const auto element_offset = value_offset - header_size;
		if (element_offset + 4u > source.size()) {
			diag::error_and_throw(
			    "split_pixeldata_payload file={} offset=0x{:X} reason=PixelData element offset is outside source",
			    df.path(), element_offset);
		}
		const auto tag = endian::load_tag_le(source.data() + element_offset);
		if (tag != "7fe0,0010"_tag) {
			diag::error_and_throw(
			    "split_pixeldata_payload file={} offset=0x{:X} reason=PixelData element header not found",
			    df.path(), element_offset);
		}
		if (value_offset > source.size() ||
		    value_length > source.size() - value_offset) {
			diag::error_and_throw(
			    "split_pixeldata_payload file={} offset=0x{:X} length={} reason=PixelData value exceeds source",
			    df.path(), value_offset, value_length);
		}
		const auto element_end = value_offset + value_length;
		const auto element_size = element_end - element_offset;
		const auto metadata = original_pixel_payload_metadata_or_throw(
		    df, source, element_offset, value_offset, value_length, explicit_vr);

		result.pixel_payload.assign(
		    source.begin() + static_cast<std::ptrdiff_t>(value_offset),
		    source.begin() + static_cast<std::ptrdiff_t>(element_end));

		const std::size_t placeholder_element_size =
		    (explicit_vr ? 12u : 8u) + kPixelDataPayloadPlaceholderMetadataSize;
		result.main_bytes.reserve(
		    source.size() - element_size + placeholder_element_size);
		result.main_bytes.insert(result.main_bytes.end(), source.begin(),
		    source.begin() + static_cast<std::ptrdiff_t>(element_offset));
		append_pixeldata_payload_placeholder_element(result.main_bytes, explicit_vr,
		    metadata.original_vr, metadata.original_vl, metadata.payload_length);
		result.main_bytes.insert(result.main_bytes.end(),
		    source.begin() + static_cast<std::ptrdiff_t>(element_end),
		    source.end());
		result.decode_descriptor = df.pixel_payload_decode_descriptor();
	} catch (const std::exception& ex) {
		set_split_pixeldata_payload_error(result, ex.what());
	}
	return result;
}

} // namespace

SplitPixelDataPayloadResult split_pixeldata_payload(
    const DataSetSelection& selection, const std::filesystem::path& path) {
	ReadOptions preflight_options;
	preflight_options.keep_on_error = true;
	preflight_options.load_until = "TransferSyntaxUID"_tag;
	auto file = read_file(path, preflight_options);
	if (!file) {
		return split_pixeldata_payload_error_result(
		    fmt::format("split_pixeldata_payload file={} reason=failed to open file",
		        path.string()));
	}
	if (auto error = split_pixeldata_payload_preflight_error(*file)) {
		return split_pixeldata_payload_error_result(std::move(*error));
	}
	ReadOptions options;
	options.keep_on_error = true;
	continue_read_selected(*file, split_pixeldata_augmented_selection(selection), options);
	return split_pixeldata_payload_loaded_or_empty(std::move(file));
}

SplitPixelDataPayloadResult split_pixeldata_payload(
    const DataSetSelection& selection, std::string_view name,
    std::span<const std::uint8_t> bytes) {
	ReadOptions preflight_options;
	preflight_options.keep_on_error = true;
	preflight_options.copy = false;
	preflight_options.load_until = "TransferSyntaxUID"_tag;
	auto file = read_bytes(std::string{name}, bytes.data(), bytes.size(), preflight_options);
	if (!file) {
		return split_pixeldata_payload_error_result(
		    fmt::format("split_pixeldata_payload file={} reason=failed to read bytes",
		        name));
	}
	if (auto error = split_pixeldata_payload_preflight_error(*file)) {
		return split_pixeldata_payload_error_result(std::move(*error));
	}
	ReadOptions options;
	options.keep_on_error = true;
	options.copy = false;
	continue_read_selected(*file, split_pixeldata_augmented_selection(selection), options);
	return split_pixeldata_payload_loaded_or_empty(std::move(file));
}

std::vector<std::uint8_t> join_pixeldata_payload(
    std::span<const std::uint8_t> main_bytes,
    std::span<const std::uint8_t> pixel_payload) {
	const auto fail = [](std::string_view reason) -> void {
		diag::error_and_throw("join_pixeldata_payload reason={}", reason);
	};
	if (main_bytes.size() < kPixelDataPayloadPlaceholderMetadataSize ||
	    pixel_payload.empty()) {
		fail("main bytes or pixel payload is empty");
	}

	const auto metadata_at = [&](std::size_t value_offset) {
		return detail::read_pixeldata_payload_placeholder_metadata(
		    main_bytes.subspan(value_offset, kPixelDataPayloadPlaceholderMetadataSize),
		    "join_pixeldata_payload");
	};

	struct Match {
		std::size_t element_offset{0};
		std::size_t value_offset{0};
		bool explicit_vr{false};
		detail::PixelDataPayloadPlaceholderMetadata metadata{};
	};
	std::optional<Match> match;

	const auto magic_size = kPixelDataPayloadPlaceholderMagic.size();
	for (std::size_t pos = main_bytes.size() - magic_size + 1u; pos-- > 0u;) {
		if (pos + kPixelDataPayloadPlaceholderMetadataSize > main_bytes.size()) {
			continue;
		}
		if (!std::equal(kPixelDataPayloadPlaceholderMagic.begin(),
		        kPixelDataPayloadPlaceholderMagic.end(), main_bytes.begin() + pos)) {
			continue;
		}

		if (pos >= 12u) {
			const auto element_offset = pos - 12u;
			const auto tag = endian::load_tag_le(main_bytes.data() + element_offset);
			const auto vr = VR(static_cast<char>(main_bytes[element_offset + 4u]),
			    static_cast<char>(main_bytes[element_offset + 5u]));
			const auto reserved =
			    endian::load_le<std::uint16_t>(main_bytes.data() + element_offset + 6u);
			const auto vl =
			    endian::load_le<std::uint32_t>(main_bytes.data() + element_offset + 8u);
			if (tag == "7fe0,0010"_tag && vr == VR::OB && reserved == 0u &&
			    vl == kPixelDataPayloadPlaceholderMetadataSize) {
				match = Match{element_offset, pos, true, metadata_at(pos)};
				break;
			}
		}

		if (pos >= 8u) {
			const auto element_offset = pos - 8u;
			const auto tag = endian::load_tag_le(main_bytes.data() + element_offset);
			const auto vl =
			    endian::load_le<std::uint32_t>(main_bytes.data() + element_offset + 4u);
			if (tag == "7fe0,0010"_tag &&
			    vl == kPixelDataPayloadPlaceholderMetadataSize) {
				match = Match{element_offset, pos, false, metadata_at(pos)};
				break;
			}
		}
	}

	if (!match) {
		fail("PixelData placeholder not found");
	}
	if (match->metadata.payload_length != pixel_payload.size()) {
		diag::error_and_throw(
		    "join_pixeldata_payload expected_length={} actual_length={} reason=pixel payload length mismatch",
		    match->metadata.payload_length, pixel_payload.size());
	}
	if (match->metadata.original_vl != 0xFFFFFFFFu &&
	    match->metadata.original_vl != pixel_payload.size()) {
		diag::error_and_throw(
		    "join_pixeldata_payload vl={} actual_length={} reason=pixel payload length does not match original VL",
		    match->metadata.original_vl, pixel_payload.size());
	}

	std::vector<std::uint8_t> out;
	const auto placeholder_end =
	    match->value_offset + kPixelDataPayloadPlaceholderMetadataSize;
	const auto reconstructed_header_size = match->explicit_vr ? 12u : 8u;
	out.reserve(main_bytes.size() -
	    (placeholder_end - match->element_offset) + reconstructed_header_size +
	    pixel_payload.size());
	out.insert(out.end(), main_bytes.begin(),
	    main_bytes.begin() + static_cast<std::ptrdiff_t>(match->element_offset));
	append_u16_le(out, 0x7FE0u);
	append_u16_le(out, 0x0010u);
	if (match->explicit_vr) {
		out.push_back(static_cast<std::uint8_t>(match->metadata.original_vr.first()));
		out.push_back(static_cast<std::uint8_t>(match->metadata.original_vr.second()));
		append_u16_le(out, 0u);
		append_u32_le(out, match->metadata.original_vl);
	} else {
		append_u32_le(out, match->metadata.original_vl);
	}
	out.insert(out.end(), pixel_payload.begin(), pixel_payload.end());
	out.insert(out.end(),
	    main_bytes.begin() + static_cast<std::ptrdiff_t>(placeholder_end),
	    main_bytes.end());
	return out;
}

} // namespace dicom
