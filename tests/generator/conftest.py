"""Pytest fixtures for the generator wire-format gate.

`regenerated_dir` yields a path to a freshly generated `generated_src/` tree.

  1. FASTFHIR_GENERATED_DIR env var, if set (CI may pre-generate).
  2. Otherwise run `python -m generator --output-dir <tmp>`.

There is deliberately NO fallback to the in-repo `generated_src/`. That fallback
used to turn "the generator is broken" into "the tests pass", by silently
comparing a stale tree against a baseline derived from that same stale tree.
A generator that will not run is a failure, not a skip.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path

import pytest

_REPO_ROOT = Path(__file__).resolve().parents[2]
# Make `import wire_witness` work without packaging the tests dir.
sys.path.insert(0, str(Path(__file__).parent))


def _shipped_profile() -> str:
    """The `FASTFHIR_PRODUCTION_PROFILE` the project actually builds with.

    Read from CMakePresets.json rather than restated here, because a second copy
    of a value is a value that drifts -- `FF_MIN_ARENA` was duplicated between
    the CLI and its own regression test and had already drifted to half the real
    figure while the test's comment claimed to mirror it.

    This matters more than it looks. `witness()` derives `vtables` from the
    EMITTED resource headers, so the gate only covers what the profile compiled.
    With the profile left to its `us-core` default, the golden held 141 blocks
    while a preset build emits 209 -- every V-Table introduced by `billing`,
    `medication-admin` and `supply` (Claim, ClaimResponse, ExplanationOfBenefit,
    SupplyDelivery, MedicationAdministration, ...) sat entirely outside "the ONE
    hard gate", free to have its field order or header size drift with nothing
    to catch it. `tags` and `codes` are unaffected: both are projected from the
    committed ledgers and cover the whole spec regardless of profile.
    """
    presets = json.loads((_REPO_ROOT / "CMakePresets.json").read_text(encoding="utf-8"))
    for preset in presets.get("configurePresets", []):
        profile = preset.get("cacheVariables", {}).get("FASTFHIR_PRODUCTION_PROFILE")
        if profile:
            return profile
    raise RuntimeError(
        "no FASTFHIR_PRODUCTION_PROFILE in CMakePresets.json — the wire gate cannot "
        "determine which profile to witness, and silently falling back to the "
        "generator's `us-core` default is what left 68 blocks ungated."
    )


def _regenerate(tmp: Path) -> Path:
    """Run `python -m generator` into `tmp`. Fail loudly if it cannot."""
    # Pin the profile EXPLICITLY. Inheriting the ambient environment means the
    # gate witnesses whatever the developer happened to export, or the
    # generator's silent `us-core` default when they exported nothing.
    env = {**os.environ, "FASTFHIR_PRODUCTION_PROFILE": _shipped_profile()}
    try:
        proc = subprocess.run(
            [sys.executable, "-m", "generator", "--output-dir", str(tmp)],
            cwd=_REPO_ROOT,
            capture_output=True,
            text=True,
            timeout=1800,
            env=env,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise RuntimeError(f"could not run `python -m generator`: {exc}") from exc

    if proc.returncode != 0:
        if "urlopen" in proc.stderr or "Download failed" in proc.stdout:
            pytest.skip("generator needs network access to packages.fhir.org")
        raise RuntimeError(
            f"`python -m generator` exited {proc.returncode}; fix the generator before "
            f"running the wire gate.\nstderr tail:\n{proc.stderr[-2000:]}"
        )
    if not any(tmp.glob("*.hpp")):
        raise RuntimeError(f"generator reported success but wrote no headers into {tmp}")
    return tmp


@pytest.fixture(scope="session")
def regenerated_dir(tmp_path_factory: pytest.TempPathFactory) -> Path:
    env_dir = os.environ.get("FASTFHIR_GENERATED_DIR")
    if env_dir and Path(env_dir).is_dir():
        return Path(env_dir)

    return _regenerate(tmp_path_factory.mktemp("generated"))
