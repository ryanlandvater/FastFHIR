"""FastFHIR Generator — shared utility functions.

Standalone helpers shared across the generator package.  These are
general-purpose string formatting, file parsing, and C++ code-generation
utilities that don't belong to any single sub-package.

``enclose_namespace`` wraps a code block in a ``namespace { ... }``
so callers never need to manually track ``namespace`` open/close pairs.
"""

from __future__ import annotations

import os
import re


def enclose_namespace(ns: str, code: str) -> str:
    """Wrap *code* inside ``namespace ns { ... }`` with a closing comment.

    Args:
        ns: Fully qualified namespace name, e.g. ``"FastFHIR"`` or
            ``"FastFHIR::FieldKeys"``.
        code: The C++ code to wrap.

    Returns:
        ``code`` enclosed in the namespace block.
    """
    return f"namespace {ns} {{\n{code}\n}} // namespace {ns}\n"


def parse_recovery_tags(recovery_path: str = "include/FF_Recovery.hpp") -> dict[str, int]:
    """Parse ``RECOVERY_TAG`` enum values from the permanent recovery header.

    Reads ``include/FF_Recovery.hpp`` and extracts all ``NAME = 0xVALUE``
    entries.  Used by the generator to validate that emitted recovery tags
    match the permanent C++ header.

    Args:
        recovery_path: Path to ``FF_Recovery.hpp`` relative to workspace root.
            Defaults to ``include/FF_Recovery.hpp``.

    Returns:
        Dict mapping tag names (e.g. ``"RECOVER_FF_STRING"``) to their
        integer hex values.
    """
    tags: dict[str, int] = {}
    with open(recovery_path) as f:
        for line in f:
            # enum members:  "    RECOVER_FF_STRING = 0x0101,"
            m = re.match(r"\s+(\w+)\s*=\s*(0x[0-9A-Fa-f]+)", line)
            if m:
                tags[m.group(1)] = int(m.group(2), 16)
                continue
            # file-scope constants: "constexpr uint16_t RECOVER_ARRAY_BIT = 0x8000;"
            m = re.match(r"constexpr\s+\w+\s+(\w+)\s*=\s*(0x[0-9A-Fa-f]+)", line)
            if m:
                tags[m.group(1)] = int(m.group(2), 16)
    return tags


def validate_recovery_bands(recovery_path: str = "include/FF_Recovery.hpp") -> None:
    """Fail loudly if a tag sits outside the band its NAME implies, or collides.

    The bands in FF_Recovery.hpp are not documentation: FF_IsResourceTag and
    FF_IsScalarBlockTag classify a block by which band its tag falls in, so a tag
    written into the wrong band is silently mis-classified at runtime rather than
    failing to compile. The C++ static_asserts catch a bad *boundary*; this
    catches a bad *tag*, which is the accident that actually happens when someone
    appends by hand to a 900-entry ledger.

    Also rejects duplicate values. A C++ enum happily accepts two enumerators
    with the same value, so two block types sharing a recovery tag -- which makes
    their blocks indistinguishable on the wire -- is otherwise completely silent.

    Limits: this checks that a tag is inside SOME band, not that it is inside the
    RIGHT one. A resource tag mistakenly written at 0x1500 lands in the backbone
    band and passes here. Catching that needs the caller's knowledge of what each
    tag is *for*, which the generator has when it builds the name by
    concatenation -- see TASKS.md A29.2.
    """
    tags = parse_recovery_tags(recovery_path)
    bands = {
        "PRIMITIVE": (tags["RECOVER_BAND_PRIMITIVE_FIRST"], tags["RECOVER_BAND_PRIMITIVE_LAST"]),
        "SCALAR": (tags["RECOVER_BAND_SCALAR_FIRST"], tags["RECOVER_BAND_SCALAR_LAST"]),
        "DATATYPE": (tags["RECOVER_BAND_DATATYPE_FIRST"], tags["RECOVER_BAND_DATATYPE_LAST"]),
        "RESOURCE": (tags["RECOVER_BAND_RESOURCE_FIRST"], tags["RECOVER_BAND_RESOURCE_LAST"]),
        "BACKBONE": (tags["RECOVER_BAND_BACKBONE_FIRST"], tags["RECOVER_BAND_BACKBONE_LAST"]),
    }
    # Boundary/mask constants are not themselves tags.
    skip = set(bands) | {"RECOVER_ARRAY_BIT", "RECOVER_TYPE_MASK"}
    members = {
        n: v
        for n, v in tags.items()
        if n.startswith(("RECOVER_FF_", "FF_RECOVER_"))
        and not any(n.startswith(f"RECOVER_BAND_{b}") for b in bands)
        and n not in skip
    }

    errors: list[str] = []

    seen: dict[int, str] = {}
    for name, value in sorted(members.items(), key=lambda kv: (kv[1], kv[0])):
        if value in seen:
            errors.append(
                f"    duplicate value 0x{value:04X}: {seen[value]} and {name} would be "
                "indistinguishable on the wire"
            )
        else:
            seen[value] = name
        if not any(lo <= value <= hi for lo, hi in bands.values()):
            errors.append(f"    {name} = 0x{value:04X} falls outside every band")

    if errors:
        raise RuntimeError(
            f"{len(errors)} recovery-tag band violation(s) in {recovery_path}:\n"
            + "\n".join(errors)
            + "\n\nSee the BAND MAP at the top of that header. Tag values are "
            "permanent wire constants -- fix the new tag, never an existing one."
        )


def validate_recovery_tags(output_dir: str, recovery_path: str = "include/FF_Recovery.hpp") -> int:
    """Fail loudly if the generator emitted a RECOVERY_TAG the header lacks.

    `include/FF_Recovery.hpp` is emitted by emit/recovery_tags.py from the
    committed ledger `dictionaries/master_tags.json`, at stage 1b -- before
    anything that references a tag. Its values are permanent wire constants.
    Every tag the spec needs is therefore already declared by the time the
    emitters run, but the reference is built by string concatenation
    (``f"RECOVER_{child_struct}"`` in model/structure.py), so a rename or a
    name discovery never produced can still ask for a tag that does not exist.

    That failure currently surfaces as a wall of C++ "undeclared identifier"
    errors across dozens of generated files. Catching it here names the exact
    tag and the file that wanted it.

    Returns the number of distinct tags referenced.
    """
    known = set(parse_recovery_tags(recovery_path))
    known.add("FF_RECOVER_UNDEFINED")  # the sentinel, declared as an enumerator

    referenced: dict[str, str] = {}
    for entry in sorted(os.listdir(output_dir)):
        if not entry.endswith((".hpp", ".cpp")):
            continue
        path = os.path.join(output_dir, entry)
        with open(path, encoding="utf-8") as fh:
            for tag in re.findall(r"\b(?:FF_)?RECOVER_[A-Z0-9_]+\b", fh.read()):
                referenced.setdefault(tag, entry)

    unknown = {t: f for t, f in referenced.items() if t not in known}
    if unknown:
        listed = "\n".join(f"    {t}  (first seen in {f})" for t, f in sorted(unknown.items()))
        raise RuntimeError(
            f"{len(unknown)} RECOVERY_TAG(s) were emitted that {recovery_path} does not "
            f"declare:\n{listed}\n\n"
            "RECOVERY_TAG values are permanent wire constants. The header is "
            "GENERATED from dictionaries/master_tags.json -- never hand-edit it. "
            "Tag discovery covers the whole spec and appends before the emitters "
            "run, so a name missing here is almost always a name the emitter built "
            "wrong: fix the emitter. If the tag is genuinely a new FHIR type, make "
            "discovery in emit/recovery_tags.py see it and re-run; it will append "
            "at the next free value in its band."
        )
    return len(referenced)
