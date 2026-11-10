# ============================================================
# FastFHIR Generator — typed layout model.
#
# The shapes the model layer builds and every emitter reads, declared once.
# Each is a TypedDict: a dict at runtime, so nothing about generation changes,
# and a struct to the type checker, so a misspelled key or a wrong value type
# fails `mypy --strict` instead of emitting wrong C++.
#
# `JsonObject` is the one untyped shape, and it stops at the parsing boundary:
# it is a StructureDefinition, ValueSet or CodeSystem as HL7 publishes it.
#
# Author: Ryan Landvater (ryanlandvater[at]gmail[dot]com)
# Copyright (c) 2025 Ryan Landvater. All rights reserved.
# License: Mozilla Public License, v. 2.0 (MPL-2.0) — see LICENSE or http://mozilla.org/MPL/2.0/
# ============================================================

from __future__ import annotations

from typing import Any, NotRequired, TypeAlias, TypedDict

# A parsed HL7 JSON document or one element of it. Past the functions that read
# HL7 packages, data travels in the typed shapes below.
JsonObject: TypeAlias = dict[str, Any]


class TypeMapping(TypedDict):
    """How one FHIR type is stored: a row of `type_map.TYPE_MAP`."""

    cpp: str  # V-Table slot type, e.g. 'uint32_t', 'Offset'
    data_type: str  # POCO member type, e.g. 'std::string_view'
    null: str  # the absent-value sentinel constant
    size: int  # slot width in bytes
    size_const: str  # the C++ constant naming that width
    macro: str  # the LOAD_* reader for the slot


class CodeEnum(TypedDict):
    """The generated C++ enum bound to one `code` field."""

    enum: str  # enum type name
    parse: str  # string -> enum function name
    serialize: str  # enum -> string function name


class ConstraintFact(TypedDict):
    """One FHIRPath invariant of an element, as published."""

    key: str
    severity: str
    human: str
    expression: str


class FixedValue(TypedDict):
    """A `fixed[x]` or `pattern[x]` value; `key` carries the type suffix."""

    key: str
    value: Any  # whatever JSON value HL7 published for it


class Conformance(TypedDict):
    """What one FHIR revision requires of one element. Never affects layout."""

    min: int
    max: str | None
    constraints: list[ConstraintFact]
    binding_strength: str
    binding_valueset: str
    fixed: FixedValue | None
    pattern: FixedValue | None


class FieldOverride(TypedDict, total=False):
    """A hand-written replacement for part of one field's storage."""

    cpp_type: str
    raw_scalar: bool
    fhir_type: str
    macro: str
    size: int
    size_const: str
    data_type: str
    url_idx: bool


class FieldLayout(TypedDict):
    """One V-Table field of a generated block.

    Order within `BlockLayout.layout` is wire-significant: the emitted C++
    offset is a symbolic sum over the preceding fields' size constants.
    """

    # --- identity ---
    name: str  # upper-case name used in C++ constants
    cpp_name: str  # member name, C++ keywords suffixed with '_'
    orig_name: str  # the FHIR element name
    fhir_type: str  # FHIR type token, e.g. 'string', 'code', 'Reference'

    # --- shape ---
    is_array: bool
    is_choice: bool
    choice_types: list[str]  # every type a choice field allows, over all revisions

    # --- storage ---
    size: int
    size_const: str
    cpp_type: str
    data_type: str
    macro: str
    offset: int

    # --- provenance ---
    first_version_name: str
    first_version_idx: int
    conformance: dict[str, Conformance]  # FHIR revision -> its requirements

    # --- set by later passes, absent until then ---
    resolved_path: NotRequired[str]  # dotted path of a backbone element's block
    code_enum: NotRequired[CodeEnum]
    external_system: NotRequired[str]
    raw_scalar: NotRequired[bool]
    url_idx: NotRequired[bool]


class BlockLayout(TypedDict):
    """A generated C++ block: a resource, a datatype or a backbone element."""

    layout: list[FieldLayout]
    seen: set[str]  # field names already laid out
    sizes: dict[str, int]  # FHIR revision -> block size in bytes
    by_name: dict[str, FieldLayout]  # the same entries as `layout`, by FHIR name
    conformance: NotRequired[dict[str, Conformance]]  # root blocks only


# Dotted FHIR path -> its block, e.g. 'Patient.contact'.
MasterBlocks: TypeAlias = dict[str, BlockLayout]

# One root type's snapshot elements per FHIR revision, oldest first.
SchemasByVersion: TypeAlias = list[tuple[str, list[JsonObject]]]
