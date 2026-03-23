#include "dataset_deflate_codec.h"

#include <diagnostics.h>

#include <algorithm>
#include <cstring>
#include <fmt/format.h>
#include <limits>
#include <libdeflate.h>

namespace diag = dicom::diag;

namespace dicom {

namespace {

[[noreturn]] void throw_dataset_transform_error(const char* stage, std::string_view reason) {
	throw diag::DicomException(fmt::format("stage={} reason={}", stage, reason));
}

[[noreturn]] void throw_dataset_transform_error(const char* stage, std::size_t offset,
    std::string_view reason) {
	throw diag::DicomException(fmt::format(
	    "stage={} offset=0x{:X} reason={}", stage, offset, reason));
}

const char* libdeflate_result_name(enum libdeflate_result result) noexcept {
	switch (result) {
	case LIBDEFLATE_SUCCESS:
		return "LIBDEFLATE_SUCCESS";
	case LIBDEFLATE_BAD_DATA:
		return "LIBDEFLATE_BAD_DATA";
	case LIBDEFLATE_SHORT_OUTPUT:
		return "LIBDEFLATE_SHORT_OUTPUT";
	case LIBDEFLATE_INSUFFICIENT_SPACE:
		return "LIBDEFLATE_INSUFFICIENT_SPACE";
	default:
		return "LIBDEFLATE_UNKNOWN";
	}
}

} // namespace

std::vector<std::uint8_t> inflate_deflated_dataset(std::span<const std::uint8_t> full_input,
    std::size_t deflated_start_offset) {
	if (deflated_start_offset > full_input.size()) {
		throw_dataset_transform_error(
		    "dataset_inflate", deflated_start_offset, "invalid deflated data start offset");
	}

	const auto compressed_input = full_input.subspan(deflated_start_offset);
	if (compressed_input.empty()) {
		return std::vector<std::uint8_t>(full_input.begin(),
		    full_input.begin() + static_cast<std::ptrdiff_t>(deflated_start_offset));
	}

	struct libdeflate_decompressor* decompressor = libdeflate_alloc_decompressor();
	if (!decompressor) {
		throw_dataset_transform_error("dataset_inflate", deflated_start_offset,
		    "failed to allocate libdeflate decompressor");
	}

	std::size_t tail_capacity = std::max<std::size_t>(compressed_input.size() * 4, 1u << 20);
	if (deflated_start_offset > std::numeric_limits<std::size_t>::max() - tail_capacity) {
		libdeflate_free_decompressor(decompressor);
		throw_dataset_transform_error(
		    "dataset_inflate", deflated_start_offset, "deflated output too large");
	}

	std::vector<std::uint8_t> output(deflated_start_offset + tail_capacity);
	if (deflated_start_offset > 0) {
		std::memcpy(output.data(), full_input.data(), deflated_start_offset);
	}

	size_t actual_out = 0;
	enum libdeflate_result result = LIBDEFLATE_INSUFFICIENT_SPACE;
	while (true) {
		result = libdeflate_deflate_decompress(decompressor, compressed_input.data(),
		    compressed_input.size(), output.data() + deflated_start_offset,
		    output.size() - deflated_start_offset, &actual_out);
		if (result == LIBDEFLATE_SUCCESS) {
			output.resize(deflated_start_offset + actual_out);
			break;
		}
		if (result != LIBDEFLATE_INSUFFICIENT_SPACE) {
			libdeflate_free_decompressor(decompressor);
			throw_dataset_transform_error("dataset_inflate", deflated_start_offset,
			    fmt::format("deflate decompression failed result={}",
			        libdeflate_result_name(result)));
		}

		const auto current_tail_capacity = output.size() - deflated_start_offset;
		if (current_tail_capacity > std::numeric_limits<std::size_t>::max() / 2) {
			libdeflate_free_decompressor(decompressor);
			throw_dataset_transform_error(
			    "dataset_inflate", deflated_start_offset, "deflated output too large");
		}

		const auto next_tail_capacity = current_tail_capacity * 2;
		if (deflated_start_offset > std::numeric_limits<std::size_t>::max() - next_tail_capacity) {
			libdeflate_free_decompressor(decompressor);
			throw_dataset_transform_error(
			    "dataset_inflate", deflated_start_offset, "deflated output too large");
		}
		output.resize(deflated_start_offset + next_tail_capacity);
	}

	libdeflate_free_decompressor(decompressor);
	return output;
}

std::vector<std::uint8_t> deflate_dataset_body(std::span<const std::uint8_t> dataset_body) {
	constexpr int kCompressionLevel = 6;

	struct libdeflate_compressor* compressor = libdeflate_alloc_compressor(kCompressionLevel);
	if (!compressor) {
		throw_dataset_transform_error(
		    "dataset_deflate", "failed to allocate libdeflate compressor");
	}

	const auto bound = libdeflate_deflate_compress_bound(compressor, dataset_body.size());
	if (bound == 0) {
		libdeflate_free_compressor(compressor);
		throw_dataset_transform_error(
		    "dataset_deflate", "invalid deflate bound for dataset body");
	}

	std::vector<std::uint8_t> compressed(bound);
	const auto actual = libdeflate_deflate_compress(compressor, dataset_body.data(),
	    dataset_body.size(), compressed.data(), compressed.size());
	libdeflate_free_compressor(compressor);

	if (actual == 0) {
		throw_dataset_transform_error("dataset_deflate", "deflate compression failed");
	}

	compressed.resize(actual);
	return compressed;
}

} // namespace dicom
