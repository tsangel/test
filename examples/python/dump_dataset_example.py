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
UI_VR = dicom.VR.UI.value


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
    if vr_value == UI_VR:
        text = elem.to_uid_string()
        if text is None:
            return None
        uid = dicom.lookup_uid(text)
        if uid is not None:
            keyword = uid.keyword or "-"
            return (
                f"UID(value='{uid.value}', keyword='{keyword}', "
                f"name='{uid.name}', type='{uid.type}')"
            )
        return f"UID(value='{text}')"
    if vr_value == dicom.VR.AT.value:
        if vm > 1:
            vec = elem.to_tag_vector()
            if vec is None:
                return None
            return "[" + ",".join(to_tag_string(tag) for tag in vec) + "]"
        single = elem.to_tag()
        return to_tag_string(single) if single is not None else None
    return None


def describe_element(elem: dicom.DataElement, indent: int = 0) -> str:
    tag = elem.tag
    vr = elem.vr
    keyword = dicom.tag_to_keyword(tag) or "-"
    vm = elem.vm
    indent_str = " " * indent
    line = (
        f"{to_tag_string(tag)} VR={vr} len={elem.length} "
        f"off={elem.offset} vm={vm} keyword={keyword}"
    )
    if elem.is_pixel_sequence:
        value = "PixelSequence (encapsulated pixel data)"
    else:
        value = try_numeric(elem) or "[TODO]"
    full = f"{indent_str}{line} value={value}"
    return full if len(full) <= MAX_LINE else full[: MAX_LINE - 3] + "..."


def dump_dataset(dataset: dicom.DataSet, indent: int = 0) -> None:
    for elem in dataset:
        print(describe_element(elem, indent))
        if elem.is_sequence:
            seq = elem.sequence
            if seq is None:
                continue
            for idx, item in enumerate(seq):
                print(" " * (indent + 2) + f"Item[{idx}] {{")
                dump_dataset(item, indent + 4)
                print(" " * (indent + 2) + "}")
        elif elem.is_pixel_sequence:
            pixseq = elem.pixel_sequence
            if pixseq is None:
                continue
            indent2 = " " * (indent + 2)
            bot = pixseq.basic_offset_table_count
            if bot == 0:
                print(indent2 + "BasicOffsetTable: none")
            else:
                print(indent2 + f"BasicOffsetTable: offset=0x{pixseq.basic_offset_table_offset:x} count={bot}")
            for fi in range(pixseq.number_of_frames):
                frame = pixseq.frame(fi)
                frags = frame.fragments
                print(indent2 + f"Frame[{fi}] fragments={len(frags)} encoded_size={frame.encoded_size}")
                for fj, frag in enumerate(frags):
                    print(indent2 + f"  frag[{fj}] offset=0x{frag.offset:x} length={frag.length}")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Dump numeric values from a DICOM file")
    parser.add_argument("path", help="Path to a DICOM file")
    args = parser.parse_args(argv)

    dicom_file = dicom.read_file(args.path)
    dump_dataset(dicom_file.dataset)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
