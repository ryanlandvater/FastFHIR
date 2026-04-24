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
