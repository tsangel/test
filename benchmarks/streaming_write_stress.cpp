#include "dicom.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

using namespace dicom::literals;

namespace {

enum class SinkMode : std::uint8_t {
	file = 0,
	seekable_memory,
	nonseekable_null,
};

struct Options {
	int rows = 512;
	int cols = 512;
	int frames = 32;
	int fragments_per_frame = 1;
	int repeat = 3;
	SinkMode sink = SinkMode::file;
	dicom::uid::WellKnown target_ts = "RLELossless"_uid;
};

void print_usage(const char* prog) {
	std::cout
	    << "Usage: " << prog
	    << " [--rows <n>] [--cols <n>] [--frames <n>] [--fragments-per-frame <n>]"
	    << " [--repeat <n>] [--sink <file|seekable_memory|nonseekable_null>]"
	    << " [--target-ts <WellKnownName|UID>]\n";
}

bool parse_positive_int(const char* text, int& out) {
	char* end = nullptr;
	const long value = std::strtol(text, &end, 10);
	if (end == text || *end != '\0' || value <= 0 || value > 1'000'000L) {
		return false;
	}
	out = static_cast<int>(value);
	return true;
}

SinkMode parse_sink_mode(std::string_view text) {
	if (text == "file") {
		return SinkMode::file;
	}
	if (text == "seekable_memory") {
		return SinkMode::seekable_memory;
	}
	if (text == "nonseekable_null") {
		return SinkMode::nonseekable_null;
	}
	throw std::invalid_argument("unsupported sink mode");
}

dicom::uid::WellKnown parse_transfer_syntax_or_throw(std::string_view text) {
	if (const auto by_lookup = dicom::uid::lookup(text);
	    by_lookup && by_lookup->uid_type() == dicom::UidType::TransferSyntax) {
		return *by_lookup;
	}

	dicom::DataSet ds;
	auto& elem = ds.add_dataelement(dicom::Tag(0x0002u, 0x0010u), dicom::VR::UI);
	if (!elem.from_uid_string(text)) {
		throw std::invalid_argument("invalid transfer syntax UID text");
	}
	const auto ts = elem.to_transfer_syntax_uid();
	if (!ts || !ts->valid()) {
		throw std::invalid_argument("transfer syntax is not a known WellKnown UID");
	}
	return *ts;
}

Options parse_args_or_throw(int argc, char** argv) {
	Options options;
	for (int i = 1; i < argc; ++i) {
		const std::string arg = argv[i];
		if (arg == "-h" || arg == "--help") {
			print_usage(argv[0]);
			std::exit(0);
		}
		if (arg == "--rows" && i + 1 < argc) {
			if (!parse_positive_int(argv[++i], options.rows)) {
				throw std::invalid_argument("invalid --rows");
			}
			continue;
		}
		if (arg == "--cols" && i + 1 < argc) {
			if (!parse_positive_int(argv[++i], options.cols)) {
				throw std::invalid_argument("invalid --cols");
			}
			continue;
		}
		if (arg == "--frames" && i + 1 < argc) {
			if (!parse_positive_int(argv[++i], options.frames)) {
				throw std::invalid_argument("invalid --frames");
			}
			continue;
		}
		if (arg == "--fragments-per-frame" && i + 1 < argc) {
			if (!parse_positive_int(argv[++i], options.fragments_per_frame)) {
				throw std::invalid_argument("invalid --fragments-per-frame");
			}
			continue;
		}
		if (arg == "--repeat" && i + 1 < argc) {
			if (!parse_positive_int(argv[++i], options.repeat)) {
				throw std::invalid_argument("invalid --repeat");
			}
			continue;
		}
		if (arg == "--sink" && i + 1 < argc) {
			options.sink = parse_sink_mode(argv[++i]);
			continue;
		}
		if (arg == "--target-ts" && i + 1 < argc) {
			options.target_ts = parse_transfer_syntax_or_throw(argv[++i]);
			continue;
		}
		throw std::invalid_argument("unknown argument: " + arg);
	}
	return options;
}

void append_u16_le(std::vector<std::uint8_t>& out, std::uint16_t v) {
	out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
	out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
}

void append_u32_le(std::vector<std::uint8_t>& out, std::uint32_t v) {
	out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
	out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
	out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFu));
	out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFFu));
}

void append_bytes(
    std::vector<std::uint8_t>& out, const std::vector<std::uint8_t>& value) {
	out.insert(out.end(), value.begin(), value.end());
}

void append_explicit_vr_le_16(std::vector<std::uint8_t>& out, dicom::Tag tag,
    char vr0, char vr1, const std::vector<std::uint8_t>& value) {
	if (value.size() > 0xFFFFu) {
		throw std::runtime_error("16-bit VL overflow while building synthetic dataset");
	}
	append_u16_le(out, tag.group());
	append_u16_le(out, tag.element());
	out.push_back(static_cast<std::uint8_t>(vr0));
	out.push_back(static_cast<std::uint8_t>(vr1));
	append_u16_le(out, static_cast<std::uint16_t>(value.size()));
	append_bytes(out, value);
}

void append_explicit_vr_le_32(std::vector<std::uint8_t>& out, dicom::Tag tag,
    char vr0, char vr1, const std::vector<std::uint8_t>& value,
    bool undefined_length = false) {
	append_u16_le(out, tag.group());
	append_u16_le(out, tag.element());
	out.push_back(static_cast<std::uint8_t>(vr0));
	out.push_back(static_cast<std::uint8_t>(vr1));
	append_u16_le(out, 0u);
	append_u32_le(out, undefined_length ? 0xFFFFFFFFu
	                                    : static_cast<std::uint32_t>(value.size()));
	append_bytes(out, value);
}

std::vector<std::uint8_t> ui_value(std::string uid) {
	if (uid.empty() || uid.back() != '\0') {
		uid.push_back('\0');
	}
	if ((uid.size() & 1u) != 0u) {
		uid.push_back('\0');
	}
	return std::vector<std::uint8_t>(uid.begin(), uid.end());
}

std::vector<std::uint8_t> build_part10(std::string transfer_syntax_uid,
    const std::vector<std::uint8_t>& body) {
	std::vector<std::uint8_t> meta_ts;
	append_explicit_vr_le_16(
	    meta_ts, dicom::Tag(0x0002u, 0x0010u), 'U', 'I',
	    ui_value(std::move(transfer_syntax_uid)));

	std::vector<std::uint8_t> meta_gl_value;
	append_u32_le(meta_gl_value, static_cast<std::uint32_t>(meta_ts.size()));

	std::vector<std::uint8_t> out(128, 0);
	out.insert(out.end(), {'D', 'I', 'C', 'M'});
	append_explicit_vr_le_16(
	    out, dicom::Tag(0x0002u, 0x0000u), 'U', 'L', meta_gl_value);
	append_bytes(out, meta_ts);
	append_bytes(out, body);
	return out;
}

std::vector<std::uint8_t> build_multiframe_encapsulated_uncompressed_body(
    const Options& options) {
	std::vector<std::uint8_t> body;
	append_explicit_vr_le_16(body, dicom::Tag(0x0028u, 0x0002u), 'U', 'S',
	    std::vector<std::uint8_t>{0x01u, 0x00u});
	append_explicit_vr_le_16(body, dicom::Tag(0x0028u, 0x0004u), 'C', 'S',
	    std::vector<std::uint8_t>{
	        'M', 'O', 'N', 'O', 'C', 'H', 'R', 'O', 'M', 'E', '2', ' '});
	append_explicit_vr_le_16(body, dicom::Tag(0x0028u, 0x0010u), 'U', 'S',
	    std::vector<std::uint8_t>{
	        static_cast<std::uint8_t>(options.rows & 0xFF),
	        static_cast<std::uint8_t>((options.rows >> 8) & 0xFF)});
	append_explicit_vr_le_16(body, dicom::Tag(0x0028u, 0x0011u), 'U', 'S',
	    std::vector<std::uint8_t>{
	        static_cast<std::uint8_t>(options.cols & 0xFF),
	        static_cast<std::uint8_t>((options.cols >> 8) & 0xFF)});
	append_explicit_vr_le_16(body, dicom::Tag(0x0028u, 0x0100u), 'U', 'S',
	    std::vector<std::uint8_t>{0x10u, 0x00u});
	append_explicit_vr_le_16(body, dicom::Tag(0x0028u, 0x0101u), 'U', 'S',
	    std::vector<std::uint8_t>{0x10u, 0x00u});
	append_explicit_vr_le_16(body, dicom::Tag(0x0028u, 0x0102u), 'U', 'S',
	    std::vector<std::uint8_t>{0x0Fu, 0x00u});
	append_explicit_vr_le_16(body, dicom::Tag(0x0028u, 0x0103u), 'U', 'S',
	    std::vector<std::uint8_t>{0x00u, 0x00u});

	const auto frame_count_text = std::to_string(options.frames);
	append_explicit_vr_le_16(body, dicom::Tag(0x0028u, 0x0008u), 'I', 'S',
	    std::vector<std::uint8_t>(frame_count_text.begin(), frame_count_text.end()));

	const std::size_t frame_payload_bytes =
	    static_cast<std::size_t>(options.rows) *
	    static_cast<std::size_t>(options.cols) * sizeof(std::uint16_t);
	if (frame_payload_bytes == 0) {
		throw std::runtime_error("frame payload size is zero");
	}
	const bool use_multiframe_eot = options.frames > 1;
	if (use_multiframe_eot && options.fragments_per_frame != 1) {
		throw std::runtime_error(
		    "multi-frame synthetic source currently requires fragments-per-frame=1");
	}

	std::vector<std::uint64_t> eot_offsets;
	std::vector<std::uint64_t> eot_lengths;
	if (use_multiframe_eot) {
		eot_offsets.reserve(static_cast<std::size_t>(options.frames));
		eot_lengths.reserve(static_cast<std::size_t>(options.frames));
	}

	std::vector<std::uint8_t> encapsulated_pixel_value;
	append_u16_le(encapsulated_pixel_value, 0xFFFEu);
	append_u16_le(encapsulated_pixel_value, 0xE000u);
	append_u32_le(encapsulated_pixel_value, 0u);
	std::uint64_t next_frame_offset = 0;

	for (int frame_index = 0; frame_index < options.frames; ++frame_index) {
		std::vector<std::uint8_t> frame_bytes(frame_payload_bytes);
		for (std::size_t pixel_index = 0; pixel_index < frame_payload_bytes / 2u;
		     ++pixel_index) {
			const std::uint16_t value = static_cast<std::uint16_t>(
			    (frame_index + pixel_index) & 0xFFFFu);
			frame_bytes[pixel_index * 2u] =
			    static_cast<std::uint8_t>(value & 0xFFu);
			frame_bytes[pixel_index * 2u + 1u] =
			    static_cast<std::uint8_t>((value >> 8) & 0xFFu);
		}

		if (use_multiframe_eot) {
			eot_offsets.push_back(next_frame_offset);
			eot_lengths.push_back(frame_payload_bytes);
			append_u16_le(encapsulated_pixel_value, 0xFFFEu);
			append_u16_le(encapsulated_pixel_value, 0xE000u);
			append_u32_le(encapsulated_pixel_value,
			    static_cast<std::uint32_t>(frame_payload_bytes));
			encapsulated_pixel_value.insert(encapsulated_pixel_value.end(),
			    frame_bytes.begin(), frame_bytes.end());
			next_frame_offset += 8u + static_cast<std::uint64_t>(frame_payload_bytes);
			continue;
		}

		const std::size_t fragment_count =
		    static_cast<std::size_t>(options.fragments_per_frame);
		const std::size_t base_fragment_size =
		    frame_payload_bytes / fragment_count;
		const std::size_t fragment_remainder =
		    frame_payload_bytes % fragment_count;
		std::size_t offset = 0;
		for (std::size_t fragment_index = 0; fragment_index < fragment_count;
		     ++fragment_index) {
			const std::size_t fragment_size = base_fragment_size +
			    (fragment_index < fragment_remainder ? 1u : 0u);
			if (fragment_size == 0) {
				continue;
			}
			append_u16_le(encapsulated_pixel_value, 0xFFFEu);
			append_u16_le(encapsulated_pixel_value, 0xE000u);
			append_u32_le(encapsulated_pixel_value,
			    static_cast<std::uint32_t>(fragment_size));
			encapsulated_pixel_value.insert(encapsulated_pixel_value.end(),
			    frame_bytes.begin() + static_cast<std::ptrdiff_t>(offset),
			    frame_bytes.begin() +
			        static_cast<std::ptrdiff_t>(offset + fragment_size));
			offset += fragment_size;
		}
	}

	append_u16_le(encapsulated_pixel_value, 0xFFFEu);
	append_u16_le(encapsulated_pixel_value, 0xE0DDu);
	append_u32_le(encapsulated_pixel_value, 0u);

	if (use_multiframe_eot) {
		std::vector<std::uint8_t> eot_offsets_bytes;
		std::vector<std::uint8_t> eot_lengths_bytes;
		eot_offsets_bytes.reserve(eot_offsets.size() * sizeof(std::uint64_t));
		eot_lengths_bytes.reserve(eot_lengths.size() * sizeof(std::uint64_t));
		for (const auto value : eot_offsets) {
			for (int shift = 0; shift < 64; shift += 8) {
				eot_offsets_bytes.push_back(
				    static_cast<std::uint8_t>((value >> shift) & 0xFFu));
			}
		}
		for (const auto value : eot_lengths) {
			for (int shift = 0; shift < 64; shift += 8) {
				eot_lengths_bytes.push_back(
				    static_cast<std::uint8_t>((value >> shift) & 0xFFu));
			}
		}
		append_explicit_vr_le_32(
		    body, dicom::Tag(0x7FE0u, 0x0001u), 'O', 'V', eot_offsets_bytes);
		append_explicit_vr_le_32(
		    body, dicom::Tag(0x7FE0u, 0x0002u), 'O', 'V', eot_lengths_bytes);
	}

	append_explicit_vr_le_32(
	    body, dicom::Tag(0x7FE0u, 0x0010u), 'O', 'B',
	    encapsulated_pixel_value, true);
	return body;
}

std::size_t count_materialized_source_frames(const dicom::DicomFile& file) {
	const auto& pixel_data = file.get_dataelement("PixelData"_tag);
	if (pixel_data.is_missing() || !pixel_data.vr().is_pixel_sequence()) {
		return 0;
	}
	const auto* sequence = pixel_data.as_pixel_sequence();
	if (!sequence) {
		return 0;
	}

	std::size_t materialized = 0;
	for (std::size_t frame_index = 0; frame_index < sequence->number_of_frames();
	     ++frame_index) {
		const auto* frame = sequence->frame(frame_index);
		if (frame && frame->encoded_data_size() != 0) {
			++materialized;
		}
	}
	return materialized;
}

std::optional<std::uint64_t> current_rss_bytes() {
#if defined(_WIN32)
	PROCESS_MEMORY_COUNTERS_EX counters{};
	if (!GetProcessMemoryInfo(GetCurrentProcess(),
	        reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
	        sizeof(counters))) {
		return std::nullopt;
	}
	return static_cast<std::uint64_t>(counters.WorkingSetSize);
#elif defined(__APPLE__)
	mach_task_basic_info info{};
	mach_msg_type_number_t info_count = MACH_TASK_BASIC_INFO_COUNT;
	if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
	        reinterpret_cast<task_info_t>(&info), &info_count) != KERN_SUCCESS) {
		return std::nullopt;
	}
	return static_cast<std::uint64_t>(info.resident_size);
#elif defined(__linux__)
	std::ifstream statm("/proc/self/statm");
	std::uint64_t total_pages = 0;
	std::uint64_t resident_pages = 0;
	if (!(statm >> total_pages >> resident_pages)) {
		return std::nullopt;
	}
	const long page_size = sysconf(_SC_PAGESIZE);
	if (page_size <= 0) {
		return std::nullopt;
	}
	return resident_pages * static_cast<std::uint64_t>(page_size);
#else
	return std::nullopt;
#endif
}

class NullStreamBuf final : public std::streambuf {
public:
	std::size_t bytes_written() const noexcept { return bytes_written_; }

protected:
	std::streamsize xsputn(const char*, std::streamsize count) override {
		if (count > 0) {
			bytes_written_ += static_cast<std::size_t>(count);
		}
		return count;
	}

	int overflow(int ch) override {
		if (ch != traits_type::eof()) {
			++bytes_written_;
			return ch;
		}
		return traits_type::not_eof(ch);
	}

	pos_type seekoff(off_type, std::ios_base::seekdir,
	    std::ios_base::openmode) override {
		return pos_type(off_type(-1));
	}

	pos_type seekpos(pos_type, std::ios_base::openmode) override {
		return pos_type(off_type(-1));
	}

private:
	std::size_t bytes_written_{0};
};

std::size_t run_one_write(dicom::DicomFile& source,
    dicom::uid::WellKnown target_ts, SinkMode sink_mode, std::size_t iteration) {
	switch (sink_mode) {
	case SinkMode::file: {
		const auto temp_dir = std::filesystem::temp_directory_path();
		const auto output_path = temp_dir /
		    ("dicomsdl_streaming_write_stress_" + std::to_string(iteration) + ".dcm");
		source.write_with_transfer_syntax(output_path.string(), target_ts);
		const auto bytes = std::filesystem::file_size(output_path);
		std::error_code ec;
		std::filesystem::remove(output_path, ec);
		return static_cast<std::size_t>(bytes);
	}
	case SinkMode::seekable_memory: {
		std::ostringstream os(std::ios::binary);
		source.write_with_transfer_syntax(os, target_ts);
		return os.str().size();
	}
	case SinkMode::nonseekable_null: {
		NullStreamBuf buf;
		std::ostream os(&buf);
		source.write_with_transfer_syntax(os, target_ts);
		return buf.bytes_written();
	}
	}
	throw std::runtime_error("unsupported sink mode");
}

const char* sink_mode_name(SinkMode mode) noexcept {
	switch (mode) {
	case SinkMode::file:
		return "file";
	case SinkMode::seekable_memory:
		return "seekable_memory";
	case SinkMode::nonseekable_null:
		return "nonseekable_null";
	}
	return "unknown";
}

std::string format_mib(std::optional<std::uint64_t> bytes) {
	if (!bytes) {
		return "n/a";
	}
	std::ostringstream os;
	os.setf(std::ios::fixed);
	os.precision(2);
	os << (static_cast<double>(*bytes) / (1024.0 * 1024.0));
	return os.str();
}

} // namespace

int main(int argc, char** argv) {
	try {
		const auto options = parse_args_or_throw(argc, argv);
		const auto body =
		    build_multiframe_encapsulated_uncompressed_body(options);
		const auto source_bytes = build_part10(
		    "1.2.840.10008.1.2.1.98", body);
		auto source = dicom::read_bytes(
		    "streaming-write-stress", source_bytes.data(), source_bytes.size());
		if (!source) {
			throw std::runtime_error("read_bytes returned null");
		}

		const auto baseline_materialized =
		    count_materialized_source_frames(*source);
		if (baseline_materialized != 0) {
			throw std::runtime_error(
			    "synthetic source unexpectedly materialized frame cache before benchmark");
		}

		const auto frame_payload_bytes =
		    static_cast<std::size_t>(options.rows) *
		    static_cast<std::size_t>(options.cols) * sizeof(std::uint16_t);
		const auto baseline_rss = current_rss_bytes();
		std::optional<std::uint64_t> max_rss = baseline_rss;
		std::size_t max_materialized = 0;
		std::size_t total_output_bytes = 0;

		std::cout << "source_ts=EncapsulatedUncompressedExplicitVRLittleEndian"
		          << " target_ts=" << options.target_ts.value()
		          << " sink=" << sink_mode_name(options.sink)
		          << " rows=" << options.rows
		          << " cols=" << options.cols
		          << " frames=" << options.frames
		          << " fragments_per_frame=" << options.fragments_per_frame
		          << " frame_payload_bytes=" << frame_payload_bytes
		          << " repeat=" << options.repeat << "\n";
		std::cout << "baseline_rss_mib=" << format_mib(baseline_rss) << "\n";

		const auto t0 = std::chrono::steady_clock::now();
		for (int iteration = 0; iteration < options.repeat; ++iteration) {
			total_output_bytes += run_one_write(
			    *source, options.target_ts, options.sink,
			    static_cast<std::size_t>(iteration));
			const auto materialized = count_materialized_source_frames(*source);
			max_materialized = std::max(max_materialized, materialized);
			if (materialized != 0) {
				throw std::runtime_error(
				    "source frame cache materialized during streaming write");
			}
			const auto rss = current_rss_bytes();
			if (rss && (!max_rss || *rss > *max_rss)) {
				max_rss = rss;
			}
			std::cout << "iteration=" << (iteration + 1)
			          << " current_rss_mib=" << format_mib(rss)
			          << " materialized_source_frames=" << materialized
			          << "\n";
		}
		const auto t1 = std::chrono::steady_clock::now();
		const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
		    t1 - t0);

		std::cout << "elapsed_ms=" << elapsed_ms.count()
		          << " total_output_bytes=" << total_output_bytes
		          << " max_observed_rss_mib=" << format_mib(max_rss)
		          << " max_materialized_source_frames=" << max_materialized
		          << "\n";
		return 0;
	} catch (const std::exception& ex) {
		std::cerr << "streaming_write_stress failed: " << ex.what() << "\n";
		return 1;
	}
}
