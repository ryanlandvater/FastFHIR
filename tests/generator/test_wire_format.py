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
    """RECOVER_FF_* tags hit bytes 8-9 of every block — must never drift.

    Permanence, not equality: a NEW tag is legal (adding a resource appends one,
    e.g. TASKS.md A27.5b's 61 billing tags), but changing or removing an existing
    one breaks every archive that used it.
    """
    current = witness(regenerated_dir)["tags"]
    errors = _check_permanence(current, baseline["tags"], "tags")
    assert not errors, "recovery-tag wire drift:\n" + "\n".join(errors)


def test_dictionary_codes_stable(regenerated_dir: Path, baseline: dict) -> None:
    """Permanent dictionary IDs from master_codes.json — the archive decode keys.

    Append-only, so additions pass and mutations do not. This is the invariant
    `118d6ad` broke once already, silently invalidating every stored archive; it
    went unguarded for as long as the witness captured an empty dict.
    """
    current = witness(regenerated_dir)["codes"]
    errors = _check_permanence(current, baseline["codes"], "codes")
    assert not errors, "dictionary-code wire drift:\n" + "\n".join(errors)


def test_vtable_layout_stable(regenerated_dir: Path, baseline: dict) -> None:
    """Field order + per-field size constant + HEADER_*_SIZE determine every
    byte offset. A change here re-lays-out the V-Table and breaks old streams.

    The rule is *prefix*, not identity: the generator may add blocks and append
    fields — that is the normal growth case — but every shipped block, field,
    size constant and header size must be unchanged. Delegated to
    _check_permanence so this gate and the golden-writer can never disagree
    about the rule again (the disagreement was A16).
    """
    current = witness(regenerated_dir)["vtables"]
    expected = baseline["vtables"]
    errors = _check_permanence(current, expected, "vtables")
    assert not errors, "wire-format drift against shipped golden:\n" + "\n".join(errors)


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
        "sizes": {
            "VALIDATION_S": "TYPE_SIZE_UINT64",
            "RECOVERY_S": "TYPE_SIZE_UINT16",
            "ID_S": "TYPE_SIZE_OFFSET",
        },
        "header_sizes": {"HEADER_R4_SIZE": 20, "HEADER_R5_SIZE": 20},
    }
    errors = _check_permanence(current["vtables"], baseline["vtables"], "vtables")
    assert not errors, f"additions should not produce permanence errors: {errors}"


def test_permanence_rejects_field_reorder(regenerated_dir: Path, baseline: dict) -> None:
    """Reordering two shipped fields shifts offsets — must be rejected."""
    current = witness(regenerated_dir)
    order = current["vtables"]["FF_PATIENT"]["order"]
    order[0], order[1] = order[1], order[0]
    errors = _check_permanence(current["vtables"], baseline["vtables"], "vtables")
    assert errors, "expected permanence errors for reordered fields"
    assert any("order" in e for e in errors), f"expected order in errors: {errors}"


def test_permanence_rejects_field_removal(regenerated_dir: Path, baseline: dict) -> None:
    """Removing a shipped field shifts offsets — must be rejected."""
    current = witness(regenerated_dir)
    current["vtables"]["FF_PATIENT"]["order"].pop(0)
    errors = _check_permanence(current["vtables"], baseline["vtables"], "vtables")
    assert errors, "expected permanence errors for removed field"
    assert any("order" in e for e in errors), f"expected order in errors: {errors}"


def test_permanence_accepts_field_append(regenerated_dir: Path, baseline: dict) -> None:
    """Appending a NEW field (and its size constant) is legal growth.

    The append is modelled the way the GENERATOR actually emits one: a new slot
    grows the block header by its width. This test used to append to `order` and
    `sizes` while leaving `header_sizes` untouched -- a state no generator run
    can produce -- so it passed while a real append was rejected by the
    `header_sizes` equality check. The rule is now monotonic growth, and the
    fixture has to exercise the real shape or it re-becomes decorative.
    """
    current = witness(regenerated_dir)
    block = current["vtables"]["FF_PATIENT"]
    block["order"].append("ZZ_APPENDED")
    block["sizes"]["ZZ_APPENDED_S"] = "TYPE_SIZE_UINT32"
    for k in block["header_sizes"]:
        block["header_sizes"][k] += 4  # the appended uint32 slot
    errors = _check_permanence(current["vtables"], baseline["vtables"], "vtables")
    assert not errors, f"appended field should pass permanence: {errors}"


def test_permanence_rejects_header_shrink(regenerated_dir: Path, baseline: dict) -> None:
    """A block header may grow, but shrinking it drops a shipped slot.

    The other half of the monotonic rule. Without this, relaxing `header_sizes`
    from equality to >= would silently permit a REMOVED field, which shifts the
    offset of every field after it for every stream already written.
    """
    current = witness(regenerated_dir)
    block = current["vtables"]["FF_PATIENT"]
    for k in block["header_sizes"]:
        block["header_sizes"][k] -= 4
    errors = _check_permanence(current["vtables"], baseline["vtables"], "vtables")
    assert errors, "a shrinking block header must fail permanence"
    assert any("SHRANK" in e for e in errors), errors


def test_codes_section_is_populated(baseline: dict) -> None:
    """The codes/tags sections must never be empty again.

    They were `{}` for as long as `witness()` scanned only generated_src/, while
    the constants live in dictionaries/FF_Codes.hpp and generated_src/FF_Recovery.hpp.
    Two of the three sections of "the ONE hard gate" were therefore comparing an
    empty dict against an empty dict and passing unconditionally (TASKS.md A15).
    A gate that passes when it measures nothing is the failure mode this whole
    file exists to prevent, so assert it is measuring something.
    """
    assert len(baseline["codes"]) > 5000, (
        f"golden has only {len(baseline['codes'])} dictionary codes — the witness "
        "is not seeing dictionaries/FF_Codes.hpp"
    )
    assert len(baseline["tags"]) > 100, (
        f"golden has only {len(baseline['tags'])} recovery tags — the witness is "
        "not seeing generated_src/FF_Recovery.hpp"
    )


def test_permanence_rejects_code_renumber(baseline: dict) -> None:
    """Renumbering a dictionary ID must fail — it is the `118d6ad` regression."""
    current = json.loads(json.dumps(baseline["codes"]))
    victim = next(iter(current))
    current[victim] += 1
    errors = _check_permanence(current, baseline["codes"], "codes")
    assert any("CHANGED" in e and victim in e for e in errors), errors


def test_permanence_accepts_code_append(baseline: dict) -> None:
    """Appending a new code at _next_id is legal and must not fail the gate."""
    current = json.loads(json.dumps(baseline["codes"]))
    current["FHIR::A_BRAND_NEW_SYSTEM::A_BRAND_NEW_CODE"] = 999_999
    assert not _check_permanence(current, baseline["codes"], "codes")
