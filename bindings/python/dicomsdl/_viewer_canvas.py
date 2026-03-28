from __future__ import annotations

from ._qt_compat import QtCore, QtGui, QtWidgets, Signal


class ImageCanvas(QtWidgets.QGraphicsView):
    display_drag_started = Signal()
    display_dragged = Signal(float, float)
    display_drag_finished = Signal()

    def __init__(self, parent=None) -> None:
        super().__init__(parent)
        self._scene = QtWidgets.QGraphicsScene(self)
        self._pixmap_item = self._scene.addPixmap(QtGui.QPixmap())
        self._fit_mode = True
        self._zoom = 1.0
        self._display_drag_active = False
        self._display_drag_last_pos: QtCore.QPoint | None = None
        self._message_overlay = QtWidgets.QFrame(self.viewport())
        self._message_title = QtWidgets.QLabel(self._message_overlay)
        self._message_detail = QtWidgets.QLabel(self._message_overlay)
        self._display_overlay = QtWidgets.QFrame(self.viewport())
        self._display_overlay_label = QtWidgets.QLabel(self._display_overlay)

        self.setObjectName("dicomviewCanvas")
        self.setScene(self._scene)
        self.setAlignment(QtCore.Qt.AlignmentFlag.AlignCenter)
        self.setBackgroundBrush(QtGui.QColor("#0f1115"))
        self.setFrameShape(QtWidgets.QFrame.Shape.NoFrame)
        self.setDragMode(QtWidgets.QGraphicsView.DragMode.ScrollHandDrag)
        self.setTransformationAnchor(
            QtWidgets.QGraphicsView.ViewportAnchor.AnchorUnderMouse
        )
        self.setResizeAnchor(QtWidgets.QGraphicsView.ViewportAnchor.AnchorViewCenter)
        self.setRenderHints(
            QtGui.QPainter.RenderHint.TextAntialiasing
            | QtGui.QPainter.RenderHint.SmoothPixmapTransform
        )

        self._message_overlay.setObjectName("dicomviewCanvasMessage")
        self._message_overlay.setStyleSheet(
            """
            QFrame#dicomviewCanvasMessage {
                background-color: rgba(27, 32, 39, 236);
                border: 1px solid rgba(201, 214, 232, 46);
                border-radius: 14px;
            }
            """
        )
        self._message_overlay.hide()

        message_layout = QtWidgets.QVBoxLayout(self._message_overlay)
        message_layout.setContentsMargins(22, 20, 22, 20)
        message_layout.setSpacing(10)

        self._message_title.setAlignment(QtCore.Qt.AlignmentFlag.AlignCenter)
        self._message_title.setWordWrap(True)
        self._message_title.setStyleSheet(
            "color: #f5f8fc; font-size: 18px; font-weight: 700;"
        )
        self._message_detail.setAlignment(QtCore.Qt.AlignmentFlag.AlignCenter)
        self._message_detail.setWordWrap(True)
        self._message_detail.setStyleSheet("color: #bcc6d4; font-size: 13px;")

        message_layout.addWidget(self._message_title)
        message_layout.addWidget(self._message_detail)

        self._display_overlay.setObjectName("dicomviewCanvasDisplay")
        self._display_overlay.setStyleSheet(
            """
            QFrame#dicomviewCanvasDisplay {
                background-color: rgba(14, 18, 24, 216);
                border: 1px solid rgba(195, 208, 226, 52);
                border-radius: 10px;
            }
            """
        )
        display_layout = QtWidgets.QHBoxLayout(self._display_overlay)
        display_layout.setContentsMargins(12, 8, 12, 8)
        display_layout.setSpacing(0)
        self._display_overlay_label.setStyleSheet(
            "color: #eef4ff; font-size: 12px; font-weight: 700;"
        )
        self._display_overlay_label.setAlignment(
            QtCore.Qt.AlignmentFlag.AlignRight | QtCore.Qt.AlignmentFlag.AlignVCenter
        )
        display_layout.addWidget(self._display_overlay_label)
        self._display_overlay.hide()

    def has_image(self) -> bool:
        return not self._pixmap_item.pixmap().isNull()

    def set_pixmap(self, pixmap: QtGui.QPixmap) -> None:
        self._hide_message()
        self._pixmap_item.setPixmap(pixmap)
        self._scene.setSceneRect(self._pixmap_item.boundingRect())
        self.fit_to_window()
        self._layout_display_overlay()

    def clear(self) -> None:
        self._hide_message()
        self._hide_display_overlay()
        self._pixmap_item.setPixmap(QtGui.QPixmap())
        self._scene.setSceneRect(QtCore.QRectF())
        self.reset_zoom()

    def show_error_message(self, title: str, detail: str = "") -> None:
        self._hide_display_overlay()
        self._pixmap_item.setPixmap(QtGui.QPixmap())
        self._scene.setSceneRect(QtCore.QRectF())
        self.reset_zoom()
        self._message_title.setText(title)
        self._message_detail.setText(detail)
        self._message_detail.setVisible(bool(detail.strip()))
        self._message_overlay.show()
        self._layout_message_overlay()

    def fit_to_window(self) -> None:
        if not self.has_image():
            return
        self._fit_mode = True
        self.resetTransform()
        self.fitInView(self._pixmap_item, QtCore.Qt.AspectRatioMode.KeepAspectRatio)
        self._zoom = 1.0

    def actual_size(self) -> None:
        if not self.has_image():
            return
        self._fit_mode = False
        self.resetTransform()
        self._zoom = 1.0

    def reset_zoom(self) -> None:
        if self.has_image():
            self.actual_size()
        else:
            self._fit_mode = True
            self.resetTransform()
            self._zoom = 1.0

    def zoom_in(self) -> None:
        self._apply_zoom(1.2)

    def zoom_out(self) -> None:
        self._apply_zoom(1.0 / 1.2)

    def zoom_percent(self) -> int:
        if self._fit_mode:
            return 100
        return max(1, int(round(self._zoom * 100.0)))

    def resizeEvent(self, event) -> None:
        super().resizeEvent(event)
        if self._fit_mode and self.has_image():
            self.fit_to_window()
        self._layout_message_overlay()
        self._layout_display_overlay()

    def wheelEvent(self, event) -> None:
        if event.modifiers() & QtCore.Qt.KeyboardModifier.ControlModifier:
            if event.angleDelta().y() > 0:
                self.zoom_in()
            else:
                self.zoom_out()
            event.accept()
            return
        super().wheelEvent(event)

    def _apply_zoom(self, factor: float) -> None:
        if not self.has_image():
            return
        self._fit_mode = False
        self.scale(factor, factor)
        self._zoom *= factor

    def _hide_message(self) -> None:
        self._message_overlay.hide()
        self._message_title.clear()
        self._message_detail.clear()

    def set_display_overlay_text(self, text: str) -> None:
        text = text.strip()
        if not text or text == "n/a" or not self.has_image():
            self._hide_display_overlay()
            return
        self._display_overlay_label.setText(text)
        self._display_overlay.show()
        self._layout_display_overlay()

    def _hide_display_overlay(self) -> None:
        self._display_overlay.hide()
        self._display_overlay_label.clear()

    def _layout_message_overlay(self) -> None:
        viewport_rect = self.viewport().rect()
        max_width = max(220, viewport_rect.width() - 48)
        width = min(560, max_width)
        height = min(220, max(110, viewport_rect.height() // 3))
        x = max(12, (viewport_rect.width() - width) // 2)
        y = max(12, (viewport_rect.height() - height) // 2)
        self._message_overlay.setGeometry(x, y, width, height)

    def _layout_display_overlay(self) -> None:
        if not self._display_overlay.isVisible():
            return
        self._display_overlay.adjustSize()
        size = self._display_overlay.sizeHint()
        viewport_rect = self.viewport().rect()
        max_width = max(180, viewport_rect.width() // 2)
        width = min(size.width(), max_width)
        height = size.height()
        x = max(12, viewport_rect.width() - width - 16)
        y = max(12, viewport_rect.height() - height - 16)
        self._display_overlay.setGeometry(x, y, width, height)

    def mousePressEvent(self, event) -> None:
        if event.button() == QtCore.Qt.MouseButton.RightButton and self.has_image():
            self._display_drag_active = True
            self._display_drag_last_pos = self._event_point(event)
            self.viewport().setCursor(QtCore.Qt.CursorShape.SizeAllCursor)
            self.display_drag_started.emit()
            event.accept()
            return
        super().mousePressEvent(event)

    def mouseMoveEvent(self, event) -> None:
        if self._display_drag_active and self._display_drag_last_pos is not None:
            point = self._event_point(event)
            delta = point - self._display_drag_last_pos
            self._display_drag_last_pos = point
            self.display_dragged.emit(float(delta.x()), float(delta.y()))
            event.accept()
            return
        super().mouseMoveEvent(event)

    def mouseReleaseEvent(self, event) -> None:
        if (
            self._display_drag_active
            and event.button() == QtCore.Qt.MouseButton.RightButton
        ):
            self._display_drag_active = False
            self._display_drag_last_pos = None
            self.viewport().unsetCursor()
            self.display_drag_finished.emit()
            event.accept()
            return
        super().mouseReleaseEvent(event)

    def contextMenuEvent(self, event) -> None:
        if self._display_drag_active:
            event.accept()
            return
        super().contextMenuEvent(event)

    @staticmethod
    def _event_point(event) -> QtCore.QPoint:
        return event.position().toPoint() if hasattr(event, "position") else event.pos()
