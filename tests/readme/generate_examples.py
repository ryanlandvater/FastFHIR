#!/usr/bin/env python3
"""Generate a RUNNABLE C++ test out of README.md's own code blocks.

PARITY IS THE POINT
-------------------
`tests/cpp/test_readme.cpp` re-implements the README's examples by hand. That
proves the examples work and says nothing about the README, so the two drifted:
every C++ block on the page called a `FastFHIR::Builder` / `Ingest::Ingestor`
API that no longer existed while `ctest` stayed green.

A compile gate (`test_readme_compiles.py`) closes the "does it still name real
API" half. This closes the other half: the suite EXECUTES the published bytes.
A block is not copied, paraphrased, or re-implemented here -- it is emitted
verbatim into a function and run.

WHAT IS NOT VERBATIM, AND WHY
-----------------------------
Three things, all mechanical, all reported by --report:

1. `int main(` in a `program` block is renamed, because the harness owns main.
   The block's body is untouched.
2. Assertions are NOT in the README. They live in `tests/readme/expect.hpp` as
   `FF_README_EXPECT_<ID>` macros that expand at the END of the example's own
   scope, so they can read the block's locals without the block mentioning them.
   Putting `REQUIRE(...)` in the docs would be worse documentation, and putting
   the docs' code in the test by hand is what failed.
3. Fixtures come from `FF_README_SETUP_<ID>` in the same header, expanded BEFORE
   the block. The examples name relative paths (`"patient.ffhr"`), so the runner
   chdir's into the artifact directory instead of rewriting those literals.

A block opts in with `run=<id>` in its `<!-- ff-compile: ... -->` directive.
Blocks without it stay compile-only -- a listing of field-key constants has no
runtime meaning, and the socket example needs a peer the page does not show.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE))

from extract import Fence, extract  # noqa: E402

_MAIN = re.compile(r"\bint\s+main\s*\(")

_FILE_HEADER = """// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Copyright (c) 2025 Ryan Landvater
//
// ============================================================================
// GENERATED FILE -- DO NOT EDIT, DO NOT COMMIT.
//
// Produced by tests/readme/generate_examples.py from the ```cpp blocks in
// README.md. Every example body below is the README's own text, verbatim.
// To change an example, edit README.md; to change what is asserted about it,
// edit tests/readme/expect.hpp.
//
// Source: {readme}
// Blocks executed: {n_run} of {n_total}
// ============================================================================

"""


def _mangle(fence: Fence) -> str:
    return f"ff_readme_{fence.run_id}"


def render(fences: list[Fence], readme: Path) -> tuple[str, list[Fence], list[Fence]]:
    # This harness emits one C++ translation unit, so a ```c fence has no place
    # in it. Those are compile-gated by tests/python/test_readme_compiles.py,
    # which routes each fence to the compiler its language names.
    fences = [f for f in fences if f.lang == "cpp"]
    runnable = [f for f in fences if f.run_id]
    skipped = [f for f in fences if not f.run_id]

    out: list[str] = [
        _FILE_HEADER.format(readme=readme.name, n_run=len(runnable), n_total=len(fences))
    ]

    # Harness first: expect.hpp pulls in the shared CHECK/REQUIRE macros and
    # every FastFHIR header the examples need, so a block's own #include lines
    # are additive rather than load-bearing.
    out.append('#include "expect.hpp"')
    out.append("")

    # Then each block's own includes, hoisted and de-duplicated in first-seen
    # order. They are part of what the README claims, so they are kept even
    # when expect.hpp already covers them.
    seen: set[str] = set()
    includes: list[str] = []
    for f in runnable:
        for inc in f.includes:
            key = inc.strip()
            if key not in seen:
                seen.add(key)
                includes.append(key)
    out += includes
    out.append("")

    for f in runnable:
        name = _mangle(f)
        upper = f.run_id.upper()
        out.append("// " + "-" * 74)
        out.append(f"// README.md:{f.line} -- {f.heading}")
        out.append(f"// mode={f.mode} run={f.run_id}")
        out.append("// " + "-" * 74)

        body = [ln for ln in f.code if not ln.startswith("#include")]

        if f.mode == "program":
            # File-scope declarations are the point of a `program` block. The
            # harness owns main(), so that one token is renamed; nothing else
            # is touched.
            renamed = [_MAIN.sub(f"int {name}_main(", ln) for ln in body]
            out += renamed
            out.append("")
            out.append(f"void {name}()")
            out.append("{")
            out.append(f"    FF_README_SETUP_{upper}")
            out.append(f"    FF_README_EXPECT_{upper}")
            out.append("}")
        else:
            out.append(f"void {name}()")
            out.append("{")
            out.append(f"    FF_README_SETUP_{upper}")
            out.append("    // ---- README.md block begins (verbatim) ----")
            out += [f"    {ln}" if ln.strip() else "" for ln in body]
            out.append("    // ---- README.md block ends ----")
            out.append(f"    FF_README_EXPECT_{upper}")
            out.append("}")
        out.append("")

    # The runner. Document order is dependency order by construction: Example 1
    # writes patient.ffhr and Example 2 reads it, in that order on the page.
    out.append("// " + "=" * 74)
    out.append("// Runner -- README document order, which is also dependency order.")
    out.append("// " + "=" * 74)
    out.append("int main(int argc, char** argv)")
    out.append("{")
    out.append("    ff_readme::init(argc, argv);")
    for f in runnable:
        out.append(f'    ff_readme::run("{f.run_id}", "README.md:{f.line}", '
                   f"{_mangle(f)});")
    out.append("    return ff_readme::report();")
    out.append("}")
    out.append("")

    return "\n".join(out), runnable, skipped


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--readme", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--report", action="store_true",
                    help="print which blocks run and which are compile-only")
    # P0-2: a generator that emits an empty runner must not be reported as a
    # success. The floor is a floor, not an expectation.
    ap.add_argument("--min-runnable", type=int, default=1)
    args = ap.parse_args()

    readme = Path(args.readme)
    fences = extract(readme)
    text, runnable, skipped = render(fences, readme)

    if len(runnable) < args.min_runnable:
        print(f"README example generator: only {len(runnable)} runnable block(s), "
              f"floor is {args.min_runnable}. Either every `run=` directive was "
              f"removed from README.md or extraction is broken; a test binary "
              f"that runs nothing passes for the wrong reason.", file=sys.stderr)
        return 1

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    # Write-if-changed: the build re-runs this on every README edit, and an
    # unchanged mtime keeps the compiler from rebuilding for nothing.
    if not out.is_file() or out.read_text(encoding="utf-8") != text:
        out.write_text(text, encoding="utf-8")

    if args.report:
        print(f"README example generator -> {out}")
        print(f"  executed  ({len(runnable)}):")
        for f in runnable:
            print(f"    {f.run_id:<22} README.md:{f.line}  {f.heading[:48]}")
        print(f"  compile-only ({len(skipped)}):")
        for f in skipped:
            print(f"    {'-':<22} README.md:{f.line}  {f.heading[:48]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
