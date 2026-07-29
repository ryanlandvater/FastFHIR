"""Code-ID integrity gate.

A code's ID is a wire constant: it is what a stored .ffhr archive contains, and
the only thing that decodes it. These tests fail the build if a regeneration
would move one.

They exist because commit 118d6ad renumbered the ledger from 1 and dropped
16,436 codes without anyone noticing. The test that was supposed to catch that
compared two fresh generator runs against *each other* -- both renumbered
identically, so it passed. That mistake shapes this file: every comparison is
against the COMMITTED ledger, and a generator that will not run is a failure,
never a silent skip.

See dictionaries/README.md.
"""

from __future__ import annotations

import json
import re
import subprocess
import sys
from pathlib import Path

import pytest

_HERE = Path(__file__).parent
_REPO_ROOT = _HERE.parents[1]
_LEDGER = _REPO_ROOT / "generator" / "master_codes.json"
_STRINGS = _REPO_ROOT / "dictionaries" / "FF_Dictionary_Strings.cpp"

# Bit 31 is FF_CODEABLE_CONCEPT_FLAG; 0xFFFFFFFF is FF_CODE_NULL.
_CODEABLE_CONCEPT_FLAG = 0x80000000
_CODE_NULL = 0xFFFFFFFF


def _ledger() -> dict:
    assert _LEDGER.exists(), f"the committed ledger is missing: {_LEDGER}"
    return json.loads(_LEDGER.read_text(encoding="utf-8"))


def _string_table() -> list:
    """Parse FF_DICTIONARY_STRINGS -- array index is the permanent code ID."""
    txt = _STRINGS.read_text(encoding="utf-8")
    body = txt.split("FF_DICTIONARY_STRINGS[] = {", 1)[1].split("\n};", 1)[0]
    out = []
    for line in body.splitlines():
        line = line.strip().rstrip(",")
        if line == "nullptr":
            out.append(None)
        elif line.startswith('"') and line.endswith('"'):
            out.append(line[1:-1])
    return out


# -- Ledger integrity -------------------------------------------------------


def test_ids_are_unique():
    ids = _ledger()["ids"]
    seen = {}
    clashes = []
    for code, cid in ids.items():
        if cid in seen:
            clashes.append((cid, seen[cid], code))
        seen[cid] = code
    assert not clashes, f"two codes share an ID: {clashes[:5]}"


def test_no_id_uses_a_reserved_value():
    ids = _ledger()["ids"]
    worst = max(ids.values())
    assert (
        worst < _CODEABLE_CONCEPT_FLAG
    ), f"max ID 0x{worst:X} has bit 31 set, which is FF_CODEABLE_CONCEPT_FLAG"
    assert _CODE_NULL not in ids.values(), "0xFFFFFFFF is FF_CODE_NULL and must not be assigned"
    assert 0 not in ids.values(), "ID 0 is the reserved null slot"


def test_next_id_is_beyond_every_assigned_id():
    ledger = _ledger()
    assert ledger["_next_id"] > max(
        ledger["ids"].values()
    ), "_next_id would hand out an ID that is already in use"


def test_every_code_has_an_identifier():
    """Every ID must be reachable by name, or the tables cannot alias it."""
    ledger = _ledger()
    named = {int(cid) for scope in ledger["scopes"].values() for cid in scope}
    missing = [c for c, i in ledger["ids"].items() if i not in named]
    assert not missing, f"{len(missing)} codes are in no scope, e.g. {missing[:5]}"


def test_identifiers_are_unique_within_each_scope():
    """Identifiers become struct/namespace members, so they must not collide.

    Uniqueness is per scope, not global: id 15024 is legitimately UCUM::LITER
    and FHIR::FDI_SURFACE::L -- one code, one ID, named as each system names it.
    """
    for scope, members in _ledger()["scopes"].items():
        seen = {}
        for cid, ident in members.items():
            assert (
                ident not in seen
            ), f"{scope}: identifier {ident!r} names both id {seen[ident]} and id {cid}"
            seen[ident] = cid


def test_no_member_shares_its_container_name():
    """`struct X { ... X ... };` does not compile."""
    for scope, members in _ledger()["scopes"].items():
        raw = scope if scope in ("UCUM", "LEGACY") else scope.split("::", 1)[1]
        container = re.sub(r"[^A-Za-z0-9_]", "_", raw).upper()
        clash = [cid for cid, n in members.items() if n == container]
        assert not clash, f"{scope}: member has the same name as its container (ids {clash})"


def test_no_identifier_is_a_libc_macro():
    """The preprocessor ignores scoping, so a macro name breaks every scope."""
    from generator.emit.codes_header import RESERVED_MACROS

    for scope, members in _ledger()["scopes"].items():
        bad = sorted({n for n in members.values() if n in RESERVED_MACROS})
        assert not bad, f"{scope}: identifiers collide with libc macros: {bad[:5]}"


# -- Ledger vs the emitted wire artifact ------------------------------------


def test_ledger_matches_the_string_table():
    """FF_DICTIONARY_STRINGS index == ledger ID, for every code.

    This is the check that would have caught 118d6ad.
    """
    ledger = _ledger()
    strings = _string_table()
    drift = []
    for code, cid in ledger["ids"].items():
        if cid >= len(strings):
            drift.append(f"id {cid} ({code!r}) is past the end of the string table")
        elif strings[cid] != code:
            drift.append(f"id {cid}: table has {strings[cid]!r}, ledger has {code!r}")
    assert not drift, (
        f"{len(drift)} slots disagree between the ledger and FF_Dictionary_Strings.cpp "
        f"-- the ID space has been renumbered. First few: {drift[:5]}"
    )


# -- Regeneration must not move anything ------------------------------------


def test_regeneration_preserves_every_committed_id():
    """Run the generator and assert it only appended.

    A generator that cannot run is a FAILURE, not a skip: a silent skip is
    exactly how the original renumbering reached main.
    """
    # Snapshot the ledger as it is on disk -- NOT via `git checkout`, which
    # would silently discard an in-progress ledger edit and hand back whatever
    # HEAD happens to hold.
    snapshot = _LEDGER.read_bytes()
    before = dict(json.loads(snapshot.decode("utf-8"))["ids"])

    try:
        proc = subprocess.run(
            [sys.executable, "-m", "generator"],
            cwd=_REPO_ROOT,
            capture_output=True,
            text=True,
            timeout=1800,
        )
        after = json.loads(_LEDGER.read_text(encoding="utf-8"))["ids"]
    finally:
        _LEDGER.write_bytes(snapshot)

    moved = {c: (before[c], after[c]) for c in before if c in after and before[c] != after[c]}
    dropped = sorted(set(before) - set(after))

    if proc.returncode != 0:
        if "urlopen" in proc.stderr or "Download failed" in proc.stdout:
            pytest.skip("generator needs network access to packages.fhir.org")
        pytest.fail(
            f"`python -m generator` exited {proc.returncode}. A generator that will not "
            f"run cannot prove ID stability.\nstderr tail:\n{proc.stderr[-1500:]}"
        )

    assert not moved, f"{len(moved)} committed IDs were REASSIGNED: {list(moved.items())[:5]}"
    assert not dropped, (
        f"{len(dropped)} codes were dropped; retired codes keep their IDs forever "
        f"because old archives still cite them. First few: {dropped[:5]}"
    )


# -- Redistribution boundary ------------------------------------------------


def test_generator_refuses_non_redistributable_sources():
    """FastFHIR ships HL7 FHIR and UCUM code values only.

    Codes from terminologies FastFHIR does not own -- SNOMED CT, LOINC, RxNorm,
    ICD, CPT, NDC, EDQM -- must never enter dictionaries/. They travel as
    FF_CODEABLE_CONCEPT blocks carrying the user's own code, under the user's
    own license.

    This is a test rather than a convention because the convention already
    failed once: an old value-set expansion pulled 13,310 LOINC/SNOMED/EDQM
    codes into the committed dictionary and nothing objected.
    """
    from generator.emit.dictionary import _assert_redistributable

    # HL7 and UCUM are fine.
    _assert_redistributable(
        {
            "administrative-gender": "http://hl7.org/fhir/administrative-gender",
            "observation-statistics": "http://terminology.hl7.org/CodeSystem/observation-statistics",
            "ucum-common": "http://unitsofmeasure.org",
        }
    )

    # Anything else stops the build.
    for name, url in (
        ("loinc", "http://loinc.org"),
        ("sct", "http://snomed.info/sct"),
        ("rxnorm", "http://www.nlm.nih.gov/research/umls/rxnorm"),
        ("cpt", "http://www.ama-assn.org/go/cpt"),
    ):
        with pytest.raises(RuntimeError, match="does not.*redistribute|Refusing"):
            _assert_redistributable({name: url})


def test_committed_codes_carry_no_foreign_terminology():
    """Nothing that looks like SNOMED/LOINC survives outside HL7's own systems.

    A shape heuristic, deliberately: it catches a bulk import of an external
    terminology, which is the failure that actually happened. A handful of HL7
    codes legitimately look numeric (observation-statistics uses '5-1'..'5-4'),
    so the bar is a bulk signal, not a single match.
    """
    ids = _ledger()["ids"]
    loinc_shaped = [c for c in ids if re.match(r"^\d{4,}-\d$", c)]
    snomed_shaped = [c for c in ids if c.isdigit() and len(c) >= 6]

    assert len(loinc_shaped) < 50, (
        f"{len(loinc_shaped)} LOINC-shaped codes in the ledger -- an external "
        f"terminology has been imported. e.g. {loinc_shaped[:5]}"
    )
    assert len(snomed_shaped) < 50, (
        f"{len(snomed_shaped)} SNOMED/EDQM-shaped codes in the ledger -- an external "
        f"terminology has been imported. e.g. {snomed_shaped[:5]}"
    )
