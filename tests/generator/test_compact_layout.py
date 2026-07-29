"""Slot widths must have exactly ONE definition.

`ff_slot_width()` in include/FF_Primitives.hpp is that definition. Everything
else is a projection of it:

  * src/FF_Compactor.cpp calls it directly when sizing dense slots
  * the generated COMPACT_SLOT_SIZES tables are emitted as *calls* to it, so
    the compact reader and writer consume identical constants
  * generated blocks static_assert their V-Table widths against it, so the
    compiler rejects a silent divergence

That last point means the byte-for-byte comparison this file used to perform is
now the compiler's job. What is left to guard is the property the compiler
cannot see: that the generator never goes back to computing widths in Python.

It did once. `generator/model/structure.py` derived them from FHIR type names
while C++ switched on the field kind; they disagreed on 1,393 of 1,611 slots,
and the compact reader read every field after the first string, array, or block
from the wrong address.
"""

from __future__ import annotations

import re
from pathlib import Path

import pytest

_REPO_ROOT = Path(__file__).resolve().parents[2]
_GENERATED = _REPO_ROOT / "generated_src"
_PRIMITIVES = _REPO_ROOT / "include" / "FF_Primitives.hpp"
_COMPACTOR = _REPO_ROOT / "src" / "FF_Compactor.cpp"


def test_slot_width_is_defined_exactly_once():
    """One `ff_slot_width` definition, in the header both sides include."""
    defs = [
        p
        for p in list(_REPO_ROOT.glob("include/*.hpp")) + list(_REPO_ROOT.glob("src/*.cpp"))
        if re.search(r"constexpr\s+uint8_t\s+ff_slot_width\s*\(", p.read_text(encoding="utf-8"))
    ]
    assert [p.name for p in defs] == ["FF_Primitives.hpp"], (
        "ff_slot_width must be defined once, in FF_Primitives.hpp; "
        f"found in {[p.name for p in defs]}"
    )


def test_compactor_does_not_reimplement_slot_width():
    """The writer calls the shared function instead of switching on kind itself."""
    text = _COMPACTOR.read_text(encoding="utf-8")
    assert "ff_slot_width(" in text, "FF_Compactor.cpp should call ff_slot_width()"
    assert not re.search(r"\w+\s+compact_slot_size\s*\(\s*FF_FieldKind", text), (
        "FF_Compactor.cpp has reintroduced a local slot-size switch. That is the "
        "exact duplication that caused the 1,393-slot COMPACT_SLOT_SIZES mismatch."
    )


def test_presence_helpers_are_defined_once():
    """The compact bitmap formula must have one definition, in FF_SIMD.hpp.

    It was a `static inline` copy in both FF_Compactor.cpp (writer) and
    FF_Parser.cpp (reader) -- the same writer/reader pair shape as the slot-size
    bug. They agreed at the time, which is exactly what made it easy to miss.
    """
    defs = {"compact_presence_bytes": [], "compact_presence_contains": []}
    for path in list(_REPO_ROOT.glob("include/*.hpp")) + list(_REPO_ROOT.glob("src/*.cpp")):
        text = path.read_text(encoding="utf-8")
        for fn in defs:
            # A definition, not a call: name followed by a parameter list and a brace.
            if re.search(rf"\b\w+\s+{fn}\s*\([^)]*\)\s*\{{", text):
                defs[fn].append(path.name)

    for fn, files in defs.items():
        assert files == ["FF_SIMD.hpp"], (
            f"{fn} must be defined once, in FF_SIMD.hpp, so the compactor (writer) and "
            f"parser (reader) share one bitmap layout. Found in {files}"
        )


def test_json_export_uses_the_shared_codeable_concept_decoder():
    """Entry::print_scalar_json must call the decoder, not switch on system.

    It carried a third per-system switch that had drifted: only UNKNOWN, UCUM,
    SNOMED_CT and DICOM were handled, so CPT, CVX, RxNorm, MDC, MED-RT and IDMP
    had their fixed-width binary payloads printed as if they were ASCII --
    straight into the exported JSON. FF_DECODE_CODEABLE_CONCEPT handled all 17
    correctly the whole time.

    Nothing behavioural catches a regression here today: A8 keeps every code on
    the UNKNOWN branch, so a switch handling only UNKNOWN would pass every test.
    """
    parser = (_REPO_ROOT / "src" / "FF_Parser.cpp").read_text(encoding="utf-8")
    fn = re.search(r"void Reflective::Entry::print_scalar_json\(.*?\n\}", parser, re.S)
    assert fn, "print_scalar_json not found"
    body = fn.group(0)

    assert "FF_DECODE_CODEABLE_CONCEPT(" in body, (
        "print_scalar_json no longer calls FF_DECODE_CODEABLE_CONCEPT -- the JSON "
        "export path has stopped sharing the decoder."
    )
    assert "FF_CodeableConceptSystem::" not in body, (
        "print_scalar_json switches on FF_CodeableConceptSystem again. That is the "
        "third per-system decoder; it drifted to 4 of 17 systems last time. Call "
        "FF_DECODE_CODEABLE_CONCEPT instead."
    )


def test_generator_emits_calls_not_numbers():
    """COMPACT_SLOT_SIZES entries must be ff_slot_width() calls, never literals.

    A literal means Python computed the width again, which is how the two sides
    drifted apart before.
    """
    if not _GENERATED.is_dir():
        pytest.skip("generated_src/ not present -- configure with the generator enabled")

    sizes_re = re.compile(r"COMPACT_SLOT_SIZES\[[^\]]+\] = \{([^}]*)\};")
    checked = 0
    offenders: list[str] = []
    for cpp in sorted(_GENERATED.glob("*.cpp")):
        for body in sizes_re.findall(cpp.read_text(encoding="utf-8")):
            for raw in body.split(","):
                entry = raw.strip()
                if not entry:
                    continue
                checked += 1
                # "0" is the SIMD tail padding past FIELD_COUNT.
                if entry != "0" and not entry.startswith("ff_slot_width("):
                    offenders.append(f"{cpp.name}: {entry}")

    assert checked, "no COMPACT_SLOT_SIZES tables found"
    assert not offenders, (
        f"{len(offenders)} COMPACT_SLOT_SIZES entries are hard-coded numbers rather than "
        f"ff_slot_width() calls -- the generator is computing widths again. "
        f"First few: {offenders[:5]}"
    )


def test_vtable_widths_are_compiler_checked():
    """Every field must carry a static_assert tying its V-Table width to its kind."""
    if not _GENERATED.is_dir():
        pytest.skip("generated_src/ not present")

    asserts = 0
    fields = 0
    for cpp in sorted(_GENERATED.glob("*.cpp")):
        text = cpp.read_text(encoding="utf-8")
        asserts += len(re.findall(r"static_assert\(\w+::\w+_S == ff_slot_width\(", text))
        for body in re.findall(
            r"const FF_FieldInfo \w+::FIELDS\[[^\]]+\] = \{(.*?)\n\};", text, re.S
        ):
            fields += len(re.findall(r'\{"[^"]*",\s*FF_FIELD_\w+', body))

    assert fields, "no FIELDS tables found"
    assert asserts == fields, (
        f"{fields} fields but only {asserts} V-Table width assertions -- some field can "
        f"drift from ff_slot_width() without the compiler noticing"
    )


def test_ff_slot_width_covers_every_field_kind():
    """No FF_FieldKind may reach `default:` by accident."""
    text = _PRIMITIVES.read_text(encoding="utf-8")
    enum_body = re.search(r"enum FF_FieldKind : uint16_t\s*\{(.*?)\};", text, re.S)
    assert enum_body, "FF_FieldKind enum not found"
    kinds = set(re.findall(r"(FF_FIELD_\w+)", enum_body.group(1)))

    fn = re.search(r"constexpr uint8_t ff_slot_width\(.*?\n\}", text, re.S)
    assert fn, "ff_slot_width not found"
    handled = set(re.findall(r"case (FF_FIELD_\w+):", fn.group(0)))

    # STRING / ARRAY / BLOCK / UNKNOWN intentionally share the offset default.
    by_default = {"FF_FIELD_STRING", "FF_FIELD_ARRAY", "FF_FIELD_BLOCK", "FF_FIELD_UNKNOWN"}
    missing = kinds - handled - by_default
    assert not missing, (
        f"FF_FieldKind values {sorted(missing)} hit ff_slot_width()'s default branch. "
        "If TYPE_SIZE_OFFSET is right for them, add them to the documented default set; "
        "otherwise give them an explicit case."
    )
