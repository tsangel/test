#include "dicom.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace dicom::literals;

namespace {

using clock_type = std::chrono::steady_clock;

enum class BenchVisitControl : std::uint8_t {
	continue_,
	skip_sequence,
	skip_dataset,
	stop,
};

struct Options {
	int iterations = 12;
	int warmup = 3;
	int inner_iterations = 1000;
};

struct BenchStats {
	std::string name;
	double mean_ns_per_element = 0.0;
	double median_ns_per_element = 0.0;
	double min_ns_per_element = 0.0;
	double max_ns_per_element = 0.0;
};

struct OpBenchStats {
	std::string name;
	double mean_ns_per_op = 0.0;
	double median_ns_per_op = 0.0;
	double min_ns_per_op = 0.0;
	double max_ns_per_op = 0.0;
};

struct VisitorStackEntry {
	dicom::DataSet::iterator current{};
	dicom::DataSet::iterator end{};
	dicom::Sequence* parent_sequence{nullptr};
	dicom::Tag parent_sequence_tag{};
	std::uint32_t item_index{0};
};

class BenchVisitorPathRef {
public:
	BenchVisitorPathRef() = default;
	explicit BenchVisitorPathRef(const std::vector<VisitorStackEntry>* stack) noexcept
	    : stack_(stack) {}

	[[nodiscard]] bool empty() const noexcept {
		return size() == 0;
	}

	[[nodiscard]] std::size_t size() const noexcept {
		if (stack_ == nullptr || stack_->size() <= 1) {
			return 0;
		}
		return stack_->size() - 1;
	}

	[[nodiscard]] bool contains_sequence(dicom::Tag sequence_tag) const noexcept {
		if (stack_ == nullptr || stack_->size() <= 1) {
			return false;
		}
		for (std::size_t i = 1; i < stack_->size(); ++i) {
			if ((*stack_)[i].parent_sequence_tag == sequence_tag) {
				return true;
			}
		}
		return false;
	}

private:
	const std::vector<VisitorStackEntry>* stack_{nullptr};
};

template <typename Fn>
void visit_dataset_generic(dicom::DataSet& dataset, Fn&& fn);

struct BenchDataset {
	dicom::DicomFile file{};
	std::size_t visited_elements = 0;
	std::size_t ui_elements = 0;
};

struct RewriteTarget {
	dicom::DataElement* element{nullptr};
	std::string original_uid{};
};

struct RewriteBenchState {
	BenchDataset dataset{};
	std::vector<RewriteTarget> targets{};
	dicom::UidRemapper remapper{};
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
double measure_ns_per_element(
    Fn&& fn, int inner_iterations, std::size_t elements_per_iteration) {
	const auto start = clock_type::now();
	for (int i = 0; i < inner_iterations; ++i) {
		fn();
	}
	const auto elapsed =
	    std::chrono::duration_cast<std::chrono::nanoseconds>(clock_type::now() - start)
	        .count();
	return static_cast<double>(elapsed) /
	    static_cast<double>(inner_iterations * elements_per_iteration);
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
BenchStats run_bench(std::string name, const Options& options,
    std::size_t elements_per_iteration, Fn&& fn) {
	std::vector<double> samples;
	samples.reserve(static_cast<std::size_t>(options.iterations));

	for (int i = 0; i < options.warmup; ++i) {
		(void)measure_ns_per_element(fn, options.inner_iterations, elements_per_iteration);
	}
	for (int i = 0; i < options.iterations; ++i) {
		samples.push_back(
		    measure_ns_per_element(fn, options.inner_iterations, elements_per_iteration));
	}

	std::sort(samples.begin(), samples.end());
	double sum = 0.0;
	for (double sample : samples) {
		sum += sample;
	}

	BenchStats stats;
	stats.name = std::move(name);
	stats.mean_ns_per_element = sum / static_cast<double>(samples.size());
	stats.median_ns_per_element = samples[samples.size() / 2];
	stats.min_ns_per_element = samples.front();
	stats.max_ns_per_element = samples.back();
	return stats;
}

template <typename Fn>
OpBenchStats run_bench_per_op(
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

	OpBenchStats stats;
	stats.name = std::move(name);
	stats.mean_ns_per_op = sum / static_cast<double>(samples.size());
	stats.median_ns_per_op = samples[samples.size() / 2];
	stats.min_ns_per_op = samples.front();
	stats.max_ns_per_op = samples.back();
	return stats;
}

void print_stats(const BenchStats& stats, double walk_baseline) {
	std::cout << std::left << std::setw(34) << stats.name
	          << " mean=" << std::fixed << std::setprecision(2)
	          << stats.mean_ns_per_element << " ns/elem"
	          << " median=" << stats.median_ns_per_element
	          << " min=" << stats.min_ns_per_element
	          << " max=" << stats.max_ns_per_element;
	if (walk_baseline > 0.0) {
		std::cout << " ratio_vs_walk=" << std::setprecision(3)
		          << (stats.mean_ns_per_element / walk_baseline) << "x";
	}
	std::cout << "\n";
}

void print_op_stats(const OpBenchStats& stats, double walk_baseline) {
	std::cout << std::left << std::setw(34) << stats.name
	          << " mean=" << std::fixed << std::setprecision(2)
	          << stats.mean_ns_per_op << " ns/pass"
	          << " median=" << stats.median_ns_per_op
	          << " min=" << stats.min_ns_per_op
	          << " max=" << stats.max_ns_per_op;
	if (walk_baseline > 0.0) {
		std::cout << " ratio_vs_walk=" << std::setprecision(3)
		          << (stats.mean_ns_per_op / walk_baseline) << "x";
	}
	std::cout << "\n";
}

[[nodiscard]] dicom::Tag make_unknown_tag(
    std::uint16_t group, std::uint16_t base_element, int index) {
	return dicom::Tag(group, static_cast<std::uint16_t>(base_element + index));
}

[[nodiscard]] std::string make_uid(int a, int b, int c, int d) {
	return "1.2.826.0.1.3680043.10.543." + std::to_string(a) + "." +
	    std::to_string(b) + "." + std::to_string(c) + "." + std::to_string(d);
}

void set_string_or_throw(
    dicom::DataSet& dataset, dicom::Tag tag, dicom::VR vr, std::string_view value) {
	if (!dataset.set_value(tag, vr, value)) {
		throw std::runtime_error("benchmark dataset string assignment failed");
	}
}

void set_long_or_throw(
    dicom::DataSet& dataset, dicom::Tag tag, dicom::VR vr, long value) {
	if (!dataset.set_value(tag, vr, value)) {
		throw std::runtime_error("benchmark dataset numeric assignment failed");
	}
}

dicom::Sequence& ensure_sequence_or_throw(dicom::DataSet& dataset, dicom::Tag tag) {
	auto& element = dataset.ensure_dataelement(tag, dicom::VR::SQ);
	auto* sequence = element.as_sequence();
	if (sequence == nullptr) {
		throw std::runtime_error("benchmark dataset sequence creation failed");
	}
	return *sequence;
}

void build_bench_dataset(BenchDataset& out) {
	auto& root = out.file.dataset();

	if (!root.set_value("StudyInstanceUID"_tag, std::string_view(make_uid(1, 1, 0, 0))) ||
	    !root.set_value("SeriesInstanceUID"_tag, std::string_view(make_uid(1, 2, 0, 0))) ||
	    !root.set_value("SOPInstanceUID"_tag, std::string_view(make_uid(1, 3, 0, 0))) ||
	    !root.set_value("FrameOfReferenceUID"_tag, std::string_view(make_uid(1, 4, 0, 0))) ||
	    !root.set_value("PatientID"_tag, std::string_view("WALKBENCH")) ||
	    !root.set_value("PatientName"_tag, std::string_view("Walk^Benchmark"))) {
		throw std::runtime_error("benchmark root initialization failed");
	}

	for (int i = 0; i < 64; ++i) {
		set_string_or_throw(
		    root, make_unknown_tag(0x7010, 0x1000, i), dicom::VR::UI,
		    make_uid(10, i, 0, 1));
		set_string_or_throw(
		    root, make_unknown_tag(0x7012, 0x1100, i), dicom::VR::LO,
		    std::string("ROOT-TEXT-") + std::to_string(i));
		set_long_or_throw(
		    root, make_unknown_tag(0x7014, 0x1200, i), dicom::VR::UL, 1000L + i);
	}

	auto& study_seq = ensure_sequence_or_throw(root, "ReferencedStudySequence"_tag);
	for (int study_index = 0; study_index < 8; ++study_index) {
		auto* study_item = study_seq.add_dataset();
		if (study_item == nullptr) {
			throw std::runtime_error("benchmark study item creation failed");
		}
		if (!study_item->set_value(
		        "StudyInstanceUID"_tag,
		        std::string_view(make_uid(20, study_index, 0, 0))) ||
		    !study_item->set_value(
		        "AccessionNumber"_tag,
		        std::string_view(std::string("ACC-") + std::to_string(study_index))) ||
		    !study_item->set_value(
		        "StudyID"_tag,
		        std::string_view(std::string("STUDY-") + std::to_string(study_index)))) {
			throw std::runtime_error("benchmark study item initialization failed");
		}

		for (int i = 0; i < 4; ++i) {
			set_string_or_throw(
			    *study_item, make_unknown_tag(0x7110, 0x1000, i), dicom::VR::UI,
			    make_uid(21, study_index, i, 1));
			set_long_or_throw(
			    *study_item, make_unknown_tag(0x7112, 0x1100, i), dicom::VR::UL,
			    2000L + study_index * 10L + i);
		}

		auto& series_seq =
		    ensure_sequence_or_throw(*study_item, "ReferencedSeriesSequence"_tag);
		for (int series_index = 0; series_index < 6; ++series_index) {
			auto* series_item = series_seq.add_dataset();
			if (series_item == nullptr) {
				throw std::runtime_error("benchmark series item creation failed");
			}
			if (!series_item->set_value(
			        "SeriesInstanceUID"_tag,
			        std::string_view(make_uid(30, study_index, series_index, 0))) ||
			    !series_item->set_value("Modality"_tag, std::string_view("CT")) ||
			    !series_item->set_value(
			        "SeriesDescription"_tag,
			        std::string_view(std::string("SERIES-") + std::to_string(series_index))) ||
			    !series_item->set_value("SeriesNumber"_tag, long(series_index + 1))) {
				throw std::runtime_error("benchmark series item initialization failed");
			}

			for (int i = 0; i < 4; ++i) {
				set_string_or_throw(
				    *series_item, make_unknown_tag(0x7210, 0x1000, i), dicom::VR::UI,
				    make_uid(31, study_index, series_index, i));
				set_long_or_throw(
				    *series_item, make_unknown_tag(0x7212, 0x1100, i), dicom::VR::UL,
				    3000L + series_index * 10L + i);
			}

			auto& instance_seq =
			    ensure_sequence_or_throw(*series_item, "ReferencedInstanceSequence"_tag);
			for (int instance_index = 0; instance_index < 6; ++instance_index) {
				auto* instance_item = instance_seq.add_dataset();
				if (instance_item == nullptr) {
					throw std::runtime_error("benchmark instance item creation failed");
				}
				if (!instance_item->set_value(
				        "ReferencedSOPClassUID"_tag,
				        std::string_view("1.2.840.10008.5.1.4.1.1.2")) ||
				    !instance_item->set_value(
				        "ReferencedSOPInstanceUID"_tag,
				        std::string_view(
				            make_uid(40, study_index, series_index, instance_index))) ||
				    !instance_item->set_value(
				        "SOPInstanceUID"_tag,
				        std::string_view(
				            make_uid(41, study_index, series_index, instance_index))) ||
				    !instance_item->set_value(
				        "FrameOfReferenceUID"_tag,
				        std::string_view(
				            make_uid(42, study_index, series_index, instance_index))) ||
				    !instance_item->set_value(
				        "InstanceNumber"_tag, long(instance_index + 1))) {
					throw std::runtime_error("benchmark instance item initialization failed");
				}

				for (int i = 0; i < 4; ++i) {
					set_string_or_throw(
					    *instance_item, make_unknown_tag(0x7310, 0x1000, i), dicom::VR::UI,
					    make_uid(43, study_index * 10 + series_index, instance_index, i));
					set_long_or_throw(
					    *instance_item, make_unknown_tag(0x7312, 0x1100, i), dicom::VR::UL,
					    4000L + instance_index * 10L + i);
				}

				auto& source_seq =
				    ensure_sequence_or_throw(*instance_item, "SourceImageSequence"_tag);
				for (int source_index = 0; source_index < 2; ++source_index) {
					auto* source_item = source_seq.add_dataset();
					if (source_item == nullptr) {
						throw std::runtime_error("benchmark source item creation failed");
					}
					if (!source_item->set_value(
					        "ReferencedSOPClassUID"_tag,
					        std::string_view("1.2.840.10008.5.1.4.1.1.2")) ||
					    !source_item->set_value(
					        "ReferencedSOPInstanceUID"_tag,
					        std::string_view(make_uid(
					            50, study_index, series_index * 10 + instance_index,
					            source_index))) ||
					    !source_item->set_value(
					        "ImageComments"_tag,
					        std::string_view(
					            std::string("SOURCE-") + std::to_string(source_index)))) {
						throw std::runtime_error(
						    "benchmark source item initialization failed");
					}
					for (int i = 0; i < 2; ++i) {
						set_string_or_throw(
						    *source_item, make_unknown_tag(0x7410, 0x1000, i), dicom::VR::UI,
						    make_uid(
						        51, study_index * 10 + series_index, instance_index * 10 + source_index,
						        i));
					}
				}
			}
		}
	}

	for (auto entry : root.walk()) {
		++out.visited_elements;
		if (entry.element.vr() == dicom::VR::UI) {
			++out.ui_elements;
		}
	}
}

[[nodiscard]] bool should_rewrite_uid_tag(dicom::Tag tag) noexcept {
	return tag == "StudyInstanceUID"_tag ||
	    tag == "SeriesInstanceUID"_tag ||
	    tag == "SOPInstanceUID"_tag ||
	    tag == "ReferencedSOPInstanceUID"_tag ||
	    tag == "ReferencedSOPInstanceUIDInFile"_tag ||
	    tag == "FrameOfReferenceUID"_tag;
}

void restore_rewrite_targets(const std::vector<RewriteTarget>& targets) {
	for (const auto& target : targets) {
		if (target.element == nullptr ||
		    !target.element->from_uid_string(target.original_uid)) {
			throw std::runtime_error("rewrite target restore failed");
		}
		g_sink += static_cast<std::uint64_t>(target.original_uid.size());
	}
}

void initialize_rewrite_state(
    RewriteBenchState& state, const std::filesystem::path& journal_path) {
	build_bench_dataset(state.dataset);
	state.remapper = dicom::UidRemapper::in_memory(
	    journal_path, dicom::uid::uid_prefix(), false);
	for (auto entry : state.dataset.file.dataset().walk()) {
		if (!should_rewrite_uid_tag(entry.element.tag()) ||
		    entry.element.vr() != dicom::VR::UI) {
			continue;
		}
		auto uid = entry.element.to_uid_string();
		if (!uid) {
			continue;
		}
		state.targets.push_back(RewriteTarget{
		    .element = &entry.element,
		    .original_uid = *uid,
		});
		g_sink += static_cast<std::uint64_t>(
		    state.remapper.map_uid(state.targets.back().original_uid).size());
	}
	if (state.targets.empty()) {
		throw std::runtime_error("rewrite benchmark should have at least one target");
	}
}

bool rewrite_uid_element(
    dicom::DataElement& element, dicom::UidRemapper& remapper) {
	if (!should_rewrite_uid_tag(element.tag()) || element.vr() != dicom::VR::UI) {
		return false;
	}
	auto uid = element.to_uid_string();
	if (!uid) {
		return false;
	}
	const auto mapped = remapper.map_uid(*uid);
	if (!element.from_uid_string(mapped)) {
		throw std::runtime_error("rewrite_uids assignment failed");
	}
	g_sink += static_cast<std::uint64_t>(mapped.size());
	return true;
}

std::size_t rewrite_uids_walk(
    dicom::DicomFile& file, dicom::UidRemapper& remapper) {
	std::size_t rewritten = 0;
	for (auto entry : file.walk()) {
		if (rewrite_uid_element(entry.element, remapper)) {
			++rewritten;
		}
	}
	return rewritten;
}

std::size_t rewrite_uids_template_visitor(
    dicom::DicomFile& file, dicom::UidRemapper& remapper) {
	std::size_t rewritten = 0;
	visit_dataset_generic(file.dataset(),
	    [&](const BenchVisitorPathRef&, dicom::DataElement& element) {
		    if (rewrite_uid_element(element, remapper)) {
			    ++rewritten;
			}
		    return BenchVisitControl::continue_;
	    });
	return rewritten;
}

std::size_t rewrite_uids_function_visitor(
    dicom::DicomFile& file, dicom::UidRemapper& remapper) {
	std::size_t rewritten = 0;
	using Callback =
	    std::function<BenchVisitControl(const BenchVisitorPathRef&, dicom::DataElement&)>;
	Callback callback = [&](const BenchVisitorPathRef&, dicom::DataElement& element) {
		if (rewrite_uid_element(element, remapper)) {
			++rewritten;
		}
		return BenchVisitControl::continue_;
	};
	visit_dataset_generic(file.dataset(), callback);
	return rewritten;
}

template <typename Fn>
void visit_dataset_generic(dicom::DataSet& dataset, Fn&& fn) {
	std::vector<VisitorStackEntry> stack;
	stack.push_back(VisitorStackEntry{
	    .current = dataset.begin(),
	    .end = dataset.end(),
	});

	while (!stack.empty()) {
		auto& stack_entry = stack.back();
		if (stack_entry.current != stack_entry.end) {
			dicom::DataElement& element = *stack_entry.current;
			const BenchVisitControl control = fn(BenchVisitorPathRef(&stack), element);
			if (control == BenchVisitControl::stop) {
				return;
			}

			dicom::Sequence* yielded_sequence =
			    (control != BenchVisitControl::skip_sequence &&
			            control != BenchVisitControl::skip_dataset &&
			            element.vr().is_sequence())
			        ? element.as_sequence()
			        : nullptr;

			if (control == BenchVisitControl::skip_dataset) {
				stack_entry.current = stack_entry.end;
			} else {
				++stack_entry.current;
			}

			if (yielded_sequence != nullptr && yielded_sequence->size() > 0) {
				dicom::DataSet* item_dataset = yielded_sequence->get_dataset(0);
				if (item_dataset != nullptr) {
					stack.push_back(VisitorStackEntry{
					    .current = item_dataset->begin(),
					    .end = item_dataset->end(),
					    .parent_sequence = yielded_sequence,
					    .parent_sequence_tag = element.tag(),
					    .item_index = 0,
					});
				} else {
					stack.push_back(VisitorStackEntry{
					    .parent_sequence = yielded_sequence,
					    .parent_sequence_tag = element.tag(),
					    .item_index = 0,
					});
				}
			}
			continue;
		}

		const VisitorStackEntry finished_entry = stack.back();
		stack.pop_back();
		if (finished_entry.parent_sequence == nullptr) {
			continue;
		}

		const auto next_item_index =
		    static_cast<std::size_t>(finished_entry.item_index) + 1;
		if (next_item_index >=
		    static_cast<std::size_t>(finished_entry.parent_sequence->size())) {
			continue;
		}

		dicom::DataSet* next_dataset =
		    finished_entry.parent_sequence->get_dataset(next_item_index);
		if (next_dataset != nullptr) {
			stack.push_back(VisitorStackEntry{
			    .current = next_dataset->begin(),
			    .end = next_dataset->end(),
			    .parent_sequence = finished_entry.parent_sequence,
			    .parent_sequence_tag = finished_entry.parent_sequence_tag,
			    .item_index = static_cast<std::uint32_t>(next_item_index),
			});
		} else {
			stack.push_back(VisitorStackEntry{
			    .parent_sequence = finished_entry.parent_sequence,
			    .parent_sequence_tag = finished_entry.parent_sequence_tag,
			    .item_index = static_cast<std::uint32_t>(next_item_index),
			});
		}
	}
}

template <typename PathRef>
void workload_count_tags(const PathRef&, dicom::DataElement& element) {
	g_sink += static_cast<std::uint64_t>(element.tag().value());
}

template <typename PathRef>
void workload_path_probe(const PathRef& path, dicom::DataElement& element) {
	g_sink += static_cast<std::uint64_t>(element.tag().value());
	g_sink += static_cast<std::uint64_t>(path.size());
	g_sink += path.contains_sequence("ReferencedSeriesSequence"_tag) ? 1u : 0u;
}

template <typename PathRef>
void workload_uid_scan(const PathRef&, dicom::DataElement& element) {
	g_sink += static_cast<std::uint64_t>(element.tag().value());
	if (element.vr() != dicom::VR::UI) {
		return;
	}
	if (auto uid = element.to_uid_string()) {
		g_sink += static_cast<std::uint64_t>(uid->size());
	}
}

template <typename WorkFn>
BenchStats bench_walk(
    const char* name, const Options& options, BenchDataset& dataset, WorkFn&& work_fn) {
	return run_bench(name, options, dataset.visited_elements, [&] {
		for (auto entry : dataset.file.dataset().walk()) {
			work_fn(entry.path, entry.element);
		}
	});
}

template <typename WorkFn>
BenchStats bench_template_visitor(
    const char* name, const Options& options, BenchDataset& dataset, WorkFn&& work_fn) {
	return run_bench(name, options, dataset.visited_elements, [&] {
		visit_dataset_generic(dataset.file.dataset(),
		    [&](const BenchVisitorPathRef& path, dicom::DataElement& element) {
			    work_fn(path, element);
			    return BenchVisitControl::continue_;
		    });
	});
}

template <typename WorkFn>
BenchStats bench_function_visitor(
    const char* name, const Options& options, BenchDataset& dataset, WorkFn&& work_fn) {
	using Callback =
	    std::function<BenchVisitControl(const BenchVisitorPathRef&, dicom::DataElement&)>;
	Callback callback =
	    [&](const BenchVisitorPathRef& path, dicom::DataElement& element) {
		    work_fn(path, element);
		    return BenchVisitControl::continue_;
	    };
	return run_bench(name, options, dataset.visited_elements, [&] {
		visit_dataset_generic(dataset.file.dataset(), callback);
	});
}

void print_section_header(const char* title) {
	std::cout << "\n[" << title << "]\n";
}

}  // namespace

int main(int argc, char** argv) {
	Options options;
	if (!parse_args(argc, argv, options)) {
		return 1;
	}

	BenchDataset dataset;
	build_bench_dataset(dataset);
	if (dataset.visited_elements == 0) {
		std::cerr << "Benchmark dataset should not be empty\n";
		return 1;
	}

	std::size_t template_count = 0;
	std::size_t function_count = 0;
	visit_dataset_generic(dataset.file.dataset(),
	    [&](const BenchVisitorPathRef&, dicom::DataElement&) {
		    ++template_count;
		    return BenchVisitControl::continue_;
	    });
	using Callback =
	    std::function<BenchVisitControl(const BenchVisitorPathRef&, dicom::DataElement&)>;
	Callback callback = [&](const BenchVisitorPathRef&, dicom::DataElement&) {
		++function_count;
		return BenchVisitControl::continue_;
	};
	visit_dataset_generic(dataset.file.dataset(), callback);

	if (template_count != dataset.visited_elements ||
	    function_count != dataset.visited_elements) {
		std::cerr << "Traversal count mismatch: walk=" << dataset.visited_elements
		          << " template=" << template_count
		          << " function=" << function_count << "\n";
		return 1;
	}

	std::cout << "DataSet walk vs generic visitor benchmark\n";
	std::cout << "iterations=" << options.iterations
	          << " warmup=" << options.warmup
	          << " inner_iterations=" << options.inner_iterations
	          << " visited_elements=" << dataset.visited_elements
	          << " ui_elements=" << dataset.ui_elements << "\n";

	print_section_header("count_tags");
	const auto walk_count =
	    bench_walk(
	        "walk range-for", options, dataset,
	        workload_count_tags<dicom::DataSetWalkPathRef>);
	const auto template_count_stats = bench_template_visitor(
	    "generic visitor (template)", options, dataset,
	    workload_count_tags<BenchVisitorPathRef>);
	const auto function_count_stats = bench_function_visitor(
	    "generic visitor (std::function)", options, dataset,
	    workload_count_tags<BenchVisitorPathRef>);
	print_stats(walk_count, walk_count.mean_ns_per_element);
	print_stats(template_count_stats, walk_count.mean_ns_per_element);
	print_stats(function_count_stats, walk_count.mean_ns_per_element);

	print_section_header("path_probe");
	const auto walk_path =
	    bench_walk(
	        "walk range-for", options, dataset,
	        workload_path_probe<dicom::DataSetWalkPathRef>);
	const auto template_path = bench_template_visitor(
	    "generic visitor (template)", options, dataset,
	    workload_path_probe<BenchVisitorPathRef>);
	const auto function_path = bench_function_visitor(
	    "generic visitor (std::function)", options, dataset,
	    workload_path_probe<BenchVisitorPathRef>);
	print_stats(walk_path, walk_path.mean_ns_per_element);
	print_stats(template_path, walk_path.mean_ns_per_element);
	print_stats(function_path, walk_path.mean_ns_per_element);

	print_section_header("uid_scan");
	const auto walk_uid =
	    bench_walk(
	        "walk range-for", options, dataset,
	        workload_uid_scan<dicom::DataSetWalkPathRef>);
	const auto template_uid = bench_template_visitor(
	    "generic visitor (template)", options, dataset,
	    workload_uid_scan<BenchVisitorPathRef>);
	const auto function_uid = bench_function_visitor(
	    "generic visitor (std::function)", options, dataset,
	    workload_uid_scan<BenchVisitorPathRef>);
	print_stats(walk_uid, walk_uid.mean_ns_per_element);
	print_stats(template_uid, walk_uid.mean_ns_per_element);
	print_stats(function_uid, walk_uid.mean_ns_per_element);

	const int rewrite_inner_iterations =
	    std::max(1, options.inner_iterations / 20);
	const auto temp_root =
	    std::filesystem::temp_directory_path() / "dicomsdl_rewrite_uids_walk_bench";
	std::error_code cleanup_ec;
	std::filesystem::remove_all(temp_root, cleanup_ec);
	std::filesystem::create_directories(temp_root, cleanup_ec);
	if (cleanup_ec) {
		std::cerr << "Failed to create rewrite benchmark temp directory\n";
		return 1;
	}

	RewriteBenchState rewrite_walk_state;
	RewriteBenchState rewrite_template_state;
	RewriteBenchState rewrite_function_state;
	initialize_rewrite_state(rewrite_walk_state, temp_root / "walk.tsv");
	initialize_rewrite_state(rewrite_template_state, temp_root / "template.tsv");
	initialize_rewrite_state(rewrite_function_state, temp_root / "function.tsv");

	print_section_header("rewrite_uids_hot");
	std::cout << "rewrite_inner_iterations=" << rewrite_inner_iterations
	          << " rewrite_targets=" << rewrite_walk_state.targets.size() << "\n";

	const auto restore_only = run_bench_per_op(
	    "restore only", options, rewrite_inner_iterations, [&] {
		    restore_rewrite_targets(rewrite_walk_state.targets);
	    });

	const auto rewrite_walk = run_bench_per_op(
	    "rewrite_uids walk + restore", options, rewrite_inner_iterations, [&] {
		    const auto rewritten = rewrite_uids_walk(
		        rewrite_walk_state.dataset.file, rewrite_walk_state.remapper);
		    if (rewritten != rewrite_walk_state.targets.size()) {
			    throw std::runtime_error("rewrite_uids walk target count mismatch");
		    }
		    restore_rewrite_targets(rewrite_walk_state.targets);
	    });

	const auto rewrite_template = run_bench_per_op(
	    "rewrite_uids template + restore", options, rewrite_inner_iterations, [&] {
		    const auto rewritten = rewrite_uids_template_visitor(
		        rewrite_template_state.dataset.file, rewrite_template_state.remapper);
		    if (rewritten != rewrite_template_state.targets.size()) {
			    throw std::runtime_error("rewrite_uids template target count mismatch");
		    }
		    restore_rewrite_targets(rewrite_template_state.targets);
	    });

	const auto rewrite_function = run_bench_per_op(
	    "rewrite_uids std::function + restore", options, rewrite_inner_iterations, [&] {
		    const auto rewritten = rewrite_uids_function_visitor(
		        rewrite_function_state.dataset.file, rewrite_function_state.remapper);
		    if (rewritten != rewrite_function_state.targets.size()) {
			    throw std::runtime_error("rewrite_uids function target count mismatch");
		    }
		    restore_rewrite_targets(rewrite_function_state.targets);
	    });

	print_op_stats(restore_only, 0.0);
	print_op_stats(rewrite_walk, rewrite_walk.mean_ns_per_op);
	print_op_stats(rewrite_template, rewrite_walk.mean_ns_per_op);
	print_op_stats(rewrite_function, rewrite_walk.mean_ns_per_op);

	std::cout << "approx_net_after_restore_subtraction:\n";
	std::cout << std::left << std::setw(34) << "rewrite_uids walk"
	          << std::fixed << std::setprecision(2)
	          << (rewrite_walk.mean_ns_per_op - restore_only.mean_ns_per_op)
	          << " ns/pass\n";
	std::cout << std::left << std::setw(34) << "rewrite_uids template"
	          << (rewrite_template.mean_ns_per_op - restore_only.mean_ns_per_op)
	          << " ns/pass\n";
	std::cout << std::left << std::setw(34) << "rewrite_uids std::function"
	          << (rewrite_function.mean_ns_per_op - restore_only.mean_ns_per_op)
	          << " ns/pass\n";

	rewrite_walk_state.remapper.close();
	rewrite_template_state.remapper.close();
	rewrite_function_state.remapper.close();
	std::filesystem::remove_all(temp_root, cleanup_ec);

	std::cout << "\nsink=" << g_sink << "\n";
	return 0;
}
