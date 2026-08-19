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
    DROPPED_RESOURCE = auto()  # Bundle entry in input with no counterpart in output
    ADDED_RESOURCE = auto()    # Bundle entry in output with no counterpart in input


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


# ─── Bundle entry identity ───────────────────────────────────────────────────
# Bundle.entry must be paired by identity, never by index. Resources that fail
# to round-trip are dropped from the output (out-of-profile types, TASKS.md
# A27), which shifts every later index -- so a positional walk compares
# unrelated resources and reports their every field as a difference. Measured
# 2026-08-19 over 12 Synthea fixtures: 28,002 positional diffs against 5,890
# identity-paired ones. 79% of the report was an artefact of the comparison.


def _entry_identity(entry: Any) -> tuple | None:
    """Stable identity for one Bundle entry, or None if it cannot be keyed.

    `(resourceType, id)` first because it is intrinsic to the resource and is
    therefore derivable on BOTH sides. `fullUrl` is only a fallback: the
    exporter does not currently emit it at all (TASKS.md A26), so keying on it
    first would match nothing and report every entry as dropped AND added.
    """
    if not isinstance(entry, dict):
        return None
    resource = entry.get("resource")
    if isinstance(resource, dict):
        rtype, rid = resource.get("resourceType"), resource.get("id")
        if rtype is not None and rid is not None:
            return ("rt-id", rtype, rid)
    full_url = entry.get("fullUrl")
    if isinstance(full_url, str):
        return ("fullUrl", full_url)
    return None


def _entry_label(entry: Any) -> str:
    ident = _entry_identity(entry)
    if ident is None:
        return "<unkeyable entry>"
    return f"{ident[1]}/{ident[2]}" if ident[0] == "rt-id" else str(ident[1])


def _is_entry_array(path: str, expected: Any, actual: Any) -> bool:
    """True for a Bundle.entry array on both sides (top-level or nested)."""
    if not path.endswith("/entry"):
        return False
    if not (isinstance(expected, list) and isinstance(actual, list)):
        return False
    both = [*expected, *actual]
    if not both:
        return False
    return all(isinstance(e, dict) and ("resource" in e or "fullUrl" in e) for e in both)


def _diff_entry_array(expected: list, actual: list, path: str) -> list[DiffEntry]:
    """Pair entries by identity, then diff only the matched pairs.

    Unmatched entries are reported as ONE finding each -- never walked as a
    subtree, which is precisely the cascade this function exists to prevent.
    No ARRAY_LENGTH is emitted: the dropped/added findings already carry it,
    and emitting both would double-report a single fact.
    """
    diffs: list[DiffEntry] = []

    # Multi-map: an identity is not guaranteed unique, so keep insertion order
    # and consume matches in order rather than assuming one entry per key.
    remaining: dict[tuple, list[int]] = {}
    for i, item in enumerate(actual):
        ident = _entry_identity(item)
        if ident is not None:
            remaining.setdefault(ident, []).append(i)

    matched_actual: set[int] = set()
    for i, exp_item in enumerate(expected):
        # Paths keep the INPUT index so a reader can find the entry in the
        # source document they already have open.
        item_path = _path_join(path, i)
        ident = _entry_identity(exp_item)
        slots = remaining.get(ident) if ident is not None else None
        if not slots:
            diffs.append(DiffEntry(
                path=item_path, kind=DiffKind.DROPPED_RESOURCE,
                expected=_entry_label(exp_item), actual=None,
                message=f"{_entry_label(exp_item)} present in input, absent from output",
            ))
            continue
        j = slots.pop(0)
        matched_actual.add(j)
        diffs.extend(diff_doms(exp_item, actual[j], item_path))

    for j, act_item in enumerate(actual):
        if j not in matched_actual:
            diffs.append(DiffEntry(
                path=_path_join(path, j), kind=DiffKind.ADDED_RESOURCE,
                expected=None, actual=_entry_label(act_item),
                message=f"{_entry_label(act_item)} present in output, absent from input",
            ))

    return diffs


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
        # Bundle.entry is paired by identity, not by index -- see above.
        if _is_entry_array(path, expected, actual):
            return _diff_entry_array(expected, actual, path)
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

_STRUCTURAL_KINDS = (DiffKind.DROPPED_RESOURCE, DiffKind.ADDED_RESOURCE)


def format_diff_report(diffs: list[DiffEntry]) -> str:
    """Pretty-print a diff list for test output.

    Structural findings lead. A handful of dropped resources is a single
    defect, but it used to be reported only as thousands of field diffs
    further down -- the summary is what keeps the cause visible above the
    symptoms.
    """
    if not diffs:
        return "  No differences found."

    structural = [d for d in diffs if d.kind in _STRUCTURAL_KINDS]
    field_diffs = [d for d in diffs if d.kind not in _STRUCTURAL_KINDS]

    lines = [f"  ERROR: {len(diffs)} difference(s):"]

    if structural:
        dropped = [d for d in structural if d.kind is DiffKind.DROPPED_RESOURCE]
        added = [d for d in structural if d.kind is DiffKind.ADDED_RESOURCE]
        lines.append(f"     ── structure: {len(dropped)} dropped, {len(added)} added ──")
        for label, group, side in (("DROPPED", dropped, "expected"),
                                   ("ADDED", added, "actual")):
            if not group:
                continue
            by_type: dict[str, int] = {}
            for d in group:
                name = str(getattr(d, side) or "?").split("/")[0]
                by_type[name] = by_type.get(name, 0) + 1
            summary = ", ".join(f"{k}x{v}" for k, v in sorted(by_type.items()))
            lines.append(f"     {label}: {summary}")
        if field_diffs:
            lines.append(f"     ── {len(field_diffs)} field difference(s) in matched resources ──")

    for d in field_diffs if structural else diffs:
        lines.append(f"     [{d.kind.name:16}] {d.path}")
        if d.message:
            lines.append(f"                        {d.message}")
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
