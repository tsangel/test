#!/usr/bin/env python3
"""Dump basic information about each element in a DICOM file."""

from __future__ import annotations

import argparse
import sys
from typing import Iterable

import dicomsdl as dicom

MAX_LINE = 160

LONG_VRS = {dicom.VR.SS.value, dicom.VR.US.value, dicom.VR.SL.value, dicom.VR.UL.value}
LONG_LONG_VRS = {dicom.VR.SV.value, dicom.VR.UV.value}
DOUBLE_VRS = {dicom.VR.FL.value, dicom.VR.FD.value, dicom.VR.DS.value, dicom.VR.IS.value}


def format_vector(values: Iterable[object]) -> str:
    items = list(values)
    return "[]" if not items else "[" + ",".join(str(v) for v in items) + "]"


def to_tag_string(tag: dicom.Tag) -> str:
    return f"({tag.group:04X},{tag.element:04X})"


def try_numeric(elem: dicom.DataElement) -> str | None:
    vr_value = elem.vr.value
    vm = elem.vm
    if vr_value in LONG_VRS:
        if vm > 1:
            vec = elem.to_long_vector()
            return format_vector(vec) if vec is not None else None
        scalar = elem.to_long()
        return str(scalar) if scalar is not None else None
    if vr_value in LONG_LONG_VRS:
        if vm > 1:
            vec = elem.to_longlong_vector()
            return format_vector(vec) if vec is not None else None
        scalar = elem.to_longlong()
        return str(scalar) if scalar is not None else None
    if vr_value in DOUBLE_VRS:
        if vm > 1:
            vec = elem.to_double_vector()
            return format_vector(vec) if vec is not None else None
        scalar = elem.to_double()
        return str(scalar) if scalar is not None else None
    if vr_value == dicom.VR.AT.value:
        if vm > 1:
            vec = elem.to_tag_vector()
            if vec is None:
                return None
            return "[" + ",".join(to_tag_string(tag) for tag in vec) + "]"
        single = elem.to_tag()
        return to_tag_string(single) if single is not None else None
    return None


def describe_element(elem: dicom.DataElement) -> str:
    tag = elem.tag
    vr = elem.vr
    keyword = dicom.tag_to_keyword(tag) or "-"
    vm = elem.vm
    line = (
        f"{to_tag_string(tag)} VR={vr} len={elem.length} "
        f"off={elem.offset} vm={vm} keyword={keyword}"
    )
    value = try_numeric(elem) or "[TODO]"
    full = f"{line} value={value}"
    return full if len(full) <= MAX_LINE else full[: MAX_LINE - 3] + "..."


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Dump numeric values from a DICOM file")
    parser.add_argument("path", help="Path to a DICOM file")
    args = parser.parse_args(argv)

    dataset = dicom.read_file(args.path)
    for elem in dataset:
        print(describe_element(elem))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
