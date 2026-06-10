# FastFHIR — Proposed Repo Map (Refactoring Target)

> **Purpose.** The current `.arbiter/repo_map.md` is a *flat per-file* index: 25
> "modules" that are each a single file. It carries no domain structure — it can't
> tell you that `FF_Builder`, `FF_Ingestor`, and `FF_Extensions` form one write
> path, or that `FF_Version`/`FF_Recovery`/`FF_Primitives` are a foundation every
> other file depends on.
>
> This document proposes a **domain-organized** target layout derived from the
> actual internal `#include` DAG and the existing build-target boundaries
> (`CMakeLists.txt` / `BUILD.bazel`). It is a map of *where things should live*,
> not a rewrite mandate. Physical moves are optional; the **layering and target
> grouping** are the deliverable.

---

## 1. Method — how this map was derived

Two ground-truth signals, not guesswork:

1. **Internal include DAG** (extracted from `include/`, `src/`, `python/`,
   `tools/`). This gives the *real* layering — what depends on what.
2. **Build-target boundaries** (`CMakeLists.txt`). The build system already
   separates `fastfhir_obj` (core) from `fastfhir_ingest_obj` (simdjson+OpenSSL),
   the optional WAMR `FF_Extensions` path, the 3 CLI tools, and the Python
   bindings. Those separations *are* the domain boundaries — they just aren't
   reflected in the flat map.

### 1.1 Verified internal dependency DAG (core headers)

```
FF_Version ─┐
            ├─► FF_Primitives ─► FF_Ops ─────────┐
FF_Recovery ┘                  └► FF_Utilities ──┤
                                                 │
FF_Memory ───────────────────────────────────────┤
generated: FF_Dictionary, FF_ResourceTypes ──────┤
                                                 ▼
                                            FF_Parser  (read path)
                                                 │
                                                 ▼
                                            FF_Builder (write path core)
                                            ╱        ╲
                              FF_Ingestor (+SIMD,    FF_Extensions (+WAMR)
                              Queue, Logger,
                              simdjson)

FF_Compactor ─► FF_Parser + FF_Memory + FF_Queue   (archive path)
FF_Queue, FF_Logger, FF_SIMD ─ standalone leaf utilities
```

Key facts that pin the layering:
- `FF_Parser` is the hinge: read path below it, write path above it
  (`FF_Builder` includes `FF_Parser`).
- `FF_Primitives` includes `FF_Version` + `FF_Recovery` — these three are the
  permanent wire-format foundation (constants that hit the wire; see `audit.md` §4).
- `FF_Ingestor` is the only core file pulling `FF_SIMD`, `FF_Queue`, `FF_Logger`
  *and* the external simdjson — confirming it belongs in its own target (it
  already is: `fastfhir_ingest_obj`).

---

## 2. Proposed Module Map (domain → files → role)

Seven domains. Each maps to an existing or recommended build target.

### D1 — `foundation` · wire-format primitives (no external deps)
> The permanent substrate. Everything includes this; it includes nothing above it.
> Constants here hit the wire and are versioned — treat as append-only.

| File | Role | Symbols |
|---|---|---|
| `include/FF_Version.hpp` | Engine + FHIR revision macros, `FF_ENCODE_HEADER_VERSION` | macros |
| `include/FF_Recovery.hpp` | `RECOVERY_TAG` enum, `recovery_type()` — permanent, hand-maintained | 2 |
| `include/FF_Primitives.hpp` | `FF_HEADER`, `FF_FieldKey`, `FF_Array`, `FF_String`, checksum types | 25 |
| `include/FF_Ops.hpp` | Endian load/store, IEEE-754 scalar fallbacks (header-only) | 2 |
| `include/FF_Utilities.hpp` | Inline array helpers — **`FF_ArrayHeader::Store` is dead, delete it** (`audit.md` §3.3) | 2 |
| `src/FF_Primitives.cpp` | Header-version encoding, checksum impls | — |

**Build target:** part of `fastfhir_obj`. No external deps. **Candidate for its
own static sub-target** (`fastfhir_foundation`) so tools/tests that only need
primitives don't pull the whole engine.

### D2 — `memory` · virtual memory arena (VMA)
> Handle/Body shared-ownership mapping. Lock-free `claim_space`. Standalone —
> depends only on `foundation`.

| File | Role | Symbols |
|---|---|---|
| `include/FF_Memory.hpp` | `Memory` (Handle) / `FF_Memory_t` (Body), `claim_space`, `View` | 29 |
| `src/FF_Memory.cpp` | mmap/SHM/file backends, atomic write-head | — |

**Build target:** part of `fastfhir_obj`.

### D3 — `read` · zero-copy read path
> The hinge layer. `FF_Parser` + generated type tables. Depends on
> `foundation` + `memory` + generated (`FF_Dictionary`, `FF_ResourceTypes`).

| File | Role | Symbols |
|---|---|---|
| `include/FF_Parser.hpp` | `Reflective::Node` lens, traversal, JSON export, type casts | 62 |
| `src/FF_Parser.cpp` | Node navigation, dictionary resolve, JSON emit | — |
| *generated* `FF_ResourceTypes.hpp`, `FF_Dictionary.hpp` | type tables consumed by parser | gen |

**Build target:** part of `fastfhir_obj`.

### D4 — `write` · builder + ingestion (write path)
> Above the parser. `FF_Builder` is the core mutator; `FF_Ingestor` adds the
> JSON front-end and is correctly isolated behind simdjson + OpenSSL.

| File | Role | Target |
|---|---|---|
| `include/FF_Builder.hpp` / `src/FF_Builder.cpp` | Node/Entry construction, `amend_*` back-patch (87 sym) | `fastfhir_obj` |
| `include/FF_Ingestor.hpp` / `src/FF_Ingestor.cpp` | simdjson → builder, fault detection, logging (9 sym) | **`fastfhir_ingest_obj`** (simdjson + OpenSSL) |
| `include/FF_Logger.hpp` | concurrent log buffer (header-only, used by Ingestor) | with ingestor |
| `include/FF_SIMD.hpp` | intrinsic wrappers + scalar fallbacks (header-only, used by Ingestor/Parser) | **D7 (shared)** |

> **Note.** `FF_Logger` and `FF_SIMD` are physically in `include/` flat, but
> logically belong to the ingest/concurrency domain. `FF_SIMD` is also pulled by
> `FF_Parser` — keep it as a shared leaf utility (D7), not buried in ingest.

### D5 — `archive` · compaction path
> Independent path: reads via `FF_Parser`, writes a compacted view. Uses
> `FF_Queue` for pending writes.

| File | Role | Symbols |
|---|---|---|
| `include/FF_Compactor.hpp` / `src/FF_Compactor.cpp` | `Compactor`, `archive()` presence-bitmask packing | 2 / 7 |

**Build target:** part of `fastfhir_obj`. Depends on `read` + `memory` + `FF_Queue`.

### D6 — `extensions` · optional WASM codec
> Optional, gated by `FASTFHIR_ENABLE_EXTENSIONS`. Pulls WAMR. Correctly
> isolated so the core never links a runtime it doesn't use.

| File | Role | Symbols |
|---|---|---|
| `include/FF_Extensions.hpp` / `src/FF_Extensions.cpp` | WASM module registry, cache, instantiate | 14 / 16 |

**Build target:** conditional `target_sources` into `fastfhir_obj` + `vmlib`.

### D7 — `concurrency_util` · standalone leaf utilities
> Dependency-free, reusable, no domain ownership. Per the style guide these are
> the "dependency-free, non-trivial → utility" tier.

| File | Role | Symbols |
|---|---|---|
| `include/FF_Queue.hpp` | templated `FIFO::Queue<T,CAP>`, injector/consumer split | 5 |
| `include/FF_SIMD.hpp` | shared intrinsic wrappers (Ingestor + Parser) | header-only |
| `include/FF_Logger.hpp` | concurrent logger (currently Ingestor-only) | 6 |

### D8 — `tooling` · generator + CLI + bindings (not in `fastfhir_obj`)

| Subdomain | Files | Target |
|---|---|---|
| **generator** (source of truth for `generated_src/`) | `tools/generator/{make_lib,ffc,ffd,ffcs,fetch_specs}.py` | Python, build-time |
| **CLI: ingest** | `tools/ingestor/FF_Ingest.cpp` | `ff_ingest` → `fastfhir_ingestor` + OpenSSL |
| **CLI: export** | `tools/exporter/FF_Export.cpp` | `ff_export` → `fastfhir_obj` |
| **CLI: compact** | `tools/compactor/FF_Compact.cpp` | `ff_compact` → `fastfhir_obj` (+OpenSSL) |
| **python bindings** | `python/FF_PythonBindings.cpp`, `python/fastfhir/__init__.py` | pybind target |

---

## 3. Aggregate header placement

`include/FastFHIR.hpp` aggregates only 4 of 13 headers and is included by exactly
one file (`python/FF_PythonBindings.cpp`) — see `audit.md` §3.4. Target role:

- It is the **public façade** for D1–D5 (read+write+archive). It should pull the
  top of each public domain: `FF_Parser` (read), `FF_Builder` (write),
  `FF_Compactor` (archive), `FF_Version`. It should **not** pull `FF_Ingestor`
  (heavy simdjson dep) or `FF_Extensions` (optional WAMR) — keep those opt-in.
- Current contents are already correct for that role. The fix is *documentation*:
  it's the public-API façade, not "the only header you need."

---

## 4. Delta vs. current `.arbiter/repo_map.md`

| Current (flat) | Proposed (domain) | Rationale |
|---|---|---|
| 25 single-file modules | 8 domains (D1–D8) | Group by dependency cluster + build target |
| `FF_Version`, `FF_SIMD` absent (no RAG JSON) | D1 / D7 explicitly | Header-only files still have a home |
| `FF_Logger`, `FF_SIMD` listed beside core | D7 leaf utilities | They're cross-cutting, not core engine |
| Ingestor beside Builder | D4 split: Builder→`fastfhir_obj`, Ingestor→`fastfhir_ingest_obj` | Mirrors the real target boundary |
| No layering shown | Explicit DAG (§1.1) | Foundation→Memory→Read→Write is the actual order |

---

## 5. Recommended physical moves (optional, low-risk)

These make the directory tree match the domain map. **None are required for
correctness** — the build globs `include/*.hpp` and `src/*.cpp` flat today.

1. **Keep `include/` flat** (current). Globbing + a flat public include dir is
   idiomatic for a single-library C++ project; sub-foldering headers would force
   `#include "read/FF_Parser.hpp"` churn across 70+ generated files. **Net: don't
   move headers.** Encode domains in the *map*, not the filesystem.
2. **Do extract sub-targets in CMake** where deps differ:
   - `fastfhir_ingest_obj` — already separate ✅
   - Extensions — already conditional ✅
   - Consider a `fastfhir_foundation` interface target (D1) so `ff_export` /
     tests that only touch primitives don't compile the whole engine.
3. **Delete dead code** as part of the remap: `FF_ArrayHeader` + `Store`
   (`FF_Utilities.hpp`) — zero call sites (`audit.md` §3.3).
4. **Normalize includes** (`audit.md` §3.2): drop `../include/` relative forms in
   the 3 `src/*.cpp` and fix the generator templates. Bare names already resolve.

---

## 6. One-line domain summary (drop-in for repo_map header)

```
D1 foundation  : Version, Recovery, Primitives, Ops, Utilities      [wire substrate]
D2 memory      : Memory (VMA, lock-free arena)                       [fastfhir_obj]
D3 read        : Parser (+ generated type tables)                    [fastfhir_obj]
D4 write       : Builder [obj] · Ingestor [ingest_obj +simdjson]     [split target]
D5 archive     : Compactor                                           [fastfhir_obj]
D6 extensions  : Extensions (+WAMR, optional)                        [conditional]
D7 concurrency : Queue, SIMD, Logger (leaf utilities)                [header-only]
D8 tooling     : generator · ff_ingest/export/compact · py bindings  [non-lib]
```
