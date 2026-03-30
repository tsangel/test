from __future__ import annotations

from pathlib import Path

from . import BufferingReporter, DataSetSelection, is_dicom_file, read_file_selected, set_thread_reporter
from ._qt_compat import QtCore, Signal, Slot
from ._viewer_model import DicomFileEntry, FolderEntry

# Reuse one canonicalized selection tree for directory scans so we only parse
# the metadata columns dicomview actually needs to populate the file table.
SCAN_METADATA_SELECTION = DataSetSelection(
    [
        "StudyDescription",
        "SeriesDescription",
        "StudyDate",
        "PatientID",
        "PatientName",
        "Modality",
        "Rows",
        "Columns",
        "BitsAllocated",
        "NumberOfFrames",
    ]
)


def _sort_key(path: Path) -> tuple[str, str]:
    return (path.name.casefold(), path.name)


def _text_value(dicom_file, keyword: str) -> str:
    element = dicom_file.get_dataelement(keyword)
    try:
        text = element.to_utf8_string(errors="replace_fffd")
    except Exception:
        try:
            text = element.to_string_view()
        except Exception:
            return ""
    if text is None:
        return ""
    text = text.strip()
    if keyword == "StudyDate" and len(text) == 8 and text.isdigit():
        return f"{text[0:4]}-{text[4:6]}-{text[6:8]}"
    return text


def _int_value(dicom_file, keyword: str, default: int | None = None) -> int | None:
    value = dicom_file.get_dataelement(keyword).to_long()
    if value is None:
        return default
    return int(value)


def _read_file_entry(path: Path) -> DicomFileEntry | None:
    if not is_dicom_file(path):
        return DicomFileEntry(path=path, name=path.name, is_dicom=False)
    try:
        dicom_file = read_file_selected(path, SCAN_METADATA_SELECTION)
    except Exception:
        return DicomFileEntry(path=path, name=path.name)

    try:
        rows = _int_value(dicom_file, "Rows")
        columns = _int_value(dicom_file, "Columns")
        bits_allocated = _int_value(dicom_file, "BitsAllocated")
        number_of_frames = _int_value(dicom_file, "NumberOfFrames", 1) or 1

        return DicomFileEntry(
            path=path,
            name=path.name,
            study_description=_text_value(dicom_file, "StudyDescription"),
            series_description=_text_value(dicom_file, "SeriesDescription"),
            study_date=_text_value(dicom_file, "StudyDate"),
            patient_id=_text_value(dicom_file, "PatientID"),
            patient_name=_text_value(dicom_file, "PatientName"),
            modality=_text_value(dicom_file, "Modality"),
            rows=rows,
            columns=columns,
            bits_allocated=bits_allocated,
            frames=number_of_frames,
        )
    except Exception:
        return DicomFileEntry(path=path, name=path.name)


class FolderScanWorker(QtCore.QObject):
    entry_ready = Signal(int, object)
    scan_failed = Signal(int, str)
    scan_finished = Signal(int, str)

    def __init__(self, scan_id: int, directory: Path, parent=None) -> None:
        super().__init__(parent)
        self._scan_id = scan_id
        self._directory = directory
        self._cancelled = False

    def cancel(self) -> None:
        self._cancelled = True

    @Slot()
    def run(self) -> None:
        reporter = BufferingReporter()
        set_thread_reporter(reporter)
        try:
            self._scan_directory()
        except Exception as exc:
            self.scan_failed.emit(self._scan_id, str(exc))
            self.scan_finished.emit(self._scan_id, str(self._directory))
        finally:
            set_thread_reporter(None)

    def _scan_directory(self) -> None:
        directory = self._directory
        parent = directory.parent
        if parent != directory:
            self.entry_ready.emit(
                self._scan_id,
                FolderEntry(path=parent, name="..", is_parent=True),
            )

        folders: list[Path] = []
        files: list[Path] = []
        for child in directory.iterdir():
            if self._cancelled:
                self.scan_finished.emit(self._scan_id, str(directory))
                return
            if child.is_dir():
                folders.append(child)
            elif child.is_file():
                files.append(child)

        for folder in sorted(folders, key=_sort_key):
            if self._cancelled:
                self.scan_finished.emit(self._scan_id, str(directory))
                return
            self.entry_ready.emit(
                self._scan_id,
                FolderEntry(path=folder, name=folder.name),
            )

        for file_path in sorted(files, key=_sort_key):
            if self._cancelled:
                self.scan_finished.emit(self._scan_id, str(directory))
                return
            entry = _read_file_entry(file_path)
            if entry is not None:
                self.entry_ready.emit(self._scan_id, entry)

        self.scan_finished.emit(self._scan_id, str(directory))
