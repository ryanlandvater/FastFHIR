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
from roundtrip_debug import annotate, drop_debug_artifacts, strip_debug

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
    debug: bool = False,
) -> tuple[bool, list[DiffEntry], str]:
    """Run one round-trip test on a Synthea fixture.

    Invokes the C++ ff_roundtrip harness to:
      ingest FHIR JSON → seal → re-parse → print_json

    Then diffs the output DOM against the input DOM via diff_doms().

    With `debug=True` the harness runs `--debug` instead, so every value arrives
    wrapped in the wire metadata it was decoded from (Node::to_debug_json).  The
    envelope is stripped back to a plain DOM before diffing -- so the comparison
    is the same one -- and the third return value carries a report naming the
    tag, kind and byte offset behind each diff.  Requires a Debug build; the
    harness exits 2 under NDEBUG.

    In debug mode the check is CONTAINMENT: every field in the source FHIR must
    survive.  The debug dump carries more than print_json does by design, so
    extra keys are not failures.
    """
    # Load input
    with open(fixture_path, "r") as f:
        input_json = f.read()

    argv = [harness_path, str(fixture_path)]
    if debug:
        argv.append("--debug")

    # Invoke C++ harness
    try:
        result = subprocess.run(
            argv,
            capture_output=True,
            text=True,
            timeout=120,
        )
    except FileNotFoundError:
        return (
            False,
            [
                DiffEntry(
                    path="",
                    kind=DiffKind.VALUE_MISMATCH,
                    expected=None,
                    actual=None,
                    message=f"C++ harness not found: {harness_path}. Build with: "
                    "cmake --build . --target ff_roundtrip",
                )
            ],
            "",
        )
    except subprocess.TimeoutExpired:
        return (
            False,
            [
                DiffEntry(
                    path="",
                    kind=DiffKind.VALUE_MISMATCH,
                    expected=None,
                    actual=None,
                    message=f"Harness timed out after 120s on {fixture_path.name}",
                )
            ],
            "",
        )

    if result.returncode == 2 and debug:
        return (
            False,
            [
                DiffEntry(
                    path="",
                    kind=DiffKind.VALUE_MISMATCH,
                    expected=None,
                    actual=None,
                    message="--debug needs a Debug build; to_debug_json is compiled out under NDEBUG",
                )
            ],
            "",
        )

    if result.returncode != 0:
        return (
            False,
            [
                DiffEntry(
                    path="",
                    kind=DiffKind.VALUE_MISMATCH,
                    expected=None,
                    actual=None,
                    message=f"Harness exited code {result.returncode}: {result.stderr.strip()}",
                )
            ],
            "",
        )

    output_json = result.stdout

    # Parse both DOM trees
    try:
        input_dom = json.loads(input_json)
    except json.JSONDecodeError as e:
        return (
            False,
            [
                DiffEntry(
                    path="",
                    kind=DiffKind.VALUE_MISMATCH,
                    expected=None,
                    actual=None,
                    message=f"Failed to parse input JSON: {e}",
                )
            ],
            "",
        )

    try:
        output_dom = json.loads(output_json)
    except json.JSONDecodeError as e:
        return (
            False,
            [
                DiffEntry(
                    path="",
                    kind=DiffKind.VALUE_MISMATCH,
                    expected=None,
                    actual=None,
                    message=f"Failed to parse output JSON: {e}",
                )
            ],
            "",
        )

    # In debug mode the DOM arrives wrapped in wire metadata. Strip it back to
    # the shape print_json would have produced, so exactly one differ is used
    # for both modes and the two cannot drift apart.
    meta: dict = {}
    if debug:
        output_dom, meta = strip_debug(output_dom)

    diffs = diff_doms(input_dom, output_dom)
    diffs = filter_allowlisted(diffs)
    if debug:
        diffs = drop_debug_artifacts(diffs, meta)

    report = annotate(diffs, meta) if debug else ""
    return len(diffs) == 0, diffs, report


# ─── Main ────────────────────────────────────────────────────────────────────


def main() -> int:
    parser = argparse.ArgumentParser(description="FastFHIR round-trip DOM parity test")
    parser.add_argument(
        "--synthea-dir",
        default=DEFAULT_SYNTHEA_DIR,
        help="Path to Synthea FHIR output directory (default: $FASTFHIR_SYNTHEA_DIR)",
    )
    parser.add_argument(
        "--fixture",
        default=None,
        help="Test a single fixture file instead of discovering all",
    )
    parser.add_argument(
        "--harness",
        default="ff_roundtrip",
        help="Path to the ff_roundtrip C++ binary (default: ff_roundtrip)",
    )
    parser.add_argument(
        "--debug",
        action="store_true",
        help="Always diff against to_debug_json output, reporting the recovery "
        "tag, field kind and byte offset behind each difference. Debug builds "
        "only. The dump is ~3x the document size and costs ~2.75x the runtime "
        "(249s vs 90s over 342 fixtures), so prefer --debug-on-failure unless "
        "you want the metadata for passing fixtures too.",
    )
    parser.add_argument(
        "--debug-on-failure",
        action="store_true",
        help="Run plainly, then re-run ONLY the fixtures that failed with "
        "to_debug_json to explain them. Free when the suite is green, which is "
        "why ctest uses it.",
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

        ok, diffs, report = run_roundtrip_test(fx, harness_path=args.harness, debug=args.debug)
        # Explaining a failure is worth a second pass; explaining a pass is not.
        # Re-running only the failures keeps the wire diagnostics free once the
        # suite is green, instead of taxing every run forever (DBG-1.4).
        if not ok and not args.debug and args.debug_on_failure:
            _, _, report = run_roundtrip_test(fx, harness_path=args.harness, debug=True)
        if ok:
            print("  PASS")
            passed += 1
        else:
            print("  FAIL")
            # In debug mode the annotated report supersedes the plain one: it is
            # the same diff list with the wire metadata behind each entry.
            print(report if report else format_diff_report(diffs))
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
