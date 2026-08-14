"""
Test: FF_CODE Dictionary permanence — zero value drift.

Ensures that regenerating the FF_R4_Dictionary.cpp and FF_R5_Dictionary.cpp
files produces NO modifications to existing entries.  Only wholly new lines
(additions for new FHIR codes) are permitted — existing code→value mappings
must never change.

This is the same permanence guarantee that FF_Recovery.hpp provides:
once committed, a wire value is fixed forever.
"""

import os
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DICT_FILES = ["src/FF_R4_Dictionary.cpp", "src/FF_R5_Dictionary.cpp"]


def _git_diff_shortstat(files: list[str]) -> dict:
    """Run git diff --shortstat on the given files and return parsed counts."""
    result = subprocess.run(
        ["git", "diff", "--shortstat", "--"] + files,
        capture_output=True, text=True, cwd=REPO_ROOT,
    )
    stdout = result.stdout.strip()
    if not stdout:
        return {"insertions": 0, "deletions": 0}

    # Parse "1 file changed, 2 insertions(+), 1 deletion(-)"
    parts = stdout.split(",")
    ins = 0
    dels = 0
    for p in parts:
        p = p.strip()
        if "insertion" in p:
            ins = int(p.split()[0])
        elif "deletion" in p:
            dels = int(p.split()[0])
    return {"insertions": ins, "deletions": dels}


def _git_diff_changed_lines(files: list[str]) -> list[str]:
    """Return the unified diff lines, or empty list if no diff."""
    result = subprocess.run(
        ["git", "diff", "-U0", "--"] + files,
        capture_output=True, text=True, cwd=REPO_ROOT,
    )
    return result.stdout.splitlines()


def test_dictionary_regeneration_no_modifications():
    """Regenerate the dictionary files and verify no existing entries changed.

    The dictionary generator reads the committed .cpp files to extract
    existing code→value mappings.  It must preserve all existing values
    exactly — only new codes (not yet in any file) may be appended.
    """
    # ── Regenerate ─────────────────────────────────────────────────
    sys.path.insert(0, str(REPO_ROOT))
    from generator.emit.code_ids import generate_master_dictionary

    code_system_urls = generate_master_dictionary(
        [("R4", "fhir_specs/R4"), ("R5", "fhir_specs/R5")]
    )
    assert isinstance(code_system_urls, set), (
        f"Expected set of code system URLs, got {type(code_system_urls)}"
    )

    # ── Check diff: no deletions/removals ───────────────────────────
    diff_stats = _git_diff_shortstat(DICT_FILES)

    # We need to check deletions more carefully. WriteIfChanged means the
    # file is only written when content actually changes, so we check git
    # for the diff. If deletions == 0, no existing entries were modified.
    if diff_stats["deletions"] > 0:
        # Show the diff to help debugging
        diff_lines = _git_diff_changed_lines(DICT_FILES)
        diff_text = "\n".join(diff_lines)
        raise AssertionError(
            f"Dictionary permanence violation: {diff_stats['deletions']} "
            f"line(s) deleted/modified (only additions are permitted).\n"
            f"Git diff:\n{diff_text}"
        )

    # Deletions == 0 means no existing values changed. Additions are OK.
    print(
        f"[OK] Dictionary unchanged ({diff_stats['insertions']} new "
        f"insertions, 0 deletions)"
    )


def test_dictionary_files_compile():
    """Verify the generated dictionary .cpp files compile standalone."""
    import subprocess

    for fname in DICT_FILES:
        src = REPO_ROOT / fname
        if not src.exists():
            raise FileNotFoundError(f"Dictionary file missing: {src}")
        result = subprocess.run(
            ["g++", "-std=c++20", "-c",
             "-I", str(REPO_ROOT / "include"),
             "-I", str(REPO_ROOT / "generated_src"),
             str(src),
             "-o", "/dev/null"],
            capture_output=True, text=True,
        )
        assert result.returncode == 0, (
            f"Compilation failed for {fname}:\n{result.stderr}"
        )
    print(f"[OK] All {len(DICT_FILES)} dictionary files compile")


def test_known_extensions_includes_code_system_urls():
    """Verify that the known-extensions header includes at least some
    FHIR code system URLs (not just extension URLs)."""
    known_path = REPO_ROOT / "generated_src" / "FF_KnownExtensions.hpp"
    assert known_path.exists(), f"Missing: {known_path}"

    with open(known_path) as f:
        content = f.read()

    # Should include FHIR code system URLs (not just StructureDefinition URLs)
    assert 'http://hl7.org/fhir/administrative-gender' in content, (
        "FF_KnownExtensions.hpp is missing FHIR code system URLs. "
        "Run the full generator pipeline to populate them."
    )
    print("[OK] Known extensions header includes code system URLs")
