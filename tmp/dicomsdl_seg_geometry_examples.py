"""DicomSDL SEG / geometry examples for the current development branch.

These snippets are written as small functions so they can be copied into
IPython, notebooks, or project scripts. They assume the branch build exposes:

- dicom.seg.read_file/read_bytes
- dicom.geometry plane, stack, overlay, enhanced, and NM helpers
"""

from __future__ import annotations

from pathlib import Path
from typing import Iterable, Sequence

import numpy as np

import dicomsdl as dicom


def print_seg_summary(seg_path: str | Path) -> dicom.seg.Segmentation:
    """Open a DICOM SEG file and print segment/frame metadata."""
    seg = dicom.seg.read_file(seg_path)

    print("valid:", seg.is_valid)
    print("segmentation_type:", seg.segmentation_type)
    print("fractional_type:", seg.fractional_type)
    print("maximum_fractional_value:", seg.maximum_fractional_value)
    print("frame_of_reference_uid:", seg.frame_of_reference_uid)
    print("rows/columns:", seg.rows, seg.columns)
    print("segments/frames:", seg.segment_count, seg.frame_count)

    print("\nSegments")
    for segment in seg.segments:
        print(
            segment.number,
            segment.label,
            segment.algorithm_type,
            segment.algorithm_name,
            segment.property_category,
            segment.property_type,
        )

    print("\nFirst frames")
    for frame in list(seg.frames)[:5]:
        print(
            "frame",
            frame.index,
            "segment",
            frame.referenced_segment_number,
            "position",
            frame.image_position_patient,
            "spacing",
            frame.pixel_spacing,
        )

    return seg


def decode_segment_masks(
    seg_path: str | Path,
    segment_number: int,
) -> list[tuple[int, np.ndarray]]:
    """Decode all frames belonging to one segment as uint8 masks."""
    seg = dicom.seg.read_file(seg_path)
    masks: list[tuple[int, np.ndarray]] = []

    for frame in seg.frames_for_segment(segment_number):
        mask = frame.to_array()
        masks.append((frame.index, mask))

    return masks


def fractional_frame_as_float(
    seg_path: str | Path,
    frame_index: int = 0,
) -> np.ndarray:
    """Decode a FRACTIONAL SEG frame and scale raw uint8 values to 0..1."""
    seg = dicom.seg.read_file(seg_path)
    if seg.segmentation_type is not dicom.seg.SegmentationType.fractional:
        raise ValueError("not a FRACTIONAL segmentation")
    if not seg.maximum_fractional_value:
        raise ValueError("MaximumFractionalValue is missing")

    raw = seg.to_array(frame_index)
    return raw.astype(np.float32) / float(seg.maximum_fractional_value)


def plan_classic_slice_stack(
    image_paths: Sequence[str | Path],
) -> dicom.geometry.SliceStackPlan:
    """Read classic single-frame CT/MR/PET slices and plan a volume stack."""
    files = [dicom.read_file(path) for path in image_paths]
    plan = dicom.geometry.plan_slice_stack(files)

    if not plan.ok:
        print("slice stack planning failed:", plan.status)
        for issue in plan.issues:
            print(issue.status, issue.source_index, issue.tag, issue.message)
        return plan

    volume = plan.volume_geometry
    assert volume is not None
    print("volume size:", volume.columns, volume.rows, volume.slices)
    print("spacing:", volume.spacing_i, volume.spacing_j, volume.spacing_k)
    print("origin:", volume.origin)

    for item in plan.placements[:5]:
        print(
            "decode source",
            item.source_index,
            "frame",
            item.frame_index,
            "-> target k",
            item.target_k,
        )

    return plan


def check_seg_frame_against_image_stack(
    seg_path: str | Path,
    image_paths: Sequence[str | Path],
    seg_frame_index: int = 0,
) -> dicom.geometry.OverlayCheck:
    """Check whether one SEG frame can be overlaid on a target image volume."""
    g = dicom.geometry
    seg = dicom.seg.read_file(seg_path)
    image_files = [dicom.read_file(path) for path in image_paths]

    image_plan = g.plan_slice_stack(image_files)
    if not image_plan.ok or image_plan.volume_geometry is None:
        raise ValueError(f"image stack is not a uniform volume: {image_plan.status}")

    seg_plane = g.plane_from_seg_frame(seg, seg_frame_index)
    target_volume = image_plan.volume_geometry

    check = g.check_overlay_compatibility(
        seg.frame_of_reference_uid,
        seg_plane,
        image_plan.frame_of_reference_uid,
        target_volume,
    )

    print("status:", check.status)
    print("can_transform:", check.can_transform)
    print("can_direct_overlay:", check.can_direct_overlay)
    print("requires_resampling:", check.requires_resampling)
    print("overlaps_extent:", check.overlaps_extent)
    print("target_k_range:", check.target_k_range)

    if check.can_transform:
        transform = g.make_plane_to_volume_transform(seg_plane, target_volume)
        center = g.ImagePoint2D(seg.columns / 2.0, seg.rows / 2.0)
        print("SEG center maps to target volume index:", transform.target_index_from_source_index(center))

    return check


def analyze_enhanced_multiframe(path: str | Path) -> dicom.geometry.ImageFrameStackAnalysis:
    """Group enhanced CT/MR/PET frames into spatial stacks and plan each group."""
    g = dicom.geometry
    file = dicom.read_file(path)
    stacks = g.analyze_image_frame_stacks(file)

    print("status:", stacks.status)
    print("groups:", len(stacks.groups))
    for group_index, group in enumerate(stacks.groups):
        print("group", group_index, "frames", group.frame_indices)
        print("stack_id:", group.key.stack_id)
        print("analysis:", group.analysis.status)
        if group.analysis.ok:
            plan = g.plan_image_frame_stack(file, group.frame_indices)
            print("plan:", plan.status, "placements:", len(plan.placements))

    for issue in stacks.issues:
        print("issue:", issue.status, issue.frame_index, issue.tag, issue.message)

    return stacks


def plan_nm_reconstructed_stack(path: str | Path) -> dicom.geometry.SliceStackPlan:
    """Plan an NM RECON TOMO / RECON GATED TOMO stack using SliceVector."""
    g = dicom.geometry
    file = dicom.read_file(path)
    plan = g.plan_nm_frame_stack(file)

    if not plan.ok:
        print("NM stack planning failed:", plan.status)
        for issue in plan.issues:
            print(issue.status, issue.frame_index, issue.tag, issue.message)
        return plan

    volume = plan.volume_geometry
    assert volume is not None
    print("NM volume size:", volume.columns, volume.rows, volume.slices)
    print("spacing:", volume.spacing_i, volume.spacing_j, volume.spacing_k)
    print("placements:", [(item.frame_index, item.target_k) for item in plan.placements[:10]])
    return plan


def synthetic_nm_example() -> dicom.geometry.SliceStackPlan:
    """Create a tiny synthetic NM RECON TOMO object and run the NM adapter."""
    df = dicom.DicomFile()
    df.set_value("Rows", 4)
    df.set_value("Columns", 5)
    df.set_value("NumberOfFrames", 3)
    df.set_value("SOPClassUID", dicom.uid_from_keyword("NuclearMedicineImageStorage").value)
    df.set_value("FrameOfReferenceUID", "1.2.826.0.1.3680043.10.543.88")
    df.set_value("ImageType", "ORIGINAL\\PRIMARY\\RECON TOMO")
    df.set_value("ImagePositionPatient", [0.0, 0.0, 100.0])
    df.set_value("ImageOrientationPatient", [1.0, 0.0, 0.0, 0.0, 1.0, 0.0])
    df.set_value("PixelSpacing", [2.0, 3.0])
    df.set_value("SpacingBetweenSlices", 5.0)
    df.set_value("FrameIncrementPointer", [dicom.Tag("SliceVector")])
    df.set_value("SliceVector", [3, 1, 2])

    plan = dicom.geometry.plan_nm_frame_stack(df)
    print("ok:", plan.ok)
    print("placements:", [(item.frame_index, item.target_k) for item in plan.placements])
    return plan


def synthetic_overlay_example() -> dicom.geometry.OverlayCheck:
    """Build geometry objects manually and preflight an overlay transform."""
    g = dicom.geometry

    seg_plane = g.make_image_plane_geometry(
        g.Point3d(0.0, 0.0, 20.0),
        g.Vec3d(1.0, 0.0, 0.0),
        g.Vec3d(0.0, 1.0, 0.0),
        g.ImageSpacing2D(1.0, 1.0),
        g.ImageSize2D(64, 64),
    )
    image_volume = g.make_image_volume_geometry(
        g.Point3d(0.0, 0.0, 0.0),
        g.Vec3d(1.0, 0.0, 0.0),
        g.Vec3d(0.0, 1.0, 0.0),
        g.Vec3d(0.0, 0.0, 1.0),
        g.ImageSpacing3D(1.0, 1.0, 5.0),
        g.ImageSize3D(64, 64, 16),
    )

    check = g.check_overlay_compatibility("1.2.3", seg_plane, "1.2.3", image_volume)
    print("status:", check.status)
    print("target_k_range:", check.target_k_range)

    transform = g.make_plane_to_volume_transform(seg_plane, image_volume)
    print(
        "seg pixel (10, 20) -> volume index",
        transform.target_index_from_source_index(g.ImagePoint2D(10.0, 20.0)),
    )
    return check


def read_many(files: Iterable[str | Path]) -> list[dicom.DicomFile]:
    """Small convenience helper used by the examples above."""
    return [dicom.read_file(file) for file in files]
