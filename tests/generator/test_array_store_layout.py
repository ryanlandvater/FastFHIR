"""Array element layout must be internally consistent in emitted store code.

A FHIR array is written by `STORE_FF_ARRAY_HEADER(base, off, MODE, STRIDE, ...)`,
and the stride is what every later reader hops by. Three array shapes exist and
their (MODE, STRIDE) pairs are not interchangeable:

  * string/code -> (OFFSET, TYPE_SIZE_OFFSET)   8-byte pointers to strings
    stored elsewhere, because the payloads are variable-length
  * Resource    -> (INLINE_BLOCK, TYPE_SIZE_RESOURCE)  10-byte offset+tag tuples
  * everything else -> (INLINE_BLOCK, FF_X::HEADER_SIZE)  fixed-size structs

These tests exist because a refactor of `generator/emit/store.py` collapsed the
first two shapes into the third. Two mechanical substitutions did it:

  1. the string branch was narrowed from `fhir_type in ("string", "code")` to
     `fhir_type == "string"`, so every `code` array fell through to the
     complex-struct branch and was emitted as an inline array of
     `FF_STRING::HEADER_SIZE` -- a fixed stride over variable-length data.
  2. `elif f["fhir_type"] == "Resource"` was rewritten to
     `elif kind == "FF_FIELD_RESOURCE"` *inside* the `kind == "FF_FIELD_ARRAY"`
     branch, which can never be true, so Resource arrays fell through to the
     same place and referenced a struct (`FF_RESOURCE`) that does not exist.

Both survived the generator -- it emits strings, so nothing checked that the
symbols were real or the strides sane. The C++ compiler caught them only by
luck, via an unrelated arity mismatch. Had `code` arrays landed on a branch that
happened to compile, the wrong stride would have shipped silently, which is the
failure mode these tests are here to prevent.
"""

from __future__ import annotations

import re
from pathlib import Path

import pytest

_REPO_ROOT = Path(__file__).resolve().parents[2]
_GENERATED = _REPO_ROOT / "generated_src"
_INCLUDE = _REPO_ROOT / "include"

# STORE_FF_ARRAY_HEADER(__base, child_off, FF_ARRAY::<MODE>, <STRIDE>, ...)
_ARRAY_HEADER = re.compile(
    r"STORE_FF_ARRAY_HEADER\(\s*__base\s*,\s*\w+\s*,\s*FF_ARRAY::(\w+)\s*,\s*([^,]+?)\s*,"
)


def _generated_sources() -> list[Path]:
    if not _GENERATED.is_dir():
        pytest.skip("generated_src/ not present -- configure with the generator enabled")
    return sorted(_GENERATED.glob("*.cpp"))


def _declared_structs() -> set[str]:
    """Every FF_* struct name declared or defined in the headers."""
    names: set[str] = set()
    for hpp in list(_GENERATED.glob("*.hpp")) + list(_INCLUDE.glob("*.hpp")):
        text = hpp.read_text(encoding="utf-8")
        names.update(re.findall(r"\bstruct\s+(FF_[A-Z0-9_]+)\b", text))
    return names


def test_array_strides_match_their_storage_mode():
    """Each (MODE, STRIDE) pair must be one of the three legal array shapes."""
    offenders: list[str] = []
    seen = 0

    for cpp in _generated_sources():
        for mode, stride in _ARRAY_HEADER.findall(cpp.read_text(encoding="utf-8")):
            seen += 1
            if mode == "OFFSET":
                # Variable-length payloads: the array holds 8-byte pointers.
                if stride != "TYPE_SIZE_OFFSET":
                    offenders.append(f"{cpp.name}: OFFSET array with stride {stride}")
            elif mode == "INLINE_BLOCK":
                # Fixed-size payloads striding in place.
                ok = stride == "TYPE_SIZE_RESOURCE" or stride.startswith("TYPE_SIZE_")
                ok = ok or re.fullmatch(r"FF_[A-Z0-9_]+::HEADER_SIZE", stride) is not None
                if not ok:
                    offenders.append(f"{cpp.name}: INLINE_BLOCK array with stride {stride}")
            else:
                offenders.append(f"{cpp.name}: unknown array mode FF_ARRAY::{mode}")

    assert seen, "no STORE_FF_ARRAY_HEADER calls found in generated_src/"
    assert not offenders, (
        f"{len(offenders)} array headers use a stride that contradicts their storage "
        f"mode. First few: {offenders[:5]}"
    )


def test_strings_are_never_stored_as_inline_array_entries():
    """FF_STRING is variable-length; it can never be an inline array stride.

    `INLINE_BLOCK, FF_STRING::HEADER_SIZE` means every entry after the first
    is read from the wrong address. String and code arrays must use the
    OFFSET/TYPE_SIZE_OFFSET shape instead.
    """
    offenders: list[str] = []
    for cpp in _generated_sources():
        for mode, stride in _ARRAY_HEADER.findall(cpp.read_text(encoding="utf-8")):
            if mode == "INLINE_BLOCK" and stride == "FF_STRING::HEADER_SIZE":
                offenders.append(cpp.name)

    assert not offenders, (
        "string/code arrays are being emitted as inline fixed-stride blocks in "
        f"{sorted(set(offenders))}. The emitter's string branch has stopped matching "
        'them -- check that it tests `fhir_type in ("string", "code")` and STRING_TYPES, '
        'not `== "string"`.'
    )


def test_generated_code_only_references_structs_that_exist():
    """Every FF_X::HEADER_SIZE and STORE_FF_X() must name a declared struct.

    A dispatch branch that falls through to the wrong handler typically invents
    a plausible-looking name -- `FF_RESOURCE` for arrays of Resource, which is a
    10-byte offset+tag tuple and has no struct at all.
    """
    declared = _declared_structs()
    assert "FF_STRING" in declared, "header scan found no structs -- the regex is wrong"

    # Emitted by the array/store paths; both derive from _resolve_ff_struct_name.
    referenced: dict[str, str] = {}
    for cpp in _generated_sources():
        text = cpp.read_text(encoding="utf-8")
        for name in re.findall(r"\b(FF_[A-Z0-9_]+)::HEADER_SIZE\b", text):
            referenced.setdefault(name, cpp.name)
        for name in re.findall(r"\bSTORE_(FF_[A-Z0-9_]+)\s*\(", text):
            # STORE_FF_ARRAY_HEADER writes an array header; it is a helper, not
            # a per-struct store function, so it names no struct.
            if name != "FF_ARRAY_HEADER":
                referenced.setdefault(name, cpp.name)

    assert referenced, "no FF_* struct references found in generated_src/"
    missing = {n: f for n, f in referenced.items() if n not in declared}
    assert not missing, (
        f"generated code references {len(missing)} struct(s) that are never declared: "
        f"{sorted(missing)[:5]} (first seen in {sorted(missing.items())[0][1]}). "
        "A store/size branch is resolving a struct name for a type that has no struct."
    )
