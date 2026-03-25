# DICOMSDL 다음 작업 TODO

## 1. 현재 우선순위

현재 우선순위는 패키징보다 문서화 마무리다.
wheel 호환성 확대, `musllinux`, `STABLE_ABI`, NumPy 설치 범위 비교 같은 작업은
문서화가 끝난 뒤에 진행한다.

## 2. 문서화 마무리 작업

- [ ] `docs/en`, `docs/ko`, `docs/ja`, `docs/zh-cn` 전반에 대해 문장 톤과 용어를 한 번 더 통일한다.
- [ ] `guide` 문서에서 `C++ 먼저, Python 병기` 원칙이 실제로 일관되게 적용됐는지 다시 점검한다.
- [ ] `quickstart`, `installation`, `core_objects`, `file_io`, `reading_data`, `writing_data`, `pixel_decode`, `pixel_encode`, `error_handling`의 예제가 실제 API와 완전히 맞는지 재검토한다.
- [ ] `ja`, `zh-cn` 문서 중 아직 영문 설명이 많이 남아 있는 페이지를 문서 단위로 다시 쓴다.
- [ ] `developer` 섹션의 번역 상태가 `pending`인 페이지를 정리한다.
- [ ] 언어 전환 링크와 번역 상태 배너가 모든 핵심 페이지에서 자연스럽게 보이는지 확인한다.
- [ ] Quickstart와 Installation의 설치 경로 설명이 서로 모순되지 않는지 최종 점검한다.
- [ ] 대표 예제와 관련 문서 링크가 문서 구조 변경 후에도 모두 유효한지 다시 확인한다.
- [ ] `./build_docs.sh check`와 `./build_docs.sh html-all` 기준으로 최종 문서 검수를 한 번 더 수행한다.

## 3. 문서화 이후 다음 작업

문서화가 끝나면 아래 패키징/배포 작업을 다음 우선순위로 진행한다.

- [ ] `dicomsdl` base wheel이 `numpy` 없이 설치 가능한 환경 범위를 정리한다.
- [ ] NumPy wheel 지원 범위와 `dicomsdl` wheel 지원 범위를 비교해, 실제로 어디서 우리가 더 유리한지/불리한지 정리한다.
- [ ] `musllinux` wheel 지원 가능성을 검토한다.
- [ ] `nanobind STABLE_ABI (abi3)` 적용 가능성과 성능/호환성 trade-off를 검토한다.
- [ ] `cibuildwheel` 설정을 기준으로 실제 배포할 Python/OS/arch matrix 초안을 만든다.
- [ ] `pip install dicomsdl`, `pip install "dicomsdl[numpy]"`, `pip install "dicomsdl[numpy,pil]"`의 문서/패키징 메시지가 일관되게 맞는지 점검한다.
- [ ] PyPI 공개 전 smoke test 시나리오를 정리한다.
- [ ] encapsulated `PixelData`의 C++ 사용성 개선을 검토한다. 다만 `DataElement::value_span()`이 첫 프레임 encoded bytes를 돌려주도록 의미를 바꾸기보다는, `PixelSequence::frame_encoded_span()`을 더 쉽게 쓸 수 있는 명시적 forwarding helper(`DataElement` 또는 `DicomFile` 쪽)를 추가하는 방향이 더 적절한지 평가한다.

## 4. 메모

- 문서가 끝나기 전에는 패키징 범위를 넓히는 작업을 먼저 시작하지 않는다.
- 문서에서 `server-only` 사용 사례를 이미 강조하고 있으므로, 패키징 확장은 그 다음 단계에서 맞춘다.
