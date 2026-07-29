# Contributing to FastFHIR

Thanks for contributing. FastFHIR is a binary wire format for healthcare data — the bar for correctness is high and a few rules are absolute. Read `README.md` and this page before opening a PR.

## The absolutes

1. **Wire constants are permanent.** Never renumber or reorder `RECOVERY_TAG`
   values (`include/FF_Recovery.hpp`), dictionary code IDs
   (`generator/master_codes.json`), vtable offset arithmetic, the `FF_HEADER`
   layout, `FF_CODEABLE_CONCEPT_FLAG`, `FF_CODE_NULL`, or `FF_NULL_OFFSET`.
   A PR that changes a committed wire value will be rejected regardless of
   its other merits.
2. **Never hand-edit generated files** (`generated_src/`, `dictionaries/*.cpp`,
   `dictionaries/FF_Codes.hpp`, `python/fastfhir/fields.py`). Fix the emitter
   in `generator/emit/` and regenerate.
3. **Two style regimes.** Python under `generator/` and `tests/generator/`
   is ruff/black-enforced with full type hints and fail-loud error handling.
   C++ matches the surrounding hand-tuned style and is not subject to the
   Python tooling.

## Build & test

```bash
cmake -S . -B build -DFASTFHIR_BUILD_INGESTOR=ON -DFASTFHIR_BUILD_TESTS=ON \
      -DFASTFHIR_BUILD_PYTHON_BINDINGS=ON     # first configure needs network
cmake --build build --target build_all -j
ctest --test-dir build --output-on-failure    # C++ (cpp_*) + Python (py_*)
pytest tests/generator -q                     # generator wire-format gate

# The wire-format gate compares against tests/generator/golden/wire_witness.json.
# If your change intentionally alters the generated layout (new block field,
# new vtable offset, new recovery tag, new code ID), update the golden file:
#   pytest tests/generator --update-golden
# Commit the updated golden alongside the generator/C++ change in the same PR.
# A golden update without a corresponding source change is a red flag.
ruff check generator tests/generator && black --check generator tests/generator
```

Windows prerequisites (OpenSSL via vcpkg) are in the README.

## Picking work

Pending work lives in `TASKS.md`. Read its Execution contract claim one task ID, verify the task's quoted code still matches the tree, and keep the diff scoped to that task. Tasks marked `Blocked on Q#` are not workable until the referenced question has an answer.

If your change touches the public API or wire format, note it for the companion benchmark repo (<https://github.com/ryanlandvater/FastFHIR-benchmark>) in your PR description (TASKS.md contract rule 9).

## Developer Certificate of Origin (DCO)

Contributions require a DCO sign-off certifying you have the right to submit the work under the project license (MPL-2.0). Add to every commit:

```
Signed-off-by: Your Name <your.email@example.com>
```

(`git commit -s` does this for you.) By signing off you certify the [Developer Certificate of Origin v1.1](https://developercertificate.org/).

## Licensing of contributions

FastFHIR is licensed under the Mozilla Public License, v. 2.0. By contributing you agree your contribution is licensed under MPL-2.0. New source files must carry the standard MPL header notice (copy it from any existing file in `src/`). The "FastFHIR" name is governed separately by `TRADEMARK.md`.
