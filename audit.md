# FastFHIR Repository Audit

> Generated from `.arbiter/` index traversal + full source inspection.
> Date: 2026-04-06

---

## 1. `.arbiter/` Coverage

### 1.1 Covered (has RAG JSONs)

| Layer | Files | Notes |
|---|---|---|
| `include/` | 11 of 13 headers | `FF_SIMD.hpp`, `FF_Version.hpp` missing |
| `src/` | 7 of 7 source files | Complete |
| `python/` | `__init__.py`, `FF_PythonBindings.cpp` | `fields/` type stubs are generated; see §1.3 |
| `tools/` | 8 files (all C++ + Python) | Complete |
| Documents | 4 (`README.md`, `architecture.md`, `python/README.md`, `.pytest_cache/README.md`) | Complete |

### 1.2 Missing from `.arbiter/` (no RAG JSONs)

| Path | Count | Severity | Rationale |
|---|---|---|---|
| `include/FF_SIMD.hpp` | 1 | Low | Header-only intrinsics; referenced by `FF_Parser.cpp` and `FF_Ingestor.cpp`. In lock file but no RAG JSON emitted. |
| `include/FF_Version.hpp` | 1 | Low | Preprocessor macros only (`#define FASTFHIR_VERSION_*`). In lock file but no RAG JSON emitted. |
| `python/fastfhir/fields/` | ~150 `.py`/`.pyi` | Low | Generated type stubs — derived from `ffc.py`; not a source of truth. |
| `tests/` | 3 files | Low | Test code; low value for symbol-level indexing. |
| `.github/prompts/WASM_extensions.prompt.md` | 1 | Low | Design document; in lock file but no RAG JSON. |

> **Note on `generated_src/`:** All 70+ files under `generated_src/` are deterministically emitted by `tools/generator/ffc.py`. They are not indexed in `.arbiter/`, which is correct — the generator is the source of truth. The generator itself IS indexed (`ffc.py`, `ffd.py`, `ffcs.py`, `make_lib.py`, `fetch_specs.py`).

### 1.3 No `.py.md` spec files

The `.arbiter/` retrieval model references `<module>/<file>.py.md` spec files as the bridge between the repo map and source. **Zero spec files have been generated.** This means the deterministic lookup path (`repo_map → spec → source`) is missing its middle step. All lookups fall through directly to the RAG JSONs or source, which is workable but loses the API-contract layer.

---

## 2. Header / Implementation Inventory

| Header | Implementation | Nature |
|---|---|---|
| `FF_Builder.hpp` | `src/FF_Builder.cpp` | ✓ |
| `FF_Compactor.hpp` | `src/FF_Compactor.cpp` | ✓ |
| `FF_Extensions.hpp` | `src/FF_Extensions.cpp` | ✓ (conditional on `FASTFHIR_ENABLE_EXTENSIONS`) |
| `FF_Ingestor.hpp` | `src/FF_Ingestor.cpp` | ✓ |
| `FF_Logger.hpp` | — | Header-only (all methods inline) |
| `FF_Memory.hpp` | `src/FF_Memory.cpp` | ✓ |
| `FF_Ops.hpp` | — | Header-only (endian load/store, IEEE-754 fallbacks) |
| `FF_Parser.hpp` | `src/FF_Parser.cpp` | ✓ |
| `FF_Primitives.hpp` | `src/FF_Primitives.cpp` | ✓ |
| `FF_Queue.hpp` | — | Header-only (fully templated `FIFO::Queue<T, CAP>`) |
| `FF_SIMD.hpp` | — | Header-only (inline intrinsic wrappers, scalar fallbacks) |
| `FF_Utilities.hpp` | — | Header-only (inline helpers + dead `FF_ArrayHeader`) |
| `FF_Version.hpp` | — | Header-only (preprocessor macros) |
| `FastFHIR.hpp` | — | Aggregate header (includes 4 of 13 headers) |

**Verdict: No missing implementations.** Every header without a `.cpp` is genuinely inline, templated, or macro-only.

---

## 3. Issues

### 3.1 🔴 CRITICAL — Case-sensitivity bug blocks Linux builds

**Files:** `BUILD.bazel:61`, `CMakeLists.txt:369`

Both build systems reference `tools/ingestor/ff_ingest.cpp` (lowercase `ff`), but the file on disk is `tools/ingestor/FF_Ingest.cpp` (uppercase `FF`). This works on Windows/macOS (case-insensitive filesystems) but will **fail to compile on Linux**.

```
# BUILD.bazel:61
srcs = ["tools/ingestor/ff_ingest.cpp"],  // BUG

# CMakeLists.txt:369
add_executable(ff_ingest tools/ingestor/ff_ingest.cpp)  // BUG
```

### 3.2 🟠 HIGH — Fragile relative `#include` paths

Two conventions coexist in the same directory:

**Bare names (correct, used by CMake include path):**
```cpp
// src/FF_Builder.cpp
#include "FF_Utilities.hpp"
#include "FF_Builder.hpp"
// src/FF_Memory.cpp
#include "FF_Primitives.hpp"
```

**Relative `../include/` paths (fragile, unnecessary):**
```cpp
// src/FF_Parser.cpp
#include "../include/FF_Utilities.hpp"
#include "../include/FF_SIMD.hpp"
// src/FF_Compactor.cpp
#include "../include/FF_Compactor.hpp"
// src/FF_Extensions.cpp
#include "../include/FF_Extensions.hpp"
```

**Mixed within one file:**
```cpp
// src/FF_Ingestor.cpp
#include "FF_Ingestor.hpp"               // correct
#include "../include/FF_Queue.hpp"       // wrong
```

**Root cause:** `tools/generator/ffc.py` and `ffd.py` emit `#include "../include/FF_Primitives.hpp"` into all 70+ generated files. Both CMake and Bazel already add `include/` and `generated_src/` to the include path, so the bare form works everywhere.

**Affected files:** 3 hand-written `src/*.cpp` + all 70+ `generated_src/*.{cpp,hpp}`.

### 3.3 🟡 MEDIUM — Dead code: `FF_ArrayHeader::Store`

`include/FF_Utilities.hpp` defines:

```cpp
struct FF_ArrayHeader {
    static constexpr uint64_t SIZE = 16;
    static void Store(uint8_t *const __base, uint64_t &write_head,
                      uint16_t recovery, uint16_t step, uint32_t count);
};
```

A full-codebase search finds **zero call sites** for `FF_ArrayHeader::Store` or construction of `FF_ArrayHeader`. The struct and its method are dead code.

### 3.4 🟡 MEDIUM — `FastFHIR.hpp` only used by Python bindings

`FastFHIR.hpp` is documented as "the only header consumers need to include," but it is `#include`d by exactly **one file**: `python/FF_PythonBindings.cpp`. It aggregates only 4 of 13 headers (`FF_Version.hpp`, `FF_Parser.hpp`, `FF_Builder.hpp`, `FF_Compactor.hpp`), omitting `FF_Memory.hpp`, `FF_Ingestor.hpp`, `FF_Extensions.hpp`, `FF_Logger.hpp`, `FF_Primitives.hpp` (transitively included), `FF_Ops.hpp`, `FF_Utilities.hpp`, `FF_Queue.hpp`, `FF_SIMD.hpp`.

### 3.5 🟡 MEDIUM — 14 unfinished TODO items in `project_progress.prompt.md`

The recovery routine backlog lists 8 sub-tasks (all unchecked), plus 5 additional active items. Key gaps:
- No `recover_archive(...)` orchestrator
- No recovery policy enum or options
- No version integrity enforcement at finalize
- No Python error mapping for recovery states
- Pending notebook streaming flow rebuild

### 3.6 🔵 INFO — `FF_SIMD.hpp` and `FF_Version.hpp` in lock file but no RAG JSON

These two files appear in `.arbiter/repo_map.lock` (the indexer scanned them), but no corresponding `.arbiter/include/<file>.json` was emitted. Likely an indexer edge case — both files are header-only with no class/function definitions for the indexer to capture.

### 3.7 🔵 INFO — `FF_Compact.cpp` referenced correctly in BUILD.bazel

Unlike the ingestor case, `BUILD.bazel:71` references `tools/compactor/FF_Compact.cpp` with the correct case. The mismatch is isolated to the ingestor.

---

## 4. Binary Format Stability (Value Drift Audit)

> This is a binary serialization engine. If numeric constants shift between
> generator runs, previously written `.ffhr` files become unreadable.
> Each generated file that assigns numeric values is traced below.

### 4.1 What hits the wire

The binary format (`DATA_BLOCK`, per `architecture.md` §4) stores these
generator-assigned values:

| Value | Location | Size | Generated by |
|---|---|---|---|
| **Recovery tag** | Bytes 8–9 of every block header | `uint16_t` | `ffc.py` → `FF_Recovery.hpp` |
| **V-Table byte offset** | Implicit in block layout | `constexpr` offset | `ffc.py` → `FF_*_internal.hpp` |
| **V-Table total size** | `HEADER_SIZE` constant | `constexpr` size | `ffc.py` → `FF_*_internal.hpp` |
| **Dictionary code** | FF_CODE block body | `uint32_t` | `ffd.py` → `FF_{v}_Dictionary.hpp` |
| **Field kind** | FF_FieldKey member | `enum` value | `ffc.py` → `FF_FieldKeys.hpp` |
| **Array stride** | `HEADER_SIZE` of child block | `constexpr` | `ffc.py` → `FF_*_internal.hpp` |

### 4.2 What does NOT hit the wire

These generated values are compile-time only and can drift without corrupting
existing `.ffhr` data:

| Value | Why safe |
|---|---|
| `RESOURCETYPE` enum ordinals | Used only as template params (`ResourceTypeTraits<RESOURCETYPE::BUNDLE>`) and switch cases — the enumerator **name** is what matters, never its ordinal |
| `FF_CodeSystems.hpp` enums (`AddressType`, `AdministrativeGender`, etc.) | Values go through string conversion → dictionary lookup before serialization. The `uint8_t` ordinal never appears in the wire format. |
| Global `FF_FieldKey` constants (`FieldKeys::FF_ACCOUNT`) | Carry only a string name, no numeric value. Lookup is string-based. |

### 4.3 Drift vectors — per generated file

#### `FF_Recovery.hpp` — RECOVERY_TAG enum
```
RECOVER_FF_BUNDLE = 0x0302
RECOVER_FF_CAREPLAN = 0x0303
```
**Assignment:** `0x0300 + index` where index = position in `resources` list sorted
by spec-defined order (`res_order` dict, line 1174 of `ffc.py`). Data types use
`target_types` order; backbone elements use alphabetical `.sort()`.

**Drift risk:** Within a single FHIR spec version → **stable**. Across spec
versions → resources/data types added mid-alphabet shift everything below. The
backbone `.sort()` is particularly fragile — a new backbone element shifts
all subsequent backbone tags.

**Mitigation:** FF_HEADER encodes FHIR revision (`FHIR_VERSION_R4`/`R5`), so
readers can detect version mismatch. But there is no hash/signature of the
recovery tag mapping itself.

#### `FF_*_internal.hpp` — V-Table offsets and HEADER_SIZE
```cpp
struct FF_PATIENT {
    static constexpr Offset ID = 10;
    static constexpr Offset META = 18;
    static constexpr Offset IMPLICITRULES = 26;
    // ...
    static constexpr Size HEADER_SIZE = 98;
};
```
**Assignment:** Cumulative: `off = 10`, then `off = prev.offset + prev.size`
for each field in `snapshot.element` order (line 956 of `ffc.py`).

**Drift risk:** If the FHIR StructureDefinition adds, removes, or reorders a
field, or changes a field's type (and thus `size`), all subsequent offsets
and HEADER_SIZE shift. Per-spec-version stable; across versions potentially
breaking.

#### `FF_{v}_Dictionary.hpp` — Dictionary codes
```cpp
enum FF_R4_Code : uint32_t {
    FF_R4_NULL = 0xFFFFFFFF,
    FF_R4_CODE_ACUTE = 1,
    FF_R4_CODE_CHRONIC = 2,
    // ...
};
```
**Assignment:** `enumerate(sorted_mappings, 1)` where mappings are sorted by
raw code string (line 112 of `ffd.py`).

**Drift risk:** A new code added to a FHIR ValueSet that sorts before
existing codes **shifts all subsequent dictionary codes**. This is a real
drift vector even within the same FHIR release if the ValueSet is revised.

#### `FF_CodeSystems.hpp` — Type-safe code enums
```cpp
enum class AddressType : uint8_t { Both, Physical, Postal, Unknown };
```
**Wire-safe.** These ordinals never hit the wire — values are converted to
strings via `FF_AddressTypeToString()`, then dictionary-looked-up to produce
a stable `uint32_t` code.

#### `FF_ResourceTypes.hpp` — Resource type enum
```cpp
enum class RESOURCETYPE : uint16_t { UNKNOWN = 0, ALLERGYINTOLERANCE, BUNDLE, ... };
```
**Wire-safe.** Ordinals are used only for compile-time template dispatch
(`ResourceTypeTraits<RESOURCETYPE::BUNDLE>`). The enumerator name maps to
a RECOVERY_TAG at compile time; the ordinal itself is never serialized.

### 4.4 Summary

| File | Wire impact | Stable per spec version | Risk if spec changes |
|---|---|---|---|
| `FF_Recovery.hpp` | Every block header | ✅ | 🔴 Tags shift |
| `FF_*_internal.hpp` | V-Table layout | ✅ | 🔴 Offsets shift |
| `FF_{v}_Dictionary.hpp` | Code blocks | ❌ (sorted) | 🔴 Codes shift |
| `FF_CodeSystems.hpp` | None | N/A | 🟢 No wire impact |
| `FF_ResourceTypes.hpp` | None | N/A | 🟢 No wire impact |
| `FF_FieldKeys.hpp` | Field lookup | ✅ | 🟡 Namespace-scoped keys use RECOVERY_TAG references |

### 4.5 Completed Mitigations

| Recommendation | Status | What was done |
|---|---|---|
| Move `FF_Recovery.hpp` out of generated code | **✅ DONE** | Moved to `include/FF_Recovery.hpp` — hand-maintained, explicit hex values; generators consume it via `_parse_recovery_tags()` |
| Hash-based dictionary codes | **✅ DONE** | `ffd.py` uses SHA-256 truncated hash (31-bit) instead of sequential `enumerate(sorted())`; collision validation added |
| Pin FHIR spec version | **❌ NOT STARTED** | Still uses unpinned HL7 URLs in `fetch_specs.py` |
| Schema hash in FF_HEADER | **❌ WON'T DO** | Over-engineering given permanent constants are immutable; `FHIR_REV` + engine `VERSION` in header is sufficient |

---

## 5. Architecture Compliance

| Invariant | Status |
|---|---|
| §1.1 Zero-copy O(1) field navigation via V-Table | ✓ |
| §1.2 Lock-free concurrency via atomic `fetch_add` | ✓ |
| §1.3 Memory-mapped sparse VMA, 4 GiB default | ✓ |
| §1.4 Version-aware headers (FHIR rev + engine version) | ✓ |
| §2 Handle/Body `Memory`/`FF_Memory_t` | ✓ |
| §7 Concurrent Builder amendments | ✓ |
| §8 Zero-copy read path (`Reflective::Node`) | ✓ |

No architectural regressions detected.

---

## 6. Compiler Coverage

| Compiler | CMake | Bazel | Notes |
|---|---|---|---|
| MSVC 19.51 (VS 2026) | ✅ 25/25 tests pass | ✅ core + 3 unit tests pass | `_COPTS` passes `-std=c++20` + `/std:c++20` on Windows |
| Clang (VS-bundled) | Not tested | Not tested | `%VCINSTALLDIR%\Tools\Llvm\bin\clang-cl.exe` available |
| GCC / Linux Clang | Not tested | Not tested | `-std=c++20` in `_COPTS` covers both |

All compiler-specific code paths are Clang-safe:
- `FF_Primitives.hpp:42`: `defined(__GNUC__) || defined(__clang__)` for visibility attribute
- `FF_SIMD.hpp:60`: `_MSC_VER && !defined(__clang__)` — Clang uses native `__builtin_ctz`
- Architecture macros (`__AVX2__`, `__BMI2__`, `__ARM_NEON`) are identical in Clang and GCC

---

## 7. Summary

### Completed This Session

| Area | Files | Status |
|---|---|---|
| Permanent `FF_Recovery.hpp` | `include/FF_Recovery.hpp` (new), `ffc.py`, `ffd.py`, `FF_Primitives.hpp`, `architecture.md`, `WASM_extensions.prompt.md` | ✅ |
| Stable dictionary codes | `ffd.py` | ✅ |
| Unit tests: core primitives | `tests/cpp/test_primitives.cpp` — 22 tests | ✅ |
| Unit tests: VMA Memory | `tests/cpp/test_memory.cpp` — 10 tests | ✅ |
| Unit tests: SIMD intrinsics | `tests/cpp/test_simd.cpp` — 9 tests | ✅ |
| CMake test infrastructure | `CMakeLists.txt` — 3 new targets + CTest entries | ✅ |
| Bazel test infrastructure | `BUILD.bazel` — 3 new `cc_test` targets + `_COPTS` constant | ✅ |

### Test Results

| Suite | CMake | Bazel |
|---|---|---|
| C++ README tests (13) | ✅ All pass | ❌ Boringssl C++20 compat issue |
| Python binding tests (9) | ✅ All pass | N/A |
| Unit: primitives (22) | ✅ All pass | ✅ All pass |
| Unit: memory (10) | ✅ All pass | ✅ All pass |
| Unit: SIMD (9) | ✅ All pass | ✅ All pass |
| **Total** | **25/25 (100%)** | **3/3 unit tests** |

### Remaining Issues

| Issue | Severity | Status |
|---|---|---|
| Case-sensitivity in build files (`ff_ingest.cpp` vs `FF_Ingest.cpp`) | 🔴 Blocks Linux | Not started |
| Fragile `../include/` paths in generated code (~75 files) | 🟠 Maintainability | Root cause in `ffc.py`/`ffd.py` |
| `FF_ArrayHeader::Store` dead code | 🟡 Dead code | Not started |
| `FastFHIR.hpp` unused outside Python bindings | 🟡 API surface | Not started |
| project_progress.prompt.md TODOs (14 items) | 🟡 Project tracking | Not started |
| Clang build testing (CMake + Bazel) | 🔵 Verification | Not started |
| Bazel `test_readme` (boringssl C++20 compat) | 🟡 CI gap | Pre-existing toolchain issue |
