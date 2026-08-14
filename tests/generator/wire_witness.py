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
  * dictionary codes     — the permanent uint32 IDs in dictionaries/FF_Codes.hpp,
                           scoped by terminology source then CodeSystem
                           (`UCUM::PERCENT`, `FHIR::FDI_SURFACE::B`)

Two of these three live OUTSIDE generated_src/ — see witness() — which is why the
`tags` and `codes` sections witnessed empty dicts for so long.

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

# --- dictionary codes: `FF_CODE_DEF PERCENT = 2;` inside nested scopes ---
# The old pattern here matched `FF_R5_CODE_PERCENT = 0x1CF1F3BB` -- a flat,
# hash-based, revision-prefixed scheme that no longer exists. Names are now
# scoped by terminology source then CodeSystem and IDs are sequential ledger
# entries, so nothing matched and `codes` witnessed an empty dict.
_CODE_DEF = re.compile(r"\bFF_CODE_DEF\s+([A-Za-z_]\w*)\s*=\s*(\d+)")

# --- a `namespace X {` or `struct X {` scope opener in FF_Codes.hpp ---
_SCOPE_OPEN = re.compile(r"^\s*(?:namespace|struct)\s+([A-Za-z_][\w:]*)")

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


def _dictionary_codes(text: str, root: str = "FastFHIR::FF_CODE") -> dict[str, int]:
    """Parse `dictionaries/FF_Codes.hpp` into {qualified name: permanent ID}.

    Constants are scoped by terminology source then CodeSystem, so the name must
    be qualified to be unique: `UCUM::PERCENT`, `FHIR::FDI_SURFACE::B`. Scopes
    are a mix of `namespace` and `struct`, so this tracks brace depth rather than
    assuming one form. The enclosing `FastFHIR::FF_CODE` is dropped as noise.
    """
    out: dict[str, int] = {}
    stack: list[tuple[int, str]] = []
    depth = 0
    for line in text.splitlines():
        s = line.strip()
        m = _SCOPE_OPEN.match(s)
        if m and "{" in s:
            depth += s.count("{") - s.count("}")
            stack.append((depth, m.group(1)))
            continue
        cm = _CODE_DEF.search(s)
        if cm:
            names = [n for _, n in stack if n != root]
            out["::".join([*names, cm.group(1)])] = int(cm.group(2))
        depth += s.count("{") - s.count("}")
        while stack and stack[-1][0] > depth:
            stack.pop()
    return out


def witness(
    generated_dir: Path,
    *,
    recovery_header: Path | None = None,
    codes_header: Path | None = None,
) -> dict:
    """Build the full wire-format witness.

    The three permanent-constant families live in THREE different trees, which is
    why this takes more than `generated_dir`:

      * vtable layouts   -> generated_src/*_internal.hpp   (regenerated)
      * recovery tags    -> include/FF_Recovery.hpp        (generated, committed)
      * dictionary codes -> generated_src/FF_Codes.hpp     (regenerated)

    Both generated trees are projections of committed ledgers in dictionaries/
    (master_tags.json, master_codes.json), so witnessing the emitted C++ checks
    the ledger AND the emitter that projects it -- a drift in either shows up.

    Scanning only `generated_dir` is exactly why the `tags` and `codes` sections
    of the golden were empty dicts, and had been passing `{} == {}` ever since:
    tags are only *referenced* in generated_src (never defined with a value), and
    FF_Codes.hpp is not under generated_src at all. 165 tags and 5,796 code IDs --
    the constants that decode every archive ever written -- had no regression
    protection whatsoever. See TASKS.md A15.
    """
    repo_root = Path(__file__).resolve().parents[2]
    recovery_header = recovery_header or repo_root / "include" / "FF_Recovery.hpp"
    # FF_Codes.hpp is generator output now, so take it from the tree being
    # witnessed rather than a fixed repo path — that is what makes the gate work
    # against a freshly regenerated tree in a tmpdir.
    codes_header = codes_header or generated_dir / "FF_Codes.hpp"

    out: dict = {"tags": {}, "codes": {}, "vtables": {}}
    for p in sorted(generated_dir.rglob("*.hpp")):
        text = p.read_text(encoding="utf-8")
        if p.name.endswith("_internal.hpp"):
            for block, layout in _vtable_layout(text).items():
                out["vtables"][block] = layout

    if recovery_header.is_file():
        for name, val in _TAG.findall(recovery_header.read_text(encoding="utf-8")):
            out["tags"][name] = int(val, 0)
    if codes_header.is_file():
        out["codes"] = _dictionary_codes(codes_header.read_text(encoding="utf-8"))
    return out


def _check_permanence(current_raw: dict, existing_raw: dict, label: str) -> list[str]:
    """Return a list of violations where an existing key changed or was deleted.

    *additions* (key in current but not in existing) are allowed.
    *changes* (same key, different value) or *deletions* (key in existing but
    not in current) are wire-format breaks and must be rejected.

    For *vtables*, drills into per-block ``order``, ``sizes``, and
    ``header_sizes`` so the error message pinpoints the exact field.
    """
    errors: list[str] = []
    for key, old_val in existing_raw.items():
        if key not in current_raw:
            errors.append(
                f"  DELETED {label}.{key} — removal changes the wire format for existing streams"
            )
            continue
        new_val = current_raw[key]
        if label == "vtables":
            # Drill into per-block sub-structures
            for sub in ("order", "sizes", "header_sizes"):
                old_sub = (
                    old_val.get(sub, {})
                    if isinstance(old_val.get(sub), dict)
                    else old_val.get(sub, [])
                )
                new_sub = (
                    new_val.get(sub, {})
                    if isinstance(new_val.get(sub), dict)
                    else new_val.get(sub, [])
                )
                if isinstance(old_sub, list):
                    # Prefix rule, not equality: the shipped golden field order
                    # must sit at the head of the current order. Appending a
                    # field is legal growth (FastFHIR models 28 of ~145 R4
                    # resources); reordering, inserting, or removing a shipped
                    # field shifts offsets for existing streams.
                    if new_sub[: len(old_sub)] != old_sub:
                        errors.append(
                            f"  CHANGED {label}.{key}.{sub}: shipped fields "
                            f"reordered or removed → wire offsets shift for "
                            f"existing streams (appending fields is legal)"
                        )
                else:
                    for k, v in old_sub.items():
                        if k not in new_sub:
                            errors.append(
                                f"  DELETED {label}.{key}.{sub}.{k} — wire constant removed"
                            )
                        elif new_sub[k] != v:
                            errors.append(
                                f"  CHANGED {label}.{key}.{sub}.{k}: {v!r} → {new_sub[k]!r}"
                                f" — wire constants are permanent"
                            )
        else:
            if new_val != old_val:
                errors.append(
                    f"  CHANGED {label}.{key}: {old_val!r} → {new_val!r}"
                    f" — wire constants are permanent"
                )
    return errors


def dump(generated_dir: Path, dest: Path, *, force: bool = False) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    current_raw = witness(generated_dir)
    current = json.dumps(current_raw, indent=2, sort_keys=True)

    if dest.exists():
        existing_raw = json.loads(dest.read_text(encoding="utf-8"))
        existing = json.dumps(existing_raw, indent=2, sort_keys=True)

        if existing == current:
            print(f"  wire witness unchanged — {dest.name} already matches.")
            print(f"  (no need to update: the gate would pass with the existing golden.)")
            return

        # Check permanence before allowing any update
        errors: list[str] = []
        for section in ("tags", "codes", "vtables"):
            errors.extend(_check_permanence(current_raw[section], existing_raw[section], section))
        if errors and not force:
            for err in errors:
                print(err)
            raise SystemExit(
                f"\nERROR: {len(errors)} wire constant(s) above changed or were deleted.\n"
                "Permanent wire constants cannot be modified once committed — they\n"
                "decode every .ffhr archive already written.\n\n"
                "If this is genuinely intentional (a pre-release re-cut, where no\n"
                "archive exists anywhere), re-run with --force. Record WHY in the\n"
                "same commit: the header's own comment and TASKS.md, not just the\n"
                "commit message."
            )
        if errors:
            # --force: the override is real, so make it impossible to perform by
            # accident or to miss in review.
            print(f"  !! --force: OVERRIDING {len(errors)} permanence violation(s) !!")
            for err in errors:
                print(err)
            print(
                "  Every archive written with the previous constants is now\n"
                "  undecodable. This is only safe pre-release."
            )

        print(f"  wire witness CHANGED — {dest.name} needs updating.")
        import difflib

        for line in difflib.unified_diff(
            existing.splitlines(),
            current.splitlines(),
            fromfile="existing",
            tofile="new",
            lineterm="",
        ):
            print(line)

    # New file or content changed — write the updated golden
    dest.write_text(current, encoding="utf-8")
    print(f"  wire witness written: {dest}")


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if a != "--force"]
    force = "--force" in sys.argv[1:]
    if len(args) != 2:
        raise SystemExit("usage: wire_witness.py [--force] <generated_dir> <out.json>")
    dump(Path(args[0]), Path(args[1]), force=force)
