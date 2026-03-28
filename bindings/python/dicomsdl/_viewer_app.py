from __future__ import annotations

from pathlib import Path

from ._qt_compat import QtWidgets
from ._viewer_main_window import ViewerMainWindow
from ._viewer_theme import general_ui_font


def _configure_application(app: QtWidgets.QApplication) -> None:
    app.setApplicationName("dicomview")
    app.setOrganizationName("dicomsdl")
    app.setFont(general_ui_font(12.0))


def run_viewer(initial_input: Path | None = None) -> int:
    app = QtWidgets.QApplication.instance()
    owns_app = app is None
    if app is None:
        app = QtWidgets.QApplication(["dicomview"])
    _configure_application(app)

    window = ViewerMainWindow(initial_input=initial_input)
    window.show()

    if owns_app:
        return app.exec()
    return 0
