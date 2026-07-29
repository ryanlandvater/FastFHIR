"""Constants the generator must agree with C++ on, across the language boundary.

Where a value can have one definition, it should (see ff_slot_width and
test_compact_layout.py). These cannot: Python needs numbers that C++ owns, and
there is no import path between them. So they are pinned here instead.

Both were flagged in candidate_redundancy.md as the same shape as the
COMPACT_SLOT_SIZES bug: a Python copy of a C++ constant, with nothing checking
that the copy is still right.
"""

from __future__ import annotations

import re
from pathlib import Path

_REPO_ROOT = Path(__file__).resolve().parents[2]
_PRIMITIVES = (_REPO_ROOT / "include" / "FF_Primitives.hpp").read_text(encoding="utf-8")

# TYPE_SIZE_* enumerator values, read straight out of the C++ header.
_TYPE_SIZES = {
    name: int(value) for name, value in re.findall(r"(TYPE_SIZE_\w+)\s*=\s*(\d+)", _PRIMITIVES)
}


def test_type_map_sizes_match_their_size_const():
    """TYPE_MAP carries both a literal `size` and a `size_const` name.

    The literal drives Python's V-Table offset arithmetic; the name is emitted
    into C++. If they disagree, the generator computes offsets the compiled
    struct does not have -- silently, and on the wire.
    """
    from generator.model.type_map import TYPE_MAP

    assert _TYPE_SIZES, "no TYPE_SIZE_* constants parsed from FF_Primitives.hpp"

    checked = 0
    bad: list[str] = []
    for fhir_type, spec in TYPE_MAP.items():
        if not isinstance(spec, dict):
            continue
        if "size" not in spec or "size_const" not in spec:
            continue
        checked += 1
        const = spec["size_const"]
        assert const in _TYPE_SIZES, f"{fhir_type}: unknown size_const {const!r}"
        if _TYPE_SIZES[const] != spec["size"]:
            bad.append(f"{fhir_type}: size={spec['size']} but {const}={_TYPE_SIZES[const]}")

    assert checked, "no TYPE_MAP entries carry both size and size_const"
    assert not bad, f"TYPE_MAP size/size_const disagree with C++: {bad}"


def test_base_block_header_size_matches_data_block():
    """merge.py hard-codes the DATA_BLOCK header size for offset arithmetic."""
    from generator.model.merge import BASE_BLOCK_HEADER_SIZE

    validation = _TYPE_SIZES["TYPE_SIZE_UINT64"]  # DATA_BLOCK::VALIDATION_S
    recovery = _TYPE_SIZES["TYPE_SIZE_UINT16"]  # DATA_BLOCK::RECOVERY_S
    expected = validation + recovery

    assert BASE_BLOCK_HEADER_SIZE == expected, (
        f"generator/model/merge.py BASE_BLOCK_HEADER_SIZE={BASE_BLOCK_HEADER_SIZE} but "
        f"DATA_BLOCK::HEADER_SIZE is {expected} (VALIDATION {validation} + RECOVERY "
        f"{recovery}). Every generated V-Table offset would be wrong by the difference."
    )
