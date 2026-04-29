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
- [ ] Rebuild/reinstall and rerun notebook streaming flow after resolving unrelated generated/build-tree blockers.
- [ ] Add concrete archive recovery routine implementation.
- [ ] Route all recovery-gate failures through one shared recovery API.
- [ ] Remove JSON round-trip guidance from Python README examples once rebuilt binaries confirm direct in-place flow.
- [ ] Rebuild/reinstall Python bindings from updated core and re-run README examples.

## Recovery Routine Backlog (Approximate Locations)

### 1) Core Recovery Entry Point
- [ ] Add `recover_archive(...)` orchestrator.
- Location: `src/FF_Builder.cpp` near `Builder::Builder(...)` and `Builder::finalize(...)` recovery gates.
- Goal: single internal path for recover-or-fail decisions when parser hydration fails.

### 2) Recovery Policy Surface
- [ ] Add recovery policy enum/options (strict fail, attempt repair, read-only salvage).
- Location: `include/FF_Builder.hpp` in `Builder` public API section (constructor/options area).
- Goal: explicit policy choice instead of hardcoded behavior.

### 3) Header Validation + Repair
- [ ] Implement header repair helper for stream metadata (`root`, `root_type`, checksum footer pointer).
- Location: `src/FF_Parser.cpp` near parser constructors and checksum extraction.
- Goal: recover minimally valid parse context when corruption is localized.

### 4) Root Reconciliation
- [ ] Add root reconciliation helper for mismatched/missing root metadata.
- Location: `include/FF_Builder.hpp` `root_handle()` fallback path and `src/FF_Builder.cpp` finalize preflight.
- Goal: avoid null-root dead ends when recoverable root candidates exist.

### 5) Version Integrity Enforcement
- [ ] Add explicit mixed-version guard with actionable error text.
- Location: `src/FF_Builder.cpp` constructor hydration and finalize recovery block.
- Goal: guarantee one FHIR version per bytestream unless full rewrite mode is enabled.

### 6) Pointer/VTable Safety Sweep
- [ ] Add optional repair checks for pointer amendments and variant/resource tags before mutation.
- Location: `src/FF_Builder.cpp` in `amend_pointer`, `amend_resource`, `amend_variant`.
- Goal: detect repairable structure faults before writes that could worsen corruption.

### 7) Python Error Mapping
- [ ] Map `**RECOVERY_REQUIRED**` native errors to structured Python exceptions.
- Location: `python/FF_PythonBindings.cpp` in `PyStream` root getter/finalize wrappers.
- Goal: make recovery-required states easy to detect and handle in Python callers.

### 8) Recovery Telemetry
- [ ] Add structured logging/metrics for recovery attempts and outcomes.
- Location: `src/FF_Builder.cpp` recovery gates and future `recover_archive(...)` implementation.
- Goal: traceability for production incidents and recovery quality.

## Code Quality Backlog

### Switch-Case Audit
- [ ] Deep review: replace all chained `if (node.is_kind())` / `if (tag == X) ... else if (tag == Y)` patterns with `switch` statements throughout the codebase.
- Locations to audit: `src/FF_Parser.cpp` (`print_json`, `standard_node_entries`, `node_lookup_field`, `standard_entry_as_node`), `src/FF_Compactor.cpp` (now fixed in `archive_node`; check remaining helpers), `include/FF_Parser.hpp` (`Node::as<T>()`), `include/FF_Utilities.hpp` (`FF_IsFieldEmpty`), `src/FF_Builder.cpp` (MutableEntry operator= overloads), generated `_from_json` / `STORE_` functions in `generated_src/`.
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
| 1 | Implement `http_get_manifest()` — real TLS HTTP GET to `registry.fastfhir.org/v1/modules/<url_hash_hex>/latest` | High |
| 2 | Implement `http_get_wasm()` — real TLS HTTP download of binary by content hash | High |
| 3 | TLS support for HTTP fetch (replace plain TCP stub in `FF_Extensions.cpp`) | High |
| 4 | `FF_IsKnownExtension()` / `FF_IsNativeExtension()` — implement string binary search in predigestion | High |
| 5 | `FF_ExtensionFilterMode` enum — apply in predigestion hot path | High |
| 6 | `Parser::unresolved_extensions()` list — collect offline-fallback skip log | Medium |
| 7 | Path A ingest dispatch — implement in concurrent workers (`FF_WasmExtension{Size,Store}`) | Medium |
| 8 | Path B round-trip export — emit stored raw JSON verbatim from `VALUE` ChoiceEntry | Medium |
| 9 | Ingest Synthea bundle end-to-end — verify URL directory, zero known-ext blocks, binary hash in registry | Medium |
| 10 | Phase 9 verification tests — MSB=0/1 correct, AOT enqueue, Path A/B round-trip | Medium |
| 11 | Write first real WASM codec module — geolocation extension test case (wasi-sdk) | Low |
| 12 | Binary file GC — evict old `.wasm` files from disk cache when superseded by new binary hash | Low |
| 13 | `generated_src/` regeneration — re-run `ffc.py` once FHIR spec downloads are available | Low |
