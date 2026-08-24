"""Emit generated_src/FF_Codes.hpp -- the named C++ constants for every code ID.

Two jobs, and it is worth keeping them straight:

  * NAMING (this file) decides what a code is *called* in C++. An identifier is
    source-level only. Renaming one breaks a recompile; it does not touch a
    single byte on the wire, and it never moves an ID.

  * NUMBERING (generator/emit/code_ids.py) decides what a code *is* on the
    wire. That is permanent. See dictionaries/README.md.

Identifiers for codes already in the ledger are read straight out of it, so the
header is a pure projection of committed state. Only genuinely new codes get a
name minted here, via assign_identifier().
"""

from __future__ import annotations

import json
import os
import re

LEDGER = os.path.join(os.path.dirname(__file__), "..", "..", "dictionaries", "master_codes.json")
# DERIVED artifact, not a ledger: a pure projection of master_codes.json, so it
# lives with the rest of the generator output rather than beside the ledger it
# comes from. dictionaries/ holds the SOURCE (the JSON); the output dir holds
# what is derived from it. The output dir is a parameter of generate() (the
# wire gate regenerates into a tmp dir); generated_src/ is the default.


# Identifiers that are macros in the C/C++ standard library or on Windows. The
# preprocessor would textually replace them, so they carry a _CODE suffix. This
# is why the header needs no #undef games -- undef'ing M_E in a public header
# would sabotage the consumer's own math code.
RESERVED_MACROS = frozenset(
    {
        # <math.h>
        "DOMAIN",
        "SING",
        "OVERFLOW",
        "UNDERFLOW",
        "TLOSS",
        "PLOSS",
        "HUGE",
        "HUGE_VAL",
        "INFINITY",
        "NAN",
        "M_E",
        "M_LOG2E",
        "M_LOG10E",
        "M_LN2",
        "M_LN10",
        "M_PI",
        "M_PI_2",
        "M_PI_4",
        "M_1_PI",
        "M_2_PI",
        "M_2_SQRTPI",
        "M_SQRT2",
        "M_SQRT1_2",
        # <stdio.h> / <stdlib.h> / <stddef.h>
        "EOF",
        "NULL",
        "BUFSIZ",
        "RAND_MAX",
        "EXIT_SUCCESS",
        "EXIT_FAILURE",
        # <stdbool.h> and common platform spellings
        "TRUE",
        "FALSE",
        "BOOL",
        # <errno.h> / POSIX control characters
        "ERROR",
        "DEL",
        "NL",
        "CR",
        "BS",
        "FF",
        "VT",
        "SP",
        "ESC",
        # Windows <windef.h> / <winnt.h>
        "MIN",
        "MAX",
        "IN",
        "OUT",
        "OPTIONAL",
        "CONST",
        "VOID",
        "NEAR",
        "FAR",
        "PASCAL",
        "WINAPI",
        "INTERFACE",
        "SEVERITY_ERROR",
        "DIFFERENCE",
        # <complex.h>
        "I",
        "complex",
        "imaginary",
    }
)

_SYMBOLS = {
    "%": "PERCENT_",
    "/": "PER_",
    "*": "TIMES_",
    "+": "PLUS_",
    "'": "PRIME",
    "[": "",
    "]": "",
    "{": "",
    "}": "",
    "(": "",
    ")": "",
    ".": "_",
    "=": "EQ_",
    "<": "LT_",
    ">": "GT_",
    "!": "NOT_",
    "#": "NUM_",
    "@": "AT_",
    "&": "AND_",
    "~": "TILDE_",
    ":": "_",
    "|": "_",
    "^": "CARET_",
    "$": "DOLLAR_",
    '"': "INCH",
}


def _fold(text: str, upper: bool) -> str:
    r = text
    for sym, repl in _SYMBOLS.items():
        r = r.replace(sym, repl)
    r = re.sub(r"[^A-Za-z0-9_]", "_", r)
    r = re.sub(r"_+", "_", r).strip("_")
    return r.upper() if upper else r


def sanitize(text: str) -> str:
    """Fold a code or descriptor into an upper-case C++ identifier fragment."""
    return _fold(text, upper=True)


def sanitize_cased(text: str) -> str:
    """sanitize() without the case fold.

    UCUM is case-sensitive by design -- 'Ms' (megasecond) and 'ms'
    (millisecond) are different units -- so the fold destroys the only thing
    that separates them. Used to break ties, never as the default.
    """
    return _fold(text, upper=False)


def _ident_from(code: str, descriptor: str, upper: bool) -> str:
    ident = _fold(code, upper)
    if not ident or ident[0].isdigit():
        desc = _fold(descriptor, upper)
        if desc:
            ident = ("V_" + desc) if re.match(r"^[\d.]+$", code) else desc
    if ident and ident[0].isdigit():
        ident = "V_" + ident
    return ident or "EMPTY"


def code_ident(code: str, descriptor: str = "") -> str:
    """Default C++ identifier for a code."""
    return _ident_from(code, descriptor, upper=True)


def assign_identifier(code: str, descriptor: str, taken: set[str]) -> str:
    """Mint an identifier for a NEW code that does not collide with `taken`.

    Discriminates on what actually differs between codes -- case, then bracket
    class -- never on the FHIR display text, which changes whenever HL7 edits a
    label and would churn the C++ API for no reason.
    """
    bracket = "_SB" if "[" in code else ("_CB" if "{" in code else "")
    cased = _ident_from(code, descriptor, upper=False)
    for cand in (code_ident(code, descriptor), cased, cased + bracket if bracket else ""):
        if cand and cand not in taken and cand not in RESERVED_MACROS:
            return cand

    stem = code_ident(code, descriptor)
    if stem in RESERVED_MACROS:
        stem += "_CODE"
    ident, n = stem, 1
    while ident in taken:
        ident = f"{stem}_{n}"
        n += 1
    return ident


def struct_name(system: str) -> str:
    """C++ struct name for a FHIR CodeSystem (e.g. 'fdi-surface' -> FDI_SURFACE)."""
    return sanitize(system) or "UNNAMED"


def generate(output_dir: str = "generated_src") -> None:
    """Project the committed ledger into <output_dir>/FF_Codes.hpp.

    Scoping is by terminology SOURCE, then by CodeSystem within FHIR:

        FF_CODE::UCUM::MMHG
        FF_CODE::FHIR::ADMINISTRATIVE_GENDER::MALE
        FF_CODE::LEGACY::<retired>

    FHIR revision (R4/R5) is deliberately NOT a namespace axis. It is version
    membership, and that already lives in the per-version lookup tables
    (FF_R4_DICTIONARY / FF_R5_DICTIONARY / FF_UCUM_DICTIONARY). Duplicating it
    here would only produce overlapping namespaces -- 908 codes are in both R4
    and R5, and under shared-ID keying they are the same constant either way.
    """
    with open(LEDGER, encoding="utf-8") as f:
        ledger = json.load(f)

    # scope name -> {str(permanent id): C++ identifier}. A code carries one ID
    # but is named per scope, so id 15024 is UCUM::LITER and FDI_SURFACE::L.
    scopes = ledger["scopes"]

    def members(scope: str, indent: str) -> list[str]:
        out, seen = [], set()
        for cid, ident in sorted(scopes.get(scope, {}).items(), key=lambda kv: int(kv[0])):
            if ident in seen:
                raise RuntimeError(f"{scope}: duplicate identifier {ident!r} (id {cid})")
            seen.add(ident)
            out.append(f"{indent}FF_CODE_DEF {ident} = {cid};")
        return out

    lines = [
        "// Auto-generated from dictionaries/master_codes.json. DO NOT EDIT.",
        "//",
        "// Values are PERMANENT wire constants -- they decode every .ffhr archive",
        "// ever written. Regenerating this file may add constants and may rename",
        "// them, but must never change a number. See dictionaries/README.md.",
        "//",
        "// Scoped by terminology source, then by CodeSystem. FHIR revision is NOT",
        "// a namespace here -- that is version membership, carried by the",
        "// FF_R4/R5/UCUM_DICTIONARY lookup tables.",
        "#pragma once",
        "#include <cstdint>",
        "",
        "namespace FastFHIR::FF_CODE {",
        "",
        "#define FF_CODE_DEF static inline constexpr uint32_t",
        "",
        "// ---- UCUM (unitsofmeasure.org) ----",
        "namespace UCUM {",
        *members("UCUM", "    "),
        "}  // namespace UCUM",
        "",
        "// ---- HL7 FHIR CodeSystems ----",
        "namespace FHIR {",
        "",
    ]

    fhir_systems = sorted(s for s in scopes if s.startswith("FHIR::"))
    for scope in fhir_systems:
        sn = struct_name(scope.split("::", 1)[1])
        lines.append(f"struct {sn} {{")
        lines.extend(members(scope, "    "))
        lines.append("};")
        lines.append("")

    lines += ["}  // namespace FHIR", ""]

    # Only emit LEGACY if something is actually retired into it. An empty
    # namespace is noise in a header people read.
    if scopes.get("LEGACY"):
        lines += [
            "// ---- Retired ----",
            "// Codes no current HL7 package claims. They keep their IDs forever",
            "// because stored archives still cite them, but no source grouping",
            "// survives for them.",
            "namespace LEGACY {",
            *members("LEGACY", "    "),
            "}  // namespace LEGACY",
            "",
        ]

    lines += ["}  // namespace FastFHIR::FF_CODE"]

    output_path = os.path.join(output_dir, "FF_Codes.hpp")
    os.makedirs(os.path.dirname(output_path), exist_ok=True)
    with open(output_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")

    total = sum(len(v) for v in scopes.values())
    print(
        f"Generated {output_path}  ({total} constants: "
        f"{len(scopes.get('UCUM', {}))} UCUM, {len(fhir_systems)} FHIR structs, "
        f"{len(scopes.get('LEGACY', {}))} legacy)"
    )


if __name__ == "__main__":
    generate()
