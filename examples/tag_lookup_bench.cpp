#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string_view>
#include <vector>

#include "dataelement_registry.hpp"
#include "keyword_lookup.hpp"

namespace {

std::vector<std::uint64_t> g_cache_sweeper(8 * 1024 * 1024, 0);

void flush_caches() {
    for (std::size_t i = 0; i < g_cache_sweeper.size(); ++i) {
        g_cache_sweeper[i] = g_cache_sweeper[i] * 1664525u + 1013904223u;
    }
}

bool parse_tag_digits(char ch, std::uint32_t& value, int& digits) {
    unsigned digit = 0;
    if (ch >= '0' && ch <= '9') {
        digit = static_cast<unsigned>(ch - '0');
    } else if (ch >= 'A' && ch <= 'F') {
        digit = 10u + static_cast<unsigned>(ch - 'A');
    } else if (ch >= 'a' && ch <= 'f') {
        digit = 10u + static_cast<unsigned>(ch - 'a');
    } else {
        return false;
    }
    value = (value << 4) | digit;
    ++digits;
    return true;
}

bool parse_tag(std::string_view tag, std::uint32_t& value_out) {
    std::uint32_t value = 0;
    int digits = 0;
    for (char ch : tag) {
        if (ch == '(' || ch == ')' || ch == ',' || ch == ' ') {
            continue;
        }
        if (!parse_tag_digits(ch, value, digits)) {
            return false;
        }
    }
    if (digits != 8) {
        return false;
    }
    value_out = value;
    return true;
}

std::vector<std::uint32_t> collect_tags() {
    std::vector<std::uint32_t> tags;
    tags.reserve(dicom::kDataElementRegistry.size());
    for (const auto& entry : dicom::kDataElementRegistry) {
        std::uint32_t value = 0;
        if (parse_tag(entry.tag, value)) {
            tags.push_back(value);
        }
    }
    return tags;
}

template <typename Fn>
void run_benchmark(const char* name, Fn&& fn, const std::vector<std::uint32_t>& tags, std::size_t iterations, bool flush_each_iteration) {
    using namespace std::chrono;
    std::size_t hits = 0;
    const auto start = steady_clock::now();
    for (std::size_t iter = 0; iter < iterations; ++iter) {
        if (flush_each_iteration) {
            flush_caches();
        }
        for (const auto tag : tags) {
            if (fn(tag)) {
                ++hits;
            }
        }
    }
    const auto elapsed = duration_cast<milliseconds>(steady_clock::now() - start).count();
    std::cout << name << ": " << elapsed << " ms (hits=" << hits << ")\n";
}

}  // namespace

int main() {
    const auto tags = collect_tags();
    if (tags.empty()) {
        std::cerr << "No valid tags collected; aborting benchmark.\n";
        return 1;
    }

    constexpr std::size_t iterations = 100;
    std::cout << "Warm cache runs\n";
    run_benchmark(
        "  Linear scan",
        [](std::uint32_t tag) {
            return dicom::lookup::tag_to_registry_index_linear(tag) != std::numeric_limits<std::uint16_t>::max();
        },
        tags,
        iterations,
        false);
    run_benchmark(
        "  Binary search",
        [](std::uint32_t tag) {
            return dicom::lookup::tag_to_registry_index_binary(tag) != std::numeric_limits<std::uint16_t>::max();
        },
        tags,
        iterations,
        false);
    run_benchmark(
        "  CHD (hash)",
        [](std::uint32_t tag) {
            return dicom::lookup::tag_to_registry_index_chd(tag) != std::numeric_limits<std::uint16_t>::max();
        },
        tags,
        iterations,
        false);

    std::cout << "Cold-ish cache runs (flush before every iteration)\n";
    run_benchmark(
        "  Linear scan",
        [](std::uint32_t tag) {
            return dicom::lookup::tag_to_registry_index_linear(tag) != std::numeric_limits<std::uint16_t>::max();
        },
        tags,
        iterations,
        true);
    run_benchmark(
        "  Binary search",
        [](std::uint32_t tag) {
            return dicom::lookup::tag_to_registry_index_binary(tag) != std::numeric_limits<std::uint16_t>::max();
        },
        tags,
        iterations,
        true);
    run_benchmark(
        "  CHD (hash)",
        [](std::uint32_t tag) {
            return dicom::lookup::tag_to_registry_index_chd(tag) != std::numeric_limits<std::uint16_t>::max();
        },
        tags,
        iterations,
        true);

    return 0;
}
