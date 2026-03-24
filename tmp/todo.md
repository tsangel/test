# TODO

- encapsulated `PixelData`의 C++ 사용성 개선 검토
  - `DataElement::value_span()`이 첫 프레임 encoded bytes를 반환하도록 의미를 바꾸는 것은 보류
  - 대신 `PixelSequence::frame_encoded_span()`을 더 쉽게 쓸 수 있는 명시적 forwarding helper를 `DataElement` 또는 `DicomFile`에 추가할지 평가
- quickstart transcode 예제 단순화 검토
  - Python에 `df.write_with_transfer_syntax("out.dcm", "HTJ2KLossless")`가 추가되었으므로, quickstart의 transcode 흐름을 `set_transfer_syntax(...)` + `write_file(...)`보다 이 one-shot 경로 중심으로 더 단순화할지 평가
- Python `ensure_loaded(...)` 후속 검토
  - 현재는 `Tag`, packed `int`, flat keyword string만 지원하고 dotted tag-path string은 지원하지 않음
  - nested path 기준 continuation API가 실제로 필요한지, 아니면 flat tag 경계 기반 API로 충분한지 사용 사례를 모아 평가
- C++/Python tag-path indexing 정리 검토
  - C++ `operator[]`도 keyword / dotted tag-path string을 받을 수 있게 되었으므로, 예제와 레퍼런스에서 `operator[]`, `get_dataelement(...)`, `get_value(...)`의 역할 분담을 더 선명하게 정리할지 검토
- nested `ensure_dataelement(...)` 경로 materialization 설명 보강
  - 기존 non-sequence intermediate element를 `SQ`로 reset해 경로를 만드는 동작이 surprising할 수 있으므로, 이 동작이 실제 사용자 코드에서 안전한 패턴인지 예제/주의사항을 더 보강할지 평가
