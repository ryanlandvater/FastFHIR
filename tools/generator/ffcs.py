# =====================================================================
# FastFHIR Code Systems Generator (ffcs.py)
# 
# Scans FHIR StructureDefinitions for 'code' fields with required
# bindings, cross-references ValueSet bundles to extract allowed values,
# and generates FF_CodeSystems.hpp with enum classes + parse/serialize
# helpers for each bounded code system.
#
# Author: Ryan Landvater (ryanlandvater[at]gmail[dot]com)
# Copyright (c) 2025 Ryan Landvater. All rights reserved.
# License: FastFHIR Shared Source License (FF-SSL)
# =============================================================

import os
import json
import re

EXCLUDED_VALUESET_FRAGMENTS = {
    'all-languages', 'mimetypes', 'ucum-units', 'bcp:47', 'languages',
}

SYMBOL_CODES = {
    '<':  'LessThan',
    '<=': 'LessOrEqual',
    '>=': 'GreaterOrEqual',
    '>':  'GreaterThan',
}


def _to_pascal_case(s):
    """Convert a kebab-case / dot-separated string to PascalCase."""
    parts = re.split(r'[^a-zA-Z0-9]+', s)
    return ''.join(p[0].upper() + p[1:] for p in parts if p)


def _code_to_identifier(code):
    """Convert a FHIR code value to a valid C++ enum identifier."""
    if code in SYMBOL_CODES:
        return SYMBOL_CODES[code]
    parts = re.split(r'[^a-zA-Z0-9]+', code)
    parts = [p for p in parts if p]
    if not parts:
        return 'Empty'
    return ''.join(p[0].upper() + p[1:] for p in parts)


def _collect_cs_codes(concepts):
    """Recursively collect codes from a CodeSystem concept hierarchy."""
    codes = set()
    for c in concepts:
        val = c.get('code', '')
        if val:
            codes.add(val)
        codes.update(_collect_cs_codes(c.get('concept', [])))
    return codes


def _build_bundle_index(bundle_json):
    """Index ValueSet and CodeSystem resources from a FHIR bundle."""
    valuesets = {}
    codesystems = {}
    for entry in bundle_json.get('entry', []):
        res = entry.get('resource', {})
        url = res.get('url', '')
        rt = res.get('resourceType')
        if rt == 'ValueSet' and url:
            valuesets[url] = res
        elif rt == 'CodeSystem' and url:
            codesystems[url] = res
    return valuesets, codesystems


def _get_valueset_codes(vs_url, valuesets, codesystems):
    """Extract codes and display name for a ValueSet URL."""
    vs = valuesets.get(vs_url)
    if not vs:
        return None, set()

    name = vs.get('name', '')
    codes = set()

    for inc in vs.get('compose', {}).get('include', []):
        for concept in inc.get('concept', []):
            c = concept.get('code', '')
            if c:
                codes.add(c)
        # If no explicit concepts, pull from the referenced CodeSystem
        if not inc.get('concept') and inc.get('system'):
            cs = codesystems.get(inc['system'])
            if cs:
                codes.update(_collect_cs_codes(cs.get('concept', [])))

    # Also check expansion (some ValueSets only have expansion, not compose)
    for contains in vs.get('expansion', {}).get('contains', []):
        c = contains.get('code', '')
        if c:
            codes.add(c)

    return name, codes


def generate_code_systems(target_types, resources, versions,
                          input_dir="fhir_specs", output_dir="generated_src"):
    """
    Scan StructureDefinitions for code fields with required bindings,
    generate FF_CodeSystems.hpp, and return a mapping for ffc.py.

    Returns:
        code_enum_map: dict  {fhir_path -> {enum, parse, serialize}}
            e.g. {"Narrative.status": {"enum": "NarrativeStatus", ...}}
    """
    # ------------------------------------------------------------------
    # Load bundles
    # ------------------------------------------------------------------
    type_bundles = {}
    resource_bundles = {}
    vs_indices = {}  # version -> (valuesets_dict, codesystems_dict)

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
    # valueset_url -> {name, codes, paths}
    vs_registry = {}
    # fhir_path -> valueset_url
    path_to_vs = {}

    def _scan_elements(elements, root_name):
        for el in elements:
            path = el.get('path', '')
            if not path.startswith(root_name + '.'):
                continue
            types = el.get('type', [])
            if not types or types[0].get('code') != 'code':
                continue
            binding = el.get('binding', {})
            if binding.get('strength') != 'required':
                continue
            vs_url = (binding.get('valueSet', '')
                      or binding.get('valueSetUri', '')
                      or binding.get('valueSetReference', {}).get('reference', ''))
            if not vs_url:
                continue
            # Strip version suffix  (e.g. "…|4.0.1")
            if '|' in vs_url:
                vs_url = vs_url.split('|')[0]
            # Skip unbounded / external code systems
            if any(frag in vs_url for frag in EXCLUDED_VALUESET_FRAGMENTS):
                continue

            path_to_vs[path] = vs_url
            if vs_url not in vs_registry:
                vs_registry[vs_url] = {'name': '', 'codes': set(), 'paths': set()}
            vs_registry[vs_url]['paths'].add(path)

    def _find_sd(bundle, name):
        for entry in bundle.get('entry', []):
            res = entry.get('resource', {})
            if (res.get('resourceType') == 'StructureDefinition'
                    and res.get('name') == name):
                return res.get('snapshot', {}).get('element', [])
        return None

    for t in target_types:
        for v, bundle in type_bundles.items():
            elements = _find_sd(bundle, t)
            if elements:
                _scan_elements(elements, t)

    for r in resources:
        for v, bundle in resource_bundles.items():
            elements = _find_sd(bundle, r)
            if elements:
                _scan_elements(elements, r)

    # ------------------------------------------------------------------
    # Collect codes from all versions for each ValueSet
    # ------------------------------------------------------------------
    for vs_url, info in vs_registry.items():
        for v, (valuesets, codesystems) in vs_indices.items():
            name, codes = _get_valueset_codes(vs_url, valuesets, codesystems)
            if name and not info['name']:
                info['name'] = name
            info['codes'].update(codes)

    # Drop valuesets that yielded no codes
    vs_registry = {url: info for url, info in vs_registry.items() if info['codes']}

    # ------------------------------------------------------------------
    # Assign enum names (prefer ValueSet.name, else derive from URL)
    # ------------------------------------------------------------------
    used_names = set()
    for vs_url, info in vs_registry.items():
        if info['name']:
            # ValueSet.name is already PascalCase by FHIR convention
            raw = re.sub(r'[^A-Za-z0-9]', '', info['name'])
        else:
            raw = _to_pascal_case(vs_url.rstrip('/').split('/')[-1])
        # Deduplicate
        enum_name = raw
        suffix = 2
        while enum_name in used_names:
            enum_name = f"{raw}{suffix}"
            suffix += 1
        used_names.add(enum_name)
        info['enum_name'] = enum_name

    # ------------------------------------------------------------------
    # Build fhir_path → enum info mapping for ffc.py
    # ------------------------------------------------------------------
    code_enum_map = {}
    for path, vs_url in path_to_vs.items():
        if vs_url in vs_registry:
            info = vs_registry[vs_url]
            enum_name = info['enum_name']
            code_enum_map[path] = {
                'enum':      enum_name,
                'parse':     f'FF_Parse{enum_name}',
                'serialize': f'FF_{enum_name}ToString',
            }

    # ------------------------------------------------------------------
    # Generate FF_CodeSystems.hpp
    # ------------------------------------------------------------------
    auto_header = (
        "// ============================================================\n"
        "// This file is autogenerated by FastFHIR. DO NOT EDIT.\n"
        "// Copyright (c) Ryan Landvater. All rights reserved.\n"
        "// ============================================================\n"
    )
    hpp = f"{auto_header}#pragma once\n#include <cstdint>\n#include <string>\n\n"

    for vs_url, info in sorted(vs_registry.items(), key=lambda x: x[1]['enum_name']):
        enum_name = info['enum_name']
        codes = sorted(info['codes'])

        # Build identifier list (dedup)
        entries = []
        seen_ids = set()
        for code in codes:
            ident = _code_to_identifier(code)
            orig = ident
            n = 2
            while ident in seen_ids:
                ident = f"{orig}{n}"
                n += 1
            seen_ids.add(ident)
            entries.append((code, ident))

        # Emit comment showing source ValueSet
        hpp += f"// ValueSet: {vs_url}\n"

        # Check if any code already maps to 'Unknown' (e.g., FHIR "unknown")
        has_unknown = 'Unknown' in seen_ids

        # Enum class
        hpp += f"enum class {enum_name} : uint8_t {{\n"
        for code, ident in entries:
            hpp += f"    {ident},\n"
        if not has_unknown:
            hpp += f"    Unknown\n"
        hpp += f"}};\n\n"

        # Parse function (string → enum)
        hpp += f"inline {enum_name} FF_Parse{enum_name}(const std::string& s) {{\n"
        for code, ident in entries:
            hpp += f"    if (s == \"{code}\") return {enum_name}::{ident};\n"
        hpp += f"    return {enum_name}::Unknown;\n}}\n\n"

        # Serialize function (enum → string)
        hpp += f"inline const char* FF_{enum_name}ToString({enum_name} v) {{\n"
        hpp += f"    switch (v) {{\n"
        for code, ident in entries:
            hpp += f"        case {enum_name}::{ident}: return \"{code}\";\n"
        hpp += f"        default: return \"\";\n"
        hpp += f"    }}\n}}\n\n"

    os.makedirs(output_dir, exist_ok=True)
    with open(os.path.join(output_dir, "FF_CodeSystems.hpp"), "w") as ff:
        ff.write(hpp)

    print(f"[Success] FF_CodeSystems.hpp: {len(vs_registry)} code system enums")
    return code_enum_map
