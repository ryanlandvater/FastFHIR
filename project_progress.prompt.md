> **SUPERSEDED (2026-07-06)** — Pending items from this document were re-verified and
> consolidated into [`TASKS.md`](TASKS.md); stale items were dropped there with rationale.
> This file is retained as a historical record only. Do not work from its checklists.

## Recent Generator Refactor - Build Integration

The `tools/generator/` monolith was decomposed into the `generator/` modular
package. The first Windows MSVC build revealed 10 bug categories in the
refactored code generator. All are now fixed.

### Completed
- [x] Unicode encoding crash fix (dictionary.py Unicode arrow -> ASCII)
- [x] Missing namespace closes in library.py (FF_DataTypes.hpp, FF_AllTypes.hpp)
- [x] FF_STRING::deserialize -> FF_STRING(...).read_view(...) in Offset branches
- [x] STORE_FF_STRING 4-arg -> 3-arg for nullable strings
- [x] Self-referential ExtensionData struct (std::vector<Offset> instead of std::vector<ExtensionData>)
- [x] Namespace resolution (using namespace FastFHIR at global scope)
- [x] OpenSSL via vcpkg for Windows (replaces Perl-based ExternalProject)
- [x] is_self_ref handling in store/size/deserialize code paths
- [x] STRING_TYPES coverage in array handlers (id, uri, markdown, etc.)
- [x] SimpleQuantity removed from PRODUCTION_TYPES (no FHIR definition)

### Current Status
- **fastfhir_obj**: Compiles with zero errors on MSVC 19.51, C++20
- **OpenSSL**: Installed via vcpkg (required for fastfhir_ingest_obj and tools)
- **CI**: Full multi-platform build not yet verified

### Lessons Learned
See `refactor_history.md` for detailed lessons on:
- MSVC incomplete-type handling for self-referential vectors
- Namespace boundary tracking in code generation
- Windows portability (Perl, Unicode, file encoding)
- Cross-cutting propagation of type changes through all emission paths

---

# FastFHIR Project Progress

## How To Use
- Update checkbox status as work moves.
- Keep entries short and action-focused.
- Use function/class anchors for locations, not line numbers.

## Active Todo List
- [x] Builder hydrates from parser metadata on existing archives.
- [x] Builder version degrades to archive header version for in-place enrichment.
- [x] Recovery gates added with explicit `**RECOVERY_GATE**` and `**RECOVERY_REQUIRED**` markers.
- [x] Restored shared stream cursor semantics using mapped-header atomic state (`STREAM_SIZE` field at bytes 8-15).
- [x] Implemented staged-header import in `Memory::StreamHead` so raw archive ingest can start at offset 0 without clobbering the live lock/cursor field.
- [x] Added API hardening checks: header-layout `static_assert`s and `reset()` lock-guard while a `StreamHead` is active.
- [x] Rebuild/reinstall and rerun notebook streaming flow after resolving unrelated generated/build-tree blockers.
- [ ] Add concrete archive recovery routine implementation.
- [ ] Route all recovery-gate failures through one shared recovery API.
- [x] Remove JSON round-trip guidance from Python README examples once rebuilt binaries confirm direct in-place flow.
- [x] Rebuild/reinstall Python bindings from updated core and re-run README examples.

## Recovery Routine Backlog (Approximate Locations)

### 1 Core Recovery Entry Point
- [ ] Add `recover_archive(...)` orchestrator.
  - `recover_archive` must either complete atomically (all repairs written or none)
    or operate on a copy; specify which strategy is required before implementation begins.
- Location: `src/FF_Builder.cpp` near `Builder::Builder(...)` and `Builder::finalize(...)` recovery gates.
- Goal: single internal path for recover-or-fail decisions when parser hydration fails.

### 2 Recovery Policy Surface
- [ ] Add recovery policy enum/options:
      - `strict fail` — abort on any corruption; no mutations attempted.
      - `attempt repair` — mutate archive in place to fix detected faults.
      - `read-only salvage` — return best-effort parsed data without any writes,
        even if data is incomplete.
- Location: `include/FF_Builder.hpp` in `Builder` public API section (constructor/options area).
- Goal: explicit policy choice instead of hardcoded behavior.

### 3 Header Validation + Repair
- [ ] Implement header repair helper for stream metadata (`root`, `root_type`, checksum footer pointer).
- Location: `src/FF_Parser.cpp` near parser constructors and checksum extraction.
- Goal: recover minimally valid parse context when corruption is localized.

### 4 Root Reconciliation
- [ ] Add root reconciliation helper for mismatched/missing root metadata.
- Location: `include/FF_Builder.hpp` `root_handle()` fallback path and `src/FF_Builder.cpp` finalize preflight.
- Goal: avoid null-root dead ends when recoverable root candidates exist.

### 5 Version Integrity Enforcement
- [ ] Add explicit mixed-version guard with actionable error text.
- Location: `src/FF_Builder.cpp` constructor hydration and finalize recovery block.
- Goal: guarantee one FHIR version per bytestream unless full rewrite mode is enabled.

### 6 Pointer/VTable Safety Sweep
- [ ] Add optional repair checks for pointer amendments and variant/resource tags before mutation.
- Location: `src/FF_Builder.cpp` in `amend_pointer`, `amend_resource`, `amend_variant`.
- Goal: detect repairable structure faults before writes that could worsen corruption.

### 7 Python Error Mapping
- [ ] Map `**RECOVERY_REQUIRED**` native errors to structured Python exceptions.
- Location: `python/FF_PythonBindings.cpp` in `PyStream` root getter/finalize wrappers.
- Goal: make recovery-required states easy to detect and handle in Python callers.

### 8 Recovery Telemetry
- [ ] Add structured logging/metrics for recovery attempts and outcomes.
- Location: `src/FF_Builder.cpp` recovery gates and future `recover_archive(...)` implementation.
- Goal: traceability for production incidents and recovery quality.

## Code Quality Backlog

### Switch-Case Audit
- [ ] Audit `src/FF_Parser.cpp`: `print_json`, `standard_node_entries`, `node_lookup_field`, `standard_entry_as_node`.
- [ ] Audit `src/FF_Compactor.cpp`: check remaining helpers beyond `archive_node`.
- [ ] Audit `include/FF_Parser.hpp`: `Node::as<T>()`.
- [ ] Audit `include/FF_Utilities.hpp`: `FF_IsFieldEmpty`.
- [ ] Audit `src/FF_Builder.cpp`: MutableEntry operator= overloads.
- [ ] Audit generated `_from_json` / `STORE_` functions in `generated_src/` — deferred until after `ffc.py` regeneration (see Low-priority TODO #13).
- Goal: exhaustiveness checking at compile time, elimination of silent fall-through bugs (like the `is_choice` misclassification and the missing `FF_FIELD_CODE` branch in `archive_node`).

## Verification Todo
- [ ] Add focused tests for corrupted header, missing root, mismatched recovery tag, and checksum footer corruption.
- [ ] Add tests for strict fail mode vs attempted repair mode.
- [ ] Add tests confirming in-place enrich works after recovery without JSON re-ingest.
- [ ] Add Python integration test asserting `RECOVERY_REQUIRED` maps to expected Python exception.

## Notes
- Current policy intent: Builder degrades to archive header version for existing streams.
- Full rewrite mode (future): explicit path that permits controlled re-emit to a target version.
- Import-mode API behavior: when stream size is `0` at acquisition, `StreamHead` stages the first header block (`38` bytes), then continues direct writes to arena memory.
- Release behavior: staged bytes are restored for non-cursor header regions; cursor/lock bytes are finalized atomically by lock release.
- Current risk status: edited Memory files are clean; full CMake build currently fails due to unrelated pre-existing/generated issues outside `FF_Memory` changes.

---

## WASM Extension Subsystem — Status & Backlog

### Completed Phases ✅

| Phase | Description |
|---|---|
| Phase 1 | Binary structures: `FF_EXTENSION` vtable, `FF_URL_DIRECTORY`, `FF_MODULE_REGISTRY` |
| Phase 2 | `ffc.py` generator: `EXT_REF` discriminated-union field, `EXTENSIONView` accessors |
| Phase 3 | Predigestion pass + ingest integration (`FF_PredigestExtensionURLs`) |
| Phase 4 | Parser read path: `has_url_directory()`, `has_module_registry()`, `url_directory()` |
| Phase 5 | `FF_KnownExtensions` generator — spec-driven, dynamic profile-native URL detection |
| Phase 6 | WAMR host integration (`FF_WasmExtensionHost`, staging ping-pong wrappers) |
| Phase 7 | Registry fetch + version-aware binary-hash caching (content-addressed disk cache + TTL) |
| Phase 8 | `ffc.py --wasm` mode (codec triple generator) |
| Phase 9 | EXT_REF MSB routing, async AOT worker, Path B passive storage |
| Phase 10 | Binary hash in `FF_MODULE_REGISTRY` (56-byte entries, version identity) |

### Hashing Architecture (Key Design Decision)

Two separate SHA-256 roles — never conflate them:

- **URL Hash (Role 1):** `sha256(url_string)` → used only as the sidecar metadata filename on disk (`meta/<url_hash_hex>.meta`). Never written to any FF binary stream. Never used as an in-memory lookup key.
- **Binary Hash (Role 2):** `sha256(wasm_bytes)` → module version identity. Written to every `FF_MODULE_REGISTRY` entry at `REG_ENTRY_MODULE_HASH` (offset 24, 32 bytes). Also names the content-addressed binary file on disk (`<binary_hash_hex>.wasm`). Changes only when the binary changes.

### Open TODOs

| # | Item | Priority |
|---|---|---|
| 0 | Define the `FF_ExtensionRegistry` interface: configurable base URL (no hardcoded hostname), URL path template (e.g. `{base}/v1/modules/{url_hash_hex}/latest`), and error contract for network failure / non-200 (throw, return null, or fall back to cached binary). Required before items 1 and 2. | **Required** |
| 1 | Implement `http_get_manifest()` — TLS HTTP GET via `FF_ExtensionRegistry` interface, resolving `{url_hash_hex}` from the URL hash. | High |
| 2 | Implement `http_get_wasm()` — TLS HTTP download of binary by content hash via `FF_ExtensionRegistry` interface. | High |
| 3 | TLS support for HTTP fetch (replace plain TCP stub in `FF_Extensions.cpp`) | High |
| 4 | `FF_IsKnownExtension()` / `FF_IsNativeExtension()` — implement string binary search in predigestion | High |
| 5 | `FF_ExtensionFilterMode` enum — apply in predigestion hot path | High |
| 6 | `Parser::unresolved_extensions()` list — collect offline-fallback skip log | Medium |
| 7 | Path A ingest dispatch — implement in concurrent workers (`FF_WasmExtension{Size,Store}`) | Medium |
| 8 | Path B round-trip export — emit stored raw JSON verbatim from `VALUE` ChoiceEntry | Medium |
| 9 | Ingest Synthea bundle end-to-end — verify URL directory, zero known-ext blocks, binary hash in registry | Medium |
| 10 | Phase 9 verification tests — MSB=0/1 correct, AOT enqueue, Path A/B round-trip | Medium |
| 11 | Write first real WASM codec module — geolocation extension test case (wasi-sdk) | Low |
| 12 | Binary file GC — evict old `.wasm` files from disk cache when superseded by new binary hash. GC must not evict a binary hash that is referenced by any loaded `FF_MODULE_REGISTRY` entry in a live `Parser` or `Builder` instance; define the reference-counting or generation-lock mechanism before implementing eviction. | Low |
| 13 | `generated_src/` regeneration — re-run `ffc.py` once FHIR spec downloads are available | Low |

## Completed This Session (2026.0.1)

### Bug Fixes
- [x] **Choice field read-back** — Added `FF_FIELD_CHOICE` materialization in
  `python/FF_PythonBindings.cpp` so `Patient.DECEASED = False` reads back as
  `False` instead of `None`.
- [x] **macOS `getpid()`** — Added `#include <unistd.h>` in `tests/cpp/test_memory.cpp`.
- [x] **Bazel duplicate symbol** — Removed redundant `#include <FF_Recovery.hpp>`
  from `tests/cpp/test_primitives.cpp`.

### Test Infrastructure
- [x] **CTest ordering** — Fixed `py_test_1 DEPENDS py_getting_started` in
  `CMakeLists.txt` to prevent fixture clobbering under parallel ctest.

### Build Cleanup (zero warnings)
- [x] `src/FF_Memory.cpp` — `total_size` scoped to `#ifdef _WIN32`.
- [x] `include/FF_Memory.hpp` — `m_file_handle`/`m_os_handle` guarded with `#ifdef _WIN32`.
- [x] `src/FF_Compactor.cpp` — Removed unused `is_inline_scalar_kind()`.
- [x] `src/FF_Parser.cpp` — Removed unused `compact_slot_size()`, fixed init-list order.
- [x] `tests/cpp/test_simd.cpp` — Removed unused `expected` variable.
- [x] `tools/ingestor/ff_ingest.cpp` — Removed unused `is_piped` variable.
- [x] `python/FF_PythonBindings.cpp` — Removed unused `render_entry_json()`,
  unused `kind`, deprecated `get_type()`.
