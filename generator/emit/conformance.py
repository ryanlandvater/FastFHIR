"""Emit the attachable conformance layer -- rule TABLES, not check code.

What this emits, and what it deliberately does not
--------------------------------------------------
The Iris File Extension emits a check body per field per block. At 18 blocks
that reads well; at 209 blocks and thousands of fields it would be a megabyte of
near-identical C++ in which every fix is a regeneration. So this emitter
produces DATA -- a `constexpr Rule[]` per block, plus a tag-keyed dispatch table
-- and the single hand-written engine in `src/conformance/FF_ConformanceEngine.hpp`
interprets it.

A Rule addresses its field by ORDINAL: the field's index in the block's layout,
which is the same list `emit/poco_visit.py` walks to emit `visit_fields()`. The
two cannot drift, because one list produces both.

THERE IS NO FHIRPATH EVALUATOR HERE, and there must never be one (TASKS.md
K3.3). `constraint[].expression` is a full language; a partial evaluator inside
an emitter is how a schema quietly becomes a programming language. Every
invariant is therefore emitted as an UNIMPLEMENTED row carrying its key and its
human text, so a report can say what it did NOT check -- which is the difference
between a blank that means "passed" and a blank that means "forgotten".

The allow-list, and what measurement did to it
----------------------------------------------
TASKS.md K3.1 asked for four enforced shapes. Measured against the compiled
profile before writing a line of C++:

    min >= 1                required-element presence   REAL, hundreds of rows
    max as a number > 1     array upper bound           ZERO rows: the base spec
                                                        spells max only "1" or "*"
    fixed[x] / pattern[x]   exact value                 ZERO rows: fixed values
                                                        live in PROFILES, and the
                                                        build profile selects
                                                        resources, not profile
                                                        constraints
    required binding        ValueSet membership         REAL, but it is TERMINOLOGY
                                                        membership -- Block J's
                                                        work, through this same
                                                        ValidationHooks struct

So REQUIRED is enforced, MAX_CARDINALITY is emitted where the data ever carries
one (the engine implements it; the base spec supplies no rows), and the other
two are recorded as UNIMPLEMENTED rows carrying the bound ValueSet URL where
they have one. Nothing is silently dropped.

Copyright (c) 2026 Ryan Landvater. All rights reserved.
License: Mozilla Public License, v. 2.0 (MPL-2.0) -- see LICENSE or http://mozilla.org/MPL/2.0/
"""

from __future__ import annotations

import os

from generator.emit.header import auto_header, write_if_changed

# Version mask bits. These mirror CONF_VERSION_* in include/FF_Conformance.hpp;
# FHIR_VERSION's own values (0x0400, 0x0500) are ordinals and cannot express
# "both", which is what a rule that holds for R4 and R5 alike needs to say.
_VERSION_BITS: dict[str, int] = {"R4": 1, "R5": 2}
_VERSION_ALL: int = 3

# The base specification's own canonical URL space. A profile would supply its
# own; the build profile selects resources, not profiles, so every rule this
# emitter produces cites the base definition.
_SD_BASE: str = "http://hl7.org/fhir/StructureDefinition/"

# An ordinal no field can occupy. Rules that belong to the BLOCK rather than to
# one of its fields -- resource-level invariants such as dom-2 or bdl-1 -- get
# this, so they sort to the end and the engine's ordinal cursor never stops on
# them. They are records, queried through conformance_rules(); they never fire.
_NO_FIELD_ORDINAL: int = 0xFFFF


def _cxx_string(text: str) -> str:
    """Escape `text` into a C++ string literal that is pure 7-bit ASCII.

    FHIR human text carries curly quotes and dashes, and the generated sources
    are compiled on Windows toolchains whose default source encoding is not
    UTF-8. Every byte outside printable ASCII becomes a three-digit octal
    escape: three digits exactly, because an octal escape consumes at most
    three and so can never swallow the character that follows it. A hex escape
    is greedy and would.
    """
    out: list[str] = []
    for byte in text.encode("utf-8"):
        char = chr(byte)
        if char == '"':
            out.append('\\"')
        elif char == "\\":
            out.append("\\\\")
        elif 0x20 <= byte < 0x7F:
            out.append(char)
        else:
            out.append(f"\\{byte:03o}")
    return '"' + "".join(out) + '"'


def _root_of(path: str) -> str:
    """The StructureDefinition a dotted block path belongs to."""
    return path.split(".")[0]


def _group_by_value(per_version: dict[str, int]) -> dict[int, int]:
    """{version name: value} -> {value: OR-ed version mask}.

    R4 and R5 genuinely disagree -- Condition.clinicalStatus is required in R5
    and optional in R4 -- so a rule is emitted once per distinct value, masked
    to the revisions that actually state it. Enforcing R5's cardinality against
    an R4 document would be a bug that reads like a spec violation.
    """
    grouped: dict[int, int] = {}
    for version, value in per_version.items():
        bit = _VERSION_BITS.get(version, 0)
        grouped[value] = grouped.get(value, 0) | bit
    return grouped


def _rules_for_block(path: str, blk: dict) -> tuple[list[dict], dict[str, int]]:
    """Build every Rule row for one block. Returns (rows, per-kind counts)."""
    rows: list[dict] = []
    counts: dict[str, int] = {
        "required": 0,
        "max_cardinality": 0,
        "unimplemented_invariant": 0,
        "unimplemented_binding": 0,
        "unimplemented_fixed": 0,
    }
    url = _SD_BASE + _root_of(path)

    for ordinal, field in enumerate(blk.get("layout", [])):
        conformance = field.get("conformance") or {}
        if not conformance:
            continue
        element = f"{path}.{field['orig_name']}"

        # --- min >= 1: the one shape that is enforced ---------------------
        mins = {v: c["min"] for v, c in conformance.items() if c["min"] >= 1}
        for minimum, mask in _group_by_value(mins).items():
            rows.append(
                {
                    "kind": "REQUIRED",
                    "ordinal": ordinal,
                    "versions": mask,
                    "min": minimum,
                    "max": 0,
                    "path": element,
                    "key": "",
                    "binding": "",
                    "human": (
                        f"{element} is required: the FHIR specification declares "
                        f"minimum cardinality {minimum}."
                    ),
                    "url": url,
                }
            )
            counts["required"] += 1

        # --- a numeric max: emitted when the data ever carries one --------
        maxes = {
            v: int(c["max"])
            for v, c in conformance.items()
            if c["max"] not in (None, "*", "1") and str(c["max"]).isdigit()
        }
        for maximum, mask in _group_by_value(maxes).items():
            rows.append(
                {
                    "kind": "MAX_CARDINALITY",
                    "ordinal": ordinal,
                    "versions": mask,
                    "min": 0,
                    "max": maximum,
                    "path": element,
                    "key": "",
                    "binding": "",
                    "human": (
                        f"{element} admits at most {maximum} element(s) per the "
                        f"FHIR specification."
                    ),
                    "url": url,
                }
            )
            counts["max_cardinality"] += 1

        # --- required bindings: recorded, and handed to Block J -----------
        bindings: dict[str, int] = {}
        for version, conf in conformance.items():
            if conf["binding_strength"] == "required" and conf["binding_valueset"]:
                bit = _VERSION_BITS.get(version, 0)
                vs = conf["binding_valueset"]
                bindings[vs] = bindings.get(vs, 0) | bit
        for valueset, mask in bindings.items():
            rows.append(
                {
                    "kind": "UNIMPLEMENTED",
                    "ordinal": ordinal,
                    "versions": mask,
                    "min": 0,
                    "max": 0,
                    "path": element,
                    "key": "",
                    "binding": valueset,
                    "human": (
                        f"{element} is bound (required) to {valueset}. Membership is "
                        f"terminology validation and is NOT checked by this layer."
                    ),
                    "url": url,
                }
            )
            counts["unimplemented_binding"] += 1

        # --- fixed[x] / pattern[x]: recorded, never enforced --------------
        fixed_versions = 0
        for version, conf in conformance.items():
            if conf["fixed"] or conf["pattern"]:
                fixed_versions |= _VERSION_BITS.get(version, 0)
        if fixed_versions:
            rows.append(
                {
                    "kind": "UNIMPLEMENTED",
                    "ordinal": ordinal,
                    "versions": fixed_versions,
                    "min": 0,
                    "max": 0,
                    "path": element,
                    "key": "",
                    "binding": "",
                    "human": (
                        f"{element} declares a fixed or pattern value, which this "
                        f"layer records but does NOT check."
                    ),
                    "url": url,
                }
            )
            counts["unimplemented_fixed"] += 1

    # --- invariants, field-level and block-level, deduplicated by key -----
    # ele-1, ext-1 and dom-* recur on nearly every element of every block; one
    # row per key per block says the same thing without saying it 40 times.
    invariants: dict[str, dict] = {}

    def _collect(conformance: dict, element: str) -> None:
        for version, conf in conformance.items():
            bit = _VERSION_BITS.get(version, 0)
            for constraint in conf["constraints"]:
                key = constraint["key"]
                if not key:
                    continue
                seen = invariants.setdefault(
                    key,
                    {
                        "human": constraint["human"],
                        "path": element,
                        "versions": 0,
                        "severity": constraint["severity"],
                    },
                )
                seen["versions"] |= bit

    for field in blk.get("layout", []):
        _collect(field.get("conformance") or {}, f"{path}.{field['orig_name']}")
    _collect(blk.get("conformance") or {}, path)

    for key, info in sorted(invariants.items()):
        rows.append(
            {
                "kind": "UNIMPLEMENTED",
                "ordinal": _NO_FIELD_ORDINAL,
                "versions": info["versions"],
                "min": 0,
                "max": 0,
                "path": info["path"],
                "key": key,
                "binding": "",
                "human": (
                    f"{info['severity'] or 'constraint'} {key}: {info['human']} "
                    f"This invariant is expressed in FHIRPath and is NOT evaluated "
                    f"by this layer."
                ),
                "url": url,
            }
        )
        counts["unimplemented_invariant"] += 1

    rows.sort(key=lambda r: (r["ordinal"], r["kind"], r["path"], r["key"]))
    return rows, counts


def _emit_rule(row: dict) -> str:
    """One Rule aggregate initialiser, field order matching FF_Conformance.hpp."""
    return (
        "    {"
        f"{_cxx_string(row['path'])}, "
        f"{_cxx_string(row['key'])}, "
        f"{_cxx_string(row['human'])}, "
        f"{_cxx_string(row['url'])}, "
        f"{_cxx_string(row['binding'])}, "
        f"{row['min']}, {row['max']}, {row['ordinal']}, "
        f"RuleKind::{row['kind']}, {row['versions']}"
        "},\n"
    )


def generate_conformance_layer(
    all_blocks: dict,
    resources: list[str],
    tag_values: dict[str, int],
    output_dir: str = "generated_src",
) -> dict[str, int]:
    """Emit FF_Conformance_Layer.{hpp,cpp}. Returns the per-kind row counts."""
    totals: dict[str, int] = {
        "required": 0,
        "max_cardinality": 0,
        "unimplemented_invariant": 0,
        "unimplemented_binding": 0,
        "unimplemented_fixed": 0,
    }
    blocks_without_tag: list[str] = []

    # Sorted by TAG VALUE, because ValidationHooks::find() is a binary search.
    # The generated file static_asserts the ordering, so a disagreement between
    # this sort and TypeTraits<>::recovery is a compile error rather than a
    # layer that silently never fires.
    def _tag_name(path: str) -> str:
        return "RECOVER_FF_" + path.replace(".", "_").upper()

    ordered: list[tuple[int, str]] = []
    for path in all_blocks:
        name = _tag_name(path)
        if name not in tag_values:
            blocks_without_tag.append(path)
            continue
        ordered.append((tag_values[name], path))
    ordered.sort()

    if blocks_without_tag:
        raise RuntimeError(
            f"{len(blocks_without_tag)} generated block(s) have no RECOVERY_TAG in "
            f"dictionaries/master_tags.json, so the conformance layer could not key "
            f"a dispatch entry for them: {sorted(blocks_without_tag)[:8]}"
        )

    body: list[str] = []
    entries: list[str] = []
    rules_index: list[str] = []

    for _value, path in ordered:
        s_name = "FF_" + path.replace(".", "_").upper()
        d_name = path.replace(".", "") + "Data"
        rows, counts = _rules_for_block(path, all_blocks[path])
        for key, count in counts.items():
            totals[key] += count

        if rows:
            body.append(f"constexpr Rule {s_name}_RULES[] = {{\n")
            for row in rows:
                body.append(_emit_rule(row))
            body.append("};\n")
            table = f"{s_name}_RULES"
            size = f"static_cast<uint32_t>(std::size({s_name}_RULES))"
            rules_index.append(
                f"    case TypeTraits<{d_name}>::recovery:\n"
                f"        return {{{s_name}_RULES, std::size({s_name}_RULES)}};\n"
            )
        else:
            # A block with no rules still needs a check: it is the only thing
            # that DESCENDS into its children. Bundle carries no rule of its
            # own, and Bundle.entry.request.method is required.
            table = "nullptr"
            size = "0"

        body.append(
            f"Status check_{s_name}(const void* data, uint32_t version,\n"
            f"                      const ValidationHooks* self) noexcept\n"
            f"{{\n"
            f"    return run_rules(*static_cast<const {d_name}*>(data), {table}, {size},\n"
            f"                     version, self);\n"
            f"}}\n\n"
        )
        entries.append(f"    {{TypeTraits<{d_name}>::recovery, &check_{s_name}}},\n")

    if totals["required"] == 0:
        raise RuntimeError(
            "The conformance emitter produced ZERO required-element rules. The FHIR "
            "specification declares min >= 1 on Observation.status, Bundle.type and "
            "many others, so an empty table means the conformance facts are not "
            "reaching this emitter -- not that the specification has no requirements."
        )

    includes = "".join(f'#include "FF_{r}.hpp"\n' for r in sorted(resources))

    hpp = (
        f"{auto_header}#pragma once\n\n"
        '#include "FF_Conformance.hpp"\n'
        '#include "FF_RecoveryTags.hpp"\n\n'
        "#include <span>\n\n"
        "// The attachable FHIR conformance layer.\n"
        "//\n"
        "// Deliberately OUTSIDE libfastfhir: conformance policy is a development\n"
        "// aid, not a production dependency, and a shipped product should link it\n"
        "// only when it wants it. Build with -DFASTFHIR_BUILD_CONFORMANCE=ON and\n"
        "// link fastfhir_conformance.\n"
        "//\n"
        "//     std::atomic<uint64_t> failures{0};\n"
        "//     auto hooks = FastFHIR::Conformance::conformance_layer();\n"
        "//     hooks.policy     = FastFHIR::Conformance::LayerPolicy::Report;\n"
        "//     hooks.diagnostic = &logger;\n"
        "//     hooks.failures   = &failures;\n"
        "//     FF_BuilderAttachLayer({.builder = builder, .hooks = &hooks});\n"
        "//\n"
        "// Copy the struct before touching it, as above: the one this returns is\n"
        "// shared and immutable.\n"
        "namespace FastFHIR\n{\nnamespace Conformance\n{\n\n"
        "/// The generated layer. Shared, immutable, and safe to read from any thread.\n"
        "[[nodiscard]] const ValidationHooks& conformance_layer() noexcept;\n\n"
        "/// Every rule recorded for one block type, INCLUDING the UNIMPLEMENTED\n"
        "/// rows. This is how a caller asks what was not checked: an invariant this\n"
        "/// layer never evaluates is still listed here, with its key and its reason.\n"
        "[[nodiscard]] std::span<const Rule> conformance_rules(RECOVERY_TAG tag) noexcept;\n\n"
        "} // namespace Conformance\n} // namespace FastFHIR\n"
    )

    cpp = (
        f"{auto_header}"
        '#include "FF_Conformance_Layer.hpp"\n'
        '#include "FF_ConformanceEngine.hpp"\n'
        '#include "FF_StreamCheck.hpp"\n'
        f"{includes}\n"
        "#include <iterator>\n\n"
        "namespace FastFHIR\n{\nnamespace Conformance\n{\nnamespace\n{\n\n"
        + "".join(body)
        + "constexpr Entry ENTRIES[] = {\n"
        + "".join(entries)
        + "};\n\n"
        "/// find() is a binary search, so a mis-sorted table would not fail -- it\n"
        "/// would quietly stop finding checks. Proven at compile time instead.\n"
        "constexpr bool entries_are_sorted() noexcept\n"
        "{\n"
        "    for (std::size_t i = 1; i < std::size(ENTRIES); ++i)\n"
        "        if (!(ENTRIES[i - 1].tag < ENTRIES[i].tag)) return false;\n"
        "    return true;\n"
        "}\n"
        "static_assert(entries_are_sorted(),\n"
        '              "conformance dispatch table is not sorted by RECOVERY_TAG");\n\n'
        "} // namespace\n\n"
        "const ValidationHooks& conformance_layer() noexcept\n"
        "{\n"
        "    static const ValidationHooks layer = [] {\n"
        "        ValidationHooks hooks{};\n"
        "        hooks.entries = ENTRIES;\n"
        "        hooks.count   = static_cast<uint32_t>(std::size(ENTRIES));\n"
        "        hooks.policy  = LayerPolicy::Throw;\n"
        "        // The one whole-document check. A per-block check cannot ask\n"
        "        // whether a reference resolves, because the target may not be\n"
        "        // written yet; this runs at finalize, when the entry set is\n"
        "        // complete. See src/conformance/FF_StreamCheck.hpp.\n"
        "        hooks.stream_check = &check_stream_references;\n"
        "        return hooks;\n"
        "    }();\n"
        "    return layer;\n"
        "}\n\n"
        "std::span<const Rule> conformance_rules(RECOVERY_TAG tag) noexcept\n"
        "{\n"
        "    switch (tag)\n"
        "    {\n" + "".join(rules_index) + "    default:\n        return {};\n"
        "    }\n"
        "}\n\n"
        "} // namespace Conformance\n} // namespace FastFHIR\n"
    )

    write_if_changed(os.path.join(output_dir, "FF_Conformance_Layer.hpp"), hpp)
    write_if_changed(os.path.join(output_dir, "FF_Conformance_Layer.cpp"), cpp)
    return totals
