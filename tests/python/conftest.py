"""
conftest.py — pytest session fixtures for FastFHIR Python tests.

When pytest imports test_readme.py all module-level run() calls are guarded
behind `if __name__ == "__main__":` so they never fire during pytest collection.
This conftest provides only the session-scoped cleanup fixture used when
running the whole suite in a single pytest invocation (e.g. python -m pytest
tests/python/).  Individual CTest entries rely on CTest's DEPENDS ordering and
a dedicated py_setup CTest step for initial artifact cleanup.
"""

import gc
import glob
import os
import tempfile

import pytest

_FF_ARTIFACTS_DIR = os.path.join(tempfile.gettempdir(), "fastfhir_test_artifacts")
_ARTIFACT_GLOBS = [os.path.join(_FF_ARTIFACTS_DIR, "*.ffhr")]


def _cleanup():
    for pattern in _ARTIFACT_GLOBS:
        for path in glob.glob(pattern):
            if os.path.isfile(path):
                try:
                    os.remove(path)
                except OSError:
                    pass


@pytest.fixture(scope="session", autouse=False)
def cleanup_artifacts():
    """Explicit fixture for full-suite runs: callers request this to clean *.ffhr before/after.
    NOT autouse — CTest individual entries use the py_setup CTest step for cleanup instead,
    and must NOT delete artifacts at session end since subsequent tests depend on them.
    """
    _cleanup()
    yield
    gc.collect()
    _cleanup()
