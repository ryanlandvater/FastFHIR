#!/usr/bin/env python3
"""Compatibility launcher for FastFHIR code generation.

Keeps the historical root command working:
    python3 make_lib.py
"""

import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / "tools"))

from generator.make_lib import main


if __name__ == "__main__":
    main()
