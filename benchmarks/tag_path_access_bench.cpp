#include "dicom.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using clock_type = std::chrono::steady_clock;
using namespace dicom::literals;

struct Options {
	int iterations = 12;
	int warmup = 3;
	int inner_iterations = 200000;
};

struct PathCase {
	const char* name;
	const char* path;
	dicom::Tag leaf_tag;
	dicom::VR leaf_vr;
};

struct BenchStats {
	std::string name;
	double mean_ns = 0.0;
	double median_ns = 0.0;
	double min_ns = 0.0;
	double max_ns = 0.0;
};

constexpr std::array<PathCase, 4> kPathCases = {{
    {"depth0",
        "ReferencedSOPInstanceUID",
        "ReferencedSOPInstanceUID"_tag,
        dicom::VR::UI},
    {"depth1",
        "ReferencedStudySequence.0.ReferencedSOPInstanceUID",
        "ReferencedSOPInstanceUID"_tag,
        dicom::VR::UI},
    {"depth2",
        "ReferencedStudySequence.0.ReferencedSeriesSequence.0.ReferencedSOPInstanceUID",
        "ReferencedSOPInstanceUID"_tag,
        dicom::VR::UI},
    {"depth4",
        "ReferencedStudySequence.0.ReferencedSeriesSequence.0.ReferencedInstanceSequence.0."
        "SourceImageSequence.0.ReferencedSOPInstanceUID",
        "ReferencedSOPInstanceUID"_tag,
        dicom::VR::UI},
}};

volatile std::uint64_t g_sink = 0;

void print_usage(const char* prog) {
	std::cout << "Usage: " << prog
	          << " [--iterations <n>] [--warmup <n>] [--inner-iterations <n>]\n";
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
		std::cerr << "Unknown argument: " << arg << "\n";
		print_usage(argv[0]);
		return false;
	}
	return true;
}

template <typename Fn>
double measure_ns_per_op(Fn&& fn, int inner_iterations) {
	const auto start = clock_type::now();
	for (int i = 0; i < inner_iterations; ++i) {
		fn();
	}
	const auto elapsed =
	    std::chrono::duration_cast<std::chrono::nanoseconds>(clock_type::now() - start)
	        .count();
	return static_cast<double>(elapsed) / static_cast<double>(inner_iterations);
}

template <typename Fn>
BenchStats run_bench(
    std::string name, const Options& options, Fn&& fn) {
	std::vector<double> samples;
	samples.reserve(static_cast<std::size_t>(options.iterations));

	for (int i = 0; i < options.warmup; ++i) {
		(void)measure_ns_per_op(fn, options.inner_iterations);
	}
	for (int i = 0; i < options.iterations; ++i) {
		samples.push_back(measure_ns_per_op(fn, options.inner_iterations));
	}

	std::sort(samples.begin(), samples.end());
	double sum = 0.0;
	for (double sample : samples) {
		sum += sample;
	}

	BenchStats stats;
	stats.name = std::move(name);
	stats.mean_ns = sum / static_cast<double>(samples.size());
	stats.median_ns = samples[samples.size() / 2];
	stats.min_ns = samples.front();
	stats.max_ns = samples.back();
	return stats;
}

void print_stats(const BenchStats& stats) {
	std::cout << std::left << std::setw(34) << stats.name << " mean=" << std::fixed
	          << std::setprecision(2) << stats.mean_ns << " ns/op"
	          << " median=" << stats.median_ns << " min=" << stats.min_ns
	          << " max=" << stats.max_ns << "\n";
}

void populate_nested_dataset(dicom::DataSet& dataset, const PathCase& path_case) {
	auto& element = dataset.ensure_dataelement(path_case.path, path_case.leaf_vr);
	element.from_string_view("1.2.840.10008.5.1.4.1.1.2");
}

void run_case(const PathCase& path_case, const Options& options) {
	dicom::DataSet dataset;
	populate_nested_dataset(dataset, path_case);

	const auto& initial = dataset.get_dataelement(path_case.path);
	if (initial.is_missing()) {
		throw std::runtime_error("failed to build nested benchmark dataset");
	}

	const std::string prefix = std::string(path_case.name) + " " + path_case.path + " ";
	const bool is_flat_keyword = std::string_view(path_case.path).find('.') == std::string_view::npos;

	const auto get_stats = run_bench(prefix + "get_dataelement", options, [&] {
		auto& element = dataset.get_dataelement(path_case.path);
		g_sink += static_cast<std::uint64_t>(element.tag().value());
	});

	const auto get_tag_stats = run_bench(prefix + "get_dataelement_tag", options, [&] {
		auto& element = dataset.get_dataelement(path_case.leaf_tag);
		g_sink += static_cast<std::uint64_t>(element.tag().value());
	});

	std::optional<BenchStats> get_keyword_tag_stats;
	if (is_flat_keyword) {
		get_keyword_tag_stats =
		    run_bench(prefix + "get_dataelement_keyword_to_tag", options, [&] {
			    auto& element = dataset.get_dataelement(dicom::Tag(path_case.path));
			    g_sink += static_cast<std::uint64_t>(element.tag().value());
		    });
	}

	const auto ensure_stats = run_bench(prefix + "ensure_dataelement", options, [&] {
		auto& element = dataset.ensure_dataelement(path_case.path);
		g_sink += static_cast<std::uint64_t>(element.tag().value());
	});

	const auto ensure_vr_stats =
	    run_bench(prefix + "ensure_dataelement_same_vr", options, [&] {
		    auto& element = dataset.ensure_dataelement(path_case.path, path_case.leaf_vr);
		    g_sink += static_cast<std::uint64_t>(element.tag().value());
	    });

	const auto ensure_tag_stats =
	    run_bench(prefix + "ensure_dataelement_tag", options, [&] {
		    auto& element = dataset.ensure_dataelement(path_case.leaf_tag);
		    g_sink += static_cast<std::uint64_t>(element.tag().value());
	    });

	std::optional<BenchStats> ensure_keyword_tag_stats;
	if (is_flat_keyword) {
		ensure_keyword_tag_stats =
		    run_bench(prefix + "ensure_dataelement_keyword_to_tag", options, [&] {
			    auto& element = dataset.ensure_dataelement(dicom::Tag(path_case.path));
			    g_sink += static_cast<std::uint64_t>(element.tag().value());
		    });
	}

	const auto ensure_tag_vr_stats =
	    run_bench(prefix + "ensure_dataelement_tag_same_vr", options, [&] {
		    auto& element =
		        dataset.ensure_dataelement(path_case.leaf_tag, path_case.leaf_vr);
		    g_sink += static_cast<std::uint64_t>(element.tag().value());
	    });

	const auto set_stats = run_bench(prefix + "set_value", options, [&] {
		const bool ok =
		    dataset.set_value(path_case.path, std::string_view("1.2.840.10008.5.1.4.1.1.2"));
		g_sink += static_cast<std::uint64_t>(ok);
	});

	const auto set_vr_stats = run_bench(prefix + "set_value_same_vr", options, [&] {
		const bool ok = dataset.set_value(
		    path_case.path, path_case.leaf_vr,
		    std::string_view("1.2.840.10008.5.1.4.1.1.2"));
		g_sink += static_cast<std::uint64_t>(ok);
	});

	const auto set_tag_stats = run_bench(prefix + "set_value_tag", options, [&] {
		const bool ok = dataset.set_value(
		    path_case.leaf_tag, std::string_view("1.2.840.10008.5.1.4.1.1.2"));
		g_sink += static_cast<std::uint64_t>(ok);
	});

	std::optional<BenchStats> set_keyword_tag_stats;
	if (is_flat_keyword) {
		set_keyword_tag_stats =
		    run_bench(prefix + "set_value_keyword_to_tag", options, [&] {
			    const bool ok = dataset.set_value(
			        dicom::Tag(path_case.path),
			        std::string_view("1.2.840.10008.5.1.4.1.1.2"));
			    g_sink += static_cast<std::uint64_t>(ok);
		    });
	}

	const auto set_tag_vr_stats = run_bench(prefix + "set_value_tag_same_vr", options, [&] {
		const bool ok = dataset.set_value(
		    path_case.leaf_tag, path_case.leaf_vr,
		    std::string_view("1.2.840.10008.5.1.4.1.1.2"));
		g_sink += static_cast<std::uint64_t>(ok);
	});

	print_stats(get_stats);
	print_stats(get_tag_stats);
	if (get_keyword_tag_stats) {
		print_stats(*get_keyword_tag_stats);
	}
	print_stats(ensure_stats);
	print_stats(ensure_vr_stats);
	print_stats(ensure_tag_stats);
	print_stats(ensure_tag_vr_stats);
	if (ensure_keyword_tag_stats) {
		print_stats(*ensure_keyword_tag_stats);
	}
	print_stats(set_stats);
	print_stats(set_vr_stats);
	print_stats(set_tag_stats);
	print_stats(set_tag_vr_stats);
	if (set_keyword_tag_stats) {
		print_stats(*set_keyword_tag_stats);
	}
	std::cout << "\n";
}

} // namespace

int main(int argc, char** argv) {
	Options options;
	if (!parse_args(argc, argv, options)) {
		return 1;
	}

	std::cout << "Nested tag-path access benchmark\n";
	std::cout << "iterations=" << options.iterations
	          << " warmup=" << options.warmup
	          << " inner_iterations=" << options.inner_iterations << "\n\n";

	for (const auto& path_case : kPathCases) {
		run_case(path_case, options);
	}

	std::cout << "sink=" << g_sink << "\n";
	return 0;
}
