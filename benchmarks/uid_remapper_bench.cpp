#include "dicom.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using clock_type = std::chrono::steady_clock;

struct Options {
	int iterations = 12;
	int warmup = 3;
	int hit_inner_iterations = 200000;
	int miss_inner_iterations = 2000;
	int replay_entry_count = 20000;
};

struct BenchStats {
	std::string name;
	double mean_ns = 0.0;
	double median_ns = 0.0;
	double min_ns = 0.0;
	double max_ns = 0.0;
};

volatile std::uint64_t g_sink = 0;

void print_usage(const char* prog) {
	std::cout << "Usage: " << prog
	          << " [--iterations <n>] [--warmup <n>]"
	          << " [--hit-inner-iterations <n>]"
	          << " [--miss-inner-iterations <n>]"
	          << " [--replay-entry-count <n>]\n";
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
		auto parse_next = [&](int& target, const char* label) {
			if (i + 1 >= argc || !parse_int_arg(argv[++i], target)) {
				std::cerr << "Invalid " << label << " value\n";
				return false;
			}
			return true;
		};
		if (arg == "--iterations") {
			if (!parse_next(options.iterations, "--iterations")) return false;
			continue;
		}
		if (arg == "--warmup") {
			if (!parse_next(options.warmup, "--warmup")) return false;
			continue;
		}
		if (arg == "--hit-inner-iterations") {
			if (!parse_next(options.hit_inner_iterations, "--hit-inner-iterations")) return false;
			continue;
		}
		if (arg == "--miss-inner-iterations") {
			if (!parse_next(options.miss_inner_iterations, "--miss-inner-iterations")) return false;
			continue;
		}
		if (arg == "--replay-entry-count") {
			if (!parse_next(options.replay_entry_count, "--replay-entry-count")) return false;
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
		fn(i);
	}
	const auto elapsed =
	    std::chrono::duration_cast<std::chrono::nanoseconds>(clock_type::now() - start)
	        .count();
	return static_cast<double>(elapsed) / static_cast<double>(inner_iterations);
}

template <typename Fn>
BenchStats run_bench(
    std::string name, const Options& options, int inner_iterations, Fn&& fn) {
	std::vector<double> samples;
	samples.reserve(static_cast<std::size_t>(options.iterations));

	for (int i = 0; i < options.warmup; ++i) {
		(void)measure_ns_per_op(fn, inner_iterations);
	}
	for (int i = 0; i < options.iterations; ++i) {
		samples.push_back(measure_ns_per_op(fn, inner_iterations));
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

std::string make_source_uid(int value) {
	return "1.2.840.113619.2.55.3.604688435.123." + std::to_string(value);
}

std::string make_target_uid(int value) {
	return "1.2.826.0.1.3680043.10.543." + std::to_string(value);
}

void remove_tree_no_throw(const std::filesystem::path& path) {
	std::error_code ec;
	std::filesystem::remove_all(path, ec);
}

}  // namespace

int main(int argc, char** argv) {
	Options options;
	if (!parse_args(argc, argv, options)) {
		return 1;
	}

	const auto temp_root =
	    std::filesystem::temp_directory_path() / "dicomsdl_uid_remapper_bench";
	remove_tree_no_throw(temp_root);
	std::filesystem::create_directories(temp_root);

	const auto hit_journal = temp_root / "hit.tsv";
	const auto miss_journal = temp_root / "miss.tsv";
	const auto miss_no_flush_journal = temp_root / "miss_no_flush.tsv";
	const auto replay_journal = temp_root / "replay.tsv";

	auto hit_remapper = dicom::UidRemapper::in_memory(hit_journal);
	auto miss_remapper = dicom::UidRemapper::in_memory(miss_journal);
	auto miss_no_flush_remapper =
	    dicom::UidRemapper::in_memory(
	        miss_no_flush_journal, dicom::uid::uid_prefix(), false);
	const auto hit_source = make_source_uid(1000);
	const auto hit_target = hit_remapper.map_uid(hit_source);
	int miss_uid_counter = 1000000;
	int miss_no_flush_uid_counter = 2000000;
	(void)hit_target;

	std::cout << "UidRemapper benchmark\n";
	std::cout << "iterations=" << options.iterations
	          << " warmup=" << options.warmup
	          << " hit_inner_iterations=" << options.hit_inner_iterations
	          << " miss_inner_iterations=" << options.miss_inner_iterations
	          << " replay_entry_count=" << options.replay_entry_count << "\n\n";

	constexpr std::string_view bench_root = "1.2.826.0.1.3680043.10.543";
	constexpr std::string_view bench_key =
	    "1.2.840.113619.2.55.3.604688435.123.45678901234567890123456789";

	print_stats(run_bench(
	    "uid::detail::validated_root",
	    options,
	    options.hit_inner_iterations,
	    [&](int) {
		    const auto uid =
		        dicom::uid::detail::try_generate_uid_validated_root(bench_root);
		    g_sink ^= static_cast<std::uint64_t>(uid->value().size());
	    }));

	print_stats(run_bench(
	    "uid::detail::validated_root_from",
	    options,
	    options.hit_inner_iterations,
	    [&](int) {
		    const auto uid = dicom::uid::detail::try_generate_uid_validated_root_from(
		        bench_root, bench_key);
		    g_sink ^= static_cast<std::uint64_t>(uid->value().size());
	    }));

	print_stats(run_bench(
	    "generate_uid(default root)",
	    options,
	    options.hit_inner_iterations,
	    [&](int) {
		    const auto uid = dicom::uid::generate_uid();
		    g_sink ^= static_cast<std::uint64_t>(uid.value().size());
	    }));

	print_stats(run_bench(
	    "UidRemapper hit",
	    options,
	    options.hit_inner_iterations,
	    [&](int) {
		    const auto mapped = hit_remapper.map_uid(hit_source);
		    g_sink ^= static_cast<std::uint64_t>(mapped.size());
	    }));

	print_stats(run_bench(
	    "UidRemapper miss + append+flush",
	    options,
	    options.miss_inner_iterations,
	    [&](int) {
		    const auto mapped = miss_remapper.map_uid(make_source_uid(miss_uid_counter++));
		    g_sink ^= static_cast<std::uint64_t>(mapped.size());
	    }));

	print_stats(run_bench(
	    "UidRemapper miss (no flush)",
	    options,
	    options.miss_inner_iterations,
	    [&](int) {
		    const auto mapped =
		        miss_no_flush_remapper.map_uid(make_source_uid(miss_no_flush_uid_counter++));
		    g_sink ^= static_cast<std::uint64_t>(mapped.size());
	    }));

	{
		const auto raw_journal = temp_root / "raw_no_flush.tsv";
		std::ofstream output(raw_journal, std::ios::binary | std::ios::trunc);
		if (!output.is_open()) {
			std::cerr << "Failed to open raw journal benchmark file\n";
			remove_tree_no_throw(temp_root);
			return 1;
		}
		int raw_counter = 3000000;
		print_stats(run_bench(
		    "Raw journal append (no flush)",
		    options,
		    options.miss_inner_iterations,
		    [&](int) {
			    const auto source_uid = make_source_uid(raw_counter);
			    const auto target_uid = make_target_uid(raw_counter);
			    ++raw_counter;
			    output << source_uid << '\t' << target_uid << '\n';
			    g_sink ^= static_cast<std::uint64_t>(source_uid.size() + target_uid.size());
		    }));
		output.flush();
	}

	{
		const auto raw_journal = temp_root / "raw_flush.tsv";
		std::ofstream output(raw_journal, std::ios::binary | std::ios::trunc);
		if (!output.is_open()) {
			std::cerr << "Failed to open raw flush benchmark file\n";
			remove_tree_no_throw(temp_root);
			return 1;
		}
		int raw_counter = 4000000;
		print_stats(run_bench(
		    "Raw journal append + flush",
		    options,
		    options.miss_inner_iterations,
		    [&](int) {
			    const auto source_uid = make_source_uid(raw_counter);
			    const auto target_uid = make_target_uid(raw_counter);
			    ++raw_counter;
			    output << source_uid << '\t' << target_uid << '\n';
			    output.flush();
			    g_sink ^= static_cast<std::uint64_t>(source_uid.size() + target_uid.size());
		    }));
	}

	{
		auto replay_seed = dicom::UidRemapper::in_memory(replay_journal);
		for (int i = 0; i < options.replay_entry_count; ++i) {
			const auto mapped = replay_seed.map_uid(make_source_uid(2000000 + i));
			g_sink ^= static_cast<std::uint64_t>(mapped.size());
		}
	}

	print_stats(run_bench(
	    "UidRemapper open + replay",
	    options,
	    1,
	    [&](int) {
		    auto remapper = dicom::UidRemapper::in_memory(replay_journal);
		    const auto mapped = remapper.map_uid(make_source_uid(2000000));
		    g_sink ^= static_cast<std::uint64_t>(mapped.size());
	    }));

	std::cout << "\nReplay sweep:\n";
	for (const int count : {1000, 5000, 20000, 50000}) {
		const auto sweep_journal =
		    temp_root / ("replay_" + std::to_string(count) + ".tsv");
		{
			auto seeded = dicom::UidRemapper::in_memory(sweep_journal);
			for (int i = 0; i < count; ++i) {
				const auto mapped = seeded.map_uid(make_source_uid(5000000 + i));
				g_sink ^= static_cast<std::uint64_t>(mapped.size());
			}
		}
		print_stats(run_bench(
		    "open + replay (" + std::to_string(count) + " entries)",
		    options,
		    1,
		    [&](int) {
			    auto remapper = dicom::UidRemapper::in_memory(sweep_journal);
			    const auto mapped = remapper.map_uid(make_source_uid(5000000));
			    g_sink ^= static_cast<std::uint64_t>(mapped.size());
		    }));
	}

	std::cout << "\n"
	          << "sink=" << g_sink << "\n";

	remove_tree_no_throw(temp_root);
	return 0;
}
