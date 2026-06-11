# FastFHIR Generator Refactor — Build Integration History

> **Document:** Post-refactor integration record
> **Date:** 2026
> **Scope:** `tools/generator/` monolith → `generator/` modular package,
>            initial Windows (MSVC 19.51, VS 2026) build with all fixes.

---

## 1. Summary

The monolithic `tools/generator/` package (5 files, ~3383 lines, `ffc.py` alone
2585 lines) was decomposed into a root-level `generator/` package with separate
`model/`, `emit/`, and `bindings/` subpackages. The decomposition followed the
plan in `generator_refactor_plan.md`.

After the structural refactor, the first Windows CMake build revealed **10
categories of bugs** introduced by the decomposition. All have been fixed.
The `fastfhir_obj` C++ library target now compiles with **zero errors** on
MSVC 19.51 (Visual Studio 2026, C++20).

---

## 2. Bugs Found and Fixed

### 2.1 Unicode Encoding Crash (Build Blocker)

**File:** `generator/emit/dictionary.py` line 193

**Error:**
```
UnicodeEncodeError: 'charmap' codec can't encode character '\u2192'
```

**Root cause:** The `→` arrow character (U+2192) cannot be encoded in the
Windows cp1252 codepage used by the Python runtime.

**Fix:** Replaced `→` with ASCII `->` in the print statement.

**Lesson:** All print/output in the generator must use ASCII-only strings.
Windows terminals use legacy codepages that cannot render characters outside
the current locale. A CI gate should run `python -c "open(sys.stdout.fileno())`
or equivalent to catch non-ASCII before merging.

---

### 2.2 Missing `} // namespace FastFHIR` (DataTypes.hpp)

**File:** `generator/library.py` (FF_DataTypes.hpp emission)

**Error:** `FF_DataTypes.hpp(15,20): error C1075: '{': no matching token found`

**Root cause:** The old `ffc.py` closed `namespace FastFHIR` before adding
forward declarations and data struct definitions at global scope. The new
`library.py` closed the namespace *after* adding `pub_hpp` instead of
*before*. This caused 1 extra `{` than `}`.

**Fix:** Added `} // namespace FastFHIR\n\n` after the TypeTraits
specializations but *before* the forward declarations and struct definitions.

**Lesson:** When extracting code from a monolithic file, every brace,
namespace, and scope boundary must be explicitly tracked. The old file
processed things in order A→B→C; the new file's order must match exactly
unless the dependency analysis proves otherwise.

---

### 2.3 `FF_STRING::deserialize` Not a Member

**Files:** `generator/emit/deserialize.py`, `generator/emit/store.py`

**Error:**
```
error C2039: 'deserialize': is not a member of 'FF_STRING'
error C2660: 'STORE_FF_STRING': function does not take 4 arguments
```

**Root cause:** The `Offset` branch in `generate_eager_deserializer` and
`generate_store_fields` handled nullable `unique_ptr<T>` fields by emitting
`T::deserialize(...)` / `STORE_T(...)`. For `T = FF_STRING`, neither function
exists — `FF_STRING` uses `FF_STRING(...).read_view(...)` for reading and
`STORE_FF_STRING(base, off, str)` (3 args) for writing.

**Fix:** Added special case: when `child_struct == "FF_STRING"`, emit the
correct constructor/read_view pattern for deserialization and the 3-arg
`STORE_FF_STRING` for storage.

**Lesson:** `FF_STRING` is a fundamental type with its own read/write API
that differs from all other DATA_BLOCK subclasses. Any code path that
resolves a FHIR type to `FF_STRING` must be checked — it cannot use the
generic `::deserialize` / `STORE_` pattern.

---

### 2.4 Self-Referential `ExtensionData` Struct (MSVC Incomplete Type)

**File:** `generator/model/merge.py` (struct type resolution)

**Error:**
```
error C2065: 'ExtensionData': undeclared identifier
error C2923: 'std::vector': 'ExtensionData' is not a valid template
type argument for parameter '_Ty'
```

**Root cause:** `struct ExtensionData` contained
`std::vector<ExtensionData>` — a self-referential vector. MSVC rejects
`std::vector` with an incomplete template argument type (which `ExtensionData`
is during its own definition). This failed in C++20 mode on MSVC 19.51.

**Fix:** When a struct's array field resolves to the same data type name
(self-referential), emit `std::vector<Offset>` instead of
`std::vector<ExtensionData>`. In the FFHR binary format, child extensions
are stored as arena offsets, so `Offset` is semantically correct.

**Lesson:** MSVC's `std::vector` does not support incomplete types for the
template argument, even in C++20 mode. GCC and Clang allow this. Any
self-referential data structure (Extension.extension,
QuestionnaireResponse.item, etc.) must use `Offset` as the vector element
type to be portable. This was a latent portability bug that would have
manifested on any MSVC build even before the refactor.

---

### 2.5 Enum Classes in `namespace FastFHIR` Not Visible at Global Scope

**Files:** `generator/library.py` (FF_DataTypes.hpp emission)

**Error:**
```
error C3646: 'comparator': unknown override specifier
error C2065: 'FF_QuantityComparator': undeclared identifier
```

**Root cause:** The `FF_CodeSystems.hpp` header defines enum classes like
`FF_QuantityComparator` inside `namespace FastFHIR { ... }`. The data struct
definitions were emitted at global scope (outside the namespace). When a
struct field used one of these enums (`FF_QuantityComparator comparator`),
the compiler could not find the type name at global scope.

**Fix:** Added `using namespace FastFHIR;` at global scope after the namespace
close and before the struct definitions. This brings all enum classes and
types into the global namespace for the struct definitions.

**Lesson:** The code generator produces struct definitions at global scope
(because they are used by other global-scope code). But the enum types from
`FF_CodeSystems.hpp` are in `namespace FastFHIR`. A `using namespace` bridge
is required at the point where structs are emitted. The old `ffc.py` achieved
this implicitly through a different processing order that the refactor did
not preserve.

---

### 2.7 STRING_TYPES Array Handling

**Files:** `generator/emit/store.py`, `generator/emit/deserialize.py`

**Error:**
```
error C2065: '__total': undeclared identifier
```

**Root cause:** The array handling code checked `f['fhir_type'] in ('string', 'code')`
for string-type arrays. But FHIR types like `id`, `uri`, `markdown`, `dateTime`,
etc. are also string-like (in `STRING_TYPES`) but are NOT `'string'` or `'code'`.
These fell through to the complex-struct branch which emitted wrong code
(using `__total` in a store function, or `FF_STRING::deserialize` in a
deserialize function).

**Fix:** Extended all string/code array checks to include STRING_TYPES:
```python
if f['fhir_type'] in ('string', 'code') or f['fhir_type'] in STRING_TYPES:
```

Also fixed the `generate_size_fields` string check similarly.

**Lesson:** The `STRING_TYPES` set in `type_map.py` exists precisely to
capture all FHIR types that are stored as `FF_STRING` on the wire. But the
code generators had ad-hoc `'string'` checks that missed these. `STRING_TYPES`
should be the authoritative set used everywhere.

---

### 2.8 `namespace FastFHIR` Missing in FF_AllTypes.hpp

**File:** `generator/library.py` (FF_AllTypes.hpp emission)

**Error:**
```
error C2059: syntax error: '}'
error C2143: syntax error: missing ';' before '}'
```

**Root cause:** The `FF_AllTypes.hpp` aggregator file ended with
`} // namespace FastFHIR` but never opened the namespace. The old `ffc.py`
opened `namespace FastFHIR {` at the top of this file.

**Fix:** Added `'namespace FastFHIR {\n'` after the includes preamble.

**Lesson:** When extracting aggregator file generation, every namespace open
and close must be preserved exactly.

---

### 2.9 `FF_SIMPLEQUANTITY` Undeclared in Reflection

**File:** `generator/model/type_map.py` (PRODUCTION_TYPES)

**Error:**
```
error C2653: 'FF_SIMPLEQUANTITY': is not a class or namespace name
```

**Root cause:** `SimpleQuantity` was in `PRODUCTION_TYPES` but the FHIR spec
bundle has no StructureDefinition for it (it is a profile of `Quantity`).
The vtable struct `FF_SIMPLEQUANTITY` was never generated, but the reflection
dispatch table referenced it.

**Fix:** Removed `"SimpleQuantity"` from `PRODUCTION_TYPES`.

**Lesson:** Every type in `PRODUCTION_TYPES` must have a corresponding
StructureDefinition in the FHIR spec bundles. Profile types that are not
independently defined cannot be generated.

---

### 2.10 OpenSSL ExternalProject Fails on Windows (No Perl)

**File:** `cmake/openssl.cmake` (build system)

**Error:**
```
'Configure' is not recognized as an internal or external command
```

**Root cause:** The `ExternalProject_Add(openssl_external ...)` used
`<SOURCE_DIR>/Configure` which is a Perl script. Windows has no Perl
interpreter by default. The cmake module `cmake/openssl.cmake` was adapted
from a macOS/Linux project without consideration for Windows.

**Fix:** Installed OpenSSL 3.6.2 via vcpkg (`vcpkg install openssl --triplet x64-windows`)
and reconfigured CMake with `-DCMAKE_TOOLCHAIN_FILE=.../vcpkg.cmake`.
This is a one-time setup cost (~7 minutes for the initial compile).

**Lesson:** The `cmake/openssl.cmake` ExternalProject approach is
Unix-only. For Windows, a vcpkg-based fallback is required. The
`cmake/openssl.cmake` should be updated to check for Perl first, then
fall back to vcpkg or a prebuilt binary download.

---

## 3. Files Modified

### Generator Python Files

| File | Change |
|---|---|
| `generator/emit/dictionary.py` | Unicode arrow `→` → `->` |
| `generator/library.py` | Namespace close before forward decls, `using namespace FastFHIR`, remove extra namespace close |
| `generator/emit/deserialize.py` | `FF_STRING` special case in Offset branch, STRING_TYPES in array check, `is_self_ref` handling |
| `generator/emit/store.py` | `FF_STRING` special case in Offset branch, STRING_TYPES in store/size checks, `is_self_ref` handling |
| `generator/model/merge.py` | Self-referential `Offset` type in struct gen, `is_self_ref` flag |
| `generator/model/type_map.py` | Remove `SimpleQuantity` from PRODUCTION_TYPES |

### Build System Files

| File | Change |
|---|---|
| `cmake/openssl.cmake` | Needs Windows fallback (see §2.10) |
| `build/` (CMake config) | Reconfigured with vcpkg toolchain for OpenSSL |

---

## 4. Key Lessons for Future Work

### 4.1 Always Build on All Target Platforms

The refactor produced code that passed lint and style checks but failed
miserably on MSVC. Key portability issues found only on Windows:
- `std::vector<IncompleteType>` is rejected by MSVC (GCC/Clang accept it)
- Unicode characters in print statements crash on cp1252 terminals
- Perl-based ExternalProject configure fails without Perl

**Recommendation:** CI must build on all three platforms (macOS, Linux,
Windows) before merging any generator changes.

### 4.2 Test Generated C++ Compilation, Not Just Python Syntax

The generator may produce valid Python output that generates invalid C++.
Every emission path (store, size, deserialize, view) can independently break.
**Recommendation:** Add a compilation smoke test that runs the generator
pipeline and then compiles a subset of the generated C++ files.

### 4.3 Self-Referential Types Are a Cross-Cutting Concern

When `Extension.extension` changed to use `Offset`, it required changes in
5 separate files (merge.py for the type, store.py for SIZE and STORE,
deserialize.py for reading, library.py for namespace). A single field change
propagates through the entire emission pipeline.

**Recommendation:** Add a `is_self_ref` field attribute at the earliest
possible point (layout construction) and check it in all emitters. Do not
re-compute it in each emitter.

### 4.4 STRING_TYPES Must Be the Single Source of Truth

The set of string-like FHIR types (`STRING_TYPES`) is defined in
`type_map.py` but the emission code had ad-hoc checks for `'string'` and
`'code'` only. Any emission path that handles string-type fields should use
`f['fhir_type'] in STRING_TYPES` rather than `== 'string'`.

### 4.5 The `namespace FastFHIR` Boundary Is Fragile

C++ types (enum classes, structs) are in `namespace FastFHIR` in some headers
but at global scope in others. The generator must carefully track:
1. Which namespace each type is in
2. Where `using namespace FastFHIR` is required
3. Where `} // namespace FastFHIR` closes each block

**Recommendation:** Add explicit markers/comments in the generator code at
every point where the namespace opens or closes.

---

## 5. Current Build Status

| Target | Status |
|---|---|
| `fastfhir_obj` (core library) | ✅ Compiles with zero errors on MSVC 19.51, C++20 |
| `fastfhir_ingest_obj` | Requires OpenSSL (see §2.10 — vcpkg installed) |
| CLI tools (`ff_ingest`, `ff_export`, `ff_compact`) | Not yet verified |
| Python bindings | Not yet verified |
| Tests (`ff_test_readme`, `ff_roundtrip`) | Not yet verified |

### Build Commands

```powershell
# From repo root, with vcpkg toolchain:
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE="build/_deps/vcpkg/scripts/buildsystems/vcpkg.cmake"
cmake --build build --config Release --target fastfhir_obj -- /m:4
```

### Generator Pipeline Commands

```powershell
python -c "from generator.pipeline import run; run(output_dir='generated_src', keep_specs=True)"
```

---

## 6. Known Open Issues

1. **`cmake/openssl.cmake`** needs a Windows-compatible fallback
   (vcpkg or prebuilt binary download) instead of the Perl-based
   ExternalProject approach.
2. **`FF_Dictionary.hpp`** in `generated_src/` is stale from the old
   `ffd.py` generator and should not be regenerated — the header is
   hand-maintained in `include/FF_Dictionary.hpp`.
3. **`SimpleQuantity`** is removed from PRODUCTION_TYPES but might be
   needed for R5 support — investigate if a FHIR definition exists.

## 7. Code Cleanup � Post-Refactor Audit Fixes

### 7.1 Removed Stale Duplicate of \merge_fhir_versions\ in \store.py
**File:** \generator/emit/store.py\ (lines 247�317 removed)

**Issue:** A ~70-line copy of \merge_fhir_versions\ was left at the bottom of \store.py\ during the refactor. This duplicate was dead code (no caller imported from \store.py\), missing \is_self_ref\ detection, and referenced \BLOCK_FIELD_OVERRIDES\ which was not imported � would crash if called.

**Fix:** Removed the duplicate function entirely. The only canonical source of \merge_fhir_versions\ is \generator/model/merge.py\.

### 7.2 Removed Dead Unreachable STRING_TYPES Branches in \store.py
**File:** \generator/emit/store.py
**Issue:** Two unreachable \elif f['fhir_type'] in STRING_TYPES:\ branches existed:

1. In \generate_size_fields\ (line 35): already caught by the preceding \if f['fhir_type'] == 'string' or f['fhir_type'] in STRING_TYPES:\ check.
2. In \generate_store_fields\ (line 157): already caught by the preceding \if f['fhir_type'] in ('string', 'code') or f['fhir_type'] in STRING_TYPES:\ check. This branch also emitted \__total += ...\ (SIZE variable) inside the STORE function � a latent bug if somehow reached.

**Fix:** Removed both dead branches.
