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
    count_leaves,
    diff_doms,
    filter_allowlisted,
    format_diff_report,
    DiffEntry,
    DiffKind,
    DiffStats,
)
from roundtrip_debug import annotate, drop_debug_artifacts, strip_debug

# ─── Configuration ───────────────────────────────────────────────────────────

# Default: resolve from environment (set by CMake at build/configure time)
DEFAULT_SYNTHEA_DIR = os.environ.get("FASTFHIR_SYNTHEA_DIR", "")

# How large a memory arena to allocate for ingest (256 MB)
ARENA_SIZE = 256 * 1024 * 1024

# Exit code for "the gate could not run" (no corpus on this machine). Registered
# as py_roundtrip's SKIP_RETURN_CODE in tests/tests.cmake, so ctest reports the
# run as Skipped instead of Passed. 77 is the automake/ctest convention.
SKIP_RETURN_CODE = 77


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


def _harness_failure(message: str) -> tuple[bool, list[DiffEntry], str, DiffStats]:
    """A fixture that never reached the diff: one finding, no report, empty stats.

    Same four-tuple as the success path. These returns once carried three
    values after `stats` was added, so a missing harness surfaced as an
    unpacking ValueError in main() instead of this message.
    """
    return (
        False,
        [
            DiffEntry(
                path="",
                kind=DiffKind.VALUE_MISMATCH,
                expected=None,
                actual=None,
                message=message,
            )
        ],
        "",
        DiffStats(),
    )


def run_roundtrip_test(
    fixture_path: Path,
    *,
    harness_path: str = "ff_roundtrip",
    debug: bool = False,
    timeout: float = 120.0,
) -> tuple[bool, list[DiffEntry], str, DiffStats]:
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

    Returns (ok, diffs, report, stats).  `stats` carries how much of the SOURCE
    the comparison actually reached -- see DiffStats: zero diffs over zero
    compared values is not a passing round-trip, it is an untested one.

    `timeout` exists so the error-path tests (test_roundtrip_errors.py) can
    reach the TimeoutExpired handler without waiting two minutes.
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
            timeout=timeout,
        )
    except FileNotFoundError:
        return _harness_failure(
            f"C++ harness not found: {harness_path}. Build with: "
            "cmake --build . --target ff_roundtrip"
        )
    except PermissionError:
        # The path exists but cannot be executed (a directory, or a file
        # without the execute bit). Uncaught, this was a traceback out of main().
        return _harness_failure(f"C++ harness is not executable: {harness_path}")
    except subprocess.TimeoutExpired:
        return _harness_failure(
            f"Harness timed out after {timeout:g}s on {fixture_path.name}"
        )

    if result.returncode == 2 and debug:
        return _harness_failure(
            "--debug needs a Debug build; to_debug_json is compiled out under NDEBUG"
        )

    if result.returncode != 0:
        return _harness_failure(
            f"Harness exited code {result.returncode}: {result.stderr.strip()}"
        )

    output_json = result.stdout

    # Parse both DOM trees
    try:
        input_dom = json.loads(input_json)
    except json.JSONDecodeError as e:
        return _harness_failure(f"Failed to parse input JSON: {e}")

    try:
        output_dom = json.loads(output_json)
    except json.JSONDecodeError as e:
        return _harness_failure(f"Failed to parse output JSON: {e}")

    # In debug mode the DOM arrives wrapped in wire metadata. Strip it back to
    # the shape print_json would have produced, so exactly one differ is used
    # for both modes and the two cannot drift apart.
    meta: dict = {}
    if debug:
        output_dom, meta = strip_debug(output_dom)

    # Coverage is measured, not assumed.  `len(diffs) == 0` scores a walk that
    # compared 38,861 leaves and one that compared none identically --
    # diff_doms({}, {}) returns zero diffs -- so ask the second question too:
    # did the comparison actually reach every value in the source document?
    stats = DiffStats(input_leaves=count_leaves(input_dom))
    diffs = diff_doms(input_dom, output_dom, stats=stats)
    diffs = filter_allowlisted(diffs)
    if debug:
        diffs = drop_debug_artifacts(diffs, meta)
    diffs.extend(_coverage_diffs(stats))

    report = annotate(diffs, meta) if debug else ""
    return len(diffs) == 0, diffs, report, stats


def _coverage_diffs(stats: DiffStats) -> list[DiffEntry]:
    """Turn a shortfall in comparison coverage into ordinary findings.

    Two separate claims, because they fail for different reasons:

    * `input_leaves == 0` -- the fixture carried nothing to compare.  A pass here
      is a pass on ZERO coverage, which is the reporting failure TASKS.md P0-2 is
      about, and it is how a corpus that silently stops loading keeps reporting
      green.
    * `unvisited > 0` -- the walk never reached part of the source.  Every early
      return in diff_doms already emits its own finding, so on a healthy corpus
      this is redundant with those; it fires ALONE only when the walk itself is
      at fault rather than the data, which is the case nothing else can see.
    """
    if stats.input_leaves == 0:
        return [DiffEntry(
            path="", kind=DiffKind.COVERAGE_SHORTFALL, expected=0, actual=0,
            message="source document has no comparable values -- nothing was tested",
        )]
    if stats.unvisited > 0:
        return [DiffEntry(
            path="", kind=DiffKind.COVERAGE_SHORTFALL,
            expected=stats.input_leaves, actual=stats.compared,
            message=(
                f"comparison reached {stats.compared}/{stats.input_leaves} source values; "
                f"{stats.unvisited} were never examined"
            ),
        )]
    return []


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

    # Resolve fixtures.
    #
    # COV-2 (P0-2): "found nothing" must never read as "passed". This returned 0
    # for an empty discovery, which is the exact shape of the 2026-08-13 vacuous
    # pass: a temporary Bundle filter in discover_fixtures excluded every
    # fixture, the gate ran nothing, and ctest reported PASS. Two cases, told
    # apart because they mean different things:
    #   - no corpus on this machine (unset, or the directory does not exist):
    #     the gate could not run. Exit SKIP_RETURN_CODE, which tests.cmake
    #     registers, so ctest reports "Skipped" -- visible, and not a pass.
    #   - a corpus directory that yields zero fixtures: something between the
    #     corpus and the gate is broken. That is a failure.
    if args.fixture:
        fixtures = [Path(args.fixture)]
    else:
        if not args.synthea_dir or not Path(args.synthea_dir).is_dir():
            print(f"SKIP: no Synthea corpus at '{args.synthea_dir}' -- round-trip gate NOT RUN")
            print("Set FASTFHIR_SYNTHEA_DIR or pass --synthea-dir")
            return SKIP_RETURN_CODE
        fixtures = discover_fixtures(args.synthea_dir)
        if not fixtures:
            print(f"FAIL: Synthea directory '{args.synthea_dir}' exists but yielded 0 fixtures")
            print("A gate that finds nothing to check has checked nothing (P0-2).")
            return 1

    # Run per-fixture
    passed = 0
    failed = 0
    total_compared = 0
    errors: list[tuple[Path, list[DiffEntry]]] = []

    for fx in fixtures:
        print(f"\n{'='*60}")
        print(f"Fixture: {fx.name}")
        print(f"{'='*60}")

        ok, diffs, report, stats = run_roundtrip_test(
            fx, harness_path=args.harness, debug=args.debug)
        # Explaining a failure is worth a second pass; explaining a pass is not.
        # Re-running only the failures keeps the wire diagnostics free once the
        # suite is green, instead of taxing every run forever (DBG-1.4).
        if not ok and not args.debug and args.debug_on_failure:
            _, _, report, _ = run_roundtrip_test(fx, harness_path=args.harness, debug=True)
        if ok:
            # Printed, not merely asserted: "it passed" and "it compared
            # anything" are different claims, and this suite has already spent a
            # session being the second while reading as the first.
            print(f"  PASS  ({stats.compared} source values compared, all present)")
            passed += 1
            total_compared += stats.compared
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
    if failed != 0:
        print(f"❌ {failed} fixture(s) had unexpected differences.")
        return 1
    # The per-fixture floor (coverage findings) already fails a fixture that
    # compared nothing; this is the corpus-level half, so a run whose total is
    # zero cannot be reported as a pass however it got there (COV-2).
    if total_compared == 0:
        print("❌ Every fixture passed but 0 source values were compared -- nothing was tested.")
        return 1
    print(f"✅ All round-trip tests passed — {total_compared} source values compared, "
          f"every one present in the round-trip.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
