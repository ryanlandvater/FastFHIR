"""Aggregate round-trip diffs across every Synthea fixture.

`py_roundtrip` needs ZERO diffs across all 342 fixtures, so a single fixture is
not a measurement -- it is a sample that has repeatedly hidden defects affecting
tens of thousands of elements elsewhere.  This bucket-by-path view is the number
that decides whether a change helped.

Run it before and after every change:

    python tests/python/roundtrip_aggregate.py             # all fixtures
    python tests/python/roundtrip_aggregate.py --limit 8   # smoke test

Paths are normalised with /\\d+/ -> /N so one defect repeated across every array
element lands in one row instead of thousands.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from collections import Counter
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from roundtrip_diff import diff_doms, filter_allowlisted  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
_INDEX = re.compile(r"/\d+")

# Set by main() before the pool forks; workers inherit it via copy-on-write
# rather than paying to pickle it per fixture.
_HARNESS = REPO / "build" / "ff_roundtrip"
_ARENA = "2147483648"


def _one(path: Path) -> tuple[str, list[tuple[str, str]], str]:
    """Round-trip one fixture and return its bucketed diffs, or an error."""
    try:
        result = subprocess.run(
            [str(_HARNESS), str(path), "--arena-size", _ARENA],
            capture_output=True,
            text=True,
            timeout=300,
        )
    except subprocess.TimeoutExpired:
        return path.name, [], "TIMEOUT"
    if result.returncode != 0:
        return path.name, [], f"EXIT {result.returncode}: {result.stderr.strip()[:200]}"
    try:
        output_dom = json.loads(result.stdout)
    except json.JSONDecodeError as exc:
        return path.name, [], f"BAD JSON: {exc}"

    diffs = filter_allowlisted(diff_doms(json.loads(path.read_text()), output_dom))
    return path.name, [(_INDEX.sub("/N", d.path), d.kind.name) for d in diffs], ""


def main() -> int:
    global _HARNESS, _ARENA

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--harness", default=str(REPO / "build" / "ff_roundtrip"))
    parser.add_argument("--fixtures", default=str(REPO / "build" / "synthea_fhir_r4"))
    parser.add_argument("--arena-size", default=_ARENA)
    parser.add_argument("--limit", type=int, help="only the first N fixtures")
    parser.add_argument("--top", type=int, default=40, help="buckets to print")
    args = parser.parse_args()

    _HARNESS, _ARENA = Path(args.harness), args.arena_size
    if not _HARNESS.is_file():
        raise SystemExit(f"harness not found: {_HARNESS} -- build ff_roundtrip first")

    fixtures = sorted(Path(args.fixtures).glob("*.json"))
    if not fixtures:
        raise SystemExit(f"no fixtures under {args.fixtures}")
    if args.limit:
        fixtures = fixtures[: args.limit]

    buckets: Counter[tuple[str, str]] = Counter()
    kinds: Counter[str] = Counter()
    errors: list[tuple[str, str]] = []
    clean = 0

    with ProcessPoolExecutor() as pool:
        for name, rows, err in pool.map(_one, fixtures):
            if err:
                errors.append((name, err))
                continue
            clean += not rows
            buckets.update(rows)
            kinds.update(kind for _, kind in rows)

    print(
        f"fixtures={len(fixtures)}  clean={clean}/{len(fixtures)}  "
        f"errors={len(errors)}  total_diffs={sum(buckets.values())}"
    )
    print("\n-- by kind --")
    for kind, count in kinds.most_common():
        print(f"{count:>8}  {kind}")
    print(f"\n-- top {args.top} buckets --")
    for (path, kind), count in buckets.most_common(args.top):
        print(f"{count:>8}  {kind:<18} {path}")
    if errors:
        print("\n-- errors --")
        for name, err in errors[:20]:
            print(f"  {name}: {err}")

    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
