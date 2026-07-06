"""Extract the wire-format-relevant constants from generated C++.

THE GATE for the generator refactor is *wire-format stability*, not C++ source
text byte-identity (see generator_refactor_plan.md (deleted; in git history) section 0). The refactor may
freely reformat emitted C++; this witness ignores formatting and captures only
the values that actually serialize into a `.ffhr` stream:

  * recovery tags        — RECOVER_FF_* = 0xNNNN   (bytes 8-9 of every block)
  * V-Table layout       — field ORDER + per-field size-constant + HEADER_*_SIZE
                           literals. NB: actual byte offsets are symbolic sums
                           (`RECOVERY = VALIDATION + VALIDATION_S`) resolved by
                           the C++ compiler, so we capture the *structure* that
                           determines them, not a (non-existent) literal offset.
  * dictionary codes     — FF_*_CODE_* = 0xNNNNNNNN (hash-based uint32)

If the witness JSON is unchanged across a refactor, the wire format is preserved
regardless of how the emitting Python or the C++ source text was reorganised.

Usage:
    python -m tests.generator.wire_witness <generated_dir> <out.json>
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

# --- recovery tags: `RECOVER_FF_BUNDLE = 0x0302` (also FF_RECOVER_UNDEFINED) ---
_TAG = re.compile(r"\b(FF_RECOVER_[A-Z0-9_]+|RECOVER_FF_[A-Z0-9_]+)\s*=\s*(0x[0-9A-Fa-f]+|\d+)")

# --- dictionary codes: `FF_R5_CODE_PERCENT = 0x1CF1F3BB` ---
_CODE = re.compile(r"\b(FF_[A-Z0-9]+_CODE_[A-Z0-9_]+)\s*=\s*(0x[0-9A-Fa-f]+|\d+)")

# --- HEADER size literals inside vtable_offsets: `HEADER_R5_SIZE = 46` ---
_HEADER_SIZE = re.compile(r"\b(HEADER_[A-Z0-9_]*SIZE)\s*=\s*(\d+)")

# --- a vtable_sizes entry: `ID_S = TYPE_SIZE_OFFSET,` ---
_SIZE_ENTRY = re.compile(r"\b([A-Z][A-Z0-9_]*_S)\s*=\s*(TYPE_SIZE_[A-Z0-9_]+)")

# --- a vtable_offsets field line: `EXTENSION = RECOVERY + RECOVERY_S`/`= 0` ---
# Capture only the field NAME in declaration order; the RHS is a symbolic sum we
# do not need to evaluate (the size-constant per field is captured separately).
_OFFSET_FIELD = re.compile(r"^\s*([A-Z][A-Z0-9_]*)\s*=\s*[^,]+,?\s*$")

# Field names that are layout machinery, not wire fields, excluded from order.
_NON_FIELD = {"HEADER_R4_SIZE", "HEADER_R5_SIZE", "HEADER_SIZE"}


def _vtable_layout(text: str) -> dict[str, dict]:
    """Parse every `vtable_sizes` / `vtable_offsets` enum pair in one header.

    Returns {block_struct_name: {"order": [...], "sizes": {field: const},
    "header_sizes": {name: int}}}. Keyed by the preceding `struct FF_* :
    DATA_BLOCK` name so witnesses stay stable even if file order changes.
    """
    out: dict[str, dict] = {}
    current: str | None = None
    mode: str | None = None  # "sizes" | "offsets" | None
    struct_re = re.compile(r"\bstruct\b.*?\b(FF_[A-Z0-9_]+)\b\s*:\s*DATA_BLOCK")

    for line in text.splitlines():
        m = struct_re.search(line)
        if m:
            current = m.group(1)
            out.setdefault(current, {"order": [], "sizes": {}, "header_sizes": {}})
            continue
        if current is None:
            continue
        if "vtable_sizes" in line:
            mode = "sizes"
            continue
        if "vtable_offsets" in line:
            mode = "offsets"
            continue
        if "}" in line:
            mode = None
            continue

        if mode == "sizes":
            sm = _SIZE_ENTRY.search(line)
            if sm:
                out[current]["sizes"][sm.group(1)] = sm.group(2)
        elif mode == "offsets":
            hm = _HEADER_SIZE.search(line)
            if hm:
                out[current]["header_sizes"][hm.group(1)] = int(hm.group(2))
                continue
            fm = _OFFSET_FIELD.match(line)
            if fm and fm.group(1) not in _NON_FIELD:
                out[current]["order"].append(fm.group(1))
    return out


def witness(generated_dir: Path) -> dict:
    """Build the full wire-format witness for a generated_src/ directory."""
    out: dict = {"tags": {}, "codes": {}, "vtables": {}}
    for p in sorted(generated_dir.rglob("*.hpp")):
        text = p.read_text(encoding="utf-8")
        for name, val in _TAG.findall(text):
            out["tags"][name] = int(val, 0)
        for name, val in _CODE.findall(text):
            out["codes"][name] = int(val, 0)
        if p.name.endswith("_internal.hpp"):
            for block, layout in _vtable_layout(text).items():
                out["vtables"][block] = layout
    return out


def dump(generated_dir: Path, dest: Path) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_text(
        json.dumps(witness(generated_dir), indent=2, sort_keys=True),
        encoding="utf-8",
    )


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: wire_witness.py <generated_dir> <out.json>")
    dump(Path(sys.argv[1]), Path(sys.argv[2]))
    print(f"wire witness written: {sys.argv[2]}")
