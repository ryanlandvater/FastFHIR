"""RECOVERY_TAG references must resolve against the permanent header.

`include/FF_Recovery.hpp` is hand-maintained and its values are permanent wire
constants -- every block on disk carries one at bytes 8-9. The generator only
*references* those tags, but it builds some of the names by concatenation
(`f"RECOVER_{child_struct}"` in generator/model/structure.py), so a rename
upstream or a new FHIR type can produce a name that does not exist.

That is the same shape as the COMPACT_SLOT_SIZES bug: Python deriving something
C++ owns. It cannot be collapsed to one definition -- the tags genuinely live in
C++ and Python must name them -- so it is checked instead.

`parse_recovery_tags()` was written for exactly this and its docstring said so,
but the one call site discarded the result. The check never ran.
"""

from __future__ import annotations

import re
from pathlib import Path

import pytest

_REPO_ROOT = Path(__file__).resolve().parents[2]
_GENERATED = _REPO_ROOT / "generated_src"
_RECOVERY = _REPO_ROOT / "include" / "FF_Recovery.hpp"


def test_every_emitted_tag_is_declared():
    """No generated file may reference a tag the header does not declare."""
    from generator.utilities import validate_recovery_tags

    if not _GENERATED.is_dir():
        pytest.skip("generated_src/ not present -- configure with the generator enabled")

    # Raises RuntimeError naming the tag and file if anything is unknown.
    count = validate_recovery_tags(str(_GENERATED), str(_RECOVERY))
    assert count > 0, "no RECOVERY_TAG references found -- the check would be vacuous"


def test_validation_is_wired_into_the_pipeline():
    """The check must actually run during generation, not just exist.

    It existed and did not run for the whole life of the module: library.py
    called parse_recovery_tags() and threw the return value away.
    """
    lib = (_REPO_ROOT / "generator" / "library.py").read_text(encoding="utf-8")
    assert "validate_recovery_tags(" in lib, (
        "generator/library.py no longer calls validate_recovery_tags -- the tag check "
        "is dead again"
    )
    assert not re.search(r"^\s*parse_recovery_tags\([^)]*\)\s*$", lib, re.M), (
        "generator/library.py calls parse_recovery_tags and discards the result. That "
        "is the exact no-op this test exists to prevent."
    )


def test_no_field_falls_through_to_undefined():
    """Every FIELDS row must carry a real child recovery tag.

    `_child_recovery_key_expr` ends in `return "FF_RECOVER_UNDEFINED"`. That is
    correct for a value with no child block, but if a *new* FHIR type reaches it
    by accident the field silently loses its type identity -- no compile error,
    no wrong number, just a block that cannot be recovered.
    """
    if not _GENERATED.is_dir():
        pytest.skip("generated_src/ not present")

    row = re.compile(r'\{"([^"]*)",\s*(FF_FIELD_\w+),\s*([^,]+),\s*(\w+)')
    offenders: list[str] = []
    checked = 0
    for cpp in sorted(_GENERATED.glob("*.cpp")):
        text = cpp.read_text(encoding="utf-8")
        for body in re.findall(
            r"const FF_FieldInfo (\w+)::FIELDS\[[^\]]+\] = \{(.*?)\n\};", text, re.S
        ):
            block, rows = body
            for name, kind, _off, recovery in row.findall(rows):
                checked += 1
                if recovery == "FF_RECOVER_UNDEFINED":
                    offenders.append(f"{block}.{name} ({kind})")

    assert checked, "no FIELDS tables found"
    assert not offenders, (
        f"{len(offenders)} field(s) fell through to FF_RECOVER_UNDEFINED, so their blocks "
        f"carry no type identity on the wire: {offenders[:5]}"
    )
