#include "dicom.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using clock_type = std::chrono::steady_clock;

struct Options {
	int iterations = 20;
	int warmup = 5;
	int inner_iterations = 500000;
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
	std::cout << std::left << std::setw(38) << stats.name << " mean=" << std::fixed
	          << std::setprecision(2) << stats.mean_ns << " ns/op"
	          << " median=" << stats.median_ns << " min=" << stats.min_ns
	          << " max=" << stats.max_ns << "\n";
}

std::array<std::string_view, 32> build_uid_keywords() {
	std::array<std::string_view, 32> keywords{};
	std::size_t out = 0;
	for (const auto& entry : dicom::kUidRegistry) {
		if (entry.keyword.empty()) {
			continue;
		}
		keywords[out++] = entry.keyword;
		if (out == keywords.size()) {
			break;
		}
	}
	if (out != keywords.size()) {
		throw std::runtime_error("Not enough UID keywords for benchmark");
	}
	return keywords;
}

std::array<std::string_view, 32> build_uid_values() {
	std::array<std::string_view, 32> values{};
	std::size_t out = 0;
	for (const auto& entry : dicom::kUidRegistry) {
		values[out++] = entry.value;
		if (out == values.size()) {
			break;
		}
	}
	if (out != values.size()) {
		throw std::runtime_error("Not enough UID values for benchmark");
	}
	return values;
}

template <std::size_t Count, typename Fn>
void run_cycle_group(std::string_view prefix, const Options& options, Fn&& fn) {
	static_assert(Count > 0);
	auto idx = std::size_t{0};
	const auto stats = run_bench(std::string(prefix), options, [&] {
		fn(idx & (Count - 1));
		++idx;
	});
	print_stats(stats);
}

template <std::size_t CacheSize>
std::uint16_t keyword_cached_bench(std::string_view keyword) {
	static_assert(CacheSize > 0);
	thread_local std::array<dicom::uid_lookup::detail::RuntimeUidCacheEntry, CacheSize> cache{};
	thread_local std::size_t next_slot = 0;
	for (const auto& entry : cache) {
		if (dicom::uid_lookup::detail::runtime_cache_hit(entry, keyword)) {
			return entry.index;
		}
	}
	const auto index = dicom::uid_lookup::detail::uid_index_from_keyword_runtime_uncached(keyword);
	dicom::uid_lookup::detail::runtime_cache_store(cache[next_slot], keyword, index);
	next_slot = (next_slot + 1) % cache.size();
	return index;
}

template <std::size_t CacheSize>
std::uint16_t value_cached_bench(std::string_view value) {
	static_assert(CacheSize > 0);
	thread_local std::array<dicom::uid_lookup::detail::RuntimeUidCacheEntry, CacheSize> cache{};
	thread_local std::size_t next_slot = 0;
	for (const auto& entry : cache) {
		if (dicom::uid_lookup::detail::runtime_cache_hit(entry, value)) {
			return entry.index;
		}
	}
	const auto index = dicom::uid_lookup::detail::uid_index_from_value_runtime_uncached(value);
	dicom::uid_lookup::detail::runtime_cache_store(cache[next_slot], value, index);
	next_slot = (next_slot + 1) % cache.size();
	return index;
}

}  // namespace

int main(int argc, char** argv) {
	Options options;
	if (!parse_args(argc, argv, options)) {
		return 1;
	}

	const auto keywords = build_uid_keywords();
	const auto values = build_uid_values();

	std::cout << "UID lookup benchmark\n";
	std::cout << "iterations=" << options.iterations
	          << " warmup=" << options.warmup
	          << " inner_iterations=" << options.inner_iterations << "\n";
	std::cout << "keywords=" << keywords.size() << " values=" << values.size() << "\n\n";

	print_stats(run_bench("uid_index_from_keyword hot_single", options, [&] {
		g_sink += dicom::uid_lookup::uid_index_from_keyword(keywords[0]);
	}));
	run_cycle_group<4>("uid_index_from_keyword cycle4", options, [&](std::size_t i) {
		g_sink += dicom::uid_lookup::uid_index_from_keyword(keywords[i]);
	});
	run_cycle_group<32>("uid_index_from_keyword cycle32", options, [&](std::size_t i) {
		g_sink += dicom::uid_lookup::uid_index_from_keyword(keywords[i]);
	});
	std::cout << "\n";

	print_stats(run_bench("uid_index_from_keyword_chd hot_single", options, [&] {
		g_sink += dicom::uid_lookup::uid_index_from_keyword_chd(keywords[0]);
	}));
	run_cycle_group<32>("uid_index_from_keyword_chd cycle32", options, [&](std::size_t i) {
		g_sink += dicom::uid_lookup::uid_index_from_keyword_chd(keywords[i]);
	});
	print_stats(run_bench("uid_index_from_keyword_uncached hot_single", options, [&] {
		g_sink += dicom::uid_lookup::detail::uid_index_from_keyword_runtime_uncached(keywords[0]);
	}));
	run_cycle_group<32>(
	    "uid_index_from_keyword_uncached cycle32", options, [&](std::size_t i) {
		    g_sink += dicom::uid_lookup::detail::uid_index_from_keyword_runtime_uncached(
		        keywords[i]);
	    });
	print_stats(run_bench("uid_index_from_keyword_cached hot_single", options, [&] {
		g_sink += dicom::uid_lookup::detail::uid_index_from_keyword_runtime_cached(keywords[0]);
	}));
	run_cycle_group<32>(
	    "uid_index_from_keyword_cached cycle32", options, [&](std::size_t i) {
		    g_sink += dicom::uid_lookup::detail::uid_index_from_keyword_runtime_cached(
		        keywords[i]);
	    });
	print_stats(run_bench("uid_index_from_keyword_cache4 hot_single", options, [&] {
		g_sink += keyword_cached_bench<4>(keywords[0]);
	}));
	run_cycle_group<4>(
	    "uid_index_from_keyword_cache4 cycle4", options, [&](std::size_t i) {
		    g_sink += keyword_cached_bench<4>(keywords[i]);
	    });
	run_cycle_group<32>(
	    "uid_index_from_keyword_cache4 cycle32", options, [&](std::size_t i) {
		    g_sink += keyword_cached_bench<4>(keywords[i]);
	    });
	print_stats(run_bench("uid_index_from_keyword_cache8 hot_single", options, [&] {
		g_sink += keyword_cached_bench<8>(keywords[0]);
	}));
	run_cycle_group<4>(
	    "uid_index_from_keyword_cache8 cycle4", options, [&](std::size_t i) {
		    g_sink += keyword_cached_bench<8>(keywords[i]);
	    });
	run_cycle_group<32>(
	    "uid_index_from_keyword_cache8 cycle32", options, [&](std::size_t i) {
		    g_sink += keyword_cached_bench<8>(keywords[i]);
	    });
	std::cout << "\n";

	print_stats(run_bench("uid_index_from_value hot_single", options, [&] {
		g_sink += dicom::uid_lookup::uid_index_from_value(values[0]);
	}));
	run_cycle_group<4>("uid_index_from_value cycle4", options, [&](std::size_t i) {
		g_sink += dicom::uid_lookup::uid_index_from_value(values[i]);
	});
	run_cycle_group<32>("uid_index_from_value cycle32", options, [&](std::size_t i) {
		g_sink += dicom::uid_lookup::uid_index_from_value(values[i]);
	});
	std::cout << "\n";

	print_stats(run_bench("uid_index_from_value_chd hot_single", options, [&] {
		g_sink += dicom::uid_lookup::uid_index_from_value_chd(values[0]);
	}));
	run_cycle_group<32>("uid_index_from_value_chd cycle32", options, [&](std::size_t i) {
		g_sink += dicom::uid_lookup::uid_index_from_value_chd(values[i]);
	});
	print_stats(run_bench("uid_index_from_value_uncached hot_single", options, [&] {
		g_sink += dicom::uid_lookup::detail::uid_index_from_value_runtime_uncached(values[0]);
	}));
	run_cycle_group<32>(
	    "uid_index_from_value_uncached cycle32", options, [&](std::size_t i) {
		    g_sink += dicom::uid_lookup::detail::uid_index_from_value_runtime_uncached(
		        values[i]);
	    });
	print_stats(run_bench("uid_index_from_value_cached hot_single", options, [&] {
		g_sink += dicom::uid_lookup::detail::uid_index_from_value_runtime_cached(values[0]);
	}));
	run_cycle_group<32>(
	    "uid_index_from_value_cached cycle32", options, [&](std::size_t i) {
		    g_sink += dicom::uid_lookup::detail::uid_index_from_value_runtime_cached(values[i]);
	    });
	print_stats(run_bench("uid_index_from_value_cache4 hot_single", options, [&] {
		g_sink += value_cached_bench<4>(values[0]);
	}));
	run_cycle_group<4>("uid_index_from_value_cache4 cycle4", options, [&](std::size_t i) {
		g_sink += value_cached_bench<4>(values[i]);
	});
	run_cycle_group<32>("uid_index_from_value_cache4 cycle32", options, [&](std::size_t i) {
		g_sink += value_cached_bench<4>(values[i]);
	});
	print_stats(run_bench("uid_index_from_value_cache8 hot_single", options, [&] {
		g_sink += value_cached_bench<8>(values[0]);
	}));
	run_cycle_group<4>("uid_index_from_value_cache8 cycle4", options, [&](std::size_t i) {
		g_sink += value_cached_bench<8>(values[i]);
	});
	run_cycle_group<32>("uid_index_from_value_cache8 cycle32", options, [&](std::size_t i) {
		g_sink += value_cached_bench<8>(values[i]);
	});
	std::cout << "\n";

	print_stats(run_bench("uid_index_from_text(keyword) hot_single", options, [&] {
		g_sink += dicom::uid_lookup::uid_index_from_text(keywords[0]);
	}));
	run_cycle_group<4>("uid_index_from_text(keyword) cycle4", options, [&](std::size_t i) {
		g_sink += dicom::uid_lookup::uid_index_from_text(keywords[i]);
	});
	run_cycle_group<32>("uid_index_from_text(keyword) cycle32", options, [&](std::size_t i) {
		g_sink += dicom::uid_lookup::uid_index_from_text(keywords[i]);
	});
	std::cout << "\n";

	print_stats(run_bench("uid_index_from_text(value) hot_single", options, [&] {
		g_sink += dicom::uid_lookup::uid_index_from_text(values[0]);
	}));
	run_cycle_group<4>("uid_index_from_text(value) cycle4", options, [&](std::size_t i) {
		g_sink += dicom::uid_lookup::uid_index_from_text(values[i]);
	});
	run_cycle_group<32>("uid_index_from_text(value) cycle32", options, [&](std::size_t i) {
		g_sink += dicom::uid_lookup::uid_index_from_text(values[i]);
	});
	std::cout << "\n";

	print_stats(run_bench("uid::lookup(keyword) hot_single", options, [&] {
		const auto wk = dicom::uid::lookup(keywords[0]);
		g_sink += wk ? wk->raw_index() : 0;
	}));
	run_cycle_group<4>("uid::lookup(keyword) cycle4", options, [&](std::size_t i) {
		const auto wk = dicom::uid::lookup(keywords[i]);
		g_sink += wk ? wk->raw_index() : 0;
	});
	run_cycle_group<32>("uid::lookup(keyword) cycle32", options, [&](std::size_t i) {
		const auto wk = dicom::uid::lookup(keywords[i]);
		g_sink += wk ? wk->raw_index() : 0;
	});
	std::cout << "\n";

	print_stats(run_bench("uid::lookup(value) hot_single", options, [&] {
		const auto wk = dicom::uid::lookup(values[0]);
		g_sink += wk ? wk->raw_index() : 0;
	}));
	run_cycle_group<4>("uid::lookup(value) cycle4", options, [&](std::size_t i) {
		const auto wk = dicom::uid::lookup(values[i]);
		g_sink += wk ? wk->raw_index() : 0;
	});
	run_cycle_group<32>("uid::lookup(value) cycle32", options, [&](std::size_t i) {
		const auto wk = dicom::uid::lookup(values[i]);
		g_sink += wk ? wk->raw_index() : 0;
	});

	std::cout << "\nsink=" << g_sink << "\n";
	return 0;
}
