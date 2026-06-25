# SEG Geometry / Overlay Helper 구현 체크리스트

## 목표

- [ ] `FrameOfReferenceUID` 기반 overlay compatibility helper를 제공한다.
- [ ] DICOM image plane metadata를 이용해 index/world transform을 계산한다.
- [ ] SEG frame geometry와 일반 DICOM image geometry를 같은 타입으로 표현한다.
- [x] DICOM slice stack에서 `ImageVolumeGeometry`를 만들 수 있는 planning helper를 제공한다.
- [ ] Enhanced multi-frame image는 전체 frame을 곧바로 한 volume으로 보지 않고, stack/dimension grouping을 먼저 수행한다.
- [ ] non-uniform slice stack에 대해서도 sorted slice, gap, uniform run 분석 결과를 제공한다.
- [ ] `check_overlay_compatibility()`는 hot loop용이 아니라 O(1) preflight helper로 정의한다.
- [ ] per-pixel/per-voxel 변환 hot path는 미리 만든 typed transform 객체만 사용한다.
- [ ] viewer, rendering, resampling, pixel volume decode, mask volume reconstruction은 후속 작업으로 분리한다.

## 참고 설계

- [ ] `imagestation-ncm.git/src/apps/volview/geometry/image_geometry.hpp`의 `Origin / Spacing / Direction / ImagePoint / WorldPoint` 모델을 DicomSDL에 맞게 adaptation한다.
- [ ] `imagestation-ncm.git/src/apps/volview/volume/ct_uniform_grid.*`의 normal projection 기반 slice placement 개념을 일반 DICOM slice stack planning에 adaptation한다.
- [ ] `testworks.git/volview2/src/volview2/timagegeometry.*`의 matrix 및 index-world 계산 개념을 참고한다.
- [ ] Qt, viewport, viewer plane UI 개념은 DicomSDL core에 넣지 않는다.
- [ ] ITK/SimpleITK의 `Origin`, `Spacing`, `Direction`, `TransformIndexToPhysicalPoint`, `TransformPhysicalPointToContinuousIndex` naming 감각을 반영한다.

## Public API 초안

- [x] 새 헤더 후보: `dicom_geometry.h`
- [x] namespace 후보: `dicom::geometry`
- [ ] nested DICOM element lookup 설계는 `tmp/element_path_dataset_lookup_checklist.ko.md`로 분리한다.
- [ ] geometry public surface는 `ImagePlaneGeometry`, `ImageVolumeGeometry`, `FrameGeometryReader`, slice stack/overlay helper로 정리한다.

### 구현 전 확정 결정

- [x] Geometry 객체는 public raw constructor 대신 validated factory로 만든다.
  - `ImagePlaneGeometryParams` / `ImageVolumeGeometryParams`를 받고 `GeometryBuildResult<T>`를 반환한다.
  - DICOM factory, 테스트, viewer/resampling code도 같은 factory를 사용해 invariant를 공유한다.
  - 구현 내부의 unchecked 생성자는 `detail`에 숨기고 public API로 노출하지 않는다.
- [x] SEG frame geometry는 strict PerFrame/Shared Functional Groups만 지원한다.
  - `plane_from_seg_frame(const seg::Segmentation&, frame_index)`에는 root dataset fallback 옵션을 두지 않는다.
  - legacy root fallback이 필요해지면 `plane_from_seg_dataset(const DataSet&, frame_index, options)` 같은 raw dataset 기반 별도 API로 검토한다.
- [x] `plane_from_multiframe_image()`는 overlay용 regular plane만 반환한다.
  - `VolumetricProperties=SAMPLED` / `DISTORTED`는 항상 실패 상태로 반환한다.
  - sampled/distorted metadata inspection은 `frame_geometry_from_multiframe_image()`만 담당하고 `ImageFrameGeometryKind`를 보존한다.
  - `ImageFrameGeometryKind` 판정은 SOP Class별 Frame Type Functional Group의 `VolumetricProperties (0008,9206)`를 우선한다.
  - Enhanced MR/CT/PET은 각각 SOP Class별 Frame Type Sequence에서 `VolumetricProperties`를 resolve한다.
  - root `VolumetricProperties=MIXED`는 frame kind로 사용하지 않고, frame-level 값을 찾지 못하면 실패로 처리한다.
  - NM Image IOD는 이 `VolumetricProperties` 기반 판정 경로에 넣지 않고 NM-specific frame vector path로 분리한다.
- [x] Enhanced multi-frame grouping은 Dimension metadata 기반을 기본으로 한다.
  - `DimensionIndexSequence`와 frame별 `DimensionIndexValues`를 descriptor/value 쌍으로 해석한다.
  - `StackID`는 stack identity, `InStackPositionNumber`는 stack 내부 ordering hint로 사용한다.
  - `InStackPositionNumber`, plane position, image position처럼 slice 위치를 나타내는 spatial dimension은 grouping key의 non-spatial values에서 제외한다.
  - echo/time/phase/volume 같은 non-spatial dimension value는 descriptor와 함께 grouping key에 포함한다.
  - `DimensionIndexSequence`가 없으면 기본값으로 `missing_dimension_module`이며, geometry-only grouping fallback은 `ImageFrameStackOptions::allow_geometry_grouping_fallback = true`일 때만 허용한다.
  - geometry-only grouping fallback은 `FrameContentSequence`, `StackID`, `InStackPositionNumber`를 요구하지 않고 frame geometry 분석으로 단일 후보 stack을 검증한다.
  - `DimensionIndexValues` 개수는 `DimensionIndexSequence` descriptor 개수와 정확히 같아야 하며, 부족하거나 초과하면 malformed metadata로 실패한다.
  - tiled multi-frame image는 MVP에서 `unsupported_tiled_image`로 실패한다.
- [x] Result/status/issue 정책은 no-throw, priority fatal status, full issue list로 고정한다.
  - metadata 누락/형식 오류/unsupported 구조는 exception이 아니라 status/issue로 반환한다.
  - `status()`는 deterministic fatal priority에서 가장 높은 issue를 대표하고, 동률이면 입력 순서를 따른다.
  - `issues()`는 가능한 모든 원인을 source/frame/tag/message와 함께 담는다.
  - analysis 객체는 partial diagnostic result를 가질 수 있지만, plan 객체는 `ok()`가 아니면 `volume_geometry()`를 비운다.
- [x] OverlayCheck boolean 의미를 truth table로 고정한다.
  - `can_transform`: 같은 `FrameOfReferenceUID`이고 geometry가 유효해 좌표 변환을 정의할 수 있다.
    - 단, `OverlayCheckOptions::require_same_grid == true`이면 직접 overlay 가능한 같은 grid일 때만 true로 둔다.
  - `can_direct_overlay`: resampling 없이 같은 index grid에 직접 overlay/copy할 수 있다.
  - `requires_resampling`: 물리 extent가 겹치고 spacing/orientation/grid mapping 때문에 interpolation 또는 resampling이 필요하다.
  - 같은 grid에서 extent만 다른 경우는 `different_extent`로 보고하고, caller가 crop/pad/clip 정책으로 처리한다.
  - `overlaps_extent`: source와 target의 physical extent가 tolerance 안에서 교차한다.
  - plane-plane direct overlay는 normal뿐 아니라 `direction_i`/`direction_j` 축까지 같은 방향이어야 한다. in-plane 90도 회전은 `requires_resampling`, 축 반전은 `opposite_orientation`으로 처리한다.
- [x] Matrix convention은 row-major storage + column vector left multiply로 고정한다.
  - 저장은 `m[row][column]`.
  - 계산은 `world_h = index_to_world * index_h`.
  - composition은 `target_index_h = target_world_to_index * source_index_to_world * source_index_h`.
  - `ImagePlaneGeometry`의 homogeneous index는 `(i, j, normal_mm, 1)`이다.
- [x] DICOM nested element 접근은 별도 core 설계 문서로 분리한다.
  - geometry 구현은 DICOM nested metadata 위치 표현에 core `dicom::ElementPath`를 적극 사용한다.
  - dotted string path는 debug/test/prototype 용도로만 두고, geometry production path에서는 사용하지 않는다.
  - geometry factory와 `FrameGeometryReader`는 `DataSet::get_dataelement(ElementPath...)` / `DicomFile::get_dataelement(ElementPath...)` / `DataSet::sequence_item(...)`를 기본 lookup primitive로 사용한다.
  - geometry 쪽 반복 frame metadata 접근은 별도 resolver 타입을 공개하지 않고 `FrameGeometryReader` 하나로 감싼다.

### 첫 geometry 구현 커밋에서 고정할 계약

- [x] `ImageOrientationPatient` mapping은 코드 주석과 테스트로 고정한다.
  - DICOM 첫 triplet은 row direction cosine이지만, DicomSDL index convention에서는 `i=column`이므로 `direction_i = IOP[0..2]`가 된다.
  - DICOM 두 번째 triplet은 column direction cosine이지만, DicomSDL index convention에서는 `j=row`이므로 `direction_j = IOP[3..5]`가 된다.
- [x] geometry factory와 `FrameGeometryReader`는 metadata 누락/형식 오류/unsupported frame에서 throw하지 않는다.
  - SEG의 throwing high-level accessor를 geometry factory 구현에 직접 사용하지 않는다.
  - raw `DataSet*`, `sequence_item()`, `ElementPath` 기반 lookup으로 실패를 `GeometryBuildStatus`에 접는다.
- [x] `OverlayCompatibility` 대표 `status` 우선순위는 구현 전에 테스트 기준으로 고정한다.
  - 여러 문제가 동시에 발견되어도 `status`는 deterministic하게 하나를 대표한다.
  - `can_transform`, `can_direct_overlay`, `requires_resampling`, `overlaps_extent`는 대표 `status`와 별개로 truth table을 따른다.

### volview 연동을 위한 최소 필수 계약

- [ ] 좌표계와 index convention을 문서화한다: DICOM patient world 좌표는 LPS, index는 sample-centered continuous index, `i=column`, `j=row`, `k=slice`.
- [ ] `ImageOrientationPatient` mapping을 명시한다: `direction_i = IOP[0..2]`, `direction_j = IOP[3..5]`, `normal = direction_i x direction_j`.
  - DICOM 용어로 첫 triplet은 row direction cosine이지만, index convention상 column index `i`가 증가하는 world direction이다.
  - DICOM 용어로 두 번째 triplet은 column direction cosine이지만, index convention상 row index `j`가 증가하는 world direction이다.
- [x] `world_from_index(...)` / `index_from_world(...)`는 `ImagePlaneGeometry`와 `ImageVolumeGeometry`의 멤버 함수로 제공한다.
- [x] `index_to_world_matrix()` / `world_to_index_matrix()`는 `ImagePlaneGeometry`와 `ImageVolumeGeometry`의 멤버 함수로 제공한다.
- [x] `ImagePlaneGeometry`의 4x4 matrix 의미를 명시한다: plane index vector는 `(i, j, normal_mm, 1)`이며, `world_to_index_matrix()` 결과의 세 번째 성분은 signed normal distance(mm)다.
- [x] `contains_index(...)` / `contains_world(...)`는 geometry 객체의 멤버 함수로 제공하고, plane의 `contains_world()`는 out-of-plane tolerance를 받는다.
- [x] `ImageVolumeGeometry::contains_world()`도 boundary floating error 처리를 위해 tolerance를 받는다.
- [x] `SliceStackItem`은 입력 DICOM slice 순서와 output volume의 `k` index를 연결하는 mapping record로 제공한다.
- [ ] geometry 비교 tolerance는 `GeometryTolerance` 또는 `OverlayCheckOptions`처럼 명시적 option 타입으로 제공한다.
- [ ] orthogonal plane, viewport coordinate, crosshair snap, MIP/slab 정책은 volview layer에 남기고 DicomSDL geometry 최소 계약에는 넣지 않는다.

### 기본 타입

- [x] `Vec3d`
- [x] `Point3d`
- [x] `ImagePoint2D`
- [x] `ImagePoint3D`
- [x] `PlaneProjection2D`: volume/world point를 plane에 투영한 2D index와 signed normal distance를 함께 표현한다.
- [x] `ImageSize2D`
- [x] `ImageSize3D`
- [x] `ImageSpacing2D`: `i=column spacing`, `j=row spacing`을 이름으로 드러낸다.
- [x] `ImageSpacing3D`: `i=column`, `j=row`, `k=slice` spacing을 이름으로 드러낸다.
- [x] `ImagePlaneGeometryParams`
- [x] `ImageVolumeGeometryParams`
- [x] `Matrix4x4d`
- [x] `GeometryTolerance`
- [x] `GeometryBuildStatus`
- [x] `GeometryBuildResult<T>`
- [x] `VolumetricPropertiesValue`
- [x] `VolumetricPropertiesInfo`
- [x] `ImageFrameStackOptions`
- [x] `SliceStackInput`
- [x] `DimensionIndexDescriptor`
- [x] `DimensionIndexValue`
- [x] `ImageFrameStackKey`
- [x] `ImageFrameStackGroup`
- [x] `ImageFrameStackAnalysis`
- [x] `SliceStackIssue`

### Core Lookup 의존성

- [x] `ElementPath` / `DataSet` nested lookup 설계는 `tmp/element_path_dataset_lookup_checklist.ko.md`를 따른다.
- [x] geometry module은 element path 타입을 정의하지 않고 core `dicom::ElementPath`를 사용한다.
- [x] geometry module의 DICOM metadata access는 dotted string path가 아니라 `dicom::ElementPath`를 기본으로 한다.
- [x] geometry module은 nested lookup helper를 직접 노출하지 않고 `FrameGeometryReader` 내부에서 사용한다.
- [x] `FrameGeometryReader`는 자주 쓰는 DICOM path를 내부 helper 함수로 표현한다. 예: `per_frame_fg_path(frame, sequence_tag, leaf_tag)`, `shared_fg_path(sequence_tag, leaf_tag)`, `frame_type_path(frame, frame_type_sequence_tag, "VolumetricProperties"_tag)`.
- [x] geometry diagnostic/source location은 가능하면 `dicom::ElementPath`로 보존한다. 사람이 읽는 문자열은 error message 생성 시점에만 만든다.
- [x] geometry implementation에서는 `PerFrameFunctionalGroupsSequence.0...` 같은 dotted string literal을 쓰지 않는다. test assertion이나 debug 출력에만 허용한다.

### Geometry 타입

- [x] `ImagePlaneGeometry`
- [x] `ImageVolumeGeometry`
- [x] `FrameGeometryReader`: 반복 frame metadata 접근용 public reader. 내부 resolver/cache 타입은 public API로 올리지 않는다.

### DICOM Factory

- [x] `plane_from_single_frame_image(const DataSet&)`: classic single-frame image root dataset 또는 같은 tag set을 가진 synthetic/test dataset용.
- [x] `plane_from_single_frame_image(const DicomFile&)`: `file.dataset()`으로 forwarding하는 convenience overload.
- [x] `DataSet` overload는 기본 구현이고, `DicomFile` overload는 사용자 편의용이다.
- [x] single-frame API 이름에는 `single_frame_image`를 포함해 임의 dataset이나 enhanced frame으로 오해하지 않게 한다.
- [x] `plane_from_multiframe_image(const DataSet&, std::size_t)`: 일반 multi-frame/enhanced image의 특정 frame plane용.
- [x] `plane_from_multiframe_image(const DicomFile&, std::size_t)`: DicomFile convenience overload.
- [x] `frame_geometry_from_multiframe_image(...)`: sampled/distorted semantics를 보존해야 하는 metadata inspection용.
- [x] `plane_from_seg_frame(const seg::Segmentation&, std::size_t)`: SEG frame용.
- [x] `frame_of_reference_from_dataset(const DataSet&)`
- [x] `frame_of_reference_from_segmentation(const seg::Segmentation&)`
- [x] multi-frame convenience factory들은 내부적으로 `FrameGeometryReader`를 사용해 구현한다.
  - single-frame factory는 root dataset parser를 사용하고, SEG frame factory는 SEG strict parser를 사용한다.
- [x] 반복 호출이 예상되는 코드는 convenience factory 대신 `FrameGeometryReader`를 직접 생성해 재사용한다.
- [x] 일반 image frame의 geometry resolution 순서는 DicomSDL의 기존 pixel transform metadata와 맞춘다: `PerFrameFunctionalGroupsSequence -> SharedFunctionalGroupsSequence -> root dataset`.
- [x] 위 resolution 순서는 내부적으로 `ElementPath`로 표현한다. 예: `PerFrameFunctionalGroupsSequence[frame] -> PlanePositionSequence[0] -> ImagePositionPatient`.
- [x] 일반 image frame factory는 `VolumetricProperties=SAMPLED` / `DISTORTED`를 일반 slice plane으로 조용히 반환하지 않는다.
- [x] `FrameType`, `VolumetricProperties`, SOP Class 기반 geometry 의미를 확인하고, 일반 overlay plane이 아니면 `sampled_frame_geometry`, `distorted_frame_geometry`, 또는 `unsupported_frame_geometry`로 반환한다.
- [x] sampled/distorted frame을 허용하는 API가 필요하면 plain `ImagePlaneGeometry`가 아니라 `ImageFrameGeometry { plane, kind }`처럼 geometry semantics를 보존하는 타입으로 반환한다.
- [x] overlay용 factory는 sampled/distorted frame을 항상 reject한다.
- [x] SEG frame의 geometry resolution은 기본 strict 모드에서 `PerFrameFunctionalGroupsSequence -> SharedFunctionalGroupsSequence`까지만 허용한다.
- [x] SEG frame factory는 root dataset fallback을 제공하지 않는다.
- [x] geometry factory는 `std::optional` 대신 실패 원인을 담는 `GeometryBuildResult<T>`를 반환한다.
- [x] `GeometryBuildResult<T>`는 `ok + nullopt` 같은 불가능 상태를 표현하지 못하게 `success(...)` / `failure(...)` factory 중심으로 설계한다.

### VolumetricProperties / ImageFrameGeometryKind 판정

- [x] `VolumetricProperties (0008,9206)`는 root dataset만 보지 않고 SOP Class별 Frame Type Functional Group에서 먼저 찾는다.
- [x] MR은 `MR Image Frame Type Sequence (0018,9226)`, CT는 `CT Image Frame Type Sequence (0018,9329)`, PET은 `PET Frame Type Sequence (0018,9751)` 안의 `VolumetricProperties`를 우선한다.
- [x] MR/CT/PET frame type lookup은 모두 `ElementPath` helper로 구성한다. 예: `PerFrameFunctionalGroupsSequence[frame] -> PETFrameTypeSequence[0] -> VolumetricProperties`.
- [x] lookup table:
  - Enhanced MR Image Storage -> `MR Image Frame Type Sequence (0018,9226)`
  - Legacy Converted Enhanced MR Image Storage -> `MR Image Frame Type Sequence (0018,9226)`
  - Enhanced CT Image Storage -> `CT Image Frame Type Sequence (0018,9329)`
  - Legacy Converted Enhanced CT Image Storage -> `CT Image Frame Type Sequence (0018,9329)`
  - Enhanced PET Image Storage -> `PET Frame Type Sequence (0018,9751)`
  - Legacy Converted Enhanced PET Image Storage -> `PET Frame Type Sequence (0018,9751)`
- [x] resolution 순서는 `PerFrameFunctionalGroupsSequence[frame] -> SharedFunctionalGroupsSequence[0] -> root dataset`이다.
- [x] root 값이 `MIXED`이면 frame-level 값 없이는 reliable하지 않으므로 `mixed_volumetric_properties`로 실패한다.
- [x] `VOLUME`이면 `ImageFrameGeometryKind::regular_plane`, `SAMPLED`이면 `sampled_projection`, `DISTORTED`이면 `distorted`로 판정한다.
- [x] `MIXED`, missing, unknown value는 `ImageFrameGeometryKind`로 조용히 변환하지 않고 `GeometryBuildStatus`로 반환한다.
- [ ] `VolumeBasedCalculationTechnique`나 `FrameType` flavor(`MAX_IP` 등)는 후속 보조 진단으로만 사용하고, kind 판정의 primary source는 `VolumetricProperties`로 둔다.
- [x] SOP Class별 Frame Type Functional Group lookup table을 내부 helper로 두고, 지원하지 않는 SOP Class에서 frame-level value를 찾아야 하면 `unsupported_frame_geometry`로 실패한다.
- [x] `VolumetricPropertiesInfo::source`는 예를 들어 `PerFrameFunctionalGroupsSequence[frame] -> PETFrameTypeSequence[0] -> VolumetricProperties`를 `ElementPath`로 보존한다.
- [x] NM Image Storage는 Enhanced Functional Group 기반 Frame Type Sequence가 아니므로 이 lookup table에 넣지 않는다.
- [x] NM Image Storage는 `Frame Increment Pointer (0028,0009)`와 NM vector tags를 이용하는 별도 `analyze_nm_frame_stack()` / `plan_nm_frame_stack()` API로 분리한다.
- [x] NM `Image Type (0008,0008)` Value 3 중 `RECON TOMO` / `RECON GATED TOMO`는 reconstructed slice 후보, `TOMO` / `GATED TOMO`는 projection acquisition 후보로 보고 regular stack에서 거부한다.

### NM Image Storage 별도 경로

- [x] NM Image Storage는 `VolumetricProperties` / Enhanced Functional Groups 기반 판정 대상이 아니다.
- [x] NM frame organization은 `Frame Increment Pointer (0028,0009)`가 참조하는 indexing vector들로 해석한다. MVP에서는 `Frame Increment Pointer`가 정확히 `Slice Vector (0054,0080)` 하나만 가리키는 경우만 지원한다.
- [ ] 주요 NM vector tags:
  - `Energy Window Vector (0054,0010)`
  - `Detector Vector (0054,0020)`
  - `Phase Vector (0054,0030)`
  - `Rotation Vector (0054,0050)`
  - `R-R Interval Vector (0054,0060)`
  - `Time Slot Vector (0054,0070)`
  - `Slice Vector (0054,0080)`
  - `Angular View Vector (0054,0090)`
  - `Time Slice Vector (0054,0100)`
- [x] NM `Image Type (0008,0008)` Value 3는 `RECON TOMO`, `RECON GATED TOMO`, `TOMO`, `GATED TOMO`를 우선 구분한다. 그 외 값은 현재 adapter에서 거부한다.
- [x] `RECON TOMO` / `RECON GATED TOMO`에서 `Slice Vector (0054,0080)`를 사용한 reconstructed slice stack 후보를 만들 수 있다.
- [x] `TOMO` / `GATED TOMO`는 projection acquisition 성격이 강하므로 일반 `ImagePlaneGeometryKind::regular_plane`로 취급하지 않는다.
- [x] NM adapter는 `analyze_nm_frame_stack()` / `plan_nm_frame_stack()`으로 구현한다.

### Overlay Helper

- [x] `OverlayCompatibility` enum
- [x] `OverlayCheckOptions`
- [x] `OverlayCheck` result struct
- [x] `check_overlay_compatibility(...)` plane-plane overload
- [x] `check_overlay_compatibility(...)` plane-volume overload
- [x] `check_overlay_compatibility(...)` volume-plane overload
- [x] `check_overlay_compatibility(...)` volume-volume overload
- [x] `check_overlay_compatibility(...)`는 plane-plane, plane-volume, volume-plane, volume-volume overload를 제공한다.
- [x] `OverlayCheck::ok()` 하나에 의미를 몰아넣지 않고 `can_transform`, `can_direct_overlay`, `requires_resampling`을 분리한다.
- [ ] geometry reference 기반 overload에서는 geometry가 이미 존재하므로 `missing_geometry` 상태를 반환하지 않는다. geometry build 실패는 factory result 단계에서 처리한다.
- [ ] 필요하면 `GeometryBuildResult` / pointer / optional 기반 preflight overload를 별도로 두고 그 경우에만 `missing_geometry`를 사용한다.
- [x] plane-volume / volume-volume check는 `can_transform`과 physical extent overlap을 분리한다.
- [x] volume-plane check는 `can_transform`과 physical extent overlap을 분리한다.
- [x] `OverlayCheck`는 `overlaps_extent`, `source_inside_target_extent`, `target_k_range` 같은 extent 진단 필드를 제공한다.
- [x] `PlaneToPlaneTransform`
- [x] `PlaneToVolumeTransform`
- [x] `VolumeToPlaneTransform`
- [x] `VolumeToVolumeTransform`
- [x] plane-to-plane은 `ImagePoint2D -> ImagePoint2D`, plane-to-volume은 `ImagePoint2D -> ImagePoint3D`, volume-to-plane은 `ImagePoint3D -> ImagePoint2D`, volume-to-volume은 `ImagePoint3D -> ImagePoint3D`로 타입을 분리한다.
- [x] volume point를 plane으로 투영하는 API는 2D index와 signed normal distance를 함께 얻을 수 있는 helper를 제공한다.

### OverlayCheck Truth Table

| Case | status | can_transform | can_direct_overlay | requires_resampling | overlaps_extent |
| --- | --- | --- | --- | --- | --- |
| FrameOfReferenceUID 없음 | `missing_frame_of_reference` | false | false | false | false |
| FrameOfReferenceUID 다름 | `different_frame_of_reference` | false | false | false | false |
| 같은 FoR, 동일 plane/grid/extent | `compatible` | true | true | false | true |
| 같은 FoR, 물리 extent가 겹치지만 grid가 다름 | `requires_resampling` 또는 세부 상태 | true | false | true | true |
| 같은 FoR, spacing만 다름 | `different_spacing` | true | false | true | extent 기준 |
| 같은 FoR, extent만 다름 | `different_extent` | true | false | false | extent 기준 |
| 같은 FoR, 평면 normal 위치가 tolerance 밖 | `out_of_plane` | true | false | false | false |
| 같은 FoR, 방향이 반대 | `opposite_orientation` | true | false | true | extent 기준 |
| 같은 FoR, plane이 volume 밖 | `out_of_plane` 또는 `different_extent` | true | false | false | false |
| 같은 FoR, volume끼리 extent가 겹치지 않음 | `different_extent` | true | false | false | false |

- [ ] `ok()`는 `can_transform`과 같은 의미로 둔다. 직접 overlay 여부는 반드시 `can_direct_overlay`를 확인한다.
- [ ] `requires_resampling=true`는 물리적으로 겹치는 영역이 있고 spacing/orientation/grid mapping 때문에 interpolation 또는 resampling이 필요한 경우에만 사용한다.
  - extent만 다른 경우는 crop/pad/clip 정책이며 `requires_resampling=false`로 둔다.
- [ ] extent 관련 overload는 가능한 경우 `target_k_range`를 채운다. extent가 겹치지 않으면 `target_k_range`는 비어 있다.
- [ ] 대표 `status` 우선순위는 다음 순서로 고정한다: `missing_frame_of_reference` -> `different_frame_of_reference` -> `non_parallel_planes` -> `opposite_orientation` -> `out_of_plane` -> `different_spacing` -> `different_extent` -> `requires_resampling` -> `compatible`.
- [ ] 대표 `status`는 진단용이며, 실제 caller 정책은 `can_transform`, `can_direct_overlay`, `requires_resampling`, `overlaps_extent`를 함께 확인한다.

### 성능 계약

- [x] `ElementPath` / `DataSet` nested lookup 자체의 성능 계약은 `tmp/element_path_dataset_lookup_checklist.ko.md`를 따른다.
- [x] geometry factory의 DICOM metadata lookup은 string parsing 없는 `ElementPath` 경로를 사용한다.
- [x] 단일 frame 또는 드문 metadata 조회는 `DataSet::get_dataelement(ElementPath...)` / `DicomFile::get_dataelement(ElementPath...)`를 그대로 사용한다.
- [x] 반복 frame metadata 접근은 `FrameGeometryReader`를 생성해서 재사용한다.
- [x] `FrameGeometryReader`는 생성 시 root dataset, `PerFrameFunctionalGroupsSequence`, `SharedFunctionalGroupsSequence`, SOP Class별 Frame Type Sequence tag 같은 반복 lookup 정보를 캐시한다.
- [x] `FrameGeometryReader`의 frame별 접근은 가능한 한 `sequence_item(frame_index)`와 소수의 tag lookup으로 끝나게 한다.
- [x] `FrameGeometryReader` hot path는 `ElementPath`의 의미를 유지하되 매 frame마다 root부터 full path traversal을 반복하지 않는다. 부모 item `DataSet*`를 캐시하거나 지역 변수로 잡은 뒤 leaf tag lookup을 수행한다.
- [x] 실패 diagnostic에는 hot path에서 사용한 parent/leaf lookup을 다시 `ElementPath` source로 재구성해 기록한다.
- [x] convenience factory(`plane_from_multiframe_image`, `frame_geometry_from_multiframe_image`)는 사용 편의용이며, 많은 frame을 순회하는 코드는 `FrameGeometryReader`를 직접 재사용하도록 문서화한다.
- [x] `check_overlay_compatibility()`는 dataset traversal, sequence lookup, heap allocation을 하지 않는다.
- [x] `check_overlay_compatibility()`는 matrix inverse, direction normalization, normal 계산을 하지 않는다.
- [x] `ImagePlaneGeometry` / `ImageVolumeGeometry` 생성 시 matrix, inverse matrix, normalized direction, normal을 미리 계산/cache한다.
- [x] `FrameOfReferenceUID` 비교는 string copy 없이 `std::string_view` 또는 caller-owned stable string으로 수행한다.
- [x] `SliceStackAnalysis`처럼 dataset보다 오래 살 수 있는 객체는 `FrameOfReferenceUID`를 `std::string`으로 소유한다.
- [x] `SliceStackPlan`처럼 dataset보다 오래 살 수 있는 객체는 `FrameOfReferenceUID`를 `std::string`으로 소유한다.
- [x] `check_overlay_compatibility()`는 UID equality, dot product, spacing/origin 차이 비교만 수행하는 O(1) 함수로 유지한다.
- [x] transform 객체 생성 시 source/target matrix, inverse matrix, composite matrix를 모두 계산해두고 point 변환은 matrix multiply만 수행한다.
- [ ] 반복 overlay/resampling loop에서는 `DataSet` lookup이나 `check_overlay_compatibility()`를 호출하지 않고 이미 만든 geometry와 typed transform 객체만 재사용한다.

### Slice Stack Planning

- [x] `SliceStackStatus` enum
- [x] `SliceStackOptions`
- [x] `SliceStackInput`: slice planner의 중심 입력 타입. `source_index`, `frame_index`, `ImagePlaneGeometry`, `FrameOfReferenceUID`를 갖는다.
- [x] `SliceStackInput::frame_of_reference_uid`는 호출 중에만 유효한 `std::string_view`여도 되고, `SliceStackAnalysis`는 UID를 `std::string`으로 복사해 소유한다.
- [x] `SliceStackSlice`: 정렬된 slice의 source index, frame index, plane geometry, normal projection 위치를 갖는다.
- [x] `SliceStackGap`: 인접 sorted slice 사이의 physical gap 정보를 갖는다.
- [x] `SliceStackRun`: 같은 spacing으로 볼 수 있는 3개 이상 연속 slice 구간 정보를 갖는다.
- [x] `SliceStackAnalysis`: sorted slices와 gaps를 제공한다.
- [x] `SliceStackItem`: `source_index`, `frame_index`, `target_k`, `position_along_normal_mm`를 갖는다.
- [x] `SliceStackPlan`
- [x] `analyze_slice_stack(std::span<const SliceStackInput>, SliceStackOptions)`를 중심 API로 둔다.
- [x] `plan_slice_stack(std::span<const SliceStackInput>, SliceStackOptions)`를 중심 API로 둔다.
- [x] `analyze_slice_stack(std::span<const DataSet* const>, SliceStackOptions)` / `plan_slice_stack(std::span<const DataSet* const>, SliceStackOptions)`는 classic multi-instance series용 convenience overload로 둔다.
- [x] `analyze_image_frame_stack(const DicomFile&, std::span<const std::size_t>, ImageFrameStackOptions)`는 caller가 고른 Enhanced frame subset을 `SliceStackInput` 목록으로 변환한다.
- [x] `plan_image_frame_stack(const DicomFile&, std::span<const std::size_t>, ImageFrameStackOptions)`는 caller가 고른 Enhanced frame subset을 `SliceStackPlan`으로 변환한다.
- [x] `analyze_image_frame_stacks(const DicomFile&, ...)`는 Enhanced multi-frame image의 frames를 우선 `StackID`, `InStackPositionNumber`로 grouping한다.
- [x] `analyze_image_frame_stacks(const DicomFile&, ...)`는 `DimensionIndexValues` 기반 non-spatial dimension grouping까지 확장한다.
- [x] `ImageFrameStackKey`는 `DimensionIndexValues`의 raw 값만 보관하지 않고, 각 값이 어떤 `DimensionIndexPointer` / `FunctionalGroupPointer` / label에 대응되는지 함께 보관한다.
- [x] `analyze_image_frame_stacks(...)`는 vector만 반환하지 않고 `ImageFrameStackAnalysis { status, groups, issues }`를 반환한다.
- [x] `analyze_image_frame_stack(const DicomFile&, ...)` / `plan_image_frame_stack(const DicomFile&, ...)`는 단일 stack으로 해석 가능한 경우에만 성공하는 convenience overload로 둔다.
- [x] 여러 stack이 섞여 있으면 `plan_image_frame_stack(file)`은 `multiple_frame_stacks` 상태를 반환하고, caller가 group별 plan을 호출하게 한다.
- [x] `SliceStackPlan::volume_geometry()`는 uniform stack에서만 값을 갖는다.
- [x] `SliceStackPlan::placements()`
- [x] `SliceStackAnalysis`는 실패 시 문제 입력을 찾을 수 있도록 `issues()`를 제공한다.
- [x] `plan_slice_stack()`은 기본적으로 uniform spacing일 때만 `ok + volume_geometry`를 반환한다.
- [x] uniform volume plan은 normal projection spacing뿐 아니라 각 slice origin의 in-plane residual도 검사한다.
- [x] in-plane residual이 tolerance를 넘으면 `non_rectilinear_stack` 또는 `inconsistent_slice_origin`으로 처리하고 `ImageVolumeGeometry`를 만들지 않는다.
- [x] non-uniform stack은 `SliceStackAnalysis`에서 sorted slices와 gaps까지 표현한다.
- [x] non-uniform stack에서 `plan_slice_stack()`은 `non_uniform_spacing`을 반환한다.
- [ ] dominant grid 선택, volume extent 확장, 빈 slice 채우기 정책은 DicomSDL이 아니라 caller가 결정한다.
- [x] volume stack 정렬 규칙을 고정한다: chosen normal 방향의 position 오름차순, `spacing_k > 0`, `origin = target_k == 0` slice origin.

### Status / Issue 정책

- [x] `GeometryBuildResult<T>`는 단일 operation의 대표 실패만 담고 issue list를 갖지 않는다.
- [x] `SliceStackAnalysis`는 `issues()`를 제공한다.
- [x] `SliceStackAnalysis::status()`는 fatal issue 중 가장 높은 우선순위 issue를 대표하고, 우선순위가 같으면 입력 source/frame 순서를 따른다.
- [x] `SliceStackAnalysis` fatal priority는 다음 순서를 기본으로 한다: `empty` -> `missing_geometry` -> `missing_frame_content` -> `missing_dimension_module` -> `unsupported_tiled_image` -> `multiple_frame_stacks` -> `geometry_parse_failure` -> `missing_frame_of_reference` -> `mixed_frame_of_reference` -> `inconsistent_rows_columns` -> `inconsistent_orientation` -> `inconsistent_pixel_spacing` -> `inconsistent_slice_origin` -> `duplicate_slice_position` -> `non_uniform_spacing`.
- [x] `SliceStackAnalysis::issues()`는 가능한 모든 fatal/warning 원인을 source/frame/tag/message와 함께 보존한다.
- [x] `SliceStackAnalysis`는 partial diagnostic result를 가질 수 있다.
- [x] plan은 `ok()==false`이면 `volume_geometry()`를 비운다.
- [x] classic DataSet stack adapter의 metadata 오류와 missing geometry는 throw하지 않고 `SliceStackIssue`로 반환한다.
- [x] unsupported 구조, missing sequence/item은 throw하지 않는다. unexpected allocation failure나 programmer error는 일반 C++ 예외 정책을 따른다.

## 구현 체크리스트

- [x] core `ElementPath` / `DataSet` nested lookup API는 `tmp/element_path_dataset_lookup_checklist.ko.md`에 따라 별도 구현한다.
- [x] geometry module은 core `ElementPath`를 include/use만 하고 재정의하지 않는다.
- [x] geometry module의 DICOM path helper는 `dicom::ElementPath`를 반환하거나 `dicom::ElementPathView`로 즉시 lookup 가능한 형태를 반환한다.
- [x] geometry parser에서 dotted string path lookup을 사용한 부분이 있으면 `ElementPath` helper로 치환한다.
- [x] `dicom_geometry.h` 추가
- [x] `Vec3d`, `Point3d`, `ImagePoint2D/3D`, `ImageSize2D/3D` 정의
- [x] `PlaneProjection2D` 정의
- [x] `ImageSpacing2D/3D` 정의: plane API에서 `Vec3d spacing()`만 노출하지 않고 `spacing_i()` / `spacing_j()`를 명확히 드러낸다.
- [x] `ImagePlaneGeometryParams` 정의
- [x] `ImageVolumeGeometryParams` 정의
- [x] `Matrix4x4d` 정의
- [x] `Matrix4x4d`는 row-major storage와 column-vector left multiply convention을 주석과 테스트로 고정한다.
- [x] affine inverse 구현
- [x] `GeometryBuildStatus`와 `GeometryBuildResult<T>` 정의: `success(...)` / `failure(...)` 생성 경로만 열어 invariant를 강하게 유지한다.
- [x] `GeometryBuildStatus`는 sampled/projection frame처럼 일반 plane overlay로 해석하면 안 되는 frame을 구분한다.
- [x] `GeometryBuildStatus`는 missing/mixed/unknown `VolumetricProperties`를 구분한다.
- [x] `FrameGeometryReader`는 매 frame마다 full path traversal을 반복하지 않고 cached sequence item pointer를 재사용한다.
- [x] `FrameGeometryReader`는 public API로 별도 cursor/path resolver를 노출하지 않고, 내부에서 `ElementPath`와 cached parent item lookup을 조합한다.
- [x] sampled/distorted frame을 지원하는 별도 API가 필요할 경우 `ImageFrameGeometryKind`를 포함해 semantics를 보존하고, overlay용 plane factory는 계속 reject한다.
- [x] `ImageSize2D/3D`는 음수 의미가 없으므로 `std::size_t` 같은 unsigned 타입을 사용한다.
- [x] `SliceStackItem::target_k`는 음수 의미가 없으므로 `std::size_t` 또는 `std::uint32_t` 같은 unsigned 타입을 사용한다.
- [x] `ImagePlaneGeometry` 구현
- [x] `ImageVolumeGeometry` 구현
- [x] `make_image_plane_geometry(...)` validated factory 구현
- [x] `make_image_volume_geometry(...)` validated factory 구현
- [x] DICOM `ImagePositionPatient`, `ImageOrientationPatient`, `PixelSpacing` 파싱 helper 추가
- [x] `ImagePositionPatient` lookup은 `PlanePositionSequence[0] -> ImagePositionPatient`를 `ElementPath`로 표현한다.
- [x] `ImageOrientationPatient` lookup은 `PlaneOrientationSequence[0] -> ImageOrientationPatient`를 `ElementPath`로 표현한다.
- [x] `PixelSpacing`, `SliceThickness` lookup은 `PixelMeasuresSequence[0] -> PixelSpacing/SliceThickness`를 `ElementPath`로 표현한다.
- [x] `ImageOrientationPatient`의 첫 triplet은 `direction_i`, 두 번째 triplet은 `direction_j`로 매핑한다는 계약을 코드 주석과 테스트에 남긴다.
  - 첫 triplet은 DICOM row direction cosine이지만 DicomSDL의 `i=column` index direction이다.
  - 두 번째 triplet은 DICOM column direction cosine이지만 DicomSDL의 `j=row` index direction이다.
- [x] `ImagePlaneGeometry::index_to_world_matrix()`는 `(i, j, normal_mm, 1)`을 world로 보내고, `world_to_index_matrix()`의 세 번째 성분은 signed normal distance(mm)임을 코드 주석에 남긴다.
- [x] classic single-frame image dataset에서 plane geometry를 만드는 parser 추가
- [x] 일반 multi-frame/enhanced image frame에서 `PerFrameFunctionalGroupsSequence` / `SharedFunctionalGroupsSequence` / root dataset fallback으로 plane geometry를 만드는 parser 추가
- [x] sampled/distorted semantics가 필요한 caller를 위해 `ImageFrameGeometry { plane, kind }`를 반환하는 metadata-preserving factory를 별도로 둔다.
- [x] 일반 image frame geometry parser는 `VolumetricProperties=SAMPLED` / `DISTORTED`를 일반 slice plane으로 처리하지 않는다.
- [x] `FrameGeometryReader::plane(frame_index)`도 regular plane만 반환하고, sampled/distorted frame은 `sampled_frame_geometry` / `distorted_frame_geometry`로 실패한다.
- [x] `FrameGeometryReader::volumetric_properties(frame_index)` 추가: SOP Class별 Frame Type Sequence를 `PerFrame -> Shared -> root` 순서로 non-throwing resolve한다.
- [x] `FrameGeometryReader` 생성 시 root dataset, per-frame/shared functional groups, SOP Class별 Frame Type Sequence tag 등 반복 lookup 정보를 캐시한다.
- [x] `FrameGeometryReader`의 frame별 lookup은 cached sequence pointer에서 `sequence_item(frame_index)`와 필요한 tag lookup만 수행하도록 구현한다.
- [x] `FrameGeometryReader::volumetric_properties(frame_index)`의 source는 실제 resolve에 사용한 `ElementPath`로 채운다.
- [x] `FrameGeometryReader::plane(frame_index)`의 missing tag failure는 가능하면 missing tag와 `ElementPath` source를 diagnostic message/issue에 남긴다.
- [x] 많은 frame을 순회하는 API 문서/주석에서는 convenience factory보다 `FrameGeometryReader` 재사용을 권장한다.
- [x] MR `MR Image Frame Type Sequence (0018,9226)`, CT `CT Image Frame Type Sequence (0018,9329)`, PET `PET Frame Type Sequence (0018,9751)` lookup을 먼저 구현하고, 다른 SOP Class는 table 확장 전까지 명시적으로 unsupported 처리한다.
- [x] NM Image Storage는 `FrameGeometryReader::volumetric_properties()` lookup table에서 제외하고, 호출되면 `unsupported_frame_geometry`로 실패한다.
- [x] NM 지원은 `Frame Increment Pointer (0028,0009)`와 NM indexing vectors를 해석하는 별도 adapter로 문서화한다. MVP는 reconstructed TOMO `SliceVector` 단독 경로만 포함한다.
- [x] root `VolumetricProperties=MIXED`는 frame-level value를 찾지 못한 경우 실패로 처리한다.
- [x] `frame_geometry_from_multiframe_image()`는 resolved `VolumetricProperties` source path를 diagnostic message 또는 issue에 남긴다.
- [x] SEG frame의 strict parser는 `PerFrameFunctionalGroupsSequence` / `SharedFunctionalGroupsSequence`에서만 plane geometry를 생성한다.
- [x] SEG frame의 root dataset fallback은 MVP에서 구현하지 않는다.
- [x] SEG frame geometry parser는 missing metadata에서 throw할 수 있는 SEG high-level accessor를 직접 타지 않는다.
- [x] classic image, enhanced image frame, SEG frame factory가 같은 `FrameGeometryReader` 또는 내부 parser를 재사용하도록 구성
- [x] geometry build 실패 원인으로 missing tag, invalid spacing, invalid orientation, unsupported frame 구조를 구분
- [x] `FrameOfReferenceUID` 추출 helper 추가
- [x] `SliceStackAnalysis` 내부의 `FrameOfReferenceUID`는 `std::string`으로 소유
- [x] frame of reference mismatch를 명시적으로 표현하는 `OverlayCompatibility` enum 추가
- [x] tolerance 값을 묶는 `OverlayCheckOptions` 추가
- [x] `OverlayCheckOptions`에 plane 간 normal 위치 차이를 비교하는 `normal_distance_tolerance_mm`를 포함한다.
- [x] `OverlayCheck`는 `can_transform`, `can_direct_overlay`, `requires_resampling`을 별도 boolean으로 제공한다.
- [x] reference 기반 `check_overlay_compatibility()` overload에서는 `missing_geometry`를 반환하지 않도록 enum/주석을 정리한다.
- [x] plane-volume / volume-volume overlay check는 extent overlap을 계산해 `overlaps_extent`, `source_inside_target_extent`, `target_k_range`를 채운다.
- [x] volume-plane overlay check는 extent overlap을 계산해 `overlaps_extent`, `source_inside_target_extent`를 채운다.
- [x] 같은 frame of reference이지만 grid가 다른 경우는 overlay 불가가 아니라 resampling 필요 상태로 구분한다.
- [x] spacing, extent, plane position, opposite orientation 문제를 `OverlayCompatibility`에서 따로 표현한다.
- [x] `OverlayCheck` result는 `requires_resampling`, `same_spacing`, `same_extent`, `max_normal_distance_mm` 같은 세부 필드를 제공한다.
- [x] plane-to-plane world/index 변환 helper 추가
- [x] plane-to-volume, volume-to-plane, volume-to-volume 변환 helper 추가
- [x] volume point를 plane에 투영하는 transform helper는 `PlaneProjection2D`로 2D index와 signed normal distance를 함께 반환한다.
- [x] `check_overlay_compatibility()`가 preflight-only O(1) 함수임을 header 주석에 명시
- [x] `check_overlay_compatibility()`는 plane-plane, plane-volume, volume-volume overload를 제공한다.
- [x] `check_overlay_compatibility()`는 volume-plane overload를 제공한다.
- [x] typed transform 객체 생성 시 source/target matrix composition을 미리 계산
- [x] typed transform 객체의 per-point 변환 함수는 allocation 없이 matrix multiply만 수행
- [x] `contains_world()`는 plane의 normal distance tolerance를 적용해 평면 밖의 world point를 false로 처리
- [x] `ImageVolumeGeometry::contains_world()`도 tolerance를 적용해 volume boundary 근처 floating error를 안정적으로 처리한다.
- [x] DICOM slice stack 입력을 `SliceStackInput` 목록으로 변환하는 helper 추가
- [x] `SliceStackIssue { status, source_index, frame_index, tag, message }`를 정의하고 analysis 결과에서 노출한다.
- [x] `ImageFrameStackOptions`를 정의한다.
- [x] dimension metadata가 없는 경우의 geometry-only grouping fallback을 기본 off로 둔다.
- [x] classic multi-instance image series는 `DataSet*` 목록에서 `SliceStackInput` 목록을 만드는 convenience adapter로 처리
- [x] Enhanced multi-frame image는 명시적 `DicomFile + frame_index` 목록에서 `SliceStackInput` 목록을 만드는 convenience adapter로 처리
- [x] Enhanced multi-frame image의 전체 frame convenience API는 먼저 `StackID`, `InStackPositionNumber` 기반 group을 만든다.
- [x] Enhanced multi-frame image의 전체 frame convenience API는 `DimensionIndexSequence`, `DimensionIndexValues` 기반 group까지 확장한다.
- [x] `DimensionIndexDescriptor`는 `dimension_index_pointer`, `functional_group_pointer`, `dimension_organization_uid`, `label`, private creator 정보를 보존한다.
- [x] `DimensionIndexValue`는 descriptor와 frame별 logical value를 같이 보관해 echo/time/phase/position 축 의미를 잃지 않게 한다.
- [x] `ImageFrameStackAnalysis`는 `status()`, `groups()`, `issues()`를 제공한다.
- [x] 단일 stack으로 볼 수 없는 Enhanced image에서 `plan_image_frame_stack(file)`은 실패 상태를 반환하고, `analyze_image_frame_stacks(file)` 결과를 사용하게 한다.
- [x] slice normal 방향 projection으로 slice position을 정렬하는 helper 추가
- [x] slice origin의 in-plane residual을 계산해 `SliceStackAnalysis` issue로 기록한다.
- [x] non-uniform stack에서도 sorted slices와 gaps를 반환하는 `SliceStackAnalysis` 추가
- [x] non-uniform stack에서도 uniform runs를 반환하는 `SliceStackAnalysis` 확장
- [x] uniform spacing일 때만 `ImageVolumeGeometry`를 갖는 `SliceStackPlan` 추가
- [x] uniform spacing이어도 in-plane residual이 tolerance를 넘으면 `ImageVolumeGeometry`를 만들지 않는다.
- [x] non-uniform stack에서는 `plan_slice_stack()`이 기본값으로 `non_uniform_spacing`을 반환하고 `volume_geometry()`는 비어 있게 한다.
- [x] source slice index와 target volume k index의 mapping을 `SliceStackItem`으로 노출
- [x] `SliceStackPlan`의 정렬 규칙은 chosen normal 방향 position 오름차순, `spacing_k > 0`, `origin = target_k == 0` slice origin으로 고정한다.
- [x] `SliceStackItem` 사용 예를 문서 주석에 남긴다: `source_index`로 입력 파일/데이터셋을 찾고, `frame_index`로 해당 frame을 decode해서 output volume의 `item.target_k` slice에 넣는다.
- [ ] dominant grid 선택, collision 해결, volume extent 확장, missing slice 채우기는 volview 같은 caller policy로 남긴다.
- [ ] pixel data decode와 volume buffer allocation은 `dicom::volume` 같은 후속 higher-level API로 분리
- [x] `FrameGeometryReader`는 throwing helper를 직접 쓰지 않고 non-throwing `DataSet` lookup helper로 실패를 `GeometryBuildStatus`에 접는다.
- [x] Python binding은 `dicom.geometry` submodule로 노출한다.
  - `GeometryBuildResult<T>`는 Python surface에 직접 노출하지 않고, 성공 시 객체를 반환하고 실패 시 `ValueError`를 던진다.
  - geometry 값 타입, plane/volume geometry, DICOM factory, overlay check, typed transform을 우선 노출한다.

## API 스케치

```cpp
#include <dicom_seg.h>
#include <dicom_geometry.h>

namespace dicom::seg {

enum class SegmentationType {
    unknown,
    binary,
    fractional,
    labelmap,
};

enum class SegmentationFractionalType {
    none,
    probability,
    occupancy,
    unknown,
};

struct SegmentMaskOptions {
    // For FRACTIONAL SEG, compare raw / MaximumFractionalValue against this
    // normalized threshold. The default 0.0 means any non-zero fractional
    // sample is present.
    double fractional_threshold = 0.0;
    bool error_when_not_present_in_frame = false;
};

class SegmentFrameView {
public:
    std::size_t index() const noexcept;

    // Preferred common API. BINARY/FRACTIONAL return a span over the existing
    // per-frame ReferencedSegmentNumber scalar. LABELMAP returns every
    // non-background segment label present in the decoded frame from a stable
    // per-frame lazy cache.
    std::span<const std::uint16_t> present_segment_numbers() const;

    // Compatibility accessor for the classic BINARY/FRACTIONAL model.
    // LABELMAP has no single referenced segment for a frame; calling this on a
    // LABELMAP frame throws a compatibility error. Keep the return type stable.
    std::uint16_t referenced_segment_number() const;

    // Existing stored frame decode. BINARY returns uint8 0/1, FRACTIONAL
    // returns raw uint8. LABELMAP BitsAllocated=8 can use this overload.
    void decode_frame_into(std::span<std::uint8_t> out) const;

    // Explicit LABELMAP typed decode. Use this for LABELMAP BitsAllocated=16.
    void decode_labelmap_frame_into(std::span<std::uint8_t> out) const;
    void decode_labelmap_frame_into(std::span<std::uint16_t> out) const;

    // Common semantic mask API. Output is always uint8 0/1 and has no palette
    // or rendering meaning.
    void mask_for_segment_into(std::uint16_t segment_number,
        std::span<std::uint8_t> out,
        SegmentMaskOptions options = {}) const;
    std::vector<std::uint8_t> mask_for_segment(
        std::uint16_t segment_number,
        SegmentMaskOptions options = {}) const;
};

class Segmentation {
public:
    SegmentationType segmentation_type() const noexcept;
    SegmentationFractionalType fractional_type() const noexcept;
    std::optional<std::uint16_t> maximum_fractional_value() const noexcept;
    std::optional<unsigned> labelmap_bits_allocated() const noexcept;

    std::size_t rows() const noexcept;
    std::size_t columns() const noexcept;
    std::size_t segment_count() const noexcept;
    std::size_t frame_count() const noexcept;

    SegmentListView segments() const noexcept;
    SegmentFrameListView frames() const noexcept;
    std::optional<SegmentView> segment_by_number(
        std::uint16_t segment_number) const noexcept;

    // Common semantics: frames where this segment is present.
    // BINARY/FRACTIONAL use ReferencedSegmentNumber; LABELMAP uses the
    // lazy per-frame present label cache. LABELMAP may decode all frames on
    // first call and can throw label validation/decode errors. On success the
    // segment-to-frame index is published as immutable storage borrowed by the
    // returned SegmentFrameListView.
    SegmentFrameListView frames_for_segment(
        std::uint16_t segment_number) const;
    std::size_t segment_frame_count(
        std::uint16_t segment_number) const;

    std::span<const std::uint16_t> present_segment_numbers(
        std::size_t frame_index) const;

    void decode_frame_into(
        std::size_t frame_index,
        std::span<std::uint8_t> out) const;

    void decode_labelmap_frame_into(
        std::size_t frame_index,
        std::span<std::uint8_t> out) const;
    void decode_labelmap_frame_into(
        std::size_t frame_index,
        std::span<std::uint16_t> out) const;
    std::vector<std::uint8_t> decode_labelmap_frame_bytes(
        std::size_t frame_index) const;

    void mask_for_segment_into(
        std::size_t frame_index,
        std::uint16_t segment_number,
        std::span<std::uint8_t> out,
        SegmentMaskOptions options = {}) const;
    std::vector<std::uint8_t> mask_for_segment(
        std::size_t frame_index,
        std::uint16_t segment_number,
        SegmentMaskOptions options = {}) const;

    // Explicit full-pixel validation for callers that want eager confidence.
    // from_dicomfile() remains metadata-only and does not call this implicitly.
    void validate_label_values() const;
};

bool is_segmentation_storage(const DicomFile& file) noexcept;
bool is_segmentation_storage(const DataSet& ds) noexcept;
bool is_labelmap_segmentation_storage(const DicomFile& file) noexcept;
bool is_labelmap_segmentation_storage(const DataSet& ds) noexcept;
bool is_any_segmentation_storage(const DicomFile& file) noexcept;
bool is_any_segmentation_storage(const DataSet& ds) noexcept;

} // namespace dicom::seg

namespace dicom::geometry {

struct Vec3d {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

struct Point3d {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

struct ImagePoint2D {
    double i = 0.0;
    double j = 0.0;
};

struct ImagePoint3D {
    double i = 0.0;
    double j = 0.0;
    double k = 0.0;
};

struct IndexRange1D {
    std::size_t begin = 0;
    std::size_t end = 0;
};

struct PlaneProjection2D {
    ImagePoint2D index;
    double signed_normal_distance_mm = 0.0;
};

struct ImageSize2D {
    std::size_t columns = 0;
    std::size_t rows = 0;
};

struct ImageSize3D {
    std::size_t columns = 0;
    std::size_t rows = 0;
    std::size_t slices = 0;
};

struct ImageSpacing2D {
    double i = 0.0;  // column spacing, DICOM PixelSpacing[1]
    double j = 0.0;  // row spacing, DICOM PixelSpacing[0]
};

struct ImageSpacing3D {
    double i = 0.0;  // column spacing
    double j = 0.0;  // row spacing
    double k = 0.0;  // slice spacing
};

struct ImagePlaneGeometryParams {
    ImageSize2D size;
    Point3d origin;
    Vec3d direction_i;
    Vec3d direction_j;
    ImageSpacing2D spacing;
};

struct ImageVolumeGeometryParams {
    ImageSize3D size;
    Point3d origin;
    Vec3d direction_i;
    Vec3d direction_j;
    Vec3d direction_k;
    ImageSpacing3D spacing;
};

struct GeometryTolerance {
    double position_mm = 1e-3;
    double spacing_mm = 1e-3;
    double orientation_dot = 1e-4;
    double normal_distance_mm = 1e-3;
};

enum class GeometryBuildStatus {
    ok,
    missing_required_tag,
    invalid_spacing,
    invalid_orientation,
    invalid_image_size,
    frame_index_out_of_range,
    missing_volumetric_properties,
    mixed_volumetric_properties,
    unknown_volumetric_properties,
    sampled_frame_geometry,
    distorted_frame_geometry,
    unsupported_frame_geometry,
};

enum class VolumetricPropertiesValue {
    volume,
    sampled,
    distorted,
    mixed,
    missing,
    unknown,
};

enum class ImageFrameGeometryKind {
    regular_plane,
    sampled_projection,
    distorted,
};

struct VolumetricPropertiesInfo {
    VolumetricPropertiesValue value = VolumetricPropertiesValue::missing;
    dicom::ElementPath source;
};

template <class T>
class GeometryBuildResult {
public:
    static GeometryBuildResult success(T value);
    static GeometryBuildResult failure(
        GeometryBuildStatus status,
        std::optional<Tag> tag = std::nullopt,
        std::string message = {});

    bool ok() const noexcept;
    GeometryBuildStatus status() const noexcept;
    const T& value() const;
    const T* maybe_value() const noexcept;
    std::optional<Tag> tag() const;
    std::string_view message() const noexcept;

private:
    GeometryBuildStatus status_ = GeometryBuildStatus::missing_required_tag;
    std::optional<T> value_;
    std::optional<Tag> tag_;
    std::string message_;
};

class Matrix4x4d {
public:
    // Row-major storage: m(row, column).
    // Math convention: homogeneous column vectors with left multiplication.
    // Example: world_h = index_to_world * index_h.
    double operator()(std::size_t row, std::size_t column) const;
};

class ImagePlaneGeometry {
public:
    ImageSize2D size() const;
    Point3d origin() const;
    ImageSpacing2D spacing() const;
    double spacing_i() const;
    double spacing_j() const;
    Vec3d direction_i() const;  // DICOM ImageOrientationPatient[0..2]
    Vec3d direction_j() const;  // DICOM ImageOrientationPatient[3..5]
    Vec3d normal() const;

    Point3d world_from_index(ImagePoint2D ij) const;
    ImagePoint2D index_from_world(Point3d p) const;
    bool contains_index(ImagePoint2D ij) const;
    bool contains_world(Point3d p, GeometryTolerance tolerance = {}) const;

    // index_to_world maps (i, j, normal_mm, 1) to patient world.
    // world_to_index returns (i, j, signed_normal_distance_mm, 1).
    Matrix4x4d index_to_world_matrix() const;
    Matrix4x4d world_to_index_matrix() const;
};

struct ImageFrameGeometry {
    ImagePlaneGeometry plane;
    ImageFrameGeometryKind kind = ImageFrameGeometryKind::regular_plane;
};

GeometryBuildResult<ImagePlaneGeometry> make_image_plane_geometry(
    const ImagePlaneGeometryParams& params);

class ImageVolumeGeometry {
public:
    ImageSize3D size() const;
    Point3d origin() const;
    ImageSpacing3D spacing() const;
    double spacing_i() const;
    double spacing_j() const;
    double spacing_k() const;
    Vec3d direction_i() const;
    Vec3d direction_j() const;
    Vec3d direction_k() const;

    Point3d world_from_index(ImagePoint3D ijk) const;
    ImagePoint3D index_from_world(Point3d p) const;
    bool contains_index(ImagePoint3D ijk) const;
    bool contains_world(Point3d p, GeometryTolerance tolerance = {}) const;

    Matrix4x4d index_to_world_matrix() const;
    Matrix4x4d world_to_index_matrix() const;
};

GeometryBuildResult<ImageVolumeGeometry> make_image_volume_geometry(
    const ImageVolumeGeometryParams& params);

class FrameGeometryReader {
public:
    static GeometryBuildResult<FrameGeometryReader> from_image(
        const DicomFile& file);
    static GeometryBuildResult<FrameGeometryReader> from_dataset(
        const DataSet& dataset);
    static GeometryBuildResult<FrameGeometryReader> from_segmentation(
        const seg::Segmentation& seg);

    GeometryBuildResult<ImagePlaneGeometry> plane(
        std::size_t frame_index) const;
    GeometryBuildResult<ImageFrameGeometry> image_frame_geometry(
        std::size_t frame_index) const;
    GeometryBuildResult<VolumetricPropertiesInfo> volumetric_properties(
        std::size_t frame_index) const;
    GeometryBuildResult<std::string> frame_of_reference() const;
};

// Classic single-frame image root dataset. The DataSet overload is the
// implementation primitive; the DicomFile overload forwards to file.dataset().
GeometryBuildResult<ImagePlaneGeometry> plane_from_single_frame_image(
    const DataSet& ds);
GeometryBuildResult<ImagePlaneGeometry> plane_from_single_frame_image(
    const DicomFile& file);

// Resolve plane metadata in this order:
// PerFrameFunctionalGroupsSequence -> SharedFunctionalGroupsSequence -> root dataset.
GeometryBuildResult<ImagePlaneGeometry> plane_from_multiframe_image(
    const DataSet& ds,
    std::size_t frame_index);
GeometryBuildResult<ImagePlaneGeometry> plane_from_multiframe_image(
    const DicomFile& file,
    std::size_t frame_index);

GeometryBuildResult<ImageFrameGeometry> frame_geometry_from_multiframe_image(
    const DataSet& ds,
    std::size_t frame_index);
GeometryBuildResult<ImageFrameGeometry> frame_geometry_from_multiframe_image(
    const DicomFile& file,
    std::size_t frame_index);

// Uses SOP Class specific Frame Type Sequence lookup for Enhanced MR/CT/PET:
// MR  (0018,9226), CT (0018,9329), PET (0018,9751).
// NM Image Storage is not part of this path; use an NM-specific adapter.
GeometryBuildResult<VolumetricPropertiesInfo> volumetric_properties_from_multiframe_image(
    const DicomFile& file,
    std::size_t frame_index);

// Strict: PerFrameFunctionalGroupsSequence -> SharedFunctionalGroupsSequence.
GeometryBuildResult<ImagePlaneGeometry> plane_from_seg_frame(
    const seg::Segmentation& seg,
    std::size_t frame_index);

GeometryBuildResult<std::string> frame_of_reference_from_dataset(const DataSet& ds);
GeometryBuildResult<std::string> frame_of_reference_from_segmentation(
    const seg::Segmentation& seg);

enum class OverlayCompatibility {
    compatible,
    different_frame_of_reference,
    missing_frame_of_reference,
    non_parallel_planes,
    opposite_orientation,
    out_of_plane,
    different_spacing,
    different_extent,
    requires_resampling,
};

struct OverlayCheckOptions {
    double frame_position_tolerance_mm = 1e-3;
    double normal_distance_tolerance_mm = 1e-3;
    double orientation_tolerance = 1e-4;
    double spacing_tolerance_mm = 1e-3;
    bool require_same_grid = false;
};

struct OverlayCheck {
    OverlayCompatibility status = OverlayCompatibility::different_frame_of_reference;
    bool same_frame_of_reference = false;
    bool can_transform = false;
    bool can_direct_overlay = false;
    bool same_grid = false;
    bool same_spacing = false;
    bool same_extent = false;
    bool overlaps_extent = false;
    bool source_inside_target_extent = false;
    bool requires_resampling = false;
    std::optional<IndexRange1D> target_k_range;
    double max_position_error_mm = 0.0;
    double max_normal_distance_mm = 0.0;
    double max_orientation_error = 0.0;
    double max_spacing_error_mm = 0.0;

    // Equivalent to can_transform. Direct pixel copy requires
    // can_direct_overlay; resampling paths should check requires_resampling.
    bool ok() const noexcept;
};

OverlayCheck check_overlay_compatibility(
    std::string_view source_frame_of_reference_uid,
    const ImagePlaneGeometry& source,
    std::string_view target_frame_of_reference_uid,
    const ImagePlaneGeometry& target,
    OverlayCheckOptions options = {});

OverlayCheck check_overlay_compatibility(
    std::string_view source_frame_of_reference_uid,
    const ImagePlaneGeometry& source,
    std::string_view target_frame_of_reference_uid,
    const ImageVolumeGeometry& target,
    OverlayCheckOptions options = {});

OverlayCheck check_overlay_compatibility(
    std::string_view source_frame_of_reference_uid,
    const ImageVolumeGeometry& source,
    std::string_view target_frame_of_reference_uid,
    const ImagePlaneGeometry& target,
    OverlayCheckOptions options = {});

OverlayCheck check_overlay_compatibility(
    std::string_view source_frame_of_reference_uid,
    const ImageVolumeGeometry& source,
    std::string_view target_frame_of_reference_uid,
    const ImageVolumeGeometry& target,
    OverlayCheckOptions options = {});

class PlaneToPlaneTransform {
public:
    ImagePoint2D target_index_from_source_index(ImagePoint2D source) const;
    ImagePoint2D source_index_from_target_index(ImagePoint2D target) const;
};

class PlaneToVolumeTransform {
public:
    ImagePoint3D target_index_from_source_index(ImagePoint2D source) const;
    ImagePoint2D source_index_from_target_index(ImagePoint3D target) const;
    PlaneProjection2D source_projection_from_target_index(ImagePoint3D target) const;
};

class VolumeToPlaneTransform {
public:
    ImagePoint2D target_index_from_source_index(ImagePoint3D source) const;
    PlaneProjection2D target_projection_from_source_index(ImagePoint3D source) const;
    ImagePoint3D source_index_from_target_index(ImagePoint2D target) const;
};

class VolumeToVolumeTransform {
public:
    ImagePoint3D target_index_from_source_index(ImagePoint3D source) const;
    ImagePoint3D source_index_from_target_index(ImagePoint3D target) const;
};

PlaneToPlaneTransform make_plane_to_plane_transform(
    const ImagePlaneGeometry& source,
    const ImagePlaneGeometry& target);
PlaneToVolumeTransform make_plane_to_volume_transform(
    const ImagePlaneGeometry& source,
    const ImageVolumeGeometry& target);
VolumeToPlaneTransform make_volume_to_plane_transform(
    const ImageVolumeGeometry& source,
    const ImagePlaneGeometry& target);
VolumeToVolumeTransform make_volume_to_volume_transform(
    const ImageVolumeGeometry& source,
    const ImageVolumeGeometry& target);

enum class SliceStackStatus {
    ok,
    empty,
    missing_geometry,
    missing_frame_content,
    missing_dimension_module,
    unsupported_tiled_image,
    multiple_frame_stacks,
    geometry_parse_failure,
    missing_frame_of_reference,
    mixed_frame_of_reference,
    inconsistent_rows_columns,
    inconsistent_orientation,
    inconsistent_pixel_spacing,
    inconsistent_slice_origin,
    duplicate_slice_position,
    non_uniform_spacing,
};

struct SliceStackOptions {
    GeometryTolerance tolerance;
    double slice_position_tolerance_mm = 1e-3;
    double origin_residual_tolerance_mm = 1e-3;
    bool allow_duplicate_positions = false;
};

struct ImageFrameStackOptions {
    SliceStackOptions slice_stack;
    bool allow_geometry_grouping_fallback = false;
    // TILED_FULL/TILED_SPARSE는 MVP에서 항상 unsupported_tiled_image로 거절한다.
};

struct SliceStackIssue {
    SliceStackStatus status = SliceStackStatus::ok;
    std::size_t source_index = 0;
    std::size_t frame_index = 0;
    std::optional<Tag> tag;
    std::string message;
};

struct DimensionIndexDescriptor {
    Tag dimension_index_pointer;
    Tag functional_group_pointer;
    std::string dimension_organization_uid;
    std::string label;
    std::string private_creator;
};

struct DimensionIndexValue {
    DimensionIndexDescriptor descriptor;
    std::int64_t value = 0;
};

struct ImageFrameStackKey {
    std::string stack_id;
    std::vector<DimensionIndexValue> dimension_values;
};

struct SliceStackInput {
    // source_index identifies the source dataset/file in the caller's list.
    // frame_index identifies the frame within that source; use 0 for
    // classic single-frame images.
    // frame_of_reference_uid only needs to stay valid during the call;
    // SliceStackAnalysis and SliceStackPlan own their copied UID.
    std::size_t source_index = 0;
    std::size_t frame_index = 0;
    ImagePlaneGeometry plane;
    std::string_view frame_of_reference_uid;
};

struct ImageFrameStackGroup {
    ImageFrameStackKey key;
    std::vector<std::size_t> frame_indices;
    SliceStackAnalysis analysis;
};

class ImageFrameStackAnalysis {
public:
    SliceStackStatus status() const;
    bool ok() const noexcept;
    std::span<const ImageFrameStackGroup> groups() const;
    std::span<const SliceStackIssue> issues() const;
};

struct SliceStackSlice {
    std::size_t source_index = 0;
    std::size_t frame_index = 0;
    ImagePlaneGeometry plane;
    double position_along_normal_mm = 0.0;
    double in_plane_residual_mm = 0.0;
};

struct SliceStackGap {
    std::size_t before_sorted_index = 0;
    std::size_t after_sorted_index = 0;
    double distance_mm = 0.0;
};

struct SliceStackRun {
    std::size_t first_sorted_index = 0;
    std::size_t count = 0;
    double spacing_mm = 0.0;
};

struct SliceStackItem {
    // Use source_index to find the source dataset/file, decode frame_index
    // from that source, and place it at target_k in the output volume buffer.
    std::size_t source_index = 0;
    std::size_t frame_index = 0;
    std::size_t target_k = 0;
    double position_along_normal_mm = 0.0;
    double in_plane_residual_mm = 0.0;
};

class SliceStackAnalysis {
public:
    SliceStackStatus status() const;
    bool has_usable_geometry() const noexcept;

    // The analysis object owns this UID; returned string_view stays valid while
    // the analysis object is alive.
    std::string_view frame_of_reference_uid() const;
    Vec3d normal() const;
    std::span<const SliceStackSlice> sorted_slices() const;
    std::span<const SliceStackGap> gaps() const;
    std::span<const SliceStackRun> uniform_runs() const;
    std::span<const SliceStackIssue> issues() const;
    double max_in_plane_residual_mm() const;
};

class SliceStackPlan {
public:
    SliceStackStatus status() const;
    bool ok() const noexcept;

    // The plan object owns this UID; returned string_view stays valid while the
    // plan object is alive.
    std::string_view frame_of_reference_uid() const;
    std::optional<ImageVolumeGeometry> volume_geometry() const;
    std::span<const SliceStackItem> placements() const;
    std::span<const SliceStackIssue> issues() const;
};

SliceStackAnalysis analyze_slice_stack(
    std::span<const SliceStackInput> inputs,
    SliceStackOptions options = {});

SliceStackPlan plan_slice_stack(
    std::span<const SliceStackInput> inputs,
    SliceStackOptions options = {});

// Convenience overloads for classic multi-instance image series.
SliceStackAnalysis analyze_slice_stack(
    std::span<const DataSet* const> datasets,
    SliceStackOptions options = {});

SliceStackPlan plan_slice_stack(
    std::span<const DataSet* const> datasets,
    SliceStackOptions options = {});

// Convenience overloads for Enhanced multi-frame images.
ImageFrameStackAnalysis analyze_image_frame_stacks(
    const DicomFile& file,
    ImageFrameStackOptions options = {});

SliceStackAnalysis analyze_image_frame_stack(
    const DicomFile& file,
    ImageFrameStackOptions options = {});

SliceStackAnalysis analyze_image_frame_stack(
    const DicomFile& file,
    std::span<const std::size_t> frame_indices,
    ImageFrameStackOptions options = {});

SliceStackPlan plan_image_frame_stack(
    const DicomFile& file,
    ImageFrameStackOptions options = {});

SliceStackPlan plan_image_frame_stack(
    const DicomFile& file,
    std::span<const std::size_t> frame_indices,
    ImageFrameStackOptions options = {});

} // namespace dicom::geometry
```

## LabelMapSegmentationStorage 구현 체크리스트

### 구현 진행 현황 (2026-06-25)

- [x] 1차 core 구현 완료: `LabelMapSegmentationStorage` + `SegmentationType=LABELMAP` 조합을 허용하고, 기존 `SegmentationStorage` + `LABELMAP` 조합은 strict error로 유지한다.
- [x] `from_dicomfile()`은 metadata-only construction을 유지한다. unknown label 검증과 PixelData payload 검증은 frame decode/presence 계산 또는 `validate_label_values()` 시점에 수행한다.
- [x] SOP Class helper 정책 구현: `is_segmentation_storage()`는 기존 SEG만, `is_labelmap_segmentation_storage()`는 Label Map SEG만, `is_any_segmentation_storage()`는 둘 중 하나만 true로 반환한다. `SOPClassUID` / `MediaStorageSOPClassUID` 충돌은 bool helper에서 false, strict classifier에서 error다.
- [x] 공통 API 구현: `present_segment_numbers(frame)`, `mask_for_segment(frame, segment_number, options)`, `mask_for_segment_into(...)`, `validate_label_values()`를 C++/Python에 연결했다.
- [x] LABELMAP decode 구현: 8-bit는 기존 `decode_frame_into(uint8)`와 labelmap-specific uint8 decode를 허용하고, 16-bit는 명시적 `decode_labelmap_frame_into(uint16)` / Python native `np.uint16` 경로만 허용한다.
- [x] LABELMAP presence cache 구현: `Segmentation` 소유 fixed-size cache array, ready empty 상태, immutable `shared_ptr<const vector<uint16_t>>`, ready entry/index no-replace, caller-owned publish, duplicate concurrent scan 허용 정책을 따른다.
- [x] payload/metadata 기본 정책 구현: native uncompressed와 지원되는 encapsulated lossless source는 동일한 typed sample contract로 처리한다. Big Endian, detached payload, missing PixelData, invalid native length/padding, lossy/near-lossless/unknown compressed source는 decode/validate/write/transcode 시 명확한 error로 처리한다. LABELMAP의 `PixelPaddingValue`는 background segment number로 허용하고, `PixelPaddingRangeLimit`은 reject한다.
- [x] Python binding/stub 반영: `present_segment_numbers()`는 tuple, labelmap `to_array()`는 `np.uint8`/`np.uint16`, `decode_frame()` bytes는 native typed bytes, `decode_frame_into()`는 unsigned dtype/endian을 검사한다.
- [x] 집중 테스트 추가: synthetic 8-bit/16-bit LABELMAP, lazy unknown-label validation, SOP conflict/helper, `PALETTE COLOR`, `PixelPaddingValue`, `SegmentNumber=0`, metadata-only absent segment, dtype mismatch, Big Endian, missing/trailing/padded PixelData, detached/PixSeq C++ smoke를 커버한다.
- [x] 공개 계약 정리: public header, Python docstring, en/ko developer segmentation 문서에서 LABELMAP 지원 범위와 native typed sample bytes 계약을 반영했다.
- [x] `plane_from_seg_frame()` LabelMap synthetic geometry smoke test를 추가했다.
- [x] cache 재사용을 decode-count로 검증하는 test-only 계측 테스트, concurrent lazy scan stress, C++ header partial-write/BINARY-FRACTIONAL-LABELMAP contract 주석 보강을 추가했다.
- [x] SEG write/transcode 초기 preflight hook 구현: 기존 `DicomFile` generic write path에서 SOP Class / `SegmentationType` 기반으로 SEG 정책을 감지한다. `classify`와 PixelData invariant validation은 분리하고, 완전 as-is write는 기존 roundtrip 성격을 유지하며, 실제 PixelData write/transcode 경로에서 타입별 invariant, lossy target/profile reject, BINARY 1-bit transcode unsupported guard를 적용한다.
- [x] Encapsulated Uncompressed와 RLE Lossless SEG roundtrip regression을 추가했다. FRACTIONAL 8-bit, LABELMAP 8-bit, LABELMAP 16-bit는 기존 generic write/transcode 경로로 쓰고 다시 SEG API로 읽어 `decode`, presence, mask, `frames_for_segment`, `validate_label_values()`를 검증한다.
- [x] write/transcode streaming semantic validation 구현: compressed source decode는 `DecodeInfo::encoded_lossy_state`가 lossless일 때만 허용하고, FRACTIONAL sample이 `MaximumFractionalValue`를 넘거나 LABELMAP decoded label이 `SegmentSequence`에 없으면 native/encapsulated transcode 중 reject한다.
- [ ] 남은 후속: 실제 외부 compressed FRACTIONAL/LABELMAP sample 기반 regression, unsupported/plugin-missing negative test, BINARY 1-bit core layout/write 지원을 진행한다.

### 범위 결정

- [ ] `LabelMapSegmentationStorage` 지원은 DicomSDL core에서 label value array를 안전하게 읽고 검증하는 범위로 한정한다.
- [ ] palette 렌더링, color mapping, opacity, UI legend, viewport overlay composition은 GUI/viewer layer 책임으로 둔다.
- [ ] DicomSDL SEG adapter는 `SegmentSequence` metadata와 decoded label sample을 연결할 수 있게 하고, 화면 표시 색을 직접 결정하지 않는다.
- [ ] `from_dicomfile()`은 LABELMAP에서도 metadata-only construction을 유지한다. PixelData 전체 scan, label value presence 계산, unknown label 검증은 open 시점에 수행하지 않는다.
- [ ] unknown label 검증은 frame decode 계열(`to_array`, `decode_frame_into`, `present_segment_numbers`, `mask_for_segment`) 또는 명시적 `validate_label_values()`에서 수행한다.
- [ ] 1차 MVP는 `LabelMapSegmentationStorage` SOP Class + `SegmentationType=LABELMAP` 조합만 허용한다.
- [ ] 기존 `SegmentationStorage` SOP Class에서 `SegmentationType=LABELMAP`이 들어오는 경우는 별도 호환 모드가 필요해질 때까지 strict error로 둔다.
- [ ] `BINARY` / `FRACTIONAL` 동작과 public return contract는 기존과 호환되게 유지한다.

### SOP Class / 진입 정책

- [ ] `dataset_has_labelmap_segmentation_sop_class()`를 hard reject 용도가 아니라 Label Map SEG 판정 helper로 전환한다.
- [ ] `from_dicomfile()`에서 `LabelMapSegmentationStorage`를 허용하고, labelmap-specific validation path로 분기한다.
- [ ] SOP Class 판정은 `SOPClassUID`와 `MediaStorageSOPClassUID`가 둘 다 존재할 때 서로 일치해야 한다. 둘 중 하나가 `SegmentationStorage`이고 다른 하나가 `LabelMapSegmentationStorage`처럼 충돌하면 `from_dicomfile()`에서 strict reject한다.
- [ ] `is_segmentation_storage()` / `is_labelmap_segmentation_storage()` bool helper는 SOP Class UID가 충돌하는 dataset에서는 `false`를 반환한다.
- [ ] `from_dicomfile()`은 bool helper만 믿지 않고 strict SOP Class classifier를 사용한다. `SOPClassUID`와 `MediaStorageSOPClassUID` 충돌은 strict classifier에서 error로 처리한다.
- [ ] `is_segmentation_storage()`는 기존 `SegmentationStorage` SOP Class만 의미하도록 유지한다.
- [ ] `is_labelmap_segmentation_storage()`를 추가해 `LabelMapSegmentationStorage` SOP Class를 별도로 판정한다.
- [ ] 필요하면 `is_any_segmentation_storage()`를 추가해 두 SOP Class를 모두 포함하는 helper로 둔다. SOP Class UID가 충돌하는 dataset에서는 `is_any_segmentation_storage()`도 `false`를 반환한다.
- [ ] Python `dicom.seg.is_segmentation_storage()` docstring은 기존 의미를 유지하고, `dicom.seg.is_labelmap_segmentation_storage()` / `is_any_segmentation_storage()` docstring을 새로 추가한다.
- [ ] `SegmentationType::labelmap` enum은 유지하되 "recognized but unsupported" 주석을 "supported through LabelMapSegmentationStorage path"로 바꾼다.
- [ ] 별도 `SegmentationStorageKind` public enum은 추가하지 않는다. public semantic 분기는 `SegmentationType`을 기준으로 하고, SOP Class 판정이 필요한 code는 `is_segmentation_storage()` / `is_labelmap_segmentation_storage()` 또는 raw dataset metadata를 사용한다.

### 공통 API 재설계 방향

- [ ] SEG public API의 중심을 `referenced_segment_number`에서 `present_segment_numbers(frame)`와 `mask_for_segment(frame, segment_number, options)`로 옮긴다.
- [ ] `present_segment_numbers(frame)`는 BINARY/FRACTIONAL/LABELMAP 모두에서 사용할 수 있는 공통 frame-to-segment query로 정의한다.
- [ ] BINARY/FRACTIONAL에서 `present_segment_numbers(frame)`는 기존 `ReferencedSegmentNumber` 기반으로 단일 segment number를 반환한다.
- [ ] BINARY/FRACTIONAL의 `present_segment_numbers(frame)`는 PixelData를 보지 않는다. 따라서 빈 BINARY frame이어도 declared `ReferencedSegmentNumber`를 반환한다는 점을 문서화한다.
- [ ] BINARY/FRACTIONAL relaxed parse에서 `ReferencedSegmentNumber`가 없거나 0이면 `present_segment_numbers(frame)`는 empty span을 반환한다. `referenced_segment_number()` compatibility accessor는 이 경우 명확한 error를 던진다.
- [ ] LABELMAP에서 `present_segment_numbers(frame)`는 decoded label values 또는 label presence cache 기반으로 해당 frame에 실제 등장하는 non-background segment numbers를 반환한다.
- [ ] `mask_for_segment(frame, segment_number, options)`는 모든 SEG type에서 `rows * columns` 크기의 `uint8` 0/1 mask를 반환하는 공통 helper로 정의한다.
- [ ] BINARY에서 `mask_for_segment()`는 frame의 `ReferencedSegmentNumber`가 요청 segment와 같으면 decoded BINARY frame을 반환하고, 다르면 zero mask를 반환한다.
- [ ] FRACTIONAL에서 `mask_for_segment()`는 frame의 `ReferencedSegmentNumber`가 요청 segment와 같을 때 `SegmentMaskOptions::fractional_threshold`를 적용해 binary mask를 만든다.
- [ ] `SegmentMaskOptions`는 단순하게 둔다. 1차 API는 normalized `fractional_threshold`와 `error_when_not_present_in_frame`만 포함한다.
- [ ] `error_when_not_present_in_frame`은 `SegmentSequence`에 존재하는 segment가 해당 frame에 present하지 않을 때만 적용한다. `SegmentSequence`에 없는 segment 요청은 옵션과 무관하게 항상 error다.
- [ ] `fractional_threshold` 범위는 `[0.0, 1.0]`로 고정하고 범위를 벗어나면 error를 낸다.
- [ ] `fractional_threshold = 0.0` 기본값은 raw sample이 0보다 크면 present라는 의미로 정의한다. 그 외 threshold는 `sample / MaximumFractionalValue >= fractional_threshold` 비교식을 사용한다.
- [ ] LABELMAP에서 `mask_for_segment()`는 decoded label array에서 `sample == segment_number` 비교로 mask를 만든다.
- [ ] 8-bit LABELMAP에서 `SegmentNumber > 255`처럼 metadata-only absent segment에 대해 `mask_for_segment(segment_number)`를 호출해도 frame을 scan/decode해서 unknown label validation을 수행한 뒤 zero mask를 반환한다. validation을 건너뛰고 shortcut zero mask를 반환하지 않는다.
- [ ] `frames_for_segment(unknown_segment)`와 `segment_frame_count(unknown_segment)`는 기존 호환성을 위해 empty/0을 반환하고 LABELMAP PixelData scan을 유발하지 않는다. `mask_for_segment(unknown_segment)`는 error로 둔다.
- [ ] `to_array(frame)`는 stored representation에 가까운 typed frame decode API로 유지한다. BINARY는 uint8 0/1, FRACTIONAL은 raw uint8, LABELMAP은 uint8/uint16 label value array를 반환한다.
- [ ] `decode_frame()` / `decode_frame_into()`는 low-level typed decode API로 유지하고, segment별 binary mask가 필요하면 `mask_for_segment()`를 쓰도록 문서화한다.
- [ ] `referenced_segment_number`는 BINARY/FRACTIONAL compatibility accessor로 남기되, 공통 API로 권장하지 않는다.
- [ ] `SegmentFrameView::referenced_segment_number()`의 C++ 반환 타입은 `std::uint16_t`로 유지한다. LABELMAP에서 호출하면 "`LABELMAP frame has no single ReferencedSegmentNumber; use present_segment_numbers()`" 계열의 명확한 compatibility error를 던진다.
- [ ] Python `SegmentFrame.referenced_segment_number`도 기존 `int` 반환 contract를 유지한다. LABELMAP에서 접근하면 `ValueError`/`RuntimeError` 계열 예외를 던지고, 새 code path는 `frame.present_segment_numbers()`를 사용하게 한다.
- [ ] Python `SegmentFrame.present_segment_numbers()` / `Segmentation.present_segment_numbers(frame)`는 borrowed memory view가 아니라 immutable `tuple[int, ...]`로 반환한다.
- [ ] Python `SegmentFrame.__repr__`는 LABELMAP에서 `referenced_segment_number`를 호출하지 않는다. LABELMAP repr은 `present_segment_numbers`를 강제로 계산하지 않고 `SegmentFrame(index=..., referenced_segment_number=<labelmap>)`처럼 metadata-only로 안전하게 표시한다.

### Metadata validation

- [ ] `SegmentationType`은 `LABELMAP`이어야 한다.
- [ ] `BitsAllocated`는 8 또는 16만 허용한다.
- [ ] `BitsStored`는 `BitsAllocated`와 일치해야 한다.
- [ ] `HighBit`는 8-bit이면 7, 16-bit이면 15여야 한다.
- [ ] `PixelRepresentation`은 unsigned 값만 허용한다.
- [ ] `SamplesPerPixel`은 1이어야 한다.
- [ ] `PhotometricInterpretation`은 MVP에서 `MONOCHROME2`와 `PALETTE COLOR`만 허용한다. `PALETTE COLOR`여도 labelmap decode path는 palette/LUT/display transform을 절대 적용하지 않고 stored label value array만 반환한다.
- [ ] `Rows`, `Columns`, `NumberOfFrames`, `FrameOfReferenceUID`, `SegmentSequence` 필수 여부를 labelmap path에서 별도로 검증한다.
- [ ] `SegmentsOverlap`이 있으면 `NO`만 허용한다.
- [ ] `SegmentNumber`는 unique이면 되고 1부터 연속일 필요가 없다는 점을 검증과 문서에 반영한다.
- [ ] `SegmentNumber=0`은 LABELMAP에서 `PixelPaddingValue=0` background segment로 허용하고, BINARY/FRACTIONAL에서는 명확히 reject한다.
- [ ] PixelData에 나타나는 non-background label value가 `SegmentSequence`에 없으면 명확한 error를 반환한다. 단, 이 검증은 `from_dicomfile()` 시점이 아니라 해당 frame decode/presence 계산 또는 `validate_label_values()` 시점에 수행한다.
- [ ] background 값은 고정된 `0`이 아니라 `PixelPaddingValue`가 지정한 segment number로 둔다. 이 값은 `SegmentSequence`에 존재해야 하며 `present_segment_numbers()`와 LABELMAP all-frame presence index에서 제외한다.
- [ ] LABELMAP에서 `PixelPaddingValue`는 background/ignored label로 해석한다. `PixelPaddingRangeLimit`은 표준 제약에 맞춰 unsupported/reject로 둔다.
- [ ] 8-bit LABELMAP에서 `SegmentNumber > 255`가 `SegmentSequence`에 존재하는 것은 metadata-only absent segment로 허용한다. 해당 값은 PixelData에 나타날 수 없으므로 `present_segment_numbers()`에 나오지 않고, `mask_for_segment()`는 zero mask를 반환한다.
- [ ] `SegmentSequence` 검증 후 instance-level `valid_label` table을 만든다. label value 공간은 최대 65536이므로 `std::bitset<65536>` 또는 동등한 compact bit table을 사용하고, `valid_label[segment_number] = true`로 채운다. LABELMAP의 `SegmentNumber=0`도 `SegmentSequence`에 있으면 valid label로 표시하되, `PixelPaddingValue`와 같으면 presence 결과에서 제외한다.
- [ ] `segment_by_number()` / requested segment validation은 기존 map을 쓰되, decoded PixelData label validation은 `valid_label[value]` table lookup으로 O(1) 처리한다.

### Frame / Segment data model

- [ ] public `SegmentFrameView::referenced_segment_number()` 반환 타입은 바꾸지 않는다. 내부 record는 labelmap absence를 표현할 수 있어야 하지만, public accessor는 LABELMAP에서 compatibility error를 던진다.
- [ ] LABELMAP에서는 한 frame 안에 여러 segment label value가 공존할 수 있음을 `Segmentation` model에 반영한다.
- [ ] `SegmentFrameRecord`는 BINARY/FRACTIONAL의 기존 `referenced_segment_number` scalar를 유지한다. BINARY/FRACTIONAL `present_segment_numbers()`는 heap allocation 없이 `std::span(&record.referenced_segment_number, 1)`를 반환한다.
- [ ] LABELMAP present labels는 `SegmentFrameRecord` 안에 직접 저장하지 않고, `Segmentation` 소유의 fixed-size lazy cache array에 둔다. BINARY/FRACTIONAL 대량 frame에서 per-frame vector allocation이 생기지 않게 한다.
- [ ] LABELMAP per-frame cache는 "uninitialized"와 "computed empty"를 구분해야 한다. 1차 구조는 `PresenceState { uninitialized, ready }` + `std::shared_ptr<const std::vector<std::uint16_t>> labels` 또는 동등한 immutable storage로 둔다.
- [ ] LABELMAP per-frame persistent cache는 전체 label table을 frame마다 저장하지 않는다. ready 상태의 값은 sorted compact `std::vector<std::uint16_t>` present label list로 저장한다. all-background frame은 ready + empty vector다.
- [ ] ready cache entry는 절대 replace하지 않는다. 이미 `present_segment_numbers()`가 반환한 span이 dangling되지 않도록, ready entry의 immutable vector storage는 `Segmentation` lifetime 동안 유지한다.
- [ ] `validate_label_values()`나 all-frame builder가 같은 frame을 다시 scan하더라도 ready cache entry를 새 vector로 교체하지 않는다. 이미 ready인 frame은 기존 cache를 재사용하고, uninitialized entry만 publish한다.
- [ ] `SegmentFrameView::present_segment_numbers()`를 추가한다.
- [ ] `Segmentation::present_segment_numbers(frame_index)` convenience API를 추가한다.
- [ ] `frames_for_segment(segment_number)`는 공통 semantics를 "해당 segment가 present인 frame 목록"으로 재정의한다.
- [ ] BINARY/FRACTIONAL에서 `frames_for_segment()`는 기존 `ReferencedSegmentNumber` index를 그대로 사용한다.
- [ ] LABELMAP에서 `frames_for_segment()`는 present label cache 기반으로 해당 segment label이 등장하는 frame 목록을 반환한다. 최초 호출은 모든 frame의 presence를 계산할 수 있으므로 expensive API로 문서화한다.
- [ ] LABELMAP에서 `frames_for_segment()` / `segment_frame_count()`는 lazy all-frame scan 중 decode error 또는 unknown label validation error를 던질 수 있다. 기존 BINARY/FRACTIONAL 동작은 유지한다.
- [ ] `segment_frame_count(segment_number)`는 `frames_for_segment(segment_number).size()`와 같은 의미로 고정한다.
- [ ] `SegmentFrameView::referenced_segment_number`는 labelmap에서 의미가 없으므로 labelmap에서 호출 시 명확한 compatibility error를 낸다.
- [ ] frame별 present label cache를 둔다. 반복 overlay preflight, `present_segment_numbers()`, `frames_for_segment()` 성능을 위해 lazy cache가 유력하다.
- [ ] cache는 thread-safe lazy init으로 고정한다. construction-time full scan은 대용량 LABELMAP open 성능을 해치므로 사용하지 않는다.
- [ ] thread-safe cache 구현은 `Segmentation` 단위 mutex + frame_count 크기의 `labelmap_presence_cache_` fixed-size array로 시작한다. `std::mutex` / `std::once_flag`를 `SegmentFrameRecord`에 직접 넣지 않는다. record가 non-movable이 되어 `std::vector<SegmentFrameRecord>`와 충돌하는 것을 피한다.
- [ ] `labelmap_presence_cache_`는 construction 이후 resize하지 않는다. cache entry는 movable한 상태 객체만 담거나, 필요하면 `std::vector<std::unique_ptr<FramePresenceCache>>`처럼 이동 안정성을 보장하는 구조를 사용한다.
- [ ] `present_segment_numbers(frame)`는 해당 frame만 decode/scan하고 cache한다. scan 중에는 임시 scratch `seen` table을 사용한다. 8-bit LABELMAP은 256-entry table, 16-bit LABELMAP은 65536-bit table을 사용해 label 등장 여부를 표시한다.
- [ ] 16-bit LABELMAP의 65536-bit `seen` table은 매 호출 stack allocation으로 두지 않는다. thread-local scratch, reusable heap allocation, 또는 local heap-backed bitset 중 하나를 선택한다. persistent per-frame cache만 compact vector로 유지한다.
- [ ] frame scan algorithm은 `sample == 0` skip, `!valid_label[sample]`이면 unknown label error, 그 외 `seen[sample] = true` 순서로 처리한다. scan 완료 후 `seen`에서 true인 label만 오름차순 compact vector로 옮겨 per-frame cache로 publish한다. label이 하나도 없으면 ready + empty vector로 publish한다.
- [ ] labelmap scan/decode는 내부 `decode_and_scan_labelmap_frame(frame_index, options/sink)` 같은 단일 helper로 공통화한다. `to_array()`, `decode_frame()`, `decode_frame_into()`, `decode_labelmap_frame_into()`, `present_segment_numbers()`, `mask_for_segment()`, `validate_label_values()`, `frames_for_segment()`는 모두 이 helper를 사용한다.
- [ ] helper request/result shape를 명확히 둔다. 예: `LabelmapFrameScanRequest { decoded_out?, mask_out?, target_segment?, collect_presence }`와 `LabelmapFrameScanResult { present_labels, decoded_written, mask_written }`. `present_segment_numbers()` / `validate_label_values()` 경로는 decoded output buffer를 요구하지 않아 불필요한 frame-sized allocation을 만들지 않는다.
- [ ] `LabelmapFrameScanRequest`의 output buffers는 non-overlapping이어야 한다. `decoded_out`과 `mask_out`이 같은 메모리 또는 겹치는 메모리를 가리키는 aliasing은 unsupported로 두고 debug assert 또는 명확한 internal precondition으로 처리한다.
- [ ] `decode_and_scan_labelmap_frame(...)` helper는 cache/index를 직접 publish하지 않는다. helper는 decoded samples write, optional mask write, sorted present labels, validation status를 담은 `LabelmapFrameScanResult` 같은 candidate/result만 반환한다.
- [ ] publish는 caller가 담당한다. `present_segment_numbers()`는 해당 frame 단위로 publish하고, `mask_for_segment()`는 helper result에서 만든 frame cache candidate를 frame 단위로 publish할 수 있으며, `validate_label_values()` / all-frame builder는 전체 성공 후 bulk publish한다.
- [ ] `to_array()`, `decode_frame()`, `decode_frame_into()`, `decode_labelmap_frame_into()` 같은 LABELMAP public decode API도 helper 성공 시 frame presence cache candidate를 no-replace 방식으로 publish한다. 실패 시에는 publish하지 않는다.
- [ ] LABELMAP의 모든 public frame decode API는 stored label values를 반환하면서 동시에 membership validation을 수행한다. unknown non-background label은 decode API에서도 error다.
- [ ] `mask_for_segment()`는 labelmap frame decode 중 presence collection과 mask generation을 한 pass에서 수행할 수 있어야 한다. presence cache가 없으면 mask 생성과 동시에 cache candidate를 만들고, helper 성공 후 caller가 frame cache publish를 시도한다.
- [ ] 동시 lazy scan은 MVP에서 중복 수행을 허용한다. 두 thread가 같은 frame 또는 all-frame index를 동시에 scan할 수 있지만, publish는 mutex 아래에서 한 번만 성공한다. 이미 ready/published인 storage가 있으면 늦게 끝난 scan 결과는 버린다.
- [ ] concurrent publish 순서는 `local result 완성 -> mutex 획득 -> 아직 unpublished인 entry/index만 publish -> 이미 published면 local result discard -> mutex release`로 고정한다.
- [ ] 중복 scan을 줄이는 `in_progress` state / condition variable은 후속 최적화로 둔다. correctness와 lifetime 보장을 1차 목표로 한다.
- [ ] 반복 호출은 cached vector/span을 재사용한다. scratch `seen` table은 호출 중 임시 저장소로만 쓰고 per-frame마다 영구 보관하지 않는다.
- [ ] `frames_for_segment(segment_number)`는 all-frame presence index가 없으면 모든 frame의 compact presence cache를 채우며 segment-to-frame index를 만든다. 이 API는 LABELMAP에서 `noexcept`가 될 수 없으므로 C++ 선언에서 `noexcept`를 제거한다.
- [ ] LABELMAP all-frame segment-to-frame index는 `segment_number -> vector<frame_index>` 형태로 만든다. 구현은 present segment 수가 적으면 `unordered_map<std::uint16_t, std::vector<std::size_t>>`, dense table이 더 단순하면 65536 table/vector 중 코드베이스 스타일에 맞춰 선택하되, empty vector 65536개의 overhead를 의식한다.
- [ ] published all-frame index에서 missing key는 empty result로 해석하고 rebuild trigger가 아니다. `SegmentSequence`에는 있지만 어떤 LABELMAP frame에도 등장하지 않는 known-absent segment도 re-scan 없이 empty view / count 0을 반환한다.
- [ ] all-frame segment-to-frame index는 local builder에서 완성한 뒤 mutex 아래에서 한 번에 immutable storage로 publish한다. 성공 publish 후에는 rehash/mutation으로 `SegmentFrameListView`가 빌린 vector 주소가 invalidate되지 않아야 한다.
- [ ] published all-frame segment-to-frame index는 절대 replace하지 않는다. `validate_label_values()`가 나중에 성공하더라도 이미 published된 index storage를 교체하지 않는다.
- [ ] all-frame builder는 public `present_segment_numbers()`를 mutex 안에서 호출하지 않는다. local builder에서 `decode_and_scan_labelmap_frame()` helper를 직접 호출하고, 마지막 publish 단계에서만 mutex를 잡아 deadlock/reentrant call을 피한다.
- [ ] Python `seg.frames_for_segment(segment_number)`는 메서드 호출 시점에 C++ all-frame index build를 강제로 수행하고, 이 시점에 비용과 예외가 발생한다. 반환된 `SegmentFrameList`의 `__len__` / `__getitem__` / iteration은 이미 published된 index만 참조한다.
- [ ] Python `SegmentFrameList.__len__` / `__getitem__`가 같은 segment에 대해 반복 호출해도 all-frame index build는 성공 후 한 번만 수행한다.
- [ ] lazy cache error propagation 정책은 "실패는 캐시하지 않음"으로 고정한다. cache miss 중 decode/validation error가 나면 호출자에게 예외를 전달하고, 해당 uninitialized cache는 uninitialized 상태로 남겨 다음 호출에서 재시도한다. 이미 ready였던 cache entry는 그대로 유지한다. all-frame segment index도 전체 scan이 성공한 뒤에만 publish한다.

### Pixel decode API

- [ ] 기존 `decode_frame_into(std::span<std::uint8_t>)` contract는 유지한다. BINARY/FRACTIONAL과 8-bit LABELMAP만 이 overload를 사용한다.
- [ ] 8-bit LABELMAP에서 `decode_frame_into(std::span<std::uint8_t>)`와 `decode_labelmap_frame_into(std::span<std::uint8_t>)`는 같은 stored label values를 쓰는 alias로 정의한다. C++ labelmap-specific code에는 명시적 `decode_labelmap_frame_into()`를 권장하지만, 기존 `decode_frame_into(uint8)` 경로도 허용한다.
- [ ] 16-bit LABELMAP은 일반 `decode_frame_into(std::span<std::uint16_t>)` overload로 열지 않고, 명시적 `decode_labelmap_frame_into(...)` API로 시작한다. BINARY/FRACTIONAL의 `uint16_t` decode 의미가 애매해지는 것을 피한다.
- [ ] `decode_labelmap_frame_into(std::span<std::uint8_t>)`는 `BitsAllocated=8`일 때만 성공한다. 16-bit LABELMAP을 uint8로 truncate하지 않는다.
- [ ] `decode_labelmap_frame_into(std::span<std::uint16_t>)`는 `BitsAllocated=16`일 때만 성공한다. 8-bit LABELMAP을 uint16으로 widen하지 않는다.
- [ ] output element type과 `BitsAllocated` mismatch는 명확한 error로 처리한다.
- [ ] C++ API:
  - `labelmap_bits_allocated()`
  - `decode_labelmap_frame_into(frame, std::span<std::uint8_t>)`
  - `decode_labelmap_frame_into(frame, std::span<std::uint16_t>)`
  - `decode_labelmap_frame_bytes(frame)`
- [ ] C++ `decode_labelmap_frame_bytes(frame)` 반환 bytes는 `decode_labelmap_frame_into()`가 만드는 host/native-endian typed samples의 contiguous bytes와 동일하게 정의한다. 16-bit LABELMAP bytes는 DICOM transfer syntax raw endian이 아니라 host/native-endian `uint16_t` byte order다.
- [ ] Python `to_array()`는 labelmap에서 `np.uint8` 또는 `np.uint16` dtype을 반환하게 한다.
- [ ] 16-bit LABELMAP decode contract: C++ typed decode는 host-endian `std::uint16_t` numeric values를 output span에 쓴다. Python `to_array()`는 native-endian `np.uint16` array를 반환한다.
- [ ] Python `decode_frame()`가 bytes를 반환하는 기존 contract는 유지하되, 반환 bytes는 `to_array(frame).tobytes(order="C")`와 같은 native dtype byte order로 정의한다. DICOM transfer syntax raw bytes가 필요하면 raw `PixelData`에 접근하도록 문서화한다.
- [ ] Python `decode_frame_into()`는 output itemsize뿐 아니라 buffer format/dtype과 endian도 검사한다. 8-bit LABELMAP은 unsigned byte buffer를 받으며, `bytearray` / writable `memoryview` 같은 byte-oriented buffer는 허용한다. 16-bit LABELMAP은 native-endian unsigned uint16 buffer만 받는다. `np.int16`, signed format, non-native-endian `uint16`은 명확히 reject한다.
- [ ] `decode_frame_into()` / `decode_labelmap_frame_into()` / `mask_for_segment_into()`는 error 발생 시 output buffer strong guarantee를 제공하지 않는다. validation 또는 decode error가 나면 output buffer가 partial write 상태일 수 있음을 header/doc에 명시한다.
- [ ] BINARY는 계속 uint8 0/1 mask를 반환하고, FRACTIONAL은 계속 uint8 raw sample을 반환한다.
- [ ] `mask_for_segment(frame, segment_number, options)` helper를 core SEG API에 추가한다.
- [ ] `mask_for_segment()` output은 항상 uint8 0/1이고, palette/rendering 의미를 갖지 않는다.
- [ ] `mask_for_segment_into(frame, segment_number, out, options)` allocation-free overload를 C++/Python에 둔다.
- [ ] requested segment가 해당 frame에 present하지 않으면 기본적으로 zero mask를 반환하고, `SegmentMaskOptions::error_when_not_present_in_frame=true`이면 error를 낸다.
- [ ] requested segment가 `SegmentSequence`에 없는 경우는 frame presence 여부와 무관하게 명확한 error로 처리한다. 존재하지만 해당 frame에 없으면 기본 zero mask, strict 옵션에서 error로 처리한다.
- [ ] compressed/encapsulated Label Map SEG는 MVP에서 명시적으로 unsupported로 둔다.
- [ ] MVP는 uncompressed native PixelData만 지원한다. Big Endian transfer syntax는 1차 구현에서 명확한 unsupported error로 둔다. 지원하려면 DicomFile endian-normalized decode path와 16-bit LABELMAP 테스트를 함께 추가한다.
- [ ] native PixelData length 검증은 DICOM element padding byte를 고려한다. expected byte count와 정확히 같거나, expected가 홀수일 때 `expected + 1`이고 마지막 byte가 `0x00`인 경우만 허용한다. 그 외 trailing data나 short data는 size mismatch error로 처리한다.
- [ ] `validate_label_values()`를 추가하면 모든 frame을 명시적으로 decode/scan해 unknown label, unsupported compressed PixelData, bad payload length를 한 번에 확인하는 expensive API로 문서화한다.
- [ ] all-frame segment-to-frame index가 이미 성공적으로 published된 상태라면 `validate_label_values()`는 전체 frame validation이 완료된 것으로 보고 재-scan 없이 return한다.
- [ ] `validate_label_values()`는 성공 시 uninitialized per-frame presence cache와 all-frame segment-to-frame index를 채운다. 전체 scan 결과를 local cache/index builder에 만든 뒤 성공하면 한 번에 publish하고, 실패하면 validate 중 새로 scan한 frame cache/index는 publish하지 않는다. validate 시작 전에 이미 ready였던 cache entry는 rollback하지 않고 그대로 유지한다.
- [ ] `validate_label_values()` publish 단계는 기존 ready per-frame cache entry와 기존 published all-frame index를 replace하지 않는다. 이미 존재하는 immutable storage는 그대로 유지한다.

### Python binding policy

- [ ] Python `seg.frames_for_segment(segment_number)`는 LABELMAP에서 호출 시점에 all-frame index build를 수행하고 예외도 이 시점에 발생하게 한다. lazy list view로 비용/예외를 `len()`이나 iteration까지 미루지 않는다.
- [ ] Python `present_segment_numbers()` 반환값은 borrowed memory view가 아니라 `tuple[int, ...]`로 둔다.
- [ ] Python LABELMAP decode, presence scan, `validate_label_values()`, `frames_for_segment()` all-frame scan은 GIL을 release한다.

### Geometry / overlay 연동

- [ ] `plane_from_seg_frame()`은 Label Map SEG에서도 frame geometry를 읽을 수 있어야 한다.
- [ ] Label Map SEG frame geometry는 `PerFrameFunctionalGroupsSequence -> SharedFunctionalGroupsSequence` strict resolution을 그대로 따른다.
- [ ] labelmap이라고 해서 root dataset geometry fallback을 추가하지 않는다.
- [ ] overlay compatibility helper는 label value semantics를 보지 않고 geometry와 `FrameOfReferenceUID`만 판정한다.
- [ ] labelmap frame을 target image에 직접 overlay할 수 있어도 색/alpha/palette 결정은 caller에게 맡긴다.
- [ ] volume reconstruction이나 resampling은 labelmap 지원 MVP에 포함하지 않는다.

### Tests

- [ ] synthetic 8-bit `LabelMapSegmentationStorage` read test를 추가한다.
- [ ] synthetic 16-bit `LabelMapSegmentationStorage` read test를 추가한다.
- [ ] non-contiguous `SegmentNumber` 예: `1`, `7`, `1024`가 정상 동작하는지 검증한다.
- [ ] PixelData에 `SegmentSequence`에 없는 label value가 있으면 `from_dicomfile()`이 아니라 해당 frame decode/presence 계산 또는 `validate_label_values()`에서 error가 나는지 검증한다.
- [ ] unknown label 검증이 `SegmentSequence`에서 만든 `valid_label` table 기반으로 동작하는지 검증한다. 예: valid segment `1`, `7`만 있는 8-bit LABELMAP에서 pixel value `9`가 나오면 decode/presence 시점에 error.
- [ ] background label `0` 정책을 테스트로 고정한다.
- [ ] all-background LABELMAP frame에서 `present_segment_numbers()`가 empty list를 반환하고, 반복 호출 시 "uninitialized"로 오인해 재-scan하지 않는지 테스트한다.
- [ ] LABELMAP에서 `SegmentNumber=0` + `PixelPaddingValue=0` background item이 허용되는지, BINARY/FRACTIONAL에서 `SegmentNumber=0`이 reject되는지 테스트한다.
- [ ] `LabelMapSegmentationStorage` SOP Class가 더 이상 `"LABELMAP SEG is outside the SEG MVP scope"`로 실패하지 않는지 검증한다.
- [ ] 기존 `SegmentationStorage` + `SegmentationType=LABELMAP` strict reject 정책을 테스트한다.
- [ ] `SOPClassUID`와 `MediaStorageSOPClassUID`가 `SegmentationStorage` / `LabelMapSegmentationStorage`로 서로 충돌하면 strict reject되는지 테스트한다.
- [ ] SOP Class UID가 충돌하는 dataset에서 `is_segmentation_storage()` / `is_labelmap_segmentation_storage()` bool helper가 `false`를 반환하는지 테스트한다.
- [ ] 기존 `is_segmentation_storage()`가 `LabelMapSegmentationStorage`를 포함하지 않고, 새 `is_labelmap_segmentation_storage()` / 필요 시 `is_any_segmentation_storage()`가 기대대로 동작하는지 검증한다. SOP Class UID 충돌 시 `is_any_segmentation_storage()`도 `false`를 반환해야 한다.
- [ ] LABELMAP에서는 frame별 `ReferencedSegmentNumber`가 없어도 metadata index가 만들어지는지 검증한다.
- [ ] LABELMAP에서 `referenced_segment_number()` / Python `frame.referenced_segment_number` 접근 시 명확한 compatibility error가 나는지 검증한다.
- [ ] LABELMAP에서 `repr(frame)` / `SegmentFrame.__repr__`가 `referenced_segment_number`를 호출해 터지지 않는지 검증한다.
- [ ] `to_array()`가 8-bit labelmap에서 `np.uint8`, 16-bit labelmap에서 `np.uint16`을 반환하는지 검증한다.
- [ ] LABELMAP `to_array()`, `decode_frame()`, `decode_frame_into()`, `decode_labelmap_frame_into()`도 unknown label membership validation을 수행하는지 테스트한다.
- [ ] LABELMAP `to_array()` / `decode_frame_into()` 성공 후 `present_segment_numbers()`가 재-decode 없이 decode API가 publish한 frame presence cache를 사용하는지 테스트한다.
- [ ] 16-bit LABELMAP `decode_frame()` bytes가 `to_array().tobytes()`와 일치하는지 검증한다.
- [ ] C++ `decode_labelmap_frame_bytes()` 16-bit bytes가 host/native-endian typed samples의 contiguous bytes와 일치하는지 테스트한다.
- [ ] `decode_frame_into()`가 dtype/itemsize mismatch를 명확히 거부하는지 검증한다.
- [ ] Python `decode_frame_into()`가 `np.int16`, signed format, non-native-endian `uint16`을 reject하는지 테스트한다.
- [ ] Python 8-bit LABELMAP `decode_frame_into()`가 `bytearray` / writable byte memoryview를 허용하는지 테스트한다.
- [ ] 8-bit LABELMAP에서 `decode_frame_into(uint8)`와 `decode_labelmap_frame_into(uint8)`가 같은 stored label values를 반환하는지 테스트한다.
- [ ] 16-bit LABELMAP을 uint8 output으로 decode하려 하거나 8-bit LABELMAP을 uint16 output으로 decode하려 하면 exact-match error가 나는지 테스트한다.
- [ ] `decode_frame_into()` / `decode_labelmap_frame_into()` error 시 output buffer가 partial write일 수 있다는 contract를 문서/API review로 확인한다.
- [ ] C++ 일반 `decode_frame_into(std::span<std::uint16_t>)` overload를 추가하지 않는 정책을 header/API review로 확인한다. 16-bit LABELMAP은 `decode_labelmap_frame_into()`를 사용한다.
- [ ] native PixelData length가 expected와 같거나 allowed padding byte 하나만 있는 경우를 허용하고, short/trailing non-zero extra data를 reject하는지 테스트한다.
- [ ] `present_segment_numbers()`가 BINARY/FRACTIONAL에서 기존 referenced segment number를 반환하는지 검증한다.
- [ ] BINARY/FRACTIONAL의 `present_segment_numbers()`가 PixelData를 scan하지 않고 declared `ReferencedSegmentNumber`를 반환한다는 edge case를 문서/테스트로 고정한다.
- [ ] BINARY/FRACTIONAL relaxed parse에서 `ReferencedSegmentNumber`가 없거나 0이면 `present_segment_numbers()`가 empty를 반환하고 `referenced_segment_number()`는 error를 던지는지 테스트한다.
- [ ] Python `present_segment_numbers()`가 `tuple[int, ...]`를 반환하는지 테스트한다.
- [ ] `present_segment_numbers()`가 LABELMAP에서 실제 등장한 label values를 반환하는지 검증한다.
- [ ] `present_segment_numbers()`로 받은 span/view가 존재하는 상태에서 `validate_label_values()`를 호출해도 ready cache entry가 replace되지 않아 dangling되지 않는지 테스트한다.
- [ ] `present_segment_numbers()` 반복 호출이 동일 frame을 매번 full decode하지 않고 lazy cache를 재사용하는지 행동/계측 테스트로 검증한다.
- [ ] per-frame persistent cache가 65536-entry table이 아니라 compact sorted vector/list로 노출되는지 검증한다. 순서는 label value 오름차순으로 고정한다.
- [ ] `frames_for_segment()` 최초 호출이 LABELMAP에서는 all-frame scan을 유발할 수 있음을 테스트하거나 benchmark-style guard로 확인한다.
- [ ] `frames_for_segment()` 성공 후 published index가 immutable이라 `SegmentFrameList.__len__` / `__getitem__` 반복 호출에서 재빌드나 invalidation이 없는지 테스트한다.
- [ ] Python `seg.frames_for_segment(segment_number)`는 호출 시점에 all-frame build와 error propagation이 일어나고, 반환된 list view의 `len()` / iteration은 build를 다시 유발하지 않는지 테스트한다.
- [ ] `frames_for_segment(unknown_segment)`와 `segment_frame_count(unknown_segment)`는 empty/0을 반환하고 LABELMAP PixelData scan을 유발하지 않는지 테스트한다.
- [ ] published all-frame index에서 missing key인 known-absent segment가 re-scan 없이 empty view / count 0을 반환하는지 테스트한다.
- [ ] concurrent lazy scan에서 같은 frame/all-frame scan이 중복될 수 있지만 ready/published storage는 한 번만 publish되고 늦게 끝난 결과가 버려지는지 stress/behavior test로 확인한다.
- [ ] `mask_for_segment()`가 BINARY/FRACTIONAL/LABELMAP 모두에서 uint8 0/1 mask를 반환하는지 검증한다.
- [ ] 8-bit LABELMAP의 metadata-only absent segment, 예: `SegmentNumber=1024`, 에 대해 `mask_for_segment(1024)`가 frame scan/unknown label validation을 수행한 뒤 zero mask를 반환하는지 테스트한다.
- [ ] LABELMAP `mask_for_segment()`가 frame decode 중 mask와 presence cache candidate를 한 pass에서 만들고, 이후 `present_segment_numbers()`가 재-decode하지 않는지 행동/계측 테스트로 검증한다.
- [ ] `decode_and_scan_labelmap_frame(...)` helper는 직접 cache/index를 publish하지 않고 caller만 publish한다는 점을 구현 체크리스트/API review로 확인한다.
- [ ] `SegmentMaskOptions::error_when_not_present_in_frame`은 known segment가 해당 frame에 없을 때만 적용되고, unknown segment 요청은 옵션과 무관하게 error가 나는지 테스트한다.
- [ ] FRACTIONAL `mask_for_segment()` threshold option 동작을 테스트로 고정한다. `fractional_threshold=0.0`은 `sample > 0`, 그 외 값은 `sample / MaximumFractionalValue >= threshold`, 범위 밖 threshold는 error다.
- [ ] `validate_label_values()` 성공 후 `present_segment_numbers()`와 `frames_for_segment()`가 재-decode 없이 채워진 cache/index를 사용하는지 테스트한다.
- [ ] `validate_label_values()`가 기존 ready cache entry나 published all-frame index를 replace하지 않는지 테스트한다.
- [ ] `validate_label_values()` 실패 시 validate 중 새로 scan한 partial cache/index가 publish되지 않고 다음 호출에서 재시도되는지 테스트한다. validate 시작 전에 이미 ready였던 cache entry는 유지되는지도 확인한다.
- [ ] BINARY/FRACTIONAL의 기존 `referenced_segment_number`, `frames_for_segment`, `decode_frame_into(uint8)` API가 깨지지 않았음을 호환성 테스트로 고정한다.
- [ ] compressed/encapsulated Label Map SEG가 MVP에서 명확한 unsupported error를 내는지 검증한다.
- [ ] LABELMAP PixelData missing, detached payload marker, unexpected PixelSequence 형태가 decode/validate에서 각각 명확한 error가 되는지 테스트한다.
- [ ] Big Endian Label Map SEG가 1차 MVP에서 명확한 unsupported error를 내는지 검증한다.
- [ ] `PhotometricInterpretation=MONOCHROME2`와 `PALETTE COLOR` allowlist, 그 외 값 reject를 테스트한다.
- [ ] `PixelPaddingValue`가 있는 LABELMAP은 background로 처리되고, `PixelPaddingRangeLimit`은 명확한 unsupported error가 나는지 테스트한다.
- [ ] 8-bit LABELMAP에서 `SegmentNumber > 255`가 metadata-only absent segment로 허용되고, 해당 segment mask가 zero mask가 되는지 테스트한다.
- [ ] `frames_for_segment()` / `segment_frame_count()`가 "segment가 present인 frame"이라는 공통 의미를 따르는지 검증한다.
- [ ] `plane_from_seg_frame()`이 Label Map SEG synthetic sample의 geometry를 반환하는지 검증한다.
- [ ] palette/color 관련 테스트는 core SEG adapter가 아니라 GUI/viewer test suite에서 다룬다.

### SEG PixelData compression / write / transcode 후속 계획

- [x] 범위는 read/decode뿐 아니라 기존 `DicomFile::set_transfer_syntax()` / `write_with_transfer_syntax()` / `write_bytes_with_transfer_syntax()` 경로를 통한 write/transcode까지 포함한다. SEG 전용 writer, SEG 전용 pre-serialization PixelData rewrite, 별도 fragment parser는 만들지 않는다.
- [x] write/transcode는 "SEG wrapper가 PixelData를 미리 변환한 뒤 generic writer에 맡기는" 방식이 아니라, generic pixel/write path가 SEG metadata와 1-bit storage를 표현할 수 있도록 보강하는 방향으로 진행한다.
- [x] generic `DicomFile` write/transcode path 안에 SEG policy preflight hook을 둔다. 위치는 transfer syntax write decision 직후와 encode policy 확정 직후로 고정하고, SOP Class / `SegmentationType` 기반 `classify`와 실제 PixelData write/transcode invariant validation을 분리한다.
- [x] public SEG API 의미는 native PixelData와 동일하게 유지한다. 압축 여부는 frame source/frame sink의 내부 구현 차이로 숨기고, `to_array()`, `decode_frame_into()`, `present_segment_numbers()`, `mask_for_segment()`, `validate_label_values()` contract는 바꾸지 않는다.
- [x] 지원 matrix를 read/decode, native->encapsulated write, encapsulated->native write, encapsulated->encapsulated transcode로 분리한다. FRACTIONAL 8-bit와 LABELMAP 8/16-bit는 lossless codec 기반 1차 지원 대상으로 두고, BINARY 1-bit는 core pixel layout/codec/write 모델 보강 후 지원한다.
- [ ] BINARY compressed SEG는 기존 `PixelPayloadDecoder`가 `BitsAllocated` 8/16/32만 받는 한 바로 지원하지 않는다. 구현 전 `BitsAllocated=1` decoded layout, unpacked bool/u8 view, native 1-bit repack, encapsulated frame encoding contract 중 어떤 계층에서 1-bit를 표현할지 먼저 확정한다.
- [ ] generic `pixel::PixelLayout` / `ConstPixelSpan` / encode source layout에 "source sample view는 u8 0/1일 수 있지만 DICOM stored BitsAllocated는 1"인 상태를 표현할 수 있는지 검토한다. 부족하면 SEG 전용 우회가 아니라 core pixel layout/encoder ABI를 확장한다.
- [x] generic write/transcode preflight가 active PixelData write/transcode에서 SEG metadata invariants를 보존하도록 검토한다. `SegmentationType`, `BitsAllocated`, `BitsStored`, `HighBit`, `SamplesPerPixel`, `PhotometricInterpretation`, `PixelRepresentation`, `MaximumFractionalValue`, `SegmentationFractionalType`, LABELMAP `PixelPaddingValue`, `PixelPaddingRangeLimit`, `SegmentsOverlap`이 타입별 contract를 벗어나면 실패시킨다. PixelData가 없는 `set_transfer_syntax()`와 완전 as-is write는 PixelData invariant validation을 수행하지 않는다.
- [x] 입력 decode는 `DecodeInfo::encoded_lossy_state` 또는 동등한 codec result 상태로 확인해 `lossy`, `near_lossless`, `unknown`을 cache publish 전에 reject한다. SEG public decode path의 FRACTIONAL/LABELMAP frame decode와 write/transcode streaming input decode path는 `DecodeInfo`를 확인해 lossless source만 허용한다. 실제 lossy-source fixture 회귀 테스트는 malformed/unsupported negative test 항목에 남긴다.
- [x] 출력 encode는 target transfer syntax/profile/options preflight에서 lossy/near-lossless 가능성을 먼저 reject하고, streaming write 도중 뒤늦게 output lossy를 발견하는 구조를 만들지 않는다.
- [x] Encapsulated Uncompressed는 압축은 아니지만 encapsulated frame source/sink 검증 대상으로 포함한다. FRACTIONAL, LABELMAP 8-bit, LABELMAP 16-bit SEG를 `write_bytes_with_transfer_syntax()`로 쓴 뒤 다시 SEG API로 읽는 C++/Python roundtrip regression을 추가했다.
- [x] `from_dicomfile()`은 압축 SEG에서도 metadata-only construction을 유지한다. codec availability, fragment integrity, decoded length, lossy state, unknown label validation은 frame decode/presence 계산 또는 `validate_label_values()` 시점에 수행한다.
- [x] PixelData source를 native direct span과 decoded scratch frame으로 분리한다. LABELMAP public decode/presence/mask/validate path는 native PixelData에서는 direct span을, encapsulated PixelData에서는 `DicomFile::decode_into()` scratch frame을 사용한다. BINARY 1-bit는 별도 core layout 후속으로 남긴다.
- [x] LABELMAP compressed decode는 `decode_and_scan_labelmap_frame(...)` 계열 공통 helper가 typed sample span을 받는 구조로 정리한다. native path는 direct span을 넘기고 encapsulated path는 per-frame scratch buffer를 넘긴다.
- [x] write/transcode 중 semantic validation 범위를 고정한다. LABELMAP public decode path와 write/transcode streaming frame scan은 decoded label membership, `PixelPaddingValue`가 `SegmentSequence`를 참조하는지, 16-bit host/native-endian label contract를 검증한다. FRACTIONAL write/transcode streaming frame scan은 sample이 `MaximumFractionalValue`를 초과하면 reject한다.
- [x] encapsulated PixelData는 기존 `PixelSequence`, `pixel_payload_decode_descriptor()`, `PixelPayloadDecoder`/decode dispatch 경로를 우선 재사용한다.
- [x] `NumberOfFrames`와 encapsulated frame count가 일치하지 않거나 frame을 특정할 수 없으면 decode/write 시 명확한 error가 나는지 negative regression으로 고정한다.
- [ ] Extended Offset Table/Extended Offset Table Lengths가 있는 파일은 기존 payload splitter/write path가 지원하는 범위 안에서만 허용한다. 부족한 부분은 generic PixelSequence/payload indexing 개선 항목으로 처리한다.
- [x] decoded output contract를 SEG type별로 고정한다. BINARY public decode/mask는 `uint8` 0/1, native storage/write는 1-bit packed; FRACTIONAL은 `uint8` sample; LABELMAP 8-bit는 `uint8` label, LABELMAP 16-bit는 host/native-endian `uint16_t` label이다.
- [ ] compressed-to-native BINARY transcode는 decoded u8 mask를 native PixelData에 그대로 쓰지 않고 generic 1-bit packer를 통해 다시 bit-packed PixelData로 써야 한다. 이 packer는 SEG 전용 함수가 아니라 core native 1-bit PixelData write helper로 둔다.
- [ ] native-to-encapsulated BINARY transcode는 encoder가 1-bit source layout을 받을 수 있어야 한다. encoder backend가 1-bit를 직접 받지 못하면 core encode layer에서 명시적으로 unsupported를 반환하고, metadata를 `BitsAllocated=8`로 바꾸는 fallback은 금지한다.
- [x] core 1-bit source layout/write 지원 전까지 BINARY SEG pixel transcode는 `write_with_transfer_syntax()`와 `set_transfer_syntax()` preflight에서 명확한 unsupported error로 막는다.
- [x] `present_segment_numbers(frame)`는 encapsulated LABELMAP에서 해당 frame 하나만 decode/scan한다. `frames_for_segment()`와 `validate_label_values()`만 all-frame decode를 유발한다. 비용과 error propagation 문서화는 별도 문서 항목으로 남긴다.
- [x] full decoded frame buffer는 기본적으로 persistent cache하지 않는다. compressed LABELMAP all-frame scan은 `frames_for_segment()` / `validate_label_values()` 내부 builder에서 decode plan 1개와 frame scratch 1개를 재사용하고, persistent cache는 기존처럼 presence list와 all-frame segment index까지만 유지한다. single-frame lazy decode는 caller별 임시 scratch를 사용한다.
- [ ] Python binding에서는 compressed SEG decode/presence scan/`validate_label_values()`/`frames_for_segment()` all-frame scan과 write/transcode 중 GIL을 release한다.
- [ ] output buffer contract는 native path와 동일하게 유지한다. `_into()` 계열에서 codec/decode/validation error가 나면 output buffer가 partial write 상태일 수 있음을 header/docstring에 명시한다.
- [x] write/transcode 후 validation test를 추가한다. Encapsulated Uncompressed와 RLE Lossless output을 다시 SEG로 열어 C++ `decode_frame_into`/`decode_labelmap_frame_into`, Python `to_array`, `present_segment_numbers`, `mask_for_segment`, `frames_for_segment`, `validate_label_values`를 실행하고 원본 frame별 mask/label value와 비교한다. C++ RLE regression은 RLE static plugin enabled build에서 조건부 실행한다. JPEG-LS/JPEG 2000 등 추가 lossless compressed regression은 별도 항목으로 남긴다.
- [x] write/transcode semantic negative test를 추가한다. FRACTIONAL sample이 `MaximumFractionalValue`를 초과하면 reject하고, LABELMAP native/encapsulated source의 decoded label이 `SegmentSequence`에 없으면 reject한다.
- [x] malformed PixelSequence 계열 negative test를 C++ smoke로 고정한다. `NumberOfFrames`와 encapsulated frame count mismatch, empty/missing fragments, zero-length fragment, decoded length mismatch를 `write_bytes_with_transfer_syntax()` 경로에서 검증한다.
- [x] lossy compressed source reject를 C++ smoke로 고정한다. JPEG builtin build에서 non-SEG로 만든 lossy JPEG source를 Label Map SEG로 재분류한 뒤 public decode와 write/transcode가 `lossless source` error를 내는지 검증한다.
- [x] decoder/encoder plugin 없음 reject를 C++ smoke로 고정한다. RLE static plugin disabled build에서 LABELMAP RLE write/set_transfer_syntax가 encoder binding error로 실패하는지 검증한다.
- [x] near-lossless compressed source reject를 C++ smoke로 고정한다. JPEG-LS builtin build에서 non-SEG로 만든 near-lossless JPEG-LS source를 Label Map SEG로 재분류한 뒤 public decode와 write/transcode가 `lossless source` error를 내는지 검증한다.
- [ ] unsupported transfer syntax와 unknown lossy-state reject를 C++ smoke와 Python 테스트로 고정한다.
- [ ] 실제 compressed FRACTIONAL, LABELMAP, 가능하면 BINARY SEG sample을 `../sample/seg` 기준 regression으로 돌린다. BINARY compressed는 core 1-bit layout/write 지원 전에는 명확한 unsupported/reject sample로 먼저 고정한다.
- [ ] 문서에는 native uncompressed, encapsulated uncompressed, lossless compressed/encapsulated, lossy compressed SEG의 read/write/transcode 지원 범위를 transfer syntax별 표로 정리한다.

### 문서 / Stub

- [ ] `docs/*/developer/segmentation.md`의 Post-MVP 항목에서 Label Map SEG를 supported scope로 이동한다.
- [ ] Python stub에서 `present_segment_numbers()`, `mask_for_segment()`, `validate_label_values()`, `SegmentMaskOptions.error_when_not_present_in_frame`, labelmap `to_array()` dtype contract, `decode_frame()` native typed bytes contract, `decode_frame_into()` buffer dtype/endian contract를 반영한다.
- [ ] C++ header 주석에서 BINARY/FRACTIONAL/LABELMAP 각각의 pixel contract를 분리해 설명한다.
- [ ] C++ header 주석에서 LABELMAP decode API도 unknown label membership validation을 수행하며, `decode_labelmap_frame_bytes()`는 host/native-endian typed sample bytes를 반환한다고 설명한다.
- [ ] C++ header 주석에서 `_into()` 계열 decode/mask API는 error 시 output buffer partial write가 가능하다고 설명한다.
- [ ] 문서에서 `referenced_segment_number`는 BINARY/FRACTIONAL compatibility accessor이고 LABELMAP에서는 error를 던진다고 설명한다. 새 공통 code는 `present_segment_numbers()` / `mask_for_segment()`를 우선 사용한다고 안내한다.
- [ ] 문서에서 concurrent LABELMAP lazy scan은 중복 수행될 수 있지만 ready cache와 published all-frame index는 한 번만 publish된다고 설명한다.
- [ ] 문서에서 BINARY/FRACTIONAL `present_segment_numbers()`는 PixelData가 아니라 declared `ReferencedSegmentNumber` 기반이고, LABELMAP `present_segment_numbers()`는 실제 decoded non-background label 기반이라는 의미 차이를 명확히 적는다.
- [ ] 문서에서 Big Endian Label Map SEG는 1차 MVP에서 unsupported라고 명시한다.
- [ ] 문서에서 `from_dicomfile()`은 LABELMAP PixelData 전체를 eager validation하지 않으며, 전체 label 검증이 필요하면 `validate_label_values()`를 호출하라고 설명한다.
- [ ] GUI/viewer 문서에는 palette rendering이 caller responsibility임을 남기고, DicomSDL core 문서에는 label value array contract만 적는다.

## Test Plan

- [x] axial synthetic plane에서 `index -> world -> index` roundtrip 검증
- [x] `make_image_plane_geometry()`가 invalid spacing, invalid size, non-orthogonal direction을 거부하는지 검증
- [x] `make_image_volume_geometry()`가 invalid spacing, invalid size, non-orthogonal direction을 거부하는지 검증
- [ ] public raw constructor 없이 validated factory로 테스트 geometry를 만들 수 있는지 검증
- [ ] core lookup test는 `tmp/element_path_dataset_lookup_checklist.ko.md`의 Test Plan에서 관리한다.
- [x] geometry code review: DICOM metadata lookup 구현이 dotted string path가 아니라 `ElementPath` helper를 사용하는지 확인
- [x] geometry code review: `VolumetricPropertiesInfo::source`와 missing geometry diagnostic이 `ElementPath` source를 보존하는지 확인
- [ ] geometry perf/code review: 많은 frame을 순회할 때 convenience factory 대신 `FrameGeometryReader`를 재사용하는 경로를 제공하는지 확인
- [ ] geometry perf/code review: `FrameGeometryReader` frame별 lookup이 root dataset에서 매번 full path traversal을 반복하지 않는지 확인
- [x] geometry code review: `ElementPath` debug string 변환이 core lookup / `FrameGeometryReader` 경로에서 호출되지 않는지 확인
- [x] `Matrix4x4d`가 row-major storage, column-vector left multiply convention을 따르는지 검증
- [x] matrix composition이 `target_index_h = target_world_to_index * source_index_to_world * source_index_h` 순서인지 검증
- [x] `contains_index()`가 sample-centered valid range `[-0.5, size - 0.5)` 기준으로 동작하는지 검증
- [x] `contains_world()`가 `index_from_world()` 결과를 기준으로 동작하는지 검증
- [x] `ImagePlaneGeometry::world_to_index_matrix()`의 세 번째 성분이 signed normal distance(mm)를 반환하는지 검증
- [ ] oblique orientation에서 direction normalization / cross product 검증
- [x] `ImageOrientationPatient`가 `direction_i = IOP[0..2]`, `direction_j = IOP[3..5]`로 들어가는지 검증
- [x] `ImageOrientationPatient` 첫 triplet이 DICOM row direction cosine이지만 DicomSDL `i=column` direction으로 쓰인다는 점을 `make_image_plane_geometry()` 테스트 이름/주석에 명시
- [x] DICOM `PixelSpacing` row/column 순서가 `spacing_j/spacing_i`로 올바르게 들어가는지 검증
- [x] `ImagePlaneGeometry::spacing()`이 `ImageSpacing2D`를 반환하고 `spacing_i()/spacing_j()`가 DICOM column/row spacing을 명확히 드러내는지 검증
- [x] classic single-frame image에서 `plane_from_single_frame_image()`가 geometry를 반환하는지 검증
- [x] enhanced multi-frame image에서 `plane_from_multiframe_image()`가 `PerFrameFunctionalGroupsSequence -> SharedFunctionalGroupsSequence -> root dataset` 순서로 geometry를 반환하는지 검증
- [x] MR frame은 `MR Image Frame Type Sequence (0018,9226)`의 `VolumetricProperties`를 root보다 우선하는지 검증
- [x] CT frame은 `CT Image Frame Type Sequence (0018,9329)`의 `VolumetricProperties`를 root보다 우선하는지 검증
- [x] PET frame은 `PET Frame Type Sequence (0018,9751)`의 `VolumetricProperties`를 root보다 우선하는지 검증
- [x] NM Image Storage를 `FrameGeometryReader::volumetric_properties()`에 넣으면 Enhanced Frame Type Sequence lookup을 시도하지 않고 `unsupported_frame_geometry`로 실패하는지 검증
- [x] NM `Frame Increment Pointer`와 `Slice Vector` 기반 reconstructed stack은 `VolumetricProperties` 경로가 아니라 NM-specific adapter로 분리되는지 검증
- [x] root `VolumetricProperties=MIXED`이고 frame-level value가 없으면 `mixed_volumetric_properties`로 실패하는지 검증
- [x] `VolumetricProperties`가 missing이면 `missing_volumetric_properties`로 실패하는지 검증
- [x] `VolumetricProperties`가 알 수 없는 값이면 `unknown_volumetric_properties`로 실패하는지 검증
- [x] `VolumetricProperties=VOLUME`일 때만 `ImageFrameGeometryKind::regular_plane` 후보가 되는지 검증
- [x] `VolumetricProperties=SAMPLED` frame은 overlay용 `plane_from_multiframe_image()`에서 항상 `sampled_frame_geometry`로 실패하는지 검증
- [x] `VolumetricProperties=DISTORTED` frame은 overlay용 `plane_from_multiframe_image()`에서 항상 `distorted_frame_geometry` 또는 `unsupported_frame_geometry`로 실패하는지 검증
- [x] `frame_geometry_from_multiframe_image()`는 sampled/distorted frame의 `ImageFrameGeometryKind`를 보존하는지 검증
- [ ] SEG frame geometry가 기존 sample의 `image_position_patient`, `image_orientation_patient`, `pixel_spacing` 값과 일치하는지 검증
- [x] SEG frame geometry는 기본 strict 모드에서 root dataset fallback을 사용하지 않는지 검증
- [x] root dataset에만 geometry tag가 있는 SEG는 `plane_from_seg_frame()`에서 실패하는지 검증
- [x] geometry tag 누락, invalid spacing, invalid orientation이 각각 다른 `GeometryBuildStatus`로 반환되는지 검증
- [x] `GeometryBuildResult<T>`가 `ok + value`, `failure + no value` invariant를 유지하는지 검증
- [x] `ImageSize2D/3D`가 unsigned 타입으로 노출되는지 검증
- [x] `SliceStackItem::target_k`가 unsigned 타입으로 노출되는지 검증
- [x] 같은 `FrameOfReferenceUID`면 `compatible`
- [x] 다른 `FrameOfReferenceUID`면 `different_frame_of_reference`
- [x] geometry tag 누락은 factory 단계에서 `missing_required_tag`, stack 단계에서 `missing_geometry` issue로 표현되는지 검증
- [x] spacing만 다르면 `different_spacing` 또는 `requires_resampling`으로 표현되는지 검증
- [ ] extent만 다르면 `different_extent` 또는 `requires_resampling`으로 표현되는지 검증
- [ ] plane normal 위치가 tolerance를 벗어나면 `out_of_plane`으로 표현되는지 검증
- [x] `OverlayCheckOptions::normal_distance_tolerance_mm`가 plane 간 위치 비교에 적용되는지 검증
- [x] 방향이 반대이면 `opposite_orientation`으로 표현되는지 검증
- [x] spacing/extent/orientation/out-of-plane 문제가 동시에 있을 때 `OverlayCompatibility` 대표 `status`가 정해진 우선순위를 따르는지 검증
- [x] `OverlayCheck`가 `can_transform`, `can_direct_overlay`, `requires_resampling`을 서로 구분해서 반환하는지 검증
- [x] plane-volume, volume-volume overlay compatibility overload가 plane-plane과 같은 frame-of-reference 정책을 쓰는지 검증
- [x] volume-plane overlay compatibility overload가 plane-plane과 같은 frame-of-reference 정책을 쓰는지 검증
- [x] plane-volume / volume-volume check가 `overlaps_extent`, `source_inside_target_extent`, `target_k_range`를 채우는지 검증
- [x] volume-plane check가 `overlaps_extent`, `source_inside_target_extent`를 채우는지 검증
- [x] plane의 `contains_world()`가 in-plane bounds뿐 아니라 normal distance tolerance도 적용하는지 검증
- [x] `check_overlay_compatibility()`가 repeated call에서 allocation을 만들지 않는지 검증
- [ ] `check_overlay_compatibility()`가 dataset access 없이 이미 생성된 geometry 값만 읽는지 code review checklist에 포함
- [x] typed transform 객체가 생성 후 per-point 변환에서 matrix multiply만 수행하는지 검증
- [x] typed transform 객체의 repeated point transform이 allocation을 만들지 않는지 검증
- [x] plane-to-plane, plane-to-volume, volume-to-plane, volume-to-volume transform overload가 같은 matrix composition 규칙을 쓰는지 검증
- [x] plane-to-plane은 `ImagePoint2D -> ImagePoint2D`, plane-to-volume은 `ImagePoint2D -> ImagePoint3D`, volume-to-plane은 `ImagePoint3D -> ImagePoint2D`, volume-to-volume은 `ImagePoint3D -> ImagePoint3D` 타입으로 컴파일 타임에 구분되는지 검증
- [x] volume-to-plane projection helper가 `PlaneProjection2D`로 2D index와 signed normal distance를 함께 반환하는지 검증
- [x] `SliceStackInput` 목록을 직접 넣으면 dataset/file 생명주기와 무관하게 slice stack analysis가 수행되는지 검증
- [x] classic multi-instance `DataSet*` convenience overload가 내부적으로 `SliceStackInput` 목록과 같은 결과를 내는지 검증
- [x] Enhanced multi-frame `DicomFile` + 명시적 frame index 목록 convenience overload가 frame index를 보존한 `SliceStackInput` 목록과 같은 결과를 내는지 검증
- [x] Enhanced multi-frame에서 `StackID`가 둘 이상이면 `analyze_image_frame_stacks()`가 group을 분리하는지 검증
- [x] `ImageFrameStackGroup`의 dimension 값이 `DimensionIndexDescriptor`와 함께 보존되어 echo/time/phase/position 축 의미를 구분할 수 있는지 검증
- [ ] `DimensionIndexSequence`가 없고 geometry fallback option이 꺼져 있으면 `missing_dimension_module`로 실패하는지 검증
- [x] `ImageFrameStackOptions::allow_geometry_grouping_fallback=true`인 경우에만 geometry-only grouping fallback을 허용하는지 검증
- [x] tiled multi-frame image가 MVP에서 `unsupported_tiled_image`로 실패하는지 검증
- [x] FrameContent 누락은 `ImageFrameStackAnalysis::issues()`에 source frame과 tag를 포함해 기록되는지 검증
- [x] dimension module/geometry parse failure는 `ImageFrameStackAnalysis::issues()`에 source frame과 tag를 포함해 기록되는지 검증
- [x] Enhanced multi-frame에서 `StackID`가 둘 이상이면 `plan_image_frame_stack(file)`이 `multiple_frame_stacks`로 실패하는지 검증
- [x] Enhanced multi-frame에서 temporal position / phase / non-spatial dimension이 섞이면 `plan_image_frame_stack(file)`이 `multiple_frame_stacks`로 실패하는지 검증
- [x] 여러 axial DICOM slice에서 uniform stack인 경우 `SliceStackPlan::volume_geometry()`가 값을 갖고 올바른 `slices`, `spacing_k`, `origin`을 반환하는지 검증
- [x] reversed input에서도 `SliceStackPlan`은 chosen normal position 오름차순, positive `spacing_k`, `target_k == 0` slice origin 규칙을 유지하는지 검증
- [x] 입력 slice 순서가 섞여 있어도 `placements()`가 normal 방향 position 기준으로 정렬되는지 검증
- [x] `SliceStackItem`을 이용해 `source_frame_for_k[item.target_k] = {item.source_index, item.frame_index}` mapping을 만들 수 있는지 검증
- [x] `SliceStackAnalysis::issues()` / `SliceStackPlan::issues()`가 inconsistent orientation, mixed FoR, duplicate slice position의 원인 source/frame index를 제공하는지 검증
- [x] normal projection spacing은 uniform이지만 slice origin에 in-plane drift가 있으면 `inconsistent_slice_origin` 또는 `non_rectilinear_stack`으로 실패하는지 검증
- [x] `SliceStackOptions::slice_position_tolerance_mm`가 duplicate slice position 판정에 적용되는지 검증
- [x] `SliceStackOptions::origin_residual_tolerance_mm`가 in-plane residual 판정에 적용되는지 검증
- [x] non-uniform slice stack에서 `analyze_slice_stack()`이 sorted slices와 gaps를 반환하는지 검증
- [x] non-uniform slice stack에서 `analyze_slice_stack()`이 uniform runs를 반환하는지 검증
- [x] non-uniform slice stack에서 `plan_slice_stack()`은 `non_uniform_spacing`을 반환하고 `volume_geometry()`는 비어 있으며, caller가 analysis를 이용해 policy를 정할 수 있는지 검증
- [x] 서로 다른 `FrameOfReferenceUID`가 섞이면 `mixed_frame_of_reference`
- [ ] rows/columns가 다르면 `inconsistent_rows_columns`
- [ ] orientation이 다르면 `inconsistent_orientation`
- [ ] pixel spacing이 다르면 `inconsistent_pixel_spacing`
- [x] slice 간격이 uniform하지 않으면 기본값으로 `non_uniform_spacing`
- [x] 같은 normal projection 위치가 중복되면 기본값으로 `duplicate_slice_position`
- [x] classic DataSet stack에서 geometry tag 누락은 throwing exception이 아니라 `SliceStackIssue::missing_geometry`로 반환되는지 검증
- [ ] geometry functional group 누락은 throwing exception이 아니라 `GeometryBuildStatus`로 반환되는지 검증
- [x] SEG frame geometry factory가 SEG high-level throwing accessor를 직접 사용하지 않고 missing metadata를 `GeometryBuildStatus`로 반환하는지 code review checklist에 포함
- [x] 여러 issue가 동시에 있을 때 `status()`가 정해진 fatal priority의 대표 issue를 반환하고 `issues()`는 원인을 모두 보존하는지 검증
- [x] Python binding smoke test 추가: validated factory, plane/world roundtrip, matrix access, overlay check, typed transform, invalid input `ValueError`

## Assumptions

- [ ] 이 문서는 구현 전 작업 메모이므로 `tmp/` 아래에 둔다.
- [ ] 정식 사용자 문서는 나중에 API가 안정된 뒤 `docs/*/developer`로 옮긴다.
- [x] 이번 범위에는 slice stack의 geometry planning을 포함한다.
- [x] 이번 범위에는 non-uniform slice stack의 geometry analysis를 포함한다.
- [x] slice stack planner의 중심 입력은 이미 만들어진 `SliceStackInput` 목록이며, `DataSet*` / `DicomFile` 기반 API는 convenience adapter로 둔다.
- [ ] Enhanced multi-frame 전체 파일을 하나의 volume으로 만드는 convenience API는 단일 stack으로 해석 가능한 경우에만 성공한다.
- [ ] Enhanced multi-frame에서 여러 stack, temporal position, phase가 섞인 경우는 먼저 `analyze_image_frame_stacks()`로 group을 나눈 뒤 group별로 plan한다.
- [x] 일반 enhanced image frame은 root dataset fallback을 허용하지만, SEG frame은 root fallback을 제공하지 않는다.
- [ ] SEG legacy root fallback은 MVP 범위에서 제외한다. 필요하면 raw dataset 기반 별도 API로 재검토한다.
- [x] `ImageVolumeGeometry`는 단일 affine과 단일 `spacing_k`로 표현 가능한 uniform grid만 나타낸다.
- [x] `ImageVolumeGeometry`를 만들려면 slice spacing뿐 아니라 slice origin의 in-plane residual도 tolerance 안에 있어야 한다.
- [x] non-uniform stack은 `ImageVolumeGeometry`로 정확히 표현하지 않고 `SliceStackAnalysis`로만 표현한다.
- [ ] dominant grid 선택, volume extent 확장, collision 해결, missing slice 채우기는 DicomSDL이 아니라 caller policy로 둔다.
- [ ] 이번 범위에는 pixel data를 decode해서 volume buffer를 만드는 기능은 포함하지 않는다.
- [ ] mask resampling, segmentation volume reconstruction, viewport coordinate mapping은 포함하지 않는다.
