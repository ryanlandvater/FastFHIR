"""Repo-wide pytest fixtures.

Sits at `tests/` rather than in a suite subdirectory so it applies to every
collection path -- `pytest tests/generator`, `pytest tests/python`, or a bare
`pytest tests/` -- because the property it guards is not specific to any one
suite. See tests/tree_guard.py for what happened without it (TASKS.md GEN-1.5).
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent))

from tree_guard import diff, fingerprint, format_violation  # noqa: E402


@pytest.fixture(scope="session", autouse=True)
def _working_tree_unchanged() -> object:
    """Fail the session if the tests modified `generated_src/` or `dictionaries/`.

    Autouse and session-scoped: the cost is one tree hash at each end (~0.08 s
    for ~840 files), and a guard that has to be opted into is a guard the next
    test to acquire this bug will not opt into.

    Raises in TEARDOWN, so it cannot mask a real failure -- the tests report
    their own results first and this is appended. A test that legitimately needs
    to write into the tree does not exist today; if one ever does, it belongs in
    a temp directory, which is what every other generator test already does.
    """
    before = fingerprint()
    yield
    entries = diff(before, fingerprint())
    if entries:
        raise RuntimeError(
            "the test session MODIFIED the working tree -- tests must not write "
            f"into generated_src/ or dictionaries/:\n{format_violation(entries)}\n\n"
            "This is TASKS.md GEN-1: `test_regeneration_preserves_every_committed_id` "
            "ran `python -m generator` with no --output-dir, regenerating the repo "
            "tree at the `us-core` default and leaving a short FF_CodeSystems.hpp "
            "beside sources from a wider profile. It presented as an intermittent "
            "generator flake for two days.\n"
            "Fix the TEST, not this guard: generate into a "
            "tempfile.TemporaryDirectory (see tests/generator/conftest.py, which "
            "has always done this correctly). If the tree is merely stale rather "
            "than test-modified, re-run `cmake --preset ninja` and try again."
        )
