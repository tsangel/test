#include "dicom.h"

#include <array>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::array<std::size_t, 4> kLookupElementCounts = {100, 300, 500, 1000};
enum class LookupPattern {
	hit_all,
	hit_last,
	miss_after_end
};

constexpr std::array<LookupPattern, 3> kLookupPatterns = {
    LookupPattern::hit_all,
    LookupPattern::hit_last,
    LookupPattern::miss_after_end,
};

struct Options {
	std::size_t elements = 32768;
	int iterations = 200;
	int warmup = 20;
};

struct BenchStats {
	std::string name;
	double mean_ns = 0.0;
	double median_ns = 0.0;
	double min_ns = 0.0;
	double max_ns = 0.0;
	std::size_t front_pointer_moves = 0;
};

struct LookupBenchStats {
	std::string name;
	std::string pattern;
	std::size_t elements = 0;
	std::size_t lookups_per_sample = 0;
	double mean_ns = 0.0;
	double median_ns = 0.0;
	double min_ns = 0.0;
	double max_ns = 0.0;
};

struct SizedBenchStats {
	std::string name;
	std::size_t elements = 0;
	double mean_ns = 0.0;
	double median_ns = 0.0;
	double min_ns = 0.0;
	double max_ns = 0.0;
};

volatile std::uint64_t g_lookup_sink = 0;

void print_usage(const char* prog) {
	std::cout << "Usage: " << prog
	          << " [--elements <n>] [--iterations <n>] [--warmup <n>]\n";
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

bool parse_size_arg(const char* text, std::size_t& out) {
	char* end = nullptr;
	const unsigned long long value = std::strtoull(text, &end, 10);
	if (end == text || *end != '\0' || value == 0 || value > 100000000ULL) {
		return false;
	}
	out = static_cast<std::size_t>(value);
	return true;
}

bool parse_args(int argc, char** argv, Options& options) {
	for (int i = 1; i < argc; ++i) {
		const std::string arg = argv[i];
		if (arg == "-h" || arg == "--help") {
			print_usage(argv[0]);
			return false;
		}
		if (arg == "--elements" && i + 1 < argc) {
			if (!parse_size_arg(argv[++i], options.elements)) {
				std::cerr << "Invalid --elements value\n";
				return false;
			}
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
	return true;
}

const char* lookup_pattern_name(LookupPattern pattern) {
	switch (pattern) {
	case LookupPattern::hit_all:
		return "hit_all";
	case LookupPattern::hit_last:
		return "hit_last";
	case LookupPattern::miss_after_end:
		return "miss_after_end";
	}
	return "unknown";
}

dicom::Tag make_tag(std::size_t index) {
	const auto group = static_cast<std::uint16_t>(0x1000u + ((index >> 16U) & 0x0FFFu));
	const auto element = static_cast<std::uint16_t>(index & 0xFFFFu);
	return dicom::Tag(group, element);
}

std::size_t make_offset(std::size_t index) {
	return index * sizeof(std::uint32_t);
}

template <typename... Args>
std::unique_ptr<dicom::DataElement> make_owned_element(Args&&... args) {
	return std::make_unique<dicom::DataElement>(std::forward<Args>(args)...);
}

class VectorQueue {
public:
	explicit VectorQueue(std::size_t reserve_count = 0) {
		if (reserve_count != 0) {
			elements_.reserve(reserve_count);
		}
	}

	dicom::DataElement* push_back(dicom::Tag tag, dicom::VR vr,
	    std::size_t offset, std::size_t length) {
		elements_.push_back(make_owned_element(tag, vr, length, offset, &owner_));
		return elements_.back().get();
	}

	const dicom::DataElement* front_ptr() const {
		return elements_.empty() ? nullptr : elements_.front().get();
	}

private:
	dicom::DataSet owner_;
	std::vector<std::unique_ptr<dicom::DataElement>> elements_;
};

class DequeQueue {
public:
	dicom::DataElement* push_back(dicom::Tag tag, dicom::VR vr,
	    std::size_t offset, std::size_t length) {
		elements_.emplace_back(tag, vr, length, offset, &owner_);
		return &elements_.back();
	}

	const dicom::DataElement* front_ptr() const {
		return elements_.empty() ? nullptr : &elements_.front();
	}

private:
	dicom::DataSet owner_;
	std::deque<dicom::DataElement> elements_;
};

class DataSetAppendQueue {
public:
	dicom::DataElement* push_back(dicom::Tag tag, dicom::VR vr,
	    std::size_t offset, std::size_t length) {
		if (!first_tag_) {
			first_tag_ = tag;
		}
		(void)offset;
		(void)length;
		return &data_set_.add_dataelement(tag, vr);
	}

	const dicom::DataElement* front_ptr() const {
		return first_tag_ ? &data_set_.get_dataelement(first_tag_) : nullptr;
	}

private:
	dicom::DataSet data_set_;
	dicom::Tag first_tag_{};
};

class IndexedDequeDataSet {
public:
	dicom::DataElement* add_dataelement(dicom::Tag tag, dicom::VR vr,
	    std::size_t offset, std::size_t length) {
		const auto tag_value = tag.value();
		if (element_index_.empty() || element_index_.back()->tag().value() < tag_value) {
			elements_.emplace_back(tag, vr, length, offset, &owner_);
			auto* element = &elements_.back();
			element_index_.push_back(element);
			return element;
		}

		auto it = std::lower_bound(element_index_.begin(), element_index_.end(), tag_value,
		    [](const dicom::DataElement* element, std::uint32_t value) {
				return element->tag().value() < value;
			});
		if (it != element_index_.end() && (*it)->tag().value() == tag_value) {
			return *it;
		}

		elements_.emplace_back(tag, vr, length, offset, &owner_);
		auto* element = &elements_.back();
		element_index_.insert(it, element);
		return element;
	}

	const dicom::DataElement* get_dataelement(dicom::Tag tag) const {
		const auto tag_value = tag.value();
		const auto it = std::lower_bound(element_index_.begin(), element_index_.end(), tag_value,
		    [](const dicom::DataElement* element, std::uint32_t value) {
			return element->tag().value() < value;
		});
		if (it != element_index_.end() && (*it)->tag().value() == tag_value) {
			return *it;
		}
		return nullptr;
	}

private:
	dicom::DataSet owner_;
	std::deque<dicom::DataElement> elements_;
	std::vector<dicom::DataElement*> element_index_;
};

class CurrentDataSetAdapter {
public:
	dicom::DataElement* add_dataelement(dicom::Tag tag, dicom::VR vr,
	    std::size_t offset, std::size_t length) {
		(void)offset;
		(void)length;
		return &data_set_.add_dataelement(tag, vr);
	}

	const dicom::DataElement* get_dataelement(dicom::Tag tag) const {
		const auto& element = data_set_.get_dataelement(tag);
		return element ? &element : nullptr;
	}

private:
	dicom::DataSet data_set_;
};

class LegacyMapFallbackDataSet {
public:
	explicit LegacyMapFallbackDataSet(std::size_t reserve_count = 0) {
		(void)reserve_count;
	}

	dicom::DataElement* add_dataelement(dicom::Tag tag, dicom::VR vr,
	    std::size_t offset, std::size_t length) {
		const auto tag_value = tag.value();

		if (elements_.empty() || elements_.back().tag().value() < tag_value) {
			elements_.emplace_back(tag, vr, length, offset, &owner_);
			return &elements_.back();
		}

		const auto find_in_elements = [&](std::uint32_t value) -> dicom::DataElement* {
			auto it = std::lower_bound(elements_.begin(), elements_.end(), value,
			    [](const dicom::DataElement& element, std::uint32_t compare_value) {
					return element.tag().value() < compare_value;
				});
			if (it != elements_.end() && it->tag().value() == value) {
				return &(*it);
			}
			return nullptr;
		};

		if (auto* element = find_in_elements(tag_value)) {
			return element;
		}

		auto map_it = element_map_.lower_bound(tag_value);
		if (map_it != element_map_.end() && map_it->first == tag_value) {
			return &map_it->second;
		}

		auto insert_it = element_map_.emplace_hint(map_it, std::piecewise_construct,
		    std::forward_as_tuple(tag_value),
		    std::forward_as_tuple(tag, vr, length, offset, &owner_));
		return &insert_it->second;
	}

	const dicom::DataElement* get_dataelement(dicom::Tag tag) const {
		const auto tag_value = tag.value();
		auto it = std::lower_bound(elements_.begin(), elements_.end(), tag_value,
		    [](const dicom::DataElement& element, std::uint32_t compare_value) {
				return element.tag().value() < compare_value;
			});
		if (it != elements_.end() && it->tag().value() == tag_value) {
			return &(*it);
		}

		if (auto map_it = element_map_.find(tag_value); map_it != element_map_.end()) {
			return &map_it->second;
		}
		return nullptr;
	}

private:
	dicom::DataSet owner_;
	std::deque<dicom::DataElement> elements_;
	std::map<std::uint32_t, dicom::DataElement> element_map_;
};

class VectorLookup {
public:
	explicit VectorLookup(std::size_t elements) {
		elements_.reserve(elements);
		for (std::size_t idx = 0; idx < elements; ++idx) {
			elements_.push_back(make_owned_element(
			    make_tag(idx), dicom::VR::UL, sizeof(std::uint32_t), make_offset(idx), &owner_));
		}
	}

	const dicom::DataElement* find(dicom::Tag tag) const {
		for (const auto& element : elements_) {
			if (element->tag() == tag) {
				return element.get();
			}
		}
		return nullptr;
	}

private:
	dicom::DataSet owner_;
	std::vector<std::unique_ptr<dicom::DataElement>> elements_;
};

class VectorBinaryLookup {
public:
	explicit VectorBinaryLookup(std::size_t elements) {
		elements_.reserve(elements);
		for (std::size_t idx = 0; idx < elements; ++idx) {
			elements_.push_back(make_owned_element(
			    make_tag(idx), dicom::VR::UL, sizeof(std::uint32_t), make_offset(idx), &owner_));
		}
	}

	const dicom::DataElement* find(dicom::Tag tag) const {
		const auto tag_value = tag.value();
		const auto it = std::lower_bound(elements_.begin(), elements_.end(), tag_value,
		    [](const std::unique_ptr<dicom::DataElement>& element, std::uint32_t value) {
				return element->tag().value() < value;
			});
		if (it != elements_.end() && (*it)->tag().value() == tag_value) {
			return it->get();
		}
		return nullptr;
	}

private:
	dicom::DataSet owner_;
	std::vector<std::unique_ptr<dicom::DataElement>> elements_;
};

class DequePointerLookup {
public:
	explicit DequePointerLookup(std::size_t elements) {
		for (std::size_t idx = 0; idx < elements; ++idx) {
			elements_.emplace_back(
			    make_tag(idx), dicom::VR::UL, sizeof(std::uint32_t), make_offset(idx), &owner_);
		}
		index_.reserve(elements_.size());
		for (const auto& element : elements_) {
			index_.push_back(&element);
		}
	}

	const dicom::DataElement* find(dicom::Tag tag) const {
		for (const auto* element : index_) {
			if (element->tag() == tag) {
				return element;
			}
		}
		return nullptr;
	}

private:
	dicom::DataSet owner_;
	std::deque<dicom::DataElement> elements_;
	std::vector<const dicom::DataElement*> index_;
};

class DequePointerBinaryLookup {
public:
	explicit DequePointerBinaryLookup(std::size_t elements) {
		for (std::size_t idx = 0; idx < elements; ++idx) {
			elements_.emplace_back(
			    make_tag(idx), dicom::VR::UL, sizeof(std::uint32_t), make_offset(idx), &owner_);
		}
		index_.reserve(elements_.size());
		for (const auto& element : elements_) {
			index_.push_back(&element);
		}
	}

	const dicom::DataElement* find(dicom::Tag tag) const {
		const auto tag_value = tag.value();
		const auto it = std::lower_bound(index_.begin(), index_.end(), tag_value,
		    [](const dicom::DataElement* element, std::uint32_t value) {
			return element->tag().value() < value;
		});
		if (it != index_.end() && (*it)->tag().value() == tag_value) {
			return *it;
		}
		return nullptr;
	}

private:
	dicom::DataSet owner_;
	std::deque<dicom::DataElement> elements_;
	std::vector<const dicom::DataElement*> index_;
};

template <typename Queue>
void fill_queue(Queue& queue, std::size_t elements) {
	for (std::size_t idx = 0; idx < elements; ++idx) {
		queue.push_back(make_tag(idx), dicom::VR::UL, make_offset(idx), sizeof(std::uint32_t));
	}
}

template <typename Factory>
std::size_t count_front_pointer_moves(std::size_t elements, Factory&& factory) {
	auto queue = factory();
	const dicom::DataElement* last_front = nullptr;
	std::size_t moves = 0;

	for (std::size_t idx = 0; idx < elements; ++idx) {
		queue.push_back(make_tag(idx), dicom::VR::UL, make_offset(idx), sizeof(std::uint32_t));
		const auto* current_front = queue.front_ptr();
		if (last_front != nullptr && current_front != last_front) {
			++moves;
		}
		last_front = current_front;
	}
	return moves;
}

std::vector<dicom::Tag> build_lookup_queries(std::size_t elements, LookupPattern pattern) {
	std::vector<dicom::Tag> queries;
	switch (pattern) {
	case LookupPattern::hit_all:
		queries.reserve(elements);
		for (std::size_t idx = 0; idx < elements; ++idx) {
			queries.push_back(make_tag(idx));
		}
		break;
	case LookupPattern::hit_last:
		queries.push_back(make_tag(elements - 1U));
		break;
	case LookupPattern::miss_after_end:
		queries.push_back(make_tag(elements));
		break;
	}
	return queries;
}

std::size_t compute_lookup_passes(std::size_t elements) {
	constexpr std::size_t kTargetLookupsPerSample = 50000;
	return std::max<std::size_t>(1, kTargetLookupsPerSample / elements);
}

double compute_median(std::vector<double> values) {
	if (values.empty()) {
		return 0.0;
	}
	std::sort(values.begin(), values.end());
	const auto mid = values.size() / 2U;
	if ((values.size() % 2U) == 0U) {
		return (values[mid - 1U] + values[mid]) * 0.5;
	}
	return values[mid];
}

template <typename Factory>
BenchStats run_case(std::string name, const Options& options, Factory&& factory) {
	std::vector<double> samples_ns;
	samples_ns.reserve(static_cast<std::size_t>(options.iterations));

	using clock = std::chrono::steady_clock;

	for (int iteration = 0; iteration < options.warmup + options.iterations; ++iteration) {
		const auto start = clock::now();
		auto queue = factory();
		fill_queue(queue, options.elements);
		const auto elapsed_ns =
		    std::chrono::duration<double, std::nano>(clock::now() - start).count();
		if (iteration >= options.warmup) {
			samples_ns.push_back(elapsed_ns);
		}
	}

	BenchStats stats;
	stats.name = std::move(name);
	stats.mean_ns =
	    std::accumulate(samples_ns.begin(), samples_ns.end(), 0.0) /
	    static_cast<double>(samples_ns.size());
	stats.median_ns = compute_median(samples_ns);
	const auto minmax = std::minmax_element(samples_ns.begin(), samples_ns.end());
	stats.min_ns = *minmax.first;
	stats.max_ns = *minmax.second;
	stats.front_pointer_moves = count_front_pointer_moves(options.elements, factory);
	return stats;
}

template <typename LookupFactory>
LookupBenchStats run_lookup_case(std::string name, LookupPattern pattern,
    const Options& options, std::size_t elements, LookupFactory&& factory) {
	const auto queries = build_lookup_queries(elements, pattern);
	const auto passes = compute_lookup_passes(elements);
	const auto lookups_per_sample = queries.size() * passes;
	std::vector<double> samples_ns;
	samples_ns.reserve(static_cast<std::size_t>(options.iterations));

	using clock = std::chrono::steady_clock;

	for (int iteration = 0; iteration < options.warmup + options.iterations; ++iteration) {
		const auto lookup = factory(elements);
		std::uint64_t local_sink = 0;
		const auto start = clock::now();
		for (std::size_t pass = 0; pass < passes; ++pass) {
			for (const auto tag : queries) {
				const auto* found = lookup.find(tag);
				local_sink += found ? found->tag().value() : 0;
			}
		}
		const auto elapsed_ns =
		    std::chrono::duration<double, std::nano>(clock::now() - start).count();
		g_lookup_sink += local_sink;
		if (iteration >= options.warmup) {
			samples_ns.push_back(elapsed_ns);
		}
	}

	LookupBenchStats stats;
	stats.name = std::move(name);
	stats.pattern = lookup_pattern_name(pattern);
	stats.elements = elements;
	stats.lookups_per_sample = lookups_per_sample;
	stats.mean_ns =
	    std::accumulate(samples_ns.begin(), samples_ns.end(), 0.0) /
	    static_cast<double>(samples_ns.size());
	stats.median_ns = compute_median(samples_ns);
	const auto minmax = std::minmax_element(samples_ns.begin(), samples_ns.end());
	stats.min_ns = *minmax.first;
	stats.max_ns = *minmax.second;
	return stats;
}

template <typename Factory>
SizedBenchStats run_add_micro_case(std::string name, const Options& options,
    std::size_t elements, Factory&& factory) {
	std::vector<double> samples_ns;
	samples_ns.reserve(static_cast<std::size_t>(options.iterations));

	using clock = std::chrono::steady_clock;

	for (int iteration = 0; iteration < options.warmup + options.iterations; ++iteration) {
		auto data_set = factory();
		const auto start = clock::now();
		for (std::size_t idx = 0; idx < elements; ++idx) {
			data_set.add_dataelement(
			    make_tag(idx), dicom::VR::UL, make_offset(idx), sizeof(std::uint32_t));
		}
		const auto elapsed_ns =
		    std::chrono::duration<double, std::nano>(clock::now() - start).count();
		if (iteration >= options.warmup) {
			samples_ns.push_back(elapsed_ns);
		}
	}

	SizedBenchStats stats;
	stats.name = std::move(name);
	stats.elements = elements;
	stats.mean_ns =
	    std::accumulate(samples_ns.begin(), samples_ns.end(), 0.0) /
	    static_cast<double>(samples_ns.size());
	stats.median_ns = compute_median(samples_ns);
	const auto minmax = std::minmax_element(samples_ns.begin(), samples_ns.end());
	stats.min_ns = *minmax.first;
	stats.max_ns = *minmax.second;
	return stats;
}

template <typename Factory, typename PrepareFn>
SizedBenchStats run_get_micro_case(std::string name, const Options& options,
    std::size_t elements, Factory&& factory, PrepareFn&& prepare) {
	const auto queries = build_lookup_queries(elements, LookupPattern::hit_all);
	const auto passes = compute_lookup_passes(elements);
	const auto lookups_per_sample = queries.size() * passes;
	std::vector<double> samples_ns;
	samples_ns.reserve(static_cast<std::size_t>(options.iterations));

	using clock = std::chrono::steady_clock;

	for (int iteration = 0; iteration < options.warmup + options.iterations; ++iteration) {
		auto data_set = factory(elements);
		prepare(data_set, elements);
		std::uint64_t local_sink = 0;
		const auto start = clock::now();
		for (std::size_t pass = 0; pass < passes; ++pass) {
			for (const auto tag : queries) {
				const auto* found = data_set.get_dataelement(tag);
				local_sink += found ? found->tag().value() : 0;
			}
		}
		const auto elapsed_ns =
		    std::chrono::duration<double, std::nano>(clock::now() - start).count();
		g_lookup_sink += local_sink;
		if (iteration >= options.warmup) {
			samples_ns.push_back(elapsed_ns / static_cast<double>(lookups_per_sample));
		}
	}

	SizedBenchStats stats;
	stats.name = std::move(name);
	stats.elements = elements;
	stats.mean_ns =
	    std::accumulate(samples_ns.begin(), samples_ns.end(), 0.0) /
	    static_cast<double>(samples_ns.size());
	stats.median_ns = compute_median(samples_ns);
	const auto minmax = std::minmax_element(samples_ns.begin(), samples_ns.end());
	stats.min_ns = *minmax.first;
	stats.max_ns = *minmax.second;
	return stats;
}

void print_case(const BenchStats& stats, std::size_t elements) {
	const double mean_ns_per_element =
	    stats.mean_ns / static_cast<double>(elements);
	const double median_ns_per_element =
	    stats.median_ns / static_cast<double>(elements);

	std::cout << std::fixed << std::setprecision(2);
	std::cout << "case=" << stats.name
	          << " mean_us=" << (stats.mean_ns / 1000.0)
	          << " median_us=" << (stats.median_ns / 1000.0)
	          << " min_us=" << (stats.min_ns / 1000.0)
	          << " max_us=" << (stats.max_ns / 1000.0)
	          << " mean_ns_per_element=" << mean_ns_per_element
	          << " median_ns_per_element=" << median_ns_per_element
	          << " front_pointer_moves=" << stats.front_pointer_moves
	          << "\n";
}

void print_lookup_case(const LookupBenchStats& stats) {
	std::cout << std::fixed << std::setprecision(2);
	std::cout << "lookup_case=" << stats.name
	          << " pattern=" << stats.pattern
	          << " elements=" << stats.elements
	          << " lookups_per_sample=" << stats.lookups_per_sample
	          << " mean_ns_per_lookup="
	          << (stats.mean_ns / static_cast<double>(stats.lookups_per_sample))
	          << " median_ns_per_lookup="
	          << (stats.median_ns / static_cast<double>(stats.lookups_per_sample))
	          << " min_ns_per_lookup="
	          << (stats.min_ns / static_cast<double>(stats.lookups_per_sample))
	          << " max_ns_per_lookup="
	          << (stats.max_ns / static_cast<double>(stats.lookups_per_sample))
	          << "\n";
}

void print_sized_case(const SizedBenchStats& stats, const char* unit) {
	std::cout << std::fixed << std::setprecision(2);
	std::cout << "micro_case=" << stats.name
	          << " elements=" << stats.elements
	          << " mean_" << unit << "=" << stats.mean_ns
	          << " median_" << unit << "=" << stats.median_ns
	          << " min_" << unit << "=" << stats.min_ns
	          << " max_" << unit << "=" << stats.max_ns
	          << "\n";
}

}  // namespace

int main(int argc, char** argv) {
	Options options;
	if (!parse_args(argc, argv, options)) {
		return 1;
	}

	std::cout << "DataElement queue benchmark\n";
	std::cout << "elements=" << options.elements
	          << " iterations=" << options.iterations
	          << " warmup=" << options.warmup
	          << " sizeof(DataElement)=" << sizeof(dicom::DataElement)
	          << "\n";

	const auto vector_unreserved =
	    run_case("vector_unreserved", options, [] { return VectorQueue{}; });
	const auto vector_reserved =
	    run_case("vector_reserved", options, [&options] { return VectorQueue{options.elements}; });
	const auto deque_queue =
	    run_case("deque_queue", options, [] { return DequeQueue{}; });
	const auto dataset_append =
	    run_case("dataset_append", options, [] { return DataSetAppendQueue{}; });

	print_case(vector_unreserved, options.elements);
	print_case(vector_reserved, options.elements);
	print_case(deque_queue, options.elements);
	print_case(dataset_append, options.elements);

	std::cout << "Tag lookup benchmark\n";
	for (const auto elements : kLookupElementCounts) {
		for (const auto pattern : kLookupPatterns) {
			const auto vector_lookup =
			    run_lookup_case("vector_direct", pattern, options, elements,
			        [](std::size_t count) { return VectorLookup{count}; });
			const auto vector_binary_lookup =
			    run_lookup_case("vector_binary", pattern, options, elements,
			        [](std::size_t count) { return VectorBinaryLookup{count}; });
			const auto deque_pointer_lookup =
			    run_lookup_case("deque_pointer", pattern, options, elements,
			        [](std::size_t count) { return DequePointerLookup{count}; });
			const auto deque_pointer_binary_lookup =
			    run_lookup_case("deque_pointer_binary", pattern, options, elements,
			        [](std::size_t count) { return DequePointerBinaryLookup{count}; });
			print_lookup_case(vector_lookup);
			print_lookup_case(vector_binary_lookup);
			print_lookup_case(deque_pointer_lookup);
			print_lookup_case(deque_pointer_binary_lookup);
		}
	}

	std::cout << "Indexed deque dataset micro benchmark\n";
	for (const auto elements : kLookupElementCounts) {
		const auto current_add =
		    run_add_micro_case("current_dataset_add", options, elements,
		        [] { return CurrentDataSetAdapter{}; });
		const auto legacy_map_fallback_add =
		    run_add_micro_case("legacy_map_fallback_add", options, elements,
		        [elements] { return LegacyMapFallbackDataSet{elements}; });
		const auto indexed_deque_add =
		    run_add_micro_case("indexed_deque_add", options, elements,
		        [] { return IndexedDequeDataSet{}; });
		const auto current_get =
		    run_get_micro_case("current_dataset_get", options, elements,
		        [](std::size_t) { return CurrentDataSetAdapter{}; },
		        [](CurrentDataSetAdapter& data_set, std::size_t count) {
					for (std::size_t idx = 0; idx < count; ++idx) {
						data_set.add_dataelement(
						    make_tag(idx), dicom::VR::UL, make_offset(idx), sizeof(std::uint32_t));
					}
				});
		const auto legacy_map_fallback_get =
		    run_get_micro_case("legacy_map_fallback_get", options, elements,
		        [](std::size_t count) { return LegacyMapFallbackDataSet{count}; },
		        [](LegacyMapFallbackDataSet& data_set, std::size_t count) {
					for (std::size_t idx = 0; idx < count; ++idx) {
						data_set.add_dataelement(
						    make_tag(idx), dicom::VR::UL, make_offset(idx), sizeof(std::uint32_t));
					}
				});
		const auto indexed_deque_get =
		    run_get_micro_case("indexed_deque_get", options, elements,
		        [](std::size_t) { return IndexedDequeDataSet{}; },
		        [](IndexedDequeDataSet& data_set, std::size_t count) {
					for (std::size_t idx = 0; idx < count; ++idx) {
						data_set.add_dataelement(
						    make_tag(idx), dicom::VR::UL, make_offset(idx), sizeof(std::uint32_t));
					}
				});
		print_sized_case(current_add, "ns_total");
		print_sized_case(legacy_map_fallback_add, "ns_total");
		print_sized_case(indexed_deque_add, "ns_total");
		print_sized_case(current_get, "ns_per_lookup");
		print_sized_case(legacy_map_fallback_get, "ns_per_lookup");
		print_sized_case(indexed_deque_get, "ns_per_lookup");
	}
	std::cout << "lookup_sink=" << g_lookup_sink << "\n";

	return 0;
}
