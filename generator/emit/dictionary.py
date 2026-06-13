# =====================================================================
# FastFHIR Dictionary Generator
#
# Scans NPM FHIR packages (CodeSystem-*.json + ValueSet-ucum-*.json)
# to extract FHIR-native codes with compound keys ("system|code").
# Generates master_codes.json for downstream C++ emission.
#
# Value assignment is PERMANENT:
#   1. Codes assigned sequential integer IDs.
#   2. Compound keys prevent cross-system collisions.
#   3. Once committed, a value never changes.
# =============================================================

import json, os

FHIR_NATIVE_PREFIXES = (
    "http://hl7.org/fhir/",
    "http://terminology.hl7.org/CodeSystem/",
)

_FF_CODE_DICTIONARY_MAX = 0x7FFFFFFF

_EXPLICIT_BLOCK = {
    "color-names", "spdx-license", "fhir-types", "resource-types",
    "data-types", "fhirpath-types",
}


def _all_numeric(codes):
    return all(c.isdigit() for c in codes)


def _short_name(url):
    for pre in (
        "http://hl7.org/fhir/CodeSystem/",
        "http://hl7.org/fhir/ValueSet/",
        "http://hl7.org/fhir/",
        "http://terminology.hl7.org/CodeSystem/",
        "http://terminology.hl7.org/ValueSet/",
        "http://terminology.hl7.org/",
    ):
        if url.startswith(pre):
            return url[len(pre):]
    return url


def _collect_code_systems(pkg_dir):
    systems = {}
    if not os.path.exists(pkg_dir): return systems
    for fname in os.listdir(pkg_dir):
        if not fname.startswith("CodeSystem-") or not fname.endswith(".json"): continue
        with open(os.path.join(pkg_dir, fname), encoding="utf-8") as f: cs = json.load(f)
        url = cs.get("url", ""); cs_id = cs.get("id", "")
        if not any(url.startswith(p) for p in FHIR_NATIVE_PREFIXES): continue
        if cs_id.startswith("v2-") or cs_id.startswith("v3-"): continue
        if cs.get("content") != "complete": continue
        name = _short_name(url)
        if any(b in name for b in _EXPLICIT_BLOCK): continue
        entries = []
        def walk(concepts):
            for c in concepts:
                code = c.get("code", "").strip()
                if code:
                    entries.append({"code": code, "descriptor": c.get("display", "").strip() or code})
                if "concept" in c: walk(c["concept"])
        walk(cs.get("concept", []))
        if entries and not _all_numeric([e["code"] for e in entries]):
            systems[name] = {"source_url": url, "entries": entries}
    return systems


def _collect_ucum_value_sets(pkg_dir):
    systems = {}
    if not os.path.exists(pkg_dir): return systems
    for fname in os.listdir(pkg_dir):
        if not (fname.startswith("ValueSet-ucum-") and fname.endswith(".json")): continue
        with open(os.path.join(pkg_dir, fname), encoding="utf-8") as f: vs = json.load(f)
        entries = []
        for inc in vs.get("compose", {}).get("include", []):
            for c in inc.get("concept", []):
                code = c.get("code", "").strip()
                if code:
                    entries.append({"code": code, "descriptor": c.get("display", "").strip() or code})
        if entries:
            name = _short_name(vs.get("url", ""))
            systems[name] = {"source_url": vs.get("url", ""), "entries": entries}
    return systems


def _collect_urls(pkg_dir):
    urls = set()
    for collector in (_collect_code_systems, _collect_ucum_value_sets):
        for sys_data in collector(pkg_dir).values():
            urls.add(sys_data["source_url"])
    return urls


def generate_master_codes(package_dirs):
    """Generate master code data from NPM FHIR packages.

    Args:
        package_dirs: {"R4": "path/to/R4/package", "R5": "path/to/R5/package"}
    Returns:
        (systems, ids_by_version, all_urls)
    """
    merged = {}
    token_versions = {}
    all_urls = set()

    for vname, pkg_dir in package_dirs.items():
        css = _collect_code_systems(pkg_dir)
        ucs = _collect_ucum_value_sets(pkg_dir)
        total = sum(len(v["entries"]) for v in css.values()) + sum(len(v["entries"]) for v in ucs.values())
        print(f"  {vname}: {len(css)} CodeSystems, {len(ucs)} UCUM, {total} codes")
        for sys_data in css.values(): all_urls.add(sys_data["source_url"])

        for source_set, is_ucum in [(css, False), (ucs, True)]:
            for sys_name, sys_data in source_set.items():
                if sys_name not in merged:
                    merged[sys_name] = {"source_url": sys_data["source_url"], "entries": []}
                existing = {e["code"] for e in merged[sys_name]["entries"]}
                for entry in sys_data["entries"]:
                    if entry["code"] not in existing:
                        merged[sys_name]["entries"].append(entry)
                    token = f"{sys_name}|{entry['code']}"
                    token_versions.setdefault(token, set()).add("UCUM" if is_ucum else vname)

    compound_ids = {}
    next_id = 1
    for sys_name in sorted(merged):
        merged[sys_name]["entries"].sort(key=lambda x: x["code"])
        for entry in merged[sys_name]["entries"]:
            compound_ids[f"{sys_name}|{entry['code']}"] = next_id
            next_id += 1

    if next_id - 1 >= _FF_CODE_DICTIONARY_MAX:
        raise RuntimeError(f"Dictionary overflow: {next_id - 1} >= {_FF_CODE_DICTIONARY_MAX}")

    ids_by_version = {"R4": {}, "R5": {}, "UCUM": {}}
    for token, tid in compound_ids.items():
        for v in token_versions.get(token, set()):
            ids_by_version[v][token] = tid

    total = sum(len(v["entries"]) for v in merged.values())
    print(f"  Master: {len(merged)} systems, {total} entries, {len(compound_ids)} IDs")
    print(f"    R4: {len(ids_by_version['R4'])}  R5: {len(ids_by_version['R5'])}  UCUM: {len(ids_by_version['UCUM'])}")

    return merged, ids_by_version, all_urls
