"""The C ABI drift gate.

`FF_MemoryCreateInfo` and its siblings live in two hand-written headers that
cannot include one another (`FastFHIR.hpp` is C++, `FastFHIR.h` is C). The shared
facts — which fields exist and in what order, which enum has which value — are
declared once in `c_abi_spec.py` beside this file, and both headers are checked
against it here. Change one side without the other and this fails with the name.

This is the C-ABI analogue of `wire_witness`: one source, a gate that refuses
silent drift.
"""

from __future__ import annotations

from pathlib import Path

import pytest

# conftest.py puts this directory on sys.path, the same way test_wire_format.py
# reaches wire_witness.
from c_abi_spec import C_ENUMS, C_STRUCTS, parse_enumerator_values, parse_struct_fields

ROOT = Path(__file__).resolve().parents[2]


def _read(rel: str) -> str:
    return (ROOT / "include" / rel).read_text()


# ── structs ──────────────────────────────────────────────────────────────────


@pytest.mark.parametrize("struct", C_STRUCTS, ids=[s.name for s in C_STRUCTS])
def test_struct_fields_match_spec_on_both_sides(struct):
    """The C++ field order and the C field order both match the spec."""
    cpp = parse_struct_fields(_read(struct.cpp_header))
    c = parse_struct_fields(_read("FastFHIR.h"))

    assert struct.name in cpp, f"{struct.name} missing from {struct.cpp_header}"
    assert struct.name in c, f"{struct.name} missing from FastFHIR.h"

    assert cpp[struct.name] == [f.cpp_name for f in struct.fields], (
        f"{struct.name} field order/names drifted in {struct.cpp_header}: "
        f"{cpp[struct.name]} != {[f.cpp_name for f in struct.fields]}"
    )
    # The C side carries every field except those marked cpp_only -- a callback
    # that cannot cross the ABI, and nothing else.
    expected_c = [f.c_name for f in struct.fields if not f.cpp_only]
    assert c[struct.name] == expected_c, (
        f"{struct.name} field order/names drifted in FastFHIR.h: "
        f"{c[struct.name]} != {expected_c}"
    )


# ── enums ────────────────────────────────────────────────────────────────────


@pytest.mark.parametrize("enum", C_ENUMS, ids=[e.cpp_type for e in C_ENUMS])
def test_enum_values_match_spec_on_both_sides(enum):
    """Each enumerator is the spec's value in the C++ header AND the C header."""
    cpp = parse_enumerator_values(_read(enum.cpp_header))
    c = parse_enumerator_values(_read("FastFHIR.h"))

    for v in enum.values:
        assert cpp.get(v.cpp_name) == v.value, (
            f"{v.cpp_name} in {enum.cpp_header} is {cpp.get(v.cpp_name)}, " f"spec says {v.value}"
        )
        assert (
            c.get(v.c_name) == v.value
        ), f"{v.c_name} in FastFHIR.h is {c.get(v.c_name)}, spec says {v.value}"


def test_gate_is_not_vacuous():
    """P0-2: a sweep that parses nothing must not pass."""
    cpp = parse_struct_fields(_read("FastFHIR.hpp"))
    assert len(cpp) >= len(C_STRUCTS), "the C++ struct parser found nothing"
    assert parse_enumerator_values(_read("FF_Primitives.hpp")), "enum parser found nothing"
