# ============================================================
# FastFHIR Generator — FHIR StructureDefinition extraction (model layer).
#
# Relocated body from tools/generator/ffc.py lines 199-957 (extraction,
# name resolution, field-kind / recovery / stride expressions)
# plus lines 2264-2330 (version discovery, resource resolution).
#
# This is the model's hub: it takes raw FHIR bundles and produces the
# typed layout dicts that emitters consume. No C++ strings here.
#
# Author: Ryan Landvater (ryanlandvater[at]gmail[dot]com)
# Copyright (c) 2025 Ryan Landvater. All rights reserved.
# License: FastFHIR Shared Source License (FF-SSL)
# ============================================================

from __future__ import annotations

import json
import os
import re
from typing import Any

from generator.model.type_map import (
    PRODUCTION_PROFILE_ENV,
    SCALAR_PRIMITIVE_TYPES,
    STRING_TYPES,
    TYPE_MAP,
    US_CORE_RESOURCES,
    UK_CORE_RESOURCES,
    _scalar_recovery_tag,
    _version_sort_key,
    sanitize_fhir_type,
)

# ---------------------------------------------------------------------------
# Path-based field-position overrides (per-resource/backbone V-Table offsets).
# Some FHIR fields have known offset positions that differ from the
# auto-computed stride based on type alone. This allows manual curation.
# ---------------------------------------------------------------------------
BLOCK_FIELD_OVERRIDES: dict = {}

# Extra accessor methods injected into specific view structs.
VIEW_EXTRA_METHODS: dict = {}

# Field-level overrides for the eager deserializer / ingest path.
INGEST_FIELD_OVERRIDES: dict = {}


# ---------------------------------------------------------------------------
# StructureDefinition extraction & naming
# ---------------------------------------------------------------------------




def load_npm_valueset_bundle(pkg_dir: str) -> dict:
    """Load all ValueSet-*.json AND CodeSystem-*.json files from an NPM
    package directory into a bundle-compatible dict with 'entry' list."""
    entries = []
    if not os.path.isdir(pkg_dir):
        return {"entry": entries}
    for fname in os.listdir(pkg_dir):
        if fname.endswith(".json") and (
            fname.startswith("ValueSet-") or fname.startswith("CodeSystem-")
        ):
            with open(os.path.join(pkg_dir, fname), "r", encoding="utf-8") as f:
                entries.append({"resource": json.load(f)})
    return {"entry": entries}



def load_npm_bundle(pkg_dir: str) -> dict:
    """Load all StructureDefinition-*.json files from an NPM package directory
    into a bundle-compatible dict with 'entry' list."""
    entries = []
    if not os.path.isdir(pkg_dir):
        return {"entry": entries}
    for fname in os.listdir(pkg_dir):
        if not fname.startswith("StructureDefinition-") or not fname.endswith(".json"):
            continue
        with open(os.path.join(pkg_dir, fname), "r", encoding="utf-8") as f:
            sd = json.load(f)
        entries.append({"resource": sd})
    return {"entry": entries}


def extract_structure_definition(bundle_json: dict, resource_name: str) -> list[dict]:
    """Return the snapshot element list for `resource_name` from a FHIR bundle.
    Raises ValueError if the resource is absent (fail-loud).
    """
    entries = bundle_json.get("entry", [])
    for entry in entries:
        resource = entry.get("resource", {})
        if (
            resource.get("resourceType") == "StructureDefinition"
            and resource.get("name") == resource_name
        ):
            return resource.get("snapshot", {}).get("element", [])
    raise ValueError(f"Resource '{resource_name}' not found.")


def _resolve_ff_struct_name(
    fhir_type: str,
    field_name: str,
    block_struct_name: str,
    resolved_path: str | None = None,
) -> str:
    """Emitted C++ struct/child-block name for a field's FHIR type."""
    # All string-like FHIR types map to FF_STRING (not individual view structs)
    if fhir_type == "string" or fhir_type in STRING_TYPES:
        return "FF_STRING"
    if fhir_type in ("BackboneElement", "Element"):
        if resolved_path:
            return "FF_" + resolved_path.replace(".", "_").upper()
        return f"{block_struct_name}_{field_name}"
    return f"FF_{fhir_type.upper()}"


def _resolve_data_type_name(
    fhir_type: str,
    field_orig_name: str,
    parent_path: str,
    resolved_path: str | None = None,
) -> str:
    """Emitted deserialization-data C++ type for a field."""
    fhir_type_l = fhir_type.lower()
    # All string-like FHIR types (dateTime, uri, markdown, etc.) map to string_view
    if fhir_type_l == "string" or fhir_type in STRING_TYPES:
        return "std::string_view"
    if fhir_type_l == "code":
        return "std::string"
    if fhir_type in ("BackboneElement", "Element"):
        if resolved_path:
            return resolved_path.replace(".", "") + "Data"
        return parent_path.replace(".", "") + field_orig_name + "Data"
    if fhir_type in TYPE_MAP and fhir_type != "DEFAULT":
        return TYPE_MAP[fhir_type]["data_type"]
    return fhir_type + "Data"


def _field_key_constant_name(raw_name: str) -> str:
    """Turn a camelCase FHIR field name into a C++ enum constant, e.g. 'FF_ACTIVE'."""
    snake = re.sub(r"(?<!^)(?=[A-Z])", "_", raw_name).upper()
    snake = re.sub(r"[^A-Z0-9]+", "_", snake)
    snake = re.sub(r"_+", "_", snake).strip("_")
    return f"FF_{snake}"


def _field_key_short_name(raw_name: str) -> str:
    """Return the short database-visible name for a field key (snake_case, no prefix)."""
    snake = re.sub(r"(?<!^)(?=[A-Z])", "_", raw_name).upper()
    snake = re.sub(r"[^A-Z0-9]+", "_", snake)
    return re.sub(r"_+", "_", snake).strip("_")


def _block_key_namespace(path: str) -> str:
    """Turn a dotted FHIR path into a C++ namespace identifier.
    Uses the full dotted path (including root resource) to ensure
    uniqueness across different resource types."""
    return re.sub(r"[^A-Z0-9_]", "_", path.replace(".", "_").upper())


# ---------------------------------------------------------------------------
# Recovery tag expressions (emitted as C++ enumerator names)
# ---------------------------------------------------------------------------

def _child_recovery_key_expr(f: dict, block_struct_name: str) -> str:
    """Return the RECOVER_FF_* enumerator expression for a field's on-disk child."""
    if f["fhir_type"] == "Resource":
        return "RECOVER_FF_RESOURCE"
    elif f["fhir_type"] == "code":
        return "RECOVER_FF_CODE"
    elif f["fhir_type"] == "boolean":
        return "RECOVER_FF_BOOL"
    elif f.get("is_array"):
        if f["fhir_type"] in ("string", "code"):
            return "RECOVER_FF_STRING"
        if f["fhir_type"] in SCALAR_PRIMITIVE_TYPES:
            return _scalar_recovery_tag(f["fhir_type"])
        child_struct = _resolve_ff_struct_name(
            f["fhir_type"], f["name"], block_struct_name, f.get("resolved_path")
        )
        return f"RECOVER_{child_struct}"
    elif f["fhir_type"] == "string":
        return "RECOVER_FF_STRING"
    elif f["cpp_type"] == "Offset":
        child_struct = _resolve_ff_struct_name(
            f["fhir_type"], f["name"], block_struct_name, f.get("resolved_path")
        )
        return f"RECOVER_{child_struct}"
    return "FF_RECOVER_UNDEFINED"


def _needs_getter(f: dict) -> bool:
    """Returns True when a field requires a parent-created getter method."""
    return f.get("is_array") or f["fhir_type"] == "string" or f["cpp_type"] == "Offset"


def _getter_return_type(f: dict, block_struct_name: str) -> str:
    """Return the C++ return type for a field's getter method."""
    if f.get("is_array"):
        return "FF_ARRAY"
    elif f["fhir_type"] == "string":
        return "FF_STRING"
    else:
        return _resolve_ff_struct_name(
            f["fhir_type"], f["name"], block_struct_name, f.get("resolved_path")
        )


# ---------------------------------------------------------------------------
# Field-kind expressions (emitted as FF_FIELD_* enumerators)
# ---------------------------------------------------------------------------

def _field_kind_expr(f: dict) -> str:
    """Return the C++ FF_FIELD_* enumerator for a field's wire-kind."""
    # Choice must be checked first — a choice field may have a fallback
    # fhir_type that would otherwise match an earlier branch incorrectly.
    if f.get("is_choice"):
        return "FF_FIELD_CHOICE"
    if f.get("is_array"):
        return "FF_FIELD_ARRAY"
    if f["fhir_type"] == "string":
        return "FF_FIELD_STRING"
    if f["cpp_type"] == "Offset":
        return "FF_FIELD_BLOCK"
    if f["fhir_type"] == "code":
        return "FF_FIELD_CODE"
    if f["fhir_type"] == "boolean":
        return "FF_FIELD_BOOL"
    if f.get("data_type") == "double":
        return "FF_FIELD_FLOAT64"
    if f.get("data_type") == "uint32_t":
        return "FF_FIELD_UINT32"
    if f.get("data_type") == "uint64_t":
        return "FF_FIELD_UINT64"
    if f["fhir_type"] == "Resource":
        return "FF_FIELD_RESOURCE"
    return "FF_FIELD_UNKNOWN"


def _compact_slot_size(f: dict) -> int:
    """Return the compact binary slot size (bytes) for a field.

    Must match compact_slot_size() in FF_Parser.cpp.  Only the SECOND
    definition in ffc.py is preserved here (the first was a duplicate
    shadowed by the second — flagged in generator_refactor_plan.md §1.2).
    """
    if f.get("is_choice"):
        return 10  # TYPE_SIZE_CHOICE
    if f["fhir_type"] == "Resource":
        return 10  # TYPE_SIZE_RESOURCE
    if f["fhir_type"] == "boolean":
        return 1   # TYPE_SIZE_UINT8
    if f["fhir_type"] == "code" and not f.get("is_array"):
        return 4   # TYPE_SIZE_UINT32 (code)
    if f.get("data_type") == "uint32_t":
        return 4   # TYPE_SIZE_UINT32/INT32
    if f.get("data_type") == "uint64_t":
        return 8   # TYPE_SIZE_UINT64
    if f.get("data_type") == "double":
        return 8   # TYPE_SIZE_FLOAT64
    # Offset arrays store arena-relative offsets (8 bytes each).
    if f.get("is_array") and f.get("array_entries_are_offsets", False):
        return 8   # TYPE_SIZE_OFFSET
    return 0


def _child_recovery_expr(f: dict, block_struct_name: str) -> str:
    """C++ recovery-tag expression for the deserializer's child-data member."""
    if f["fhir_type"] == "Resource":
        return "RECOVER_FF_RESOURCE"
    if f.get("is_array"):
        if f["fhir_type"] in ("string", "code"):
            return "RECOVER_FF_STRING"
        if f["fhir_type"] in SCALAR_PRIMITIVE_TYPES:
            return _scalar_recovery_tag(f["fhir_type"])
        child = _resolve_ff_struct_name(
            f["fhir_type"], f["name"], block_struct_name, f.get("resolved_path")
        )
        return f"RECOVER_{child}"
    if f["fhir_type"] == "string":
        return "RECOVER_FF_STRING"
    if f["fhir_type"] in SCALAR_PRIMITIVE_TYPES:
        return _scalar_recovery_tag(f["fhir_type"])
    child = _resolve_ff_struct_name(
        f["fhir_type"], f["name"], block_struct_name, f.get("resolved_path")
    )
    return f"RECOVER_{child}"


def _array_entries_are_offsets_expr(f: dict) -> str:
    """Return 'true'/'false' C++ literal for whether an array entry is an arena offset."""
    if f["fhir_type"] == "Resource":
        return "false"  # Resource arrays store ResourceReference inline, not offsets
    if f["fhir_type"] in ("string", "code"):
        return "false"  # Strings/codes are stored inline in arrays
    if f["fhir_type"] in SCALAR_PRIMITIVE_TYPES:
        return "false"  # Scalars are inline
    return "true"  # Block-typed children are arena offsets


def _annotate_external_systems(master_blocks: dict, external_system_map: dict | None) -> None:
    """Annotate code fields with their external code system (UCUM, etc.)."""
    if not external_system_map:
        return
    for path, blk in master_blocks.items():
        for f in blk.get("layout", []):
            if f.get("fhir_type") == "code":
                fhir_path = path + "." + f.get("orig_name", "")
                if fhir_path in external_system_map:
                    f["external_system"] = external_system_map[fhir_path]


def _annotate_code_enums(master_blocks: dict, code_enum_map: dict | None) -> None:
    """Annotate code fields with their corresponding ValueSet enum bindings."""
    if not code_enum_map:
        return
    for path, blk in master_blocks.items():
        for f in blk.get("layout", []):
            if f.get("fhir_type") == "code":
                fhir_path = path + "." + f.get("orig_name", "")
                if fhir_path in code_enum_map:
                    f["code_enum"] = code_enum_map[fhir_path]


# ---------------------------------------------------------------------------
# Version & resource discovery
# ---------------------------------------------------------------------------

def discover_versions(specs_dir: str = "fhir_packages") -> list[str]:
    """Discover available FHIR versions from extracted spec folders."""
    if not os.path.isdir(specs_dir):
        return []
    versions: list[str] = []
    for name in os.listdir(specs_dir):
        full = os.path.join(specs_dir, name)
        if os.path.isdir(full) and re.match(r"^R\d+[A-Za-z]*$", name):
            versions.append(name)
    return sorted(set(versions), key=_version_sort_key)


def _discover_resource_names(
    specs_dir: str = "fhir_packages",
    versions: list[str] | None = None,
    include_abstract: bool = False,
) -> list[str]:
    """Discover resource StructureDefinition names from NPM package files."""
    versions = versions or discover_versions(specs_dir)
    discovered: set[str] = set()
    for v in versions:
        pkg = os.path.join(specs_dir, v, "package")
        if not os.path.isdir(pkg):
            continue
        for fname in os.listdir(pkg):
            if not fname.startswith("StructureDefinition-") or not fname.endswith(".json"):
                continue
            with open(os.path.join(pkg, fname), "r", encoding="utf-8") as f:
                sd = json.load(f)
            if sd.get("resourceType") != "StructureDefinition":
                continue
            if sd.get("kind") != "resource":
                continue
            if not include_abstract and sd.get("abstract", False):
                continue
            name = sd.get("name")
            if name:
                discovered.add(name)
    return sorted(discovered)


def resolve_production_resources(
    specs_dir: str = "fhir_packages",
    versions: list[str] | None = None,
    profile: str | None = None,
) -> list[str]:
    """Resolve the active production resource set for the configured profile.

    profile values:
      - us (default): curated US Core resource list
      - uk: curated UK Core resource list
      - all: discover all concrete FHIR resources from profiles-resources.json
    Defaults to env FASTFHIR_PRODUCTION_PROFILE, then 'us'.
    """
    selected = (profile or os.getenv(PRODUCTION_PROFILE_ENV, "us")).strip().lower()
    if selected == "us":
        resources = list(US_CORE_RESOURCES)
    elif selected == "uk":
        resources = list(UK_CORE_RESOURCES)
    elif selected == "all":
        resources = _discover_resource_names(
            specs_dir=specs_dir, versions=versions, include_abstract=False
        )
        if not resources:
            raise RuntimeError(
                "FASTFHIR_PRODUCTION_PROFILE=all: no resources discovered. "
                "Ensure fhir_packages/<version>/package/ exists with StructureDefinition-*.json files."
            )
    else:
        raise RuntimeError(
            f"Unknown production profile: '{selected}'. "
            "Expected one of: us, uk, all."
        )
    return resources
