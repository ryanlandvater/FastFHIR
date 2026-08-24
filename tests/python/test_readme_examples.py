"""Execute the published README code blocks exactly as written.

Covers `python/README.md` and the root `README.md`.

Why this exists, and why it is not `test_readme.py`
---------------------------------------------------
`test_readme.py` *re-implements* the README examples. That is a different thing
from testing them, and the difference had already bitten: its `test_1` sets
`stream.root = patient_node` before `finalize()`, the README's Example 1 did not,
and so the suite was green while the very first example a Python user copies
failed with::

    RuntimeError: Cannot finalize because root is unset/invalid.

A hand-written mirror tests the author's memory of the documentation. This file
tests the documentation.

How a block is selected
-----------------------
A fenced ```py block is EXECUTED if its first statement imports fastfhir --
that is what makes a snippet a complete, standalone example rather than a
reference fragment (an enum listing, a field-path illustration, an API table's
signature line). The default is therefore "run it": a new end-to-end example
added to the README is covered the moment it is written, with no registration
step to forget. A fragment that cannot stand alone simply does not import.

Blocks run IN ORDER, in one temp directory, because the examples chain -- Example
1 writes `patient.ffhr`, Example 2 reads it, Example 6 compacts it. Each block
gets a fresh module namespace, so a name defined in one example is never
silently available to the next; only files cross the boundary, exactly as they
would for a reader working through the page.

Fixtures (`patient.json`, `bundle.json`, `bundle.ffhr`) are seeded below because
the README legitimately assumes the reader already has FHIR JSON on disk. They
are the smallest documents that satisfy what the prose claims -- e.g. Example 1
prints `['Ryan', 'Eric'] Landvater`, so the fixture has to contain exactly that.
"""

from __future__ import annotations

import io
import json
import os
import re
import sys
import tempfile
import traceback
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]

# Every markdown file that publishes runnable Python. The root README carries a
# teaser snippet inside a callout that advertises "every runnable example is
# executed by our test suite" -- so it had better be one of them. A claim about
# testing that is itself untested is worse than no claim.
READMES = (REPO / "python" / "README.md", REPO / "README.md")
README = READMES[0]  # retained for the coverage assertion below

try:
    import fastfhir as ff
except ModuleNotFoundError as exc:  # pragma: no cover - environment guard
    raise RuntimeError(
        "fastfhir is not importable; build the Python bindings before running this suite."
    ) from exc


# --------------------------------------------------------------------------
# Fixtures the published examples assume already exist on disk
# --------------------------------------------------------------------------
PATIENT_JSON = {
    "resourceType": "Patient",
    "id": "patient-1",
    "active": True,
    "gender": "male",
    # Example 1's comment prints `['Ryan', 'Eric'] Landvater` -- match it exactly,
    # or the example is "passing" against a document it does not describe.
    "name": [{"use": "usual", "family": "Landvater", "given": ["Ryan", "Eric"]}],
}

# Example 5 searches this bundle for `patient-42`, so it must be in here.
BUNDLE_JSON = {
    "resourceType": "Bundle",
    "id": "readme-bundle",
    "type": "collection",
    "entry": [
        {
            "fullUrl": "Patient/patient-1",
            "resource": {
                "resourceType": "Patient",
                "id": "patient-1",
                "active": True,
                "gender": "male",
                "name": [{"use": "usual", "family": "Landvater", "given": ["Ryan", "Eric"]}],
            },
        },
        {
            "fullUrl": "Patient/patient-42",
            "resource": {
                "resourceType": "Patient",
                "id": "patient-42",
                "active": True,
                "gender": "female",
                "name": [{"use": "usual", "family": "Nightingale", "given": ["Florence"]}],
            },
        },
        {
            "fullUrl": "Observation/obs-1",
            "resource": {
                "resourceType": "Observation",
                "id": "obs-1",
                "status": "final",
                "code": {
                    "coding": [
                        {"system": "http://loinc.org", "code": "8867-4", "display": "Heart rate"}
                    ]
                },
                "subject": {"reference": "Patient/patient-1"},
                "valueQuantity": {
                    "value": 72.0,
                    "unit": "beats/minute",
                    "system": "http://unitsofmeasure.org",
                },
            },
        },
        {
            "fullUrl": "DiagnosticReport/dr-1",
            "resource": {
                "resourceType": "DiagnosticReport",
                "id": "dr-1",
                "status": "final",
                "code": {
                    "coding": [
                        {"system": "http://loinc.org", "code": "58410-2", "display": "CBC panel"}
                    ]
                },
                "subject": {"reference": "Patient/patient-1"},
            },
        },
    ],
}


def _seed_fixtures(workdir: Path) -> None:
    """Write the documents the examples open, plus the sealed bundle Example 5 edits."""
    (workdir / "patient.json").write_text(json.dumps(PATIENT_JSON, indent=2), encoding="utf-8")
    (workdir / "bundle.json").write_text(json.dumps(BUNDLE_JSON, indent=2), encoding="utf-8")

    # Example 5 re-opens an EXISTING sealed bundle.ffhr, so one has to exist.
    mem = ff.Memory.create_from_file(str(workdir / "bundle.ffhr"), capacity=512 * 1024 * 1024)
    try:
        with ff.Stream(mem, ff.FhirVersion.R5) as stream:
            ingestor = ff.Ingestor()
            root, _ = ingestor.ingest(stream, ff.SourceType.FHIR_JSON, json.dumps(BUNDLE_JSON))
            stream.root = root
            stream.finalize(ff.Checksum.SHA256)
    finally:
        mem.close()


# --------------------------------------------------------------------------
# Block extraction
# --------------------------------------------------------------------------
# An example that cannot run unattended opts out with an HTML comment on the
# line before its fence:
#
#     <!-- readme-test: skip — reason -->
#
# HTML comments do not render, so the published page is unchanged. The marker is
# deliberately noisy in the source and must carry a reason: the only legitimate
# ones are examples that block on an external actor (Example 4 waits on
# socket.accept()), and each should name what covers it instead. Skipping
# because an example is broken is not a use for this.
_SKIP = re.compile(r"<!--\s*readme-test:\s*skip\b(.*?)-->", re.I | re.S)


class Block:
    __slots__ = ("index", "line", "section", "code", "skip_reason", "source")

    def __init__(
        self, index: int, line: int, section: str, code: str, skip_reason: str | None
    ) -> None:
        self.index, self.line, self.section, self.code = index, line, section, code
        self.skip_reason = skip_reason
        self.source = "python/README.md"

    @property
    def is_example(self) -> bool:
        """A complete example imports the library; a reference fragment does not."""
        for raw in self.code.splitlines():
            stripped = raw.strip()
            if not stripped or stripped.startswith("#"):
                continue
            return stripped.startswith("import fastfhir")
        return False

    @property
    def runnable(self) -> bool:
        return self.is_example and self.skip_reason is None


def extract_blocks(markdown: str) -> list[Block]:
    blocks: list[Block] = []
    section = "(top)"
    lines = markdown.splitlines()
    i = 0
    n = 0
    while i < len(lines):
        heading = re.match(r"^#{1,3} (.+)$", lines[i])
        if heading:
            section = heading.group(1)
        if lines[i].strip().startswith("```py"):
            # Look back for a skip marker, joining a small window so a multi-line
            # HTML comment is matched as one string. (A single-line lookback
            # silently missed a three-line marker and ran the server example
            # anyway — the harness hung, which is at least a loud failure, but
            # the check has to see what the author wrote.)
            reason = None
            k = i - 1
            while k >= 0 and not lines[k].strip():
                k -= 1
            window = "\n".join(lines[max(0, k - 5) : k + 1])
            m = _SKIP.search(window)
            if m and window.rstrip().endswith("-->"):
                reason = " ".join(m.group(1).split()).strip(" —-:") or "(no reason given)"
            start = i + 1
            j = start
            while j < len(lines) and not lines[j].strip().startswith("```"):
                j += 1
            n += 1
            blocks.append(Block(n, start + 1, section, "\n".join(lines[start:j]), reason))
            i = j
        i += 1
    return blocks


# --------------------------------------------------------------------------
# The test
# --------------------------------------------------------------------------
def test_readme_examples() -> None:
    blocks: list[Block] = []
    for path in READMES:
        assert path.is_file(), f"missing {path}"
        rel = path.relative_to(REPO).as_posix()
        for b in extract_blocks(path.read_text(encoding="utf-8")):
            b.source = rel
            blocks.append(b)

    examples = [b for b in blocks if b.is_example]
    runnable = [b for b in examples if b.runnable]

    print(f"  {len(blocks)} code blocks, {len(examples)} examples, {len(runnable)} executed")
    for b in examples:
        if b.skip_reason:
            print(f"    skip  {b.source}:{b.line} — {b.skip_reason}")
    # Guard against the extractor silently matching nothing -- a green test that
    # measured zero examples is the failure mode this file exists to prevent.
    assert len(runnable) >= 6, (
        f"only {len(runnable)} runnable examples found in {README.name}; the "
        "extractor is probably not matching the fences any more"
    )

    failures: list[str] = []
    original_cwd = os.getcwd()
    with tempfile.TemporaryDirectory(prefix="ffhr_readme_") as tmp:
        workdir = Path(tmp)
        _seed_fixtures(workdir)
        os.chdir(workdir)
        try:
            for block in runnable:
                label = f"{block.source}:{block.line} — {block.section}"
                buf = io.StringIO()
                stdout = sys.stdout
                sys.stdout = buf
                try:
                    exec(  # noqa: S102 - executing the documentation is the point
                        compile(block.code, f"{block.source}:{block.line}", "exec"),
                        {"__name__": "__readme__"},
                    )
                except BaseException:  # noqa: BLE001 - report, do not mask
                    sys.stdout = stdout
                    failures.append(f"{label}\n{traceback.format_exc()}")
                    print(f"    FAIL  {label}")
                    continue
                finally:
                    sys.stdout = stdout
                print(f"    ok    {label}")
        finally:
            os.chdir(original_cwd)

    assert not failures, (
        f"{len(failures)} of {len(runnable)} published README example(s) do not run.\n"
        "These are the examples a new user copies first — fix the README, not this test.\n\n"
        + "\n".join(failures)
    )


if __name__ == "__main__":
    try:
        test_readme_examples()
    except AssertionError as exc:
        print(exc)
        raise SystemExit(1) from exc
    print("all published README examples run")
