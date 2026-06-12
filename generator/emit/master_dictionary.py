# FastFHIR Master Dictionary Generator
# Builds master_codes.json from committed dictionaries + UCUM.
# Deduplication: UCUM > R4 > R5.
# Deterministic: existing IDs preserved, new codes appended after max.

import json, os, re, subprocess
from generator.utilities import enclose_namespace

def build_master_json(input_dir="fhir_specs", output_dir="generator"):
    os.makedirs(output_dir, exist_ok=True)
    json_path = os.path.join(output_dir, "master_codes.json")

    existing_ids = {}
    existing_ucum = []
    if os.path.exists(json_path):
        with open(json_path) as f:
            old = json.load(f)
        existing_ids = old.get("_ids", {})
        existing_ucum = old.get("UCUM", [])
        print(f"  Loaded existing JSON: {len(existing_ids)} IDs, {len(existing_ucum)} UCUM")

    max_id = max(existing_ids.values()) if existing_ids else 0
    committed_labels = {}
    for v_name in ("R4", "R5"):
        path = f"dictionaries/FF_{v_name}_Dictionary.cpp"
        cpp = _git_show(path)
        if not cpp:
            continue
        for val_hex, label in re.findall(r'\{ (0x[0-9A-F]+), "([^"]+)" \}', cpp):
            code_id = int(val_hex, 16)
            if label in existing_ids:
                if existing_ids[label] != code_id:
                    raise RuntimeError(f"ID drift: {label}")
            else:
                existing_ids[label] = code_id
                max_id = max(max_id, code_id)
            committed_labels.setdefault(label, set()).add(v_name)
    print(f"  Committed dictionary codes: {len(committed_labels)}")

    if existing_ucum:
        ucum_labels = existing_ucum
    else:
        ucum_labels = _read_ucum_from_git()
    print(f"  UCUM codes: {len(ucum_labels)}")

    # Case-insensitive dedup: %{Abnormal} and %{abnormal} are the same concept
    seen_lower = set()
    deduped = []
    for label in sorted(ucum_labels):
        key = label.lower()
        if key not in seen_lower:
            seen_lower.add(key)
            deduped.append(label)
    ucum_labels = deduped
    ucum_set = set(ucum_labels)
    r4_labels = sorted(l for l, v in committed_labels.items() if "R4" in v and l not in ucum_set)
    r5_labels = sorted(l for l, v in committed_labels.items() if "R5" in v and l not in ucum_set and l not in r4_labels)
    ucum_sorted = sorted(ucum_labels)

    all_labels = set(ucum_sorted) | set(r4_labels) | set(r5_labels)
    new_labels = sorted(l for l in all_labels if l not in existing_ids)
    next_id = max_id + 1
    for label in new_labels:
        existing_ids[label] = next_id
        next_id += 1
    if new_labels:
        print(f"  New codes: {len(new_labels)}")

    doc = {"UCUM": ucum_sorted, "R4": r4_labels, "R5": r5_labels, "_ids": existing_ids}
    with open(json_path, "w") as f:
        json.dump(doc, f, indent=2)
    print(f"  Written {json_path}")
    print(f"    UCUM: {len(ucum_sorted)}, R4: {len(r4_labels)}, R5: {len(r5_labels)}, IDs: {len(existing_ids)}")
    return os.path.abspath(json_path)


def generate_from_master_json(json_path="generator/master_codes.json", output_dir="dictionaries"):
    with open(json_path) as f:
        doc = json.load(f)
    os.makedirs(output_dir, exist_ok=True)

    ucum = doc["UCUM"]
    r4 = doc["R4"]
    r5 = doc["R5"]
    ids = doc["_ids"]

    max_id = max(ids.values())
    strings_init = ["nullptr"] * (max_id + 1)
    for label, code_id in ids.items():
        esc = label.replace("\\", "\\\\").replace('"', '\\"')
        strings_init[code_id] = f'"{esc}"'

    strings_cpp = '#include <cstddef>\n\nextern const char* const FF_DICTIONARY_STRINGS[] = {\n'
    for s in strings_init:
        strings_cpp += f"    {s},\n"
    strings_cpp += f"}};\n\nextern const size_t FF_DICTIONARY_STRINGS_SIZE = {max_id + 1};\n"
    strings_cpp += 'static_assert(sizeof(FF_DICTIONARY_STRINGS) / sizeof(FF_DICTIONARY_STRINGS[0]) == FF_DICTIONARY_STRINGS_SIZE, "string table size mismatch");\n'
    _write_if_changed(os.path.join(output_dir, "FF_Dictionary_Strings.cpp"), strings_cpp)

    for v_name, labels in [("R4", r4), ("R5", r5)]:
        codes = [(ids[l], l) for l in labels if l in ids]
        codes.sort(key=lambda x: x[0])
        cpp = '#include "../include/FF_Dictionary.hpp"\n\nstatic const FF_CodeEntry k{}Table[] = {{\n'.format(v_name)
        for code_id, label in codes:
            cpp += f'    {{ 0x{code_id:08X}, FF_DICTIONARY_STRINGS[{code_id}] }},\n'
        cpp += f"}};\n\nconst FF_CodeEntry* const FF_{v_name}_DICTIONARY = k{v_name}Table;\n"
        cpp += f"const size_t FF_{v_name}_DICTIONARY_SIZE = sizeof(k{v_name}Table) / sizeof(k{v_name}Table[0]);\n"
        _write_if_changed(os.path.join(output_dir, f"FF_{v_name}_Dictionary.cpp"), cpp)
        print(f"  Generated: FF_{v_name}_Dictionary.cpp ({len(codes)} codes)")

    ucum_codes = [(ids[l], l) for l in ucum if l in ids]
    ucum_codes.sort(key=lambda x: x[0])
    r4_codes = [(ids[l], l) for l in r4 if l in ids]
    r4_codes.sort(key=lambda x: x[0])
    r5_codes = [(ids[l], l) for l in r5 if l in ids]
    r5_codes.sort(key=lambda x: x[0])

    ucum_idents = {l: _ucum_label_to_identifier(l) for _, l in ucum_codes}
    r4_idents = {l: _fhircode_to_identifier(l) for _, l in r4_codes}
    r5_idents = {l: _fhircode_to_identifier(l) for _, l in r5_codes}

    for v_name, labels, idents in [
        ("R4", r4, r4_idents),
        ("R5", r5, r5_idents),
    ]:
        codes = [(ids[l], l) for l in labels if l in ids]
        codes.sort(key=lambda x: x[0])
        cpp = _gen_version_dict_enum(v_name, codes, idents)
        _write_if_changed(os.path.join(output_dir, f"FF_{v_name}_Dictionary.cpp"), cpp)
        print(f"  Generated: FF_{v_name}_Dictionary.cpp ({len(codes)} codes)")

    # UCUM dictionary
    ucum_dict = [(ids[l], l) for l in ucum if l in ids]
    ucum_dict.sort(key=lambda x: x[0])
    ucum_cpp = _gen_version_dict_enum("UCUM", ucum_dict, ucum_idents)
    _write_if_changed(os.path.join(output_dir, "FF_UCUM_Dictionary.cpp"), ucum_cpp)
    print(f"  Generated: FF_UCUM_Dictionary.cpp ({len(ucum_dict)} codes)")

    hpp = _gen_codes_header(ucum_idents, r4_idents, r5_idents, ucum_codes, r4_codes, r5_codes)
    _write_if_changed(os.path.join(output_dir, "FF_Codes.hpp"), hpp)
    print(f"  Generated: FF_Codes.hpp (UCUM:{len(ucum_codes)} R4:{len(r4_codes)} R5:{len(r5_codes)})")
    print(f"  Generated: FF_Dictionary_Strings.cpp ({max_id + 1} entries)")


def _gen_version_dict(vname, codes):
    cpp = '#include "../include/FF_Dictionary.hpp"\n\nstatic const FF_CodeEntry k{}Table[] = {{\n'.format(vname)
    for code_id, label in codes:
        cpp += f'    {{ 0x{code_id:08X}, FF_DICTIONARY_STRINGS[{code_id}] }},\n'
    cpp += f"}};\n\nconst FF_CodeEntry* const FF_{vname}_DICTIONARY = k{vname}Table;\n"
    cpp += f"const size_t FF_{vname}_DICTIONARY_SIZE = sizeof(k{vname}Table) / sizeof(k{vname}Table[0]);\n"
    return cpp


def _gen_version_dict_enum(vname, codes, idents):
    entries = ""
    for code_id, label in codes:
        ident = _safe_ident(idents.get(label, "EMPTY"))
        enum_ref = f"FastFHIR::FF_CODE::{vname}::{ident}"
        entries += f'    {{ {enum_ref}, FF_DICTIONARY_STRINGS[static_cast<size_t>({enum_ref})] }},\n'
    table = f"static const FF_CodeEntry k{vname}Table[] = {{\n{entries}}};\n"
    table += f"const FF_CodeEntry* const FF_{vname}_DICTIONARY = k{vname}Table;\n"
    table += f"const size_t FF_{vname}_DICTIONARY_SIZE = sizeof(k{vname}Table) / sizeof(k{vname}Table[0]);\n"
    return f'#include "../include/FF_Dictionary.hpp"\n#include "FF_Codes.hpp"\n\n{table}'


def _write_if_changed(path, content):
    try:
        with open(path) as f:
            if f.read() == content:
                print(f"  (unchanged) {path}")
                return
    except FileNotFoundError:
        pass
    with open(path, "w") as f:
        f.write(content)
    print(f"  Written {path}")


def _git_show(path):
    try:
        r = subprocess.run(["git", "show", f"HEAD:{path}"], capture_output=True, text=True, timeout=10)
        return r.stdout if r.returncode == 0 else None
    except Exception:
        return None


def _read_ucum_from_git():
    cpp = _git_show("dictionaries/FF_UCUM_Concepts.cpp")
    if not cpp:
        return []
    sb = re.search(r'FF_UCUM_STRINGS\[\]\s*=\s*\{(.*?)\};', cpp, re.DOTALL)
    if sb:
        return re.findall(r'"([^"]*)"', sb.group(1))
    return []


def _fhircode_to_identifier(label):
    parts = re.split(r'[^a-zA-Z0-9]+', label)
    parts = [p.upper() for p in parts if p]
    return '_'.join(parts) if parts else 'EMPTY'


def _ucum_label_to_identifier(label):
    # Semantic naming for common UCUM patterns
    special = {"%": "PERCENT", "'": "ARC_MINUTE", "''": "ARC_SECOND",
               '"': "ARC_INCH", "10*": "TEN_POWER", "10^": "TEN_TO_THE"}
    if label in special:
        return special[label]

    # Power-of-ten semantic names
    power_names = {
        "10*3": "THOUSAND", "10*6": "MILLION", "10*9": "BILLION",
        "10*12": "TRILLION", "10*15": "QUADRILLION",
        "10*-3": "THOUSANDTH", "10*-6": "MILLIONTH", "10*-9": "BILLIONTH",
        "10*-12": "TRILLIONTH",
        "10*2": "HUNDRED", "10*1": "TEN", "10*-1": "TENTH", "10*-2": "HUNDREDTH",
    }
    if label in power_names:
        return power_names[label]

    # Per-power-of-ten: /10*N → PER_N
    for power, name in power_names.items():
        if label == f"/{power}":
            return f"PER_{name}"

    # Metric prefixes → full names
    metric_prefix = {"Y": "YOTTA", "Z": "ZETTA", "E": "EXA", "P": "PETA",
                     "T": "TERA", "G": "GIGA", "M": "MEGA",
                     "k": "KILO", "h": "HECTO", "da": "DEKA",
                     "d": "DECI", "c": "CENTI", "m": "MILLI",
                     "u": "MICRO", "n": "NANO", "p": "PICO",
                     "f": "FEMTO", "a": "ATTO", "z": "ZEPTO", "y": "YOCTO"}
    # Base units → full names
    base_units = {"m": "METER", "g": "GRAM", "s": "SECOND", "L": "LITER",
                 "Hz": "HERTZ", "N": "NEWTON", "Pa": "PASCAL",
                 "J": "JOULE", "W": "WATT", "V": "VOLT", "A": "AMPERE",
                 "F": "FARAD", "H": "HENRY", "S": "SIEMENS",
                 "Wb": "WEBER", "T": "TESLA", "Bq": "BECQUEREL",
                 "Gy": "GRAY", "Sv": "SIEVERT", "kat": "KATAL",
                 "mol": "MOLE", "cd": "CANDELA", "rad": "RADIAN",
                 "sr": "STERADIAN", "lm": "LUMEN", "lx": "LUX",
                 "C": "COULOMB", "K": "KELVIN",
                 "min": "MINUTE", "h": "HOUR", "d": "DAY",
                 "wk": "WEEK", "mo": "MONTH", "a": "YEAR"}
    # Map metric-prefixed unit to semantic name (e.g., dL → DECILITER)
    for pfx, pfx_name in metric_prefix.items():
        for unit, unit_name in base_units.items():
            if label == pfx + unit:
                return f"{pfx_name}{unit_name}"

    parts = []
    i = 0
    while i < len(label):
        c = label[i]
        # Handle 10*N power-of-ten patterns (e.g., 10*3, 10*-6, 10*3/uL)
        if c == "1" and i + 2 < len(label) and label[i:i+3] == "10*":
            j = i + 3
            if j < len(label) and label[j] == "-":
                j += 1
            while j < len(label) and label[j].isdigit():
                j += 1
            power = label[i:j]
            name = power_names.get(power)
            if name:
                parts.append(name)
            else:
                parts.append(f"TEN_POW_{label[i+3:j].replace('-', 'NEG_')}")
            i = j
            continue
        if c == "{":
            j = label.find("}", i)
            if j > i:
                for word in re.split(r"[^a-zA-Z0-9]+", label[i+1:j]):
                    if word:
                        parts.append(_split_camel(word))
                i = j + 1
            else:
                i += 1
        elif c == "[":
            j = label.find("]", i)
            if j > i:
                for word in re.split(r"[^a-zA-Z0-9]+", label[i+1:j]):
                    if word:
                        parts.append(word.upper())
                i = j + 1
            else:
                i += 1
        elif c == "/":
            parts.append("PER")
            i += 1
        elif c == "%":
            parts.append("PERCENT")
            i += 1
        elif c == "*":
            parts.append("TIMES")
            i += 1
        elif c == ".":
            i += 1
        elif c == "'":
            if i + 1 < len(label) and label[i+1] == "'":
                parts.append("DOUBLE_PRIME")
                i += 2
            else:
                parts.append("PRIME")
                i += 1
        elif c.isalpha() or c.isdigit():
            start = i
            while i < len(label) and (label[i].isalpha() or label[i].isdigit()):
                i += 1
            word = label[start:i]
            # Check if word is a known metric-prefixed unit (e.g., dL, km, mg)
            semantic = None
            for pfx, pfx_name in metric_prefix.items():
                for unit, unit_name in base_units.items():
                    if word == pfx + unit:
                        semantic = f"{pfx_name}{unit_name}"
                        break
                if semantic:
                    break
            if semantic:
                parts.append(semantic)
            elif word in base_units:
                parts.append(base_units[word])
            else:
                parts.append(_split_camel(word))
        else:
            i += 1
    ident = "_".join(p for p in parts if p).upper()
    return ident or "EMPTY"


def _split_camel(s):
    result = []
    cur = ""
    for ch in s:
        if cur and cur[-1].islower() and ch.isupper() and len(cur) > 1:
            result.append(cur.upper())
            cur = ch
        else:
            cur += ch
    if cur:
        result.append(cur.upper())
    return "_".join(result)


def _safe_ident(ident):
    """Ensure identifier is valid C++ (prepend _ if starts with digit)."""
    return f"_{ident}" if ident and ident[0].isdigit() else ident


def _gen_codes_header(ucum_idents, r4_idents, r5_idents, ucum_entries, r4_entries, r5_entries):
    def gen_enum(entries, label_to_ident_fn):
        body = "    _NONE = 0,\n"
        used = set()
        for code_id, label in entries:
            base = _safe_ident(label_to_ident_fn(label))
            ident = base
            suffix = 2
            while ident in used:
                ident = f"{base}_{suffix}"
                suffix += 1
            used.add(ident)
            body += f"    {ident} = {code_id},\n"
        return body

    ucum_body = gen_enum(ucum_entries, lambda l: ucum_idents[l])
    r4_body = gen_enum(r4_entries, lambda l: r4_idents[l])
    r5_body = gen_enum(r5_entries, lambda l: r5_idents[l])

    body = f"""enum class UCUM : uint32_t {{
{ucum_body}}};

enum class R4 : uint32_t {{
{r4_body}}};

enum class R5 : uint32_t {{
{r5_body}}};

using METRIC = UCUM;
using Quantity = UCUM;"""

    return f"""#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include "../include/FF_Dictionary.hpp"

#ifdef M_E
#undef M_E
#endif
#ifdef DOMAIN
#undef DOMAIN
#endif

{enclose_namespace("FastFHIR::FF_CODE", body)}

// UCUM thin wrappers are in FF_Dictionary.hpp
REMOVED_{
    return static_cast<FastFHIR::FF_CODE::UCUM>(
        FF_GetDictionaryCode(std::string(label), FHIR_VERSION_R5));
}}

inline const char* FF_ResolveUCUMCode(FastFHIR::FF_CODE::UCUM code) noexcept {{
    return FF_ResolveCode(static_cast<uint32_t>(code), FHIR_VERSION_R5);
}}
"""
