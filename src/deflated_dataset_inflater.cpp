#include "deflated_dataset_inflater.h"

#include <diagnostics.h>

#include <algorithm>
#include <cstring>
#include <fmt/format.h>
#include <limits>
#include <libdeflate.h>

namespace diag = dicom::diag;

namespace dicom {

namespace {

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
    std::size_t deflated_start_offset, const std::string& file_path) {
	if (deflated_start_offset > full_input.size()) {
		diag::error_and_throw(
		    fmt::format(
		        "DataSet::read_attached_stream file={} offset=0x{:X} reason=invalid deflated data start offset",
		        file_path, deflated_start_offset));
	}

	const auto compressed_input = full_input.subspan(deflated_start_offset);
	if (compressed_input.empty()) {
		return std::vector<std::uint8_t>(full_input.begin(),
		    full_input.begin() + static_cast<std::ptrdiff_t>(deflated_start_offset));
	}

	struct libdeflate_decompressor* decompressor = libdeflate_alloc_decompressor();
	if (!decompressor) {
		diag::error_and_throw(
		    fmt::format(
		        "DataSet::read_attached_stream file={} offset=0x{:X} reason=failed to allocate libdeflate decompressor",
		        file_path, deflated_start_offset));
	}

	std::size_t tail_capacity = std::max<std::size_t>(compressed_input.size() * 4, 1u << 20);
	if (deflated_start_offset > std::numeric_limits<std::size_t>::max() - tail_capacity) {
		libdeflate_free_decompressor(decompressor);
		diag::error_and_throw(
		    fmt::format(
		        "DataSet::read_attached_stream file={} offset=0x{:X} reason=deflated output too large",
		        file_path, deflated_start_offset));
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
			diag::error_and_throw(
			    fmt::format(
			        "DataSet::read_attached_stream file={} offset=0x{:X} reason=deflate decompression failed result={}",
			        file_path, deflated_start_offset, libdeflate_result_name(result)));
		}

		const auto current_tail_capacity = output.size() - deflated_start_offset;
		if (current_tail_capacity > std::numeric_limits<std::size_t>::max() / 2) {
			libdeflate_free_decompressor(decompressor);
			diag::error_and_throw(
			    fmt::format(
			        "DataSet::read_attached_stream file={} offset=0x{:X} reason=deflated output too large",
			        file_path, deflated_start_offset));
		}

		const auto next_tail_capacity = current_tail_capacity * 2;
		if (deflated_start_offset > std::numeric_limits<std::size_t>::max() - next_tail_capacity) {
			libdeflate_free_decompressor(decompressor);
			diag::error_and_throw(
			    fmt::format(
			        "DataSet::read_attached_stream file={} offset=0x{:X} reason=deflated output too large",
			        file_path, deflated_start_offset));
		}
		output.resize(deflated_start_offset + next_tail_capacity);
	}

	libdeflate_free_decompressor(decompressor);
	return output;
}

} // namespace dicom
