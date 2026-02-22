#include "dicom.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

using namespace dicom::literals;

namespace {

struct Options {
	std::string path;
	int iterations = 200;
	int warmup = 20;
};

void print_usage(const char* prog) {
	std::cout << "Usage: " << prog
	          << " --path <dicom-file> [--iterations <n>] [--warmup <n>]\n";
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
		for (int i = 0; i < total; ++i) {
			const auto start = clock::now();
			auto file = dicom::read_file(options.path);
			checksum += touch_dataset(*file);
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
	std::cout << "checksum=" << checksum << "\n";
	std::cout << "mean_ms=" << mean_ms << " median_ms=" << median_ms
	          << " p95_ms=" << p95_ms << " p99_ms=" << p99_ms
	          << " min_ms=" << min_ms << " max_ms=" << max_ms << "\n";
	std::cout << "RESULT"
	          << " mean_ms=" << mean_ms
	          << " median_ms=" << median_ms
	          << " p95_ms=" << p95_ms
	          << " p99_ms=" << p99_ms
	          << " min_ms=" << min_ms
	          << " max_ms=" << max_ms
	          << " checksum=" << checksum << "\n";
	return 0;
}
