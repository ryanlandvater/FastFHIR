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


def validate_recovery_tags(output_dir: str, recovery_path: str = "include/FF_Recovery.hpp") -> int:
    """Fail loudly if the generator emitted a RECOVERY_TAG the header lacks.

    `include/FF_Recovery.hpp` is hand-maintained and its values are permanent
    wire constants -- the generator only ever *references* them. But the
    reference is built by string concatenation
    (``f"RECOVER_{child_struct}"`` in model/structure.py), so a rename or a new
    FHIR type can produce a name that does not exist.

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
            "RECOVERY_TAG values are permanent wire constants and the header is "
            "hand-maintained -- the generator may only reference existing tags. Either "
            "add the tag to the header deliberately (it is a wire constant: append, "
            "never renumber) or fix the name the emitter is building."
        )
    return len(referenced)
