"""The shared facts the C and C++ headers must agree on, and the parsers to read them.

WHY THIS EXISTS
===============
`FF_MemoryCreateInfo` and its siblings are declared twice: in the hand-written
C++ `FastFHIR.hpp` and in the hand-written C `FastFHIR.h`. They are NOT the same
type -- the C++ struct carries default member initializers and the `Size` alias,
the C struct carries `uint64_t` and an opaque `FF_MemoryHandle` -- so neither
header can `#include` the other. That is by design (two ABIs over one library),
but the SHARED FACTS (which fields exist, in what order, which enum has which
value) must not drift between them.

This module holds those shared facts once; `test_c_abi.py` reads both headers
and checks each against it. Change a field or an enum value in one header
without the other and the gate fails with the offending name.

WHY IT LIVES UNDER tests/, NOT generator/emit/
==============================================
Nothing is generated from it. Both headers stay hand-written -- the C header's
prose (two-call export, thread-local message lifetime) IS its interface, and it
is an installed public header, so it cannot move to the gitignored
`generated_src/`. A spec that only a gate reads belongs with the gate; putting
it under `emit/` would claim a pipeline stage that does not exist.

WHAT IS CHECKED, AND WHAT IS NOT
================================
CHECKED (on both sides, against the spec below):
  - struct field names and their order
  - enum value names and their numeric values

NOT CHECKED (authored, because no single text is valid in both languages):
  - field TYPES: `Size`/`FF_Memory` in C++ are `uint64_t`/`FF_MemoryHandle` in C
  - field NAMES where they differ (`version` in C++ is `fhir_version` in C)
  - FUNCTION SIGNATURES: C++ `FF_CreateMemory(const FF_MemoryCreateInfo&,
    FF_Memory&)` is C `FF_CreateMemory(const FF_MemoryCreateInfo*,
    FF_MemoryHandle*)` -- different parameter passing and different ownership.
    A reference is not expressible in C, and `std::string_view`/`shared_ptr`
    cannot cross the ABI, so the functions are a mapping table rather than a
    translation. `src/FF_C_API.cpp` is where that mapping is implemented, and
    its `static_assert`s are the compile-time half of this gate.
"""

from __future__ import annotations

import re
from dataclasses import dataclass

# =====================================================================
# THE SPEC — the one source of truth for what the two headers share
# =====================================================================


@dataclass(frozen=True)
class Field:
    cpp_name: str  # name in FastFHIR.hpp
    cpp_type: str  # C++ spelling, for the reader
    c_name: str  # name in FastFHIR.h (may differ: version -> fhir_version)
    c_type: str  # C spelling
    cpp_only: bool = False  # present in C++, deliberately absent from the C struct


@dataclass(frozen=True)
class Struct:
    name: str  # shared type name (both sides)
    fields: tuple[Field, ...]
    cpp_header: str = "FastFHIR.hpp"  # which C++ header declares it


@dataclass(frozen=True)
class EnumValue:
    cpp_name: str  # enumerator in C++ (FHIR_VERSION_R5, FF_CHECKSUM_SHA256, ...)
    c_name: str  # enumerator in C (FF_FHIR_R5, FF_CHECKSUM_ALGO_SHA256, ...)
    value: int


@dataclass(frozen=True)
class Enum:
    cpp_type: str  # C++ enum type name, for messages
    values: tuple[EnumValue, ...]
    cpp_header: str = "FF_Primitives.hpp"


# Every C++ `Info` struct that has a C twin. The order here is the order the
# gate reports; the field order within a struct must match BOTH headers.
C_STRUCTS: tuple[Struct, ...] = (
    Struct(
        "FF_MemoryCreateInfo",
        (
            Field("capacity", "Size", "capacity", "uint64_t"),
            Field("shm_name", "const char*", "shm_name", "const char *"),
            Field("filepath", "const char*", "filepath", "const char *"),
        ),
    ),
    Struct(
        "FF_BuilderCreateInfo",
        (
            Field("capacity", "Size", "capacity", "uint64_t"),
            Field("version", "FHIR_VERSION", "fhir_version", "int32_t"),
            Field("arena", "FF_Memory", "arena", "FF_MemoryHandle"),
            Field("filepath", "const char*", "filepath", "const char *"),
            Field("shm_name", "const char*", "shm_name", "const char *"),
        ),
    ),
    Struct(
        "FF_BuilderSetRootInfo",
        (
            Field("builder", "FF_Builder", "builder", "FF_BuilderHandle"),
            Field("root", "Reflective::ObjectHandle", "root", "FF_ObjectHandle"),
        ),
    ),
    # `hasher` is a std::function the library calls back into, so it cannot cross
    # the ABI and the C struct stops one field short. cpp_only records that as a
    # decision rather than letting the two lists differ silently.
    Struct(
        "FF_BuilderFinalizeInfo",
        (
            Field("builder", "FF_Builder", "builder", "FF_BuilderHandle"),
            Field("algorithm", "FF_Checksum_Algorithm", "algorithm", "int32_t"),
            Field("hasher", "HashCallback", "", "", cpp_only=True),
        ),
    ),
    Struct(
        "FF_CompactInfo",
        (
            Field("source", "Parser", "source", "FF_ParserHandle"),
            Field("algorithm", "FF_Checksum_Algorithm", "algorithm", "int32_t"),
            Field("hasher", "HashCallback", "", "", cpp_only=True),
        ),
    ),
)

# Every C++ enum whose values cross into C. The C side spells them distinctly
# (FF_CHECKSUM_ALGO_*, FF_FHIR_*) because the C++ names already occupy
# FF_CHECKSUM_*, FHIR_VERSION_* at global scope. An enum with no C entry point
# to take it does not belong in the C header, so it does not belong here.
C_ENUMS: tuple[Enum, ...] = (
    Enum(
        "FHIR_VERSION",
        (
            EnumValue("FHIR_VERSION_R4", "FF_FHIR_R4", 0x0400),
            EnumValue("FHIR_VERSION_R5", "FF_FHIR_R5", 0x0500),
        ),
    ),
    Enum(
        "FF_Checksum_Algorithm",
        (
            EnumValue("FF_CHECKSUM_NONE", "FF_CHECKSUM_ALGO_NONE", 0),
            EnumValue("FF_CHECKSUM_CRC32", "FF_CHECKSUM_ALGO_CRC32", 1),
            EnumValue("FF_CHECKSUM_MD5", "FF_CHECKSUM_ALGO_MD5", 2),
            EnumValue("FF_CHECKSUM_SHA256", "FF_CHECKSUM_ALGO_SHA256", 3),
        ),
    ),
)


# =====================================================================
# PARSERS — shared by the gate, so both sides are read the same way
# =====================================================================

_BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.S)
_LINE_COMMENT = re.compile(r"//[^\n]*")
_STRUCT = re.compile(r"(?:typedef\s+)?struct\s+(FF_\w+)\s*\{(.*?)\}\s*(?:\w+)?\s*;", re.S)
_ENUM = re.compile(r"enum(?:\s+class)?\s*(\w+)?\s*(?::\s*\w+\s*)?\{(.*?)\}\s*;", re.S)
_TRAILING_IDENTIFIER = re.compile(r"(\w+)\s*$")
_ENUMERATOR = re.compile(r"(\w+)\s*(?:=\s*(0x[0-9A-Fa-f]+|\d+))?")


def _strip_comments(text: str) -> str:
    return _LINE_COMMENT.sub("", _BLOCK_COMMENT.sub("", text))


def parse_struct_fields(text: str) -> dict[str, list[str]]:
    """Field names, in order, for every `(typedef )?struct NAME { ... } NAME?;`.

    Handles both the C++ form (`struct X { ... };`) and the C form
    (`typedef struct X { ... } X;`). Defaults (`= nullptr`) are dropped.
    """
    body = _strip_comments(text)
    out: dict[str, list[str]] = {}
    for m in _STRUCT.finditer(body):
        name, fields_src = m.group(1), m.group(2)
        names: list[str] = []
        for decl in fields_src.split(";"):
            decl = decl.split("=")[0].strip()  # drop `= 0` / `= nullptr`
            if not decl:
                continue
            mm = _TRAILING_IDENTIFIER.search(decl)  # trailing identifier is the name
            if mm:
                names.append(mm.group(1))
        out[name] = names
    return out


def parse_enumerator_values(text: str) -> dict[str, int]:
    """Every EXPLICITLY VALUED enumerator -> value, across all enum blocks.

    An enumerator written without `= value` is skipped rather than having its
    value worked out from its position. The two headers are edited
    independently, so inferring a value from position could assign a number the
    compiler never assigned. The spec therefore lists only enumerators that both
    headers write out explicitly. If one of them loses its explicit value, this
    function returns nothing for it and the gate fails by comparing None against
    the number the spec expects.
    """
    body = _strip_comments(text)
    out: dict[str, int] = {}
    for m in _ENUM.finditer(body):
        for item in m.group(2).split(","):
            item = item.strip()
            if not item:
                continue
            mm = _ENUMERATOR.match(item)
            if mm and mm.group(2) is not None:
                out[mm.group(1)] = int(mm.group(2), 0)
    return out
