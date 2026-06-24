from __future__ import annotations

import pytest

import dicomsdl as dicom


def _set(df: dicom.DicomFile, key: str, value: object) -> None:
    assert df.set_value(key, value) is True


def _plane(z: float = 30.0):
    g = dicom.geometry
    return g.make_image_plane_geometry(
        g.Point3d(10.0, 20.0, z),
        g.Vec3d(1.0, 0.0, 0.0),
        g.Vec3d(0.0, 1.0, 0.0),
        g.ImageSpacing2D(2.5, 1.5),
        g.ImageSize2D(256, 128),
    )


def _volume():
    g = dicom.geometry
    return g.make_image_volume_geometry(
        g.Point3d(10.0, 20.0, 30.0),
        g.Vec3d(1.0, 0.0, 0.0),
        g.Vec3d(0.0, 1.0, 0.0),
        g.Vec3d(0.0, 0.0, 1.0),
        g.ImageSpacing3D(2.5, 1.5, 4.0),
        g.ImageSize3D(256, 128, 32),
    )


def _make_enhanced_ct_stack(
    with_dimension_index: bool = True,
    with_frame_content: bool = True,
) -> dicom.DicomFile:
    df = dicom.DicomFile()
    _set(df, "Rows", 4)
    _set(df, "Columns", 5)
    _set(df, "NumberOfFrames", 3)
    _set(df, "SOPClassUID", dicom.uid_from_keyword("EnhancedCTImageStorage").value)
    _set(df, "FrameOfReferenceUID", "1.2.826.0.1.3680043.10.543.77")
    _set(
        df,
        "SharedFunctionalGroupsSequence.0.PixelMeasuresSequence.0.PixelSpacing",
        [2.0, 3.0],
    )
    _set(
        df,
        "SharedFunctionalGroupsSequence.0.PlaneOrientationSequence.0."
        "ImageOrientationPatient",
        [1.0, 0.0, 0.0, 0.0, 1.0, 0.0],
    )
    _set(
        df,
        "SharedFunctionalGroupsSequence.0.CTImageFrameTypeSequence.0."
        "VolumetricProperties",
        "VOLUME",
    )
    if with_dimension_index:
        _set(
            df,
            "DimensionIndexSequence.0.DimensionIndexPointer",
            dicom.Tag("InStackPositionNumber"),
        )
        _set(
            df,
            "DimensionIndexSequence.0.FunctionalGroupPointer",
            dicom.Tag("FrameContentSequence"),
        )
        _set(df, "DimensionIndexSequence.0.DimensionDescriptionLabel", "Stack position")
    for frame_index, z in enumerate((10.0, 20.0, 30.0)):
        base = f"PerFrameFunctionalGroupsSequence.{frame_index}."
        _set(df, base + "PlanePositionSequence.0.ImagePositionPatient", [0.0, 0.0, z])
        if with_frame_content:
            _set(df, base + "FrameContentSequence.0.StackID", "STACK_A")
            _set(df, base + "FrameContentSequence.0.InStackPositionNumber", frame_index + 1)
        if with_dimension_index and with_frame_content:
            _set(df, base + "FrameContentSequence.0.DimensionIndexValues", [frame_index + 1])
    return df


def test_geometry_plane_roundtrip_and_matrix_contract() -> None:
    g = dicom.geometry
    plane = _plane()

    world = plane.world_from_index(g.ImagePoint2D(2.0, 3.0))
    assert world.x == pytest.approx(15.0)
    assert world.y == pytest.approx(24.5)
    assert world.z == pytest.approx(30.0)

    index = plane.index_from_world(world)
    assert index.i == pytest.approx(2.0)
    assert index.j == pytest.approx(3.0)
    assert plane.normal_distance_from_world(g.Point3d(15.0, 24.5, 34.0)) == pytest.approx(4.0)
    assert plane.contains_world(world)

    matrix = plane.index_to_world_matrix
    assert matrix.at(0, 0) == pytest.approx(2.5)
    assert len(matrix.to_tuple()) == 16


def test_geometry_overlay_and_transform_bindings() -> None:
    g = dicom.geometry
    source = _plane(46.0)
    target = _volume()

    check = g.check_overlay_compatibility("1.2.3", source, "1.2.3", target)
    assert check.ok
    assert check.can_direct_overlay
    assert check.status is g.OverlayCompatibility.compatible
    assert check.target_k_range is not None
    assert check.target_k_range.begin == 4
    assert check.target_k_range.end == 5

    transform = g.make_plane_to_volume_transform(source, target)
    mapped = transform.target_index_from_source_index(g.ImagePoint2D(7.0, 11.0))
    assert mapped.i == pytest.approx(7.0)
    assert mapped.j == pytest.approx(11.0)
    assert mapped.k == pytest.approx(4.0)

    reverse = transform.source_index_from_target_index(mapped)
    assert reverse.i == pytest.approx(7.0)
    assert reverse.j == pytest.approx(11.0)

    plane = _plane(30.0)
    rotated = g.make_image_plane_geometry(
        g.Point3d(10.0, 20.0, 30.0),
        g.Vec3d(0.0, 1.0, 0.0),
        g.Vec3d(-1.0, 0.0, 0.0),
        g.ImageSpacing2D(2.5, 1.5),
        g.ImageSize2D(256, 128),
    )
    rotated_check = g.check_overlay_compatibility("1.2.3", plane, "1.2.3", rotated)
    assert rotated_check.ok
    assert not rotated_check.can_direct_overlay
    assert rotated_check.requires_resampling
    assert rotated_check.status is g.OverlayCompatibility.requires_resampling

    smaller_extent = g.make_image_plane_geometry(
        g.Point3d(10.0, 20.0, 30.0),
        g.Vec3d(1.0, 0.0, 0.0),
        g.Vec3d(0.0, 1.0, 0.0),
        g.ImageSpacing2D(2.5, 1.5),
        g.ImageSize2D(128, 64),
    )
    extent_check = g.check_overlay_compatibility("1.2.3", plane, "1.2.3", smaller_extent)
    assert extent_check.ok
    assert extent_check.overlaps_extent
    assert not extent_check.requires_resampling
    assert extent_check.status is g.OverlayCompatibility.different_extent

    strict_check = g.check_overlay_compatibility(
        "1.2.3",
        plane,
        "1.2.3",
        g.make_image_plane_geometry(
            g.Point3d(10.0, 20.0, 30.0),
            g.Vec3d(1.0, 0.0, 0.0),
            g.Vec3d(0.0, 1.0, 0.0),
            g.ImageSpacing2D(3.0, 1.5),
            g.ImageSize2D(256, 128),
        ),
        g.OverlayCheckOptions(require_same_grid=True),
    )
    assert not strict_check.ok
    assert not strict_check.can_transform
    assert strict_check.requires_resampling
    assert strict_check.status is g.OverlayCompatibility.different_spacing


def test_geometry_factories_raise_value_error_on_invalid_input() -> None:
    g = dicom.geometry
    with pytest.raises(ValueError, match="invalid_spacing"):
        g.make_image_plane_geometry(
            g.Point3d(0.0, 0.0, 0.0),
            g.Vec3d(1.0, 0.0, 0.0),
            g.Vec3d(0.0, 1.0, 0.0),
            g.ImageSpacing2D(0.0, 1.0),
            g.ImageSize2D(32, 32),
        )


def test_geometry_slice_stack_analysis_and_plan_bindings() -> None:
    g = dicom.geometry
    inputs = [
        g.SliceStackInput(_plane(20.0), "1.2.3", source_index=2, frame_index=0),
        g.SliceStackInput(_plane(0.0), "1.2.3", source_index=0, frame_index=0),
        g.SliceStackInput(_plane(10.0), "1.2.3", source_index=1, frame_index=0),
    ]

    analysis = g.analyze_slice_stack(inputs)
    assert analysis.ok
    assert analysis.status is g.SliceStackStatus.ok
    assert analysis.frame_of_reference_uid == "1.2.3"
    assert analysis.uniform_spacing_k == pytest.approx(10.0)
    assert [s.source_index for s in analysis.slices] == [0, 1, 2]
    assert [gap.spacing_mm for gap in analysis.gaps] == pytest.approx([10.0, 10.0])

    plan = g.plan_slice_stack(inputs)
    assert plan.ok
    assert plan.volume_geometry is not None
    assert plan.volume_geometry.slices == 3
    assert plan.volume_geometry.spacing_k == pytest.approx(10.0)
    assert [(item.source_index, item.target_k) for item in plan.placements] == [
        (0, 0),
        (1, 1),
        (2, 2),
    ]

    mixed = g.analyze_slice_stack(
        [
            g.SliceStackInput(_plane(0.0), "1.2.3"),
            g.SliceStackInput(_plane(10.0), "1.2.4"),
        ]
    )
    assert not mixed.ok
    assert mixed.status is g.SliceStackStatus.mixed_frame_of_reference
    assert mixed.issues

    close_positions = [
        g.SliceStackInput(_plane(0.0), "1.2.3"),
        g.SliceStackInput(_plane(0.0005), "1.2.3"),
    ]
    duplicate = g.analyze_slice_stack(close_positions)
    assert not duplicate.ok
    assert duplicate.status is g.SliceStackStatus.duplicate_slice_position

    allowed_duplicate_plan = g.plan_slice_stack(
        close_positions,
        g.SliceStackOptions(allow_duplicate_positions=True),
    )
    assert not allowed_duplicate_plan.ok
    assert allowed_duplicate_plan.status is g.SliceStackStatus.non_uniform_spacing
    assert allowed_duplicate_plan.issues
    assert allowed_duplicate_plan.issues[0].tag == dicom.Tag("ImagePositionPatient")

    tight = g.analyze_slice_stack(
        close_positions,
        g.SliceStackOptions(slice_position_tolerance_mm=1e-4),
    )
    assert tight.ok
    assert tight.uniform_spacing_k == pytest.approx(0.0005)

    with pytest.raises(TypeError):
        g.SliceStackOptions(None, 1e-4, 0.2, True)


def test_geometry_enhanced_image_frame_stack_bindings() -> None:
    g = dicom.geometry
    df = _make_enhanced_ct_stack()

    stacks = g.analyze_image_frame_stacks(df)
    assert stacks.ok
    assert len(stacks.groups) == 1
    group = stacks.groups[0]
    assert group.key.stack_id == "STACK_A"
    assert group.frame_indices == [0, 1, 2]
    assert group.analysis.ok

    analysis = g.analyze_image_frame_stack(df)
    assert analysis.ok
    assert analysis.uniform_spacing_k == pytest.approx(10.0)

    plan = g.plan_image_frame_stack(df)
    assert plan.ok
    assert plan.volume_geometry is not None
    assert plan.volume_geometry.rows == 4
    assert plan.volume_geometry.columns == 5
    assert plan.volume_geometry.slices == 3
    assert plan.volume_geometry.spacing_i == pytest.approx(3.0)
    assert plan.volume_geometry.spacing_j == pytest.approx(2.0)
    assert plan.volume_geometry.spacing_k == pytest.approx(10.0)
    assert [(item.frame_index, item.target_k) for item in plan.placements] == [
        (0, 0),
        (1, 1),
        (2, 2),
    ]

    subset = g.plan_image_frame_stack(df, [2, 0, 1])
    assert subset.ok
    assert [(item.frame_index, item.target_k) for item in subset.placements] == [
        (0, 0),
        (1, 1),
        (2, 2),
    ]

    missing_dimension = _make_enhanced_ct_stack(with_dimension_index=False)
    missing = g.analyze_image_frame_stacks(missing_dimension)
    assert not missing.ok
    assert missing.status is g.SliceStackStatus.missing_dimension_module
    missing_dimension = _make_enhanced_ct_stack(
        with_dimension_index=False,
        with_frame_content=False,
    )
    fallback = g.analyze_image_frame_stacks(
        missing_dimension,
        g.ImageFrameStackOptions(allow_geometry_grouping_fallback=True),
    )
    assert fallback.ok
    assert len(fallback.groups) == 1

    tiled = _make_enhanced_ct_stack()
    _set(tiled, "DimensionOrganizationType", "TILED_FULL")
    tiled_analysis = g.analyze_image_frame_stacks(tiled)
    assert not tiled_analysis.ok
    assert tiled_analysis.status is g.SliceStackStatus.unsupported_tiled_image
