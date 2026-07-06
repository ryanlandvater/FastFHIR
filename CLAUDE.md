# CLAUDE.md — FastFHIR

FastFHIR is a C++20 zero-copy **binary serialization format for HL7 FHIR** (R4/R5):
offset-based data blocks in a memory-mapped arena, lock-free concurrent building, JSON
ingest/export, optional compact archives, and a Python code generator that emits the typed
C++ from official HL7 StructureDefinitions. Alpha stage. Licensed under FF-SSL (Apache-2.0
base with redistribution/attribution restrictions) — never modify `LICENSE` or strip
attribution headers from source files.

**Pending work lives in `TASKS.md`** — read its Execution contract before claiming a task.
The older checklist docs (`audit*.md`, `*_revision.todo.md`, `project_progress.prompt.md`,
`generator_refactor_plan.md`, `unification.plan.md`) are superseded historical records.

## Repo map

| Path | Role |
|---|---|
| `include/` | Public + internal headers. `FastFHIR.hpp` is the consumer entry point. `FF_Recovery.hpp` and `FF_Primitives.hpp` define wire constants — **hand-maintained, values permanent**. |
| `src/` | Core library: Memory (VMA), Builder, Parser, Compactor, Ingestor, Dictionary, Extensions (WASM), Primitives. |
| `dictionaries/` | **Generated** code dictionaries (`FF_R4/R5_Dictionary.cpp`, `FF_Dictionary_Strings.cpp`, `FF_UCUM_Dictionary.cpp`, `FF_Codes.hpp`). Committed, but never hand-edit — regenerate via `python -m generator`. |
| `generator/` | Python code generator (`pipeline.py` orchestrates; `model/` pure data, `emit/` model→str, `bindings/` Python emission). Source of truth for everything in `generated_src/` and `dictionaries/`. `master_codes.json` holds committed permanent code IDs. |
| `generated_src/` | Generator output (~70 C++ files). **Gitignored** — produced at CMake configure time; requires network (HL7 / packages.fhir.org). |
| `python/` | pybind11 bindings (`FF_PythonBindings.cpp` → `_core`) + `fastfhir` package. `fastfhir/fields.py` is generated at build time. |
| `tools/` | CLI tools: `ingestor/FF_Ingest.cpp`, `exporter/FF_Export.cpp`, `compactor/FF_Compact.cpp`. |
| `tests/` | `cpp/` (standalone-main tests via ctest), `python/` (README/round-trip suites via ctest `py_*`), `generator/` (pytest wire-format gate). |
| `architecture.md` | Deep reference for the binary format, VMA, builder, and read path. Read it before touching wire-format code. |
| `terminology_layer_architecture.md` | CodeableConcept / code-system encoding design. |

## Build & test

```bash
# Configure (runs the Python generator — needs network on first configure)
cmake -S . -B build -DFASTFHIR_BUILD_INGESTOR=ON -DFASTFHIR_BUILD_TESTS=ON \
      -DFASTFHIR_BUILD_PYTHON_BINDINGS=ON
cmake --build build --target build_all -j

# C++ + Python integration tests (names: cpp_*, py_*)
ctest --test-dir build --output-on-failure

# Generator tests (wire-format gate)
pytest tests/generator -q

# Python lint/format (generator code only — generated C++ is out of scope)
ruff check generator tests/generator && black --check generator tests/generator
```

Key CMake options: `FASTFHIR_PRODUCTION_PROFILE` (`us`|`uk`), `FASTFHIR_BUILD_INGESTOR`
(needs simdjson; also gates `ff_ingest` and OpenSSL), `FASTFHIR_BUILD_TESTS`,
`FASTFHIR_BUILD_PYTHON_BINDINGS`, `FASTFHIR_RUN_GENERATOR` (default ON, at configure time).
Bazel targets mirror the CMake ones (`MODULE.bazel`/`BUILD.bazel`) but CMake is primary.
Windows: OpenSSL via vcpkg (see README → Windows Build Prerequisites).

## Architecture in brief

- **Memory (VMA)** — `Memory::create()` / `createFromFile()` reserve a sparse virtual arena
  (mmap / VirtualAlloc). All structures live at stable offsets inside it; `Memory::View` is
  the lifetime-safe window; `StreamHead` is the exclusive raw-ingest cursor.
- **Builder** — appends `DATA_BLOCK`s via lock-free `claim_space()` (atomic bump); fields are
  fixed vtable slots back-patched by `amend_pointer` / `amend_resource` / `amend_variant`.
  `finalize()` writes the `FF_HEADER` (54-byte layout documented at
  `include/FF_Primitives.hpp:537`) and optional checksum footer.
- **Parser / `Reflective::Node`** — zero-copy read lens over the raw bytes; `root()[KEY]`
  navigation, `node.as<T>()` typed extraction, `print_json()` export. No heap allocation on
  the read path.
- **Dual type system** — every block carries a 2-byte `RECOVERY_TAG` (semantic identity,
  `include/FF_Recovery.hpp`) and fields carry an `FF_FieldKind` (physical layout). Choice
  (`[x]`) fields are a 10-byte slot: 8-byte value/offset + 2-byte tag.
- **Codes** — assignment tries the dictionary (`FF_GetDictionaryCode` → permanent uint32 ID
  from `master_codes.json`), else writes an `FF_CODEABLE_CONCEPT` block and sets
  `FF_CODEABLE_CONCEPT_FLAG` (`0x80000000`) in the slot. `FF_CODE_NULL` = `0xFFFFFFFF`.
- **Extensions** — per-extension `EXT_REF` word routes to a registered WASM codec module
  (MSB=1), a retained URL in `FF_URL_DIRECTORY` (MSB=0), or suppression (`0xFFFFFFFF`).
- **Compactor** — post-finalize rewrite of a sealed stream into a presence-bitmask compact
  layout; output is read-only, traversed with the same Node API.

## Hard invariants — breaking these corrupts data on the wire

1. **Wire constants are permanent.** Never renumber or reorder: `RECOVERY_TAG` values
   (`include/FF_Recovery.hpp` — hand-maintained, generator only *validates* names against
   it), dictionary code IDs (`generator/master_codes.json` — committed source of truth; the
   generator must never reassign an existing ID), vtable offset arithmetic
   (symbolic sums in headers — never introduce literal offsets), `FF_HEADER` layout,
   `FF_CODEABLE_CONCEPT_FLAG`, `FF_CODE_NULL`, `FF_NULL_OFFSET`.
2. **Never hand-edit generated files** (`generated_src/`, `dictionaries/*.cpp`,
   `dictionaries/FF_Codes.hpp`, `python/fastfhir/fields.py`). Fix the emitter in
   `generator/emit/` and regenerate. If a generated file and its emitter disagree, the
   emitter wins.
3. **Two style regimes.** Python in `generator/` + `tests/generator/`: ruff/black enforced
   (pyproject.toml), full type hints, fail-loud (`raise` over silent fallback). C++ (and
   generated C++): match surrounding hand-tuned style; not subject to the Python tooling.
4. **Includes are bare names** (`#include "FF_Parser.hpp"`), never `"../include/..."` —
   both include dirs are on the compiler path. (Legacy violations are being removed —
   TASKS.md A2.)
5. **Errors are exceptions** (`std::runtime_error` / `std::system_error`) with actionable
   messages on the write path; the read path returns falsy Nodes / null sentinels instead of
   throwing on absent fields. Recovery-gate failures use `**RECOVERY_GATE**` /
   `**RECOVERY_REQUIRED**` markers in messages — preserve them, tooling greps for them.
6. **Concurrency contract:** `claim_space()` appends are lock-free and thread-safe;
   pointer amendments and finalize are not concurrency-protected (see TASKS.md Q9). Don't
   introduce mutexes into the append hot path.
7. **The two SHA-256 roles in the WASM registry are never conflated:** `sha256(url)` is a
   disk metadata filename only; `sha256(wasm_bytes)` is module identity on the wire.

## Working a task from TASKS.md

1. Pick one unchecked task (or subsection) whose `Blocked on Q#` (if any) has an answer.
2. Re-verify every file:line reference with `grep` — line numbers drift.
3. Make the change; keep the diff scoped to that task.
4. Run the task's Verify command plus the affected suite (`ctest`, `pytest tests/generator`).
5. Check the box in TASKS.md with the commit hash, commit both together.

Common pitfalls for agents: `generated_src/` won't exist until you configure with network;
`ctest` Python tests use `.venv/bin/python` if present, else the system interpreter; the
wire-format pytest gate currently **skips silently** until TASKS.md A4 lands — a green run
does not prove wire stability; MSVC is the strictest compiler for this codebase (incomplete
types, namespace boundaries) — if you can't build on Windows, at least keep
self-referential structs using `std::vector<Offset>` indirection (see `refactor_history.md`
§4 lessons).
