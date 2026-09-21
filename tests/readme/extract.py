"""Extract every ```cpp and ```c fence from README.md.

The README's code blocks are the thing readers copy. Nothing compiled them
until now, which is how all six examples drifted onto a `FastFHIR::Builder` /
`Ingest::Ingestor` API that no longer exists while `ctest` stayed green --
`tests/cpp/test_readme.cpp` is a hand-maintained parallel implementation of the
same examples, not an extraction of these blocks, so it cannot witness the
README's own bytes.

Both languages are extracted because the README documents both surfaces. The
```c fence holds the C ABI example. `FastFHIR.h` is hand-written, recent, and
has no generator keeping it in step with the library, so its example is the one
most likely to fall out of date; if it were left unextracted, the C block would
rot in exactly the way the C++ blocks did before this gate existed.
`Fence.lang` records which compiler each block needs. The gate uses it to pick
between the C and C++ compilers, and the runtime harness, which emits a single
C++ translation unit, skips every fence whose lang is not cpp.

This module is the extraction half. `tests/python/test_readme_compiles.py` is
the gate.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path

FENCE_OPEN = re.compile(r"^\s*```(?P<lang>cpp|c)\s*$")
FENCE_CLOSE = re.compile(r"^\s*```\s*$")
# An HTML comment carrying gate directives. Invisible in rendered Markdown, so
# it can sit directly above the fence it configures without changing the page.
DIRECTIVE = re.compile(r"<!--\s*ff-compile:\s*(?P<body>.*?)\s*-->", re.DOTALL)
# ONLY #include is hoisted to the top of the translation unit. Every other
# preprocessor directive stays where the block put it: an #if/#else/#endif is
# meaningless apart from the code it guards, and hoisting the three directives
# while leaving their bodies behind silently inverts the conditional. (That is
# not hypothetical -- it made the concurrency block compile the branch its own
# guard excludes.) Indenting a directive is legal C++, so wrapping is safe.
INCLUDE = re.compile(r"^\s*#\s*include\b")


@dataclass
class Fence:
    index: int              # 1-based, in document order
    line: int               # 1-based line of the fence opener
    heading: str            # nearest preceding Markdown heading
    body: list[str]         # fence content, verbatim, without the fences
    lang: str = "cpp"       # "cpp" | "c" -- which compiler this block is owed
    mode: str = "fragment"  # "fragment" | "program" | "expressions" | "skip"
    reason: str = ""        # required when mode == "skip"
    needs: list[str] = field(default_factory=list)  # preamble stanzas to inject
    requires: list[str] = field(default_factory=list)  # optional toolchain features
    run_id: str = ""        # non-empty => executed by the runtime harness under this id

    @property
    def includes(self) -> list[str]:
        return [ln for ln in self.body if INCLUDE.match(ln)]

    @property
    def code(self) -> list[str]:
        return [ln for ln in self.body if not INCLUDE.match(ln)]

    @property
    def label(self) -> str:
        return f"{self.lang} fence {self.index} (README.md:{self.line}, {self.heading!r})"


def _parse_directive(text: str) -> tuple[str, str, list[str], list[str], str]:
    """Parse `skip reason="..."`, `program`, `fragment`, `expressions`,
    `needs=a,b`, `requires=a,b`, `run=<id>`."""
    mode, reason, needs, requires, run_id = "fragment", "", [], [], ""
    m = re.search(r"run\s*=\s*([A-Za-z_][A-Za-z0-9_]*)", text)
    if m:
        run_id = m.group(1)
        text = text[: m.start()] + text[m.end():]
    m = re.search(r"requires\s*=\s*([A-Za-z0-9_,]+)", text)
    if m:
        requires = [n for n in m.group(1).split(",") if n]
        text = text[: m.start()] + text[m.end():]
    m = re.search(r'reason\s*=\s*"([^"]*)"', text)
    if m:
        reason = m.group(1)
        text = text[: m.start()] + text[m.end():]
    m = re.search(r"needs\s*=\s*([A-Za-z0-9_,]+)", text)
    if m:
        needs = [n for n in m.group(1).split(",") if n]
        text = text[: m.start()] + text[m.end():]
    for token in text.replace(",", " ").split():
        if token in ("skip", "program", "fragment", "expressions"):
            mode = token
    return mode, reason, needs, requires, run_id


def extract(readme: Path) -> list[Fence]:
    lines = readme.read_text(encoding="utf-8").split("\n")
    fences: list[Fence] = []
    heading = ""
    i = 0
    while i < len(lines):
        if lines[i].startswith("#"):
            heading = lines[i].strip()
        opener = FENCE_OPEN.match(lines[i])
        if opener:
            lang = opener.group("lang")
            j = i + 1
            while j < len(lines) and not FENCE_CLOSE.match(lines[j]):
                j += 1
            if j >= len(lines):
                raise RuntimeError(
                    f"README.md:{i + 1}: ```{lang} fence is never closed"
                )
            # A directive applies to the next fence; look back over blank lines
            # so it may sit above the prose sentence introducing the block.
            # A C block is a whole translation unit; there is no C wrapper to
            # drop a fragment into, and no shared C preamble to inject.
            mode, reason, needs, requires, run_id = (
                "program" if lang == "c" else "fragment", "", [], [], "")
            for k in range(i - 1, max(-1, i - 6), -1):
                m = DIRECTIVE.search(lines[k])
                if m:
                    (mode, reason, needs, requires,
                     run_id) = _parse_directive(m.group("body"))
                    break
            fences.append(Fence(
                index=len(fences) + 1, line=i + 1, heading=heading, lang=lang,
                body=lines[i + 1:j], mode=mode, reason=reason, needs=needs,
                requires=requires, run_id=run_id,
            ))
            i = j
        i += 1
    return fences
