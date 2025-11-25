import pathlib
import sys

# Project paths
ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

project = "dicomsdl"
author = "dicomsdl contributors"
release = version = (ROOT / "VERSION").read_text().strip()

extensions = [
    "myst_parser",
    "sphinx.ext.autodoc",
    "sphinx.ext.napoleon",
    "sphinx.ext.autosectionlabel",
    "sphinx.ext.intersphinx",
    "sphinx.ext.todo",
    "sphinx.ext.viewcode",
    "breathe",
    "sphinx_autodoc_typehints",
]

myst_enable_extensions = ["colon_fence", "deflist", "substitution", "linkify"]
source_suffix = [".rst", ".md"]
autosummary_generate = False
autodoc_mock_imports = ["dicomsdl"]
autosectionlabel_prefix_document = True

# Intersphinx
intersphinx_mapping = {
    "python": ("https://docs.python.org/3", None),
}

todo_include_todos = True

# Breathe / Doxygen
_doxygen_xml = ROOT / "docs" / "_build" / "doxygen" / "xml"
breathe_projects = {"dicomsdl": str(_doxygen_xml)}
breathe_default_project = "dicomsdl"

# HTML
html_theme = "furo"
html_title = f"dicomsdl {version}"
html_static_path = []

# Misc
templates_path = []
exclude_patterns = ["_build", "Thumbs.db", ".DS_Store"]
primary_domain = "py"

# Type hints
always_document_param_types = True
typehints_fully_qualified = False
