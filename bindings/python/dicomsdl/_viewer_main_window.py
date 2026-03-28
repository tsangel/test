from __future__ import annotations

import math
import os
from pathlib import Path

from ._qt_compat import QtCore, QtGui, QtWidgets
from ._viewer_canvas import ImageCanvas
from ._viewer_display import DisplaySettings, modality_group
from ._viewer_model import DicomFileEntry, FileTableModel, FolderEntry
from ._viewer_render import (
    DicomNotDisplayableError,
    NonDicomFileError,
    RenderedFrame,
    load_dicom_for_view,
    render_loaded_dicom,
)
from ._viewer_scan_worker import FolderScanWorker
from ._viewer_state import ViewerState
from ._viewer_theme import fixed_ui_font, general_ui_font

DUMP_HEADERS = ("TAG", "VR", "LEN", "VM", "OFFSET", "VALUE", "KEYWORD")


def _format_pixel_representation(value: int | None) -> str:
    if value == 0:
        return "unsigned (0)"
    if value == 1:
        return "signed (1)"
    if value is None:
        return "-"
    return str(value)


def _format_stored_bits(bits_stored: int | None, high_bit: int | None) -> str:
    if bits_stored is None and high_bit is None:
        return "-"
    left = "?" if bits_stored is None else str(bits_stored)
    right = "?" if high_bit is None else str(high_bit)
    return f"{left} / high {right}"


def _format_raw_range(minimum: float | int | None, maximum: float | int | None) -> str:
    if minimum is None or maximum is None:
        return "-"
    return f"{minimum} .. {maximum}"


def _window_from_bounds(lower: float, upper: float) -> tuple[float, float]:
    width = max(1.0, float(upper) - float(lower) + 1.0)
    center = (float(lower) + float(upper) + 1.0) * 0.5
    return (center, width)


def _format_special_dump_row(text: str) -> str:
    if text.startswith("FRAME #"):
        return f"  {text}"
    if text.startswith("FRAGMENT #"):
        return f"      {text}"
    return text


def _sync_wrapped_label_height(label: QtWidgets.QLabel) -> None:
    width = max(1, label.width(), label.geometry().width())
    if width <= 1:
        width = max(120, label.sizeHint().width())
    if label.wordWrap() and label.hasHeightForWidth():
        height = label.heightForWidth(width)
    else:
        height = label.sizeHint().height()
    height = max(label.fontMetrics().height(), height)
    label.setMinimumHeight(height)
    label.setMaximumHeight(height)
    label.updateGeometry()


class FileTableHeaderView(QtWidgets.QHeaderView):
    sectionActivated = QtCore.Signal(int)

    def __init__(self, orientation, parent=None) -> None:
        super().__init__(orientation, parent)
        self.setDefaultAlignment(
            QtCore.Qt.AlignmentFlag.AlignLeft | QtCore.Qt.AlignmentFlag.AlignVCenter
        )
        self._drag_press_pos: QtCore.QPoint | None = None
        self._drag_section: int = -1
        self._drag_active = False
        self._pressed_on_divider = False
        self._resize_hotspot = 6

    def paintSection(self, painter, rect, logical_index) -> None:
        super().paintSection(painter, rect, logical_index)

        if logical_index != self.sortIndicatorSection():
            return
        if rect.width() < 36 or rect.height() < 18:
            return

        order = self.sortIndicatorOrder()
        triangle_width = 8
        triangle_height = 6
        x = rect.right() - 16
        y = rect.center().y()

        if order == QtCore.Qt.SortOrder.AscendingOrder:
            points = [
                QtCore.QPointF(x - triangle_width / 2, y + triangle_height / 2),
                QtCore.QPointF(x + triangle_width / 2, y + triangle_height / 2),
                QtCore.QPointF(x, y - triangle_height / 2),
            ]
        else:
            points = [
                QtCore.QPointF(x - triangle_width / 2, y - triangle_height / 2),
                QtCore.QPointF(x + triangle_width / 2, y - triangle_height / 2),
                QtCore.QPointF(x, y + triangle_height / 2),
            ]

        painter.save()
        painter.setRenderHint(QtGui.QPainter.RenderHint.Antialiasing, True)
        painter.setPen(QtCore.Qt.PenStyle.NoPen)
        painter.setBrush(QtGui.QColor("#d9e4f2"))
        painter.drawPolygon(QtGui.QPolygonF(points))
        painter.restore()

    def mousePressEvent(self, event) -> None:
        point = self._event_point(event)
        self._drag_press_pos = point
        self._drag_section = self.logicalIndexAt(point)
        self._drag_active = False
        self._pressed_on_divider = self._is_resize_hotspot(point)
        if self._pressed_on_divider:
            super().mousePressEvent(event)
            return
        if event.button() == QtCore.Qt.MouseButton.LeftButton:
            event.accept()
            return
        super().mousePressEvent(event)

    def mouseMoveEvent(self, event) -> None:
        if self._pressed_on_divider:
            super().mouseMoveEvent(event)
            return

        if not (event.buttons() & QtCore.Qt.MouseButton.LeftButton):
            super().mouseMoveEvent(event)
            return

        if self._drag_press_pos is None or self._drag_section < 0:
            super().mouseMoveEvent(event)
            return

        point = self._event_point(event)
        if not self._drag_active:
            distance = (point - self._drag_press_pos).manhattanLength()
            if distance < QtWidgets.QApplication.startDragDistance():
                return
            self._drag_active = True

        target_logical = self.logicalIndexAt(point)
        if target_logical < 0:
            target_visual = 0 if point.x() < 0 else self.count() - 1
        else:
            target_visual = self.visualIndex(target_logical)
        source_visual = self.visualIndex(self._drag_section)
        if source_visual != target_visual:
            self.moveSection(source_visual, target_visual)
        event.accept()

    def mouseReleaseEvent(self, event) -> None:
        released_section = self._drag_section
        released_point = self._event_point(event)
        clicked_same_section = (
            released_section >= 0
            and self.logicalIndexAt(released_point) == released_section
        )
        suppress_click = self._drag_active and not self._pressed_on_divider
        was_divider = self._pressed_on_divider
        self._drag_press_pos = None
        self._drag_section = -1
        self._drag_active = False
        self._pressed_on_divider = False
        if was_divider:
            super().mouseReleaseEvent(event)
            return
        if suppress_click:
            event.accept()
            return
        if (
            event.button() == QtCore.Qt.MouseButton.LeftButton
            and clicked_same_section
        ):
            self.sectionActivated.emit(released_section)
            event.accept()
            return
        super().mouseReleaseEvent(event)

    def _is_resize_hotspot(self, point: QtCore.QPoint) -> bool:
        for visual_index in range(self.count()):
            logical_index = self.logicalIndex(visual_index)
            if logical_index < 0 or self.isSectionHidden(logical_index):
                continue
            right_edge = (
                self.sectionViewportPosition(logical_index)
                + self.sectionSize(logical_index)
            )
            if abs(point.x() - right_edge) <= self._resize_hotspot:
                return True
        return False

    @staticmethod
    def _event_point(event) -> QtCore.QPoint:
        return event.position().toPoint() if hasattr(event, "position") else event.pos()


class DumpTableModel(QtCore.QAbstractTableModel):
    def __init__(self, parent=None) -> None:
        super().__init__(parent)
        self._headers: list[str] = list(DUMP_HEADERS)
        self._rows: list[list[str]] = []
        self._special_rows: set[int] = set()

    def set_dump(
        self,
        headers: list[str],
        rows: list[list[str]],
        special_rows: set[int] | None = None,
    ) -> None:
        self.beginResetModel()
        self._headers = list(headers)
        self._rows = [list(row) for row in rows]
        self._special_rows = set() if special_rows is None else set(special_rows)
        self.endResetModel()

    def clear(self) -> None:
        self.set_dump(list(DUMP_HEADERS), [], set())

    def rowCount(self, parent=QtCore.QModelIndex()) -> int:
        if parent.isValid():
            return 0
        return len(self._rows)

    def columnCount(self, parent=QtCore.QModelIndex()) -> int:
        if parent.isValid():
            return 0
        return len(self._headers)

    def headerData(self, section: int, orientation, role=int(QtCore.Qt.ItemDataRole.DisplayRole)):
        if role != int(QtCore.Qt.ItemDataRole.DisplayRole):
            return None
        if orientation == QtCore.Qt.Orientation.Horizontal and 0 <= section < len(self._headers):
            return self._headers[section]
        return None

    def data(self, index, role=int(QtCore.Qt.ItemDataRole.DisplayRole)):
        if not index.isValid():
            return None
        row = index.row()
        column = index.column()
        if not (0 <= row < len(self._rows) and 0 <= column < len(self._headers)):
            return None
        if row in self._special_rows:
            special_text = self._rows[row][0] if self._rows[row] else ""
            if role == int(QtCore.Qt.ItemDataRole.DisplayRole):
                return _format_special_dump_row(special_text) if column == 0 else ""
            if role == int(QtCore.Qt.ItemDataRole.ToolTipRole):
                return special_text
            if role == int(QtCore.Qt.ItemDataRole.TextAlignmentRole):
                return int(QtCore.Qt.AlignmentFlag.AlignLeft | QtCore.Qt.AlignmentFlag.AlignVCenter)
            if role == int(QtCore.Qt.ItemDataRole.ForegroundRole):
                return QtGui.QColor("#d6dee9")
            if role == int(QtCore.Qt.ItemDataRole.BackgroundRole):
                return QtGui.QColor("#11171d")
            return None
        value = self._rows[row][column]
        if role == int(QtCore.Qt.ItemDataRole.DisplayRole):
            return value
        if role == int(QtCore.Qt.ItemDataRole.ToolTipRole):
            return value
        if role == int(QtCore.Qt.ItemDataRole.TextAlignmentRole):
            if column in {1, 2, 3, 4}:
                return int(QtCore.Qt.AlignmentFlag.AlignCenter)
            return int(QtCore.Qt.AlignmentFlag.AlignLeft | QtCore.Qt.AlignmentFlag.AlignVCenter)
        return None

    def is_special_row(self, row: int) -> bool:
        return row in self._special_rows


class DumpFilterProxyModel(QtCore.QSortFilterProxyModel):
    def __init__(self, parent=None) -> None:
        super().__init__(parent)
        self._needle = ""

    def set_filter_text(self, text: str) -> None:
        self._needle = text.casefold().strip()
        self.invalidateFilter()

    def filterAcceptsRow(self, source_row: int, source_parent) -> bool:
        if not self._needle:
            return True
        model = self.sourceModel()
        if model is None:
            return False
        for column in range(model.columnCount()):
            index = model.index(source_row, column, source_parent)
            text = model.data(index, int(QtCore.Qt.ItemDataRole.DisplayRole))
            if text is not None and self._needle in str(text).casefold():
                return True
        return False


class ViewerMainWindow(QtWidgets.QMainWindow):
    def __init__(self, initial_input: Path | None = None, parent=None) -> None:
        super().__init__(parent)
        initial_dir = Path.cwd()
        self._state = ViewerState(current_dir=initial_dir)
        self._settings = self._create_settings()
        self._scan_thread = None
        self._scan_worker = None
        self._scan_id = 0
        self._pending_selection: Path | None = None
        self._ignore_frame_changes = False
        self._settings_ready = False
        self._layout_restored = False
        self._pending_splitter_sizes: list[int] | None = None
        self._pending_window_maximized = False
        self._current_dicom_file = None
        self._current_dicom_path: Path | None = None
        self._last_rendered_frame: RenderedFrame | None = None
        self._display_drag_origin: DisplaySettings | None = None
        self._display_drag_dx = 0.0
        self._display_drag_dy = 0.0
        self._settings_save_timer = QtCore.QTimer(self)
        self._settings_save_timer.setSingleShot(True)
        self._settings_save_timer.setInterval(250)

        self._model = FileTableModel(self)
        self._table = QtWidgets.QTableView(self)
        self._table.setHorizontalHeader(
            FileTableHeaderView(QtCore.Qt.Orientation.Horizontal, self._table)
        )
        self._canvas = ImageCanvas(self)
        self._path_edit = QtWidgets.QLineEdit(self)
        self._frame_spin = QtWidgets.QSpinBox(self)
        self._frame_scroll = QtWidgets.QScrollBar(QtCore.Qt.Orientation.Horizontal, self)
        self._frame_first_button = QtWidgets.QToolButton(self)
        self._frame_play_button = QtWidgets.QToolButton(self)
        self._frame_last_button = QtWidgets.QToolButton(self)
        self._frame_status_label = QtWidgets.QLabel("1 / 1", self)
        self._frame_bar = QtWidgets.QWidget(self)
        self._info_tabs = None
        self._details_tab = None
        self._dump_tab = None
        self._dump_filter_edit = QtWidgets.QLineEdit(self)
        self._dump_columns_button = QtWidgets.QToolButton(self)
        self._dump_stack = QtWidgets.QStackedWidget(self)
        self._dump_message_label = QtWidgets.QLabel(self)
        self._dump_table = QtWidgets.QTableView(self)
        self._dump_model = DumpTableModel(self)
        self._dump_proxy = DumpFilterProxyModel(self)
        self._info_labels: dict[str, QtWidgets.QLabel] = {}
        self._dump_source_path: Path | None = None
        self._dump_loaded_path: Path | None = None
        self._dump_full_text = ""
        self._dump_headers: list[str] = []
        self._dump_rows: list[list[str]] = []
        self._dump_special_rows: set[int] = set()
        self._dump_error_text = ""
        self._dump_layout_key: tuple[str, ...] | None = None
        self._pending_dump_widths: dict[int, int] = {}
        self._pending_dump_hidden: dict[int, bool] = {}
        self._dump_column_actions: list[QtGui.QAction] = []
        self._column_actions: list[QtGui.QAction] = []
        self._cine_timer = QtCore.QTimer(self)
        self._cine_timer.setInterval(100)

        self._prev_action = None
        self._next_action = None
        self._toolbar = None
        self._splitter = None
        self._columns_button = None
        self._display_button = None
        self._display_auto_action = None
        self._display_action_groups: list[tuple[QtGui.QAction, set[str] | None, bool]] = []
        self._display_digit_shortcuts: list[QtGui.QShortcut] = []
        self._help_shortcuts: list[QtGui.QShortcut] = []
        self._shortcuts_action = None
        self._shortcuts_dialog = None
        self._header_resize_hotspot = 6

        self._build_ui()
        self._apply_theme()
        self._connect_signals()

        self.setWindowTitle("dicomview")
        self._restore_window_settings()
        self._restore_table_settings()
        self._restore_dump_table_settings()
        self._settings_ready = True

        target = (
            Path(initial_input).expanduser()
            if initial_input is not None
            else self._restore_last_path()
        )
        self.open_location(target)

    def closeEvent(self, event) -> None:
        self._stop_playback()
        self._stop_scan_worker()
        self._save_settings()
        super().closeEvent(event)

    def showEvent(self, event) -> None:
        super().showEvent(event)
        if self._layout_restored:
            return
        self._layout_restored = True
        if self._pending_splitter_sizes is not None and self._splitter is not None:
            sizes = list(self._pending_splitter_sizes)
            QtCore.QTimer.singleShot(0, lambda sizes=sizes: self._splitter.setSizes(sizes))
        if self._pending_window_maximized:
            QtCore.QTimer.singleShot(0, self.showMaximized)

    def moveEvent(self, event) -> None:
        super().moveEvent(event)
        self._queue_save_settings()

    def resizeEvent(self, event) -> None:
        super().resizeEvent(event)
        self._queue_save_settings()
        QtCore.QTimer.singleShot(0, self._refresh_info_label_heights)

    def changeEvent(self, event) -> None:
        super().changeEvent(event)
        if event.type() == QtCore.QEvent.Type.WindowStateChange:
            self._queue_save_settings()

    @staticmethod
    def _create_settings() -> QtCore.QSettings:
        settings_path = os.environ.get("DICOMVIEW_SETTINGS_PATH", "").strip()
        if settings_path:
            return QtCore.QSettings(settings_path, QtCore.QSettings.Format.IniFormat)
        return QtCore.QSettings("dicomsdl", "dicomview")

    @staticmethod
    def _sort_order_value(order) -> int:
        return int(getattr(order, "value", order))

    def _restore_last_path(self) -> Path:
        raw = self._settings.value("browser/last_path", "", type=str)
        if raw:
            candidate = Path(raw).expanduser()
            if candidate.exists():
                return candidate
        return Path.cwd()

    def _build_ui(self) -> None:
        self.setObjectName("dicomviewWindow")
        self._path_edit.setReadOnly(True)
        self._path_edit.setObjectName("dicomviewPathBar")

        self._toolbar = self.addToolBar("Main")
        self._toolbar.setObjectName("dicomviewToolbar")
        self._toolbar.setMovable(False)
        self._toolbar.setFloatable(False)
        self._toolbar.setToolButtonStyle(QtCore.Qt.ToolButtonStyle.ToolButtonTextOnly)

        open_folder_action = self._toolbar.addAction("Open Folder")
        open_folder_action.triggered.connect(self._open_folder_dialog)

        open_file_action = self._toolbar.addAction("Open File")
        open_file_action.triggered.connect(self._open_file_dialog)

        up_action = self._toolbar.addAction("Up")
        up_action.setShortcut("Alt+Up")
        up_action.triggered.connect(self._go_up)

        refresh_action = self._toolbar.addAction("Refresh")
        refresh_action.triggered.connect(self._refresh)

        self._toolbar.addSeparator()

        self._columns_button = QtWidgets.QToolButton(self)
        self._columns_button.setObjectName("dicomviewColumnsButton")
        self._columns_button.setText("Columns")
        self._columns_button.setToolTip("Show or hide table columns")
        self._columns_button.setPopupMode(
            QtWidgets.QToolButton.ToolButtonPopupMode.InstantPopup
        )
        self._columns_button.setToolButtonStyle(
            QtCore.Qt.ToolButtonStyle.ToolButtonTextBesideIcon
        )
        self._columns_button.setLayoutDirection(QtCore.Qt.LayoutDirection.RightToLeft)
        self._columns_button.setIcon(
            self.style().standardIcon(QtWidgets.QStyle.StandardPixmap.SP_ArrowDown)
        )
        self._columns_button.setMenu(self._build_columns_menu(self._columns_button))
        self._toolbar.addWidget(self._columns_button)

        self._toolbar.addSeparator()

        self._prev_action = self._toolbar.addAction("Prev")
        self._prev_action.setShortcut("Left")
        self._prev_action.triggered.connect(self._select_previous_file)

        self._next_action = self._toolbar.addAction("Next")
        self._next_action.setShortcut("Right")
        self._next_action.triggered.connect(self._select_next_file)

        self._toolbar.addSeparator()

        fit_action = self._toolbar.addAction("Fit")
        fit_action.setShortcut("F")
        fit_action.triggered.connect(self._canvas.fit_to_window)

        actual_action = self._toolbar.addAction("100%")
        actual_action.setShortcut("Ctrl+1")
        actual_action.triggered.connect(self._canvas.actual_size)

        reset_action = self._toolbar.addAction("Reset")
        reset_action.setShortcut("R")
        reset_action.triggered.connect(self._canvas.reset_zoom)

        self._toolbar.addSeparator()

        self._display_auto_action = self._toolbar.addAction("Auto")
        self._display_auto_action.setShortcut("0")
        self._display_auto_action.setToolTip("Reset display to modality-aware auto mode")
        self._display_auto_action.triggered.connect(self._set_display_auto)

        self._display_button = QtWidgets.QToolButton(self)
        self._display_button.setObjectName("dicomviewDisplayButton")
        self._display_button.setText("Display")
        self._display_button.setToolTip("Choose display presets for the current file")
        self._display_button.setPopupMode(
            QtWidgets.QToolButton.ToolButtonPopupMode.InstantPopup
        )
        self._display_button.setToolButtonStyle(
            QtCore.Qt.ToolButtonStyle.ToolButtonTextBesideIcon
        )
        self._display_button.setLayoutDirection(QtCore.Qt.LayoutDirection.RightToLeft)
        self._display_button.setIcon(
            self.style().standardIcon(QtWidgets.QStyle.StandardPixmap.SP_ArrowDown)
        )
        self._display_button.setMenu(self._build_display_menu(self._display_button))
        self._toolbar.addWidget(self._display_button)

        self._toolbar.addSeparator()

        self._shortcuts_action = self._toolbar.addAction("Help")
        self._shortcuts_action.setToolTip("Show keyboard and mouse help")
        self._shortcuts_action.triggered.connect(self._show_shortcuts_dialog)

        central = QtWidgets.QWidget(self)
        central.setObjectName("dicomviewCentral")
        self.setCentralWidget(central)

        root_layout = QtWidgets.QVBoxLayout(central)
        root_layout.setContentsMargins(10, 8, 10, 10)
        root_layout.setSpacing(10)
        root_layout.addWidget(self._path_edit)

        self._splitter = QtWidgets.QSplitter(QtCore.Qt.Orientation.Horizontal, central)
        self._splitter.setObjectName("dicomviewMainSplitter")
        root_layout.addWidget(self._splitter, 1)

        self._table.setModel(self._model)
        self._table.setObjectName("dicomviewTable")
        self._table.setSelectionBehavior(QtWidgets.QAbstractItemView.SelectionBehavior.SelectRows)
        self._table.setSelectionMode(QtWidgets.QAbstractItemView.SelectionMode.SingleSelection)
        self._table.setEditTriggers(QtWidgets.QAbstractItemView.EditTrigger.NoEditTriggers)
        self._table.setAlternatingRowColors(True)
        self._table.setSortingEnabled(False)
        header = self._table.horizontalHeader()
        header.setStretchLastSection(False)
        self._table.verticalHeader().setVisible(False)
        self._table.setWordWrap(False)
        self._table.setShowGrid(False)
        self._table.setFocusPolicy(QtCore.Qt.FocusPolicy.StrongFocus)
        self._table.setVerticalScrollMode(
            QtWidgets.QAbstractItemView.ScrollMode.ScrollPerPixel
        )
        self._table.setHorizontalScrollMode(
            QtWidgets.QAbstractItemView.ScrollMode.ScrollPerPixel
        )
        self._table.setMouseTracking(True)
        header.setSectionsMovable(False)
        header.setFirstSectionMovable(True)
        header.setSectionsClickable(True)
        header.setHighlightSections(False)
        header.setSectionResizeMode(QtWidgets.QHeaderView.ResizeMode.Interactive)
        header.setSortIndicatorShown(True)
        header.setToolTip(
            "Click to sort. Drag the header to reorder columns. Drag the divider to resize. Double-click the divider to fit the content."
        )
        header.viewport().installEventFilter(self)
        self._splitter.addWidget(self._table)
        self._splitter.addWidget(self._build_viewer_panel())
        self._splitter.addWidget(self._build_info_panel())
        self._splitter.setChildrenCollapsible(False)
        self._splitter.setSizes(self._default_splitter_sizes())
        self._splitter.setStretchFactor(0, 5)
        self._splitter.setStretchFactor(1, 8)
        self._splitter.setStretchFactor(2, 3)

        prev_frame_shortcut = QtGui.QShortcut(QtGui.QKeySequence("PageUp"), self)
        prev_frame_shortcut.activated.connect(self._previous_frame)
        next_frame_shortcut = QtGui.QShortcut(QtGui.QKeySequence("PageDown"), self)
        next_frame_shortcut.activated.connect(self._next_frame)
        toggle_playback_shortcut = QtGui.QShortcut(QtGui.QKeySequence("Space"), self)
        toggle_playback_shortcut.activated.connect(self._toggle_playback)

        activate_return_shortcut = QtGui.QShortcut(QtGui.QKeySequence("Return"), self._table)
        activate_return_shortcut.activated.connect(self._activate_current_selection)
        activate_enter_shortcut = QtGui.QShortcut(QtGui.QKeySequence("Enter"), self._table)
        activate_enter_shortcut.activated.connect(self._activate_current_selection)
        go_up_shortcut = QtGui.QShortcut(QtGui.QKeySequence("Backspace"), self)
        go_up_shortcut.activated.connect(self._go_up)

        for digit in range(1, 10):
            shortcut = QtGui.QShortcut(QtGui.QKeySequence(str(digit)), self)
            shortcut.activated.connect(
                lambda digit=digit: self._trigger_display_preset_shortcut(digit)
            )
            self._display_digit_shortcuts.append(shortcut)

        shortcut = QtGui.QShortcut(QtGui.QKeySequence("F1"), self)
        shortcut.setContext(QtCore.Qt.ShortcutContext.WidgetWithChildrenShortcut)
        shortcut.activated.connect(self._show_shortcuts_dialog)
        self._help_shortcuts.append(shortcut)

    def _build_columns_menu(self, parent) -> QtWidgets.QMenu:
        menu = QtWidgets.QMenu(parent)
        self._column_actions = []
        for column, title in enumerate(self._model.columns):
            action = menu.addAction(title)
            action.setCheckable(True)
            action.setChecked(True)
            action.toggled.connect(
                lambda checked, column=column: self._set_column_visible(column, checked)
            )
            self._column_actions.append(action)
        return menu

    def _build_dump_columns_menu(self, parent) -> QtWidgets.QMenu:
        menu = QtWidgets.QMenu(parent)
        self._dump_column_actions = []
        headers = self._dump_headers or list(self._dump_model._headers)
        for column, title in enumerate(headers):
            action = menu.addAction(title)
            action.setCheckable(True)
            action.setChecked(not self._dump_table.isColumnHidden(column))
            action.toggled.connect(
                lambda checked, column=column: self._set_dump_column_visible(column, checked)
            )
            self._dump_column_actions.append(action)
        return menu

    def _build_display_menu(self, parent) -> QtWidgets.QMenu:
        menu = QtWidgets.QMenu(parent)
        self._display_action_groups = []

        def add_action(
            text: str,
            handler,
            groups: set[str] | None = None,
            *,
            include_in_digits: bool = True,
        ) -> None:
            action = menu.addAction(text)
            action.triggered.connect(handler)
            self._display_action_groups.append((action, groups, include_in_digits))

        add_action("DICOM Window", self._set_display_dicom)
        add_action("Min-Max", self._set_display_min_max)
        menu.addSeparator()
        add_action(
            "Enter Window/Level…",
            self._prompt_window_level_input,
            include_in_digits=False,
        )
        add_action(
            "Enter Min/Max…",
            self._prompt_min_max_input,
            include_in_digits=False,
        )
        menu.addSeparator()
        add_action(
            "CT Lung",
            lambda: self._set_display_window_preset("Lung", -600.0, 1500.0),
            {"ct"},
        )
        add_action(
            "CT Mediastinum",
            lambda: self._set_display_window_preset("Mediastinum", 50.0, 350.0),
            {"ct"},
        )
        add_action(
            "CT Bone",
            lambda: self._set_display_window_preset("Bone", 300.0, 1500.0),
            {"ct"},
        )
        add_action(
            "CT Brain",
            lambda: self._set_display_window_preset("Brain", 40.0, 80.0),
            {"ct"},
        )
        menu.addSeparator()
        add_action(
            "MR P10-P90",
            lambda: self._set_display_percentile_preset(
                "P10-P90", 10.0, 90.0, clamp_min_zero=False
            ),
            {"mr"},
        )
        add_action(
            "MR P20-P80",
            lambda: self._set_display_percentile_preset(
                "P20-P80", 20.0, 80.0, clamp_min_zero=False
            ),
            {"mr"},
        )
        add_action(
            "MR P5-P95",
            lambda: self._set_display_percentile_preset(
                "P5-P95", 5.0, 95.0, clamp_min_zero=False
            ),
            {"mr"},
        )
        menu.addSeparator()
        add_action(
            "PT/NM 0-P90",
            lambda: self._set_display_percentile_preset(
                "0-P90", 0.0, 90.0, clamp_min_zero=True
            ),
            {"nuclear"},
        )
        add_action(
            "PT/NM 0-P80",
            lambda: self._set_display_percentile_preset(
                "0-P80", 0.0, 80.0, clamp_min_zero=True
            ),
            {"nuclear"},
        )
        add_action(
            "PT/NM 0-Max",
            lambda: self._set_display_percentile_preset(
                "0-Max", 0.0, 100.0, clamp_min_zero=True
            ),
            {"nuclear"},
        )
        return menu

    def _show_shortcuts_dialog(self) -> None:
        dialog = self._ensure_shortcuts_dialog()
        dialog.show()
        dialog.raise_()
        dialog.activateWindow()

    def _ensure_shortcuts_dialog(self) -> QtWidgets.QDialog:
        if self._shortcuts_dialog is None:
            self._shortcuts_dialog = self._create_shortcuts_dialog()
        return self._shortcuts_dialog

    def _create_shortcuts_dialog(self) -> QtWidgets.QDialog:
        dialog = QtWidgets.QDialog(self)
        dialog.setWindowTitle("Help")
        dialog.setModal(False)
        dialog.resize(700, 620)

        layout = QtWidgets.QVBoxLayout(dialog)
        layout.setContentsMargins(20, 18, 20, 18)
        layout.setSpacing(14)

        title = QtWidgets.QLabel("Help", dialog)
        title.setStyleSheet("color: #f4f7fb; font-size: 16px; font-weight: 700;")
        layout.addWidget(title)

        hint = QtWidgets.QLabel(
            "Display digit shortcuts follow the current Display menu order for preset items. "
            "`0` is Auto, `1` is the first active preset, `2..9` continue in order. "
            "Manual input actions are not part of the digit shortcuts.",
            dialog,
        )
        hint.setWordWrap(True)
        hint.setStyleSheet("color: #9aa6b8; font-size: 12px;")
        layout.addWidget(hint)

        scroll = QtWidgets.QScrollArea(dialog)
        scroll.setWidgetResizable(True)
        scroll.setFrameShape(QtWidgets.QFrame.Shape.NoFrame)
        scroll.setHorizontalScrollBarPolicy(QtCore.Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        layout.addWidget(scroll, 1)

        content = QtWidgets.QWidget(scroll)
        scroll.setWidget(content)
        content_layout = QtWidgets.QVBoxLayout(content)
        content_layout.setContentsMargins(0, 0, 0, 0)
        content_layout.setSpacing(18)

        settings_path = self._settings.fileName() or "(unknown)"
        settings_override = os.environ.get("DICOMVIEW_SETTINGS_PATH", "").strip()

        sections = [
            (
                "Navigation",
                [
                    ("Left / Right", "Previous or next file"),
                    ("Enter / Return", "Open selected file or enter selected folder"),
                    ("Alt+Up / Backspace", "Go to parent folder"),
                ],
            ),
            (
                "Frames",
                [
                    ("PageUp / PageDown", "Previous or next frame"),
                    ("Space", "Play or pause cine"),
                    ("First / Last", "Jump to the first or last frame"),
                ],
            ),
            (
                "View",
                [
                    ("F", "Fit image to window"),
                    ("Ctrl+1", "Show image at 100%"),
                    ("R", "Reset zoom"),
                    ("Ctrl+Wheel", "Zoom in or out"),
                ],
            ),
            (
                "Display",
                [
                    ("0", "Auto display"),
                    ("1..9", "Display presets in current menu order"),
                    ("Display menu", "Enter Window/Level or Min/Max directly"),
                    ("Right drag", "Adjust display; CT/MR = center/width, PT/NM = 0-max"),
                ],
            ),
            (
                "Table",
                [
                    ("Header click", "Sort by column"),
                    ("Header drag", "Reorder columns"),
                    ("Divider drag", "Resize column width"),
                    ("Divider double-click", "Auto-fit column"),
                ],
            ),
            (
                "Inspector",
                [
                    ("Details / Dump", "Switch between summary metadata and structured dicomdump rows"),
                    ("Dump filter", "Show only dump rows that contain the filter text"),
                ],
            ),
            (
                "Help",
                [
                    ("F1", "Open this help popup"),
                    ("Esc", "Close this help popup"),
                ],
            ),
            (
                "Settings",
                [
                    ("Current file", settings_path),
                    (
                        "Override",
                        "Set DICOMVIEW_SETTINGS_PATH=/path/to/dicomview.ini to use a different settings file.",
                    ),
                ]
                + (
                    [("Env value", settings_override)]
                    if settings_override
                    else []
                ),
            ),
        ]

        key_font = fixed_ui_font(11.0)
        for section_title, rows in sections:
            section_widget = QtWidgets.QWidget(content)
            section_layout = QtWidgets.QVBoxLayout(section_widget)
            section_layout.setContentsMargins(0, 0, 0, 0)
            section_layout.setSpacing(8)

            header = QtWidgets.QLabel(section_title, section_widget)
            header.setStyleSheet("color: #dfe7f3; font-size: 13px; font-weight: 700;")
            section_layout.addWidget(header)

            grid = QtWidgets.QGridLayout()
            grid.setContentsMargins(0, 0, 0, 0)
            grid.setHorizontalSpacing(14)
            grid.setVerticalSpacing(8)
            section_layout.addLayout(grid)

            for row, (shortcut, description) in enumerate(rows):
                key_label = QtWidgets.QLabel(shortcut, section_widget)
                key_label.setFont(key_font)
                key_label.setStyleSheet(
                    "color: #f5f8fc; background: #1b2129; border: 1px solid #2f3742; "
                    "border-radius: 7px; padding: 4px 8px;"
                )
                desc_label = QtWidgets.QLabel(description, section_widget)
                desc_label.setWordWrap(True)
                desc_label.setStyleSheet("color: #b7c1cf; font-size: 12px;")
                grid.addWidget(key_label, row, 0)
                grid.addWidget(desc_label, row, 1)
            grid.setColumnStretch(1, 1)
            content_layout.addWidget(section_widget)

        content_layout.addStretch(1)

        button_row = QtWidgets.QHBoxLayout()
        button_row.addStretch(1)
        close_button = QtWidgets.QPushButton("Close", dialog)
        close_button.clicked.connect(dialog.close)
        button_row.addWidget(close_button)
        layout.addLayout(button_row)

        dialog.setStyleSheet(
            """
            QDialog {
                background: #161b21;
                color: #eef2f7;
            }
            QScrollArea {
                background: transparent;
            }
            QPushButton {
                background: #212833;
                border: 1px solid #2f3948;
                border-radius: 8px;
                color: #e4eaf3;
                font-weight: 600;
                padding: 7px 14px;
            }
            QPushButton:hover {
                background: #283240;
                border-color: #3a485a;
            }
            QPushButton:pressed {
                background: #334153;
            }
            """
        )
        return dialog

    def _build_info_panel(self) -> QtWidgets.QWidget:
        panel = QtWidgets.QWidget(self)
        panel.setObjectName("dicomviewInfoPanel")
        panel.setMinimumWidth(320)

        layout = QtWidgets.QVBoxLayout(panel)
        layout.setContentsMargins(18, 18, 18, 18)
        layout.setSpacing(10)

        self._info_tabs = QtWidgets.QTabWidget(panel)
        self._info_tabs.setObjectName("dicomviewInfoTabs")
        self._info_tabs.setDocumentMode(False)
        self._info_tabs.tabBar().setDrawBase(False)
        self._info_tabs.tabBar().setExpanding(False)
        self._info_tabs.tabBar().setUsesScrollButtons(False)
        layout.addWidget(self._info_tabs, 1)

        self._details_tab = QtWidgets.QWidget(self._info_tabs)
        self._details_tab.setObjectName("dicomviewDetailsTab")
        self._info_tabs.addTab(self._details_tab, "Details")

        details_layout = QtWidgets.QVBoxLayout(self._details_tab)
        details_layout.setContentsMargins(0, 0, 0, 0)
        details_layout.setSpacing(12)

        details_card = QtWidgets.QFrame(self._details_tab)
        details_card.setObjectName("dicomviewDetailsCard")
        details_layout.addWidget(details_card)
        details_layout.addStretch(1)

        details_card_layout = QtWidgets.QVBoxLayout(details_card)
        details_card_layout.setContentsMargins(14, 14, 14, 14)
        details_card_layout.setSpacing(14)

        sections = (
            ("Study", ("File", "Study", "Series", "Date")),
            ("Patient", ("Patient Name", "Patient ID")),
            ("DICOM", ("Modality", "SOP Class", "Transfer Syntax")),
            (
                "Pixel",
                (
                    "Size",
                    "Frame",
                    "Photometric",
                    "Window/Level",
                    "Rescale",
                    "Pixel Repr",
                    "Stored Bits",
                    "Stored Value Range",
                ),
            ),
        )

        for section_title, keys in sections:
            section_label = QtWidgets.QLabel(section_title, details_card)
            section_label.setObjectName("dicomviewInfoSection")
            details_card_layout.addWidget(section_label)

            form = QtWidgets.QFormLayout()
            form.setContentsMargins(0, 0, 0, 0)
            form.setHorizontalSpacing(14)
            form.setVerticalSpacing(12)
            form.setFieldGrowthPolicy(
                QtWidgets.QFormLayout.FieldGrowthPolicy.ExpandingFieldsGrow
            )
            form.setRowWrapPolicy(
                QtWidgets.QFormLayout.RowWrapPolicy.WrapLongRows
            )
            form.setLabelAlignment(
                QtCore.Qt.AlignmentFlag.AlignLeft | QtCore.Qt.AlignmentFlag.AlignTop
            )
            form.setFormAlignment(QtCore.Qt.AlignmentFlag.AlignTop)
            details_card_layout.addLayout(form)

            for key in keys:
                key_label = QtWidgets.QLabel(f"{key}", panel)
                key_label.setObjectName("dicomviewInfoKey")
                value_label = QtWidgets.QLabel("-")
                value_label.setObjectName("dicomviewInfoValue")
                value_label.setWordWrap(True)
                value_label.setAlignment(
                    QtCore.Qt.AlignmentFlag.AlignLeft | QtCore.Qt.AlignmentFlag.AlignTop
                )
                value_label.setSizePolicy(
                    QtWidgets.QSizePolicy.Policy.Expanding,
                    QtWidgets.QSizePolicy.Policy.Preferred,
                )
                value_label.setTextInteractionFlags(
                    QtCore.Qt.TextInteractionFlag.TextSelectableByMouse
                )
                self._info_labels[key] = value_label
                form.addRow(key_label, value_label)

        self._dump_tab = QtWidgets.QWidget(self._info_tabs)
        self._dump_tab.setObjectName("dicomviewDumpTab")
        self._info_tabs.addTab(self._dump_tab, "Dump")

        dump_layout = QtWidgets.QVBoxLayout(self._dump_tab)
        dump_layout.setContentsMargins(0, 0, 0, 0)
        dump_layout.setSpacing(10)

        dump_toolbar = QtWidgets.QHBoxLayout()
        dump_toolbar.setContentsMargins(0, 0, 0, 0)
        dump_toolbar.setSpacing(8)
        dump_layout.addLayout(dump_toolbar)

        self._dump_filter_edit.setObjectName("dicomviewDumpFilter")
        self._dump_filter_edit.setPlaceholderText("Filter dump rows")
        self._dump_filter_edit.setClearButtonEnabled(True)
        self._dump_filter_edit.setEnabled(False)
        dump_toolbar.addWidget(self._dump_filter_edit, 1)

        self._dump_columns_button.setObjectName("dicomviewDumpColumnsButton")
        self._dump_columns_button.setText("Columns")
        self._dump_columns_button.setToolTip("Show or hide dump columns")
        self._dump_columns_button.setPopupMode(
            QtWidgets.QToolButton.ToolButtonPopupMode.InstantPopup
        )
        self._dump_columns_button.setToolButtonStyle(
            QtCore.Qt.ToolButtonStyle.ToolButtonTextBesideIcon
        )
        self._dump_columns_button.setLayoutDirection(QtCore.Qt.LayoutDirection.RightToLeft)
        self._dump_columns_button.setIcon(
            self.style().standardIcon(QtWidgets.QStyle.StandardPixmap.SP_ArrowDown)
        )
        self._dump_columns_button.setEnabled(False)
        dump_toolbar.addWidget(self._dump_columns_button)

        self._dump_stack.setObjectName("dicomviewDumpStack")
        dump_layout.addWidget(self._dump_stack, 1)

        self._dump_message_label.setObjectName("dicomviewDumpMessage")
        self._dump_message_label.setWordWrap(True)
        self._dump_message_label.setAlignment(
            QtCore.Qt.AlignmentFlag.AlignCenter | QtCore.Qt.AlignmentFlag.AlignVCenter
        )
        self._dump_stack.addWidget(self._dump_message_label)

        self._dump_proxy.setSourceModel(self._dump_model)
        self._dump_table.setObjectName("dicomviewDumpTable")
        self._dump_table.setModel(self._dump_proxy)
        self._dump_table.setEditTriggers(QtWidgets.QAbstractItemView.EditTrigger.NoEditTriggers)
        self._dump_table.setSelectionBehavior(
            QtWidgets.QAbstractItemView.SelectionBehavior.SelectRows
        )
        self._dump_table.setSelectionMode(
            QtWidgets.QAbstractItemView.SelectionMode.SingleSelection
        )
        self._dump_table.setAlternatingRowColors(True)
        self._dump_table.setWordWrap(False)
        self._dump_table.setShowGrid(False)
        self._dump_table.setSortingEnabled(False)
        self._dump_table.verticalHeader().setVisible(False)
        self._dump_table.horizontalHeader().setStretchLastSection(False)
        self._dump_table.horizontalHeader().setSectionResizeMode(
            QtWidgets.QHeaderView.ResizeMode.Interactive
        )
        self._dump_table.horizontalHeader().setMinimumSectionSize(48)
        self._dump_table.horizontalHeader().setToolTip(
            "Drag the divider to resize dump columns."
        )
        self._dump_table.setHorizontalScrollBarPolicy(
            QtCore.Qt.ScrollBarPolicy.ScrollBarAsNeeded
        )
        self._dump_table.setHorizontalScrollMode(
            QtWidgets.QAbstractItemView.ScrollMode.ScrollPerPixel
        )
        self._dump_table.setVerticalScrollMode(
            QtWidgets.QAbstractItemView.ScrollMode.ScrollPerPixel
        )
        self._dump_stack.addWidget(self._dump_table)
        self._set_dump_message("Dump is available for DICOM files only.")
        return panel

    def _build_viewer_panel(self) -> QtWidgets.QWidget:
        panel = QtWidgets.QWidget(self)
        panel.setObjectName("dicomviewViewerPanel")
        layout = QtWidgets.QVBoxLayout(panel)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(10)
        layout.addWidget(self._canvas, 1)

        self._frame_bar.setObjectName("dicomviewFrameBar")
        frame_layout = QtWidgets.QHBoxLayout(self._frame_bar)
        frame_layout.setContentsMargins(14, 12, 14, 12)
        frame_layout.setSpacing(10)
        frame_title = QtWidgets.QLabel("Frames", self._frame_bar)
        frame_title.setObjectName("dicomviewFrameTitle")
        frame_layout.addWidget(frame_title)
        self._frame_first_button.setText("First")
        self._frame_first_button.setToolTip("Go to first frame")
        self._frame_play_button.setText("Play")
        self._frame_play_button.setToolTip("Play or pause cine")
        self._frame_last_button.setText("Last")
        self._frame_last_button.setToolTip("Go to last frame")
        self._frame_status_label.setObjectName("dicomviewFrameStatus")
        frame_layout.addWidget(self._frame_first_button)
        frame_layout.addWidget(self._frame_play_button)
        frame_layout.addWidget(self._frame_scroll, 1)
        frame_layout.addWidget(self._frame_last_button)
        frame_layout.addWidget(self._frame_status_label)

        self._frame_scroll.setMinimum(1)
        self._frame_scroll.setMaximum(1)
        self._frame_scroll.setSingleStep(1)
        self._frame_scroll.setPageStep(1)
        self._frame_first_button.setEnabled(False)
        self._frame_play_button.setEnabled(False)
        self._frame_last_button.setEnabled(False)
        self._frame_bar.setVisible(False)
        layout.addWidget(self._frame_bar)
        return panel

    def _apply_theme(self) -> None:
        general_font = general_ui_font(12.0)
        self.setFont(general_font)

        mono_font = fixed_ui_font(11.0)
        self._path_edit.setFont(mono_font)

        status_font = QtGui.QFont(mono_font)
        status_font.setPointSizeF(max(10.0, mono_font.pointSizeF() - 0.5))
        self.statusBar().setFont(status_font)
        self.statusBar().setSizeGripEnabled(False)

        toolbar_font = general_ui_font(11.0)
        toolbar_font.setWeight(QtGui.QFont.Weight.DemiBold)
        if self._toolbar is not None:
            self._toolbar.setFont(toolbar_font)
        if self._columns_button is not None:
            self._columns_button.setFont(toolbar_font)
            self._columns_button.setIconSize(QtCore.QSize(10, 10))
        if self._display_button is not None:
            self._display_button.setFont(toolbar_font)
            self._display_button.setIconSize(QtCore.QSize(10, 10))
        if self._dump_columns_button is not None:
            self._dump_columns_button.setFont(toolbar_font)
            self._dump_columns_button.setIconSize(QtCore.QSize(10, 10))
        if self._dump_table is not None:
            self._dump_table.setFont(mono_font)

        header_font = QtGui.QFont(general_font)
        header_font.setPointSizeF(max(11.0, general_font.pointSizeF() - 0.5))
        header_font.setWeight(QtGui.QFont.Weight.DemiBold)
        self._table.horizontalHeader().setFont(header_font)
        self._table.horizontalHeader().setDefaultAlignment(
            QtCore.Qt.AlignmentFlag.AlignLeft | QtCore.Qt.AlignmentFlag.AlignVCenter
        )

        table_row_height = 38
        self._table.verticalHeader().setDefaultSectionSize(table_row_height)
        self._table.verticalHeader().setMinimumSectionSize(table_row_height)
        self._table.setMinimumWidth(460)
        self._table.horizontalHeader().setMinimumSectionSize(84)

        if self._toolbar is not None:
            self._toolbar.setIconSize(QtCore.QSize(16, 16))

        if self._splitter is not None:
            self._splitter.setHandleWidth(8)

        self.setStyleSheet(
            """
            QMainWindow#dicomviewWindow {
                background: #14181d;
                color: #eef2f7;
            }
            QWidget#dicomviewCentral {
                background: #14181d;
            }
            QToolBar#dicomviewToolbar {
                background: #1a1f25;
                border: none;
                border-bottom: 1px solid #262d36;
                spacing: 6px;
                padding: 8px 10px 7px 10px;
            }
            QToolBar#dicomviewToolbar::separator {
                width: 1px;
                margin: 6px 8px;
                background: #2a313b;
            }
            QToolBar#dicomviewToolbar QToolButton {
                background: transparent;
                border: 1px solid transparent;
                border-radius: 9px;
                color: #d8deea;
                font-size: 11px;
                font-weight: 600;
                padding: 7px 10px 7px 10px;
            }
            QToolBar#dicomviewToolbar QToolButton:hover,
            QToolButton#dicomviewColumnsButton:hover,
            QToolButton#dicomviewDisplayButton:hover {
                background: #252c35;
                border-color: #303947;
            }
            QToolBar#dicomviewToolbar QToolButton:pressed,
            QToolButton#dicomviewColumnsButton:pressed,
            QToolButton#dicomviewDisplayButton:pressed {
                background: #2d3642;
            }
            QToolButton#dicomviewColumnsButton,
            QToolButton#dicomviewDisplayButton,
            QToolButton#dicomviewDumpColumnsButton {
                background: transparent;
                border: 1px solid transparent;
                border-radius: 9px;
                color: #d8deea;
                font-size: 11px;
                font-weight: 600;
                padding: 7px 10px 7px 12px;
                qproperty-autoRaise: false;
            }
            QToolButton#dicomviewColumnsButton::menu-indicator,
            QToolButton#dicomviewDisplayButton::menu-indicator,
            QToolButton#dicomviewDumpColumnsButton::menu-indicator {
                image: none;
                width: 0px;
            }
            QToolButton#dicomviewColumnsButton::icon,
            QToolButton#dicomviewDisplayButton::icon,
            QToolButton#dicomviewDumpColumnsButton::icon {
                padding-left: 4px;
            }
            QLineEdit#dicomviewPathBar {
                background: #0f1216;
                border: 1px solid #29313b;
                border-radius: 11px;
                color: #f2f5fa;
                padding: 9px 12px;
                selection-background-color: #365f96;
            }
            QTableView#dicomviewTable {
                background: #181d23;
                alternate-background-color: #1d232b;
                border: 1px solid #2a313b;
                border-radius: 14px;
                color: #eef2f7;
                gridline-color: transparent;
                outline: none;
                selection-background-color: #1d4d88;
                selection-color: #f8fbff;
            }
            QTableView#dicomviewTable::item {
                border: none;
                padding: 8px 12px;
            }
            QTableView#dicomviewTable::item:hover {
                background: #222933;
            }
            QHeaderView::section {
                background: #20262e;
                border: none;
                border-bottom: 1px solid #2f3742;
                border-right: 1px solid #313a46;
                color: #a9b3c3;
                padding: 10px 26px 10px 12px;
            }
            QHeaderView::section:hover {
                background: #27303a;
            }
            QTableCornerButton::section {
                background: #20262e;
                border: none;
            }
            QWidget#dicomviewInfoPanel {
                background: #1a2027;
                border: 1px solid #2b3340;
                border-radius: 14px;
            }
            QFrame#dicomviewDetailsCard {
                background: #171c22;
                border: 1px solid #2b3340;
                border-radius: 12px;
            }
            QTabWidget#dicomviewInfoTabs::tab-bar {
                left: 0px;
            }
            QTabWidget#dicomviewInfoTabs::pane {
                border: none;
                background: transparent;
                top: 6px;
            }
            QTabWidget#dicomviewInfoTabs QTabBar {
                background: transparent;
            }
            QTabWidget#dicomviewInfoTabs QTabBar::tab {
                background: #151b21;
                border: 1px solid #2b3340;
                border-radius: 9px;
                color: #8f9aaa;
                font-size: 12px;
                font-weight: 700;
                padding: 7px 12px;
                margin-right: 6px;
                margin-top: 2px;
            }
            QTabWidget#dicomviewInfoTabs QTabBar::tab:selected {
                background: #222933;
                border-color: #35404f;
                color: #eef2f7;
                margin-top: 0px;
            }
            QTabWidget#dicomviewInfoTabs QTabBar::tab:hover:!selected {
                background: #1c232b;
                color: #c7d0dd;
            }
            QLabel#dicomviewInfoKey {
                color: #8f9aaa;
                font-size: 11px;
                font-weight: 700;
                padding-top: 2px;
            }
            QLabel#dicomviewInfoSection {
                color: #c9d4e3;
                font-size: 12px;
                font-weight: 800;
                letter-spacing: 0.3px;
                padding-top: 4px;
                padding-bottom: 2px;
            }
            QLabel#dicomviewInfoValue {
                color: #edf2f8;
                font-size: 13px;
                font-weight: 500;
            }
            QLineEdit#dicomviewDumpFilter {
                background: #11161b;
                border: 1px solid #2b3340;
                border-radius: 10px;
                color: #eef2f7;
                padding: 8px 11px;
                selection-background-color: #365f96;
            }
            QLabel#dicomviewDumpMessage {
                background: #11161b;
                border: 1px solid #2b3340;
                border-radius: 12px;
                color: #bfc8d6;
                padding: 10px 12px;
                font-size: 12px;
            }
            QTableView#dicomviewDumpTable {
                background: #11161b;
                alternate-background-color: #141a20;
                border: 1px solid #2b3340;
                border-radius: 12px;
                color: #eef2f7;
                gridline-color: transparent;
                outline: none;
                selection-background-color: #24456d;
                selection-color: #f8fbff;
            }
            QTableView#dicomviewDumpTable::item {
                border: none;
                padding: 6px 8px;
            }
            QTableView#dicomviewDumpTable::item:hover {
                background: #18212a;
            }
            QTableView#dicomviewDumpTable QHeaderView::section {
                background: #1a2027;
                border: none;
                border-bottom: 1px solid #2f3742;
                border-right: 1px solid #313a46;
                color: #a9b3c3;
                padding: 8px 10px;
            }
            QWidget#dicomviewFrameBar {
                background: #171c22;
                border: 1px solid #2b3340;
                border-radius: 12px;
            }
            QLabel#dicomviewFrameTitle {
                color: #98a3b4;
                font-weight: 700;
            }
            QLabel#dicomviewFrameStatus {
                color: #edf2f8;
                font-weight: 700;
                min-width: 64px;
            }
            QWidget#dicomviewFrameBar QToolButton {
                background: #212833;
                border: 1px solid #2f3948;
                border-radius: 8px;
                color: #e4eaf3;
                font-weight: 600;
                padding: 6px 10px;
            }
            QWidget#dicomviewFrameBar QToolButton:hover {
                background: #283240;
                border-color: #3a485a;
            }
            QWidget#dicomviewFrameBar QToolButton:pressed {
                background: #334153;
            }
            QScrollBar:horizontal {
                background: #111519;
                height: 12px;
                margin: 0;
                border-radius: 6px;
            }
            QScrollBar::handle:horizontal {
                background: #526174;
                min-width: 28px;
                border-radius: 6px;
            }
            QScrollBar::handle:horizontal:hover {
                background: #617288;
            }
            QScrollBar::add-line:horizontal,
            QScrollBar::sub-line:horizontal,
            QScrollBar::add-page:horizontal,
            QScrollBar::sub-page:horizontal {
                background: none;
                border: none;
            }
            QScrollBar:vertical {
                background: #111519;
                width: 12px;
                margin: 0;
                border-radius: 6px;
            }
            QScrollBar::handle:vertical {
                background: #526174;
                min-height: 28px;
                border-radius: 6px;
            }
            QScrollBar::handle:vertical:hover {
                background: #617288;
            }
            QScrollBar::add-line:vertical,
            QScrollBar::sub-line:vertical,
            QScrollBar::add-page:vertical,
            QScrollBar::sub-page:vertical {
                background: none;
                border: none;
            }
            QGraphicsView {
                background: #0f1115;
                border: 1px solid #2a313b;
                border-radius: 14px;
            }
            QStatusBar {
                background: #12161b;
                border-top: 1px solid #252c35;
                color: #9ca7b6;
            }
            QStatusBar::item {
                border: none;
            }
            QSplitter#dicomviewMainSplitter::handle {
                background: #101419;
            }
            QMenu {
                background: #191e24;
                border: 1px solid #2c3440;
                border-radius: 10px;
                color: #e5ebf3;
                padding: 6px;
            }
            QMenu::item {
                border-radius: 7px;
                padding: 7px 26px 7px 10px;
            }
            QMenu::item:selected {
                background: #27446b;
            }
            """
        )

    @staticmethod
    def _default_column_widths() -> dict[int, int]:
        return {
            0: 188,
            1: 232,
            2: 316,
            3: 108,
            4: 110,
            5: 248,
            6: 84,
            7: 170,
        }

    @staticmethod
    def _default_dump_column_widths() -> dict[int, int]:
        return {
            0: 124,
            1: 56,
            2: 72,
            3: 52,
            4: 92,
            5: 720,
            6: 320,
        }

    @staticmethod
    def _default_splitter_sizes() -> list[int]:
        return [620, 940, 300]

    @staticmethod
    def _read_int_list(value) -> list[int]:
        if value is None:
            return []
        if isinstance(value, (list, tuple)):
            result = []
            for item in value:
                try:
                    result.append(int(item))
                except (TypeError, ValueError):
                    return []
            return result
        text = str(value).strip()
        if not text:
            return []
        parts = [part.strip() for part in text.split(",")]
        result = []
        for part in parts:
            if not part:
                continue
            try:
                result.append(int(part))
            except ValueError:
                return []
        return result

    def _connect_signals(self) -> None:
        selection_model = self._table.selectionModel()
        selection_model.currentRowChanged.connect(self._on_current_row_changed)
        self._table.doubleClicked.connect(self._on_table_activated)
        self._table.activated.connect(self._on_table_activated)
        self._frame_scroll.valueChanged.connect(self._on_frame_scroll_changed)
        self._frame_first_button.clicked.connect(self._go_first_frame)
        self._frame_play_button.clicked.connect(self._toggle_playback)
        self._frame_last_button.clicked.connect(self._go_last_frame)
        self._cine_timer.timeout.connect(self._advance_playback_frame)
        self._canvas.display_drag_started.connect(self._start_display_drag)
        self._canvas.display_dragged.connect(self._update_display_drag)
        self._canvas.display_drag_finished.connect(self._finish_display_drag)
        self._table.horizontalHeader().sectionActivated.connect(
            self._on_header_section_clicked
        )
        self._table.horizontalHeader().sectionMoved.connect(self._queue_save_settings)
        self._table.horizontalHeader().sectionResized.connect(self._queue_save_settings)
        self._table.horizontalHeader().sectionHandleDoubleClicked.connect(
            self._fit_column_to_contents
        )
        self._info_tabs.currentChanged.connect(self._on_info_tab_changed)
        self._dump_filter_edit.textChanged.connect(self._on_dump_filter_changed)
        self._dump_table.horizontalHeader().sectionResized.connect(
            self._on_dump_column_resized
        )
        self._splitter.splitterMoved.connect(self._queue_save_settings)
        self._splitter.splitterMoved.connect(self._refresh_info_label_heights)
        self._settings_save_timer.timeout.connect(self._save_settings)

    def _refresh_info_label_heights(self, *args) -> None:
        del args
        for label in self._info_labels.values():
            _sync_wrapped_label_height(label)

    def _open_folder_dialog(self) -> None:
        selected = QtWidgets.QFileDialog.getExistingDirectory(
            self,
            "Open Folder",
            str(self._state.current_dir),
        )
        if selected:
            self.open_location(Path(selected))

    def _open_file_dialog(self) -> None:
        selected, _ = QtWidgets.QFileDialog.getOpenFileName(
            self,
            "Open DICOM File",
            str(self._state.current_dir),
        )
        if selected:
            self.open_location(Path(selected))

    def _go_up(self) -> None:
        current_dir = self._state.current_dir
        parent = current_dir.parent
        if parent != current_dir:
            self.open_location(parent, pending_selection=current_dir)

    def _refresh(self) -> None:
        self.open_location(self._state.current_dir)

    def open_location(
        self,
        path: Path,
        *,
        pending_selection: Path | None = None,
    ) -> None:
        path = path.expanduser()
        self._stop_playback()
        if not path.exists():
            self._stop_scan_worker()
            self._canvas.show_error_message(
                "Path not found",
                str(path),
            )
            self.statusBar().showMessage(f"Path does not exist: {path}")
            return

        if path.is_dir():
            directory = path.resolve()
            if pending_selection is not None:
                pending_selection = pending_selection.resolve()
        else:
            directory = path.resolve().parent
            pending_selection = path.resolve()

        self._state.current_dir = directory
        self._pending_selection = pending_selection
        self._state.selected_file = None
        self._state.current_frame = 0
        self._state.display = DisplaySettings.auto()
        self._current_dicom_file = None
        self._current_dicom_path = None
        self._last_rendered_frame = None

        self._stop_playback()
        self._settings.setValue("browser/last_path", str(directory))
        self._path_edit.setText(str(directory))
        self._path_edit.setCursorPosition(0)
        self._canvas.clear()
        self._set_frame_controls(1, 0)
        self._update_info_panel(None, None)
        self._set_dump_source(None)
        self._update_display_controls(None)
        self._start_scan(directory)

    def _start_scan(self, directory: Path) -> None:
        self._stop_scan_worker()
        self._scan_id += 1
        self._model.clear()
        self._scan_thread = QtCore.QThread(self)
        self._scan_worker = FolderScanWorker(self._scan_id, directory)
        self._scan_worker.moveToThread(self._scan_thread)

        self._scan_thread.started.connect(self._scan_worker.run)
        self._scan_worker.entry_ready.connect(self._on_scan_entry_ready)
        self._scan_worker.scan_failed.connect(self._on_scan_failed)
        self._scan_worker.scan_finished.connect(self._on_scan_finished)
        self._scan_worker.scan_finished.connect(self._scan_thread.quit)
        self._scan_worker.scan_finished.connect(self._scan_worker.deleteLater)
        self._scan_thread.finished.connect(self._scan_thread.deleteLater)
        self._scan_thread.start()
        self.statusBar().showMessage(f"Scanning {directory} ...")

    def _stop_scan_worker(self) -> None:
        if self._scan_worker is not None:
            self._scan_worker.cancel()
        if self._scan_thread is not None:
            self._scan_thread.quit()
            self._scan_thread.wait(1000)
        self._scan_thread = None
        self._scan_worker = None

    def _on_scan_entry_ready(self, scan_id: int, entry) -> None:
        if scan_id != self._scan_id:
            return
        self._model.append_entry(entry)
        row = self._model.rowCount() - 1
        if (
            self._pending_selection is not None
            and getattr(entry, "path", None) == self._pending_selection
        ):
            self._pending_selection = None
            self._select_row(row)
            return

    def _on_scan_failed(self, scan_id: int, message: str) -> None:
        if scan_id != self._scan_id:
            return
        self._canvas.show_error_message(
            "Failed to scan folder",
            message,
        )
        self.statusBar().showMessage(f"Scan failed: {message}")

    def _on_scan_finished(self, scan_id: int, directory_text: str) -> None:
        if scan_id != self._scan_id:
            return
        header = self._table.horizontalHeader()
        self._apply_sort(
            header.sortIndicatorSection(),
            header.sortIndicatorOrder(),
            preserve_selection=self._state.selected_file is not None,
            update_indicator=False,
        )
        if self._pending_selection is not None and self._state.selected_file is None:
            pending_path = self._pending_selection
            self._pending_selection = None
            if pending_path.is_file():
                self._show_non_dicom_file(pending_path)
                self.statusBar().showMessage(f"Not a DICOM file: {pending_path.name}")
                self._scan_worker = None
                self._scan_thread = None
                return
        if self._state.selected_file is None and not self._table.currentIndex().isValid():
            first_row = self._model.first_file_row()
            if first_row >= 0:
                self._select_row(first_row)
        self._pending_selection = None
        if self._state.selected_file is not None and self._last_rendered_frame is not None:
            row = self._model.row_of_path(self._state.selected_file)
            entry = self._model.entry_at(row)
            if isinstance(entry, DicomFileEntry):
                self._update_status(entry, self._last_rendered_frame)
            else:
                self.statusBar().showMessage(f"Ready: {directory_text}")
        else:
            self.statusBar().showMessage(f"Ready: {directory_text}")
        self._scan_worker = None
        self._scan_thread = None

    def _select_row(self, row: int) -> None:
        if row < 0:
            return
        index = self._model.index(row, 0)
        self._table.selectionModel().setCurrentIndex(
            index,
            QtCore.QItemSelectionModel.SelectionFlag.ClearAndSelect
            | QtCore.QItemSelectionModel.SelectionFlag.Rows,
        )
        self._table.scrollTo(index, QtWidgets.QAbstractItemView.ScrollHint.PositionAtCenter)

    def _on_current_row_changed(self, current, previous) -> None:
        del previous
        entry = self._model.entry_at(current.row())
        if isinstance(entry, DicomFileEntry):
            self._stop_playback()
            is_new_file = self._state.selected_file != entry.path
            self._state.selected_file = entry.path
            self._state.current_frame = 0
            if is_new_file:
                self._state.display = DisplaySettings.auto()
            self._load_selected_entry(entry)
        elif isinstance(entry, FolderEntry):
            self._stop_playback()
            self._state.selected_file = None
            self._state.current_frame = 0
            self._state.display = DisplaySettings.auto()
            self._current_dicom_file = None
            self._current_dicom_path = None
            self._last_rendered_frame = None
            self._canvas.clear()
            self._set_frame_controls(1, 0)
            self._update_info_panel(entry, None)
            self._set_dump_source(None)
            self._update_display_controls(None)
            self.statusBar().showMessage(f"{self._state.current_dir} | selected folder {entry.path.name}")

    def _on_table_activated(self, index) -> None:
        entry = self._model.entry_at(index.row())
        if isinstance(entry, FolderEntry):
            if entry.is_parent:
                self.open_location(entry.path, pending_selection=self._state.current_dir)
            else:
                self.open_location(entry.path)
        elif isinstance(entry, DicomFileEntry):
            self._load_selected_entry(entry)

    def _activate_current_selection(self) -> None:
        index = self._table.currentIndex()
        if not index.isValid():
            return
        self._on_table_activated(index)

    def _set_column_visible(self, column: int, visible: bool) -> None:
        self._table.setColumnHidden(column, not visible)
        self._queue_save_settings()

    def _set_dump_column_visible(self, column: int, visible: bool) -> None:
        self._dump_table.setColumnHidden(column, not visible)
        self._pending_dump_hidden[column] = not visible
        self._apply_dump_spans()
        self._sync_dump_column_actions()
        self._queue_save_settings()

    def _on_dump_column_resized(self, logical_index: int, _old_size: int, new_size: int) -> None:
        self._pending_dump_widths[logical_index] = new_size
        self._queue_save_settings()

    def _fit_column_to_contents(self, logical_index: int) -> None:
        if logical_index < 0:
            return
        self._table.resizeColumnToContents(logical_index)
        fitted_width = self._table.columnWidth(logical_index)
        self._table.setColumnWidth(logical_index, min(max(96, fitted_width + 18), 480))
        self._queue_save_settings()

    def _on_header_section_clicked(self, logical_index: int) -> None:
        header = self._table.horizontalHeader()
        current_column = header.sortIndicatorSection()
        current_order = header.sortIndicatorOrder()
        if logical_index == current_column:
            next_order = (
                QtCore.Qt.SortOrder.DescendingOrder
                if current_order == QtCore.Qt.SortOrder.AscendingOrder
                else QtCore.Qt.SortOrder.AscendingOrder
            )
        else:
            next_order = QtCore.Qt.SortOrder.AscendingOrder
        self._apply_sort(
            logical_index,
            next_order,
            preserve_selection=True,
            update_indicator=True,
        )

    def _apply_sort(
        self,
        column: int,
        order,
        *,
        preserve_selection: bool,
        update_indicator: bool,
    ) -> None:
        header = self._table.horizontalHeader()
        selected_path = self._state.selected_file if preserve_selection else None
        if update_indicator:
            blocker = QtCore.QSignalBlocker(header)
            header.setSortIndicator(column, order)
            del blocker
        self._model.sort(column, order)
        if selected_path is not None:
            row = self._model.row_of_path(selected_path)
            if row >= 0:
                self._select_row(row)
        self._queue_save_settings()

    def _current_file_entry(self) -> DicomFileEntry | None:
        if self._state.selected_file is None:
            return None
        row = self._model.row_of_path(self._state.selected_file)
        entry = self._model.entry_at(row)
        return entry if isinstance(entry, DicomFileEntry) else None

    def _reload_current_file(self) -> None:
        entry = self._current_file_entry()
        if entry is not None:
            self._load_selected_entry(entry)

    def _set_display_auto(self) -> None:
        if self._current_file_entry() is None:
            return
        self._state.display = DisplaySettings.auto()
        self._reload_current_file()

    def _set_display_dicom(self) -> None:
        if self._current_file_entry() is None:
            return
        self._state.display = DisplaySettings.dicom_window()
        self._reload_current_file()

    def _set_display_min_max(self) -> None:
        rendered = self._last_rendered_frame
        if self._current_file_entry() is None or rendered is None:
            return
        if rendered.source_min is None or rendered.source_max is None:
            return
        self._state.display = DisplaySettings.value_range(
            float(rendered.source_min),
            float(rendered.source_max),
            label="Min-Max",
        )
        self._reload_current_file()

    def _set_display_window_preset(
        self,
        label: str,
        center: float,
        width: float,
    ) -> None:
        if self._current_file_entry() is None:
            return
        self._state.display = DisplaySettings.window(center, width, label=label)
        self._reload_current_file()

    def _set_display_percentile_preset(
        self,
        label: str,
        low_percentile: float,
        high_percentile: float,
        *,
        clamp_min_zero: bool,
    ) -> None:
        if self._current_file_entry() is None:
            return
        self._state.display = DisplaySettings.percentile_range(
            low_percentile,
            high_percentile,
            clamp_min_zero=clamp_min_zero,
            label=label,
        )
        self._reload_current_file()

    def _prompt_window_level_input(self) -> None:
        if self._current_file_entry() is None:
            return
        center = 40.0
        width = 400.0
        rendered = self._last_rendered_frame
        if rendered is not None and rendered.display_min is not None and rendered.display_max is not None:
            center, width = _window_from_bounds(
                float(rendered.display_min),
                float(rendered.display_max),
            )
        values = self._prompt_display_pair(
            title="Enter Window/Level",
            first_label="Level",
            first_value=center,
            second_label="Window",
            second_value=width,
            second_minimum=-1_000_000_000.0,
            second_maximum=1_000_000_000.0,
            second_decimals=3,
            enforce_second_positive=True,
        )
        if values is None:
            return
        level, window = values
        self._state.display = DisplaySettings.window(level, window, label="Manual")
        self._reload_current_file()

    def _prompt_min_max_input(self) -> None:
        if self._current_file_entry() is None:
            return
        rendered = self._last_rendered_frame
        minimum = 0.0
        maximum = 1.0
        if rendered is not None:
            if rendered.source_min is not None:
                minimum = float(rendered.source_min)
            if rendered.source_max is not None:
                maximum = float(rendered.source_max)
            if rendered.display_min is not None and rendered.display_max is not None:
                minimum = float(rendered.display_min)
                maximum = float(rendered.display_max)
        values = self._prompt_display_pair(
            title="Enter Min/Max",
            first_label="Minimum",
            first_value=minimum,
            second_label="Maximum",
            second_value=maximum,
            first_minimum=-1_000_000_000.0,
            first_maximum=1_000_000_000.0,
            second_minimum=-1_000_000_000.0,
            second_maximum=1_000_000_000.0,
            second_decimals=4,
            require_second_greater=True,
        )
        if values is None:
            return
        minimum, maximum = values
        self._state.display = DisplaySettings.value_range(
            minimum,
            maximum,
            label="Manual",
        )
        self._reload_current_file()

    def _prompt_display_pair(
        self,
        *,
        title: str,
        first_label: str,
        first_value: float,
        second_label: str,
        second_value: float,
        first_minimum: float = -1_000_000_000.0,
        first_maximum: float = 1_000_000_000.0,
        second_minimum: float = -1_000_000_000.0,
        second_maximum: float = 1_000_000_000.0,
        first_decimals: int = 4,
        second_decimals: int = 4,
        enforce_second_positive: bool = False,
        require_second_greater: bool = False,
    ) -> tuple[float, float] | None:
        dialog = QtWidgets.QDialog(self)
        dialog.setWindowTitle(title)
        dialog.setModal(True)

        layout = QtWidgets.QVBoxLayout(dialog)
        layout.setContentsMargins(18, 16, 18, 16)
        layout.setSpacing(14)

        form = QtWidgets.QFormLayout()
        form.setContentsMargins(0, 0, 0, 0)
        form.setHorizontalSpacing(14)
        form.setVerticalSpacing(10)
        layout.addLayout(form)

        first_spin = QtWidgets.QDoubleSpinBox(dialog)
        first_spin.setDecimals(first_decimals)
        first_spin.setRange(first_minimum, first_maximum)
        first_spin.setValue(float(first_value))
        first_spin.setSingleStep(1.0)
        first_spin.setAccelerated(True)
        first_spin.setMinimumWidth(180)

        second_spin = QtWidgets.QDoubleSpinBox(dialog)
        second_spin.setDecimals(second_decimals)
        second_spin.setRange(second_minimum, second_maximum)
        second_spin.setValue(float(second_value))
        second_spin.setSingleStep(1.0)
        second_spin.setAccelerated(True)
        second_spin.setMinimumWidth(180)

        form.addRow(first_label, first_spin)
        form.addRow(second_label, second_spin)

        buttons = QtWidgets.QDialogButtonBox(
            QtWidgets.QDialogButtonBox.StandardButton.Ok
            | QtWidgets.QDialogButtonBox.StandardButton.Cancel,
            parent=dialog,
        )
        layout.addWidget(buttons)
        buttons.accepted.connect(dialog.accept)
        buttons.rejected.connect(dialog.reject)

        if dialog.exec() != int(QtWidgets.QDialog.DialogCode.Accepted):
            return None

        first = float(first_spin.value())
        second = float(second_spin.value())
        if enforce_second_positive and second <= 0.0:
            QtWidgets.QMessageBox.warning(
                self,
                title,
                f"{second_label} must be greater than 0.",
            )
            return None
        if require_second_greater and second <= first:
            QtWidgets.QMessageBox.warning(
                self,
                title,
                f"{second_label} must be greater than {first_label}.",
            )
            return None
        return (first, second)

    def _trigger_display_preset_shortcut(self, digit: int) -> None:
        if digit < 1:
            return
        available_actions = [
            action
            for action, _groups, include_in_digits in self._display_action_groups
            if include_in_digits and action.isEnabled()
        ]
        index = digit - 1
        if 0 <= index < len(available_actions):
            available_actions[index].trigger()

    def _start_display_drag(self) -> None:
        entry = self._current_file_entry()
        rendered = self._last_rendered_frame
        if entry is None or rendered is None or not rendered.display_adjustable:
            self._display_drag_origin = None
            return

        group = modality_group(entry.modality)
        if group == "nuclear":
            maximum = rendered.display_max if rendered.display_max is not None else 1.0
            maximum = max(float(maximum), 1e-6)
            self._display_drag_origin = DisplaySettings.value_range(
                0.0,
                maximum,
                label="Manual",
            )
        elif rendered.display_min is not None and rendered.display_max is not None:
            center = 0.5 * (rendered.display_min + rendered.display_max)
            width = max(rendered.display_max - rendered.display_min, 1e-3)
            self._display_drag_origin = DisplaySettings.window(
                center,
                width,
                label="Manual",
            )
        else:
            self._display_drag_origin = None
            return

        self._display_drag_dx = 0.0
        self._display_drag_dy = 0.0

    def _update_display_drag(self, dx: float, dy: float) -> None:
        entry = self._current_file_entry()
        origin = self._display_drag_origin
        if entry is None or origin is None:
            return

        self._display_drag_dx += dx
        self._display_drag_dy += dy

        if origin.mode == "range" and origin.maximum is not None:
            factor = math.exp((self._display_drag_dx - self._display_drag_dy) * 0.01)
            maximum = max(origin.maximum * factor, 1e-6)
            self._state.display = DisplaySettings.value_range(
                origin.minimum or 0.0,
                maximum,
                label="Manual",
            )
        elif origin.mode == "window" and origin.center is not None and origin.width is not None:
            base_width = max(origin.width, 1.0)
            center = origin.center + (self._display_drag_dx * base_width * 0.005)
            width = max(base_width * math.exp(-self._display_drag_dy * 0.01), 1e-3)
            self._state.display = DisplaySettings.window(center, width, label="Manual")
        else:
            return

        self._reload_current_file()

    def _finish_display_drag(self) -> None:
        self._display_drag_origin = None
        self._display_drag_dx = 0.0
        self._display_drag_dy = 0.0

    def _update_display_controls(
        self,
        entry: DicomFileEntry | None,
    ) -> None:
        group = modality_group(entry.modality) if entry is not None else None
        enabled = entry is not None
        if self._display_button is not None:
            self._display_button.setEnabled(enabled)
        if self._display_auto_action is not None:
            self._display_auto_action.setEnabled(enabled)
        for action, groups, _include_in_digits in self._display_action_groups:
            action.setEnabled(enabled and (groups is None or group in groups))

    def _show_non_dicom_file(self, path: Path) -> None:
        self._stop_playback()
        self._state.selected_file = None
        self._state.current_frame = 0
        self._state.display = DisplaySettings.auto()
        self._current_dicom_file = None
        self._current_dicom_path = None
        self._last_rendered_frame = None
        self._canvas.clear()
        self._set_frame_controls(1, 0)
        self._update_info_panel_for_path(path)
        self._set_dump_source(None)
        self._update_display_controls(None)

    def _update_info_panel_for_path(self, path: Path) -> None:
        for key, label in self._info_labels.items():
            if key == "File":
                label.setText(path.name)
                label.setToolTip(str(path))
            else:
                label.setText("-")
                label.setToolTip("")
            _sync_wrapped_label_height(label)

    def _on_info_tab_changed(self, index: int) -> None:
        if self._info_tabs is None or self._dump_tab is None:
            return
        if self._info_tabs.widget(index) is self._dump_tab:
            self._ensure_dump_loaded()

    def _on_dump_filter_changed(self, _text: str) -> None:
        if self._info_tabs is None or self._dump_tab is None:
            return
        if self._info_tabs.currentWidget() is self._dump_tab:
            self._ensure_dump_loaded()

    def _set_dump_message(self, text: str) -> None:
        self._dump_message_label.setText(text)
        self._dump_stack.setCurrentWidget(self._dump_message_label)

    @staticmethod
    def _parse_dump_text(text: str) -> tuple[list[str], list[list[str]], set[int]]:
        lines = text.splitlines()
        if not lines:
            return ([], [], set())
        headers = lines[0].split("\t")
        if len(headers) < 2:
            return ([], [], set())
        rows: list[list[str]] = []
        special_rows: set[int] = set()
        width = len(headers)
        for line in lines[1:]:
            stripped = line.strip()
            if stripped.startswith("FRAME #") or stripped.startswith("FRAGMENT #"):
                rows.append([stripped] + [""] * (width - 1))
                special_rows.add(len(rows) - 1)
                continue
            parts = line.split("\t", width - 1)
            if len(parts) < width:
                parts.extend([""] * (width - len(parts)))
            if parts and parts[-1].startswith("# "):
                parts[-1] = parts[-1][2:]
            rows.append(parts)
        return (headers, rows, special_rows)

    def _show_dump_table(
        self,
        headers: list[str],
        rows: list[list[str]],
        special_rows: set[int],
    ) -> None:
        self._dump_model.set_dump(headers, rows, special_rows)
        self._dump_stack.setCurrentWidget(self._dump_table)
        self._dump_columns_button.setEnabled(bool(headers))
        self._dump_columns_button.setMenu(self._build_dump_columns_menu(self._dump_columns_button))
        layout_key = tuple(headers)
        if self._dump_layout_key != layout_key:
            self._dump_layout_key = layout_key
            header = self._dump_table.horizontalHeader()
            header.setStretchLastSection(False)
            for column in range(self._dump_model.columnCount()):
                header.setSectionResizeMode(
                    column,
                    QtWidgets.QHeaderView.ResizeMode.Interactive,
                )
            defaults = self._default_dump_column_widths()
            for column in range(self._dump_model.columnCount()):
                self._dump_table.setColumnWidth(
                    column,
                    self._pending_dump_widths.get(column, defaults.get(column, 120)),
                )

        for column in range(self._dump_model.columnCount()):
            self._dump_table.setColumnHidden(
                column,
                self._pending_dump_hidden.get(column, False),
            )
        self._sync_dump_column_actions()
        self._apply_dump_spans()

    def _apply_dump_spans(self) -> None:
        self._dump_table.clearSpans()
        column_count = self._dump_proxy.columnCount()
        if column_count <= 0:
            return
        anchor_column = next(
            (
                column
                for column in range(column_count)
                if not self._dump_table.isColumnHidden(column)
            ),
            -1,
        )
        if anchor_column < 0:
            return
        span_width = column_count - anchor_column
        for proxy_row in range(self._dump_proxy.rowCount()):
            proxy_index = self._dump_proxy.index(proxy_row, anchor_column)
            source_index = self._dump_proxy.mapToSource(proxy_index)
            if source_index.isValid() and self._dump_model.is_special_row(source_index.row()):
                self._dump_table.setSpan(proxy_row, anchor_column, 1, span_width)

    def _set_dump_source(self, path: Path | None) -> None:
        if path == self._dump_source_path:
            if self._info_tabs is not None and self._dump_tab is not None:
                if self._info_tabs.currentWidget() is self._dump_tab:
                    self._ensure_dump_loaded()
            return

        self._dump_source_path = path
        self._dump_loaded_path = None
        self._dump_full_text = ""
        self._dump_headers = []
        self._dump_rows = []
        self._dump_special_rows = set()
        self._dump_error_text = ""
        self._dump_layout_key = None
        self._dump_model.clear()
        self._dump_proxy.set_filter_text("")
        self._dump_columns_button.setEnabled(False)
        self._dump_columns_button.setMenu(None)
        self._dump_filter_edit.setEnabled(path is not None)

        if path is None:
            self._set_dump_message("Dump is available for DICOM files only.")
            return

        self._set_dump_message("Open the Dump tab to load dicomdump text.")
        if self._info_tabs is not None and self._dump_tab is not None:
            if self._info_tabs.currentWidget() is self._dump_tab:
                self._ensure_dump_loaded()

    def _ensure_dump_loaded(self) -> None:
        path = self._dump_source_path
        if path is None:
            self._set_dump_message("Dump is available for DICOM files only.")
            return

        if self._dump_loaded_path == path and not self._dump_error_text:
            self._apply_dump_filter()
            return

        QtWidgets.QApplication.setOverrideCursor(QtCore.Qt.CursorShape.WaitCursor)
        try:
            dicom_file = (
                self._current_dicom_file
                if self._current_dicom_path == path and self._current_dicom_file is not None
                else load_dicom_for_view(path)
            )
            self._dump_full_text = dicom_file.dump(
                max_print_chars=120,
                include_offset=True,
            )
            (
                self._dump_headers,
                self._dump_rows,
                self._dump_special_rows,
            ) = self._parse_dump_text(self._dump_full_text)
            self._dump_model.set_dump(
                self._dump_headers,
                self._dump_rows,
                self._dump_special_rows,
            )
            self._dump_loaded_path = path
            self._dump_error_text = ""
        except NonDicomFileError:
            self._dump_loaded_path = path
            self._dump_full_text = ""
            self._dump_headers = []
            self._dump_rows = []
            self._dump_special_rows = set()
            self._dump_error_text = "Not a DICOM file."
            self._set_dump_message(self._dump_error_text)
            return
        except Exception as exc:
            self._dump_loaded_path = path
            self._dump_full_text = ""
            self._dump_headers = []
            self._dump_rows = []
            self._dump_special_rows = set()
            self._dump_error_text = str(exc)
            self._set_dump_message(
                f"Failed to load dicomdump text for {path.name}.\n\n{exc}"
            )
            return
        finally:
            QtWidgets.QApplication.restoreOverrideCursor()

        self._apply_dump_filter()

    def _apply_dump_filter(self) -> None:
        if self._dump_error_text:
            self._set_dump_message(self._dump_error_text)
            return
        if not self._dump_headers:
            self._set_dump_message("No dicomdump rows are available.")
            return

        filter_text = self._dump_filter_edit.text().strip()
        self._dump_proxy.set_filter_text(filter_text)
        if self._dump_proxy.rowCount() == 0:
            if filter_text:
                self._set_dump_message(f"No dump rows matched: {filter_text}")
            else:
                self._set_dump_message("No dicomdump rows are available.")
            return
        self._show_dump_table(
            self._dump_headers,
            self._dump_rows,
            self._dump_special_rows,
        )

    def _queue_save_settings(self, *args) -> None:
        del args
        if not self._settings_ready:
            return
        self._settings_save_timer.start()

    def _save_table_settings(self) -> None:
        header = self._table.horizontalHeader()
        self._settings.setValue("table/header_state", header.saveState())
        self._settings.setValue("table/sort_column", header.sortIndicatorSection())
        self._settings.setValue(
            "table/sort_order",
            self._sort_order_value(header.sortIndicatorOrder()),
        )
        for column in range(self._model.columnCount()):
            self._settings.setValue(
                f"table/columns/{column}/hidden",
                self._table.isColumnHidden(column),
            )
            self._settings.setValue(
                f"table/columns/{column}/width",
                self._table.columnWidth(column),
            )

    def _save_dump_table_settings(self) -> None:
        for column in range(len(DUMP_HEADERS)):
            self._settings.setValue(
                f"dump/columns/{column}/hidden",
                self._dump_table.isColumnHidden(column),
            )
            self._settings.setValue(
                f"dump/columns/{column}/width",
                self._dump_table.columnWidth(column),
            )

    def _restore_window_settings(self) -> None:
        width = self._settings.value("window/width", 0, type=int)
        height = self._settings.value("window/height", 0, type=int)
        geometry = self._settings.value("window/geometry")

        if width > 0 and height > 0:
            self.resize(width, height)
        elif geometry is not None and self.restoreGeometry(geometry):
            pass
        else:
            self.resize(1440, 900)

        splitter_sizes = self._read_int_list(self._settings.value("window/splitter_sizes"))
        if len(splitter_sizes) == 3 and all(size > 0 for size in splitter_sizes):
            self._pending_splitter_sizes = splitter_sizes
        else:
            splitter_state = self._settings.value("window/splitter_state")
            if splitter_state is not None and self._splitter is not None:
                restored_splitter = self._splitter.restoreState(splitter_state)
                if not restored_splitter:
                    self._pending_splitter_sizes = self._default_splitter_sizes()
            else:
                self._pending_splitter_sizes = self._default_splitter_sizes()

        self._pending_window_maximized = self._settings.value(
            "window/maximized",
            False,
            type=bool,
        )

    def _restore_table_settings(self) -> None:
        header = self._table.horizontalHeader()
        state = self._settings.value("table/header_state")
        if state is not None:
            header.restoreState(state)
        default_widths = self._default_column_widths()
        for column in range(self._model.columnCount()):
            hidden = self._settings.value(
                f"table/columns/{column}/hidden",
                False,
                type=bool,
            )
            width = self._settings.value(
                f"table/columns/{column}/width",
                0,
                type=int,
            )
            self._table.setColumnHidden(column, hidden)
            if width > 0:
                self._table.setColumnWidth(column, width)
            elif column in default_widths:
                self._table.setColumnWidth(column, default_widths[column])
        sort_column = self._settings.value("table/sort_column", 0, type=int)
        sort_order = QtCore.Qt.SortOrder(
            self._settings.value(
                "table/sort_order",
                self._sort_order_value(QtCore.Qt.SortOrder.AscendingOrder),
                type=int,
            )
        )
        self._apply_sort(
            sort_column,
            sort_order,
            preserve_selection=False,
            update_indicator=True,
        )
        self._sync_column_actions()

    def _restore_dump_table_settings(self) -> None:
        self._pending_dump_widths = {}
        self._pending_dump_hidden = {}
        defaults = self._default_dump_column_widths()
        for column in range(len(DUMP_HEADERS)):
            hidden = self._settings.value(
                f"dump/columns/{column}/hidden",
                False,
                type=bool,
            )
            width = self._settings.value(
                f"dump/columns/{column}/width",
                0,
                type=int,
            )
            self._pending_dump_hidden[column] = hidden
            self._pending_dump_widths[column] = width if width > 0 else defaults[column]

    def _sync_column_actions(self) -> None:
        for column, action in enumerate(self._column_actions):
            action.blockSignals(True)
            action.setChecked(not self._table.isColumnHidden(column))
            action.blockSignals(False)

    def _sync_dump_column_actions(self) -> None:
        for column, action in enumerate(self._dump_column_actions):
            action.blockSignals(True)
            action.setChecked(not self._dump_table.isColumnHidden(column))
            action.blockSignals(False)

    def _save_settings(self) -> None:
        self._save_table_settings()
        self._save_dump_table_settings()
        normal_geometry = self.normalGeometry() if self.isMaximized() else self.geometry()
        self._settings.setValue("browser/last_path", str(self._state.current_dir))
        self._settings.setValue("window/geometry", self.saveGeometry())
        self._settings.setValue("window/width", normal_geometry.width())
        self._settings.setValue("window/height", normal_geometry.height())
        self._settings.setValue("window/maximized", self.isMaximized())
        if self._splitter is not None:
            self._settings.setValue("window/splitter_state", self._splitter.saveState())
            self._settings.setValue("window/splitter_sizes", self._splitter.sizes())
        self._settings.sync()

    def _load_selected_entry(self, entry: DicomFileEntry) -> None:
        try:
            if self._current_dicom_path != entry.path or self._current_dicom_file is None:
                self._current_dicom_file = load_dicom_for_view(entry.path)
                self._current_dicom_path = entry.path
            rendered = render_loaded_dicom(
                self._current_dicom_file,
                frame_index=self._state.current_frame,
                display=self._state.display,
                modality=entry.modality,
            )
        except NonDicomFileError:
            self._show_non_dicom_file(entry.path)
            self.statusBar().showMessage(f"Not a DICOM file: {entry.path.name}")
            return
        except DicomNotDisplayableError as exc:
            self._stop_playback()
            self._last_rendered_frame = None
            self._canvas.show_error_message(
                f"Cannot display {entry.name}",
                str(exc),
            )
            self._set_frame_controls(1, 0)
            self._update_info_panel(entry, None)
            self._set_dump_source(entry.path)
            self._update_display_controls(entry)
            self.statusBar().showMessage(f"Cannot display {entry.path.name}: {exc}")
            return
        except Exception as exc:
            self._stop_playback()
            self._last_rendered_frame = None
            self._canvas.show_error_message(
                f"Failed to open {entry.name}",
                str(exc),
            )
            self._set_frame_controls(1, 0)
            self._update_info_panel(entry, None)
            self._set_dump_source(entry.path)
            self._update_display_controls(entry)
            self.statusBar().showMessage(f"Failed to open {entry.path.name}: {exc}")
            return

        self._state.selected_file = entry.path
        self._state.current_frame = rendered.frame_index
        self._last_rendered_frame = rendered
        self._canvas.set_pixmap(rendered.pixmap)
        self._canvas.set_display_overlay_text(f"Display {rendered.display_text}")
        self._set_frame_controls(rendered.frame_count, rendered.frame_index)
        self._update_info_panel(entry, rendered)
        self._set_dump_source(entry.path)
        self._update_display_controls(entry)
        self._update_status(entry, rendered)

    def _set_frame_controls(self, frame_count: int, frame_index: int) -> None:
        if frame_count <= 1:
            self._stop_playback()
        self._ignore_frame_changes = True
        self._frame_scroll.setEnabled(frame_count > 1)
        self._frame_scroll.setMinimum(1)
        self._frame_scroll.setMaximum(max(1, frame_count))
        self._frame_scroll.setValue(frame_index + 1)
        self._frame_scroll.setPageStep(1)
        self._frame_first_button.setEnabled(frame_count > 1)
        self._frame_play_button.setEnabled(frame_count > 1)
        self._frame_last_button.setEnabled(frame_count > 1)
        self._frame_status_label.setText(f"{frame_index + 1} / {max(1, frame_count)}")
        self._frame_bar.setVisible(frame_count > 1)
        self._ignore_frame_changes = False
        self._update_play_button()

    def _on_frame_scroll_changed(self, value: int) -> None:
        if self._ignore_frame_changes or self._state.selected_file is None:
            return
        frame_index = max(0, value - 1)
        if frame_index == self._state.current_frame:
            return
        self._state.current_frame = frame_index
        row = self._model.row_of_path(self._state.selected_file)
        entry = self._model.entry_at(row)
        if isinstance(entry, DicomFileEntry):
            self._load_selected_entry(entry)

    def _previous_frame(self) -> None:
        if not self._frame_scroll.isEnabled():
            return
        self._frame_scroll.setValue(
            max(self._frame_scroll.minimum(), self._frame_scroll.value() - 1)
        )

    def _next_frame(self) -> None:
        if not self._frame_scroll.isEnabled():
            return
        self._frame_scroll.setValue(
            min(self._frame_scroll.maximum(), self._frame_scroll.value() + 1)
        )

    def _go_first_frame(self) -> None:
        if not self._frame_scroll.isEnabled():
            return
        self._frame_scroll.setValue(self._frame_scroll.minimum())

    def _go_last_frame(self) -> None:
        if not self._frame_scroll.isEnabled():
            return
        self._frame_scroll.setValue(self._frame_scroll.maximum())

    def _toggle_playback(self) -> None:
        if not self._frame_scroll.isEnabled():
            return
        if self._cine_timer.isActive():
            self._stop_playback()
            return
        self._cine_timer.start()
        self._update_play_button()

    def _stop_playback(self) -> None:
        if self._cine_timer.isActive():
            self._cine_timer.stop()
        self._update_play_button()

    def _update_play_button(self) -> None:
        self._frame_play_button.setText("Pause" if self._cine_timer.isActive() else "Play")

    def _advance_playback_frame(self) -> None:
        if not self._frame_scroll.isEnabled():
            self._stop_playback()
            return
        current = self._frame_scroll.value()
        maximum = self._frame_scroll.maximum()
        next_value = current + 1 if current < maximum else self._frame_scroll.minimum()
        self._frame_scroll.setValue(next_value)

    def _select_previous_file(self) -> None:
        current_row = self._table.currentIndex().row()
        row = self._model.previous_file_row(current_row if current_row >= 0 else self._model.rowCount())
        if row >= 0:
            self._select_row(row)

    def _select_next_file(self) -> None:
        current_row = self._table.currentIndex().row()
        row = self._model.next_file_row(current_row)
        if current_row < 0:
            row = self._model.first_file_row()
        if row >= 0:
            self._select_row(row)

    def _update_info_panel(
        self,
        entry: DicomFileEntry | FolderEntry | None,
        rendered: RenderedFrame | None,
    ) -> None:
        values = {key: "-" for key in self._info_labels}
        tooltips = {key: "" for key in self._info_labels}
        if isinstance(entry, FolderEntry):
            values["File"] = entry.display_name
            tooltips["File"] = str(entry.path)
        elif isinstance(entry, DicomFileEntry):
            values["File"] = entry.name
            tooltips["File"] = str(entry.path)
            values["Study"] = entry.study_description or "-"
            values["Series"] = entry.series_description or "-"
            values["Date"] = entry.study_date or "-"
            values["Patient ID"] = entry.patient_id or "-"
            values["Patient Name"] = entry.patient_name or "-"
            values["Modality"] = entry.modality or "-"
            values["Size"] = entry.size_summary
        if rendered is not None:
            values["Size"] = " x ".join(
                [
                    str(rendered.rows),
                    str(rendered.columns),
                    str(rendered.bits_allocated or "?"),
                    str(rendered.frame_count),
                ]
            )
            values["Frame"] = f"{rendered.frame_index + 1}/{rendered.frame_count}"
            values["Window/Level"] = rendered.dicom_window_text or "-"
            values["Rescale"] = rendered.rescale_text or "-"
            values["SOP Class"] = rendered.sop_class or "-"
            values["Transfer Syntax"] = rendered.transfer_syntax or "-"
            values["Photometric"] = rendered.photometric or "-"
            values["Pixel Repr"] = _format_pixel_representation(
                rendered.pixel_representation
            )
            values["Stored Bits"] = _format_stored_bits(
                rendered.bits_stored,
                rendered.high_bit,
            )
            values["Stored Value Range"] = _format_raw_range(
                rendered.raw_min,
                rendered.raw_max,
            )
            tooltips["SOP Class"] = rendered.sop_class_uid_value or ""
            tooltips["Transfer Syntax"] = rendered.transfer_syntax_uid_value or ""
        for key, label in self._info_labels.items():
            label.setText(values.get(key, "-"))
            label.setToolTip(tooltips.get(key, ""))
            _sync_wrapped_label_height(label)

    def _update_status(self, entry: DicomFileEntry, rendered: RenderedFrame) -> None:
        row = self._model.row_of_path(entry.path)
        file_position, file_total = self._model.file_position(row)
        zoom_percent = self._canvas.zoom_percent()
        status = (
            f"{self._state.current_dir} | "
            f"selected {entry.name} | "
            f"file {file_position}/{file_total} | "
            f"frame {rendered.frame_index + 1}/{rendered.frame_count} | "
            f"{rendered.rows}x{rendered.columns}x{rendered.bits_allocated or '?'}bpp | "
            f"Display {rendered.display_text} | "
            f"raw {rendered.raw_dtype or '?'} {_format_raw_range(rendered.raw_min, rendered.raw_max)} | "
            f"{rendered.photometric or '-'} | "
            f"{_format_pixel_representation(rendered.pixel_representation)} | "
            f"{zoom_percent}%"
        )
        self.statusBar().showMessage(status)

    def eventFilter(self, watched, event) -> bool:
        header_viewport = self._table.horizontalHeader().viewport()
        if watched is header_viewport:
            event_type = event.type()
            if event_type in (
                QtCore.QEvent.Type.MouseMove,
                QtCore.QEvent.Type.HoverMove,
            ):
                position = (
                    event.position().toPoint()
                    if hasattr(event, "position")
                    else event.pos()
                )
                self._update_header_resize_cursor(position.x())
            elif event_type == QtCore.QEvent.Type.Enter:
                self._update_header_resize_cursor(None)
            elif event_type == QtCore.QEvent.Type.Leave:
                header_viewport.unsetCursor()
        return super().eventFilter(watched, event)

    def _update_header_resize_cursor(self, x: int | None) -> None:
        header = self._table.horizontalHeader()
        viewport = header.viewport()
        if x is None:
            viewport.unsetCursor()
            return
        for visual_index in range(header.count()):
            logical_index = header.logicalIndex(visual_index)
            if logical_index < 0 or header.isSectionHidden(logical_index):
                continue
            right_edge = (
                header.sectionViewportPosition(logical_index)
                + header.sectionSize(logical_index)
            )
            if abs(x - right_edge) <= self._header_resize_hotspot:
                viewport.setCursor(QtCore.Qt.CursorShape.SplitHCursor)
                return
        viewport.unsetCursor()
