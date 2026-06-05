# P0 Plan: Binary Format Stability

> Principle: Constants that hit the wire must be permanent — not ordering-derived.
> The generators consume a checked-in header for RECOVERY_TAG names; dictionary
> codes use hash-based assignment. V-Table offsets are already versioned by
> FHIR_REV and support bidirectional compatibility by design — parsers can
> read both past and future sources.

---

## 1. Move `FF_Recovery.hpp` to `include/`, generators consume it   ✅ DONE

**What was done:**

1.1. **`include/FF_Recovery.hpp` created** — hand-maintained, copied content
    from `generated_src/FF_Recovery.hpp`. Header comment explains the
    permanence convention and block ranges.

1.2. **`generate_recovery_header()` removed from `ffc.py`** (was lines 1134–1245).
    Replaced by `_parse_recovery_tags()` which reads the permanent header and
    returns a `{name: value}` dict for tag-name validation.

1.3. **Parser added to both `ffc.py` and `ffd.py`** — same regex-based function.
    Called during generation to validate that emitted tag names are defined.

1.4. **Tag-by-name references in generated code** — unchanged. Functions like
    `_scalar_recovery_tag()` still return tag name strings. C++ compiler
    resolves names to values from the checked-in header.

1.5. **Maintenance convention** added via comments in `FF_Recovery.hpp`.

1.6. **`FF_Primitives.hpp` include path** changed from `../generated_src/`
    to `"FF_Recovery.hpp"` (same directory).

1.7. **`generated_src/FF_Recovery.hpp` deleted** — no longer generated.

1.8. **Documentation updated** — `architecture.md` and `.github/prompts/`
    references changed from `generated_src/` to `include/`.

| File | Change |
|---|---|
| `include/FF_Recovery.hpp` | Created — hand-maintained, permanent |
| `generated_src/FF_Recovery.hpp` | Deleted |
| `tools/generator/ffc.py` | `generate_recovery_header()` → `_parse_recovery_tags()` |
| `tools/generator/ffd.py` | Added `_parse_recovery_tags()` (same function) |
| `include/FF_Primitives.hpp` | Include changed to `"FF_Recovery.hpp"` |
| `CMakeLists.txt` / `BUILD.bazel` | No change — glob picks up new file automatically |

---

## 2. Make dictionary code assignments stable   ✅ DONE

**What was done:**

2.1. **`_code_hash()` function added to `ffd.py`** — SHA-256 truncated to first
    4 bytes, sign bit cleared, sentinel collision avoided. Same string → same
    uint32_t always.

2.2. **Sequential assignment replaced** — `enumerate(sorted(mappings), 1)`
    replaced with hash-based assignment in both hpp and cpp generation.

2.3. **`Resolve` function reworked** — since codes are no longer sequential
    integers, the DICT array lookup was replaced with a
    `std::unordered_map<uint32_t, const char*>` reverse map.

2.4. **Collision validation added** — raises `RuntimeError` with both
    colliding strings and the hash value for immediate debugging.

2.5. **`_parse_recovery_tags()` added to `ffd.py`** — reads the permanent
    `include/FF_Recovery.hpp`. Same function as in `ffc.py`.

| File | Change |
|---|---|
| `tools/generator/ffd.py` | Added `_code_hash()`, `_parse_recovery_tags()`, hash-based assignments, collision check, map-based Resolve |
| `generated_src/FF_*_Dictionary.hpp` | Regenerated — enum values now hash-based. Must rebuild. |

---

## 3. V-Table offsets — already safe

The user confirmed: V-Table offsets are generated and can only be appended with
version revision. A newer FHIR revision (R5) may add fields at the end of a
block's V-Table, but never reorder or remove existing fields. This is the FHIR
compatibility contract.

- `FF_HEADER.FHIR_REV` tells the parser which revision's layout to use.
- Generated `*_internal.hpp` structs define per-revision `HEADER_SIZE` and
  field offset enums. The generated deserializers clamp to the stream
- Bidirectional by design: R5 parser reads R4 streams (clamps to R4 offsets);
  R4 parser reads R5 streams (ignores appended fields).
- No changes needed here.

---

## 4. Test Coverage — Drift Prevention

### State of existing tests

| Test suite | File | What it covers | Gap |
|---|---|---|---|
| README C++ | `tests/cpp/test_readme.cpp` | 12 examples: ingest, parse, edit, compact, concurrent gen, Synthea | Pass/fail only — no byte-level golden assertions |
| README Python | `tests/python/test_readme.py` | Python binding round-trip | Same — success-condition checks only |
| Queue | `tests/python/test_long_array_queue.py` | FIFO::Queue Python binding | Single component, Python side only |

No test asserts exact byte-level output. No granular unit tests for individual
components (Builder, Parser, Memory, SIMD, FF_HEADER, FieldKey). Drift in
generated constants or V-Table offsets would only be caught by downstream
breakage.

### 4.1 Unit tests: `tests/cpp/test_primitives.cpp` ✅ (22 tests, all passing)

Targets the hand-written core (not generated code). Tests that the foundational
types and operations work correctly in isolation.

| Component | What to test | Status |
|---|---|---|
| `FF_HEADER` | `validate_full` on valid/corrupt buffers, version encode/decode round-trip, FHIR_REV get/set | ✅ |
| `RECOVERY_TAG` | `IsArrayTag`, `GetTypeFromTag`, `ToArrayTag` — edge cases (0, 0x7FFF, 0x8000, 0xFFFF) | ✅ |
| `FF_FieldKey` | Construction with owner/kind/offset/child, null-offset sentinel, `from_cstr` | ✅ |
| `FF_ARRAY` | Header read/write via `STORE_FF_ARRAY_HEADER` | ✅ |
| `FF_STRING` | `read_view` and `STORE_FF_STRING` | ✅ |
| `ResourceType` | `resource_type_from_recovery` and `ResourceTypeTraits` | ✅ |
| Byte ops | `LOAD_U16/U32/U64` and `STORE_U16/U32/U64` | ✅ |
| `DATA_BLOCK` | `validate_offset` expected-failure on FF_HEADER | ✅ |

### 4.2 Unit tests: `tests/cpp/test_memory.cpp` ✅ (10 tests, all passing)

Targets the VMA handle/body, stream heads, and views.

| Test | What it validates | Status |
|---|---|---|
| `Memory::create` (anonymous) | Factory creates valid handle | ✅ |
| `Memory::createFromFile` | File-backed arena | ✅ |
| `claim_space` | Single-threaded monotonic progression | ✅ |
| `claim_space` overflow | Throws on exhaustion | ✅ |
| `claim_space` concurrent | 8 threads, no overlapping offsets | ✅ |
| `StreamHead` | RAII lock acquisition/release | ✅ |
| `View` | Construction and size | ✅ |
| `truncate_file` | Reduces backing file size | ✅ |
| `shm_create` | POSIX SHM segment | ✅ |

### 4.3 Unit tests: `tests/cpp/test_simd.cpp` ✅ (9 tests, all passing)

Targets all three code paths for each SIMD helper.

| Test | What it validates | Status |
|---|---|---|
| `ff_sum_sizes_masked` | All-zeros, single-bit, multi-bit, all-bits | ✅ |
| `ff_compact_dense_offset` | First field offset | ✅ |
| `ff_match_mask_u64x8` | Exact match, no match, match at index 0 and 7 | ✅ |

End-to-end FHIR JSON → FFHR → JSON round-trip with golden hash assertions.

| Pipeline | Steps | Golden assertion |
|---|---|---|
| **Ingest** | Load Synthea `Patient.json` → `Ingest::Ingestor` → `Builder::finalize` → raw FFHR bytes | SHA-256 of FFHR stream matches checked-in golden hash |
| **Parse** | `Parser::create` on FFHR bytes → `root()` → walk fields by `FF_FieldKey` → extract values | Every extracted value matches original JSON field |
| **JSON export** | `root().to_json()` → compare against original JSON | JSON strings match (normalized for ordering) |
| **Compaction** | `Compactor::archive` on FFHR → `Parser::create` on compacted stream → `root().to_json()` | JSON matches original (same normalization) |
| **Multi-resource** | Same as above with a Synthea Bundle (hundreds of resources) | Same golden hash contract per-resource |

**Synthea data** is already downloaded by CMake (`FASTFHIR_DOWNLOAD_SYNTHEA=ON`,
extracted to `${CMAKE_CURRENT_BINARY_DIR}/synthea_fhir_r4/`). Tests should use
this canonical data and fail loudly if the golden hash changes.

### Implementation plan

| Step | File | Effort |
|---|---|---|
| 1 | Create `tests/cpp/test_primitives.cpp` targeting FF_HEADER, RECOVERY_TAG, FF_FieldKey, Memory, SIMD, Dictionary, Builder (single-thread), Parser (trivial) | ~200 lines |
| 2 | Create `tests/cpp/test_builder.cpp` coverage for amendments, URL directory, checksum, hydration, concurrent append | ~250 lines |
| 3 | Create `tests/cpp/test_roundtrip.cpp` with Synthea data, golden hash assertions, JSON round-trip, compaction | ~300 lines |
| 4 | Update `CMakeLists.txt` to add new test executables / link targets | ~10 lines |
| 5 | Generate golden hashes from first known-good run, check-in as `tests/golden/hashes.txt` | ~5 lines |
| 6 | Run full test suite, verify all tests pass | — |

All three C++ test files follow the same pattern as `test_readme.cpp`:
`--filter <name>` dispatch, `REQUIRE` assertions, `main()` returning 0/1.
