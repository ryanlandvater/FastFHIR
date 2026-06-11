# =====================================================================
# FastFHIR Dictionary Generator
#
# Scans FHIR ValueSet bundles to extract allowed values
# and generates FF_{version}_Dictionary.cpp with permanent, auditable
# code-value arrays.  The generated .cpp files define FF_CodeEntry[]
# tables that are linked against the hand-maintained FF_Dictionary.hpp
# interface.
#
# Value assignment is PERMANENT:
#   1. All unique code strings across ALL FHIR versions are collected.
#   2. Sorted alphabetically and assigned sequential hex values
#      starting from 0x00000001.
#   3. Each per-version .cpp file includes only the codes present in
#      that version's ValueSets, using the same permanent value for
#      each string.
#   4. Once committed, a value never changes.  New FHIR versions get
#      new values appended at the end of the sequence.
#
# Author: Ryan Landvater (ryanlandvater[at]gmail[dot]com)
# Copyright (c) 2025 Ryan Landvater. All rights reserved.
# License: FastFHIR Shared Source License (FF-SSL)
# =============================================================

import json
import os
import re
import subprocess

from generator.emit.header import auto_header, write_if_changed

_FF_CODE_NULL = 0xFFFFFFFF


def _version_sort_key(label: str) -> tuple:
    m = re.match(r"^R(\d+)([A-Za-z]*)$", label)
    if not m:
        return (10**9, label)
    major = int(m.group(1))
    minor = m.group(2).upper()
    return (major, minor)


def _collect_codes(bundle) -> set[str]:
    """Collect all unique code strings from a FHIR ValueSet bundle."""
    codes: set[str] = set()
    for entry in bundle.get("entry", []):
        res = entry.get("resource", {})
        if res.get("resourceType") == "ValueSet":
            for inc in res.get("compose", {}).get("include", []):
                # Collect system URIs
                system = inc.get("system", "")
                if system:
                    codes.add(system)
                # Collect concept codes
                for concept in inc.get("concept", []):
                    code = concept.get("code", "")
                    if code:
                        codes.add(code)
            # Also check expansion
            for contains in res.get("expansion", {}).get("contains", []):
                c = contains.get("code", "")
                if c:
                    codes.add(c)
        elif res.get("resourceType") == "CodeSystem":
            def walk(concepts):
                for c in concepts:
                    code = c.get("code", "")
                    if code:
                        codes.add(code)
                    walk(c.get("concept", []))
            walk(res.get("concept", []))
    return codes


def _load_codes_for_version(version_dir: str) -> set[str]:
    """Load all codes from a version's valuesets.json."""
    bundle_path = os.path.join(version_dir, "valuesets.json")
    if not os.path.exists(bundle_path):
        return set()
    with open(bundle_path, "r", encoding="utf-8") as f:
        bundle = json.load(f)
    return _collect_codes(bundle)


def _verify_no_drift(
    v_name: str,
    new_entries: list[tuple[str, int]],
    output_dir: str = "generated_src",
) -> None:
    """Verify that every committed code→value pair still has the same value.

    Reads the committed version via git show HEAD.  For every entry that
    exists in BOTH the committed file and the new output, the value must
    match exactly.  If any existing value changed, this raises a hard error
    — the generator will NOT write a file with drifted values.
    
    Only wholly new codes (absent from the committed file) may be added.
    """
    version_path = f"src/FF_{v_name}_Dictionary.cpp"
    try:
        r = subprocess.run(
            ["git", "show", f"HEAD:{version_path}"],
            capture_output=True, text=True, timeout=5,
        )
    except Exception as exc:
        raise RuntimeError(
            f"Cannot verify drift for {version_path}: git failed ({exc}). "
            f"Dictionary files must be committed before regeneration."
        )
    if r.returncode != 0:
        raise RuntimeError(
            f"Committed dictionary not found: {version_path} is not in git. "
            f"Run `git add src/FF_*_Dictionary.cpp && git commit` first."
        )
    
    # Extract committed (code_str → value) mappings
    committed: dict[str, int] = {}
    for val_hex, code_str in re.findall(r'{ (0x[0-9A-F]+), "([^"]+)" }', r.stdout):
        committed[code_str] = int(val_hex, 16)
    
    # Build new (code_str → value) mappings
    new_map: dict[str, int] = dict(new_entries)
    
    # Check every code that exists in both: value MUST be identical
    drifted = []
    for code_str, old_val in committed.items():
        new_val = new_map.get(code_str)
        if new_val is not None and new_val != old_val:
            drifted.append((code_str, old_val, new_val))
    
    if drifted:
        lines_list = ["DRIFT DETECTED in %s: %d existing mapping(s) changed. Aborting write." % (version_path, len(drifted))]
        for code_str, old_val, new_val in drifted[:10]:
            lines_list.append("  %r: 0x%08X -> 0x%08X" % (code_str, old_val, new_val))
        if len(drifted) > 10:
            lines_list.append("  ... and %d more" % (len(drifted) - 10))
        raise RuntimeError("\n".join(lines_list))


def generate_fastfhir_dictionary(
    v_name: str,
    codes_this_version: set[str],
    master_code_list: list[tuple[str, int]],
    output_dir: str = "generated_src",
) -> bool:
    """
    Generate a single FF_{v_name}_Dictionary.cpp file.

    Args:
        v_name: Version label (e.g. "R4", "R5").
        codes_this_version: Set of code strings present in this version.
        master_code_list: Sorted list of (code_string, permanent_value) pairs.
        output_dir: Output directory.

    Returns: True if generated successfully.
    """
    # Filter master list to only codes present in this version
    entries: list[tuple[str, int]] = [
        (code, value) for code, value in master_code_list
        if code in codes_this_version
    ]

    if not entries:
        print(f"[Warning] No codes for {v_name} — skipping")
        return False

    # Sort by value (which is by code alphabetically)
    entries.sort(key=lambda x: x[1])
    # Verify no FF_CODE_NULL collision
    for code, value in entries:
        if value == _FF_CODE_NULL:
            raise RuntimeError(
                f"Code {code!r} would be assigned FF_CODE_NULL (0xFFFFFFFF). "
                "This is reserved. Remap the code."
            )

    cpp = auto_header
    cpp += f'// Generated from {v_name} ValueSets\n'
    cpp += f'#include "../include/FF_Dictionary.hpp"\n\n'
    cpp += f'static const FF_CodeEntry FF_{v_name}_TABLE[] = {{\n'
    for code_str, value in entries:
        escaped = code_str.replace("\\", "\\\\").replace('"', '\\"')
        cpp += f'    {{ 0x{value:08X}, "{escaped}" }},\n'
    cpp += '};\n\n'
    cpp += f'const FF_CodeEntry* const FF_{v_name}_DICTIONARY = FF_{v_name}_TABLE;\n'
    cpp += f'const size_t FF_{v_name}_DICTIONARY_SIZE = sizeof(FF_{v_name}_TABLE) / sizeof(FF_{v_name}_TABLE[0]);\n'

    # ── Guard: verify no committed code-value mapping changed ──
    _verify_no_drift(v_name, entries)

    write_if_changed(os.path.join("src", f"FF_{v_name}_Dictionary.cpp"), cpp)
    print(f"  -> {len(entries)} entries in FF_{v_name}_Dictionary.cpp")
    return True


def _normalize_version_configs(versions, default_input_dir: str) -> list[tuple[str, str]]:
    normalized: list[tuple[str, str]] = []
    for item in versions:
        if isinstance(item, str):
            normalized.append((item, os.path.join(default_input_dir, item)))
        elif isinstance(item, (tuple, list)) and len(item) == 2:
            v_name, version_dir = item
            if isinstance(v_name, str) and isinstance(version_dir, str):
                normalized.append((v_name, version_dir))
            else:
                print(f"[Warning] Skipping invalid version config: {item}")
        else:
            print(f"[Warning] Skipping invalid version config: {item}")
    return normalized


def generate_master_dictionary(
    versions, input_dir: str = "fhir_specs", output_dir: str = "generated_src"
) -> set[str]:
    os.makedirs(output_dir, exist_ok=True)
    normalized_versions = _normalize_version_configs(versions, input_dir)
    normalized_versions = sorted(normalized_versions, key=lambda x: _version_sort_key(x[0]))

    # ── Phase 1: Collect ALL unique codes across ALL versions ──────
    version_codes: dict[str, set[str]] = {}
    all_codes: set[str] = set()
    for v_name, version_dir in normalized_versions:
        codes = _load_codes_for_version(version_dir)
        version_codes[v_name] = codes
        all_codes.update(codes)
        print(f"  {v_name}: {len(codes)} raw codes")

    # ── Phase 2: Load existing permanent values via git (committed) ──
    # Use `git show HEAD:{path}` to get the COMMITTED version of each
    # dictionary file, never the working copy.  This guarantees that once
    # a code→value mapping is committed, it is set in stone.  The only
    # thing the generator may do is append new entries for codes that
    # aren't in any committed file yet.
    #
    # If git is unavailable or the file isn't committed yet (first run),
    # fall back to reading the working copy — safe because the files are
    # freshly generated in that case.
    existing_values: dict[str, int] = {}
    for v_name, _ in normalized_versions:
        version_path = f"src/FF_{v_name}_Dictionary.cpp"
        existing_cpp = None
        try:
            r = subprocess.run(
                ["git", "show", f"HEAD:{version_path}"],
                capture_output=True, text=True, timeout=5,
            )
            if r.returncode == 0:
                existing_cpp = r.stdout
        except Exception:
            pass
        
        if existing_cpp is None:
            raise RuntimeError(
                f"Committed dictionary not found: {version_path} is not in git. "
                "The dictionary files must be committed before regeneration. "
                "Run `git add src/FF_*_Dictionary.cpp && git commit` first."
            )
        
        if existing_cpp:
            pairs = re.findall(r'{ (0x[0-9A-F]+), "([^"]+)" }', existing_cpp)
            for val_hex, code_str in pairs:
                if code_str not in existing_values:
                    existing_values[code_str] = int(val_hex, 16)

    # Separate into existing and new codes
    existing_codes = {c for c in all_codes if c in existing_values}
    new_codes = sorted(all_codes - existing_codes)

    # Determine next available value
    max_existing = max(existing_values.values()) if existing_values else 0
    master_code_list = [
        (code, existing_values[code]) for code in sorted(existing_codes)
    ]
    # New codes get sequential values after max_existing, sorted alphabetically
    for idx, code in enumerate(new_codes):
        master_code_list.append((code, max_existing + 1 + idx))

    print(f"  Master dictionary: {len(master_code_list)} unique codes "
          f"({len(existing_codes)} preserved, {len(new_codes)} new)")

    # ── Phase 3: Generate per-version .cpp files ──────────────────
    active_versions: list[str] = []
    for v_name, _ in normalized_versions:
        if v_name in version_codes and version_codes[v_name]:
            if generate_fastfhir_dictionary(v_name, version_codes[v_name], master_code_list, output_dir):
                active_versions.append(v_name)

    # ── Phase 4: No master header generated ───────────────────────
    # FF_Dictionary.hpp is hand-maintained in include/.
    # FF_Dictionary.cpp is hand-maintained in src/.
    # ── Phase 5: Collect code system URLs for known-URL filter ────
    code_system_urls = {code for code in all_codes if code.startswith('http')}
    print(f"  Code system URLs: {len(code_system_urls)}")

    print(f"[Success] Generated dictionaries for {len(active_versions)} versions.")
    if active_versions:
        print(f"  Versions: {', '.join(active_versions)}")
        print(f"  Total unique codes: {len(master_code_list)}")
    
    return code_system_urls
