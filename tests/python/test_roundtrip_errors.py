"""Error paths of the round-trip gate -- TASKS.md A22.2 and COV-2.

`test_roundtrip.py` is a gate, and a gate's failure paths are the part nobody
runs: they only execute when something is already wrong. This one has broken
twice without a single test noticing:

  * the handlers raised `NameError` because `DiffKind` was not imported (A22),
    destroying the "harness not found" message they existed to print;
  * after `stats` joined the success return, the error returns kept three
    values, so `main()`'s four-way unpack raised `ValueError` instead (found
    2026-09-15 under the Xcode preset, whose harness path was wrong).

And the corpus-level gate passed vacuously when discovery found nothing (the
2026-08-13 incident), which COV-2 closes.

Every case here uses a stand-in harness -- a missing path, a non-executable
file, a script that sleeps or exits non-zero, or one that echoes its input --
so each path is reached deliberately, with no C++ build and no Synthea corpus.
The assertions are on the MESSAGE and the EXIT CODE, because "the run failed"
was true in both historical breakages; what was lost was why.
"""

from __future__ import annotations

import json
import os
import stat
import subprocess
import sys
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE))

import test_roundtrip  # noqa: E402  (path set above)
from roundtrip_diff import DiffStats  # noqa: E402

_SCRIPT = _HERE / "test_roundtrip.py"

_POSIX_ONLY = pytest.mark.skipif(
    sys.platform == "win32", reason="stand-in harnesses are POSIX shell scripts"
)

_PATIENT = {
    "resourceType": "Patient",
    "id": "roundtrip-errors",
    "active": True,
    "name": [{"family": "Gate", "given": ["Error", "Path"]}],
}


def _fixture(directory: Path) -> Path:
    path = directory / "patient.json"
    path.write_text(json.dumps(_PATIENT), encoding="utf-8")
    return path


def _script(directory: Path, name: str, body: str, executable: bool = True) -> Path:
    path = directory / name
    path.write_text("#!/bin/sh\n" + body + "\n", encoding="utf-8")
    mode = stat.S_IRUSR | stat.S_IWUSR
    if executable:
        mode |= stat.S_IXUSR
    os.chmod(path, mode)
    return path


def _only_message(result: tuple) -> str:
    """Assert the error-return contract and hand back its single message."""
    assert len(result) == 4, "error returns carry the same four values as success"
    ok, diffs, report, stats = result
    assert ok is False
    assert isinstance(stats, DiffStats)
    assert report == ""
    assert len(diffs) == 1
    return diffs[0].message


def _run_main(*args: str) -> subprocess.CompletedProcess[str]:
    env = dict(os.environ)
    env.pop("FASTFHIR_SYNTHEA_DIR", None)  # the CLI arguments below are the whole input
    return subprocess.run(
        [sys.executable, str(_SCRIPT), *args],
        capture_output=True,
        text=True,
        timeout=60,
        env=env,
    )


# ── A22.2: each handler, called directly ────────────────────────────────────


def test_missing_harness_reports_not_found(tmp_path: Path) -> None:
    missing = tmp_path / "no_such_ff_roundtrip"
    message = _only_message(
        test_roundtrip.run_roundtrip_test(_fixture(tmp_path), harness_path=str(missing))
    )
    assert "C++ harness not found" in message
    assert str(missing) in message


@_POSIX_ONLY
def test_non_executable_harness_is_reported_not_raised(tmp_path: Path) -> None:
    harness = _script(tmp_path, "ff_roundtrip", "exit 0", executable=False)
    message = _only_message(
        test_roundtrip.run_roundtrip_test(_fixture(tmp_path), harness_path=str(harness))
    )
    assert "not executable" in message


@_POSIX_ONLY
def test_hung_harness_reports_timeout(tmp_path: Path) -> None:
    # `exec` so the timeout kills the sleep itself, not a shell that outlives it.
    harness = _script(tmp_path, "ff_roundtrip", "exec sleep 30")
    message = _only_message(
        test_roundtrip.run_roundtrip_test(
            _fixture(tmp_path), harness_path=str(harness), timeout=0.5
        )
    )
    assert "timed out after 0.5s" in message
    assert "patient.json" in message


@_POSIX_ONLY
def test_failing_harness_reports_exit_code_and_stderr(tmp_path: Path) -> None:
    harness = _script(tmp_path, "ff_roundtrip", "echo 'ingest exploded' >&2\nexit 3")
    message = _only_message(
        test_roundtrip.run_roundtrip_test(_fixture(tmp_path), harness_path=str(harness))
    )
    assert "exited code 3" in message
    assert "ingest exploded" in message


# ── A22.2: the same paths through main(), where the unpack bug lived ────────


def test_main_reports_missing_harness_without_a_traceback(tmp_path: Path) -> None:
    proc = _run_main(
        "--fixture", str(_fixture(tmp_path)), "--harness", str(tmp_path / "missing")
    )
    assert proc.returncode == 1, proc.stdout + proc.stderr
    assert "C++ harness not found" in proc.stdout
    assert "Traceback" not in proc.stderr, proc.stderr


@_POSIX_ONLY
def test_main_passes_when_the_round_trip_is_exact(tmp_path: Path) -> None:
    # The control case: a harness that returns its input unchanged is a perfect
    # round-trip. Without it, every assertion above could pass against a gate
    # that fails everything.
    harness = _script(tmp_path, "ff_roundtrip", 'cat "$1"')
    proc = _run_main("--fixture", str(_fixture(tmp_path)), "--harness", str(harness))
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "source values compared" in proc.stdout


# ── COV-2: an empty corpus is never a pass ──────────────────────────────────


def test_missing_corpus_is_a_skip_not_a_pass(tmp_path: Path) -> None:
    proc = _run_main("--synthea-dir", str(tmp_path / "absent"), "--harness", "unused")
    assert proc.returncode == test_roundtrip.SKIP_RETURN_CODE, proc.stdout
    assert "NOT RUN" in proc.stdout


def test_unset_corpus_is_a_skip_not_a_pass() -> None:
    proc = _run_main("--synthea-dir", "", "--harness", "unused")
    assert proc.returncode == test_roundtrip.SKIP_RETURN_CODE, proc.stdout


def test_empty_corpus_directory_fails(tmp_path: Path) -> None:
    empty = tmp_path / "synthea"
    empty.mkdir()
    proc = _run_main("--synthea-dir", str(empty), "--harness", "unused")
    assert proc.returncode == 1, proc.stdout
    assert "yielded 0 fixtures" in proc.stdout


def test_corpus_of_non_fixtures_fails(tmp_path: Path) -> None:
    # The 2026-08-13 shape: a directory full of files, none of which discovery
    # accepts. Here they are not .json; then, a filter excluded every Bundle.
    corpus = tmp_path / "synthea"
    corpus.mkdir()
    (corpus / "README.txt").write_text("not a fixture", encoding="utf-8")
    proc = _run_main("--synthea-dir", str(corpus), "--harness", "unused")
    assert proc.returncode == 1, proc.stdout
