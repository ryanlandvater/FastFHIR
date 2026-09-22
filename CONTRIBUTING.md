# Contributing to FastFHIR

Thanks for contributing. FastFHIR is a binary wire format for healthcare data — the bar for correctness is high and a few rules are absolute. Read `README.md` and this page before opening a PR.

## The absolutes

1. **Wire constants are permanent.** Never renumber or reorder `RECOVERY_TAG`
   values (`dictionaries/master_tags.json` — `generated_src/FF_RecoveryTags.hpp` is
   its generated projection, not the source), dictionary code IDs
   (`dictionaries/master_codes.json`), vtable offset arithmetic, the `FF_HEADER`
   layout, `FF_CODED_VALUE_FLAG`, `FF_CODE_NULL`, or `FF_NULL_OFFSET`.
   A PR that changes a committed wire value will be rejected regardless of
   its other merits.
2. **Never hand-edit generated files** (`generated_src/` — including `FF_Codes.hpp`,
   the dictionary tables and `FF_RecoveryTags.hpp` — plus `python/fastfhir/fields.py`). Fix
   the emitter in `generator/emit/` and regenerate.
3. **Two style regimes.** Python under `generator/` and `tests/generator/`
   is ruff/black-enforced with full type hints and fail-loud error handling.
   C++ matches the surrounding hand-tuned style and is not subject to the
   Python tooling.
4. **C++ types live in the `FastFHIR` namespace; their `FF_` names are global
   aliases.** A type is named PascalCase inside the namespace
   (`FastFHIR::Builder`, `FastFHIR::String`, `FastFHIR::Result`); the C-style
   spelling is a global `using`, collected in the alias block in
   `include/FastFHIR.hpp` (`using FF_Builder = FastFHIR::Builder;`). The block is
   the ONE list; the only exception is a type the low-level headers return
   before they can see `FastFHIR.hpp` — `FF_Result` and its enums alias beside
   their definition in `FF_Primitives.hpp`, because `FF_Builder.hpp` returns an
   `FF_Result` without including the consumer header. A heap body carries a `_t`
   suffix and its handle is the shared_ptr over it: `Builder_t` /
   `FastFHIR::Builder` and `Memory_t` / `FastFHIR::Memory` alike. `Memory` is
   the typedef every call site uses; `std::shared_ptr<Memory_t>` is never
   spelled outside `FF_Memory.hpp`. Wire block structs (`FF_HEADER`,
   `FF_STRING`) and the C-ABI
   `FF_*Info` structs keep their `FF_` names — they ARE the C surface, not a C++
   type behind one. Do not put a global `FF_` alias at the bottom of a type's
   own header.
5. **Info structs are better than long argument lists.** A public function takes one
   `const XxxInfo&` — plus a `T& out` where it returns a handle — rather than
   four or more positional arguments. The external `FF_*` surface, the internal
   `seal_stream(const StreamSealInfo&)`, the `amend_*` verbs
   (`AmendResourceInfo` / `AmendVariantInfo` / `AmendDatetimeInfo`),
   `Compactor::archive(const ArchiveInfo&)`, and
   `Ingest::InsertAtFieldInfo` all follow this. A function that needs a fourth
   argument is a signal to bundle, not to add a parameter: it lets the call site
   name each field and lets the surface grow a field without touching callers.
6. **The C ABI is a separate, hand-written header.** `include/FastFHIR.h` is
   pure C; it and `FastFHIR.hpp` are two surfaces over one library and declare
   same-named-but-different types (`FF_MemoryCreateInfo`, `FF_CreateMemory`, …),
   so neither includes the other. The facts they must agree on — struct fields
   and their order, enum values — are held once in `tests/generator/c_abi_spec.py`
   and checked against BOTH headers by `tests/generator/test_c_abi.py`. Absolute
   5 applies to it too, and symmetrically: **where `FastFHIR.hpp` has an
   `FF_XxxInfo`, `FastFHIR.h` has the same-named struct with the same fields in
   the same order**, so porting between the surfaces is renaming rather than
   redesigning. A field that cannot cross the ABI — today only the checksum
   `hasher`, a `std::function` the library calls back into — is marked
   `cpp_only` in the spec, which is what keeps "absent on purpose" distinct from
   "forgotten". **Do not include both in one translation unit**, as
   `FastFHIR.h` itself says: the C
   `FF_MemoryCreateInfo` is global and the C++ one is in `namespace FastFHIR`,
   so the single `using namespace FastFHIR;` that every example writes makes the
   name ambiguous and the TU stops compiling. Pick the surface the TU needs.
   Spell it `c_api`, never `capi` (`src/FF_C_API.cpp`, `ff_test_c_api`).

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
#   python -m tests.generator.wire_witness generated_src \
#       tests/generator/golden/wire_witness.json
# Commit the updated golden alongside the generator/C++ change in the same PR.
# A golden update without a corresponding source change is a red flag.
# The witness script refuses to overwrite if the output matches the existing
# golden.
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
