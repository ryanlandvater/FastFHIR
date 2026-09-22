"""Gate on the generated conformance layer (TASKS.md Block K).

The failure this exists to prevent is a layer that compiles, links, attaches and
checks NOTHING. Every assertion here is about the tables being non-empty and
complete, because an empty table is indistinguishable from a passing one at
runtime -- P0-2: a gate that returns zero results must not pass.
"""

from __future__ import annotations

import json
import re
from pathlib import Path

import pytest

_REPO_ROOT = Path(__file__).resolve().parents[2]


@pytest.fixture(scope="module")
def layer_cpp(regenerated_dir: Path) -> str:
    path = regenerated_dir / "FF_Conformance_Layer.cpp"
    assert path.is_file(), f"the conformance layer was not emitted into {regenerated_dir}"
    return path.read_text(encoding="utf-8")


@pytest.fixture(scope="module")
def rule_rows(layer_cpp: str) -> list[tuple[str, str]]:
    """(RuleKind, whole row) for every emitted Rule."""
    return [
        (m.group(1), m.group(0))
        for m in re.finditer(r"\{\"[^\n]*RuleKind::(\w+), \d+\},", layer_cpp)
    ]


def test_required_rules_exist(rule_rows: list[tuple[str, str]]) -> None:
    """min >= 1 is the one shape this layer enforces; zero rows means it is inert."""
    required = [row for kind, row in rule_rows if kind == "REQUIRED"]
    assert len(required) > 100, (
        f"only {len(required)} required-element rules were emitted. The FHIR "
        "specification declares min >= 1 on hundreds of elements across the "
        "compiled profile, so a small number means the conformance facts are "
        "not reaching the emitter."
    )


def test_the_known_required_elements_are_present(rule_rows: list[tuple[str, str]]) -> None:
    """Named cases, verified against the StructureDefinitions by hand.

    Observation.status and Bundle.entry.request.method are the fixtures the C++
    suite asserts against; if they stop being emitted, that suite would go green
    for the wrong reason.
    """
    required = " ".join(row for kind, row in rule_rows if kind == "REQUIRED")
    for element in (
        "Observation.status",
        "Observation.code",
        "Bundle.entry.request.method",
        "Bundle.entry.request.url",
        "Patient.link.other",
    ):
        assert f'"{element}"' in required, f"{element} is min >= 1 in FHIR but was not emitted"


def test_unimplemented_rows_carry_their_reason(rule_rows: list[tuple[str, str]]) -> None:
    """K3.2: what was NOT checked must be as visible as what was."""
    unimplemented = [row for kind, row in rule_rows if kind == "UNIMPLEMENTED"]
    assert len(unimplemented) > 100, f"only {len(unimplemented)} unimplemented rows recorded"

    invariants = [r for r in unimplemented if "FHIRPath" in r]
    bindings = [r for r in unimplemented if "terminology validation" in r]
    assert invariants, "no FHIRPath invariant was recorded as unimplemented"
    assert bindings, "no required ValueSet binding was recorded as unimplemented"
    # Every constraint row names the invariant it declined to evaluate.
    assert any('"dom-2"' in r for r in invariants), "dom-2 is on every DomainResource"


def test_every_constraint_key_in_the_spec_is_accounted_for(
    regenerated_dir: Path, layer_cpp: str
) -> None:
    """No invariant may be silently dropped.

    Collected from the StructureDefinitions the generator itself read, so this
    compares the emitter's output against its own input rather than against a
    hand-written list that would drift.
    """
    packages = _REPO_ROOT / "fhir_packages"
    if not packages.is_dir():
        pytest.skip("FHIR packages not fetched")

    # FHIR invariant keys are not uniformly lower-case-and-digits: `bdl-3a` and
    # `docRef-1` are both real, and a narrower pattern reports them as missing
    # from a table that in fact holds them.
    emitted = set(re.findall(r'\{"[^"]*", "([A-Za-z][A-Za-z0-9]*-[A-Za-z0-9]+)"', layer_cpp))
    assert emitted, "no invariant keys were emitted at all"

    # Only the resources this profile actually generated carry rules.
    generated = {p.stem[3:] for p in regenerated_dir.glob("FF_*.hpp")}
    spec_keys: set[str] = set()
    for version in ("R4", "R5"):
        for sd in (packages / version / "package").glob("StructureDefinition-*.json"):
            name = sd.stem.replace("StructureDefinition-", "")
            if name not in generated:
                continue
            data = json.loads(sd.read_text(encoding="utf-8"))
            for element in data.get("snapshot", {}).get("element", []):
                for constraint in element.get("constraint", []) or []:
                    if constraint.get("key"):
                        spec_keys.add(constraint["key"])

    assert spec_keys, "no constraints found in the packages -- the scan is broken"
    missing = spec_keys - emitted
    assert not missing, (
        f"{len(missing)} FHIR invariant(s) appear in the compiled resources but in no "
        f"conformance table, so a report could not say they went unchecked: "
        f"{sorted(missing)[:10]}"
    )


def test_every_block_with_typetraits_has_a_dispatch_entry(
    regenerated_dir: Path, layer_cpp: str
) -> None:
    """A block with no entry is a hole in DESCENT, not merely an unchecked block.

    Bundle carries no rule of its own; if it had no entry, nothing would ever
    reach Bundle.entry.request.method.
    """
    traits: set[str] = set()
    for header in regenerated_dir.glob("FF_*.hpp"):
        traits.update(re.findall(r"template<> struct TypeTraits<(\w+Data)>", header.read_text()))
    assert traits, "no TypeTraits specialisations found"

    entries = set(re.findall(r"\{TypeTraits<(\w+Data)>::recovery, &check_\w+\}", layer_cpp))
    missing = traits - entries
    assert not missing, (
        f"{len(missing)} block(s) have TypeTraits but no conformance dispatch entry, so "
        f"the layer cannot descend into them: {sorted(missing)[:10]}"
    )


def test_the_layer_emits_no_wire_constant(layer_cpp: str) -> None:
    """The layer observes; it never encodes.

    A RECOVERY_TAG literal or an offset arithmetic expression in this file would
    mean the emitter had started making layout decisions.
    """
    assert "RECOVER_FF_" not in layer_cpp, (
        "the conformance layer names a RECOVERY_TAG literal. It must reach tags only "
        "through TypeTraits<T>::recovery, so the tag and the type cannot disagree."
    )
    assert "HEADER_SIZE" not in layer_cpp, "the conformance layer references a wire layout constant"


def test_the_stream_check_is_wired(layer_cpp: str) -> None:
    """The layer must offer the whole-document check, not only per-block ones.

    The per-block checks cannot answer a question about two resources at once,
    so the reference check is a separate hook that runs at finalize. A layer that
    compiles, attaches and answers nothing there is the same failure this file
    exists to catch, one level up: an absent check reads exactly like a passing
    one at runtime (P0-2).
    """
    assert "stream_check" in layer_cpp, (
        "the generated conformance layer does not set ValidationHooks::stream_check, "
        "so the finalize-time reference check would never run"
    )
    assert "&check_stream_references" in layer_cpp, (
        "stream_check is set but does not name the reference check: it must point at "
        "check_stream_references (src/conformance/FF_StreamCheck.hpp)"
    )
