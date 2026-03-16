#!/usr/bin/env python3
"""Demonstrate the recommended DataSet access patterns."""

from __future__ import annotations

import dicomsdl as dicom


def main() -> int:
    ds = dicom.DataSet()

    rows = ds.ensure_dataelement("Rows", dicom.VR.US)
    rows.value = 512

    assert ds.set_value("Columns", 256)
    assert ds.set_value(0x00090030, dicom.VR.US, 16)

    preserved = ds.ensure_dataelement("Rows", dicom.VR.UL)

    print(f"Rows VR after ensure(UL): {preserved.vr}")
    print(f"Rows value: {ds.get_value('Rows')}")
    print(f"Columns value: {ds['Columns'].value}")
    print(f"Private value: {ds[0x00090030].value}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
