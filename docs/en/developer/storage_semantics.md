# Storage Semantics Layer

This page describes the storage semantics layer built around the `storage/*` API that sits above the core DICOM dataset API.

## Scope

The storage semantics layer is for storage-oriented object semantics:

- SOP Class UID to storage IOD mapping
- IOD component/module listing
- declared attribute type lookup (`1`, `1C`, `2`, `2C`, `3`, `unknown`)
- effective type evaluation against a concrete `DataSet`
- recursive storage context traversal for structures such as SR content trees

This layer is intentionally separate from:

- `dicom.h` core file/dataset/tag/UID APIs
- DIMSE/network protocol handling
- normalized/service-oriented SOP classes such as Storage Commitment, UPS, MPPS, Print, or Query/Retrieve

## Public C++ entry points

Core umbrella:

```cpp
#include <dicom.h>
```

Storage semantics umbrella:

```cpp
#include <storage/storage.hpp>
```

Main headers:

- `include/storage/storage_classifier.hpp`
- `include/storage/storage_dataset.hpp`
- `include/storage/storage_effective.hpp`
- `include/storage/storage_listing.hpp`
- `include/storage/storage_lookup.hpp`
- `include/storage/storage_registry.hpp`

Recommended public entry points:

- `dicom::storage::StorageClassifier`
- `dicom::storage::make_storage_classifier(...)`
- `dicom::storage::list_modules(...)`
- `dicom::storage::list_attributes(...)`
- `dicom::storage::list_effective_modules(...)`
- `dicom::storage::list_effective_attributes(...)`

## Layering

The intended layering is:

1. `dicom.h`
   - raw DICOM primitives
   - `DicomFile`, `DataSet`, `DataElement`, `Tag`, `VR`, UID/tag lookup
2. `storage/*`
   - storage object semantics
   - IOD/module/attribute/type interpretation
3. higher-level tools
   - anonymizer
   - validator
   - schema-aware editors

Do not treat the storage layer as a replacement for `dicom.h`. It depends on concrete dataset content and generated storage registries.

## Dictionary and registry pipeline

The storage layer is generated from standard XML plus extracted TSV intermediates.

Main inputs:

- `misc/dictionary/part03.xml`
- `misc/dictionary/part04.xml`
- `misc/dictionary/_uid_registry.tsv`

Main extracted TSVs:

- `misc/dictionary/_sopclass_iod_map.tsv`
- `misc/dictionary/_iod_component_registry.tsv`
- `misc/dictionary/_component_attribute_rules.tsv`
- `misc/dictionary/_storage_context_registry.tsv`
- `misc/dictionary/_storage_context_transition_registry.tsv`
- `misc/dictionary/_storage_context_rule_index_registry.tsv`
- `misc/dictionary/_storage_external_conditions.tsv`
- `misc/dictionary/_iod_attribute_overrides.tsv`

Generated outputs:

- `include/storage/storage_registry.hpp`
- `src/storage/storage_registry.cpp`

Normal refresh path:

```bash
misc/dictionary/update_dictionaries.sh
```

This now refreshes both the core registries and the storage semantics registry.

## Declared versus effective type

The layer keeps two related but different concepts:

- `TypeDesignation`
  - the declared PS3.3 table type
  - `Type1`, `Type1C`, `Type2`, `Type2C`, `Type3`, `Unknown`
- `EffectiveType`
  - the evaluated result for a concrete dataset
  - `Type1`, `Type2`, `Type3`, `Prohibited`, `Unknown`

`1C` and `2C` remain conditional until evaluated against a dataset and optional external condition context.

## Condition evaluation

Natural-language PS3.3 conditional clauses are compiled by the generator into a compact condition IR.

Examples of conditions handled internally:

- tag present / not present / empty
- tag text equals / not equals
- value `N` equals / not equals
- simple `and` / `or`
- selected tag-to-tag comparisons
- waveform filter context special cases

Conditions that cannot be reduced to a single-dataset predicate are represented as external conditions and may require caller-supplied context.

Relevant runtime types:

- `ConditionState`
- `ConditionHandlingPolicy`
- `ConditionEvaluationContext`
- `ConditionEvaluationRequest`

Relevant inspection helpers:

- `collect_condition_issues(...)`
- `collect_storage_condition_issues(...)` in Python

The default policy is `BestEffort`. In that mode unresolved conditional module usage may still be treated as active when the component is already present in the dataset. `Strict` disables that fallback and keeps unresolved conditions visible through the condition report APIs.

The generator also emits `misc/dictionary/_storage_external_conditions.tsv`, which lists every module usage or attribute rule that still compiles to `StorageConditionOp::External`.

## Recursive storage contexts

Some storage structures are recursive at the specification level. Structured Reporting is the main example.

The storage layer keeps two parallel ideas:

- flat generated registry tables for storage contexts and context transitions
- runtime traversal state that tracks the current active context set plus the actual path stack in the instance

This allows recursive spec constructs to be modeled without building dynamic trees at runtime.

## Minimal C++ example

```cpp
#include <storage/storage.hpp>
#include <dicom.h>

int main() {
  dicom::DataSet ds;
  ds.set_value("SOPClassUID", "1.2.840.10008.5.1.4.1.1.2");

  auto classifier = dicom::storage::make_storage_classifier(ds);
  if (!classifier) {
    return 1;
  }

  auto modules = dicom::storage::list_modules(*classifier);
  auto attributes = dicom::storage::list_effective_attributes(ds);
  return modules.empty() || attributes.empty();
}
```

## Python bindings

Python exposes a practical subset of this layer.

Use the recommended alias:

```python
import dicomsdl as dicom
```

Available Python entry points:

- `dicom.StorageClassifier`
- `dicom.make_storage_classifier(...)`
- `dicom.list_effective_storage_modules(...)`
- `dicom.list_effective_storage_attributes(...)`
- `dicom.collect_storage_condition_issues(...)`

The Python API returns `list[dict]` summaries rather than exposing every C++ registry struct directly.

Example:

```python
import dicomsdl as dicom

cls = dicom.StorageClassifier.from_sop_class_keyword("CTImageStorage")
mods = cls.list_modules()

ds = dicom.DataSet()
ds.set_value("SOPClassUID", "1.2.840.10008.5.1.4.1.1.2")

attrs = dicom.list_effective_storage_attributes(ds, keyword="PatientName")
```

## Naming conventions

Current naming is intentional:

- `storage_*`
  - storage-object semantics layer
- `TypeDesignation`
  - PS3.3 declared type terminology
- `EffectiveType`
  - evaluated runtime type result
- `ModuleUsage`
  - PS3.3 module usage column

`dicom.h` remains the core umbrella. Storage semantics is kept under `storage/storage.hpp` so the semantic layer stays explicit.
