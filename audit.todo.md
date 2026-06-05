# Audit Action Items

> Derived from `audit.md`. Ordered by priority.

---

## 🔴 P0 — Binary Format Stability (Data Corruption Risk)

Generated constants whose values hit the wire must never drift between
generator runs. The fix: make them permanent (hand-maintained or hash-based).

- [x] **Move FF_Recovery.hpp from generated to hand-maintained**
  - `include/FF_Recovery.hpp` created with permanent hex values.
  - `ffc.py` : `generate_recovery_header()` removed; `_parse_recovery_tags()`
    reads the permanent header to validate emitted tag names.
  - `ffd.py` : `_parse_recovery_tags()` added for the same purpose.
  - `FF_Primitives.hpp` : include path changed from `../generated_src/` to `FF_Recovery.hpp`.
  - `generated_src/FF_Recovery.hpp` deleted.
  - `architecture.md` and `WASM_extensions.prompt.md` references updated.
  - Build files (CMake glob, Bazel glob) pick up the new header automatically.
- [x] **Make dictionary code assignments stable (ffd.py)**
  - Sequential `enumerate(sorted(mappings), 1)` replaced with SHA-256 truncated
    hash (`_code_hash()`). Same string → same uint32_t always.
  - Resolve function changed from DICT array to REV unordered_map (codes are
    no longer sequential).
  - Collision validation: raises `RuntimeError` if two codes collide.
  - Codes are positive 31-bit values; `FF_CODE_NULL` sentinel is avoided.

> Not changing: V-Table offsets are spec-defined and versioned by FHIR_REV in
> the stream header — already safe.

---

## 🔴 P0 — Test Coverage (Drift Prevention)

- [x] **Unit tests for core primitives (`tests/cpp/test_primitives.cpp`)** — 22 tests
  - FF_HEADER: validate_full, FHIR_REV, engine version, root, checksum
  - RECOVERY_TAG: IsArrayTag, GetTypeFromTag, ToArrayTag, known values
  - FF_FieldKey: construction (block, scalar, array-of-offsets), string ctor
  - FF_ARRAY: header read/write
  - FF_STRING: read/write, empty string
  - ResourceType: recovery → enum mapping
  - ByteOps: LOAD/STORE U16/U32/U64 round-trip
  - DATA_BLOCK: validate_offset expected-failure on FF_HEADER
- [x] **Unit tests for VMA Memory (`tests/cpp/test_memory.cpp`)** — 10 tests
  - default handle, anonymous create, file-backed create, claim_space,
    overflow rejection, concurrent claim (8 threads), StreamHead, View,
    truncate_file, SHM create
- [x] **Unit tests for SIMD intrinsics (`tests/cpp/test_simd.cpp`)** — 9 tests
  - ff_sum_sizes_masked (4 masks), ff_compact_dense_offset, ff_match_mask_u64x8 (4 patterns)
- [ ] **Add unit tests for Builder operations (`tests/cpp/test_builder.cpp`)**
  - claim_space atomicity and write-head progression
  - amend_pointer, amend_resource, amend_variant
  - Extension URL directory population
  - Checksum finalization (SHA-256 callback)
  - Builder hydration from archive (Parser → Builder round-trip)
  - Concurrent multi-threaded append (lock-free contract validation)
- [ ] **Add unit tests for Parser accessors (`tests/cpp/test_parser.cpp`)**
  - Node navigation by FF_FieldKey and by string key
  - Entry value extraction: scalar, string, offset, resource reference
  - Array traversal: count, index access, stride
  - Choice type resolution
  - Stream-level metadata: URL directory, module registry
  - JSON output: root().to_json() round-trip against expected string
- [ ] **Add golden integration test (`tests/cpp/test_roundtrip.cpp`)**
  - Ingestion pipeline: load FHIR JSON → Ingest::Ingestor → Builder → finalize
  - Read pipeline: Parser::create → root() → field-by-field walk
  - Export pipeline: root().to_json() → compare against original JSON
  - Compaction pipeline: Compactor::archive → Parser on compacted stream
  - Uses Synthea sample data (already downloaded by CMake) as canonical inputs
  - Each pipeline stage asserts exact byte-level output against a golden hash
> Not needed: No schema hash in FF_HEADER, no --locked flag, no golden file
> tests for drift. Existing FHIR_REV + engine VERSION is sufficient.

---

## 🟠 P1 — Build Blockers

- [ ] **Fix case-sensitivity bug in `BUILD.bazel:61`**
  - Change `"tools/ingestor/ff_ingest.cpp"` → `"tools/ingestor/FF_Ingest.cpp"`
- [ ] **Fix case-sensitivity bug in `CMakeLists.txt:369`**
  - Change `tools/ingestor/ff_ingest.cpp` → `tools/ingestor/FF_Ingest.cpp`

---

## 🟠 P1 — High

- [ ] **Normalize `#include` paths in `src/FF_Parser.cpp`**
  - `"../include/FF_Utilities.hpp"` → `"FF_Utilities.hpp"`
  - `"../include/FF_Parser.hpp"` → `"FF_Parser.hpp"`
  - `"../include/FF_SIMD.hpp"` → `"FF_SIMD.hpp"`
  - `"../generated_src/FF_Dictionary.hpp"` → `"FF_Dictionary.hpp"`
  - `"../generated_src/FF_Reflection.hpp"` → `"FF_Reflection.hpp"`
- [ ] **Normalize `#include` paths in `src/FF_Compactor.cpp`**
  - `"../include/FF_Compactor.hpp"` → `"FF_Compactor.hpp"`
  - `"../include/FF_Queue.hpp"` → `"FF_Queue.hpp"`
  - `"../include/FF_Utilities.hpp"` → `"FF_Utilities.hpp"`
  - `"../generated_src/FF_Reflection.hpp"` → `"FF_Reflection.hpp"`
- [ ] **Normalize `#include` paths in `src/FF_Extensions.cpp`**
  - `"../include/FF_Extensions.hpp"` → `"FF_Extensions.hpp"`
- [ ] **Normalize `#include` paths in `src/FF_Ingestor.cpp`**
  - `"../include/FF_Queue.hpp"` → `"FF_Queue.hpp"`
  - `"../include/FF_SIMD.hpp"` → `"FF_SIMD.hpp"`
  - `"../include/FF_Utilities.hpp"` → `"FF_Utilities.hpp"`
  - `"../generated_src/FF_Bundle.hpp"` → `"FF_Bundle.hpp"`
  - `"../generated_src/FF_IngestMappings.hpp"` → `"FF_IngestMappings.hpp"`
  - `"../generated_src/FF_KnownExtensions.hpp"` → `"FF_KnownExtensions.hpp"`
- [ ] **Fix code generator to emit bare include names**
  - `tools/generator/ffc.py`: replace `"../include/FF_Primitives.hpp"` → `"FF_Primitives.hpp"`
  - `tools/generator/ffc.py`: replace `"../include/FF_Utilities.hpp"` → `"FF_Utilities.hpp"`
  - `tools/generator/ffc.py`: replace `"../include/FF_Parser.hpp"` → `"FF_Parser.hpp"`
  - `tools/generator/ffc.py`: replace `"../include/FF_Builder.hpp"` → `"FF_Builder.hpp"`
  - `tools/generator/ffd.py`: replace `"../include/FF_Primitives.hpp"` → `"FF_Primitives.hpp"`
  - Re-run generator to refresh `generated_src/`
- [ ] **Rebuild and re-run test suite after include normalization**

---

## 🟡 P2 — Medium

- [ ] **Remove or document `FF_ArrayHeader::Store` in `include/FF_Utilities.hpp`**
  - Zero call sites across entire codebase. Either delete the struct or add a comment explaining it's reserved for external consumers.
- [ ] **Decide fate of `include/FastFHIR.hpp`**
  - Options: (a) expand to cover all public headers, (b) deprecate and remove, or (c) leave as-is with a comment noting it's Python-bindings-only.
- [ ] **Address 14 unfinished TODOs in `project_progress.prompt.md`**
  - Prioritize the recovery routine backlog if that's the current milestone.
  - Mark any stale items as won't-fix or move to a separate backlog.

---

## 🔵 P3 — Low

- [ ] **Regenerate `.arbiter/` index for `FF_SIMD.hpp` and `FF_Version.hpp`**
  - These are in `repo_map.lock` but have no corresponding RAG JSONs.
- [ ] **Generate `.py.md` spec files for core modules**
  - Start with `FF_Memory.hpp`, `FF_Parser.hpp`, `FF_Builder.hpp` (the three architecture-critical headers).
- [ ] **Verify `.gitignore` covers `build/` artifacts**
  - `build/` contains `.dll`, `.exe`, `.lib`, `.pdb`, `.exp` + Synthea sample data zip.
- [ ] **Index `tests/cpp/test_readme.cpp` in `.arbiter/`**
  - Low priority but useful for understanding API usage patterns.

---

## Notes

- `generated_src/` (~70 files) is **intentionally not indexed** in `.arbiter/` — the generator (`ffc.py`) is the source of truth. Regenerating from the fixed generator will propagate the include normalization.
- No architecture violations found. All 4 design invariants from `architecture.md` are intact.
