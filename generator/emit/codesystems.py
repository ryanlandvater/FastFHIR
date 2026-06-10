# =====================================================================
# FastFHIR Code Systems Generator
#
# Scans FHIR StructureDefinitions for 'code' fields with required
# bindings, cross-references ValueSet bundles to extract allowed values,
# and generates FF_CodeSystems.hpp with enum classes + parse/serialize
# helpers for each bounded code system.
#
# Relocated from tools/generator/ffcs.py. Logic is verbatim.
# `auto_header` / `write_if_changed` now imported from emit/header.py.
#
# Author: Ryan Landvater (ryanlandvater[at]gmail[dot]com)
# Copyright (c) 2025 Ryan Landvater. All rights reserved.
# License: FastFHIR Shared Source License (FF-SSL)
# =============================================================

import json
import os
import re

from generator.emit.header import auto_header, write_if_changed

EXCLUDED_VALUESET_FRAGMENTS: set[str] = {
    "all-languages",
    "mimetypes",
    "ucum-units",
    "bcp:47",
    "languages",
}

SYMBOL_CODES: dict[str, str] = {
    "<": "LessThan",
    "<=": "LessOrEqual",
    ">=": "GreaterOrEqual",
    ">": "GreaterThan",
}


def _to_pascal_case(s: str) -> str:
    """Convert a kebab-case / dot-separated string to PascalCase."""
    parts = re.split(r"[^a-zA-Z0-9]+", s)
    return "".join(p[0].upper() + p[1:] for p in parts if p)


def _code_to_identifier(code: str) -> str:
    """Convert a FHIR code value to a valid C++ enum identifier."""
    if code in SYMBOL_CODES:
        return SYMBOL_CODES[code]
    parts = re.split(r"[^a-zA-Z0-9]+", code)
    parts = [p for p in parts if p]
    if not parts:
        return "Empty"
    return "".join(p[0].upper() + p[1:] for p in parts)


def _collect_cs_codes(concepts: list[dict]) -> set[str]:
    """Recursively collect codes from a CodeSystem concept hierarchy."""
    codes: set[str] = set()
    for c in concepts:
        val = c.get("code", "")
        if val:
            codes.add(val)
        codes.update(_collect_cs_codes(c.get("concept", [])))
    return codes


def _build_bundle_index(bundle_json: dict) -> tuple[dict, dict]:
    """Index ValueSet and CodeSystem resources from a FHIR bundle."""
    valuesets: dict[str, dict] = {}
    codesystems: dict[str, dict] = {}
    for entry in bundle_json.get("entry", []):
        res = entry.get("resource", {})
        url = res.get("url", "")
        rt = res.get("resourceType")
        if rt == "ValueSet" and url:
            valuesets[url] = res
        elif rt == "CodeSystem" and url:
            codesystems[url] = res
    return valuesets, codesystems


def _get_valueset_codes(
    vs_url: str, valuesets: dict, codesystems: dict
) -> tuple[str | None, set[str]]:
    """Extract codes and display name for a ValueSet URL."""
    vs = valuesets.get(vs_url)
    if not vs:
        return None, set()

    name: str | None = vs.get("name", "")
    codes: set[str] = set()

    for inc in vs.get("compose", {}).get("include", []):
        for concept in inc.get("concept", []):
            c = concept.get("code", "")
            if c:
                codes.add(c)
        # If no explicit concepts, pull from the referenced CodeSystem
        if not inc.get("concept") and inc.get("system"):
            cs = codesystems.get(inc["system"])
            if cs:
                codes.update(_collect_cs_codes(cs.get("concept", [])))

    # Also check expansion (some ValueSets only have expansion, not compose)
    for contains in vs.get("expansion", {}).get("contains", []):
        c = contains.get("code", "")
        if c:
            codes.add(c)

    return name, codes


def generate_code_systems(
    target_types: list[str],
    resources: list[str],
    versions: list[str],
    input_dir: str = "fhir_specs",
    output_dir: str = "generated_src",
) -> dict[str, dict[str, str]]:
    """Scan StructureDefinitions for code fields with required bindings,
    generate FF_CodeSystems.hpp, and return a mapping for ffc.py.

    Returns:
        code_enum_map: dict  {fhir_path -> {enum, parse, serialize}}
            e.g. {"Narrative.status": {"enum": "NarrativeStatus", ...}}
    """
    # ------------------------------------------------------------------
    # Load bundles
    # ------------------------------------------------------------------
    type_bundles: dict[str, dict] = {}
    resource_bundles: dict[str, dict] = {}
    vs_indices: dict[str, tuple[dict, dict]] = {}

    for v in versions:
        tp = os.path.join(input_dir, v, "profiles-types.json")
        rp = os.path.join(input_dir, v, "profiles-resources.json")
        vp = os.path.join(input_dir, v, "valuesets.json")
        if os.path.exists(tp):
            with open(tp, encoding="utf-8") as f:
                type_bundles[v] = json.load(f)
        if os.path.exists(rp):
            with open(rp, encoding="utf-8") as f:
                resource_bundles[v] = json.load(f)
        if os.path.exists(vp):
            with open(vp, encoding="utf-8") as f:
                vs_indices[v] = _build_bundle_index(json.load(f))

    # ------------------------------------------------------------------
    # Scan StructureDefinitions for code + required binding
    # ------------------------------------------------------------------
    vs_registry: dict[str, dict] = {}
    path_to_vs: dict[str, str] = {}

    def _scan_elements(elements, root_name):
        for el in elements:
            path = el.get("path", "")
            if not path.startswith(root_name + "."):
                continue
            for td in el.get("type", []):
                code = td.get("code", "")
                if code != "code":
                    continue
                binding = el.get("binding", {})
                if binding.get("strength") != "required":
                    continue
                vs_url = binding.get("valueSet", "")
                if not vs_url:
                    continue
                # Strip FHIR version suffix (e.g. "|5.0.0") so the
                # URL matches the valueset index keys stored without version.
                vs_url = vs_url.split("|")[0]
                path_to_vs[path] = vs_url
                if vs_url not in vs_registry:
                    vs_registry[vs_url] = {}

    for v_name, bundle in type_bundles.items():
        for entry in bundle.get("entry", []):
            res = entry.get("resource", {})
            if res.get("resourceType") != "StructureDefinition":
                continue
            kind = res.get("kind")
            if kind == "complex-type" or kind == "primitive-type":
                name = res.get("name", "")
                if name:
                    _scan_elements(res.get("snapshot", {}).get("element", []), name)

    for v_name, bundle in resource_bundles.items():
        for entry in bundle.get("entry", []):
            res = entry.get("resource", {})
            if res.get("resourceType") != "StructureDefinition":
                continue
            kind = res.get("kind")
            if kind != "resource":
                continue
            name = res.get("name", "")
            if name and name in resources:
                _scan_elements(res.get("snapshot", {}).get("element", []), name)

    # ------------------------------------------------------------------
    # Resolve valueset codes & build enums
    # ------------------------------------------------------------------
    code_enum_map: dict[str, dict[str, str]] = {}
    enum_defs: dict[str, tuple[str | None, set[str]]] = {}

    for vs_url in sorted(vs_registry):
        for v_name, (vss, css) in vs_indices.items():
            name, codes = _get_valueset_codes(vs_url, vss, css)
            if name and codes:
                if vs_url not in enum_defs or len(codes) > len(enum_defs.get(vs_url, (None, set()))[1]):
                    enum_defs[vs_url] = (name, codes)

    # Build enum names from value set names and assign to paths.
    for vs_url, (name, codes) in enum_defs.items():
        if not codes:
            continue
        clean = _to_pascal_case(name) if name else _to_pascal_case(vs_url.split("/")[-1])
        enum_name = f"FF_{clean}"
        # Build parse / serialize helpers.
        parse_name = f"parse_{clean}"
        serialize_name = f"serialize_{clean}"
        for path, url in path_to_vs.items():
            if url == vs_url:
                code_enum_map[path] = {
                    "enum": enum_name,
                    "parse": parse_name,
                    "serialize": serialize_name,
                }

    # ------------------------------------------------------------------
    # Emit FF_CodeSystems.hpp
    # ------------------------------------------------------------------
    hpp = auto_header
    hpp += "#pragma once\n"
    hpp += "#include <string_view>\n"
    hpp += "#include <cstdint>\n\n"
    hpp += "namespace FastFHIR {\n\n"

    emitted: set[str] = set()
    for vs_url, (name, codes) in sorted(enum_defs.items(), key=lambda x: x[0]):
        if not codes:
            continue
        clean = _to_pascal_case(name) if name else _to_pascal_case(vs_url.split("/")[-1])
        enum_name = f"FF_{clean}"
        if enum_name in emitted:
            continue
        emitted.add(enum_name)

        parse_name = f"parse_{clean}"
        serialize_name = f"serialize_{clean}"

        hpp += f"// --- {name or vs_url} ---\n"
        hpp += f"enum class {enum_name} : uint8_t {{\n"
        for code in sorted(codes):
            ident = _code_to_identifier(code)
            hpp += f"    {ident},\n"
        hpp += "};\n\n"

        # Serialize function.
        hpp += f"inline const char* {serialize_name}({enum_name} e) {{\n"
        hpp += f"    switch (e) {{\n"
        for code in sorted(codes):
            ident = _code_to_identifier(code)
            escaped = code.replace("\\", "\\\\").replace('"', '\\"')
            hpp += f'        case {enum_name}::{ident}: return "{escaped}";\n'
        hpp += '        default: return "";\n'
        hpp += "    }\n"
        hpp += "}\n\n"

        # Parse function.
        hpp += f"inline {enum_name} {parse_name}(std::string_view sv) {{\n"
        for code in sorted(codes):
            ident = _code_to_identifier(code)
            escaped = code.replace("\\", "\\\\").replace('"', '\\"')
            hpp += f'    if (sv == "{escaped}") return {enum_name}::{ident};\n'
        hpp += f'    return static_cast<{enum_name}>(0);\n'
        hpp += "}\n\n"

    hpp += "} // namespace FastFHIR\n"
    write_if_changed(os.path.join(output_dir, "FF_CodeSystems.hpp"), hpp)
    print(f"-- Emitted {output_dir}/FF_CodeSystems.hpp ({len(emitted)} enums)")
    return code_enum_map
