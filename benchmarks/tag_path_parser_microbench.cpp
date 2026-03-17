#include "dicom.h"

#include <algorithm>
#include <array>
#include <charconv>
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

struct Options {
	int iterations = 20;
	int warmup = 5;
	int inner_iterations = 300000;
};

struct BenchStats {
	std::string name;
	double mean_ns = 0.0;
	double median_ns = 0.0;
	double min_ns = 0.0;
	double max_ns = 0.0;
};

struct PathCase {
	const char* name;
	const char* path;
};

struct TagPathStep {
	std::string_view tag_token{};
	bool has_child = false;
	std::string_view child_index_token{};
};

constexpr std::array<PathCase, 3> kPathCases = {{
    {"depth0", "ReferencedSOPInstanceUID"},
    {"depth2",
        "ReferencedStudySequence.0.ReferencedSeriesSequence.0.ReferencedSOPInstanceUID"},
    {"depth4",
        "ReferencedStudySequence.0.ReferencedSeriesSequence.0.ReferencedInstanceSequence.0."
        "SourceImageSequence.0.ReferencedSOPInstanceUID"},
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
BenchStats run_bench(std::string name, const Options& options, Fn&& fn) {
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
	std::cout << std::left << std::setw(42) << stats.name << " mean=" << std::fixed
	          << std::setprecision(2) << stats.mean_ns << " ns/op"
	          << " median=" << stats.median_ns << " min=" << stats.min_ns
	          << " max=" << stats.max_ns << "\n";
}

std::string_view trim_ws(std::string_view text) {
	const auto first = text.find_first_not_of(" \t\r\n");
	if (first == std::string_view::npos) {
		return {};
	}
	const auto last = text.find_last_not_of(" \t\r\n");
	return text.substr(first, last - first + 1);
}

bool next_tag_path_step_old(std::string_view& remaining, TagPathStep& step) {
	remaining = trim_ws(remaining);
	if (remaining.empty()) {
		return false;
	}

	const auto dot_pos = remaining.find('.');
	step.tag_token = remaining.substr(0, dot_pos);
	step.has_child = (dot_pos != std::string_view::npos);
	if (!step.has_child) {
		step.child_index_token = {};
		remaining = {};
		return true;
	}

	remaining = remaining.substr(dot_pos + 1);
	const auto next_dot = remaining.find('.');
	step.child_index_token = remaining.substr(0, next_dot);
	remaining =
	    (next_dot == std::string_view::npos) ? std::string_view{} : remaining.substr(next_dot + 1);
	return true;
}

std::optional<dicom::Tag> locate_tag_path_token_bench(std::string_view token) {
	token = trim_ws(token);
	try {
		return dicom::Tag(token);
	} catch (const std::invalid_argument&) {
		return std::nullopt;
	}
}

std::optional<dicom::Tag> locate_tag_path_token_uncached_bench(std::string_view token) {
	token = trim_ws(token);
	if (const auto* entry = dicom::lookup::keyword_to_entry_chd(token)) {
		return dicom::Tag::from_value(entry->tag_value);
	}
	return std::nullopt;
}

const dicom::DataElement& missing_element(const dicom::DataSet& dataset) {
	return dataset.get_dataelement(dicom::Tag(0xFFFFu, 0xFFFFu));
}

std::size_t parse_index_old(std::string_view token) {
	token = trim_ws(token);
	return static_cast<std::size_t>(std::stoul(std::string(token)));
}

std::size_t parse_index_new(std::string_view token) {
	token = trim_ws(token);
	std::size_t value = 0;
	const auto* begin = token.data();
	const auto* end = token.data() + token.size();
	const auto result = std::from_chars(begin, end, value, 10);
	if (result.ec != std::errc() || result.ptr != end) {
		throw std::runtime_error("failed to parse sequence index");
	}
	return value;
}

template <typename LocateTokenFn, typename ParseIndexFn>
const dicom::DataElement& get_dataelement_by_path_generic(
    const dicom::DataSet& dataset, std::string_view tag_path, LocateTokenFn&& locate_token,
    ParseIndexFn&& parse_index) {
	const dicom::DataSet* current = &dataset;
	std::string_view remaining = trim_ws(tag_path);
	TagPathStep step;
	while (next_tag_path_step_old(remaining, step)) {
		const auto tag = locate_token(step.tag_token);
		if (!tag) {
			return missing_element(dataset);
		}

		const auto& element = current->get_dataelement(*tag);
		if (!step.has_child || element.is_missing()) {
			return element;
		}
		if (!element.vr().is_sequence()) {
			throw std::runtime_error("intermediate path element is not a sequence");
		}

		const auto* seq = element.as_sequence();
		if (!seq) {
			return missing_element(dataset);
		}

		const auto index = parse_index(step.child_index_token);
		current = seq->get_dataset(index);
		if (!current) {
			return missing_element(dataset);
		}
	}
	return missing_element(dataset);
}

template <typename LocateTokenFn>
const dicom::DataElement& get_dataelement_by_path_new_like(
    const dicom::DataSet& dataset, std::string_view tag_path, LocateTokenFn&& locate_token) {
	tag_path = trim_ws(tag_path);
	if (tag_path.empty()) {
		return missing_element(dataset);
	}
	if (tag_path.find('.') == std::string_view::npos) {
		const auto tag = locate_token(tag_path);
		if (!tag) {
			return missing_element(dataset);
		}
		return dataset.get_dataelement(*tag);
	}
	return get_dataelement_by_path_generic(
	    dataset, tag_path, std::forward<LocateTokenFn>(locate_token), parse_index_new);
}

void populate_nested_dataset(dicom::DataSet& dataset, std::string_view path) {
	auto& element = dataset.ensure_dataelement(path, dicom::VR::UI);
	element.from_string_view("1.2.840.10008.5.1.4.1.1.2");
}

void run_case(const PathCase& path_case, const Options& options) {
	dicom::DataSet dataset;
	populate_nested_dataset(dataset, path_case.path);

	const std::string prefix = std::string(path_case.name) + " " + path_case.path + " ";
	const bool is_flat = std::string_view(path_case.path).find('.') == std::string_view::npos;

	if (is_flat) {
		print_stats(run_bench(prefix + "old_generic_loop", options, [&] {
			const auto& element = get_dataelement_by_path_generic(
			    dataset, path_case.path, locate_tag_path_token_bench, parse_index_new);
			g_sink += static_cast<std::uint64_t>(element.tag().value());
		}));
		print_stats(run_bench(prefix + "uncached_old_generic_loop", options, [&] {
			const auto& element = get_dataelement_by_path_generic(
			    dataset, path_case.path, locate_tag_path_token_uncached_bench, parse_index_new);
			g_sink += static_cast<std::uint64_t>(element.tag().value());
		}));
		print_stats(run_bench(prefix + "new_flat_fast_path", options, [&] {
			const auto& element =
			    get_dataelement_by_path_new_like(dataset, path_case.path, locate_tag_path_token_bench);
			g_sink += static_cast<std::uint64_t>(element.tag().value());
		}));
		print_stats(run_bench(prefix + "uncached_flat_fast_path", options, [&] {
			const auto& element = get_dataelement_by_path_new_like(
			    dataset, path_case.path, locate_tag_path_token_uncached_bench);
			g_sink += static_cast<std::uint64_t>(element.tag().value());
		}));
		print_stats(run_bench(prefix + "current_get_dataelement", options, [&] {
			const auto& element = dataset.get_dataelement(path_case.path);
			g_sink += static_cast<std::uint64_t>(element.tag().value());
		}));
		std::cout << "\n";
		return;
	}

	print_stats(run_bench(prefix + "old_stoul_parser", options, [&] {
		const auto& element = get_dataelement_by_path_generic(
		    dataset, path_case.path, locate_tag_path_token_bench, parse_index_old);
		g_sink += static_cast<std::uint64_t>(element.tag().value());
	}));
	print_stats(run_bench(prefix + "uncached_old_stoul_parser", options, [&] {
		const auto& element = get_dataelement_by_path_generic(
		    dataset, path_case.path, locate_tag_path_token_uncached_bench, parse_index_old);
		g_sink += static_cast<std::uint64_t>(element.tag().value());
	}));
	print_stats(run_bench(prefix + "new_from_chars_parser", options, [&] {
		const auto& element = get_dataelement_by_path_generic(
		    dataset, path_case.path, locate_tag_path_token_bench, parse_index_new);
		g_sink += static_cast<std::uint64_t>(element.tag().value());
	}));
	print_stats(run_bench(prefix + "uncached_from_chars_parser", options, [&] {
		const auto& element = get_dataelement_by_path_generic(
		    dataset, path_case.path, locate_tag_path_token_uncached_bench, parse_index_new);
		g_sink += static_cast<std::uint64_t>(element.tag().value());
	}));
	print_stats(run_bench(prefix + "current_get_dataelement", options, [&] {
		const auto& element = dataset.get_dataelement(path_case.path);
		g_sink += static_cast<std::uint64_t>(element.tag().value());
	}));
	std::cout << "\n";
}

} // namespace

int main(int argc, char** argv) {
	Options options;
	if (!parse_args(argc, argv, options)) {
		return 1;
	}

	std::cout << "Tag path parser microbenchmark\n";
	std::cout << "iterations=" << options.iterations
	          << " warmup=" << options.warmup
	          << " inner_iterations=" << options.inner_iterations << "\n\n";

	for (const auto& path_case : kPathCases) {
		run_case(path_case, options);
	}

	std::cout << "sink=" << g_sink << "\n";
	return 0;
}
