#!/usr/bin/env python3
"""Run dicomconv CLI from examples."""

from __future__ import annotations

import sys

from dicomsdl.dicomconv import main


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
