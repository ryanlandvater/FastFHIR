# =====================================================================
# FastFHIR Recovery Tag Emitter
#
# Projects dictionaries/master_tags.json -- the permanent tag ledger -- into
# include/FF_Recovery.hpp, and reconciles the ledger against the FHIR packages
# so a new FHIR release (R6, ...) appends its new types instead of renumbering
# anything.
#
# This is the same shape as emit/code_ids.py: a committed JSON ledger owns the
# NUMBERS, the emitter owns the C++ TEXT, and existing assignments are frozen.
# Recovery tags previously had no ledger, so their values lived only in a
# hand-maintained header and could not be reconciled against anything.
#
# Author: Ryan Landvater (ryanlandvater[at]gmail[dot]com)
# Copyright (c) 2026 Ryan Landvater. All rights reserved.
# License: Mozilla Public License, v. 2.0 (MPL-2.0) - see LICENSE or http://mozilla.org/MPL/2.0/
# =====================================================================

from __future__ import annotations

import glob
import json
import os

from generator.emit.header import write_if_changed

LEDGER_PATH = "dictionaries/master_tags.json"
HEADER_PATH = "include/FF_Recovery.hpp"

# Bands that discovery may append to. Primitive and scalar tags are FastFHIR's
# own concepts (FF_HEADER, FF_STRING, the inline scalars) and are seeded by hand
# in the ledger -- nothing in a StructureDefinition implies them.
_DISCOVERED_BANDS = ("datatype", "resource", "backbone")

_BAND_TITLE = {
    "primitive": "Core Primitives",
    "scalar": "Inline Scalars",
    "datatype": "Data Types",
    "resource": "Resources",
    "backbone": "Sub-elements / BackboneElements",
}


def tag_name(path: str) -> str:
    """FHIR path -> RECOVERY_TAG enumerator (`Bundle.entry` -> RECOVER_FF_BUNDLE_ENTRY)."""
    return "RECOVER_FF_" + path.replace(".", "_").replace("-", "_").upper()


def load_tag_ledger(path: str = LEDGER_PATH) -> dict:
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


def discover_fhir_tags(specs_dir: str = "fhir_packages") -> dict[str, set[str]]:
    """Every tag the WHOLE spec needs, by band. Profile-independent on purpose.

    The ledger covers all of FHIR so the emitted header is byte-identical for
    every FASTFHIR_PRODUCTION_PROFILE. A permanent wire artifact whose contents
    depend on build configuration is not a permanent wire artifact -- that
    asymmetry (dictionaries profile-independent, tags profile-dependent) was the
    original defect here.
    """
    datatypes: set[str] = set()
    resources: set[str] = set()
    backbones: set[str] = set()

    for pkg in sorted(glob.glob(os.path.join(specs_dir, "*", "package"))):
        for sd_path in sorted(glob.glob(os.path.join(pkg, "StructureDefinition-*.json"))):
            try:
                with open(sd_path, encoding="utf-8") as fh:
                    sd = json.load(fh)
            except (OSError, json.JSONDecodeError):
                continue
            if sd.get("resourceType") != "StructureDefinition" or sd.get("abstract"):
                continue
            # `constraint` means a PROFILE (US Core Patient, CQF-Questionnaire),
            # not a new type -- it reuses its base resource's tag. Counting them
            # inflated the resource set from 178 to 275 (TASKS.md A29.1).
            if sd.get("derivation") != "specialization":
                continue
            name = sd.get("name") or sd.get("id")
            if not name:
                continue
            kind = sd.get("kind")
            if kind in ("complex-type", "primitive-type"):
                datatypes.add(tag_name(name))
            elif kind == "resource":
                resources.add(tag_name(name))
                for element in sd.get("snapshot", {}).get("element", []):
                    if any(t.get("code") == "BackboneElement" for t in element.get("type", [])):
                        backbones.add(tag_name(element["path"]))

    return {"datatype": datatypes, "resource": resources, "backbone": backbones}


def reconcile_tag_ledger(ledger: dict, discovered: dict[str, set[str]]) -> int:
    """Append tags the ledger lacks. Returns how many were appended.

    APPEND-ONLY. An existing assignment is never touched, so re-running against a
    newer FHIR package set adds R6's types and leaves every prior stream
    decodable. Raises if a band would overflow rather than spilling into the next
    one, which would silently mis-classify every tag past the boundary.
    """
    tags = ledger["tags"]
    next_id = {b: int(v, 16) for b, v in ledger["_next_id"].items()}
    bands = {b: (int(v["first"], 16), int(v["last"], 16)) for b, v in ledger["_bands"].items()}
    appended = 0

    for band in _DISCOVERED_BANDS:
        for name in sorted(discovered.get(band, ())):
            if name in tags:
                continue
            _, last = bands[band]
            if next_id[band] > last:
                raise RuntimeError(
                    f"recovery-tag band '{band}' is full at 0x{last:04X}; cannot append "
                    f"{name}. Re-cut the band map in {LEDGER_PATH} and FF_Recovery.hpp "
                    "together -- and note that moving an existing tag is a wire break."
                )
            tags[name] = {"value": f"0x{next_id[band]:04X}", "band": band}
            next_id[band] += 1
            appended += 1

    ledger["_next_id"] = {b: f"0x{v:04X}" for b, v in next_id.items()}
    return appended


def assert_no_drift(ledger: dict, previous: dict) -> None:
    """Fail if any tag changed or vanished between two ledger states.

    This is the corruption check: a renumbered tag makes every archive holding
    the old value decode as the wrong block type, and a deleted one makes it
    decode as nothing. Additions are the normal case (a new FHIR release).
    """
    errors: list[str] = []
    for name, entry in previous.get("tags", {}).items():
        if name not in ledger["tags"]:
            errors.append(f"    DELETED {name} (was {entry['value']})")
        elif ledger["tags"][name]["value"] != entry["value"]:
            errors.append(
                f"    CHANGED {name}: {entry['value']} -> {ledger['tags'][name]['value']}"
            )
    if errors:
        raise RuntimeError(
            f"{len(errors)} recovery-tag drift violation(s) against the committed ledger:\n"
            + "\n".join(errors)
            + f"\n\nTag values in {LEDGER_PATH} are permanent wire constants. Append only."
        )


def _enum_body(ledger: dict) -> str:
    """The RECOVERY_TAG enumerators, grouped by band, ordered by value."""
    by_band: dict[str, list[tuple[int, str, str]]] = {b: [] for b in ledger["_bands"]}
    for name, entry in ledger["tags"].items():
        by_band[entry["band"]].append((int(entry["value"], 16), name, entry.get("note", "")))

    out = ""
    for band in ("primitive", "scalar", "datatype", "resource", "backbone"):
        entries = sorted(by_band.get(band, []))
        if not entries:
            continue
        first = int(ledger["_bands"][band]["first"], 16)
        last = int(ledger["_bands"][band]["last"], 16)
        out += (
            f"\n    // --- {_BAND_TITLE[band]} "
            f"(0x{first:04X} - 0x{last:04X}, {len(entries)} assigned) ---\n"
            f"    // Append only, inside this band. Never reorder, never renumber.\n"
        )
        for value, name, note in entries:
            comment = f"  // {note}" if note else ""
            out += f"    {name:<52} = 0x{value:04X},{comment}\n"
    return out


def _band_map_comment(ledger: dict) -> str:
    counts: dict[str, int] = {b: 0 for b in ledger["_bands"]}
    for entry in ledger["tags"].values():
        counts[entry["band"]] += 1

    rows = ""
    for band in ("primitive", "scalar", "datatype", "resource", "backbone"):
        first = int(ledger["_bands"][band]["first"], 16)
        last = int(ledger["_bands"][band]["last"], 16)
        slots = last - first + 1
        used = counts[band]
        rows += (
            f"//   {_BAND_TITLE[band]:<32} 0x{first:04X} - 0x{last:04X}  "
            f"{slots:>6,} slots  {used:>4} used\n"
        )
    return rows


def generate_recovery_header(ledger: dict, header_path: str = HEADER_PATH) -> None:
    """Emit include/FF_Recovery.hpp from the ledger."""
    bands = ledger["_bands"]
    b = {name: (v["first"], v["last"]) for name, v in bands.items()}
    counts: dict[str, int] = {name: 0 for name in bands}
    for entry in ledger["tags"].values():
        counts[entry["band"]] += 1

    hpp = f"""/**
 * @file FF_Recovery.hpp
 * @author Ryan Landvater (ryanlandvater[at]gmail[dot]com)
 * @copyright (c) 2026 Ryan Landvater. All rights reserved.
 * @version 0.1
 * @brief FastFHIR Recovery Tags
 * @license This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0 (MPL-2.0) - see LICENSE or http://mozilla.org/MPL/2.0/.
 *
 * GENERATED from {LEDGER_PATH} by generator/emit/recovery_tags.py. DO NOT EDIT.
 * Committed to the repo on purpose: these values decode every .ffhr stream ever
 * written, so they are reviewed in diffs like any other permanent constant.
 *
 * Regenerate when a new FHIR release lands (R6, ...). Reconciliation is
 * APPEND-ONLY -- new types take the next free value in their band and nothing
 * already assigned moves. The generator refuses to emit if any existing value
 * changed or disappeared, which is the drift-equals-corruption check: a
 * renumbered tag makes archives holding the old value decode as the wrong
 * block type.
 *
 * A recovery tag identifies the type of a data block during validation and
 * error handling, enabling recovery when parsing or processing FastFHIR
 * streams. It occupies bytes 8-9 of every DATA_BLOCK.
 */

// ============================================================
// BAND MAP  (bit 15 is RECOVER_ARRAY_BIT, so 0x0000-0x7FFF is the usable space)
//
{_band_map_comment(ledger)}//
// The ledger -- and therefore this header -- covers the WHOLE FHIR spec
// (R4 union R5), never just the compiled profile. FF_Recovery.hpp is byte
// identical for every FASTFHIR_PRODUCTION_PROFILE: a permanent wire artifact
// must not depend on build configuration. Only which resources get generated
// C++ varies with the profile; which tags EXIST does not.
//
// Bands are NOT merely documentation. FF_IsScalarBlockTag / FF_IsResourceTag /
// FF_IsBackboneTag (FF_Utilities.hpp) classify a block by which band its tag
// falls in, so a tag written into the wrong band is silently mis-classified at
// runtime rather than failing to compile. Guards:
//   * the static_asserts below, which catch a boundary edit that overlaps two
//     bands or shrinks one below what the spec already needs;
//   * generator/utilities.py:validate_recovery_bands(), which checks on every
//     run that each tag lies inside a band and no two tags share a value.
// ============================================================
#pragma once

#include <cstdint>

// =====================================================================
// RECOVERY TAG REGISTRY
// =====================================================================
enum RECOVERY_TAG : uint16_t {{
{_enum_body(ledger)}}};

constexpr uint16_t RECOVER_ARRAY_BIT = 0x8000;
constexpr uint16_t RECOVER_TYPE_MASK = 0x7FFF;

// --- Band boundaries (see the BAND MAP at the top of this file) --------------
// Inclusive [FIRST, LAST]. These are wire constants: a tag's band is part of its
// identity, and the classifier predicates in FF_Utilities.hpp dispatch on them.
constexpr uint16_t RECOVER_BAND_PRIMITIVE_FIRST = {b["primitive"][0]};
constexpr uint16_t RECOVER_BAND_PRIMITIVE_LAST  = {b["primitive"][1]};
constexpr uint16_t RECOVER_BAND_SCALAR_FIRST    = {b["scalar"][0]};
constexpr uint16_t RECOVER_BAND_SCALAR_LAST     = {b["scalar"][1]};
constexpr uint16_t RECOVER_BAND_DATATYPE_FIRST  = {b["datatype"][0]};
constexpr uint16_t RECOVER_BAND_DATATYPE_LAST   = {b["datatype"][1]};
constexpr uint16_t RECOVER_BAND_RESOURCE_FIRST  = {b["resource"][0]};
constexpr uint16_t RECOVER_BAND_RESOURCE_LAST   = {b["resource"][1]};
constexpr uint16_t RECOVER_BAND_BACKBONE_FIRST  = {b["backbone"][0]};
constexpr uint16_t RECOVER_BAND_BACKBONE_LAST   = {b["backbone"][1]};

// Bands must partition [0, RECOVER_TYPE_MASK] with no gap and no overlap. An
// off-by-one here silently reclassifies every tag on the wrong side of it.
static_assert(RECOVER_BAND_PRIMITIVE_FIRST == 0, "primitive band must start at 0");
static_assert(RECOVER_BAND_PRIMITIVE_LAST + 1 == RECOVER_BAND_SCALAR_FIRST,   "gap/overlap: primitive|scalar");
static_assert(RECOVER_BAND_SCALAR_LAST    + 1 == RECOVER_BAND_DATATYPE_FIRST, "gap/overlap: scalar|datatype");
static_assert(RECOVER_BAND_DATATYPE_LAST  + 1 == RECOVER_BAND_RESOURCE_FIRST, "gap/overlap: datatype|resource");
static_assert(RECOVER_BAND_RESOURCE_LAST  + 1 == RECOVER_BAND_BACKBONE_FIRST, "gap/overlap: resource|backbone");
static_assert(RECOVER_BAND_BACKBONE_LAST == RECOVER_TYPE_MASK, "backbone band must run to the type mask");

// Each band's base marker must sit in its own band -- the cheapest check that a
// whole block was not pasted at the wrong offset.
static_assert(RECOVER_FF_SCALAR_BLOCK    == RECOVER_BAND_SCALAR_FIRST,   "scalar block base moved");
static_assert(RECOVER_FF_DATA_TYPE_BLOCK == RECOVER_BAND_DATATYPE_FIRST, "datatype block base moved");
static_assert(RECOVER_FF_RESOURCE_BLOCK  == RECOVER_BAND_RESOURCE_FIRST, "resource block base moved");
static_assert(RECOVER_FF_BACKBONE_BLOCK  == RECOVER_BAND_BACKBONE_FIRST, "backbone block base moved");

// Each band must still hold what the spec currently needs. Counts are what the
// ledger actually assigns, so shrinking a band below its own contents fails here.
static_assert(RECOVER_BAND_DATATYPE_LAST - RECOVER_BAND_DATATYPE_FIRST + 1 >= {counts["datatype"]},
              "datatype band too small for the assigned tags");
static_assert(RECOVER_BAND_RESOURCE_LAST - RECOVER_BAND_RESOURCE_FIRST + 1 >= {counts["resource"]},
              "resource band too small for the assigned tags");
static_assert(RECOVER_BAND_BACKBONE_LAST - RECOVER_BAND_BACKBONE_FIRST + 1 >= {counts["backbone"]},
              "backbone band too small for the assigned tags");

inline constexpr bool IsArrayTag(RECOVERY_TAG tag) {{return(tag & RECOVER_ARRAY_BIT)!= 0;}}
inline constexpr RECOVERY_TAG GetTypeFromTag(RECOVERY_TAG tag) {{return static_cast<RECOVERY_TAG>(tag & RECOVER_TYPE_MASK);}}
inline constexpr RECOVERY_TAG ToArrayTag(RECOVERY_TAG base_tag) {{return static_cast<RECOVERY_TAG>(base_tag | RECOVER_ARRAY_BIT);}}
"""
    write_if_changed(header_path, hpp)


def generate_recovery_tags(
    specs_dir: str = "fhir_packages",
    ledger_path: str = LEDGER_PATH,
    header_path: str = HEADER_PATH,
) -> tuple[int, int]:
    """Reconcile the ledger against the packages and emit the header.

    Returns (total_tags, appended). The ledger is rewritten only when something
    was appended, so a normal build leaves it untouched in git.
    """
    ledger = load_tag_ledger(ledger_path)
    previous = json.loads(json.dumps(ledger))  # frozen copy for the drift check

    appended = reconcile_tag_ledger(ledger, discover_fhir_tags(specs_dir))
    assert_no_drift(ledger, previous)

    if appended:
        with open(ledger_path, "w", encoding="utf-8") as fh:
            json.dump(ledger, fh, indent=2)
            fh.write("\n")

    generate_recovery_header(ledger, header_path)
    return len(ledger["tags"]), appended
