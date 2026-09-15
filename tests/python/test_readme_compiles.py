#!/usr/bin/env python3
"""Every ```cpp block in README.md must compile.

WHY THIS EXISTS
---------------
`tests/cpp/test_readme.cpp` is a hand-maintained parallel implementation of the
README's numbered examples -- not an extraction of them. It proves the examples
WORK; it cannot prove the README's own bytes are valid, because it never reads
them. So the two drifted, and `ctest` stayed green the whole way: every C++
block in the README was calling a `FastFHIR::Builder` / `Ingest::Ingestor` API
that no longer exists, alongside three defects a rename cannot explain --
`builder.mutable_handle()` (never existed anywhere), `RECOVERY_TAG::Patient`
(an unscoped enum, so the qualification is invalid), and a concurrency example
that transformed a variable it never declared.

This gate closes that by compiling the README itself. It is deliberately NOT a
runtime test: a doc block cannot be run without inventing fixtures for it, and
`-fsyntax-only` already catches the entire class above at a fraction of the
cost. Runtime behaviour stays `test_readme.cpp`'s job. Two different claims,
both now checked:

    test_readme.cpp        the examples work
    this gate              the README says something that compiles

HOW A BLOCK DECLARES ITSELF
---------------------------
With an HTML comment above the fence -- invisible in rendered Markdown:

    <!-- ff-compile: program -->            top-level declarations; compiled as a TU
    <!-- ff-compile: fragment -->           statements; wrapped in a function (DEFAULT)
    <!-- ff-compile: expressions -->        an identifier listing; each line
                                            checked as an expression
    <!-- ff-compile: skip reason="..." -->  excluded, and the reason is mandatory
    <!-- ff-compile: needs=arena,parser --> inject context stanzas (tests/readme/context.hpp)

`expressions` is for the reference listings -- `FastFHIR::Fields::PATIENT::ID
// "id"` and friends. They are not statements, but the identifiers must still
exist, and a listing is exactly where a renamed constant rots unnoticed.

P0-2: this gate asserts a non-zero floor and a skip ceiling. A run that
compiles zero blocks, or that has quietly grown its skip list, fails.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

_HERE = Path(__file__).resolve().parent
_REPO_ROOT = _HERE.parent.parent
sys.path.insert(0, str(_REPO_ROOT / "tests" / "readme"))

from extract import Fence, extract  # noqa: E402

# ── P0-2 floors ──────────────────────────────────────────────────────────────
# A gate that returns zero results must not pass. These are floors, not
# expected values: raise them when the README grows, and never lower one to
# make a red run green -- that is the failure mode they exist to prevent.
MIN_BLOCKS_COMPILED = 20
MAX_BLOCKS_SKIPPED = 2


def _shared_preamble() -> str:
    """Includes every wrapper gets, whatever the block itself pulls in.

    A block's own `#include` lines are hoisted and kept -- they are part of
    what the README claims. These are the ones a reader has from context: the
    umbrella header, the generated POD structs and field keys, and the standard
    library a code sample uses without ceremony.
    """
    return "\n".join([
        '#include <FastFHIR.hpp>',
        '#include "FF_AllTypes.hpp"',    # PatientData, ObservationData, QuantityData, ...
        '#include "FF_FieldKeys.hpp"',   # FastFHIR::Fields::<RESOURCE>::<FIELD>
        '#include "context.hpp"',        # FF_README_CTX_* stanzas
        "#include <cstdint>",
        "#include <iostream>",
        "#include <memory>",
        "#include <sstream>",
        "#include <stdexcept>",
        "#include <string>",
        "#include <string_view>",
        "#include <thread>",
        "#include <vector>",
    ])


def _strip_trailing_comment(line: str) -> str:
    """Drop a trailing `// ...` so an identifier listing becomes an expression.

    Only splits outside a string literal, so a `//` inside a URL survives.
    """
    in_str = False
    escaped = False
    for i, ch in enumerate(line):
        if escaped:
            escaped = False
            continue
        if ch == "\\":
            escaped = True
        elif ch == '"':
            in_str = not in_str
        elif ch == "/" and not in_str and line[i + 1:i + 2] == "/":
            return line[:i]
    return line


# Optional toolchain features a block may declare with `requires=`. Each maps to
# a header that must be reachable AND the macro that gates the FastFHIR side of
# it. This is how a block documenting an opt-in subsystem stays CHECKED wherever
# that subsystem is built, instead of being skipped everywhere forever.
_FEATURES: dict[str, tuple[str, str]] = {
    # FF_Extensions.hpp is entirely inside `#ifdef FASTFHIR_ENABLE_EXTENSIONS`
    # (FF_Extensions.hpp:25) and includes WAMR's <wasm_export.h>. The CMake
    # option defaults OFF (CMakeLists.txt:85), so a default build has neither.
    "extensions": ("wasm_export.h", "FASTFHIR_ENABLE_EXTENSIONS"),
}


def _feature_available(name: str, cxx: str, include_dirs: list[str]) -> bool:
    """True when the header behind an optional feature is on the include path."""
    header, _macro = _FEATURES[name]
    with tempfile.TemporaryDirectory() as td:
        probe = Path(td) / "probe.cpp"
        probe.write_text(f"#include <{header}>\nint main(){{}}\n", encoding="utf-8")
        cmd = [cxx, "-std=c++20", "-fsyntax-only"]
        for d in include_dirs:
            cmd += ["-I", d]
        cmd.append(str(probe))
        return subprocess.run(cmd, capture_output=True, text=True).returncode == 0


def _context_stanzas(fence: Fence) -> list[str]:
    return [f"    FF_README_CTX_{n.upper()}" for n in fence.needs]


def build_tu(fence: Fence) -> str:
    """Render one fence as a compilable translation unit."""
    parts = [
        f"// README.md:{fence.line} -- {fence.heading}",
        f"// mode={fence.mode} needs={','.join(fence.needs) or '-'}",
        _shared_preamble(),
        *fence.includes,
        "",
    ]

    if fence.mode == "program":
        # Top-level declarations are the point; emit the body at file scope.
        parts += fence.code
    elif fence.mode == "expressions":
        parts.append(f"static void ff_readme_fence_{fence.index}() {{")
        parts += _context_stanzas(fence)
        for raw in fence.code:
            expr = _strip_trailing_comment(raw).strip()
            if not expr:
                continue
            parts.append(f"    (void)sizeof({expr});")
        parts.append("}")
    else:  # fragment
        parts.append(f"static void ff_readme_fence_{fence.index}() {{")
        parts += _context_stanzas(fence)
        parts += [f"    {ln}" if ln.strip() else ln for ln in fence.code]
        parts.append("}")

    return "\n".join(parts) + "\n"


def compile_one(fence: Fence, cxx: str, include_dirs: list[str],
                keep_dir: Path | None = None) -> tuple[bool, str]:
    tu = build_tu(fence)
    with tempfile.TemporaryDirectory() as td:
        src = Path(td) / f"ff_readme_fence_{fence.index}.cpp"
        src.write_text(tu, encoding="utf-8")
        cmd = [cxx, "-std=c++20", "-fsyntax-only", "-DASIO_STANDALONE"]
        for feature in fence.requires:
            cmd.append(f"-D{_FEATURES[feature][1]}")
        for d in include_dirs:
            cmd += ["-I", d]
        cmd.append(str(src))
        proc = subprocess.run(cmd, capture_output=True, text=True)
        if proc.returncode != 0 and keep_dir is not None:
            keep_dir.mkdir(parents=True, exist_ok=True)
            kept = keep_dir / src.name
            kept.write_text(tu, encoding="utf-8")
            return False, proc.stderr + f"\n[wrapper kept at {kept}]"
        return proc.returncode == 0, proc.stderr


def _cmake_cache_var(build_dir: Path, name: str) -> str | None:
    """Read one variable out of an existing CMakeCache.txt.

    The gate must compile against the SAME third-party headers the build uses.
    Hardcoding `/opt/homebrew/include` would pass on one machine and fail on
    the next, and guessing a list of prefixes is the same bug with more steps --
    so the answer comes from the build that configured this tree. When CMake
    invokes the gate it passes these as --include-dir instead, and this path is
    only the standalone fallback.
    """
    cache = build_dir / "CMakeCache.txt"
    if not cache.is_file():
        return None
    prefix = f"{name}:"
    for line in cache.read_text(encoding="utf-8", errors="replace").split("\n"):
        if line.startswith(prefix) and "=" in line:
            return line.split("=", 1)[1].strip() or None
    return None


def _default_include_dirs(build_dir: Path) -> list[str]:
    """Include path for the gate.

    `generated_src/` is required: the README uses `PatientData` and
    `FastFHIR::Fields::...`, neither of which exists until the generator has
    run. simdjson and OpenSSL are needed because `FF_Ingestor.hpp` includes the
    first and four blocks include `<openssl/sha.h>` for the finalize hasher.
    """
    dirs = [
        _REPO_ROOT / "include",
        _REPO_ROOT / "generated_src",
        _REPO_ROOT / "tests" / "readme",
    ]
    for candidate in (
        build_dir / "_deps" / "simdjson-src" / "include",
        build_dir / "_deps" / "simdjson-src" / "singleheader",
        # The socket example includes <asio.hpp>; tests/tests.cmake fetches it
        # here when the system has no copy (tests.cmake:38-45).
        build_dir / "_deps" / "asio-src" / "asio" / "include",
    ):
        if candidate.is_dir():
            dirs.append(candidate)
    ssl = _cmake_cache_var(build_dir, "OPENSSL_INCLUDE_DIR")
    if ssl and Path(ssl).is_dir():
        dirs.append(Path(ssl))
    return [str(d) for d in dirs]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--readme", default=str(_REPO_ROOT / "README.md"))
    ap.add_argument("--build-dir", default=str(_REPO_ROOT / "build"))
    ap.add_argument("--cxx", default=os.environ.get("CXX") or "c++")
    ap.add_argument("--include-dir", action="append", default=[],
                    help="extra -I directory (repeatable)")
    ap.add_argument("--only", type=int, action="append", default=[],
                    help="compile only these fence indices (debugging)")
    ap.add_argument("--dump", type=int, default=None,
                    help="print the generated wrapper for one fence and exit")
    ap.add_argument("--keep-failed", default=None,
                    help="directory to write the wrapper of any failing block")
    args = ap.parse_args()

    cxx = shutil.which(args.cxx)
    if cxx is None:
        # Not a pass. A missing compiler means the gate did not run, and
        # "did not run" must never read as "passed" (P0-2).
        print(f"README compile gate: no C++ compiler found (tried {args.cxx!r}).")
        print("Set CXX or pass --cxx. Refusing to report success on zero blocks.")
        return 2

    readme = Path(args.readme)
    fences = extract(readme)

    if args.dump is not None:
        for f in fences:
            if f.index == args.dump:
                print(build_tu(f))
                return 0
        print(f"no fence {args.dump}; README has {len(fences)}")
        return 2

    include_dirs = _default_include_dirs(Path(args.build_dir)) + args.include_dir
    generated = _REPO_ROOT / "generated_src" / "FF_FieldKeys.hpp"
    if not generated.is_file():
        print(f"README compile gate: {generated} is missing.")
        print("Configure with the generator first (see CLAUDE.md 'Build & test');")
        print("the README's blocks name generated types and cannot compile without it.")
        return 2

    keep_dir = Path(args.keep_failed) if args.keep_failed else None
    compiled = skipped = 0
    failures: list[tuple[Fence, str]] = []
    skips: list[Fence] = []
    unavailable: list[tuple[Fence, str]] = []
    feature_cache: dict[str, bool] = {}

    for fence in fences:
        if args.only and fence.index not in args.only:
            continue

        if fence.mode == "skip":
            if not fence.reason:
                failures.append((fence, "mode=skip with no reason= given; a "
                                        "skipped block must say why"))
                continue
            skipped += 1
            skips.append(fence)
            print(f"  SKIP  {fence.label}: {fence.reason}")
            continue

        # A block gated on an optional subsystem is compiled wherever that
        # subsystem's headers exist and reported as unavailable where they do
        # not. Deliberately NOT counted as a skip: a skip is a decision to stop
        # checking, and this is the environment answering. It is still printed,
        # because "did not run" must never be invisible.
        missing = [f for f in fence.requires
                   if not feature_cache.setdefault(
                       f, _feature_available(f, cxx, include_dirs))]
        if missing:
            why = ", ".join(f"{m} (<{_FEATURES[m][0]}> not on the include path)"
                            for m in missing)
            unavailable.append((fence, why))
            print(f"  n/a   {fence.label}: requires {why}")
            continue

        ok, err = compile_one(fence, cxx, include_dirs, keep_dir)
        if ok:
            compiled += 1
            print(f"  ok    {fence.label} [{fence.mode}]")
        else:
            failures.append((fence, err))
            print(f"  FAIL  {fence.label} [{fence.mode}]")

    print()
    print(f"README compile gate: {compiled} compiled, {skipped} skipped, "
          f"{len(unavailable)} unavailable, {len(failures)} failed, "
          f"of {len(fences)} ```cpp blocks.")
    if unavailable:
        print("  Unavailable blocks are checked in a build that has the feature;")
        print("  configure with it enabled to cover them here:")
        for fence, why in unavailable:
            print(f"    {fence.label}: {why}")

    for fence, err in failures:
        print()
        print("=" * 72)
        print(f"FAILED: {fence.label} [mode={fence.mode}]")
        print("=" * 72)
        errs = [ln for ln in err.split("\n")
                if ": error:" in ln or ": fatal error:" in ln] or err.split("\n")
        for ln in errs[:12]:
            print("  " + ln.strip())
        print()
        print(f"  The block is at README.md:{fence.line}. Reproduce with:")
        print(f"    python3 tests/python/test_readme_compiles.py --only {fence.index}")
        print(f"    python3 tests/python/test_readme_compiles.py --dump {fence.index}")

    # ── P0-2: the floors ─────────────────────────────────────────────────────
    verdict = 0
    if failures:
        verdict = 1
    if not args.only:
        if compiled < MIN_BLOCKS_COMPILED:
            print()
            print(f"FLOOR BREACH: only {compiled} blocks compiled, floor is "
                  f"{MIN_BLOCKS_COMPILED}. A gate that checks almost nothing "
                  f"passes for the wrong reason -- either blocks were removed "
                  f"from the README (lower the floor deliberately, in the same "
                  f"commit) or extraction is broken.")
            verdict = 1
        if skipped > MAX_BLOCKS_SKIPPED:
            print()
            print(f"SKIP CEILING BREACH: {skipped} blocks skipped, ceiling is "
                  f"{MAX_BLOCKS_SKIPPED}. Skips are how this gate decays. Fix "
                  f"the block or raise the ceiling on purpose:")
            for f in skips:
                print(f"    {f.label}: {f.reason}")
            verdict = 1

    if verdict == 0:
        print(f"OK -- every one of {compiled} README C++ blocks compiles.")
    return verdict


if __name__ == "__main__":
    sys.exit(main())
