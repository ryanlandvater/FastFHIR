"""Wire-format stability gate for the generator refactor.

This is the ONE hard gate (generator_refactor_plan.md (deleted; in git history) section 6). It asserts
that the wire-relevant constants emitted by the generator match a committed
baseline witness. C++ source formatting is explicitly NOT checked here.

Run:  pytest tests/generator/test_wire_format.py -q

Baseline regeneration (only when a wire change is intentional and reviewed):
    python -m tests.generator.wire_witness generated_src \
        tests/generator/golden/wire_witness.json
"""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from wire_witness import witness, _check_permanence

_HERE = Path(__file__).parent
_BASELINE = _HERE / "golden" / "wire_witness.json"


@pytest.fixture(scope="session")
def baseline() -> dict:
    if not _BASELINE.exists():
        pytest.skip(
            f"no baseline witness at {_BASELINE} — generate it first with "
            "`python -m tests.generator.wire_witness generated_src "
            "tests/generator/golden/wire_witness.json`"
        )
    return json.loads(_BASELINE.read_text(encoding="utf-8"))


def test_recovery_tags_stable(regenerated_dir: Path, baseline: dict) -> None:
    """RECOVER_FF_* tags hit bytes 8-9 of every block — must never drift."""
    current = witness(regenerated_dir)["tags"]
    drift = _symmetric_diff(current, baseline["tags"])
    assert not drift, f"recovery-tag wire drift: {drift}"


def test_dictionary_codes_stable(regenerated_dir: Path, baseline: dict) -> None:
    """Hash-based FF_*_CODE_* values are serialized in FF_CODE blocks."""
    current = witness(regenerated_dir)["codes"]
    drift = _symmetric_diff(current, baseline["codes"])
    assert not drift, f"dictionary-code wire drift: {drift}"


def test_vtable_layout_stable(regenerated_dir: Path, baseline: dict) -> None:
    """Field order + per-field size constant + HEADER_*_SIZE determine every
    byte offset. A change here re-lays-out the V-Table and breaks old streams."""
    current = witness(regenerated_dir)["vtables"]
    expected = baseline["vtables"]
    assert set(current) == set(expected), (
        "block set changed: "
        f"added={set(current) - set(expected)} removed={set(expected) - set(current)}"
    )
    for block in sorted(expected):
        assert (
            current[block]["order"] == expected[block]["order"]
        ), f"{block}: field ORDER drifted (offsets shift) -> {current[block]['order']}"
        assert (
            current[block]["sizes"] == expected[block]["sizes"]
        ), f"{block}: field SIZE constants drifted -> {current[block]['sizes']}"
        assert (
            current[block]["header_sizes"] == expected[block]["header_sizes"]
        ), f"{block}: HEADER_*_SIZE drifted -> {current[block]['header_sizes']}"


def test_permanence_rejects_modification(regenerated_dir: Path, baseline: dict) -> None:
    """Modifying an existing wire constant must be rejected."""
    current = witness(regenerated_dir)
    # Modify a vtable size in the current output
    current["vtables"]["FF_PATIENT"]["sizes"]["ID_S"] = "TYPE_SIZE_UINT32"
    errors = _check_permanence(current["vtables"], baseline["vtables"], "vtables")
    assert errors, "expected permanence errors for modified field size"
    assert any("ID_S" in e and "0x" not in e for e in errors), f"expected ID_S in errors: {errors}"
    assert not any("order" in e for e in errors), "order should not have triggered"


def test_permanence_rejects_deletion(regenerated_dir: Path, baseline: dict) -> None:
    """Deleting an existing wire constant must be rejected."""
    current = witness(regenerated_dir)
    del current["vtables"]["FF_PATIENT"]["sizes"]["ID_S"]
    errors = _check_permanence(current["vtables"], baseline["vtables"], "vtables")
    assert errors, "expected permanence errors for deleted field size"
    assert any("DELETED" in e and "ID_S" in e for e in errors), f"expected DELETED ID_S: {errors}"


def test_permanence_accepts_addition(regenerated_dir: Path, baseline: dict) -> None:
    """Adding a NEW wire constant must be allowed (no permanence errors)."""
    current = witness(regenerated_dir)
    # Add a new block vtable entry that doesn't exist in the golden
    current["vtables"]["FF_NEW_BLOCK"] = {
        "order": ["VALIDATION", "RECOVERY", "ID"],
        "sizes": {"VALIDATION_S": "TYPE_SIZE_UINT64", "RECOVERY_S": "TYPE_SIZE_UINT16", "ID_S": "TYPE_SIZE_OFFSET"},
        "header_sizes": {"HEADER_R4_SIZE": 20, "HEADER_R5_SIZE": 20},
    }
    errors = _check_permanence(current["vtables"], baseline["vtables"], "vtables")
    assert not errors, f"additions should not produce permanence errors: {errors}"


def _symmetric_diff(a: dict, b: dict) -> dict:
    """Return {name: (current, baseline)} for every key that differs."""
    keys = set(a) | set(b)
    return {k: (a.get(k), b.get(k)) for k in keys if a.get(k) != b.get(k)}
