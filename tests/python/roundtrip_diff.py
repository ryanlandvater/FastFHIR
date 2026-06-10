"""
Round-trip DOM diff engine.

Compares a parsed FHIR JSON input against the re-serialised output of
FastFHIR's print_json after an ingest → FFHR → re-parse pipeline.

Designed to collect ALL differences (not short-circuit on first mismatch)
and report structured DiffEntry records for systematic triage.
"""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from enum import Enum, auto
from typing import Any


class DiffKind(Enum):
    MISSING_KEY = auto()       # key present in input but missing from output
    EXTRA_KEY = auto()         # key present in output but not in input
    VALUE_MISMATCH = auto()    # values differ (coerced to string for comparison)
    TYPE_MISMATCH = auto()     # type differs (e.g. string vs int, array vs object)
    ARRAY_LENGTH = auto()      # array lengths differ
    NULL_VS_ABSENT = auto()    # null vs key-not-present


@dataclass
class DiffEntry:
    path: str               # JSON pointer, e.g. "/Patient/name/0/given"
    kind: DiffKind          # category of difference
    expected: Any = None    # value from input DOM
    actual: Any = None      # value from output DOM
    message: str = ""       # human-readable explanation


# ─── Allowlist ───────────────────────────────────────────────────────────────
# Known cosmetic / intentional differences between input FHIR JSON and
# print_json output.  Add entries here as triage progresses so the
# CI test can assert ZERO unexpected diffs.

ALLOWED_DIFFS: set[tuple[str, DiffKind]] = {
    # Examples (remove these once real allowlist entries are triaged):
    # ("/Patient/extension", DiffKind.EXTRA_KEY),      # extensions reconstructed from native blocks
    # ("/Patient/meta/versionId", DiffKind.MISSING_KEY), # versioning not stored
}

EXPECTED_ORDER_INSENSITIVE_PATHS: set[str] = {
    # JSON object key order is semantically irrelevant in FHIR.
    # Every path that is an object's child is implicitly order-insensitive.
    # This set is for array elements where order does not matter.
    # Example:
    # "/Patient/identifier",  # identifier array order is not significant
}


# ─── Helpers ─────────────────────────────────────────────────────────────────

def _json_type_name(v: Any) -> str:
    if v is None:
        return "null"
    if isinstance(v, bool):
        return "bool"
    if isinstance(v, int):
        return "int"
    if isinstance(v, float):
        return "float"
    if isinstance(v, str):
        return "string"
    if isinstance(v, list):
        return "array"
    if isinstance(v, dict):
        return "object"
    return type(v).__name__


def _stringify(v: Any) -> str:
    """Normalise a value to string for comparison."""
    if v is None:
        return "null"
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, float):
        # Normalise float representation: strip trailing zeros
        return f"{v}".rstrip("0").rstrip(".")
    return str(v)


def _path_join(parent: str, child: str | int) -> str:
    if isinstance(child, int):
        return f"{parent}/{child}"
    # Escape ~ and / in JSON Pointer
    escaped = child.replace("~", "~0").replace("/", "~1")
    return f"{parent}/{escaped}"


# ─── Core diff logic ─────────────────────────────────────────────────────────

def diff_doms(
    expected: Any,
    actual: Any,
    path: str = "",
) -> list[DiffEntry]:
    """Recursively compare two JSON-deserialised DOM trees.

    Returns a flat list of DiffEntry records describing every difference.
    Order of entries is depth-first traversal order of the expected DOM.
    """
    diffs: list[DiffEntry] = []

    # Type mismatch (fundamental)
    if type(expected) is not type(actual):
        # Special case: int vs float (JSON parsers may deserialise 0 as int or float)
        if isinstance(expected, (int, float)) and isinstance(actual, (int, float)):
            # Allow numeric coercion
            if float(expected) != float(actual):
                diffs.append(DiffEntry(
                    path=path, kind=DiffKind.VALUE_MISMATCH,
                    expected=expected, actual=actual,
                    message=f"numeric values differ: {expected} vs {actual}",
                ))
            return diffs

        if expected is None and actual is not None:
            diffs.append(DiffEntry(
                path=path, kind=DiffKind.NULL_VS_ABSENT,
                expected=None, actual=actual,
                message="input has null, output has value",
            ))
            return diffs
        if expected is not None and actual is None:
            diffs.append(DiffEntry(
                path=path, kind=DiffKind.NULL_VS_ABSENT,
                expected=expected, actual=None,
                message="input has value, output has null",
            ))
            return diffs

        diffs.append(DiffEntry(
            path=path, kind=DiffKind.TYPE_MISMATCH,
            expected=_json_type_name(expected),
            actual=_json_type_name(actual),
            message=f"type mismatch: {_json_type_name(expected)} vs {_json_type_name(actual)}",
        ))
        return diffs

    # Scalars
    if not isinstance(expected, (dict, list)):
        if _stringify(expected) != _stringify(actual):
            diffs.append(DiffEntry(
                path=path, kind=DiffKind.VALUE_MISMATCH,
                expected=expected, actual=actual,
                message=f"values differ: {_stringify(expected)} vs {_stringify(actual)}",
            ))
        return diffs

    # Arrays
    if isinstance(expected, list):
        if len(expected) != len(actual):
            diffs.append(DiffEntry(
                path=path, kind=DiffKind.ARRAY_LENGTH,
                expected=len(expected), actual=len(actual),
                message=f"array length: {len(expected)} vs {len(actual)}",
            ))
        # Compare elementwise up to min length
        for i in range(min(len(expected), len(actual))):
            item_path = _path_join(path, i)
            diffs.extend(diff_doms(expected[i], actual[i], item_path))
        return diffs

    # Objects
    if isinstance(expected, dict):
        expected_keys = set(expected.keys())
        actual_keys = set(actual.keys())

        # Keys in expected but missing from actual
        for key in sorted(expected_keys - actual_keys):
            item_path = _path_join(path, key)
            diffs.append(DiffEntry(
                path=item_path, kind=DiffKind.MISSING_KEY,
                expected=expected[key], actual=None,
                message=f"key '{key}' missing from output",
            ))

        # Keys in actual but not in expected
        for key in sorted(actual_keys - expected_keys):
            item_path = _path_join(path, key)
            diffs.append(DiffEntry(
                path=item_path, kind=DiffKind.EXTRA_KEY,
                expected=None, actual=actual[key],
                message=f"unexpected key '{key}' in output",
            ))

        # Keys in both — recurse
        for key in sorted(expected_keys & actual_keys):
            item_path = _path_join(path, key)
            diffs.extend(diff_doms(expected[key], actual[key], item_path))

        return diffs

    return diffs


# ─── Filtering ───────────────────────────────────────────────────────────────

def filter_allowlisted(diffs: list[DiffEntry]) -> list[DiffEntry]:
    """Remove diffs that match known allowlist entries.

    Returns only the diffs that represent genuine (unexpected) regressions.
    """
    return [
        d for d in diffs
        if (d.path, d.kind) not in ALLOWED_DIFFS
    ]


# ─── Reporting ───────────────────────────────────────────────────────────────

def format_diff_report(diffs: list[DiffEntry]) -> str:
    """Pretty-print a diff list for test output."""
    if not diffs:
        return "  ✅ No differences found."

    lines = [f"  ❌ {len(diffs)} difference(s):"]
    for d in diffs:
        lines.append(f"     [{d.kind.name:14}] {d.path}")
        if d.message:
            lines.append(f"                      {d.message}")
    return "\n".join(lines)


# ─── Self-test ───────────────────────────────────────────────────────────────

if __name__ == "__main__":
    # Quick smoke test
    a = {"a": 1, "b": [{"c": "hello"}], "d": None}
    b = {"a": "1", "b": [{"c": "world", "e": 2}], "d": "present"}
    diffs = diff_doms(a, b)
    print(format_diff_report(diffs))
    print()
    filtered = filter_allowlisted(diffs)
    print(f"After filtering: {len(filtered)} diffs remain")
    print(f"  (all {len(diffs) - len(filtered)} were allowlisted)")
