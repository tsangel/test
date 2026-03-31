#include "dicom.h"
#include "storage/storage.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string_view>
#include <vector>

using namespace dicom::literals;

namespace {

using clock_type = std::chrono::steady_clock;

struct BenchCase {
    const char* name{};
    int iterations{0};
    dicom::DataSet dataset{};
};

volatile std::uint64_t g_sink = 0;

void expect(bool ok, std::string_view message) {
    if (!ok) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

std::vector<dicom::storage::EffectiveAttributeInfo> old_list_effective_attributes(
    const dicom::storage::StorageClassifier& classifier,
    const dicom::DataSet& dataset,
    const dicom::storage::EffectiveAttributeListOptions& options = {},
    const dicom::storage::ConditionEvaluationContext& context = {}) {
    std::vector<dicom::storage::EffectiveAttributeInfo> attributes;
    const auto modules = dicom::storage::evaluate_components(dataset, classifier, context);
    for (const auto& module : modules) {
        if (!module.component) {
            continue;
        }
        const auto rules = classifier.component_rules(*module.component);
        attributes.reserve(attributes.size() + rules.size());
        const auto module_info =
            dicom::storage::EffectiveModuleInfo{classifier.sop_class_entry(), module};
        for (const auto& rule : rules) {
            const auto evaluated = dicom::storage::evaluate_rule(
                dataset,
                dicom::storage::DeclaredRuleRef{module.component, &rule},
                context);
            const auto info = dicom::storage::EffectiveAttributeInfo{
                classifier.sop_class_entry(),
                module.component,
                &rule,
                evaluated.condition_state,
                evaluated.effective_type,
            };
            if (!dicom::storage::matches_effective_attribute_options(module_info, info, options)) {
                continue;
            }
            attributes.push_back(info);
        }
    }
    return attributes;
}

std::vector<dicom::storage::EffectiveAttributeInfo> ungrouped_scope_aware_attributes(
    const dicom::storage::StorageClassifier& classifier,
    const dicom::DataSet& dataset,
    const dicom::storage::EffectiveAttributeListOptions& options = {},
    const dicom::storage::ConditionEvaluationContext& context = {}) {
    std::vector<dicom::storage::EffectiveAttributeInfo> attributes;
    const auto modules = dicom::storage::evaluate_components(dataset, classifier, context);
    dicom::storage::detail::ScopeDatasetRegistry scope_datasets;
    bool scope_datasets_collected = false;

    for (const auto& module : modules) {
        if (!module.component) {
            continue;
        }
        const auto rules = classifier.component_rules(*module.component);
        attributes.reserve(attributes.size() + rules.size());
        const auto module_info =
            dicom::storage::EffectiveModuleInfo{classifier.sop_class_entry(), module};
        for (const auto& rule : rules) {
            const auto* path_node = dicom::storage::find_storage_path_node_entry(rule.path_node_index);
            if (path_node == nullptr) {
                continue;
            }
            const auto scope_node_index = path_node->parent_node_index;
            if (scope_node_index == 0) {
                const auto evaluated = dicom::storage::evaluate_rule(
                    dataset,
                    dicom::storage::DeclaredRuleRef{module.component, &rule},
                    context);
                const auto info = dicom::storage::EffectiveAttributeInfo{
                    classifier.sop_class_entry(),
                    module.component,
                    &rule,
                    evaluated.condition_state,
                    evaluated.effective_type,
                };
                if (!dicom::storage::matches_effective_attribute_options(module_info, info, options)) {
                    continue;
                }
                attributes.push_back(info);
                continue;
            }

            if (!scope_datasets_collected) {
                scope_datasets = dicom::storage::detail::collect_scope_datasets(dataset);
                scope_datasets_collected = true;
            }
            const auto* scopes =
                dicom::storage::detail::find_scope_datasets(scope_datasets, scope_node_index);
            if (scopes == nullptr || scopes->empty()) {
                continue;
            }

            for (const auto* scope_dataset : *scopes) {
                if (scope_dataset == nullptr) {
                    continue;
                }
                const auto evaluated = dicom::storage::evaluate_rule(
                    *scope_dataset,
                    dicom::storage::DeclaredRuleRef{module.component, &rule},
                    context);
                const auto info = dicom::storage::EffectiveAttributeInfo{
                    classifier.sop_class_entry(),
                    module.component,
                    &rule,
                    evaluated.condition_state,
                    evaluated.effective_type,
                };
                if (!dicom::storage::matches_effective_attribute_options(module_info, info, options)) {
                    continue;
                }
                attributes.push_back(info);
            }
        }
    }

    return attributes;
}

void fill_flat_ct_dataset(dicom::DataSet& ds) {
    expect(ds.set_value("SOPClassUID"_tag, "1.2.840.10008.5.1.4.1.1.2"), "set SOPClassUID");
    expect(ds.set_value("PatientName"_tag, "Bench^Flat"), "set PatientName");
    expect(ds.set_value("PatientID"_tag, "flat-001"), "set PatientID");
    expect(
        ds.set_value("StudyInstanceUID"_tag, "1.2.826.0.1.3680043.10.100.1"),
        "set StudyInstanceUID");
    expect(
        ds.set_value("SeriesInstanceUID"_tag, "1.2.826.0.1.3680043.10.100.2"),
        "set SeriesInstanceUID");
    expect(
        ds.set_value("SOPInstanceUID"_tag, "1.2.826.0.1.3680043.10.100.3"),
        "set SOPInstanceUID");
    expect(ds.set_value("Modality"_tag, "CT"), "set Modality");
    const std::array<std::string_view, 3> image_type{"ORIGINAL", "PRIMARY", "AXIAL"};
    expect(
        ds.set_value("ImageType"_tag, std::span<const std::string_view>(image_type)),
        "set ImageType");
    expect(ds.set_value("Rows"_tag, 512L), "set Rows");
    expect(ds.set_value("Columns"_tag, 512L), "set Columns");
}

void fill_sr_dataset(dicom::DataSet& ds, int item_count) {
    expect(
        ds.set_value("SOPClassUID"_tag, "1.2.840.10008.5.1.4.1.1.88.11"),
        "set SR SOPClassUID");
    expect(
        ds.set_value("StudyInstanceUID"_tag, "1.2.826.0.1.3680043.10.200.1"),
        "set SR StudyInstanceUID");
    expect(
        ds.set_value("SeriesInstanceUID"_tag, "1.2.826.0.1.3680043.10.200.2"),
        "set SR SeriesInstanceUID");
    expect(
        ds.set_value("SOPInstanceUID"_tag, "1.2.826.0.1.3680043.10.200.3"),
        "set SR SOPInstanceUID");

    auto* content = ds.ensure_dataelement("ContentSequence"_tag, dicom::VR::SQ).as_sequence();
    expect(content != nullptr, "create ContentSequence");
    for (int i = 0; i < item_count; ++i) {
        auto* item = content->add_dataset();
        expect(item != nullptr, "add SR content item");
        expect(item->set_value("RelationshipType"_tag, "CONTAINS"), "set RelationshipType");
        if ((i % 2) == 0) {
            expect(item->set_value("ValueType"_tag, "TEXT"), "set ValueType TEXT");
            expect(item->set_value("TextValue"_tag, "hello"), "set TextValue");
        } else {
            expect(item->set_value("ValueType"_tag, "PNAME"), "set ValueType PNAME");
            expect(item->set_value("PersonName"_tag, "Doe^John"), "set PersonName");
        }
    }
}

template <typename Fn>
double measure_ms(Fn&& fn, int iterations) {
    for (int i = 0; i < 5; ++i) {
        g_sink += static_cast<std::uint64_t>(fn().size());
    }
    const auto start = clock_type::now();
    for (int i = 0; i < iterations; ++i) {
        g_sink += static_cast<std::uint64_t>(fn().size());
    }
    const auto end = clock_type::now();
    return std::chrono::duration<double, std::milli>(end - start).count();
}

void run_case(const BenchCase& bench_case) {
    const auto classifier = dicom::storage::make_storage_classifier(bench_case.dataset);
    if (!classifier) {
        std::cerr << "classifier unavailable for " << bench_case.name << '\n';
        std::exit(1);
    }

    auto options = dicom::storage::EffectiveAttributeListOptions{};
    options.active_components_only = false;
    options.include_prohibited = true;
    options.include_unknown_effective_types = true;
    options.include_conditional_declared_types = true;

    const auto old_ms = measure_ms(
        [&] { return old_list_effective_attributes(*classifier, bench_case.dataset, options); },
        bench_case.iterations);
    const auto ungrouped_ms = measure_ms(
        [&] { return ungrouped_scope_aware_attributes(*classifier, bench_case.dataset, options); },
        bench_case.iterations);
    const auto new_ms = measure_ms(
        [&] { return dicom::storage::list_effective_attributes(*classifier, bench_case.dataset, options); },
        bench_case.iterations);

    const auto old_per_call = old_ms / static_cast<double>(bench_case.iterations);
    const auto ungrouped_per_call = ungrouped_ms / static_cast<double>(bench_case.iterations);
    const auto new_per_call = new_ms / static_cast<double>(bench_case.iterations);

    std::cout << bench_case.name << '\n';
    std::cout << "  iterations: " << bench_case.iterations << '\n';
    std::cout << "  old_root_only_ms_per_call: " << std::fixed << std::setprecision(6)
              << old_per_call << '\n';
    std::cout << "  ungrouped_scope_aware_ms_per_call: " << std::fixed << std::setprecision(6)
              << ungrouped_per_call << '\n';
    std::cout << "  new_scope_aware_ms_per_call: " << std::fixed << std::setprecision(6)
              << new_per_call << '\n';
    std::cout << "  ratio_ungrouped_over_old: " << std::fixed << std::setprecision(3)
              << (ungrouped_per_call / old_per_call) << '\n';
    std::cout << "  ratio_new_over_old: " << std::fixed << std::setprecision(3)
              << (new_per_call / old_per_call) << '\n';
    std::cout << "  ratio_new_over_ungrouped: " << std::fixed << std::setprecision(3)
              << (new_per_call / ungrouped_per_call) << '\n';
}

} // namespace

int main() {
    BenchCase flat_ct{.name = "flat_ct", .iterations = 500};
    fill_flat_ct_dataset(flat_ct.dataset);

    BenchCase sr_2{.name = "sr_2_items", .iterations = 300};
    fill_sr_dataset(sr_2.dataset, 2);

    BenchCase sr_100{.name = "sr_100_items", .iterations = 60};
    fill_sr_dataset(sr_100.dataset, 100);

    BenchCase sr_500{.name = "sr_500_items", .iterations = 12};
    fill_sr_dataset(sr_500.dataset, 500);

    run_case(flat_ct);
    run_case(sr_2);
    run_case(sr_100);
    run_case(sr_500);

    std::cout << "sink: " << g_sink << '\n';
    return 0;
}
