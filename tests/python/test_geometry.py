from __future__ import annotations

import pytest

import dicomsdl as dicom


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
