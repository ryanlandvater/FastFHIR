"""Group every round-trip failure by signature, with the wire cause attached."""

from __future__ import annotations

import json, re, subprocess, sys
from collections import defaultdict
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tests" / "python"))
from roundtrip_diff import diff_doms, filter_allowlisted  # noqa: E402
from roundtrip_debug import strip_debug, drop_debug_artifacts, _find_siblings  # noqa: E402

H = str(REPO / "build" / "ff_roundtrip")
IDX = re.compile(r"/\d+")


def one(p: Path):
    r = subprocess.run(
        [H, str(p), "--arena-size", "2147483648", "--debug"],
        capture_output=True,
        text=True,
        timeout=600,
    )
    if r.returncode != 0:
        return p.name, [("HARNESS", f"exit {r.returncode}", "", None, None)], ""
    dom, meta = strip_debug(json.loads(r.stdout))
    diffs = drop_debug_artifacts(
        filter_allowlisted(diff_doms(json.loads(p.read_text()), dom)), meta
    )
    rows = []
    for d in diffs:
        m = meta.get(d.meta_path())
        sib = None
        if m is None:
            hits = _find_siblings(d.path, meta)
            if hits:
                sib = (
                    hits[0][0],
                    hits[0][1].get("_tag"),
                    hits[0][1].get("_kind"),
                    hits[0][1].get("_off"),
                )
        rows.append((d.kind.name, IDX.sub("/N", d.path), d.path, (m or {}).get("_tag"), sib))
    return p.name, rows, r.stderr.strip()[:200]


def main() -> int:
    fx = sorted((REPO / "build" / "synthea_fhir_r4").glob("*.json"))
    groups = defaultdict(
        lambda: {
            "n": 0,
            "fixtures": set(),
            "examples": [],
            "per_pass": defaultdict(set),
            "counts": defaultdict(int),
        }
    )
    warn = defaultdict(int)
    clean_per_pass = []
    PASSES = 2
    # Ingest is load-sensitive (TASKS.md AR-3), so a single pass invents
    # signatures that vanish on the next run. Two passes; anything absent from
    # one of them is flagged rather than presented as a defect to go fix.
    for pass_i in range(PASSES):
        clean = 0
        with ProcessPoolExecutor() as pool:
            for name, rows, stderr in pool.map(one, fx):
                if not rows:
                    clean += 1
                if pass_i == 0:
                    for w in re.findall(r"(\w+) x\d+", stderr):
                        warn[w] += 1
                for kind, npath, path, tag, sib in rows:
                    key = (kind, npath, tag, sib[0] if sib else None)
                    g = groups[key]
                    g["per_pass"][pass_i].add(name)
                    g["counts"][pass_i] += 1
                    if pass_i == 0:
                        g["n"] += 1
                        g["fixtures"].add(name)
                        if len(g["examples"]) < 3:
                            g["examples"].append((name, path, sib))
        clean_per_pass.append(clean)
    clean = clean_per_pass[0]
    failed = len(fx) - clean
    # Reproducible means the SAME fixtures fail the same way, not merely that
    # the signature appeared again somewhere. AR-3 always corrupts some fixture,
    # so an "appeared in both passes" test marks its artifacts stable -- which it
    # did, wrongly, on the first version of this report.
    STABLE = {
        k for k, g in groups.items() if g["per_pass"][0] and g["per_pass"][0] == g["per_pass"][1]
    }

    out = [
        "# Round-trip failures, explained by `to_debug_json`",
        "",
        f"Generated {subprocess.run(['date','+%Y-%m-%d %H:%M'],capture_output=True,text=True).stdout.strip()}",
        f" from {len(fx)} Synthea fixtures via `ff_roundtrip --debug`.",
        "",
        f"- **{clean} / {len(fx)} fixtures clean** in pass 1, {failed} failing "
        f"(pass 2: {clean_per_pass[1]} clean)",
        f"- **{sum(g['n'] for g in groups.values()):,} total differences** in "
        f"**{len(groups)} distinct signatures**",
        "",
        "A *signature* is (diff kind, path with array indices collapsed to `/N`,",
        "recovery tag, sibling key actually emitted). Fixing one signature fixes",
        "every instance of it, so this table is the work list — not the failing files.",
        "",
        "> ### Read the STABLE column first",
        ">",
        "> Ingest is load-sensitive (TASKS.md **AR-3**): the same fixture keeps a",
        "> different set of resources run to run. So this report runs the whole",
        "> corpus **twice** and marks whether a signature appeared in both passes.",
        ">",
        "> **`PHANTOM` rows are AR-3 manufacturing a signature — not defects to",
        "> go fix.** They are the largest rows by diff count, which is exactly why",
        "> a single-pass report is misleading: it invents huge phantom work items.",
        "> `jitter` rows ARE real defects whose fixture set moves at the margins.",
        "> Reproducible means the **same fixtures** fail the same way in both",
        "> passes — not merely that the signature reappeared somewhere, which it",
        "> always does while AR-3 is live.",
        "",
        "## Signatures, most instances first",
        "",
        "| # | Stable | Diffs | Fixtures | Kind | Path | Wire cause |",
        "|--:|:------:|------:|---------:|------|------|------------|",
    ]
    ranked = sorted(groups.items(), key=lambda kv: -kv[1]["n"])
    for i, ((kind, npath, tag, sibkey), g) in enumerate(ranked, 1):
        if sibkey:
            cause = f"emitted as `{sibkey}` — tag `{tag or '—'}`"
        elif tag:
            cause = f"tag `{tag}`"
        else:
            cause = "no node at this path (absent from stream)"
        key = (kind, npath, tag, sibkey)
        if key in STABLE:
            mark = "**yes**"
        else:
            c0, c1 = g["counts"][0], g["counts"][1]
            f0, f1 = len(g["per_pass"][0]), len(g["per_pass"][1])
            common = len(g["per_pass"][0] & g["per_pass"][1])
            mark = (
                "_jitter_" if common >= 0.8 * max(f0, f1) else "**PHANTOM**"
            ) + f" ({common}/{f0} fx)"
        out.append(
            f"| {i} | {mark} | {g['n']:,} | {len(g['fixtures'])} | {kind} | `{npath}` | {cause} |"
        )

    out += ["", "## Detail", ""]
    for i, ((kind, npath, tag, sibkey), g) in enumerate(ranked, 1):
        stable = (kind, npath, tag, sibkey) in STABLE
        g0, g1 = g["per_pass"][0], g["per_pass"][1]
        out += [
            f"### {i}. {kind} — `{npath}`",
            "",
            f"**{g['n']:,} diffs across {len(g['fixtures'])} fixtures.** "
            + (
                "Reproduced in both passes."
                if stable
                else (
                    f"**MOSTLY REPRODUCIBLE — {len(g0 & g1)} of {len(g0)} fixtures "
                    f"fail this way in both passes ({len(g0)} vs {len(g1)} total). "
                    f"The defect is real; AR-3 only jitters which fixtures show it.**"
                    if len(g0 & g1) >= 0.8 * max(len(g0), len(g1))
                    else f"**PHANTOM — only {len(g0 & g1)} of {len(g0)} fixtures fail this "
                    f"way in both passes ({len(g0)} vs {len(g1)} total). This is "
                    f"AR-3 (load-sensitive ingest) manufacturing a signature, not a "
                    f"defect at this path.**"
                )
            ),
            "",
        ]
        if sibkey:
            out += [
                f"The reader emitted **`{sibkey}`** where the source has the key above. "
                f"The value survived; the tag renamed the field.",
                "",
            ]
        elif tag:
            out += [f"Wire node present, recovery tag **`{tag}`**.", ""]
        else:
            out += [
                "No node at this path — the field is absent from the stream, "
                "so this is an ingest/write-path loss, not a reader mislabel.",
                "",
            ]
        out.append("Examples:")
        out.append("")
        for name, path, sib in g["examples"]:
            line = f"- `{path}` in `{name[:44]}`"
            if sib:
                line += f" → emitted `{sib[0]}` tag=`{sib[1]}` kind=`{sib[2]}` off=`{sib[3]}`"
            out.append(line)
        out.append("")

    if warn:
        out += [
            "## Ingest warnings (out-of-profile discards)",
            "",
            "| Resource type | Fixtures reporting it |",
            "|---|--:|",
        ]
        for k, v in sorted(warn.items(), key=lambda kv: -kv[1]):
            out.append(f"| {k} | {v} |")
        out.append("")

    dest = REPO / "build" / "roundtrip_failures.md"
    dest.write_text("\n".join(out))
    print(f"wrote {dest} ({dest.stat().st_size:,} bytes, {len(groups)} signatures)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
