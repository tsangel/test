#include "dicom.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
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

constexpr std::array<std::string_view, 32> kKeywords = {{
    "PatientName",
    "PatientID",
    "PatientBirthDate",
    "PatientSex",
    "StudyDate",
    "StudyTime",
    "AccessionNumber",
    "Modality",
    "Manufacturer",
    "InstitutionName",
    "ReferringPhysicianName",
    "StudyDescription",
    "SeriesDescription",
    "Rows",
    "Columns",
    "BitsAllocated",
    "BitsStored",
    "HighBit",
    "PixelRepresentation",
    "SamplesPerPixel",
    "PhotometricInterpretation",
    "WindowCenter",
    "WindowWidth",
    "RescaleIntercept",
    "RescaleSlope",
    "SOPClassUID",
    "SOPInstanceUID",
    "StudyInstanceUID",
    "SeriesInstanceUID",
    "FrameOfReferenceUID",
    "ImageType",
    "ImagePositionPatient",
}};

constexpr std::string_view kNumericTagPacked = "00100010";
constexpr std::string_view kNumericTagFormatted = "(0010,0010)";

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

std::array<dicom::Tag, kKeywords.size()> build_tags() {
	std::array<dicom::Tag, kKeywords.size()> tags{};
	for (std::size_t i = 0; i < kKeywords.size(); ++i) {
		tags[i] = dicom::Tag(kKeywords[i]);
	}
	return tags;
}

void populate_dataset(
    dicom::DataSet& dataset, const std::array<dicom::Tag, kKeywords.size()>& tags) {
	for (const auto tag : tags) {
		dataset.ensure_dataelement(tag, dicom::VR::LO).from_string_view("x");
	}
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
const dicom::DataElementEntry* keyword_cached_bench(std::string_view keyword) {
	static_assert(CacheSize > 0);
	thread_local std::array<dicom::lookup::detail::RuntimeKeywordCacheEntry, CacheSize> cache{};
	const auto hash = dicom::lookup::detail::runtime_keyword_hash64(
	    keyword, dicom::lookup::kPerfectHashSeed);
	const auto slot_index = static_cast<std::size_t>(hash) & (CacheSize - 1);
	const auto& slot = cache[slot_index];
	if (slot.entry && slot.hash == hash &&
	    dicom::lookup::detail::runtime_sv_equal(slot.entry->keyword, keyword)) {
		return slot.entry;
	}

	const auto index = dicom::lookup::keyword_to_registry_index_runtime_with_hash(keyword, hash);
	const auto* entry = index == dicom::lookup::detail::kInvalidRegistryIndex
	                        ? nullptr
	                        : dicom::lookup::detail::entry_from_index(index);
	if (entry) {
		cache[slot_index] =
		    dicom::lookup::detail::RuntimeKeywordCacheEntry{hash, entry};
	}
	return entry;
}

} // namespace

int main(int argc, char** argv) {
	Options options;
	if (!parse_args(argc, argv, options)) {
		return 1;
	}

	const auto tags = build_tags();
	dicom::DataSet dataset;
	populate_dataset(dataset, tags);

	std::cout << "Keyword lookup benchmark\n";
	std::cout << "iterations=" << options.iterations
	          << " warmup=" << options.warmup
	          << " inner_iterations=" << options.inner_iterations << "\n";
	std::cout << "cache_size_hint=4 keywords=" << kKeywords.size() << "\n\n";

	print_stats(run_bench("Tag(keyword) hot_single", options, [&] {
		g_sink += static_cast<std::uint64_t>(dicom::Tag(kKeywords[0]).value());
	}));
	print_stats(run_bench("Tag(numeric_tag_packed) hot_single", options, [&] {
		g_sink += static_cast<std::uint64_t>(dicom::Tag(kNumericTagPacked).value());
	}));
	print_stats(run_bench("Tag(numeric_tag_formatted) hot_single", options, [&] {
		g_sink += static_cast<std::uint64_t>(dicom::Tag(kNumericTagFormatted).value());
	}));
	run_cycle_group<4>("Tag(keyword) cache_friendly_cycle4", options, [&](std::size_t i) {
		g_sink += static_cast<std::uint64_t>(dicom::Tag(kKeywords[i]).value());
	});
	run_cycle_group<32>("Tag(keyword) cache_cold_cycle32", options, [&](std::size_t i) {
		g_sink += static_cast<std::uint64_t>(dicom::Tag(kKeywords[i]).value());
	});
	std::cout << "\n";

	print_stats(run_bench("keyword_to_tag_vr hot_single", options, [&] {
		g_sink += static_cast<std::uint64_t>(
		    dicom::lookup::keyword_to_tag_vr(kKeywords[0]).first.value());
	}));
	run_cycle_group<4>(
	    "keyword_to_tag_vr cache_friendly_cycle4", options, [&](std::size_t i) {
		    g_sink += static_cast<std::uint64_t>(
		        dicom::lookup::keyword_to_tag_vr(kKeywords[i]).first.value());
	    });
	run_cycle_group<32>(
	    "keyword_to_tag_vr cache_cold_cycle32", options, [&](std::size_t i) {
		    g_sink += static_cast<std::uint64_t>(
		        dicom::lookup::keyword_to_tag_vr(kKeywords[i]).first.value());
	    });
	std::cout << "\n";

	print_stats(run_bench("keyword_to_entry_chd hot_single", options, [&] {
		const auto* entry = dicom::lookup::keyword_to_entry_chd(kKeywords[0]);
		g_sink += static_cast<std::uint64_t>(entry ? entry->tag_value : 0);
	}));
	run_cycle_group<32>(
	    "keyword_to_entry_chd cycle32", options, [&](std::size_t i) {
		    const auto* entry = dicom::lookup::keyword_to_entry_chd(kKeywords[i]);
		    g_sink += static_cast<std::uint64_t>(entry ? entry->tag_value : 0);
	    });

	print_stats(run_bench("keyword_to_entry_cached hot_single", options, [&] {
		const auto* entry = dicom::lookup::keyword_to_entry_runtime_cached(kKeywords[0]);
		g_sink += static_cast<std::uint64_t>(entry ? entry->tag_value : 0);
	}));
	run_cycle_group<32>(
	    "keyword_to_entry_cached cycle32", options, [&](std::size_t i) {
		    const auto* entry = dicom::lookup::keyword_to_entry_runtime_cached(kKeywords[i]);
		    g_sink += static_cast<std::uint64_t>(entry ? entry->tag_value : 0);
	    });
	print_stats(run_bench("keyword_to_entry_cache4 hot_single", options, [&] {
		const auto* entry = keyword_cached_bench<4>(kKeywords[0]);
		g_sink += static_cast<std::uint64_t>(entry ? entry->tag_value : 0);
	}));
	run_cycle_group<4>("keyword_to_entry_cache4 cycle4", options, [&](std::size_t i) {
		const auto* entry = keyword_cached_bench<4>(kKeywords[i]);
		g_sink += static_cast<std::uint64_t>(entry ? entry->tag_value : 0);
	});
	run_cycle_group<32>("keyword_to_entry_cache4 cycle32", options, [&](std::size_t i) {
		const auto* entry = keyword_cached_bench<4>(kKeywords[i]);
		g_sink += static_cast<std::uint64_t>(entry ? entry->tag_value : 0);
	});
	print_stats(run_bench("keyword_to_entry_cache8 hot_single", options, [&] {
		const auto* entry = keyword_cached_bench<8>(kKeywords[0]);
		g_sink += static_cast<std::uint64_t>(entry ? entry->tag_value : 0);
	}));
	run_cycle_group<4>("keyword_to_entry_cache8 cycle4", options, [&](std::size_t i) {
		const auto* entry = keyword_cached_bench<8>(kKeywords[i]);
		g_sink += static_cast<std::uint64_t>(entry ? entry->tag_value : 0);
	});
	run_cycle_group<32>("keyword_to_entry_cache8 cycle32", options, [&](std::size_t i) {
		const auto* entry = keyword_cached_bench<8>(kKeywords[i]);
		g_sink += static_cast<std::uint64_t>(entry ? entry->tag_value : 0);
	});
	print_stats(run_bench("keyword_to_entry_uncached hot_single", options, [&] {
		const auto* entry = dicom::lookup::keyword_to_entry_runtime_uncached(kKeywords[0]);
		g_sink += static_cast<std::uint64_t>(entry ? entry->tag_value : 0);
	}));
	run_cycle_group<32>(
	    "keyword_to_entry_uncached cycle32", options, [&](std::size_t i) {
		    const auto* entry =
		        dicom::lookup::keyword_to_entry_runtime_uncached(kKeywords[i]);
		    g_sink += static_cast<std::uint64_t>(entry ? entry->tag_value : 0);
	    });
	std::cout << "\n";

	print_stats(run_bench("DataSet::get_dataelement(keyword) hot_single", options, [&] {
		g_sink += static_cast<std::uint64_t>(dataset.get_dataelement(kKeywords[0]).tag().value());
	}));
	print_stats(run_bench("DataSet::get_dataelement(numeric_tag) hot_single", options, [&] {
		g_sink += static_cast<std::uint64_t>(
		    dataset.get_dataelement(kNumericTagFormatted).tag().value());
	}));
	run_cycle_group<4>(
	    "DataSet::get_dataelement(keyword) cache_friendly_cycle4", options,
	    [&](std::size_t i) {
		    g_sink += static_cast<std::uint64_t>(dataset.get_dataelement(kKeywords[i]).tag().value());
	    });
	run_cycle_group<32>(
	    "DataSet::get_dataelement(keyword) cache_cold_cycle32", options,
	    [&](std::size_t i) {
		    g_sink += static_cast<std::uint64_t>(dataset.get_dataelement(kKeywords[i]).tag().value());
	    });
	std::cout << "\n";

	print_stats(run_bench("tag_to_entry hot_single", options, [&] {
		const auto* entry = dicom::lookup::tag_to_entry(tags[0].value());
		g_sink += static_cast<std::uint64_t>(entry ? entry->tag_value : 0);
	}));
	run_cycle_group<4>("tag_to_entry cycle4", options, [&](std::size_t i) {
		const auto* entry = dicom::lookup::tag_to_entry(tags[i].value());
		g_sink += static_cast<std::uint64_t>(entry ? entry->tag_value : 0);
	});
	run_cycle_group<32>("tag_to_entry cycle32", options, [&](std::size_t i) {
		const auto* entry = dicom::lookup::tag_to_entry(tags[i].value());
		g_sink += static_cast<std::uint64_t>(entry ? entry->tag_value : 0);
	});
	std::cout << "\n";

	print_stats(run_bench("tag_to_keyword hot_single", options, [&] {
		g_sink += static_cast<std::uint64_t>(
		    dicom::lookup::tag_to_keyword(tags[0].value()).size());
	}));
	run_cycle_group<4>("tag_to_keyword cycle4", options, [&](std::size_t i) {
		g_sink += static_cast<std::uint64_t>(
		    dicom::lookup::tag_to_keyword(tags[i].value()).size());
	});
	run_cycle_group<32>("tag_to_keyword cycle32", options, [&](std::size_t i) {
		g_sink += static_cast<std::uint64_t>(
		    dicom::lookup::tag_to_keyword(tags[i].value()).size());
	});

	std::cout << "\nsink=" << g_sink << "\n";
	return 0;
}
