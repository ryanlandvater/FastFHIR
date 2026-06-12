"""FastFHIR Generator — shared utility functions.

Standalone helpers shared across the generator package.  These are
general-purpose string formatting, file parsing, and C++ code-generation
utilities that don't belong to any single sub-package.

``enclose_namespace`` wraps a code block in a ``namespace { ... }``
so callers never need to manually track ``namespace`` open/close pairs.
"""

from __future__ import annotations

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
    with open(recovery_path, "r") as f:
        for line in f:
            m = re.match(r"\s+(\w+)\s*=\s*(0x[0-9A-Fa-f]+)", line)
            if m:
                tags[m.group(1)] = int(m.group(2), 16)
    return tags
