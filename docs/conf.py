import pathlib
import posixpath
import re
import sys
import os

# Project paths
ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))


def _read_dicomsdl_version() -> str:
    header_path = ROOT / "include" / "dicom_const.h"
    pattern = re.compile(r'^\s*#define\s+DICOMSDL_VERSION\s+"([^"]+)"\s*$')

    for line in header_path.read_text(encoding="utf-8").splitlines():
        match = pattern.match(line)
        if match:
            return match.group(1).strip()

    raise RuntimeError(f"Could not find DICOMSDL_VERSION macro in {header_path}")


project = "dicomsdl"
author = "dicomsdl contributors"
release = version = _read_dicomsdl_version()

SUPPORTED_DOC_LANGUAGES = ("en", "ko", "ja", "zh-cn")
_LANGUAGE_ALIASES = {
    "zh-CN": "zh-cn",
    "zh_CN": "zh-cn",
    "zh-cn": "zh-cn",
    "zh-Hans": "zh-cn",
}
LANGUAGE_LABELS = {
    "en": "English",
    "ko": "한국어",
    "ja": "日本語",
    "zh-cn": "简体中文",
}
_DEFAULT_ACTIVE_LANGUAGES = ",".join(SUPPORTED_DOC_LANGUAGES)
_UI_TEXT = {
    "en": {
        "languages": "Languages",
        "status": "Status",
        "source": ("English source", "This page is the canonical version."),
        "localized": (
            "Localized page",
            "This page is translated. New updates land in English first.",
        ),
        "pending": (
            "Translation pending",
            "This page still uses English body text. Check the English page for the newest wording.",
        ),
    },
    "ko": {
        "languages": "언어",
        "status": "상태",
        "source": ("영문 원본", "이 페이지가 기준본입니다."),
        "localized": (
            "번역본",
            "이 페이지는 현지화되어 있으며 최신 변경은 영문 문서에 먼저 반영됩니다.",
        ),
        "pending": (
            "번역 대기",
            "이 페이지는 아직 영어 본문을 그대로 사용합니다. 최신 내용은 영문 문서를 기준으로 확인하세요.",
        ),
    },
    "ja": {
        "languages": "言語",
        "status": "状態",
        "source": ("英語原本", "このページが基準版です。"),
        "localized": (
            "翻訳済み",
            "このページはローカライズ済みですが、新しい更新は英語版に先に反映されます。",
        ),
        "pending": (
            "翻訳待ち",
            "このページはまだ英語本文をそのまま使っています。最新の内容は英語版を参照してください。",
        ),
    },
    "zh-cn": {
        "languages": "语言",
        "status": "状态",
        "source": ("英文原版", "这个页面是基准版本。"),
        "localized": (
            "已本地化",
            "这个页面已本地化，但最新更新会先进入英文版。",
        ),
        "pending": (
            "待翻译",
            "这个页面暂时仍沿用英文正文。最新内容请以英文版为准。",
        ),
    },
}
_requested_language = os.environ.get("DICOMSDL_DOC_LANGUAGE", "en").strip() or "en"
doc_language = _LANGUAGE_ALIASES.get(_requested_language, _requested_language)
if doc_language not in SUPPORTED_DOC_LANGUAGES:
    supported = ", ".join(SUPPORTED_DOC_LANGUAGES)
    raise RuntimeError(
        f"Unsupported DICOMSDL_DOC_LANGUAGE={_requested_language!r}; "
        f"expected one of: {supported}"
    )
_requested_active_languages = os.environ.get(
    "DICOMSDL_ACTIVE_DOC_LANGUAGES", _DEFAULT_ACTIVE_LANGUAGES
)
active_doc_languages_list: list[str] = []
for raw_lang in _requested_active_languages.split(","):
    raw_lang = raw_lang.strip()
    if not raw_lang:
        continue
    normalized = _LANGUAGE_ALIASES.get(raw_lang, raw_lang)
    if normalized not in SUPPORTED_DOC_LANGUAGES:
        continue
    if normalized not in active_doc_languages_list:
        active_doc_languages_list.append(normalized)
if doc_language not in active_doc_languages_list:
    active_doc_languages_list.append(doc_language)
ACTIVE_DOC_LANGUAGES = tuple(active_doc_languages_list)
language = {
    "en": "en",
    "ko": "ko",
    "ja": "ja",
    "zh-cn": "zh_CN",
}[doc_language]

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
html_static_path = ["_static"]
html_css_files = ["docchrome.css"]
html_context = {
    "doc_language": doc_language,
    "supported_doc_languages": ACTIVE_DOC_LANGUAGES,
}

# Misc
templates_path = ["_templates"]
exclude_patterns = ["_build", "Thumbs.db", ".DS_Store"]
primary_domain = "py"

# Type hints
always_document_param_types = True
typehints_fully_qualified = False


def _build_mode(outdir: pathlib.Path) -> str:
    if outdir.parent.name == "html-multilang":
        return "multilang"
    if outdir.name == "html":
        return "single-root"
    if outdir.parent.name == "html":
        return "single-lang-subdir"
    return "single-root"


def _uri_from_build_root(target_uri: str, lang: str, mode: str) -> pathlib.PurePosixPath:
    target_path = pathlib.PurePosixPath(target_uri)
    if mode == "multilang":
        return pathlib.PurePosixPath(lang) / target_path
    if mode == "single-root":
        return target_path if lang == "en" else pathlib.PurePosixPath(lang) / target_path
    if mode == "single-lang-subdir":
        return target_path if lang == "en" else pathlib.PurePosixPath(lang) / target_path
    return target_path


def _relative_language_url(app, pagename: str, target_lang: str, outdir: pathlib.Path) -> str:
    target_uri = app.builder.get_target_uri(pagename)
    mode = _build_mode(outdir)
    current_path = _uri_from_build_root(target_uri, doc_language, mode)
    target_path = _uri_from_build_root(target_uri, target_lang, mode)
    return posixpath.relpath(
        target_path.as_posix(),
        start=current_path.parent.as_posix(),
    )


def _read_source_text(base_dir: pathlib.Path, pagename: str) -> str | None:
    for suffix in (".md", ".rst"):
        path = base_dir / f"{pagename}{suffix}"
        if path.exists():
            return path.read_text(encoding="utf-8").strip()
    return None


def _translation_status_key(app, pagename: str) -> str | None:
    if doc_language == "en":
        return "source"

    docs_root = pathlib.Path(app.srcdir).parent
    current_text = _read_source_text(pathlib.Path(app.srcdir), pagename)
    english_text = _read_source_text(docs_root / "en", pagename)
    if current_text is None or english_text is None:
        return None
    if current_text == english_text:
        return "pending"
    return "localized"


def _inject_doc_ui(app, pagename, templatename, context, doctree):
    outdir = pathlib.Path(app.outdir)
    ui_text = _UI_TEXT[doc_language]

    context["doc_language_links"] = [
        {
            "code": lang,
            "label": LANGUAGE_LABELS[lang],
            "current": lang == doc_language,
            "url": _relative_language_url(app, pagename, lang, outdir),
        }
        for lang in ACTIVE_DOC_LANGUAGES
    ]
    context["doc_ui_languages_label"] = ui_text["languages"]
    context["doc_ui_status_label"] = ui_text["status"]

    status_key = _translation_status_key(app, pagename)
    if status_key is None:
        context["doc_translation_status"] = None
    else:
        title, message = ui_text[status_key]
        context["doc_translation_status"] = {
            "kind": status_key,
            "title": title,
            "message": message,
        }


def setup(app):
    app.connect("html-page-context", _inject_doc_ui)
