"""Detect a test suite that writes into the working tree.

TASKS.md GEN-1.5. This exists because a single test did exactly that for two
days and it was mistaken for a generator flake.

`tests/generator/test_code_ids.py` ran `python -m generator` with no
`--output-dir`, so it regenerated the repo's own `generated_src/` as a side
effect -- and with `FASTFHIR_PRODUCTION_PROFILE` unset it did so at the
`CMakeLists.txt` default of `us-core`, whatever profile the tree had been built
with. A developer on `us-core,billing,...` would build cleanly, run
`pytest tests/generator`, and be left with a 72-enum `FF_CodeSystems.hpp` beside
billing sources that `write_if_changed` had left untouched at an older mtime.
The next build then died on `unknown type name 'FF_Use'` in generated code
nobody had regenerated, and the next `cmake` configure silently repaired it.

The bug was one missing argument. What made it cost two days was that **nothing
asserted the property it violated**: running the tests must not modify the tree.
So assert it.

Protected paths and why:

  generated_src/   the victim above -- and every emitter writes here by default,
                   so it is one forgotten flag away from happening again.
  dictionaries/    the PERMANENT WIRE LEDGERS. A test that rewrites
                   master_codes.json can silently renumber constants that decode
                   stored archives; that has happened once already (118d6ad).

Content hashes, not mtimes: `write_if_changed` deliberately leaves an unchanged
file's mtime alone, so mtimes would miss a same-timestamp rewrite and, worse,
would report a false violation for a no-op regeneration. The whole tree hashes
in well under a tenth of a second, so there is no reason to approximate.
"""

from __future__ import annotations

import hashlib
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]

# Relative to REPO_ROOT. A missing directory is not an error -- `generated_src/`
# does not exist on a clean checkout -- but a directory that APPEARS during the
# session is a violation, and comparing the two mappings catches that for free.
PROTECTED = ("generated_src", "dictionaries")


def fingerprint() -> dict[str, str]:
    """Map every protected file to the SHA-256 of its contents."""
    out: dict[str, str] = {}
    for rel in PROTECTED:
        root = REPO_ROOT / rel
        if not root.is_dir():
            continue
        for path in sorted(root.rglob("*")):
            if not path.is_file():
                continue
            out[str(path.relative_to(REPO_ROOT))] = hashlib.sha256(
                path.read_bytes()
            ).hexdigest()
    return out


def diff(before: dict[str, str], after: dict[str, str]) -> list[str]:
    """Human-readable list of what the session changed, newest concern first."""
    changed = sorted(k for k in before.keys() & after.keys() if before[k] != after[k])
    added = sorted(after.keys() - before.keys())
    removed = sorted(before.keys() - after.keys())
    return (
        [f"MODIFIED {p}" for p in changed]
        + [f"CREATED  {p}" for p in added]
        + [f"DELETED  {p}" for p in removed]
    )


def format_violation(entries: list[str]) -> str:
    """The first 20 entries, with a count of any remainder."""
    listed = "\n".join(f"    {e}" for e in entries[:20])
    if len(entries) > 20:
        listed += f"\n    ... and {len(entries) - 20} more"
    return listed
