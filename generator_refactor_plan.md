# FastFHIR Generator Refactor — Master Plan

> **Status:** Planning · **Owner:** multi-agent sprint · **Scope:** restructure `tools/generator/` → root-level `generator/` package; split the 2585-line `ffc.py` monolith into single-concern modules.
> **Hard invariant:** generated C++ output is **byte-identical** before and after every change. The C++ is the user's hand-tuned house style — the generator's job is to reproduce it exactly. We refactor *the Python that emits*, never the emitted bytes.

---

## 0. Context for any agent picking this up cold

| Fact | Value |
|---|---|
| Workspace root | `/Users/RyanLandvater/Programming_Projects/FastFHIR` |
| Refactor target | `tools/generator/*.py` (5 files, 3383 lines; `ffc.py` = 2585) |
| Destination | root-level `generator/` package |
| Output dir (unchanged) | `generated_src/` |
| Pipeline entry today | `tools/generator/make_lib.py::main()` |
| Build invocation | CMake `execute_process(... make_lib.py)`; Bazel genrule |
| Golden rule | **Byte-identical generated output** after every sprint |

### Two distinct style regimes (do not conflate)

1. **Generated C++** (`generated_src/`, plus all `src/`/`include/`): the user's hand-tuned style. **Sacred.** The generator emits string literals that must reproduce it to the byte. Never "improve" emitted C++.
2. **Generator Python** (`generator/`): agentic, optimized for **readability + strong typing that mirrors C++ conventions** (dataclasses, enums, full type hints). This is what we clean up.

---

## 1. Code-structure & style review (current generator)

Verified findings from the existing source — these motivate the split and seed cleanup tasks.

### 1.1 `ffc.py` — 2585-line monolith, ≥8 unrelated concerns
Its own banner comments already mark the seams:

| Lines | Concern | Target module |
|---|---|---|
| 33–199 | `TYPE_MAP`, `sanitize_fhir_type`, scalar/string sets | `model/type_map.py` |
| 201–597 | `extract_structure_definition`, name resolvers, field-kind exprs | `model/structure.py` |
| 599–913 | `generate_eager_deserializer`, size/store field emit | `emit/deserialize.py`, `emit/store.py` |
| 345–446 | `generate_lazy_view_struct`, field_info, reflection dispatch | `emit/views.py` |
| 554–597 | `generate_resource_traits_header` | `emit/traits.py` |
| 915–1129 | `merge_fhir_versions` (R4/R5 unification) | `model/merge.py` |
| 1151–1727 | `generate_ingest_mappings` | `emit/ingest_mappings.py` |
| 1729–2007 | `emit_python_fields/_ast/_stubs` | `bindings/python_fields.py` |
| 2009–2356 | `compile_fhir_library` (driver) | `library.py` |
| 2358–2585 | WASM codec C emitter | `emit/extensions_wasm.py` |

**Verdict:** one file doing model-building, eight kinds of C++ emission, Python-stub emission, and orchestration. Untestable in isolation.

### 1.2 Confirmed concrete violations (seed tasks)

| ID | Issue | Evidence | Fix sprint |
|---|---|---|---|
| SV-1 | `_write_if_changed` **duplicated verbatim** | `ffc.py:19` **and** `make_lib.py:6` | S1 → `emit/header.py` |
| SV-2 | **Indentation split**: `make_lib.py` tab-indented (135 lines), `ffc/ffd/ffcs` space-indented | `awk` scan | S1 |
| SV-3 | `ffc.py` has **16 stray tab lines** mixed into a space file | `awk` scan | S1 |
| SV-4 | Orchestrator carries an **emitter**: `generate_known_extensions` (105 lines) lives in `make_lib.py`, not an emit module | `make_lib.py:68` | S5 |
| SV-5 | **No type hints / dataclasses** — layout passed as bare `dict`/`list`; fails the "strong-typing mirrors C++" goal | `compile_fhir_library` signature | S2 |
| SV-6 | **Model and emit interleaved** — can't unit-test V-Table layout without diffing emitted C++ strings | `ffc.py` §201–913 | S2–S4 |
| SV-7 | Dual import shim (`if __package__:`) duplicated | `make_lib.py:18–31` | S1 |

---

## 2. Target architecture

### 2.1 Directory tree (destination)

```
generator/                          # promoted to root; first-class subsystem
├── __init__.py                     # re-exports stable public API (5 symbols)
├── __main__.py                     # `python -m generator` → pipeline.run()
│
├── pipeline.py                     # orchestrator ONLY (was make_lib.main). No codegen.
├── specs.py                        # spec fetch/cleanup (was fetch_specs.py)
│
├── model/                          # FHIR schema → typed layout model. NO C++ strings.
│   ├── __init__.py
│   ├── types.py                    # @dataclass Field, Block, Layout + TypeInfo enum
│   ├── type_map.py                 # TYPE_MAP, sanitize_fhir_type, scalar/string sets
│   ├── structure.py                # extract_structure_definition, name resolvers
│   └── merge.py                    # merge_fhir_versions (R4/R5 unification)
│
├── emit/                           # C++/C emitters — one wire concern each.
│   ├── __init__.py
│   ├── header.py                   # auto_header, write_if_changed (single source of truth)
│   ├── views.py                    # lazy-view struct, field_info, reflection dispatch
│   ├── deserialize.py              # eager deserializer
│   ├── store.py                    # store_fields / size_fields
│   ├── traits.py                   # resource traits header
│   ├── dictionary.py               # was ffd.py
│   ├── codesystems.py              # was ffcs.py
│   ├── ingest_mappings.py          # ingest mappings
│   ├── extensions_known.py         # known-extension URL table (moved out of make_lib)
│   └── extensions_wasm.py          # WASM codec C emitter
│
├── bindings/
│   ├── __init__.py
│   └── python_fields.py            # emit_python_fields / _ast / _stubs
│
└── library.py                      # compile_fhir_library (per-resource driver)
```

### 2.2 Dependency direction (enforced, acyclic)

```
specs ─► pipeline ◄─ library
                       │
        ┌──────────────┼───────────────┐
        ▼              ▼                ▼
      model/   ◄──   emit/   ──►   bindings/
        │              │
        └──► emit/header (write_if_changed, auto_header) ◄─ everyone
```

**Rule:** `model/` imports nothing from `emit/` or `bindings/`. Emitters consume model dataclasses and produce strings. `pipeline.py`/`library.py` are the only orchestrators. This kills SV-6 (model/emit interleave) structurally — a cycle becomes an import error.

### 2.3 Stable public API (`__init__.py`)

Any existing `from generator import X` must keep working. Re-export exactly the 5 symbols `pipeline` consumes today:

```python
# generator/__init__.py
# Stable façade. External callers (build, tests) import from here, never from
# submodules — so internal reorganisation never breaks them.
from .library import compile_fhir_library, resolve_production_resources
from .model.structure import discover_versions          # was ffc._discover_versions
from .model.type_map import PRODUCTION_TYPES
from .emit.header import auto_header

__all__ = [
    "compile_fhir_library", "resolve_production_resources",
    "discover_versions", "PRODUCTION_TYPES", "auto_header",
]
```

> Note: `_discover_versions` loses its leading underscore on promotion to public API — it *is* the public version-discovery entry point now. Document the rename in the sprint that moves it.

---

## 3. Python style rules for the new generator

These mirror the user's intent: **readable Python that stays close to C++ strong-typing conventions.** Every example below is the target; review generated code against it.

### 3.1 Typed layout model replaces bare dicts (fixes SV-5/SV-6)

**Before (current — untyped, mirrors nothing):**
```python
# layout is a list[dict] with stringly-typed keys scattered across 2000 lines
for f in layout:
    if f['kind'] == 'scalar':
        off = f['offset']; ...
```

**After (target — strong types mirroring the C++ structs they describe):**
```python
# generator/model/types.py
from dataclasses import dataclass
from enum import Enum, auto

class FieldKind(Enum):
    """Mirrors the C++ FF_FieldKey kind enum. One Python enum ⇄ one wire concept."""
    SCALAR        = auto()   # in-place V-Table slot
    BLOCK_OFFSET  = auto()   # 8-byte arena offset to child block
    INLINE_ARRAY  = auto()   # entries() + index*stride
    OFFSET_ARRAY  = auto()   # array of arena offsets
    STRING        = auto()   # FF_String

@dataclass(frozen=True)
class Field:
    """One V-Table field. `offset`/`stride` are wire-format constants — never reorder."""
    name: str            # FHIR field name (source-of-truth identifier)
    cpp_type: str        # emitted C++ type, e.g. "uint32_t"
    kind: FieldKind
    offset: int          # byte offset within the V-Table (wire-format; permanent)
    recovery_tag: str    # RECOVER_FF_* name resolved by the C++ compiler
    stride: int = 0      # element stride for array kinds; 0 for scalars

@dataclass(frozen=True)
class Block:
    """A generated C++ block (resource, datatype, or backbone element)."""
    struct_name: str
    header_size: int     # V-Table total size for this FHIR revision (wire constant)
    fields: tuple[Field, ...]
```

**Why frozen dataclasses:** immutability mirrors the wire-format permanence contract — once a `Field.offset` is set, nothing in the pipeline may mutate it. The type system now enforces what the architecture doc states in prose.

### 3.2 Emitters: pure functions `model → str`. No I/O inside.

```python
# generator/emit/deserialize.py
from generator.model.types import Block, FieldKind

def emit_eager_deserializer(block: Block, data_name: str) -> str:
    """Return the C++ body for `block`'s eager deserializer.

    Pure: takes a typed Block, returns a C++ string. No file writes, no globals.
    The emitted text is the user's hand-style C++ and must stay byte-identical —
    do not reflow, re-indent, or 'tidy' the produced string.
    """
    lines: list[str] = [f"    {data_name} data;"]
    for f in block.fields:
        # One branch per wire kind — matches the C++ read path exactly.
        if f.kind is FieldKind.SCALAR:
            lines.append(f"    data.{f.name} = LOAD_{f.cpp_type.upper()}(...);")
        # ... remaining kinds ...
    return "\n".join(lines) + "\n"
```

**Rules for every emitter:**
- Signature is `(model_object, *opts) -> str`. **No file writes** — only `write_if_changed` (in `pipeline`/`library`) touches disk. Centralizes I/O, makes emitters trivially unit-testable.
- The returned string is **verbatim user C++**. Comment any literal block: `# emits FF_HEADER read — see architecture.md §4`.
- No mutable default args. No `from x import *`. Full type hints on every public function (the C++-discipline goal).

### 3.3 Single source of truth for shared helpers (fixes SV-1)

```python
# generator/emit/header.py — the ONLY definition of these two.
def write_if_changed(path: str, content: str, encoding: str = "utf-8") -> None:
    """Write only on change, preserving mtime on no-op regen (incremental builds)."""
    ...

auto_header = "// AUTO-GENERATED by FastFHIR generator. Do not edit.\n"
```
Everyone imports from here. Delete both duplicate `_write_if_changed` bodies.

### 3.4 Formatting (fixes SV-2/SV-3)
- **4-space indent everywhere.** Convert `make_lib.py` (tabs) and the 16 stray tab lines in `ffc.py`. Add `pyproject.toml` `[tool.black]` + `[tool.ruff]` so it's enforced, not hoped for.

---

## 4. Sprint plan

Seven sprints. Each is independently shippable, ends green, and is safe to hand to a fresh agent. **Every sprint ends with the golden-output gate passing** (generated `generated_src/` byte-identical to the baseline).

### Sprint sequencing & dependencies

```
S0 (baseline+harness) ─► S1 (package skeleton + shared helpers)
                           │
                           ▼
                         S2 (model/ extraction) ─► S3 (emit/ core) ─► S4 (emit/ remainder)
                                                                        │
                                                                        ▼
                                                  S5 (bindings + extensions) ─► S6 (cutover + cleanup)
```

S2→S3→S4 are strictly ordered (emitters depend on the typed model). S0/S1 are prerequisites for everyone.

---

### S0 — Baseline & golden harness (BLOCKING; do first)
**Goal:** freeze a byte-exact reference of generated output so every later sprint can prove zero drift.
**No source moves in this sprint.**

- Capture baseline: run the current pipeline, snapshot `generated_src/` to `tests/golden/baseline/`.
- Write `tests/generator/test_golden_output.py`: regenerate into a temp dir, assert byte-identical to baseline (per-file diff on mismatch).
- Write `tests/generator/conftest.py` fixture that runs the generator once per session into `tmp_path`.
- CI/local command documented: `pytest tests/generator -q`.

**Milestone M0:** golden test passes against unmodified generator. This is the contract for all later sprints.
**Gate:** `pytest tests/generator/test_golden_output.py` green.

---

### S1 — Package skeleton + shared helpers
**Goal:** create root `generator/` with the dir tree, `__init__`/`__main__`, and the single-source-of-truth helpers. Old `tools/generator/` still works (re-export shim).

- Create `generator/` tree (empty `__init__.py` in each subpackage).
- `generator/emit/header.py`: canonical `write_if_changed` + `auto_header` (fixes SV-1).
- `generator/__main__.py` → calls `pipeline.run()` (thin).
- `generator/pipeline.py`: copy of `make_lib.main()` body, **4-space**, import shim removed (fixes SV-2/SV-7), `generate_known_extensions` still imported from old location *temporarily*.
- Reformat to 4-space; add `pyproject.toml` (`black`, `ruff`).
- `generator/__init__.py` re-exports the 5 public symbols (initially still pointing at old modules via thin imports).
- Leave `tools/generator/` in place; no deletions yet.

**Milestone M1:** `python -m generator` produces byte-identical output to `tools/generator/make_lib.py`.
**Gate:** golden test green via **both** entry points. `ruff check generator/` clean.

---

### S2 — `model/` extraction (typed layout)
**Goal:** move all schema→model logic into `generator/model/`, introduce dataclasses (fixes SV-5/SV-6). Emitters in `ffc.py` temporarily import the new model.

- `model/types.py`: `FieldKind`, `Field`, `Block`, `Layout` dataclasses (§3.1).
- `model/type_map.py`: `TYPE_MAP`, `sanitize_fhir_type`, scalar/string sets, `PRODUCTION_TYPES`.
- `model/structure.py`: `extract_structure_definition`, name resolvers, `discover_versions` (rename from `_discover_versions`).
- `model/merge.py`: `merge_fhir_versions`.
- Add `tests/generator/test_model.py`: unit-test layout construction **without** emitting C++ (e.g. assert a known resource's field offsets/strides). This is the first test that catches wire-format drift directly.
- `ffc.py` updated to import from `model/`; behavior unchanged.

**Milestone M2:** model unit tests pass; golden still byte-identical.
**Gate:** `test_golden_output` + `test_model` green. No `emit`→`model` cycle (`ruff`/import check).

---

### S3 — `emit/` core emitters
**Goal:** extract the hot-path C++ emitters into pure `model → str` functions.

- `emit/views.py`, `emit/deserialize.py`, `emit/store.py`, `emit/traits.py`.
- Each: signature `(Block, ...) -> str`, no I/O (§3.2).
- `tests/generator/test_emit_core.py`: feed a fixture `Block`, assert emitted C++ string matches a small golden snippet (catches emitter drift at unit granularity).
- `ffc.py` now delegates these sections to `emit/`.

**Milestone M3:** core emitters isolated; golden byte-identical.
**Gate:** golden + emit-core snapshot tests green.

---

### S4 — `emit/` remaining emitters
**Goal:** extract dictionary, codesystems, ingest mappings.

- `emit/dictionary.py` (was `ffd.py`), `emit/codesystems.py` (was `ffcs.py`), `emit/ingest_mappings.py`.
- Preserve the hash-based code assignment and recovery-tag parsing exactly (wire-critical — see `audit.plan.md` §2).
- `tests/generator/test_emit_dict.py`: assert `_code_hash` stability (same string → same uint32) and collision guard.

**Milestone M4:** all non-binding emitters isolated; golden byte-identical.
**Gate:** golden + dict-stability tests green.

---

### S5 — bindings + extensions
**Goal:** extract Python-stub emission and both extension emitters; fix SV-4.

- `bindings/python_fields.py`: `emit_python_fields/_ast/_stubs`.
- `emit/extensions_wasm.py`: WASM codec C emitter (`ffc.py` §2358).
- `emit/extensions_known.py`: `generate_known_extensions` **moved out of** `make_lib.py`/`pipeline.py` (fixes SV-4). `pipeline.py` now calls `emit.extensions_known.generate(...)`.
- `tests/generator/test_bindings.py`: assert a known resource's `.pyi` stub matches golden.

**Milestone M5:** orchestrator carries zero emitters; golden byte-identical.
**Gate:** golden + bindings tests green.

---

### S6 — cutover & cleanup
**Goal:** delete `tools/generator/`, repoint build, finalize.

- `library.py`: final `compile_fhir_library` driver importing only `model/`+`emit/`+`bindings/`.
- Update **CMake** `execute_process` and **Bazel** genrule: invoke `python -m generator` (path-independent).
- Update docs: `architecture.md` §9, `README.md` generator table, `.github/prompts/WASM_extensions.prompt.md`.
- **Delete** `tools/generator/` (all 5 files). Remove `tools/` if now empty (confirm exporter/compactor/ingestor still under `tools/`).
- Remove the temporary re-export shims.
- Final `ruff`/`black` pass; confirm no `from x import *`, no mutable defaults, full type hints on public functions.

**Milestone M6 (RELEASE):** old generator gone, build green via `python -m generator`, golden byte-identical, all unit tests green.
**Gate:** full suite + a clean-tree CMake configure that regenerates and compiles `fastfhir`.

---

## 5. Full todo list (copy into tracker; check off across agents)

> **Gate-model deviation (alpha):** the byte-identical golden harness was
> **superseded** by the narrower **wire-witness gate** (`wire_witness.py` +
> `test_wire_format.py`). Contract = wire-convention stability (recovery tags,
> hash-based dictionary codes, V-Table field order + size constants), NOT
> source-text byte-identity. Golden-named items below are re-scoped to the wire
> gate; genuinely-open work is consolidated in §10.

### S0 — Baseline & harness  *(re-scoped: wire-witness, not byte-golden)*
- [x] S0.1 ~~snapshot → `tests/golden/baseline/`~~ → superseded by `wire_witness.py` projection
- [x] S0.2 `tests/generator/conftest.py`: session fixture regenerating into `tmp_path`
- [x] S0.3 ~~`test_golden_output.py`~~ → `test_wire_format.py` (tags/codes/vtables)
- [ ] S0.4 Confirm deterministic output across 2 runs *(not yet re-verified — §10.3)*
- [ ] **M0 gate:** wire gate green — **BLOCKED**: baseline `wire_witness.json` not committed → gate skips (§10.1)

### S1 — Package skeleton
- [x] S1.1 Create `generator/` tree + empty `__init__.py` per subpackage
- [x] S1.2 `emit/header.py`: canonical `write_if_changed` + `auto_header` (SV-1)
- [x] S1.3 `pipeline.py`: port `make_lib.main()`, 4-space, drop import shim (SV-2/7)
- [x] S1.4 `__main__.py` → `pipeline.run()`
- [x] S1.5 `__init__.py` re-export 5 public symbols
- [x] S1.6 `pyproject.toml`: black + ruff config
- [x] S1.7 Reformat all generator Python to 4-space (SV-3)
- [ ] **M1 gate:** `python -m generator` runs; `ruff` clean — **BLOCKED**: ruff/black not installed (§10.2)

### S2 — model/
- [x] S2.1 `model/types.py`: `Field`/`Block` dataclasses (SV-5)
- [x] S2.2 `model/type_map.py`: `TYPE_MAP`, sanitizers, `PRODUCTION_TYPES`
- [x] S2.3 `model/structure.py`: extraction + resolvers + `discover_versions` (rename)
- [x] S2.4 `model/merge.py`: `merge_fhir_versions`
- [x] S2.5 `tests/generator/test_model.py`: model unit asserts
- [x] S2.6 ~~Repoint `ffc.py` imports~~ → `ffc.py` deleted in S6; emitters consume model directly
- [ ] **M2 gate:** model tests green — not yet executed (§10)

### S3 — emit/ core
- [x] S3.1 `emit/views.py` (lazy view, field_info, reflection dispatch)
- [x] S3.2 `emit/deserialize.py` (eager deserializer)
- [x] S3.3 `emit/store.py` (store/size fields)
- [x] S3.4 `emit/traits.py` (resource traits)
- [ ] S3.5 `tests/generator/test_emit_core.py`: fixture-Block snapshot — **NOT CREATED** (folded into wire gate; §10)
- [x] S3.6 ~~`ffc.py` delegates~~ → emitters are standalone pure modules
- [ ] **M3 gate:** wire gate green — blocked on M0

### S4 — emit/ remainder
- [x] S4.1 `emit/dictionary.py` (was `ffd.py`); `_code_hash` preserved
- [x] S4.2 `emit/codesystems.py` (was `ffcs.py`)
- [x] S4.3 `emit/ingest_mappings.py`
- [x] S4.4 collision guard added to `_code_hash` assignment (audit W-1) — *inline fail-loud; dedicated `test_emit_dict.py` not created*
- [x] **(audit C-1)** fixed `FF_*_Resolve` f-string placeholder leak
- [x] **(audit C-2)** fixed `FF_Dictionary::resolve` N-fold redefinition
- [ ] **M4 gate:** wire gate green — blocked on M0

### S5 — bindings + extensions
- [x] S5.1 `bindings/python_fields.py` (`emit_python_fields/_ast/_stubs`)
- [x] S5.2 `emit/extensions_wasm.py` (WASM codec)
- [x] S5.3 `emit/extensions_known.py`; moved out of `pipeline.py` (SV-4)
- [ ] S5.4 `tests/generator/test_bindings.py`: `.pyi` golden — **NOT CREATED** (§10)
- [x] **M5 gate:** orchestrator emitter-free (verified — `pipeline.py` imports only)

### S6 — cutover
- [x] S6.1 `library.py`: final driver, model/emit/bindings only
- [x] S6.2 CMake `execute_process` → `python -m generator`
- [n/a] S6.3 Bazel genrule — no Bazel build in repo (no `BUILD`/`WORKSPACE`)
- [~] S6.4 Docs: `README.md` updated (registry config); `architecture.md` §9 / WASM prompt not yet checked (§10)
- [x] S6.5 Delete `tools/generator/`; remove shims
- [ ] S6.6 Final black/ruff; verify no `import *`, no mutable defaults — **BLOCKED** on ruff install (§10.2)
- [ ] S6.7 Clean-tree CMake configure → regenerate → compile `fastfhir` — not yet run (§10.4)
- [ ] **M6 (RELEASE):** wire gate + build green via `python -m generator` — blocked on M0/S6.7

---

## 6. Test strategy (catch breaks early)

> **Note (alpha):** the byte-identical golden tier below was **not** built; the
> wire-witness gate (§10.1) replaces it. The §6.3 model-unit and §6.4
> determinism tiers remain the intended direction. Snippets are illustrative of
> intent, not the shipped implementation.

### 6.1 Three-tier test pyramid

| Tier | File | Catches | Runs |
|---|---|---|---|
| **Wire witness** (top guard) | `wire_witness.py` + `test_wire_format.py` | Drift in recovery tags / dict codes / V-Table layout | Every gate |
| **Model units** | `test_model.py` | Wrong offset/stride/kind at the source — wire-format bugs | S2+ |
| **(planned) emitter snapshots** | `test_emit_*.py` | Per-emitter drift — **not yet created** | — |

### 6.2 Determinism check (run twice, diff)
Generators that iterate `set`/`dict` can emit nondeterministic order. Run the
pipeline **twice** and diff — if it isn't stable against itself, fix ordering
(`sorted(...)`) before trusting the witness baseline. Sorting is in place
(`mappings`, code table, version configs) but the end-to-end double-run is not
yet re-verified (§10.3).

### 6.3 C++ build smoke (final milestone only)
M6 requires a clean-tree CMake configure that regenerates and **compiles
`fastfhir`** — proves the emitted C++ still builds. The regex witness does NOT
check compilability, so this is a distinct, required gate (§10.4).

---

## 7. Risk register

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| Nondeterministic emit poisons baseline | Med | High | §6.2 double-run diff before trusting witness |
| `_code_hash` collision → silent wire data loss | Med | Critical | fail-loud collision guard in `emit/dictionary.py` (audit W-1) |
| `_code_hash`/recovery-tag drift | Low | Critical | recovery tags parsed from `FF_Recovery.hpp`; hash frozen behind a version bump |
| Set-ordering changes across Python versions | Low | High | `sorted()` everywhere ordering hits the wire |
| Build invocation path breaks | Med | Med | `python -m generator` (path-independent); test clean configure (§10.4) |
| Wire gate inert (no baseline) | High (current) | High | commit `wire_witness.json`; drop conftest fallback (§10.1) |
| `tools/` removal breaks other CLIs | Low | Med | exporter/compactor/ingestor confirmed still present under `tools/` |

---

## 8. Multi-agent handoff protocol

**Golden rules for any agent:**
1. **Never edit generated C++ or emitted string literals to "improve" them.** The C++ is the user's house style; reproduce byte-for-byte.
2. **Never `git checkout`.** Correct surgically with IDE edits.
3. **Use IDE edit tools**, not CLI `sed`/`echo` for file edits.
4. **A sprint is atomic:** start only when the prior milestone gate is green. Verify with `pytest tests/generator -q` *before* writing code, to inherit a known-green base.
5. **One sprint = disjoint module set.** No two in-flight sprints touch the same file.

**Per-sprint handoff checklist (paste into PR/commit):**
```
Sprint: S_
Modules created/moved: ...
Tests added: ...
Golden gate: PASS/FAIL
ruff/black: CLEAN
Public API (__init__) unchanged: YES/NO (if NO, list)
Next sprint unblocked: S_
```

**Context recovery for a cold agent:** read this plan §0–§2, run the golden test to confirm current state, then `grep` the todo list (§5) for the first unchecked item whose dependencies are met.

---

## 9. Definition of done
- `tools/generator/` deleted; root `generator/` is the sole generator. ✅
- `python -m generator` is the only invocation; CMake updated (no Bazel in repo). ✅
- Generated `generated_src/` preserves **wire convention** (alpha: tags/codes/vtable layout stable — not byte-identity). ⏳ gate not yet armed (§10).
- `ffc.py` monolith decomposed: model/emit/bindings separated; no import cycles. ✅
- All Python: 4-space, full type hints, no `import *`, no mutable defaults, no duplicated helpers. ⏳ ruff not yet run (§10).
- Full test suite green; clean-tree C++ build of `fastfhir` succeeds. ⏳ not yet run (§10).

---

## 10. Remaining work (post-structural-refactor)

The S1–S6 module decomposition is **done** and the two audit compile-bugs (C-1, C-2)
plus the hash-collision guard (W-1) are fixed. What remains is **arming the safety net
and final verification** — none of it is more module-moving.

### 10.1 Arm the wire gate  *(highest priority — currently the gate skips)*
- [ ] Generate + commit the baseline: `python -m tests.generator.wire_witness generated_src tests/generator/golden/wire_witness.json`. Until this file exists, all three `test_wire_format.py` tests `pytest.skip` and prove nothing.
- [ ] Drop the `generated_src` fallback in `tests/generator/conftest.py::regenerated_dir`; post-cutover it must **hard-fail** when `python -m generator` produces no output (otherwise a broken generator reads as green).

### 10.2 Lint / format (M1 / S6.6)
- [ ] Install `ruff` + `black` into `.venv` (declared in `pyproject.toml`, not present).
- [ ] `ruff check generator/ tests/generator/` clean; `black --check` clean.
- [ ] Confirm no `import *`, no mutable default args across the package.

### 10.3 Determinism (S0.4)
- [ ] Run `python -m generator` twice into two temp dirs; diff. Must be byte-stable against itself (sorting is in place but unverified end-to-end).

### 10.4 Build smoke (S6.7 / M6)
- [ ] Clean-tree CMake configure with `FASTFHIR_RUN_GENERATOR=ON` → regenerate → compile `fastfhir`. Proves the emitted C++ (incl. the C-1/C-2 dictionary fixes) actually builds. The wire witness does **not** check compilability.

### 10.5 Optional hardening (from audit.md)
- [ ] W-2: parse `FF_CODE_NULL` from `include/FF_Primitives.hpp` instead of re-declaring `0xFFFFFFFF` in `emit/dictionary.py` (match the recovery-tag pattern, the single-source-of-truth model).
- [ ] G-2: add a generated-C++ compile smoke test to the Python suite (catches the C-1/C-2 class the regex witness can't see).
- [ ] H-2: add `__pycache__/` + `*.pyc` to `.gitignore`.
- [ ] H-3: fix the stale "path bridge / legacy tools/generator" comment in `generator/__init__.py` (no bridge exists post-cutover).
- [ ] H-1: stage `generator/`, `tests/generator/`, `pyproject.toml` atomically with the `tools/generator/` deletion + CMake repoint (the replacement is currently untracked).

<!-- ARBITER_TRIM_BELOW: everything after this line is a stale duplicate of §2–§9 -->

## 2. Target architecture

### 2.1 Directory tree (destination)

```
generator/                          # promoted to root; first-class subsystem
├── __init__.py                     # re-exports stable public API (5 symbols)
├── __main__.py                     # `python -m generator` → pipeline.run()
│
├── pipeline.py                     # orchestrator ONLY (was make_lib.main). No codegen.
├── specs.py                        # spec fetch/cleanup (was fetch_specs.py)
│
├── model/                          # FHIR schema → typed layout model. NO C++ strings.
│   ├── __init__.py
│   ├── types.py                    # @dataclass Field, Block, Layout + TypeInfo enum
│   ├── type_map.py                 # TYPE_MAP, sanitize_fhir_type, scalar/string sets
│   ├── structure.py                # extract_structure_definition, name resolvers
│   └── merge.py                    # merge_fhir_versions (R4/R5 unification)
│
├── emit/                           # C++/C emitters — one wire concern each.
│   ├── __init__.py
│   ├── header.py                   # auto_header, write_if_changed (single source of truth)
│   ├── views.py                    # lazy-view struct, field_info, reflection dispatch
│   ├── deserialize.py              # eager deserializer
│   ├── store.py                    # store_fields / size_fields
│   ├── traits.py                   # resource traits header
│   ├── dictionary.py               # was ffd.py
│   ├── codesystems.py              # was ffcs.py
│   ├── ingest_mappings.py          # ingest mappings
│   ├── extensions_known.py         # known-extension URL table (moved out of make_lib)
│   └── extensions_wasm.py          # WASM codec C emitter
│
├── bindings/
│   ├── __init__.py
│   └── python_fields.py            # emit_python_fields / _ast / _stubs
│
└── library.py                      # compile_fhir_library (per-resource driver)
```

### 2.2 Dependency direction (enforced, acyclic)

```
specs ─► pipeline ◄─ library
                       │
        ┌──────────────┼───────────────┐
        ▼              ▼                ▼
      model/   ◄──   emit/   ──►   bindings/
        │              │
        └──► emit/header (write_if_changed, auto_header) ◄─ everyone
```

**Rule:** `model/` imports nothing from `emit/` or `bindings/`. Emitters consume model dataclasses and produce strings. `pipeline.py`/`library.py` are the only orchestrators. This kills SV-6 (model/emit interleave) structurally — a cycle becomes an import error.

### 2.3 Stable public API (`__init__.py`)

Any existing `from generator import X` must keep working. Re-export exactly the 5 symbols `pipeline` consumes today:

```python
# generator/__init__.py
# Stable façade. External callers (build, tests) import from here, never from
# submodules — so internal reorganisation never breaks them.
from .library import compile_fhir_library, resolve_production_resources
from .model.structure import discover_versions          # was ffc._discover_versions
from .model.type_map import PRODUCTION_TYPES
from .emit.header import auto_header

__all__ = [
    "compile_fhir_library", "resolve_production_resources",
    "discover_versions", "PRODUCTION_TYPES", "auto_header",
]
```

> Note: `_discover_versions` loses its leading underscore on promotion to public API — it *is* the public version-discovery entry point now. Document the rename in the sprint that moves it.

---

## 3. Python style rules for the new generator

These mirror the user's intent: **readable Python that stays close to C++ strong-typing conventions.** Every example below is the target; review generated code against it.

### 3.1 Typed layout model replaces bare dicts (fixes SV-5/SV-6)

**Before (current — untyped, mirrors nothing):**
```python
# layout is a list[dict] with stringly-typed keys scattered across 2000 lines
for f in layout:
    if f['kind'] == 'scalar':
        off = f['offset']; ...
```

**After (target — strong types mirroring the C++ structs they describe):**
```python
# generator/model/types.py
from dataclasses import dataclass
from enum import Enum, auto

class FieldKind(Enum):
    """Mirrors the C++ FF_FieldKey kind enum. One Python enum ⇄ one wire concept."""
    SCALAR        = auto()   # in-place V-Table slot
    BLOCK_OFFSET  = auto()   # 8-byte arena offset to child block
    INLINE_ARRAY  = auto()   # entries() + index*stride
    OFFSET_ARRAY  = auto()   # array of arena offsets
    STRING        = auto()   # FF_String

@dataclass(frozen=True)
class Field:
    """One V-Table field. `offset`/`stride` are wire-format constants — never reorder."""
    name: str            # FHIR field name (source-of-truth identifier)
    cpp_type: str        # emitted C++ type, e.g. "uint32_t"
    kind: FieldKind
    offset: int          # byte offset within the V-Table (wire-format; permanent)
    recovery_tag: str    # RECOVER_FF_* name resolved by the C++ compiler
    stride: int = 0      # element stride for array kinds; 0 for scalars

@dataclass(frozen=True)
class Block:
    """A generated C++ block (resource, datatype, or backbone element)."""
    struct_name: str
    header_size: int     # V-Table total size for this FHIR revision (wire constant)
    fields: tuple[Field, ...]
```

**Why frozen dataclasses:** immutability mirrors the wire-format permanence contract — once a `Field.offset` is set, nothing in the pipeline may mutate it. The type system now enforces what the architecture doc states in prose.

### 3.2 Emitters: pure functions `model → str`. No I/O inside.

```python
# generator/emit/deserialize.py
from generator.model.types import Block, FieldKind

def emit_eager_deserializer(block: Block, data_name: str) -> str:
    """Return the C++ body for `block`'s eager deserializer.

    Pure: takes a typed Block, returns a C++ string. No file writes, no globals.
    The emitted text is the user's hand-style C++ and must stay byte-identical —
    do not reflow, re-indent, or 'tidy' the produced string.
    """
    lines: list[str] = [f"    {data_name} data;"]
    for f in block.fields:
        # One branch per wire kind — matches the C++ read path exactly.
        if f.kind is FieldKind.SCALAR:
            lines.append(f"    data.{f.name} = LOAD_{f.cpp_type.upper()}(...);")
        # ... remaining kinds ...
    return "\n".join(lines) + "\n"
```

**Rules for every emitter:**
- Signature is `(model_object, *opts) -> str`. **No file writes** — only `write_if_changed` (in `pipeline`/`library`) touches disk. Centralizes I/O, makes emitters trivially unit-testable.
- The returned string is **verbatim user C++**. Comment any literal block: `# emits FF_HEADER read — see architecture.md §4`.
- No mutable default args. No `from x import *`. Full type hints on every public function (the C++-discipline goal).

### 3.3 Single source of truth for shared helpers (fixes SV-1)

```python
# generator/emit/header.py — the ONLY definition of these two.
def write_if_changed(path: str, content: str, encoding: str = "utf-8") -> None:
    """Write only on change, preserving mtime on no-op regen (incremental builds)."""
    ...

auto_header = "// AUTO-GENERATED by FastFHIR generator. Do not edit.\n"
```
Everyone imports from here. Delete both duplicate `_write_if_changed` bodies.

### 3.4 Formatting (fixes SV-2/SV-3)
- **4-space indent everywhere.** Convert `make_lib.py` (tabs) and the 16 stray tab lines in `ffc.py`. Add `pyproject.toml` `[tool.black]` + `[tool.ruff]` so it's enforced, not hoped for.

---

## 4. Sprint plan

Seven sprints. Each is independently shippable, ends green, and is safe to hand to a fresh agent. **Every sprint ends with the golden-output gate passing** (generated `generated_src/` byte-identical to the baseline).

### Sprint sequencing & dependencies

```
S0 (baseline+harness) ─► S1 (package skeleton + shared helpers)
                           │
                           ▼
                         S2 (model/ extraction) ─► S3 (emit/ core) ─► S4 (emit/ remainder)
                                                                        │
                                                                        ▼
                                                  S5 (bindings + extensions) ─► S6 (cutover + cleanup)
```

S2→S3→S4 are strictly ordered (emitters depend on the typed model). S0/S1 are prerequisites for everyone.

---

### S0 — Baseline & golden harness (BLOCKING; do first)
**Goal:** freeze a byte-exact reference of generated output so every later sprint can prove zero drift.
**No source moves in this sprint.**

- Capture baseline: run the current pipeline, snapshot `generated_src/` to `tests/golden/baseline/`.
- Write `tests/generator/test_golden_output.py`: regenerate into a temp dir, assert byte-identical to baseline (per-file diff on mismatch).
- Write `tests/generator/conftest.py` fixture that runs the generator once per session into `tmp_path`.
- CI/local command documented: `pytest tests/generator -q`.

**Milestone M0:** golden test passes against unmodified generator. This is the contract for all later sprints.
**Gate:** `pytest tests/generator/test_golden_output.py` green.

---

### S1 — Package skeleton + shared helpers
**Goal:** create root `generator/` with the dir tree, `__init__`/`__main__`, and the single-source-of-truth helpers. Old `tools/generator/` still works (re-export shim).

- Create `generator/` tree (empty `__init__.py` in each subpackage).
- `generator/emit/header.py`: canonical `write_if_changed` + `auto_header` (fixes SV-1).
- `generator/__main__.py` → calls `pipeline.run()` (thin).
- `generator/pipeline.py`: copy of `make_lib.main()` body, **4-space**, import shim removed (fixes SV-2/SV-7), `generate_known_extensions` still imported from old location *temporarily*.
- Reformat to 4-space; add `pyproject.toml` (`black`, `ruff`).
- `generator/__init__.py` re-exports the 5 public symbols (initially still pointing at old modules via thin imports).
- Leave `tools/generator/` in place; no deletions yet.

**Milestone M1:** `python -m generator` produces byte-identical output to `tools/generator/make_lib.py`.
**Gate:** golden test green via **both** entry points. `ruff check generator/` clean.

---

### S2 — `model/` extraction (typed layout)
**Goal:** move all schema→model logic into `generator/model/`, introduce dataclasses (fixes SV-5/SV-6). Emitters in `ffc.py` temporarily import the new model.

- `model/types.py`: `FieldKind`, `Field`, `Block`, `Layout` dataclasses (§3.1).
- `model/type_map.py`: `TYPE_MAP`, `sanitize_fhir_type`, scalar/string sets, `PRODUCTION_TYPES`.
- `model/structure.py`: `extract_structure_definition`, name resolvers, `discover_versions` (rename from `_discover_versions`).
- `model/merge.py`: `merge_fhir_versions`.
- Add `tests/generator/test_model.py`: unit-test layout construction **without** emitting C++ (e.g. assert a known resource's field offsets/strides). This is the first test that catches wire-format drift directly.
- `ffc.py` updated to import from `model/`; behavior unchanged.

**Milestone M2:** model unit tests pass; golden still byte-identical.
**Gate:** `test_golden_output` + `test_model` green. No `emit`→`model` cycle (`ruff`/import check).

---

### S3 — `emit/` core emitters
**Goal:** extract the hot-path C++ emitters into pure `model → str` functions.

- `emit/views.py`, `emit/deserialize.py`, `emit/store.py`, `emit/traits.py`.
- Each: signature `(Block, ...) -> str`, no I/O (§3.2).
- `tests/generator/test_emit_core.py`: feed a fixture `Block`, assert emitted C++ string matches a small golden snippet (catches emitter drift at unit granularity).
- `ffc.py` now delegates these sections to `emit/`.

**Milestone M3:** core emitters isolated; golden byte-identical.
**Gate:** golden + emit-core snapshot tests green.

---

### S4 — `emit/` remaining emitters
**Goal:** extract dictionary, codesystems, ingest mappings.

- `emit/dictionary.py` (was `ffd.py`), `emit/codesystems.py` (was `ffcs.py`), `emit/ingest_mappings.py`.
- Preserve the hash-based code assignment and recovery-tag parsing exactly (wire-critical — see `audit.plan.md` §2).
- `tests/generator/test_emit_dict.py`: assert `_code_hash` stability (same string → same uint32) and collision guard.

**Milestone M4:** all non-binding emitters isolated; golden byte-identical.
**Gate:** golden + dict-stability tests green.

---

### S5 — bindings + extensions
**Goal:** extract Python-stub emission and both extension emitters; fix SV-4.

- `bindings/python_fields.py`: `emit_python_fields/_ast/_stubs`.
- `emit/extensions_wasm.py`: WASM codec C emitter (`ffc.py` §2358).
- `emit/extensions_known.py`: `generate_known_extensions` **moved out of** `make_lib.py`/`pipeline.py` (fixes SV-4). `pipeline.py` now calls `emit.extensions_known.generate(...)`.
- `tests/generator/test_bindings.py`: assert a known resource's `.pyi` stub matches golden.

**Milestone M5:** orchestrator carries zero emitters; golden byte-identical.
**Gate:** golden + bindings tests green.

---

### S6 — cutover & cleanup
**Goal:** delete `tools/generator/`, repoint build, finalize.

- `library.py`: final `compile_fhir_library` driver importing only `model/`+`emit/`+`bindings/`.
- Update **CMake** `execute_process` and **Bazel** genrule: invoke `python -m generator` (path-independent).
- Update docs: `architecture.md` §9, `README.md` generator table, `.github/prompts/WASM_extensions.prompt.md`.
- **Delete** `tools/generator/` (all 5 files). Remove `tools/` if now empty (confirm exporter/compactor/ingestor still under `tools/`).
- Remove the temporary re-export shims.
- Final `ruff`/`black` pass; confirm no `from x import *`, no mutable defaults, full type hints on public functions.

**Milestone M6 (RELEASE):** old generator gone, build green via `python -m generator`, golden byte-identical, all unit tests green.
**Gate:** full suite + a clean-tree CMake configure that regenerates and compiles `fastfhir`.

---

## 5. Full todo list (copy into tracker; check off across agents)

### S0 — Baseline & harness
- [ ] S0.1 Run current pipeline; snapshot `generated_src/` → `tests/golden/baseline/`
- [ ] S0.2 `tests/generator/conftest.py`: session fixture regenerating into `tmp_path`
- [ ] S0.3 `tests/generator/test_golden_output.py`: byte-identical per-file assertion
- [ ] S0.4 Document run command; confirm deterministic output across 2 runs
- [ ] **M0 gate:** golden green on unmodified generator

### S1 — Package skeleton
- [ ] S1.1 Create `generator/` tree + empty `__init__.py` per subpackage
- [ ] S1.2 `emit/header.py`: canonical `write_if_changed` + `auto_header` (SV-1)
- [ ] S1.3 `pipeline.py`: port `make_lib.main()`, 4-space, drop import shim (SV-2/7)
- [ ] S1.4 `__main__.py` → `pipeline.run()`
- [ ] S1.5 `__init__.py` re-export 5 public symbols
- [ ] S1.6 `pyproject.toml`: black + ruff config
- [ ] S1.7 Reformat all generator Python to 4-space (SV-3)
- [ ] **M1 gate:** `python -m generator` byte-identical; `ruff` clean

### S2 — model/
- [ ] S2.1 `model/types.py`: `FieldKind`/`Field`/`Block` dataclasses (SV-5)
- [ ] S2.2 `model/type_map.py`: `TYPE_MAP`, sanitizers, `PRODUCTION_TYPES`
- [ ] S2.3 `model/structure.py`: extraction + resolvers + `discover_versions` (rename)
- [ ] S2.4 `model/merge.py`: `merge_fhir_versions`
- [ ] S2.5 `tests/generator/test_model.py`: offset/stride unit asserts
- [ ] S2.6 Repoint `ffc.py` imports to `model/`
- [ ] **M2 gate:** model tests + golden green; no emit→model cycle

### S3 — emit/ core
- [ ] S3.1 `emit/views.py` (lazy view, field_info, reflection dispatch)
- [ ] S3.2 `emit/deserialize.py` (eager deserializer)
- [ ] S3.3 `emit/store.py` (store/size fields)
- [ ] S3.4 `emit/traits.py` (resource traits)
- [ ] S3.5 `tests/generator/test_emit_core.py`: fixture-Block snapshot
- [ ] S3.6 `ffc.py` delegates to `emit/`
- [ ] **M3 gate:** golden + emit-core snapshots green

### S4 — emit/ remainder
- [ ] S4.1 `emit/dictionary.py` (was `ffd.py`); preserve `_code_hash`
- [ ] S4.2 `emit/codesystems.py` (was `ffcs.py`)
- [ ] S4.3 `emit/ingest_mappings.py`
- [ ] S4.4 `tests/generator/test_emit_dict.py`: hash stability + collision guard
- [ ] **M4 gate:** golden + dict tests green

### S5 — bindings + extensions
- [ ] S5.1 `bindings/python_fields.py` (`emit_python_fields/_ast/_stubs`)
- [ ] S5.2 `emit/extensions_wasm.py` (WASM codec)
- [ ] S5.3 `emit/extensions_known.py`; move out of `pipeline.py` (SV-4)
- [ ] S5.4 `tests/generator/test_bindings.py`: `.pyi` golden
- [ ] **M5 gate:** orchestrator emitter-free; golden green

### S6 — cutover
- [ ] S6.1 `library.py`: final driver, model/emit/bindings only
- [ ] S6.2 CMake `execute_process` → `python -m generator`
- [ ] S6.3 Bazel genrule → `python -m generator`
- [ ] S6.4 Docs: `architecture.md` §9, `README.md`, WASM prompt
- [ ] S6.5 Delete `tools/generator/`; remove shims
- [ ] S6.6 Final black/ruff; verify no `import *`, no mutable defaults
- [ ] S6.7 Clean-tree CMake configure → regenerate → compile `fastfhir`
- [ ] **M6 (RELEASE):** full suite green; build green via `python -m generator`

---

## 6. Test strategy (catch breaks early)

### 6.1 Three-tier test pyramid

| Tier | File | Catches | Runs |
|---|---|---|---|
| **Golden output** (top guard) | `test_golden_output.py` | *Any* byte drift in `generated_src/` | Every sprint gate |
| **Emitter snapshots** | `test_emit_*.py` | Drift localized to one emitter, before it reaches golden | S3–S5 |
| **Model units** | `test_model.py` | Wrong offset/stride/kind at the source — wire-format bugs | S2+ |

The golden test is the **backstop**; the unit tiers exist so a failure points at *one module* instead of a 70-file diff.

### 6.2 Golden test shape (authoritative example)

```python
# tests/generator/test_golden_output.py
import filecmp, pathlib

BASELINE = pathlib.Path(__file__).parent.parent / "golden" / "baseline"

def test_generated_output_is_byte_identical(regenerated_dir):
    """Regenerated_dir fixture runs `python -m generator` into tmp_path.

    Wire-format contract: the refactor must not change a single emitted byte.
    On mismatch we report the offending files explicitly so the failing sprint
    knows exactly which emitter drifted.
    """
    match, mismatch, errors = filecmp.cmpfiles(
        BASELINE, regenerated_dir,
        common=[p.name for p in BASELINE.iterdir()],
        shallow=False,                       # compare contents, not just mtime/size
    )
    assert not mismatch and not errors, (
        f"generated output drifted:\n  mismatch={mismatch}\n  errors={errors}"
    )
```

### 6.3 Model unit test (catches wire bugs at the source)

```python
# tests/generator/test_model.py
from generator.model.structure import extract_structure_definition
from generator.model.types import FieldKind

def test_bundle_field_offsets_are_stable(bundle_r5_json):
    """Offsets/strides are wire-format constants — a change here breaks every
    previously written .ffhr. Pin a few known fields as drift sentinels."""
    block = extract_structure_definition(bundle_r5_json, "Bundle")
    types = block.fields[0]
    assert types.kind is FieldKind.INLINE_ARRAY
    assert types.offset == 10            # immediately after 10-byte universal header
    assert types.stride > 0
```

### 6.4 PLACEHOLDER_TRIM







---

## 4. Post-Refactor Audit & Fixes (Completed)

After the initial S1-S6 split was completed, a systematic audit compared the new `generator/` pipeline output against the old `tools/generator/` pipeline output using identical FHIR specs. The following issues were identified and fixed:

### 4.1 Generator Crash Fixes

| # | Issue | File(s) | Fix |
|---|---|---|---|
| A-1 | `KeyError: 'Address'` in `emit_python_ast` — `token_registry` not populated for data-type blocks (only resources) | `generator/library.py` | Moved `token_registry`/`python_resource_map` init before types loop; added field-key population in types loop |
| A-2 | Code systems emitting 0 enums — R5 spec version suffix (`\|5.0.0`) on valueSet URLs not stripped before lookup | `generator/emit/codesystems.py` | Strip `\|version` suffix in `_scan_elements` before storing `vs_url` |
| A-3 | Parse function template bug — `static_cast<{enum_name}>(0)` emitted as literal due to missing f-string prefix | `generator/emit/codesystems.py` | Fixed: `'{enum_name}'` → `f'{enum_name}'` |

### 4.2 Structural Output Differences (Old → New Parity)

| # | Issue | Files | Fix |
|---|---|---|---|
| B-1 | Missing `_DATA_TYPES_TRAITS` for `vector<uint8_t>`, `vector<uint32_t>`, `vector<double>` | `generator/library.py` | Added 3 TypeTraits specializations to `_DATA_TYPES_TRAITS` constant |
| B-2 | Missing `#include "../include/FF_Utilities.hpp"` and `#include "FF_Dictionary.hpp"` in generated .cpp files | `generator/library.py` | Added includes to both `types_cpp` and `res_cpp` |
| B-3 | Data-type blocks not added to `reflected_block_names` | `generator/library.py` | Added `reflected_block_names.update()` in types loop |
| B-4 | Resource bundles loaded per-resource (28× redundant JSON parses) instead of once | `generator/library.py` | Pre-load resource bundles before resources loop |
| B-5 | `_block_key_namespace` stripped root resource, causing namespace collisions (e.g. `AllergyIntolerance.participant` and `Encounter.participant` both → `PARTICIPANT`) | `generator/model/structure.py` | Changed to use FULL dotted path (e.g. `ALLERGYINTOLERANCE_PARTICIPANT`) |
| B-6 | `STRING_TYPES` not checked in `_resolve_data_type_name`, `_resolve_ff_struct_name`, or view getter generation — types like `xhtml`, `dateTime`, `base64Binary` resolved to `XhtmlData` instead of `std::string_view` | `generator/model/structure.py`, `generator/emit/views.py` | Added `STRING_TYPES` import and check alongside `'string'`/`'code'` checks |
| B-7 | View forward declaration ordering — templates used before their forward declarations, causing Clang lookup errors | `generator/model/merge.py` | Accumulate all type blocks and call `generate_cxx_for_blocks` ONCE (matching old ffc.py) instead of per-type |
| B-8 | `_child_recovery_expr` didn't handle `SCALAR_PRIMITIVE_TYPES` — produced `RECOVER_FF_BOOLEAN` instead of `RECOVER_FF_BOOL` | `generator/model/structure.py` | Added SCALAR_PRIMITIVE_TYPES check before fallthrough |

### 4.3 API-Level Incompatibilities (New → Old API)

| # | Issue | Files | Fix |
|---|---|---|---|
| C-1 | FieldKeys emitted as simple string-only `extern const FF_FieldKey` instead of rich 6-arg `inline constexpr` with Registry[] | `generator/library.py` | Rewrote FieldKeys generation to match old ffc.py: 3 parts (global string constants, schema-specific 6-arg keys in namespaces, Registry[] array in .cpp) |
| C-2 | Dictionary missing `FF_{v}_GetCode()` (string→code lookup) and `.cpp` files | `generator/emit/dictionary.py` | Added `FF_{v}_GetCode` declaration to .hpp, generated .cpp with unordered_map-based GetCode |
| C-3 | `FF_Dictionary.hpp` used struct-based API (`FF_Dictionary::resolve()`) instead of function-based (`FF_ResolveCode()`, `FF_GetDictionaryCode()`) | `generator/emit/dictionary.py` | Rewrote `generate_master_dictionary` to emit old API with version-aware dispatch |
| C-4 | Reflection dispatch only had `make_node`/`make_root` — missing `reflected_fields`, `reflected_keys`, `reflected_resource_type`, `reflected_child_node`, `compact_field_sizes` | `generator/emit/views.py` | Rewrote `generate_reflection_dispatch` with full old API |
| C-5 | `FF_Dictionary.hpp` table-size constant `FF_{v}_CODE_COUNT` collided with enum value when "COUNT" appeared as a code string | `generator/emit/dictionary.py` | Renamed to `FF_{v}_TABLE_SIZE` to avoid hash collision |
| C-6 | `Decode::choice`/`scalar` references qualified with `FastFHIR::` — incorrect when code is inside `namespace FastFHIR` (nested namespace) | `generator/emit/views.py` | Changed to unqualified `Decode::choice`/`Decode::scalar` |

### 4.4 Remaining Issues

| # | Issue | File | Status |
|---|---|---|---|
| D-1 | Deserializer doesn't handle `code_enum` fields — assigns `std::string` to enum type | `generator/emit/deserialize.py` | OPEN — needs `code_enum` check and parse function call |
| D-2 | `Decode::array_header` referenced from generated ingest code, but `Decode::` resolves to `FastFHIR::FastFHIR::Decode::` (nested namespace) | various generated .cpp files | OPEN — needs namespace fix in ingest mappings generator |
| D-3 | `FF_KnownExtensions.hpp` written to `generated_src/` by default, not to `output_dir` | `generator/pipeline.py` | MINOR — should pass `output_dir` through |
| D-4 | No Python `__init__.py` in generated `python/fields/` | `generator/bindings/python_fields.py` | MINOR — should emit package init |

### 4.5 Key Architectural Lessons

1. **Namespace discipline**: The entire generated content lives in a single `namespace FastFHIR { }` block opened in `FF_DataTypes.hpp` and closed at the end of `FF_AllTypes.hpp`. All resource internal headers are included between these boundaries. This ensures code-system enums, TypeTraits, structs, and views all resolve each other without qualification.

2. **Open namespace = careful include ordering**: The open `namespace FastFHIR { }` in `FF_DataTypes.hpp` means any standard library includes AFTER it are polluted. Source files that include generated headers must order their includes so that standard headers come BEFORE generated ones, or avoid including headers that transitively open the namespace.

3. **Network vs wire stability**: The `_code_hash` function uses SHA-256 truncated to 31 bits. If the FHIR spec download changes (new ValueSet entries), the hash output changes for existing code strings, changing the wire format. The download must be pinned to a known spec version for wire-format stability.

4. **Two-phase lookup in templates**: Clang requires all template names to be forward-declared before their first use in template definitions, even in dependent contexts. View forward declarations must ALL precede any view definitions.
