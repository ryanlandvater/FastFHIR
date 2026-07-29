"""The generator must produce byte-identical output on every run.

Without this, you cannot diff two generator runs and trust that a difference is
a real change. Block emission order used to vary with PYTHONHASHSEED because
the topological sort in generator/model/merge.py iterated an unordered set --
~10,000 lines of phantom diff between consecutive runs of unchanged code.

The wire was never affected (per-block V-Table offsets are self-contained), but
the noise made review impossible and would have hidden real drift.
"""

from __future__ import annotations

import filecmp
import subprocess
import sys
from pathlib import Path

import pytest

_REPO_ROOT = Path(__file__).resolve().parents[2]


def _generate(out: Path) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, "-m", "generator", "--output-dir", str(out)],
        cwd=_REPO_ROOT,
        capture_output=True,
        text=True,
        timeout=1800,
    )


def _differences(a: Path, b: Path) -> list[str]:
    """Recursively collect every path where the two trees disagree."""
    out: list[str] = []

    def walk(cmp: filecmp.dircmp, prefix: str = "") -> None:
        out.extend(f"{prefix}{n} (differs)" for n in cmp.diff_files)
        out.extend(f"{prefix}{n} (only in first)" for n in cmp.left_only)
        out.extend(f"{prefix}{n} (only in second)" for n in cmp.right_only)
        out.extend(f"{prefix}{n} (unreadable)" for n in cmp.funny_files)
        for name, sub in cmp.subdirs.items():
            walk(sub, f"{prefix}{name}/")

    walk(filecmp.dircmp(str(a), str(b)))
    return out


def test_two_runs_are_byte_identical(tmp_path: Path) -> None:
    a, b = tmp_path / "run_a", tmp_path / "run_b"

    for out in (a, b):
        proc = _generate(out)
        if proc.returncode != 0:
            if "urlopen" in proc.stderr or "Download failed" in proc.stdout:
                pytest.skip("generator needs network access to packages.fhir.org")
            pytest.fail(
                f"`python -m generator` exited {proc.returncode}\n"
                f"stderr tail:\n{proc.stderr[-1500:]}"
            )

    diffs = _differences(a, b)
    assert not diffs, (
        f"{len(diffs)} paths differ between two runs of the same code -- the generator "
        f"is non-deterministic. Look for iteration over an unordered set or dict. "
        f"First few: {diffs[:8]}"
    )
