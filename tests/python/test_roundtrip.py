#!/usr/bin/env python3
"""
FastFHIR Round-Trip DOM Parity Test.

For each Synthea fixture bundle found under FASTFHIR_SYNTHEA_DIR:
  1. Ingest the FHIR JSON into a Memory arena
  2. Seal and re-parse
  3. Capture print_json output
  4. Compare input DOM against output DOM via diff_doms()
  5. Report PASS / DIFFS / ERROR per fixture

Usage:
    python tests/python/test_roundtrip.py              # uses default paths
    python tests/python/test_roundtrip.py --synthea-dir /path/to/synthea/fhir

Exit code: 0 if all fixtures PASS, 1 if any fixture has diffs or errors.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

# Add project root to path for imports
PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(PROJECT_ROOT / "tests" / "python"))

from roundtrip_diff import (
    diff_doms,
    filter_allowlisted,
    format_diff_report,
    DiffEntry,
    DiffKind,
)

# ─── Configuration ───────────────────────────────────────────────────────────

# Default: resolve from environment (set by CMake at build/configure time)
DEFAULT_SYNTHEA_DIR = os.environ.get("FASTFHIR_SYNTHEA_DIR", "")

# How large a memory arena to allocate for ingest (256 MB)
ARENA_SIZE = 256 * 1024 * 1024


# ─── C++ harness wrapper ────────────────────────────────────────────────────

def _build_roundtrip_tool() -> Path:
    """Build the C++ round-trip helper if it doesn't exist.

    Returns path to the built binary.
    """
    # For Phase 1, we invoke the test_readme binary with a filter flag,
    # or use a small standalone C++ program compiled on the fly.
    # This is a placeholder — Phase 1 uses Python-driven subprocess calls
    # to a small C++ helper that does ingest → seal → print_json.
    raise NotImplementedError("C++ harness not yet built — Phase 1 uses Python-driven approach")

# ─── Fixture discovery ───────────────────────────────────────────────────────

def discover_fixtures(synthea_dir: str) -> list[Path]:
    """Return sorted list of .json fixture paths under synthea_dir."""
    root = Path(synthea_dir)
    if not root.is_dir():
        return []

    # Synthea output lands in a 'fhir/' sub-directory; probe both
    candidates: list[Path] = []
    for search_dir in [root / "fhir", root]:
        if not search_dir.is_dir():
            continue
        for entry in sorted(search_dir.iterdir()):
            if entry.suffix == ".json":
                candidates.append(entry)
    return candidates


# ─── Per-fixture round-trip test ─────────────────────────────────────────────

def run_roundtrip_test(
    fixture_path: Path,
    *,
    harness_path: str = "ff_roundtrip",
) -> tuple[bool, list[DiffEntry]]:
    """Run one round-trip test on a Synthea fixture.

    Invokes the C++ ff_roundtrip harness to:
      ingest FHIR JSON → seal → re-parse → print_json

    Then diffs the output DOM against the input DOM via diff_doms().
    """
    # Load input
    with open(fixture_path, "r") as f:
        input_json = f.read()

    # Invoke C++ harness
    try:
        result = subprocess.run(
            [harness_path, str(fixture_path)],
            capture_output=True, text=True, timeout=120,
        )
    except FileNotFoundError:
        return False, [DiffEntry(
            path="", kind=DiffKind.VALUE_MISMATCH,
            expected=None, actual=None,
            message=f"C++ harness not found: {harness_path}. Build with: "

            "cmake --build . --target ff_roundtrip",
        )]
    except subprocess.TimeoutExpired:
        return False, [DiffEntry(
            path="", kind=DiffKind.VALUE_MISMATCH,
            expected=None, actual=None,
            message=f"Harness timed out after 120s on {fixture_path.name}",
        )]

    if result.returncode != 0:
        return False, [DiffEntry(
            path="", kind=DiffKind.VALUE_MISMATCH,
            expected=None, actual=None,
            message=f"Harness exited code {result.returncode}: {result.stderr.strip()}",
        )]

    output_json = result.stdout

    # Parse both DOM trees
    try:
        input_dom = json.loads(input_json)
    except json.JSONDecodeError as e:
        return False, [DiffEntry(
            path="", kind=DiffKind.VALUE_MISMATCH,
            expected=None, actual=None,
            message=f"Failed to parse input JSON: {e}",
        )]

    try:
        output_dom = json.loads(output_json)
    except json.JSONDecodeError as e:
        return False, [DiffEntry(
            path="", kind=DiffKind.VALUE_MISMATCH,
            expected=None, actual=None,
            message=f"Failed to parse output JSON: {e}",
        )]

    # Diff
    diffs = diff_doms(input_dom, output_dom)
    diffs = filter_allowlisted(diffs)
    return len(diffs) == 0, diffs


# ─── Main ────────────────────────────────────────────────────────────────────

def main() -> int:
    parser = argparse.ArgumentParser(description="FastFHIR round-trip DOM parity test")
    parser.add_argument(
        "--synthea-dir", default=DEFAULT_SYNTHEA_DIR,
        help="Path to Synthea FHIR output directory (default: $FASTFHIR_SYNTHEA_DIR)",
    )
    parser.add_argument(
        "--fixture", default=None,
        help="Test a single fixture file instead of discovering all",
    )
    parser.add_argument(
        "--harness", default="ff_roundtrip",
        help="Path to the ff_roundtrip C++ binary (default: ff_roundtrip)",
    )
    args = parser.parse_args()

    # Resolve fixtures
    if args.fixture:
        fixtures = [Path(args.fixture)]
    else:
        fixtures = discover_fixtures(args.synthea_dir)
        if not fixtures:
            print(f"No Synthea fixtures found under '{args.synthea_dir}'")
            print("Set FASTFHIR_SYNTHEA_DIR or pass --synthea-dir")
            # Not a failure — tests may be skipped in CI without Synthea data
            return 0

    # Run per-fixture
    passed = 0
    failed = 0
    errors: list[tuple[Path, list[DiffEntry]]] = []

    for fx in fixtures:
        print(f"\n{'='*60}")
        print(f"Fixture: {fx.name}")
        print(f"{'='*60}")

        ok, diffs = run_roundtrip_test(fx, harness_path=args.harness)
        if ok:
            print("  PASS")
            passed += 1
        else:
            print("  FAIL")
            print(format_diff_report(diffs))
            failed += 1
            errors.append((fx, diffs))

    # Summary
    total = passed + failed
    print(f"\n{'='*60}")
    print(f"Results: {passed}/{total} passed, {failed}/{total} failed")
    if failed == 0:
        print("✅ All round-trip tests passed.")
        return 0
    else:
        print(f"❌ {failed} fixture(s) had unexpected differences.")
        return 1


if __name__ == "__main__":
    sys.exit(main())
