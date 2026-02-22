from __future__ import annotations

import ast
from pathlib import Path

import dicomsdl as dicom


def _stub_tree() -> ast.Module:
    stub_path = Path(dicom.__file__).with_name("_dicomsdl.pyi")
    return ast.parse(stub_path.read_text(encoding="utf-8"))


def _stub_all_names(tree: ast.Module) -> list[str]:
    for node in tree.body:
        if not isinstance(node, ast.Assign):
            continue
        if len(node.targets) != 1:
            continue
        target = node.targets[0]
        if not isinstance(target, ast.Name) or target.id != "__all__":
            continue
        if not isinstance(node.value, ast.List):
            raise AssertionError("__all__ in stub must be a list literal")
        names: list[str] = []
        for elt in node.value.elts:
            if not isinstance(elt, ast.Constant) or not isinstance(elt.value, str):
                raise AssertionError("__all__ entries in stub must be string literals")
            names.append(elt.value)
        return names
    raise AssertionError("stub is missing __all__ assignment")


def _class_method_names(tree: ast.Module, class_name: str) -> set[str]:
    for node in tree.body:
        if isinstance(node, ast.ClassDef) and node.name == class_name:
            return {
                child.name
                for child in node.body
                if isinstance(child, (ast.FunctionDef, ast.AsyncFunctionDef))
            }
    raise AssertionError(f"stub is missing class {class_name}")


def _module_function_names(tree: ast.Module) -> set[str]:
    return {
        node.name
        for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
    }


def test_stub_all_matches_runtime_all():
    tree = _stub_tree()
    assert _stub_all_names(tree) == list(dicom.__all__)


def test_stub_dataset_methods_include_runtime_mutation_api():
    tree = _stub_tree()
    methods = _class_method_names(tree, "DataSet")
    assert {"add_dataelement", "remove_dataelement", "get_dataelement"} <= methods
    dataelement_methods = _class_method_names(tree, "DataElement")
    assert "__bool__" in dataelement_methods
    assert {"is_present", "is_missing"} <= dataelement_methods


def test_stub_vr_does_not_expose_removed_first_second():
    tree = _stub_tree()
    methods = _class_method_names(tree, "VR")
    assert "first" not in methods
    assert "second" not in methods


def test_stub_includes_diagnostics_api():
    tree = _stub_tree()
    functions = _module_function_names(tree)
    assert {"set_default_reporter", "set_thread_reporter", "set_log_level"} <= functions
    buffering_methods = _class_method_names(tree, "BufferingReporter")
    assert {"take_messages", "for_each"} <= buffering_methods
