#include <chrono>
#include <cstdint>
#include <iostream>
#include <vector>

#include "dataelement_registry.hpp"
#include "keyword_lookup.hpp"
#include "keyword_lookup_tables.hpp"

namespace {

std::vector<std::uint64_t> g_cache_sweeper(8 * 1024 * 1024, 0);

void flush_caches() {
    for (std::size_t i = 0; i < g_cache_sweeper.size(); ++i) {
        g_cache_sweeper[i] = g_cache_sweeper[i] * 1664525u + 1013904223u;
    }
}

template <typename Fn>
void run_benchmark(const char* name, Fn&& fn, std::size_t iterations, bool flush_each_iteration) {
    using namespace std::chrono;
    std::size_t hits = 0;
    const auto start = steady_clock::now();
    for (std::size_t iter = 0; iter < iterations; ++iter) {
        if (flush_each_iteration) {
            flush_caches();
        }
        for (const auto index : dicom::lookup::kKeywordRegistryIndices) {
            const auto& entry = dicom::kDataElementRegistry[index];
            if (fn(entry.keyword)) {
                ++hits;
            }
        }
    }
    const auto elapsed = duration_cast<milliseconds>(steady_clock::now() - start).count();
    std::cout << name << ": " << elapsed << " ms (hits=" << hits << ")\n";
}

}  // namespace

int main() {
    constexpr std::size_t iterations = 100;
    std::cout << "Warm cache runs\n";
    run_benchmark(
        "  CHD (hash + G[x1])",
        [](std::string_view keyword) {
            return dicom::lookup::keyword_to_entry_chd(keyword) != nullptr;
        },
        iterations,
        false);
    run_benchmark(
        "  CHM (hash + G[x2])",
        [](std::string_view keyword) {
            return dicom::lookup::keyword_to_entry_chm(keyword) != nullptr;
        },
        iterations,
        false);
    run_benchmark(
        "  Binary search",
        [](std::string_view keyword) {
            return dicom::lookup::keyword_to_entry_binary(keyword) != nullptr;
        },
        iterations,
        false);

    std::cout << "Cold-ish cache runs (flush before every iteration)\n";
    run_benchmark(
        "  CHD (hash + G[x1])",
        [](std::string_view keyword) {
            return dicom::lookup::keyword_to_entry_chd(keyword) != nullptr;
        },
        iterations,
        true);
    run_benchmark(
        "  CHM (hash + G[x2])",
        [](std::string_view keyword) {
            return dicom::lookup::keyword_to_entry_chm(keyword) != nullptr;
        },
        iterations,
        true);
    run_benchmark(
        "  Binary search",
        [](std::string_view keyword) {
            return dicom::lookup::keyword_to_entry_binary(keyword) != nullptr;
        },
        iterations,
        true);
    return 0;
}
