#include "dicom.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace dicom::literals;

namespace {

enum class SourceMode {
	file,
	memory,
	memory_nocache,
};

enum class WorkloadMode {
	parse_only,
	read,
	lookup_hot,
	decode_hot,
	read_decode,
};

struct Options {
	std::string path;
	int iterations = 200;
	int warmup = 20;
	int inner_iterations = 1024;
	SourceMode source = SourceMode::file;
	WorkloadMode workload = WorkloadMode::read;
};

std::unique_ptr<dicom::DicomFile> load_file_for_benchmark(
    const std::string& path, SourceMode source);

void print_usage(const char* prog) {
	std::cout << "Usage: " << prog
	          << " --path <dicom-file> [--iterations <n>] [--warmup <n>]"
	          << " [--inner-iterations <n>]"
	          << " [--source <file|memory|memory_nocache>]"
	          << " [--workload <parse_only|read|lookup_hot|decode_hot|read_decode>]\n";
}

bool parse_int_arg(const char* text, int& out) {
	char* end = nullptr;
	const long value = std::strtol(text, &end, 10);
	if (end == text || *end != '\0' || value <= 0 || value > 100000000L) {
		return false;
	}
	out = static_cast<int>(value);
	return true;
}

bool parse_source_arg(std::string_view text, SourceMode& out) {
	if (text == "file") {
		out = SourceMode::file;
		return true;
	}
	if (text == "memory") {
		out = SourceMode::memory;
		return true;
	}
	if (text == "memory_nocache") {
		out = SourceMode::memory_nocache;
		return true;
	}
	return false;
}

bool parse_workload_arg(std::string_view text, WorkloadMode& out) {
	if (text == "parse_only") {
		out = WorkloadMode::parse_only;
		return true;
	}
	if (text == "read") {
		out = WorkloadMode::read;
		return true;
	}
	if (text == "lookup_hot") {
		out = WorkloadMode::lookup_hot;
		return true;
	}
	if (text == "decode_hot") {
		out = WorkloadMode::decode_hot;
		return true;
	}
	if (text == "read_decode") {
		out = WorkloadMode::read_decode;
		return true;
	}
	return false;
}

const char* source_mode_name(SourceMode mode) noexcept {
	switch (mode) {
	case SourceMode::file:
		return "file";
	case SourceMode::memory:
		return "memory";
	case SourceMode::memory_nocache:
		return "memory_nocache";
	}
	return "unknown";
}

const char* workload_mode_name(WorkloadMode mode) noexcept {
	switch (mode) {
	case WorkloadMode::parse_only:
		return "parse_only";
	case WorkloadMode::read:
		return "read";
	case WorkloadMode::lookup_hot:
		return "lookup_hot";
	case WorkloadMode::decode_hot:
		return "decode_hot";
	case WorkloadMode::read_decode:
		return "read_decode";
	}
	return "unknown";
}

std::vector<std::uint8_t> read_file_bytes(const std::string& path, bool use_nocache) {
	const int fd = ::open(path.c_str(), O_RDONLY);
	if (fd < 0) {
		throw std::system_error(errno, std::generic_category(), "open failed");
	}

	if (use_nocache) {
#ifdef F_NOCACHE
		(void)::fcntl(fd, F_NOCACHE, 1);
#endif
	}

	struct stat st {};
	if (::fstat(fd, &st) != 0) {
		const int err = errno;
		(void)::close(fd);
		throw std::system_error(err, std::generic_category(), "fstat failed");
	}
	if (st.st_size < 0) {
		(void)::close(fd);
		throw std::runtime_error("negative file size");
	}

	std::vector<std::uint8_t> buffer(static_cast<std::size_t>(st.st_size));
	std::size_t offset = 0;
	while (offset < buffer.size()) {
		const ssize_t n = ::read(fd, buffer.data() + offset, buffer.size() - offset);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			const int err = errno;
			(void)::close(fd);
			throw std::system_error(err, std::generic_category(), "read failed");
		}
		if (n == 0) {
			break;
		}
		offset += static_cast<std::size_t>(n);
	}
	(void)::close(fd);

	if (offset != buffer.size()) {
		throw std::runtime_error("short read");
	}
	return buffer;
}

bool parse_args(int argc, char** argv, Options& options) {
	for (int i = 1; i < argc; ++i) {
		const std::string arg = argv[i];
		if (arg == "-h" || arg == "--help") {
			print_usage(argv[0]);
			return false;
		}
		if (arg == "--path" && i + 1 < argc) {
			options.path = argv[++i];
			continue;
		}
		if (arg == "--iterations" && i + 1 < argc) {
			if (!parse_int_arg(argv[++i], options.iterations)) {
				std::cerr << "Invalid --iterations value\n";
				return false;
			}
			continue;
		}
		if (arg == "--warmup" && i + 1 < argc) {
			if (!parse_int_arg(argv[++i], options.warmup)) {
				std::cerr << "Invalid --warmup value\n";
				return false;
			}
			continue;
		}
		if (arg == "--inner-iterations" && i + 1 < argc) {
			if (!parse_int_arg(argv[++i], options.inner_iterations)) {
				std::cerr << "Invalid --inner-iterations value\n";
				return false;
			}
			continue;
		}
		if (arg == "--source" && i + 1 < argc) {
			if (!parse_source_arg(argv[++i], options.source)) {
				std::cerr << "Invalid --source value\n";
				return false;
			}
			continue;
		}
		if (arg == "--workload" && i + 1 < argc) {
			if (!parse_workload_arg(argv[++i], options.workload)) {
				std::cerr << "Invalid --workload value\n";
				return false;
			}
			continue;
		}
		std::cerr << "Unknown argument: " << arg << "\n";
		print_usage(argv[0]);
		return false;
	}
	if (options.path.empty()) {
		std::cerr << "--path is required\n";
		print_usage(argv[0]);
		return false;
	}
	return true;
}

std::uint64_t touch_dataset(const dicom::DicomFile& file) {
	const auto& ds = file.dataset();
	std::uint64_t checksum = 0;
	checksum += static_cast<std::uint64_t>(ds["Rows"_tag].to_long(0));
	checksum += static_cast<std::uint64_t>(ds["Columns"_tag].to_long(0));
	checksum += static_cast<std::uint64_t>(ds["BitsAllocated"_tag].to_long(0));
	checksum += static_cast<std::uint64_t>(ds["SamplesPerPixel"_tag].to_long(0));
	checksum += static_cast<std::uint64_t>(ds["NumberOfFrames"_tag].to_long(1));
	checksum += static_cast<std::uint64_t>(ds.size());
	return checksum;
}

std::uint64_t touch_decoded_frame(const dicom::DicomFile& file) {
	const auto plan = file.create_decode_plan(dicom::pixel::DecodeOptions{});
	if (plan.strides.frame == 0) {
		throw std::runtime_error("decode plan frame stride is zero");
	}
	std::vector<std::uint8_t> decoded(plan.strides.frame);
	file.decode_into(0, std::span<std::uint8_t>(decoded), plan);

	std::uint64_t checksum = decoded.size();
	if (!decoded.empty()) {
		checksum += decoded.front();
		checksum += decoded[decoded.size() / 2];
		checksum += decoded.back();
		const std::size_t sample_count = std::min<std::size_t>(decoded.size(), 64);
		for (std::size_t i = 0; i < sample_count; ++i) {
			checksum = (checksum * 1315423911u) ^ decoded[i];
		}
	}
	return checksum;
}

std::uint64_t touch_lookup_hot(
    const dicom::DicomFile& file, int inner_iterations) {
	const auto& ds = file.dataset();
	constexpr std::array<dicom::Tag, 6> kTags{
	    "Rows"_tag,
	    "Columns"_tag,
	    "BitsAllocated"_tag,
	    "SamplesPerPixel"_tag,
	    "NumberOfFrames"_tag,
	    "PixelData"_tag,
	};

	std::uint64_t checksum = 0;
	for (int i = 0; i < inner_iterations; ++i) {
		for (const auto tag : kTags) {
			const auto* element = ds.get_dataelement(tag);
			checksum += static_cast<std::uint64_t>(element->tag().value());
			checksum += static_cast<std::uint64_t>(element->length());
			checksum += static_cast<std::uint64_t>(element->offset());
		}
	}
	return checksum;
}

struct DecodeHotContext {
	std::unique_ptr<dicom::DicomFile> file;
	dicom::pixel::DecodePlan plan{};
	std::vector<std::uint8_t> buffer;
};

DecodeHotContext prepare_decode_hot_context(const std::string& path, SourceMode source) {
	DecodeHotContext ctx;
	ctx.file = load_file_for_benchmark(path, source);
	ctx.plan = ctx.file->create_decode_plan(dicom::pixel::DecodeOptions{});
	if (ctx.plan.strides.frame == 0) {
		throw std::runtime_error("decode plan frame stride is zero");
	}
	ctx.buffer.resize(ctx.plan.strides.frame);
	return ctx;
}

std::uint64_t touch_decode_hot(DecodeHotContext& ctx, int inner_iterations) {
	std::uint64_t checksum = 0;
	for (int i = 0; i < inner_iterations; ++i) {
		ctx.file->decode_into(0, std::span<std::uint8_t>(ctx.buffer), ctx.plan);
		checksum += ctx.buffer.size();
		if (!ctx.buffer.empty()) {
			checksum += ctx.buffer.front();
			checksum += ctx.buffer[ctx.buffer.size() / 2];
			checksum += ctx.buffer.back();
		}
	}
	return checksum;
}

std::unique_ptr<dicom::DicomFile> load_file_for_benchmark(
    const std::string& path, SourceMode source) {
	switch (source) {
	case SourceMode::file:
		return dicom::read_file(path);
	case SourceMode::memory:
		return dicom::read_bytes(path, read_file_bytes(path, false));
	case SourceMode::memory_nocache:
		return dicom::read_bytes(path, read_file_bytes(path, true));
	}
	throw std::runtime_error("unsupported source mode");
}

double compute_median(const std::vector<double>& sorted) {
	if (sorted.empty()) {
		return 0.0;
	}
	const std::size_t n = sorted.size();
	if ((n % 2U) == 1U) {
		return sorted[n / 2U];
	}
	return (sorted[n / 2U - 1U] + sorted[n / 2U]) * 0.5;
}

double compute_percentile(const std::vector<double>& sorted, double pct) {
	if (sorted.empty()) {
		return 0.0;
	}
	const double rank = (pct / 100.0) * static_cast<double>(sorted.size());
	std::size_t index = 0;
	if (rank > 1.0) {
		index = static_cast<std::size_t>(std::ceil(rank)) - 1U;
	}
	if (index >= sorted.size()) {
		index = sorted.size() - 1U;
	}
	return sorted[index];
}

}  // namespace

int main(int argc, char** argv) {
	Options options;
	if (!parse_args(argc, argv, options)) {
		return 1;
	}

	{
		std::ifstream ifs(options.path, std::ios::binary);
		if (!ifs.good()) {
			std::cerr << "Input file does not exist: " << options.path << "\n";
			return 1;
		}
	}

	std::vector<double> samples_ms;
	samples_ms.reserve(static_cast<std::size_t>(options.iterations));

	std::uint64_t checksum = 0;
	try {
		using clock = std::chrono::steady_clock;
		const int total = options.warmup + options.iterations;
		std::unique_ptr<dicom::DicomFile> lookup_hot_file;
		if (options.workload == WorkloadMode::lookup_hot) {
			lookup_hot_file = load_file_for_benchmark(options.path, options.source);
		}
		std::optional<DecodeHotContext> decode_hot_ctx;
		if (options.workload == WorkloadMode::decode_hot) {
			decode_hot_ctx.emplace(prepare_decode_hot_context(options.path, options.source));
		}

		for (int i = 0; i < total; ++i) {
			const auto start = clock::now();
			switch (options.workload) {
			case WorkloadMode::parse_only: {
				auto file = load_file_for_benchmark(options.path, options.source);
				checksum += static_cast<std::uint64_t>(file->size());
				break;
			}
			case WorkloadMode::read: {
				auto file = load_file_for_benchmark(options.path, options.source);
				checksum += touch_dataset(*file);
				break;
			}
			case WorkloadMode::lookup_hot:
				checksum += touch_lookup_hot(*lookup_hot_file, options.inner_iterations);
				break;
			case WorkloadMode::decode_hot:
				checksum += touch_decode_hot(*decode_hot_ctx, options.inner_iterations);
				break;
			case WorkloadMode::read_decode: {
				auto file = load_file_for_benchmark(options.path, options.source);
				checksum += touch_dataset(*file);
				checksum += touch_decoded_frame(*file);
				break;
			}
			}
			const auto end = clock::now();
			const double elapsed_ms =
			    std::chrono::duration<double, std::milli>(end - start).count();
			if (i >= options.warmup) {
				samples_ms.push_back(elapsed_ms);
			}
		}
	} catch (const std::exception& ex) {
		std::cerr << "Benchmark failed: " << ex.what() << "\n";
		return 2;
	}

	if (samples_ms.empty()) {
		std::cerr << "No benchmark samples were recorded\n";
		return 2;
	}

	std::vector<double> sorted_ms = samples_ms;
	std::sort(sorted_ms.begin(), sorted_ms.end());

	const double total_ms = std::accumulate(samples_ms.begin(), samples_ms.end(), 0.0);
	const double mean_ms = total_ms / static_cast<double>(samples_ms.size());
	const double min_ms = sorted_ms.front();
	const double max_ms = sorted_ms.back();
	const double median_ms = compute_median(sorted_ms);
	const double p95_ms = compute_percentile(sorted_ms, 95.0);
	const double p99_ms = compute_percentile(sorted_ms, 99.0);

	std::cout << std::fixed << std::setprecision(3);
	std::cout << "path=" << options.path << "\n";
	std::cout << "iterations=" << options.iterations << " warmup=" << options.warmup << "\n";
	std::cout << "inner_iterations=" << options.inner_iterations << "\n";
	std::cout << "source=" << source_mode_name(options.source)
	          << " workload=" << workload_mode_name(options.workload) << "\n";
	std::cout << "checksum=" << checksum << "\n";
	std::cout << "mean_ms=" << mean_ms << " median_ms=" << median_ms
	          << " p95_ms=" << p95_ms << " p99_ms=" << p99_ms
	          << " min_ms=" << min_ms << " max_ms=" << max_ms << "\n";
	std::cout << "RESULT"
	          << " inner_iterations=" << options.inner_iterations
	          << " source=" << source_mode_name(options.source)
	          << " workload=" << workload_mode_name(options.workload)
	          << " mean_ms=" << mean_ms
	          << " median_ms=" << median_ms
	          << " p95_ms=" << p95_ms
	          << " p99_ms=" << p99_ms
	          << " min_ms=" << min_ms
	          << " max_ms=" << max_ms
	          << " checksum=" << checksum << "\n";
	return 0;
}
