# ============================================================
# FastFHIR Generator — typed layout model.
#
# A strongly-typed view over the per-field layout dict that ffc.py builds. The
# generator Python's stated goal is "readable Python that mirrors C++ strong-
# typing conventions" — bare stringly-typed dicts (`f['fhir_type']`) defeat that.
#
# S2 STRATEGY (non-breaking): these dataclasses are an ADAPTER, not a rewrite.
# `Field.from_dict` / `Field.to_dict` round-trip the exact dict shape the
# existing emitters consume, so we can introduce types and unit-test the layout
# WITHOUT changing any wire output. Emitters keep taking dicts until a later
# sprint migrates them one at a time, each guarded by the wire-format gate.
#
# The dict keys mirrored here (verified against ffc.py):
#   fhir_type, name, orig_name, is_array, is_choice, cpp_type, data_type,
#   resolved_path, code_enum
#
# Author: Ryan Landvater (ryanlandvater[at]gmail[dot]com)
# Copyright (c) 2025 Ryan Landvater. All rights reserved.
# License: Mozilla Public License, v. 2.0 (MPL-2.0) — see LICENSE or http://mozilla.org/MPL/2.0/
# ============================================================

from __future__ import annotations

from dataclasses import dataclass, field as dc_field
from typing import Any


@dataclass
class Field:
    """One V-Table field of a generated block.

    Mirrors the layout dict ffc.py builds. `offset` is intentionally absent:
    in the emitted C++ the byte offset is a SYMBOLIC sum
    (`RECOVERY = VALIDATION + VALIDATION_S`) the compiler resolves, so the
    wire position is determined by field ORDER + each field's size constant,
    not a literal stored here. See generator_refactor_plan.md (deleted; in git history) section 6.
    """

    # --- identity ---
    name: str                       # sanitized field name used in C++ identifiers
    orig_name: str                  # original FHIR element name (for path joins)
    fhir_type: str                  # FHIR type token, e.g. 'string', 'code', 'Reference'

    # --- emitted C++ typing ---
    cpp_type: str                   # view/struct type, e.g. 'Offset', 'uint32_t'
    data_type: str                  # eager-deserialize data type, e.g. 'std::string_view'

    # --- shape flags (drive field-kind / recovery / stride decisions) ---
    is_array: bool = False
    is_choice: bool = False

    # --- optional resolution context ---
    resolved_path: str | None = None  # dotted FHIR path for backbone elements
    code_enum: dict[str, str] | None = None  # ValueSet enum binding, if any

    # Any layout keys we have not explicitly modelled are preserved verbatim so
    # the adapter is loss-free even as ffc.py evolves. Never read these in new
    # code — promote them to real fields instead.
    extra: dict[str, Any] = dc_field(default_factory=dict)

    # Keys this dataclass owns; everything else falls into `extra`.
    _OWNED = frozenset({
        "name", "orig_name", "fhir_type", "cpp_type", "data_type",
        "is_array", "is_choice", "resolved_path", "code_enum",
    })

    @classmethod
    def from_dict(cls, d: dict[str, Any]) -> Field:
        """Build a Field from an ffc.py layout dict, preserving unknown keys."""
        return cls(
            name=d["name"],
            orig_name=d.get("orig_name", d["name"]),
            fhir_type=d["fhir_type"],
            cpp_type=d["cpp_type"],
            data_type=d["data_type"],
            is_array=bool(d.get("is_array", False)),
            is_choice=bool(d.get("is_choice", False)),
            resolved_path=d.get("resolved_path"),
            code_enum=d.get("code_enum"),
            extra={k: v for k, v in d.items() if k not in cls._OWNED},
        )

    def to_dict(self) -> dict[str, Any]:
        """Reproduce the exact dict shape emitters consume.

        Round-trips losslessly: `Field.from_dict(d).to_dict()` restores `d`
        (modulo absent optional keys, which emitters access via `.get`).
        """
        out: dict[str, Any] = {
            "name": self.name,
            "orig_name": self.orig_name,
            "fhir_type": self.fhir_type,
            "cpp_type": self.cpp_type,
            "data_type": self.data_type,
            "is_array": self.is_array,
            "is_choice": self.is_choice,
        }
        if self.resolved_path is not None:
            out["resolved_path"] = self.resolved_path
        if self.code_enum is not None:
            out["code_enum"] = self.code_enum
        out.update(self.extra)
        return out


@dataclass
class Block:
    """A generated C++ block (resource, datatype, or backbone element).

    `layout` is the ordered field list — order is wire-significant because it
    determines V-Table offsets. Mirrors `master_blocks[path]` in ffc.py, whose
    canonical key is `layout`.
    """

    path: str                       # dotted FHIR path, e.g. 'Patient.contact'
    struct_name: str                # emitted C++ struct name, e.g. 'FF_PATIENT'
    layout: list[Field] = dc_field(default_factory=list)
    extra: dict[str, Any] = dc_field(default_factory=dict)

    @classmethod
    def from_dict(cls, path: str, struct_name: str, d: dict[str, Any]) -> Block:
        return cls(
            path=path,
            struct_name=struct_name,
            layout=[Field.from_dict(f) for f in d.get("layout", [])],
            extra={k: v for k, v in d.items() if k != "layout"},
        )

    def to_dict(self) -> dict[str, Any]:
        out: dict[str, Any] = {"layout": [f.to_dict() for f in self.layout]}
        out.update(self.extra)
        return out
