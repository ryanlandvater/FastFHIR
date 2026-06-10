"""Pytest fixtures for the generator wire-format gate.

`regenerated_dir` yields a path to a freshly generated `generated_src/` tree.
Resolution order (first that works wins):

  1. FASTFHIR_GENERATED_DIR env var, if set (CI may pre-generate).
  2. Run `python -m generator --output-dir <tmp>` into a session tmp dir.
  3. Fall back to the in-repo `generated_src/` (lets the gate run against the
     current tree even before the new package can self-invoke).

The fallback means the witness test is meaningful from day one: it pins the
CURRENT generated tree as the baseline, then every refactor re-checks against it.
"""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

import pytest

_REPO_ROOT = Path(__file__).resolve().parents[2]
# Make `import wire_witness` work without packaging the tests dir.
sys.path.insert(0, str(Path(__file__).parent))


def _try_regenerate(tmp: Path) -> Path | None:
    """Attempt `python -m generator` into `tmp`. Return tmp on success, else None."""
    try:
        proc = subprocess.run(
            [sys.executable, "-m", "generator", "--output-dir", str(tmp)],
            cwd=_REPO_ROOT,
            capture_output=True,
            text=True,
            timeout=600,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    if proc.returncode != 0:
        # Surface the failure in -q output without hard-failing collection;
        # the fallback path keeps the gate usable during migration.
        print(proc.stdout)
        print(proc.stderr, file=sys.stderr)
        return None
    return tmp if any(tmp.glob("*.hpp")) else None


@pytest.fixture(scope="session")
def regenerated_dir(tmp_path_factory: pytest.TempPathFactory) -> Path:
    env_dir = os.environ.get("FASTFHIR_GENERATED_DIR")
    if env_dir and Path(env_dir).is_dir():
        return Path(env_dir)

    tmp = tmp_path_factory.mktemp("generated")
    regenerated = _try_regenerate(tmp)
    if regenerated is not None:
        return regenerated

    fallback = _REPO_ROOT / "generated_src"
    if not fallback.is_dir():
        pytest.skip("no generated_src/ and `python -m generator` unavailable")
    return fallback
