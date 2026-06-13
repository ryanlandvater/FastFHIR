"""Generate dictionaries/FF_Codes.hpp from generator/master_codes.json.

Uses #define FF_CODE_DEF for clean generated code.
Descriptor-driven naming for poor code identifiers.
"""
import json, re, os

INPUT = os.path.join(os.path.dirname(__file__), "..", "master_codes.json")
OUTPUT = os.path.join(os.path.dirname(__file__), "..", "..", "dictionaries", "FF_Codes.hpp")


def sanitize(text):
    syms = {
        '%':'PERCENT','/':'PER_','*':'TIMES_','+':'PLUS_',"'":'PRIME',
        '[':'',']':'','{':'','}':'','(':'',')':'','.':'_','=':'EQ_',
        '<':'LT_','>':'GT_','!':'NOT_','#':'NUM_','@':'AT_','&':'AND_',
        '~':'TILDE_',':':'_','|':'_','^':'CARET_','$':'DOLLAR_','"':'INCH',
    }
    r = text
    for s, repl in syms.items(): r = r.replace(s, repl)
    r = re.sub(r'[^A-Za-z0-9_]', '_', r)
    r = re.sub(r'_+', '_', r).strip('_').upper()
    return r or ''


def code_ident(code, descriptor):
    """Generate a C++ identifier from a code string, falling back to descriptor."""
    ident = sanitize(code)
    # If sanitized code is empty or pure numeric -> use descriptor
    if not ident or ident[0].isdigit():
        desc_ident = sanitize(descriptor)
        if desc_ident:
            # If code looks like a version (digits+dots), prefix with V_
            if re.match(r'^[\d.]+$', code):
                ident = 'V_' + desc_ident
            else:
                ident = desc_ident
    # Must not start with digit
    if ident and ident[0].isdigit():
        ident = 'V_' + ident
    return ident or 'EMPTY'


def generate():
    with open(INPUT, encoding="utf-8") as f:
        data = json.load(f)

    systems = data["systems"]
    id_map = {}
    for vm in data["_ids"].values():
        id_map.update(vm)

    lines = [
        "// Auto-generated from NPM FHIR CodeSystem packages.",
        "// DO NOT EDIT.",
        "#pragma once",
        "#include <cstdint>",
        "",
        "namespace FF_CODES {",
        "",
        "#define FF_CODE_DEF static inline constexpr uint32_t",
        "",
        "class Token {",
        "public:",
        "    constexpr Token(uint32_t val) : _id(val) {}",
        "    constexpr uint32_t get() const { return _id; }",
        "private:",
        "    uint32_t _id;",
        "};",
        "",
    ]

    lines.append("namespace FHIR {")
    for sn in sorted(systems):
        if sn.startswith("ucum-"): continue
        si = sanitize(sn)
        if not si: continue
        lines.append(f"struct {si} {{")
        for e in sorted(systems[sn]["entries"], key=lambda x: x["code"]):
            t = f"{sn}|{e['code']}"
            if t in id_map:
                ci = code_ident(e["code"], e["descriptor"])
                lines.append(f"    FF_CODE_DEF {ci} = {id_map[t]};")
        lines.append("};")
        lines.append("")
    lines.append("}  // namespace FHIR")
    lines.append("")

    for sn in sorted(systems):
        if not sn.startswith("ucum-"): continue
        ns = "UCUM_" + (sanitize(sn[5:]) or "UNITS")
        lines.append(f"namespace {ns} {{")
        for e in sorted(systems[sn]["entries"], key=lambda x: x["code"]):
            t = f"{sn}|{e['code']}"
            if t in id_map:
                ci = code_ident(e["code"], e["descriptor"])
                lines.append(f"    FF_CODE_DEF {ci} = {id_map[t]};")
        lines.append("}  // namespace UCUM")
        lines.append("")

    lines.append("}  // namespace FF_CODES")

    with open(OUTPUT, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")

    count = sum(1 for l in lines if "FF_CODE_DEF " in l)
    print(f"Generated {OUTPUT}  ({count} constants)")


if __name__ == "__main__":
    generate()
