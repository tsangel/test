# Dictionary Generator Usage

`misc/dictionary`는 DICOM dictionary 계열 산출물을 갱신하는 경로다.
`misc/charset`와 달리 dictionary 쪽은 표준 개정 시 갱신 빈도가 높아서, 업데이트 절차와 일상적인 재생성 절차를 분리해서 관리한다.

## 대상 산출물

- `misc/dictionary/_dataelement_registry.tsv`
- `misc/dictionary/_uid_registry.tsv`
- `misc/dictionary/_specific_character_sets.tsv`
- `misc/dictionary/_sopclass_iod_map.tsv`
- `misc/dictionary/_iod_component_registry.tsv`
- `misc/dictionary/_component_attribute_rules.tsv`
- `misc/dictionary/_storage_context_registry.tsv`
- `misc/dictionary/_storage_context_transition_registry.tsv`
- `misc/dictionary/_storage_context_rule_index_registry.tsv`
- `misc/dictionary/_storage_external_conditions.tsv`
- `misc/dictionary/_iod_attribute_overrides.tsv`
- `misc/dictionary/_dicom_version.txt`
- `include/dataelement_registry.hpp`
- `include/dataelement_lookup_tables.hpp`
- `include/uid_registry.hpp`
- `include/uid_lookup_tables.hpp`
- `include/specific_character_set_registry.hpp`
- `include/storage/storage_registry.hpp`
- `include/storage/storage_lookup.hpp`
- `include/storage/storage_classifier.hpp`
- `include/storage/storage_dataset.hpp`
- `include/storage/storage_effective.hpp`
- `include/storage/storage_listing.hpp`
- `src/storage/storage_registry.cpp`

## 주요 스크립트

- `extract_part06_tables.py`
  - DICOM Part 06 XML에서 tag registry, UID registry, DICOM version을 추출한다.
- `extract_part03_specific_character_sets.py`
  - DICOM Part 03 XML에서 Specific Character Set 표를 추출한다.
- `extract_part04_sopclass_iod_map.py`
  - DICOM Part 04 XML의 Storage Service Class 표에서 `SOP Class UID -> IOD` 매핑 TSV를 추출한다.
- `extract_part03_iod_tables.py`
  - DICOM Part 03 XML에서 IOD component usage, component attribute rule, recursive storage context, sparse override scaffold TSV를 추출한다.
- `generate_dataelement_registry.py`
  - `_dataelement_registry.tsv`에서 `include/dataelement_registry.hpp`를 생성한다.
- `generate_lookup_tables.py`
  - `_dataelement_registry.tsv`에서 keyword/tag lookup CHD 테이블을 생성한다.
- `generate_uid_registry.py`
  - `_uid_registry.tsv`에서 `include/uid_registry.hpp`를 생성한다.
- `generate_uid_lookup_tables.py`
  - `_uid_registry.tsv`에서 UID lookup CHD 테이블을 생성한다.
- `generate_specific_character_set_registry.py`
  - `_specific_character_sets.tsv`에서 `include/specific_character_set_registry.hpp`를 생성한다.
- `generate_storage_registry.py`
  - IOD registry TSV들에서 `include/storage/storage_registry.hpp`, `src/storage/storage_registry.cpp`, `_storage_external_conditions.tsv`를 생성한다.
- `update_dictionaries.sh`
  - extract + generate + version sync를 한 번에 수행한다.

## 일상적인 재생성

표준 XML을 다시 받지 않고, 현재 저장소에 들어 있는 TSV만 기준으로 generated header를 다시 만들고 싶을 때 쓴다.

PowerShell 예시:

```powershell
python misc/dictionary/generate_dataelement_registry.py `
  --source misc/dictionary/_dataelement_registry.tsv `
  --output include/dataelement_registry.hpp

python misc/dictionary/generate_lookup_tables.py `
  --registry misc/dictionary/_dataelement_registry.tsv `
  --output include/dataelement_lookup_tables.hpp

python misc/dictionary/generate_uid_registry.py `
  --source misc/dictionary/_uid_registry.tsv `
  --output include/uid_registry.hpp

python misc/dictionary/generate_uid_lookup_tables.py `
  --source misc/dictionary/_uid_registry.tsv `
  --output include/uid_lookup_tables.hpp

python misc/dictionary/generate_specific_character_set_registry.py `
  --source misc/dictionary/_specific_character_sets.tsv `
  --output include/specific_character_set_registry.hpp

python misc/dictionary/generate_storage_registry.py `
  --uid-registry-source misc/dictionary/_uid_registry.tsv `
  --sopclass-iod-source misc/dictionary/_sopclass_iod_map.tsv `
  --iod-component-source misc/dictionary/_iod_component_registry.tsv `
  --component-rule-source misc/dictionary/_component_attribute_rules.tsv `
  --context-source misc/dictionary/_storage_context_registry.tsv `
  --context-transition-source misc/dictionary/_storage_context_transition_registry.tsv `
  --context-rule-index-source misc/dictionary/_storage_context_rule_index_registry.tsv `
  --override-source misc/dictionary/_iod_attribute_overrides.tsv `
  --header-output include/storage/storage_registry.hpp `
  --source-output src/storage/storage_registry.cpp
```

권장 사항:

- `--source`, `--registry`, `--output`는 저장소 루트 기준 상대경로로 넘긴다.
- `dataelement_registry.hpp`, `uid_registry.hpp`, `specific_character_set_registry.hpp`는 source 경로를 헤더 주석에 남기므로, 절대경로를 넘기면 불필요한 diff가 생긴다.
- generator는 `write-if-changed` 방식이라 내용이 같으면 파일 timestamp만 바꾸지 않는다.

## 표준 업데이트

새 DICOM 릴리스를 반영할 때는 `update_dictionaries.sh`를 우선 사용한다.

```bash
misc/dictionary/update_dictionaries.sh
```

이 스크립트는 다음 순서로 동작한다.

1. Part 06 XML에서 tag/UID registry와 DICOM version 추출
2. `dataelement_registry.hpp` 재생성
3. `dataelement_lookup_tables.hpp` 재생성
4. `uid_registry.hpp` 재생성
5. `uid_lookup_tables.hpp` 재생성
6. Part 03 XML에서 Specific Character Set 표 추출
7. `specific_character_set_registry.hpp` 재생성
8. Part 04 XML에서 `_sopclass_iod_map.tsv` 추출
9. Part 03 XML에서 `_iod_component_registry.tsv`, `_component_attribute_rules.tsv`, `_storage_context_*`, `_iod_attribute_overrides.tsv` 추출
10. `include/storage/storage_registry.hpp`, `src/storage/storage_registry.cpp` 재생성
11. `include/dicom_const.h`의 `DICOM_STANDARD_VERSION` 동기화

추가 동작:

- `misc/dictionary/_dicom_version.txt`를 갱신한다.
- Git 기준으로 이전 버전과 달라지면 `misc/dictionary/dictionary_diff_<old>_to_<new>.md` 보고서를 만든다.

전제 조건:

- `bash`
- `python3`
- `git`
- 네트워크 접근

Python 실행 파일을 바꾸고 싶으면 다음처럼 지정한다.

```bash
PYTHON_BIN=/path/to/python3 misc/dictionary/update_dictionaries.sh
```

## extract만 따로 실행할 때

표준 XML 추출 결과만 갱신하고 generator는 나중에 돌리고 싶을 때 쓴다.

Part 06:

```powershell
python misc/dictionary/extract_part06_tables.py
```

출력:

- `misc/dictionary/_dataelement_registry.tsv`
- `misc/dictionary/_uid_registry.tsv`
- `misc/dictionary/_dicom_version.txt`

Part 03:

```powershell
python misc/dictionary/extract_part03_specific_character_sets.py
```

출력:

- `misc/dictionary/_specific_character_sets.tsv`

Part 04 / IOD bootstrap:

```powershell
python misc/dictionary/extract_part04_sopclass_iod_map.py
python misc/dictionary/extract_part03_iod_tables.py
```

출력:

- `misc/dictionary/_sopclass_iod_map.tsv`
- `misc/dictionary/_iod_component_registry.tsv`
- `misc/dictionary/_component_attribute_rules.tsv`
- `misc/dictionary/_storage_context_registry.tsv`
- `misc/dictionary/_storage_context_transition_registry.tsv`
- `misc/dictionary/_storage_context_rule_index_registry.tsv`
- `misc/dictionary/_storage_external_conditions.tsv`
- `misc/dictionary/_iod_attribute_overrides.tsv`

기본 동작은 `current` DICOM XML을 내려받아 `misc/dictionary/part06.xml`, `misc/dictionary/part03.xml`로 저장한 뒤 추출한다.
원하면 XML 경로를 직접 인자로 넘길 수 있다.

예시:

```powershell
python misc/dictionary/extract_part06_tables.py misc/dictionary/part06.xml
python misc/dictionary/extract_part03_specific_character_sets.py misc/dictionary/part03.xml
python misc/dictionary/extract_part04_sopclass_iod_map.py --part04 misc/dictionary/part04.xml
python misc/dictionary/extract_part03_iod_tables.py --part03 misc/dictionary/part03.xml
```

참고:

- `_iod_attribute_overrides.tsv`는 sparse manual override용 scaffold다.
- `_component_attribute_rules.tsv`에서 PS3.3 표에 `Type` 컬럼이 없는 row는 `type=unknown`으로 기록한다.
- `_storage_context_*` TSV는 recursive include를 flat context/transition/rule-index registry로 보존한다.
- `_storage_external_conditions.tsv`는 generator가 내부 IR로 내리지 못한 `External` 조건의 실제 발생 위치를 usage/rule 단위로 기록한다.
- `update_dictionaries.sh`는 storage IOD TSV와 `storage_registry.hpp/.cpp`도 함께 갱신한다.
- generator regression 범위는 아직 storage IOD generator까지 포함하지 않는다.
- public API는 `storage_*` header를 기준으로 사용한다.
- listing/query entry point는 `include/storage/storage_listing.hpp`다.
- recursive context lookup/traversal entry point는 `include/storage/storage_lookup.hpp`다.

## 회귀 테스트

Dictionary generator는 별도 회귀 테스트로 checked-in header와 재생성 결과가 동일한지 확인한다.

```powershell
ctest --test-dir build-msyscheck -R dictionary_generator_regression --output-on-failure
```

이 테스트는 다음만 검증한다.

- `_dataelement_registry.tsv -> dataelement_registry.hpp`
- `_dataelement_registry.tsv -> dataelement_lookup_tables.hpp`
- `_uid_registry.tsv -> uid_registry.hpp`
- `_uid_registry.tsv -> uid_lookup_tables.hpp`
- `_specific_character_sets.tsv -> specific_character_set_registry.hpp`

즉, 네트워크 다운로드나 표준 추출 단계는 회귀 테스트 범위에 넣지 않는다.
회귀 테스트는 deterministic한 TSV -> header 생성만 본다.

## 권장 작업 흐름

일상적인 코드 수정 중:

1. 필요한 TSV를 직접 수정하거나 유지
2. 관련 generator만 다시 실행
3. `dictionary_generator_regression` 실행

표준 버전 업데이트 시:

1. `misc/dictionary/update_dictionaries.sh` 실행
2. 생성된 TSV/header diff 검토
3. 버전 보고서 확인
4. 관련 smoke/regression 테스트 실행

## 주의사항

- `update_dictionaries.sh`는 Git 정보를 사용해 diff 보고서를 만든다.
- extract 스크립트는 기본적으로 NEMA `current` XML을 받으므로 실행 시점에 따라 결과가 달라질 수 있다.
- 반대로 generator regression은 저장소에 들어 있는 `_*.tsv`만 쓰므로 재현 가능해야 한다.
