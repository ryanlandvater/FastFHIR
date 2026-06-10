# ============================================================
# FastFHIR Generator — CLI entry point.
#
# Invoked by the build systems (CMake/Bazel) and the test harness as:
#     python -m generator [--output-dir DIR] [--keep-specs]
#
# This replaces the path-coupled `python tools/generator/make_lib.py` call;
# `python -m generator` is location-independent, so internal file moves never
# break the build invocation.
# ============================================================

from __future__ import annotations

import argparse

from generator import pipeline


def main() -> None:
    parser = argparse.ArgumentParser(prog="generator", description="FastFHIR C++ code generator")
    parser.add_argument(
        "--output-dir",
        default="generated_src",
        help="destination directory for generated C++ (default: generated_src)",
    )
    parser.add_argument(
        "--keep-specs",
        action="store_true",
        help="do not delete fhir_specs/ after generation",
    )
    args = parser.parse_args()
    pipeline.run(output_dir=args.output_dir, keep_specs=args.keep_specs)


if __name__ == "__main__":
    main()
