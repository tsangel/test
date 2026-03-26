from __future__ import annotations

"""Common series-to-volume helpers shared by the Python bridge modules.

The bridge keeps the stored-value dtype whenever possible. In particular:

- no rescale transform -> keep the raw stored dtype
- integer-only modality rescale with slope ~= 1 -> keep an integer dtype
- fractional rescale -> promote to float32 via ``apply_rescale_frames``

This avoids unnecessary float promotion for common CT/MR inputs while still
matching modality-value semantics for PET and other fractional-rescale cases.

Single-file 2D images are represented as single-slice arrays so they map cleanly
to SimpleITK and VTK reference readers:

- grayscale 2D -> ``(1, y, x)``
- vector 2D (for example RGB) -> ``(1, y, x, c)``

Single-file multiframe grayscale volumes are represented as ``(z, y, x)`` when
the frames form a stack-like volume.

For stack-like 3D inputs, the bridge canonicalizes slice order into physical
stack order:

- slices/frames are ordered by projection onto the base slice normal
- the first output slice has the smallest projected position
- ``spacing[2]`` is kept positive
- ``direction`` carries the stack axis instead of encoding it via negative
  spacing
"""

from dataclasses import dataclass
from pathlib import Path
from typing import Any

import numpy as np

from . import Tag, apply_rescale_frames, read_file

_FLOAT_TOL = 1e-6
_ORIENTATION_TOL = 1e-4
_OFF_AXIS_TOL = 1e-2
_METADATA_LOAD_UNTIL = Tag("0028,1053")


@dataclass(slots=True)
class _SliceEntry:
    path: Path
    dicom_file: Any
    position: tuple[float, float, float] | None
    instance_number: int | None
    sort_position: float | None
    slope: float
    intercept: float
    axis_x: np.ndarray
    axis_y: np.ndarray
    axis_z: np.ndarray


@dataclass(slots=True)
class SeriesVolume:
    series_dir: Path
    source_paths: tuple[Path, ...]
    array: np.ndarray
    spacing: tuple[float, float, float]
    origin: tuple[float, float, float]
    direction: tuple[float, ...]
    modality: str | None
    series_description: str | None
    patient_name: str | None
    units: str | None
    study_id: str | None
    transfer_syntax_uid: str | None
    rescale_applied: bool
    unique_slope_count: int


def _optional_text(value: Any) -> str | None:
    if value is None:
        return None
    text = str(value).strip()
    if not text:
        return None
    return text


def _dataset_path_value(dicom_file: Any, path: str) -> Any:
    try:
        return dicom_file.get_value(path, default=None)
    except Exception:
        return None


def _functional_group_value(
    dicom_file: Any,
    sequence_keywords: tuple[str, ...],
    value_keyword: str,
    *,
    frame_index: int | None = None,
) -> Any:
    if frame_index is not None:
        for sequence_keyword in sequence_keywords:
            value = _dataset_path_value(
                dicom_file,
                f"PerFrameFunctionalGroupsSequence.{frame_index}.{sequence_keyword}.0.{value_keyword}",
            )
            if value is not None:
                return value

    for sequence_keyword in sequence_keywords:
        value = _dataset_path_value(
            dicom_file,
            f"SharedFunctionalGroupsSequence.0.{sequence_keyword}.0.{value_keyword}",
        )
        if value is not None:
            return value

    return getattr(dicom_file, value_keyword, None)


def _series_units(first_file: Any) -> str | None:
    units = _optional_text(getattr(first_file, "Units", None))
    if units is not None:
        return units
    modality = _optional_text(getattr(first_file, "Modality", None))
    if modality != "PT":
        return None
    first_file.ensure_loaded("Units")
    return _optional_text(getattr(first_file, "Units", None))


def _float_tuple(value: Any, expected_len: int) -> tuple[float, ...] | None:
    if value is None:
        return None
    try:
        items = tuple(float(item) for item in value)
    except TypeError:
        return None
    if len(items) != expected_len:
        return None
    return items


def _int_value(value: Any) -> int | None:
    if value is None:
        return None
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def _is_close(value: float, expected: float) -> bool:
    return abs(value - expected) <= _FLOAT_TOL


def _is_integer_like(value: float) -> bool:
    return abs(value - round(value)) <= _FLOAT_TOL


def _normalize(vec: np.ndarray, fallback: np.ndarray) -> np.ndarray:
    norm = float(np.linalg.norm(vec))
    if norm <= 0.0:
        return fallback.copy()
    return vec / norm


def _frame_orientation(
    dicom_file: Any,
    frame_index: int | None = None,
) -> tuple[float, ...] | None:
    return _float_tuple(
        _functional_group_value(
            dicom_file,
            ("PlaneOrientationSequence", "PlaneOrientationVolumeSequence"),
            "ImageOrientationPatient",
            frame_index=frame_index,
        ),
        6,
    )


def _frame_position(
    dicom_file: Any,
    frame_index: int | None = None,
) -> tuple[float, ...] | None:
    return _float_tuple(
        _functional_group_value(
            dicom_file,
            ("PlanePositionSequence", "PlanePositionVolumeSequence"),
            "ImagePositionPatient",
            frame_index=frame_index,
        ),
        3,
    )


def _orientation_axes(first_file: Any, frame_index: int | None = None) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    orientation = _frame_orientation(first_file, frame_index)
    if orientation is None:
        axis_x = np.array((1.0, 0.0, 0.0), dtype=np.float64)
        axis_y = np.array((0.0, 1.0, 0.0), dtype=np.float64)
    else:
        axis_x = _normalize(
            np.asarray(orientation[:3], dtype=np.float64),
            np.array((1.0, 0.0, 0.0), dtype=np.float64),
        )
        axis_y = _normalize(
            np.asarray(orientation[3:], dtype=np.float64),
            np.array((0.0, 1.0, 0.0), dtype=np.float64),
        )
    axis_z = _normalize(
        np.cross(axis_x, axis_y),
        np.array((0.0, 0.0, 1.0), dtype=np.float64),
    )
    return axis_x, axis_y, axis_z


def _orientation_distance(
    axis_x: np.ndarray,
    axis_y: np.ndarray,
    reference_x: np.ndarray,
    reference_y: np.ndarray,
) -> float:
    return max(
        float(np.max(np.abs(axis_x - reference_x))),
        float(np.max(np.abs(axis_y - reference_y))),
    )


def _dominant_orientation_group(series_entries: list[_SliceEntry]) -> list[_SliceEntry]:
    if not series_entries:
        return []

    groups: list[list[_SliceEntry]] = []
    for entry in series_entries:
        for group in groups:
            anchor = group[0]
            if (
                _orientation_distance(entry.axis_x, entry.axis_y, anchor.axis_x, anchor.axis_y)
                <= _ORIENTATION_TOL
            ):
                group.append(entry)
                break
        else:
            groups.append([entry])

    return max(groups, key=len)


def _projected_distance(
    position: tuple[float, float, float],
    base_origin: np.ndarray,
    axis_z: np.ndarray,
) -> float:
    delta = np.asarray(position, dtype=np.float64) - base_origin
    return float(np.dot(delta, axis_z))


def _off_axis_distance(
    position: tuple[float, float, float],
    base_origin: np.ndarray,
    axis_z: np.ndarray,
) -> float:
    delta = np.asarray(position, dtype=np.float64) - base_origin
    distance = float(np.dot(delta, axis_z))
    return float(np.linalg.norm(delta - distance * axis_z))


def _spacing_z_from_sorted_positions(sort_positions: list[float]) -> float | None:
    if len(sort_positions) < 2:
        return None

    diffs = [
        float(sort_positions[index + 1] - sort_positions[index])
        for index in range(len(sort_positions) - 1)
    ]
    positive_diffs = [diff for diff in diffs if diff > _FLOAT_TOL]
    if not positive_diffs:
        return None
    return float(np.median(np.asarray(positive_diffs, dtype=np.float64)))


def _spacing(series_entries: list[_SliceEntry], first_file: Any) -> tuple[float, float, float]:
    pixel_spacing = _float_tuple(
        _functional_group_value(
            first_file,
            ("PixelMeasuresSequence",),
            "PixelSpacing",
            frame_index=0,
        ),
        2,
    )
    if pixel_spacing is None:
        spacing_x = 1.0
        spacing_y = 1.0
    else:
        # DICOM PixelSpacing is [row_spacing, column_spacing].
        spacing_y = float(pixel_spacing[0])
        spacing_x = float(pixel_spacing[1])

    spacing_z = None
    sortable = [entry.sort_position for entry in series_entries if entry.sort_position is not None]
    if len(sortable) >= 2:
        spacing_z = _spacing_z_from_sorted_positions(sortable)

    if spacing_z is None:
        spacing_between = _functional_group_value(
            first_file,
            ("PixelMeasuresSequence",),
            "SpacingBetweenSlices",
            frame_index=0,
        )
        if spacing_between is not None:
            spacing_z = float(spacing_between)
    if spacing_z is None:
        slice_thickness = _functional_group_value(
            first_file,
            ("PixelMeasuresSequence",),
            "SliceThickness",
            frame_index=0,
        )
        if slice_thickness is not None:
            spacing_z = float(slice_thickness)
    if spacing_z is None:
        spacing_z = 1.0

    return (spacing_x, spacing_y, float(spacing_z))


def _single_slice_spacing(first_file: Any) -> tuple[float, float, float]:
    pixel_spacing = _float_tuple(
        _functional_group_value(
            first_file,
            ("PixelMeasuresSequence",),
            "PixelSpacing",
            frame_index=0,
        ),
        2,
    )
    if pixel_spacing is None:
        spacing_x = 1.0
        spacing_y = 1.0
    else:
        spacing_y = float(pixel_spacing[0])
        spacing_x = float(pixel_spacing[1])

    spacing_between = _functional_group_value(
        first_file,
        ("PixelMeasuresSequence",),
        "SpacingBetweenSlices",
        frame_index=0,
    )
    spacing_z = float(spacing_between) if spacing_between is not None else 1.0
    return (spacing_x, spacing_y, spacing_z)


def _direction_tuple(axis_x: np.ndarray, axis_y: np.ndarray, axis_z: np.ndarray) -> tuple[float, ...]:
    return (
        float(axis_x[0]),
        float(axis_y[0]),
        float(axis_z[0]),
        float(axis_x[1]),
        float(axis_y[1]),
        float(axis_z[1]),
        float(axis_x[2]),
        float(axis_y[2]),
        float(axis_z[2]),
    )


def _normalize_series_dir(path: str | Path) -> Path:
    series_path = Path(path).expanduser().resolve()
    if not series_path.exists():
        raise FileNotFoundError(f"Path does not exist: {series_path}")
    if series_path.is_file():
        series_path = series_path.parent
    if not series_path.is_dir():
        raise NotADirectoryError(f"Path is not a directory: {series_path}")
    return series_path


def _series_instance_uid(dicom_file: Any) -> str | None:
    return _optional_text(getattr(dicom_file, "SeriesInstanceUID", None))


def _single_series_directory_paths(source_path: Path, candidate_paths: tuple[Path, ...]) -> tuple[Path, ...]:
    if len(candidate_paths) <= 1:
        return candidate_paths

    series_groups: dict[str | None, list[Path]] = {}
    for candidate_path in candidate_paths:
        dicom_file = _read_bridge_file(candidate_path)
        series_groups.setdefault(_series_instance_uid(dicom_file), []).append(candidate_path)

    if len(series_groups) <= 1:
        return candidate_paths

    details = ", ".join(
        f"{uid if uid is not None else '<missing>'} ({len(paths)} files)"
        for uid, paths in sorted(
            series_groups.items(),
            key=lambda item: (
                item[0] is None,
                item[0] if item[0] is not None else "",
            ),
        )
    )
    raise ValueError(
        "Directory input must contain exactly one SeriesInstanceUID "
        f"(found {len(series_groups)} under {source_path}: {details})"
    )


def _discover_dicom_paths(source_path: Path, candidate_paths: tuple[Path, ...]) -> tuple[Path, ...]:
    dicom_paths: list[Path] = []
    for candidate_path in candidate_paths:
        try:
            _read_bridge_file(candidate_path)
        except Exception:
            continue
        dicom_paths.append(candidate_path)
    return tuple(dicom_paths)


def _resolve_source_paths(path: str | Path) -> tuple[Path, tuple[Path, ...]]:
    source_path = Path(path).expanduser().resolve()
    if not source_path.exists():
        raise FileNotFoundError(f"Path does not exist: {source_path}")
    if source_path.is_file():
        return (source_path.parent, (source_path,))
    if not source_path.is_dir():
        raise NotADirectoryError(f"Path is not a directory: {source_path}")

    dcm_files = tuple(
        sorted(
            child
            for child in source_path.iterdir()
            if child.is_file() and child.suffix.lower() == ".dcm"
        )
    )
    if dcm_files:
        return (source_path, _single_series_directory_paths(source_path, dcm_files))

    all_files = tuple(sorted(child for child in source_path.iterdir() if child.is_file()))
    dicom_files = _discover_dicom_paths(source_path, all_files)
    if dicom_files:
        return (source_path, _single_series_directory_paths(source_path, dicom_files))

    raise FileNotFoundError(f"No DICOM files found under: {source_path}")


def _smallest_integer_dtype(min_value: int, max_value: int) -> np.dtype:
    if min_value >= 0:
        for dtype in (np.uint8, np.uint16, np.uint32, np.uint64):
            info = np.iinfo(dtype)
            if max_value <= info.max:
                return np.dtype(dtype)
    for dtype in (np.int8, np.int16, np.int32, np.int64):
        info = np.iinfo(dtype)
        if info.min <= min_value and max_value <= info.max:
            return np.dtype(dtype)
    return np.dtype(np.int64)


def _can_use_integer_rescale(raw_volume: np.ndarray, slopes: list[float], intercepts: list[float]) -> bool:
    return (
        raw_volume.dtype.kind in "iu"
        and all(_is_close(slope, 1.0) for slope in slopes)
        and all(_is_integer_like(intercept) for intercept in intercepts)
    )


def _apply_integer_rescale(raw_volume: np.ndarray, intercepts: list[float]) -> np.ndarray:
    offsets = [int(round(intercept)) for intercept in intercepts]
    value_min = int(raw_volume.min()) + min(offsets)
    value_max = int(raw_volume.max()) + max(offsets)
    target_dtype = _smallest_integer_dtype(value_min, value_max)
    volume = raw_volume.astype(target_dtype, copy=True)
    if not any(offsets):
        return volume
    if len(set(offsets)) == 1:
        volume += np.asarray(offsets[0], dtype=target_dtype)
        return volume
    offsets_array = np.asarray(offsets, dtype=target_dtype).reshape((-1, 1, 1))
    return volume + offsets_array


def _decode_plan_shape(plan: Any) -> tuple[int, ...]:
    return tuple(int(value) for value in plan.shape())


def _decode_plan_shape_all_frames(plan: Any) -> tuple[int, ...]:
    return tuple(int(value) for value in plan.shape(frame=-1))


def _read_bridge_file(path: Path) -> Any:
    return read_file(path, load_until=_METADATA_LOAD_UNTIL)


def _load_pixel_plan(dicom_file: Any) -> Any:
    dicom_file.ensure_loaded("PixelData")
    return dicom_file.create_decode_plan()


def _decode_file_with_plan(dicom_file: Any, plan: Any | None = None) -> np.ndarray:
    if plan is None:
        plan = _load_pixel_plan(dicom_file)
    out = np.empty(_decode_plan_shape(plan), dtype=np.dtype(plan.dtype))
    dicom_file.decode_into(out, frame=0, plan=plan)
    return out


def _decode_all_frames_with_plan(dicom_file: Any, plan: Any) -> np.ndarray:
    out = np.empty(_decode_plan_shape_all_frames(plan), dtype=np.dtype(plan.dtype))
    dicom_file.decode_into(out, frame=-1, plan=plan)
    return out


def _canonical_multiframe_order(
    dicom_file: Any,
    frame_count: int,
    axis_x: np.ndarray,
    axis_y: np.ndarray,
    axis_z: np.ndarray,
    *,
    source_path: Path,
) -> tuple[list[int], list[tuple[float, float, float] | None], list[float | None]]:
    frame_positions = [_frame_position(dicom_file, frame_index) for frame_index in range(frame_count)]
    frame_axes = [_orientation_axes(dicom_file, frame_index) for frame_index in range(frame_count)]
    reference_origin = frame_positions[0]
    if reference_origin is None:
        return (list(range(frame_count)), frame_positions, [None] * frame_count)

    base_origin = np.asarray(reference_origin, dtype=np.float64)
    sort_positions: list[float | None] = []
    for frame_index, position in enumerate(frame_positions):
        frame_axis_x, frame_axis_y, _frame_axis_z = frame_axes[frame_index]
        if _orientation_distance(frame_axis_x, frame_axis_y, axis_x, axis_y) > _ORIENTATION_TOL:
            raise NotImplementedError(
                "Only stack-like multiframe volumes are supported for single-file bridge inputs "
                f"(found varying frame orientations in {source_path.name})"
            )
        if position is None:
            sort_positions.append(None)
            continue
        off_axis = _off_axis_distance(position, base_origin, axis_z)
        if off_axis > _OFF_AXIS_TOL:
            raise NotImplementedError(
                "Only stack-like multiframe volumes are supported for single-file bridge inputs "
                f"(found non-collinear frame positions in {source_path.name})"
            )
        sort_positions.append(_projected_distance(position, base_origin, axis_z))

    frame_indices = list(range(frame_count))
    frame_indices.sort(
        key=lambda frame_index: (
            sort_positions[frame_index] is None,
            sort_positions[frame_index] if sort_positions[frame_index] is not None else 0.0,
            frame_index,
        )
    )
    return (frame_indices, frame_positions, sort_positions)


def _single_source_array(
    dicom_file: Any,
    source_path: Path,
    *,
    to_modality_value: bool,
) -> tuple[np.ndarray, bool, tuple[float, float, float], tuple[float, float, float], tuple[float, ...], int]:
    plan = _load_pixel_plan(dicom_file)
    frame_count = int(plan.frames)
    axis_x, axis_y, axis_z = _orientation_axes(dicom_file, 0)

    if frame_count == 1:
        raw_array = _decode_file_with_plan(dicom_file, plan)
        rescale = dicom_file.rescale_transform_for_frame(0)
        slope = float(rescale.slope) if rescale is not None else 1.0
        intercept = float(rescale.intercept) if rescale is not None else 0.0
        need_rescale = not _is_close(slope, 1.0) or not _is_close(intercept, 0.0)
        spacing = _single_slice_spacing(dicom_file)
        position = _frame_position(dicom_file, 0)
        origin = tuple(float(value) for value in position) if position is not None else (0.0, 0.0, 0.0)
        direction = _direction_tuple(axis_x, axis_y, axis_z)

        if raw_array.ndim == 2:
            volume = raw_array[np.newaxis, :, :]
            if to_modality_value and need_rescale:
                if _can_use_integer_rescale(volume, [slope], [intercept]):
                    volume = _apply_integer_rescale(volume, [intercept])
                else:
                    volume = apply_rescale_frames(volume, [slope], [intercept])
            return (
                np.ascontiguousarray(volume),
                bool(to_modality_value and need_rescale),
                spacing,
                origin,
                direction,
                1,
            )

        if raw_array.ndim == 3 and raw_array.shape[-1] in (3, 4):
            if to_modality_value and need_rescale:
                raise NotImplementedError(
                    "Modality rescale for multi-component 2D images is not supported by this bridge"
                )
            return (
                np.ascontiguousarray(raw_array[np.newaxis, :, :, :]),
                False,
                spacing,
                origin,
                direction,
                1,
            )

        raise NotImplementedError(
            f"Only single-frame 2D grayscale or vector images are supported for single-file bridge inputs "
            f"(got {raw_array.shape} from {source_path.name})"
        )

    raw_array = _decode_all_frames_with_plan(dicom_file, plan)
    if raw_array.ndim != 3:
        raise NotImplementedError(
            "Only multiframe grayscale stacks are supported for single-file bridge inputs "
            f"(got {raw_array.shape} from {source_path.name})"
        )

    frame_indices, frame_positions, sort_positions = _canonical_multiframe_order(
        dicom_file,
        frame_count,
        axis_x,
        axis_y,
        axis_z,
        source_path=source_path,
    )
    if frame_indices != list(range(frame_count)):
        raw_array = np.ascontiguousarray(raw_array[frame_indices, ...])

    slopes: list[float] = []
    intercepts: list[float] = []
    for frame_index in frame_indices:
        rescale = dicom_file.rescale_transform_for_frame(frame_index)
        slopes.append(float(rescale.slope) if rescale is not None else 1.0)
        intercepts.append(float(rescale.intercept) if rescale is not None else 0.0)

    need_rescale = any(
        not _is_close(slope, 1.0) or not _is_close(intercept, 0.0)
        for slope, intercept in zip(slopes, intercepts)
    )
    if to_modality_value and need_rescale:
        if _can_use_integer_rescale(raw_array, slopes, intercepts):
            volume = _apply_integer_rescale(raw_array, intercepts)
        else:
            volume = apply_rescale_frames(raw_array, slopes, intercepts)
    else:
        volume = raw_array

    pixel_spacing = _float_tuple(
        _functional_group_value(
            dicom_file,
            ("PixelMeasuresSequence",),
            "PixelSpacing",
            frame_index=0,
        ),
        2,
    )
    if pixel_spacing is None:
        spacing_x = 1.0
        spacing_y = 1.0
    else:
        spacing_y = float(pixel_spacing[0])
        spacing_x = float(pixel_spacing[1])

    ordered_positions = [
        sort_positions[frame_index]
        for frame_index in frame_indices
        if sort_positions[frame_index] is not None
    ]
    spacing_z = _spacing_z_from_sorted_positions(ordered_positions)
    if spacing_z is None:
        spacing_between = _functional_group_value(
            dicom_file,
            ("PixelMeasuresSequence",),
            "SpacingBetweenSlices",
            frame_index=0,
        )
        if spacing_between is not None:
            spacing_z = float(spacing_between)
    if spacing_z is None:
        slice_thickness = _functional_group_value(
            dicom_file,
            ("PixelMeasuresSequence",),
            "SliceThickness",
            frame_index=0,
        )
        if slice_thickness is not None:
            spacing_z = float(slice_thickness)
    if spacing_z is None:
        spacing_z = 1.0

    first_sorted_position = frame_positions[frame_indices[0]]
    if first_sorted_position is not None:
        origin = tuple(float(value) for value in first_sorted_position)
    else:
        origin = (0.0, 0.0, 0.0)
    spacing = (spacing_x, spacing_y, float(spacing_z))
    direction = _direction_tuple(axis_x, axis_y, axis_z)
    return (
        np.ascontiguousarray(volume),
        bool(to_modality_value and need_rescale),
        spacing,
        origin,
        direction,
        len(set(zip(slopes, intercepts))),
    )


def read_series_volume(
    path: str | Path,
    *,
    to_modality_value: bool = True,
) -> SeriesVolume:
    series_dir, source_paths = _resolve_source_paths(path)
    first_file = _read_bridge_file(source_paths[0])

    if len(source_paths) == 1:
        single_array, rescale_applied, spacing, origin, direction, unique_slope_count = _single_source_array(
            first_file,
            source_paths[0],
            to_modality_value=to_modality_value,
        )

        return SeriesVolume(
            series_dir=series_dir,
            source_paths=source_paths,
            array=single_array,
            spacing=spacing,
            origin=origin,
            direction=direction,
            modality=_optional_text(getattr(first_file, "Modality", None)),
            series_description=_optional_text(getattr(first_file, "SeriesDescription", None)),
            patient_name=_optional_text(getattr(first_file, "PatientName", None)),
            units=_series_units(first_file),
            study_id=_optional_text(getattr(first_file, "StudyID", None)),
            transfer_syntax_uid=_optional_text(getattr(first_file, "TransferSyntaxUID", None)),
            rescale_applied=rescale_applied,
            unique_slope_count=unique_slope_count,
        )

    series_entries: list[_SliceEntry] = []
    for index, source_path in enumerate(source_paths):
        dicom_file = first_file if index == 0 else _read_bridge_file(source_path)
        rows = _int_value(getattr(dicom_file, "Rows", None))
        cols = _int_value(getattr(dicom_file, "Columns", None))
        frames = _int_value(getattr(dicom_file, "NumberOfFrames", None)) or 1
        samples_per_pixel = _int_value(getattr(dicom_file, "SamplesPerPixel", None)) or 1
        if frames != 1 or rows is None or cols is None or samples_per_pixel != 1:
            raise NotImplementedError(
                f"Only single-frame 2D slices are supported for this bridge "
                f"(got rows={rows}, cols={cols}, frames={frames}, samples={samples_per_pixel} from {source_path.name})"
            )

        position = _frame_position(dicom_file, 0)
        instance_number = _int_value(getattr(dicom_file, "InstanceNumber", None))
        rescale = dicom_file.rescale_transform_for_frame(0)
        slope = float(rescale.slope) if rescale is not None else 1.0
        intercept = float(rescale.intercept) if rescale is not None else 0.0
        entry_axis_x, entry_axis_y, entry_axis_z = _orientation_axes(dicom_file, 0)
        series_entries.append(
            _SliceEntry(
                path=source_path,
                dicom_file=dicom_file,
                position=tuple(position) if position is not None else None,
                instance_number=instance_number,
                sort_position=None,
                slope=slope,
                intercept=intercept,
                axis_x=entry_axis_x,
                axis_y=entry_axis_y,
                axis_z=entry_axis_z,
            )
        )

    series_entries = _dominant_orientation_group(series_entries)
    if not series_entries:
        raise FileNotFoundError(f"No supported DICOM slices found under: {series_dir}")

    base_entry = series_entries[0]
    axis_x = base_entry.axis_x
    axis_y = base_entry.axis_y
    axis_z = base_entry.axis_z
    base_origin = (
        np.asarray(base_entry.position, dtype=np.float64)
        if base_entry.position is not None
        else None
    )
    for entry in series_entries:
        if entry.position is None or base_origin is None:
            entry.sort_position = None
            continue
        off_axis = _off_axis_distance(entry.position, base_origin, axis_z)
        if off_axis > _OFF_AXIS_TOL:
            raise NotImplementedError(
                "Only single-stack 3D volumes are supported for this bridge "
                f"(found off-axis slice motion in {series_dir.name})"
            )
        entry.sort_position = _projected_distance(entry.position, base_origin, axis_z)

    series_entries.sort(
        key=lambda entry: (
            entry.sort_position is None,
            entry.sort_position if entry.sort_position is not None else 0.0,
            entry.instance_number if entry.instance_number is not None else 0,
            entry.path.name,
        )
    )

    first_plan = _load_pixel_plan(series_entries[0].dicom_file)
    if int(first_plan.frames) != 1:
        raise NotImplementedError(
            f"Only single-frame 2D slices are supported for this bridge (got {first_plan.frames} frames from {series_entries[0].path.name})"
        )
    expected_shape = _decode_plan_shape(first_plan)
    if len(expected_shape) != 2:
        raise NotImplementedError(
            f"Only 2D slice decode plans are supported for this bridge (got shape {expected_shape} from {series_entries[0].path.name})"
        )
    expected_dtype = np.dtype(first_plan.dtype)
    raw_volume = np.empty((len(series_entries), *expected_shape), dtype=expected_dtype)
    series_entries[0].dicom_file.decode_into(raw_volume[0], frame=0, plan=first_plan)
    for index, entry in enumerate(series_entries[1:], start=1):
        plan = _load_pixel_plan(entry.dicom_file)
        shape = _decode_plan_shape(plan)
        if shape != expected_shape:
            raise ValueError(
                f"Mixed slice shapes are not supported ({expected_shape} vs {shape} from {entry.path.name})"
            )
        if np.dtype(plan.dtype) != expected_dtype:
            raise ValueError(
                f"Mixed decoded dtypes are not supported ({expected_dtype} vs {np.dtype(plan.dtype)} from {entry.path.name})"
            )
        entry.dicom_file.decode_into(raw_volume[index], frame=0, plan=plan)

    slopes = [entry.slope for entry in series_entries]
    intercepts = [entry.intercept for entry in series_entries]
    need_rescale = any(
        not _is_close(slope, 1.0) or not _is_close(intercept, 0.0)
        for slope, intercept in zip(slopes, intercepts)
    )
    if to_modality_value and need_rescale:
        # Preserve integer output when modality rescale is just a per-slice
        # integer offset. PET-style fractional slopes still promote to float32.
        if _can_use_integer_rescale(raw_volume, slopes, intercepts):
            volume = _apply_integer_rescale(raw_volume, intercepts)
        else:
            volume = apply_rescale_frames(raw_volume, slopes, intercepts)
    else:
        volume = raw_volume

    metadata_file = base_entry.dicom_file
    spacing = _spacing(series_entries, metadata_file)
    if series_entries[0].position is not None:
        origin = tuple(float(value) for value in series_entries[0].position)
    else:
        origin = (0.0, 0.0, 0.0)

    return SeriesVolume(
        series_dir=series_dir,
        source_paths=tuple(entry.path for entry in series_entries),
        array=np.ascontiguousarray(volume),
        spacing=spacing,
        origin=origin,
        direction=_direction_tuple(axis_x, axis_y, axis_z),
        modality=_optional_text(getattr(metadata_file, "Modality", None)),
        series_description=_optional_text(getattr(metadata_file, "SeriesDescription", None)),
        patient_name=_optional_text(getattr(metadata_file, "PatientName", None)),
        units=_series_units(metadata_file),
        study_id=_optional_text(getattr(metadata_file, "StudyID", None)),
        transfer_syntax_uid=_optional_text(getattr(metadata_file, "TransferSyntaxUID", None)),
        rescale_applied=bool(to_modality_value and need_rescale),
        unique_slope_count=len({(entry.slope, entry.intercept) for entry in series_entries}),
    )


__all__ = ("SeriesVolume", "read_series_volume")
