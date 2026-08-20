# =====================================================================
# FastFHIR Dictionary Generator -- the permanent code-ID ledger.
#
# Scans NPM FHIR packages (CodeSystem-*.json + ValueSet-ucum-*.json) for
# FHIR-native codes, then reconciles them against dictionaries/master_codes.json.
#
# THE ONE RULE
# ------------
# A code's ID is a WIRE CONSTANT. It is written into .ffhr streams and is the
# only thing that decodes them. Therefore:
#
#     * an existing code KEEPS its ID, forever;
#     * a new code is APPENDED at _next_id;
#     * a code that HL7 removes KEEPS its ID (old streams still cite it) --
#       it is dropped from the version lists, never from the ledger;
#     * a retired ID is NEVER reused.
#
# Renumbering invalidates every archive ever written. This module must never
# assign an ID by counting from zero. See dictionaries/README.md.
# =============================================================

import json
import os

from generator.emit.header import write_if_changed

# The committed ledger. This file is the source of truth for every code ID;
# the generator only ever appends to it.
_LEDGER_PATH = os.path.join(
    os.path.dirname(__file__), "..", "..", "dictionaries", "master_codes.json"
)

# =====================================================================
# WHAT FASTFHIR IS ALLOWED TO REDISTRIBUTE
#
# FastFHIR's contribution is the NUMBERING -- the permanent code-string ->
# uint32 assignment -- not the terminology. Code values from HL7 FHIR and UCUM
# ship in dictionaries/. Everything else (SNOMED CT, LOINC, RxNorm, ICD, CPT,
# NDC, EDQM, ...) is licensed by its owner and is NOT shipped: those codes
# travel as FF_CODEABLE_CONCEPT blocks carrying the literal code the *user*
# supplied, under the user's own license.
#
# This is enforced, not merely intended -- see _assert_redistributable().
# It is enforced because a convention already failed once: an older scrape
# expanded value sets that referenced external terminologies and pulled 13,310
# LOINC/SNOMED/EDQM codes into the committed dictionary with nothing objecting.
# =====================================================================
REDISTRIBUTABLE_PREFIXES = (
    "http://hl7.org/fhir/",
    "http://terminology.hl7.org/",
    "http://unitsofmeasure.org",
)

# Kept as the historical name used by the collectors below.
FHIR_NATIVE_PREFIXES = (
    "http://hl7.org/fhir/",
    "http://terminology.hl7.org/CodeSystem/",
)

# Bit 31 (0x80000000) is FF_CODEABLE_CONCEPT_FLAG -- never assign an ID with it set.
_FF_CODE_DICTIONARY_MAX = 0x7FFFFFFF

_EXPLICIT_BLOCK = {
    "color-names",
    "spdx-license",
    "fhir-types",
    "resource-types",
    "data-types",
    "fhirpath-types",
}


def _all_numeric(codes: list[str]) -> bool:
    return all(c.isdigit() for c in codes)


def _short_name(url: str) -> str:
    for pre in (
        "http://hl7.org/fhir/CodeSystem/",
        "http://hl7.org/fhir/ValueSet/",
        "http://hl7.org/fhir/",
        "http://terminology.hl7.org/CodeSystem/",
        "http://terminology.hl7.org/ValueSet/",
        "http://terminology.hl7.org/",
    ):
        if url.startswith(pre):
            return url[len(pre) :]
    return url


def _collect_code_systems(pkg_dir: str) -> dict:
    systems = {}
    if not os.path.exists(pkg_dir):
        return systems
    for fname in os.listdir(pkg_dir):
        if not fname.startswith("CodeSystem-") or not fname.endswith(".json"):
            continue
        with open(os.path.join(pkg_dir, fname), encoding="utf-8") as f:
            cs = json.load(f)
        url = cs.get("url", "")
        cs_id = cs.get("id", "")
        if not any(url.startswith(p) for p in FHIR_NATIVE_PREFIXES):
            continue
        if cs_id.startswith(("v2-", "v3-")):
            continue
        if cs.get("content") != "complete":
            continue
        name = _short_name(url)
        if any(b in name for b in _EXPLICIT_BLOCK):
            continue
        entries = []

        def walk(concepts: list[dict], entries: list = entries) -> None:
            for c in concepts:
                code = c.get("code", "").strip()
                if code:
                    entries.append(
                        {"code": code, "descriptor": c.get("display", "").strip() or code}
                    )
                if "concept" in c:
                    walk(c["concept"])

        walk(cs.get("concept", []))
        if entries and not _all_numeric([e["code"] for e in entries]):
            systems[name] = {"source_url": url, "entries": entries}
    return systems


def _collect_ucum_value_sets(pkg_dir: str) -> dict:
    systems = {}
    if not os.path.exists(pkg_dir):
        return systems
    for fname in os.listdir(pkg_dir):
        if not (fname.startswith("ValueSet-ucum-") and fname.endswith(".json")):
            continue
        with open(os.path.join(pkg_dir, fname), encoding="utf-8") as f:
            vs = json.load(f)
        entries = []
        for inc in vs.get("compose", {}).get("include", []):
            for c in inc.get("concept", []):
                code = c.get("code", "").strip()
                if code:
                    entries.append(
                        {"code": code, "descriptor": c.get("display", "").strip() or code}
                    )
        if entries:
            name = _short_name(vs.get("url", ""))
            systems[name] = {"source_url": vs.get("url", ""), "entries": entries}
    return systems


def _collect_urls(pkg_dir: str) -> set[str]:
    urls = set()
    for collector in (_collect_code_systems, _collect_ucum_value_sets):
        for sys_data in collector(pkg_dir).values():
            urls.add(sys_data["source_url"])
    return urls


def _assert_redistributable(source_urls: dict[str, str]) -> None:
    """Refuse to put codes we do not own into the shipped dictionary.

    `source_urls` maps a system's short name to the CodeSystem/ValueSet URL it
    came from. Anything outside REDISTRIBUTABLE_PREFIXES stops the build rather
    than silently entering dictionaries/.
    """
    foreign = {
        name: url
        for name, url in source_urls.items()
        if not any(url.startswith(p) for p in REDISTRIBUTABLE_PREFIXES)
    }
    if foreign:
        listed = "\n".join(f"    {n}  <-  {u}" for n, u in sorted(foreign.items())[:10])
        more = f"\n    ... and {len(foreign) - 10} more" if len(foreign) > 10 else ""
        raise RuntimeError(
            f"Refusing to add codes from {len(foreign)} system(s) FastFHIR does not "
            f"redistribute:\n{listed}{more}\n\n"
            "FastFHIR ships HL7 FHIR and UCUM code values only. Codes from other "
            "terminologies (SNOMED CT, LOINC, RxNorm, ICD, CPT, NDC, EDQM, ...) are "
            "licensed by their owners and must travel as FF_CODEABLE_CONCEPT blocks "
            "carrying the user's own code. See dictionaries/README.md."
        )


def load_ledger() -> dict:
    """Read the committed permanent code-ID ledger."""
    with open(_LEDGER_PATH, encoding="utf-8") as f:
        ledger = json.load(f)
    if ledger.get("_format") != 3:
        raise RuntimeError(
            f"{_LEDGER_PATH}: unsupported _format {ledger.get('_format')!r} (expected 3). "
            "Refusing to touch code IDs against a ledger this generator does not understand."
        )
    return ledger


def save_ledger(ledger: dict) -> None:
    """Write the ledger back, sorted by ID so diffs read as pure appends."""
    ledger["ids"] = dict(sorted(ledger["ids"].items(), key=lambda kv: kv[1]))
    ledger["versions"] = {v: sorted(c) for v, c in ledger["versions"].items()}
    ledger["scopes"] = {
        s: {k: sc[k] for k in sorted(sc, key=int)} for s, sc in sorted(ledger["scopes"].items())
    }
    with open(_LEDGER_PATH, "w", encoding="utf-8") as f:
        json.dump(ledger, f, indent=1, ensure_ascii=False)
        f.write("\n")


def assign_ids(ledger: dict, discovered: dict[str, dict]) -> int:
    """Reconcile discovered codes against the ledger. Append-only.

    `discovered` maps a code string to
    ``{"versions": {...}, "scopes": {scope: descriptor}}``.

    Returns the number of codes appended. Existing codes are never renumbered,
    retired codes are never dropped, and version/scope membership only ever
    grows -- a code that leaves an HL7 package keeps its ID, its string, and
    its C++ name, because stored archives still cite it.
    """
    from generator.emit.code_names import assign_identifier, struct_name

    ids = ledger["ids"]
    next_id = ledger["_next_id"]
    appended = 0

    for code in sorted(discovered):
        if code in ids:
            continue  # permanent -- never reassigned
        if next_id >= _FF_CODE_DICTIONARY_MAX:
            raise RuntimeError(
                f"Dictionary overflow: next id {next_id} >= {_FF_CODE_DICTIONARY_MAX}. "
                "Bit 31 is FF_CODEABLE_CONCEPT_FLAG and must stay clear."
            )
        ids[code] = next_id
        next_id += 1
        appended += 1

    ledger["_next_id"] = next_id

    # Version membership drives the string -> id ingest lookup, so dropping a
    # retired code would make archives containing it un-ingestible. Union only.
    for vname, members in ledger["versions"].items():
        found = {c for c, d in discovered.items() if vname in d["versions"]}
        ledger["versions"][vname] = sorted(set(members) | found)

    # Place every discovered code in its scope(s) and name it there. A code
    # already named in a scope keeps that name -- renaming is allowed but
    # gratuitous churn is not.
    for code, info in sorted(discovered.items()):
        cid = str(ids[code])
        for scope, descriptor in sorted(info["scopes"].items()):
            members = ledger["scopes"].setdefault(scope, {})
            if cid in members:
                continue
            container = (
                scope if scope in ("UCUM", "LEGACY") else struct_name(scope.split("::", 1)[1])
            )
            members[cid] = assign_identifier(code, descriptor, set(members.values()) | {container})

    _verify_ledger(ledger)
    return appended


def _verify_ledger(ledger: dict) -> None:
    """Fail loudly on anything that would corrupt an existing archive."""
    ids = ledger["ids"]
    if len(set(ids.values())) != len(ids):
        raise RuntimeError("Ledger corrupt: duplicate code IDs.")
    if 0 in ids.values():
        raise RuntimeError("Ledger corrupt: ID 0 is reserved (null slot).")
    worst = max(ids.values())
    if worst >= _FF_CODE_DICTIONARY_MAX:
        raise RuntimeError(f"Ledger corrupt: ID {worst} has bit 31 set.")
    if ledger["_next_id"] <= worst:
        raise RuntimeError(
            f"Ledger corrupt: _next_id {ledger['_next_id']} would collide with existing ID {worst}."
        )
    named = {int(cid) for scope in ledger["scopes"].values() for cid in scope}
    missing = [c for c, i in ids.items() if i not in named]
    if missing:
        raise RuntimeError(
            f"Ledger corrupt: {len(missing)} codes are in no scope and so have no "
            f"C++ identifier, e.g. {missing[:5]}"
        )


def generate_master_codes(package_dirs: dict[str, str]) -> tuple[dict, dict, set[str]]:
    """Scan NPM FHIR packages and reconcile them against the permanent ledger.

    Args:
        package_dirs: {"R4": "path/to/R4/package", "R5": "path/to/R5/package"}
    Returns:
        (systems, ledger, all_urls)
    """
    merged = {}
    token_versions = {}
    all_urls = set()

    for vname, pkg_dir in package_dirs.items():
        css = _collect_code_systems(pkg_dir)
        ucs = _collect_ucum_value_sets(pkg_dir)
        total = sum(len(v["entries"]) for v in css.values()) + sum(
            len(v["entries"]) for v in ucs.values()
        )
        print(f"  {vname}: {len(css)} CodeSystems, {len(ucs)} UCUM, {total} codes")
        for sys_data in css.values():
            all_urls.add(sys_data["source_url"])

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

    for sys_name in sorted(merged):
        merged[sys_name]["entries"].sort(key=lambda x: x["code"])

    # The ledger is keyed on the bare code string, matching FF_DICTIONARY_STRINGS
    # (one string per ID). Collapse the per-system tokens down to that, keeping
    # each system's own display text so the code can be named per scope.
    discovered: dict[str, dict] = {}
    for token, versions in token_versions.items():
        sys_name, code = token.split("|", 1)
        info = discovered.setdefault(code, {"versions": set(), "scopes": {}})
        info["versions"].update(versions)
        scope = (
            "UCUM" if "UCUM" in versions and sys_name.startswith("ucum-") else f"FHIR::{sys_name}"
        )
        descriptor = next(
            (e["descriptor"] for e in merged[sys_name]["entries"] if e["code"] == code), ""
        )
        info["scopes"].setdefault(scope, descriptor)

    # Gate BEFORE anything reaches the ledger.
    _assert_redistributable({n: d["source_url"] for n, d in merged.items()})

    ledger = load_ledger()
    before = len(ledger["ids"])
    snapshot = json.dumps(ledger, sort_keys=True)
    appended = assign_ids(ledger, discovered)
    if json.dumps(ledger, sort_keys=True) != snapshot:
        save_ledger(ledger)

    total = sum(len(v["entries"]) for v in merged.values())
    print(f"  Master: {len(merged)} systems, {total} entries")
    print(f"  Ledger: {before} committed IDs, {appended} appended, next id {ledger['_next_id']}")
    unknown = len(discovered) - sum(1 for c in discovered if c in ledger["ids"])
    if unknown:
        raise RuntimeError(f"{unknown} discovered codes still have no ID after assignment")

    return merged, ledger, all_urls


# =====================================================================
# dictionaries/*.cpp emission
#
# The lookup tables reference FF_Codes.hpp constants symbolically rather than
# by raw integer. That is deliberate: if a constant is renamed or disappears,
# these tables stop COMPILING instead of silently drifting out of agreement
# with the header. It is the one wire-safety property a C++ compiler can
# actually enforce for us.
# =====================================================================

# DERIVED artifacts: the runtime lookup tables are a pure projection of
# master_codes.json, so they live with the generator output. The ledger stays
# in dictionaries/ -- source there, derived in the output dir (generated_src/).

_BANNER = (
    "// Auto-generated from dictionaries/master_codes.json. DO NOT EDIT.\n"
    "//\n"
    "// The index/id in this table is a PERMANENT wire constant: it is what a\n"
    "// stored .ffhr archive contains. Entries are only ever appended.\n"
    "// See dictionaries/README.md.\n"
)


def _cpp_escape(s: str) -> str:
    return s.replace("\\", "\\\\").replace('"', '\\"')


def _emit(path: str, content: str) -> bool:
    """Write via the shared emitter, reporting whether the file changed.

    The write itself belongs to generator/emit/header.py -- this only adds the
    changed/unchanged answer the progress line needs.
    """
    existed = os.path.exists(path)
    previous = None
    if existed:
        with open(path, encoding="utf-8") as f:
            previous = f.read()
    write_if_changed(path, content)
    return previous != content


def _qualified_names(ledger: dict) -> dict[int, str]:
    """Map each permanent ID to its fully-qualified FF_Codes.hpp constant.

    A code can sit in more than one scope (515 of them do -- 'normal' is in five
    CodeSystems). They all alias the same ID, so any one qualification is
    correct; pick deterministically so the emitted table is stable.
    """
    from generator.emit.code_names import struct_name

    out: dict[int, str] = {}
    for scope in sorted(ledger["scopes"]):
        if scope == "UCUM":
            prefix = "FastFHIR::FF_CODE::UCUM"
        elif scope == "LEGACY":
            prefix = "FastFHIR::FF_CODE::LEGACY"
        else:
            prefix = f"FastFHIR::FF_CODE::FHIR::{struct_name(scope.split('::', 1)[1])}"
        for cid, ident in ledger["scopes"][scope].items():
            out.setdefault(int(cid), f"{prefix}::{ident}")
    missing = [c for c in ledger["ids"].values() if c not in out]
    if missing:
        raise RuntimeError(f"{len(missing)} IDs have no scope, e.g. {missing[:5]}")
    return out


def generate_dictionary_tables(
    ledger: dict | None = None, output_dir: str = "generated_src"
) -> None:
    """Emit FF_Dictionary_Strings.cpp and the per-version lookup tables."""
    ledger = ledger or load_ledger()
    ids = ledger["ids"]
    by_id = {cid: code for code, cid in ids.items()}
    top = max(by_id)

    # ---- FF_Dictionary_Strings.cpp : index position IS the code ID ----
    rows = ["    nullptr,"]
    for cid in range(1, top + 1):
        code = by_id.get(cid)
        # A retired code keeps its slot; the slot must never shift.
        rows.append(f'    "{_cpp_escape(code)}",' if code is not None else "    nullptr,")
    strings = (
        f"{_BANNER}#include <cstddef>\n\n"
        "extern const char* const FF_DICTIONARY_STRINGS[] = {\n" + "\n".join(rows) + "\n};\n\n"
        f"extern const size_t FF_DICTIONARY_STRINGS_SIZE = {top + 1};\n"
        "static_assert(sizeof(FF_DICTIONARY_STRINGS) / sizeof(FF_DICTIONARY_STRINGS[0])\n"
        '              == FF_DICTIONARY_STRINGS_SIZE, "string table size mismatch");\n'
    )
    written = [_emit(os.path.join(output_dir, "FF_Dictionary_Strings.cpp"), strings)]

    # ---- per-version {id, label} tables for string -> id ingest lookup ----
    qualified = _qualified_names(ledger)
    for vname in ("R4", "R5", "UCUM"):
        codes = sorted(ledger["versions"][vname], key=lambda c: ids[c])
        body = "".join(
            f"    {{ {q}, FF_DICTIONARY_STRINGS[static_cast<size_t>({q})] }},\n"
            for q in (qualified[ids[c]] for c in codes)
        )
        cpp = (
            f'{_BANNER}#include "FF_Dictionary.hpp"\n'
            f'#include "FF_Codes.hpp"\n\n'
            f"static const FF_CodeEntry k{vname}Table[] = {{\n{body}}};\n\n"
            f"const FF_CodeEntry* const FF_{vname}_DICTIONARY = k{vname}Table;\n"
            f"const size_t FF_{vname}_DICTIONARY_SIZE = "
            f"sizeof(k{vname}Table) / sizeof(k{vname}Table[0]);\n"
        )
        written.append(_emit(os.path.join(output_dir, f"FF_{vname}_Dictionary.cpp"), cpp))

    print(f"  Tables: {top + 1} string slots, {sum(written)}/4 files rewritten")
