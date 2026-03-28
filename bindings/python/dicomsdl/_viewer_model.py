from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

from ._qt_compat import QtCore, QtGui


@dataclass(frozen=True, slots=True)
class FolderEntry:
    path: Path
    name: str
    is_parent: bool = False

    @property
    def display_name(self) -> str:
        if self.is_parent:
            return "[..]"
        return f"[{self.name}/]"


@dataclass(frozen=True, slots=True)
class DicomFileEntry:
    path: Path
    name: str
    is_dicom: bool = True
    study_description: str = ""
    series_description: str = ""
    study_date: str = ""
    patient_id: str = ""
    patient_name: str = ""
    modality: str = ""
    rows: int | None = None
    columns: int | None = None
    bits_allocated: int | None = None
    frames: int = 1

    @property
    def size_summary(self) -> str:
        return " x ".join(
            [
                str(self.rows or "?"),
                str(self.columns or "?"),
                str(self.bits_allocated or "?"),
                str(self.frames or 1),
            ]
        )


ViewerEntry = FolderEntry | DicomFileEntry


class FileTableModel(QtCore.QAbstractTableModel):
    columns = (
        "Name",
        "Study",
        "Series",
        "Date",
        "Patient ID",
        "Patient Name",
        "Modality",
        "Size",
    )

    EntryRole = int(QtCore.Qt.ItemDataRole.UserRole) + 1
    PathRole = EntryRole + 1
    KindRole = EntryRole + 2

    def __init__(self, parent=None) -> None:
        super().__init__(parent)
        self._entries: list[ViewerEntry] = []
        self._sort_column = 0
        self._sort_order = QtCore.Qt.SortOrder.AscendingOrder

    def clear(self) -> None:
        self.beginResetModel()
        self._entries = []
        self.endResetModel()

    def append_entry(self, entry: ViewerEntry) -> None:
        row = len(self._entries)
        self.beginInsertRows(QtCore.QModelIndex(), row, row)
        self._entries.append(entry)
        self.endInsertRows()

    def entry_at(self, row: int) -> ViewerEntry | None:
        if 0 <= row < len(self._entries):
            return self._entries[row]
        return None

    def row_of_path(self, path: Path) -> int:
        for row, entry in enumerate(self._entries):
            if entry.path == path:
                return row
        return -1

    def first_file_row(self) -> int:
        for row, entry in enumerate(self._entries):
            if isinstance(entry, DicomFileEntry):
                return row
        return -1

    def next_file_row(self, current_row: int) -> int:
        for row in range(current_row + 1, len(self._entries)):
            if isinstance(self._entries[row], DicomFileEntry):
                return row
        return -1

    def previous_file_row(self, current_row: int) -> int:
        for row in range(current_row - 1, -1, -1):
            if isinstance(self._entries[row], DicomFileEntry):
                return row
        return -1

    def file_position(self, row: int) -> tuple[int, int]:
        total = sum(1 for entry in self._entries if isinstance(entry, DicomFileEntry))
        if total == 0:
            return (0, 0)
        position = 0
        for index, entry in enumerate(self._entries):
            if isinstance(entry, DicomFileEntry):
                position += 1
            if index == row:
                return (position, total) if isinstance(entry, DicomFileEntry) else (0, total)
        return (0, total)

    def rowCount(self, parent=QtCore.QModelIndex()) -> int:
        if parent.isValid():
            return 0
        return len(self._entries)

    def columnCount(self, parent=QtCore.QModelIndex()) -> int:
        if parent.isValid():
            return 0
        return len(self.columns)

    def headerData(self, section: int, orientation, role=int(QtCore.Qt.ItemDataRole.DisplayRole)):
        if role != int(QtCore.Qt.ItemDataRole.DisplayRole):
            return None
        if orientation == QtCore.Qt.Orientation.Horizontal and 0 <= section < len(self.columns):
            return self.columns[section]
        return None

    def data(self, index, role=int(QtCore.Qt.ItemDataRole.DisplayRole)):
        if not index.isValid():
            return None

        entry = self._entries[index.row()]
        column = index.column()

        if role == int(QtCore.Qt.ItemDataRole.DisplayRole):
            if isinstance(entry, FolderEntry):
                return entry.display_name if column == 0 else ""
            return self._display_value(entry, column)

        if role == int(QtCore.Qt.ItemDataRole.FontRole) and isinstance(entry, FolderEntry):
            font = QtGui.QFont()
            font.setBold(True)
            return font

        if role == int(QtCore.Qt.ItemDataRole.ForegroundRole):
            if isinstance(entry, FolderEntry):
                return QtGui.QBrush(QtGui.QColor("#79b8f2"))
            if isinstance(entry, DicomFileEntry) and not entry.is_dicom:
                return QtGui.QBrush(QtGui.QColor("#7c8795"))

        if role == int(QtCore.Qt.ItemDataRole.ToolTipRole):
            if isinstance(entry, FolderEntry):
                return str(entry.path)
            if isinstance(entry, DicomFileEntry) and not entry.is_dicom:
                return f"{entry.path}\n\nNot a DICOM file."
            return str(entry.path)

        if role == int(QtCore.Qt.ItemDataRole.TextAlignmentRole) and column == 7:
            return int(QtCore.Qt.AlignmentFlag.AlignVCenter | QtCore.Qt.AlignmentFlag.AlignLeft)

        if role == self.EntryRole:
            return entry
        if role == self.PathRole:
            return str(entry.path)
        if role == self.KindRole:
            return "folder" if isinstance(entry, FolderEntry) else "file"
        return None

    def flags(self, index):
        if not index.isValid():
            return QtCore.Qt.ItemFlag.NoItemFlags
        return (
            QtCore.Qt.ItemFlag.ItemIsEnabled
            | QtCore.Qt.ItemFlag.ItemIsSelectable
        )

    def sort(
        self,
        column: int,
        order: QtCore.Qt.SortOrder = QtCore.Qt.SortOrder.AscendingOrder,
    ) -> None:
        if not self._entries:
            self._sort_column = column
            self._sort_order = order
            return

        self.layoutAboutToBeChanged.emit()

        parent_folders = [
            entry
            for entry in self._entries
            if isinstance(entry, FolderEntry) and entry.is_parent
        ]
        child_folders = sorted(
            (
                entry
                for entry in self._entries
                if isinstance(entry, FolderEntry) and not entry.is_parent
            ),
            key=lambda entry: entry.name.casefold(),
        )
        files = [
            entry for entry in self._entries if isinstance(entry, DicomFileEntry)
        ]
        files.sort(
            key=lambda entry: self._file_sort_key(entry, column),
            reverse=order == QtCore.Qt.SortOrder.DescendingOrder,
        )

        self._entries = [*parent_folders, *child_folders, *files]
        self._sort_column = column
        self._sort_order = order
        self.layoutChanged.emit()

    def current_sort(self) -> tuple[int, QtCore.Qt.SortOrder]:
        return (self._sort_column, self._sort_order)

    @staticmethod
    def _display_value(entry: DicomFileEntry, column: int) -> str:
        values = (
            entry.name,
            entry.study_description,
            entry.series_description,
            entry.study_date,
            entry.patient_id,
            entry.patient_name,
            entry.modality,
            entry.size_summary,
        )
        return values[column]

    @staticmethod
    def _file_sort_key(entry: DicomFileEntry, column: int):
        if column == 0:
            value = entry.name
        elif column == 1:
            value = entry.study_description
        elif column == 2:
            value = entry.series_description
        elif column == 3:
            value = entry.study_date
        elif column == 4:
            value = entry.patient_id
        elif column == 5:
            value = entry.patient_name
        elif column == 6:
            value = entry.modality
        elif column == 7:
            return (
                entry.rows or -1,
                entry.columns or -1,
                entry.bits_allocated or -1,
                entry.frames or 1,
                entry.name.casefold(),
            )
        else:
            value = entry.name

        text = (value or "").strip()
        return (text == "", text.casefold(), entry.name.casefold())
