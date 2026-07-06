> **SUPERSEDED (2026-07-06)** — Pending items from this document were re-verified and
> consolidated into [`TASKS.md`](TASKS.md); stale items were dropped there with rationale.
> This file is retained as a historical record only. Do not work from its checklists.

# FastFHIR — Dictionary Unification Plan

> **Status:** Architecture Proposal  
> **Date:** 2025-07-18  
> **Scope:** Merge R4/R5 dictionaries + UCUM concepts into a single unified
> dictionary with one master string table, permanent 31-bit IDs, and
> type-safe per-system enums.

---

## Table of Contents

1. [Current State Analysis](#1-current-state-analysis)
2. [Target Architecture](#2-target-architecture)
3. [Master JSON Data Format](#3-master-json-data-format)
4. [Generator Refactor](#4-generator-refactor)
5. [Runtime Lookup Refactor](#5-runtime-lookup-refactor)
6. [Enum Generation](#6-enum-generation)
7. [Migration Sequence](#7-migration-sequence)
8. [static_assert Guards](#8-static_assert-guards)

---

## 1. Current State Analysis

### 1.1 Three Separate Data Tables

| File | Format | Count | Key Type |
|---|---|---|---|
| `dictionaries/FF_R4_Dictionary.cpp` | `FF_CodeEntry{uint32_t, const char*}[]` | 20,770 | 31-bit permanent ID |
| `dictionaries/FF_R5_Dictionary.cpp` | `FF_CodeEntry{uint32_t, const char*}[]` | 6,225 | 31-bit permanent ID |
| `dictionaries/FF_UCUM_Concepts.cpp` | `FF_ConceptEntry{FF_UCUM_CODES, const char*}[]` | 1,613 | 56-bit enum value |

### 1.2 Problems

1. **String duplication between R4 and R5.** Codes present in both versions
   (e.g., `"required"`, `"candidate"`) have their labels stored twice.

2. **UCUM codes duplicated between systems.** Codes like `"%"`, `"cm"`,
   `"mg/dL"` appear in both the R4/R5 dictionary AND in
   `FF_UCUM_Concepts.cpp` with DIFFERENT permanent IDs in each.

3. **Two lookup mechanisms.** `FF_GetDictionaryCode` / `FF_ResolveCode` for
   general FHIR codes, and `FF_GetUCUMCode` / `FF_ResolveUCUMCode` for UCUM.
   Same pattern, different implementations.

4. **No shared string table.** Each `.cpp` file owns its own string
   literals. There is no master index of all known code strings.

5. **No compile-time guard.** The dictionary generator verifies drift via
   `git show HEAD` at generation time, but the compiled binary has no
   `static_assert` that the table sizes are consistent.

### 1.3 Current Lookup Flow

```
FF_GetDictionaryCode(string_view) → O(1) lazy hash map → uint32_t code
FF_ResolveCode(uint32_t code)     → O(log n) binary search → const char* label

FF_GetUCUMCode(string_view)       → O(1) lazy hash map → FF_UCUM_CODES
FF_ResolveUCUMCode(FF_UCUM_CODES) → O(1) array index    → const char* label
```

---

## 2. Target Architecture

### 2.1 Single Master Dictionary

One JSON source file drives all output. The generator produces:

| Output | Contents |
|---|---|
| `dictionaries/FF_Dictionary_Strings.cpp` | Master string table — every unique code string, indexed by permanent 31-bit ID |
| `dictionaries/FF_R4_Dictionary.cpp` | R4 code list — `FF_CodeEntry{code, label*}` pointing into the string table |
| `dictionaries/FF_R5_Dictionary.cpp` | R5 code list — same pattern |
| `dictionaries/FF_UCUM_Codes.hpp` | `FF_UCUM_CODES` enum — values match dictionary IDs |

### 2.2 Data Layout

```cpp
// dictionaries/FF_Dictionary_Strings.cpp
extern const char* const FF_DICTIONARY_STRINGS[] = {
    nullptr,             // 0 = invalid
    "%",                 // 1
    "%/100{WBC}",        // 2
    // ... all ~22,000 codes in alphabetical order
};

static_assert(sizeof(FF_DICTIONARY_STRINGS) / sizeof(FF_DICTIONARY_STRINGS[0])
              == FF_DICTIONARY_STRINGS_SIZE);
```

```cpp
// dictionaries/FF_R4_Dictionary.cpp
static const FF_CodeEntry kR4Table[] = {
    { 1, FF_DICTIONARY_STRINGS[1] },
    { 5, FF_DICTIONARY_STRINGS[5] },
    // ... only codes present in R4
};
```

```cpp
// dictionaries/FF_UCUM_Codes.hpp
enum class FF_UCUM_CODES : uint64_t {
    UCUM_INVALID = 0,
    UCUM_PERCENT = 1,                // ID 1 in master dictionary
    UCUM_PERCENT_PER_100_WBC = 2,    // ID 2
    UCUM_TOT = 1613,                 // ID 1613
};
```

### 2.3 Unified Lookup Flow

```
FF_GetDictionaryCode(string_view) → O(1) lazy hash map → uint32_t code
FF_ResolveCode(uint32_t code)     → O(1) index into FF_DICTIONARY_STRINGS

FF_GetUCUMCode(string_view)       → calls FF_GetDictionaryCode → FF_UCUM_CODES
FF_ResolveUCUMCode(FF_UCUM_CODES) → calls FF_ResolveCode
```

### 2.4 What Dies

| File | Disposition |
|---|---|
| `dictionaries/FF_UCUM_Concepts.cpp` | Deleted — merged into dictionary |
| `FF_UCUM_STRINGS` array | Deleted — replaced by `FF_DICTIONARY_STRINGS` |
| `kUCUMTable` | Deleted — UCUM codes resolved via `FF_GetDictionaryCode` |
| UCUM lazy hash map | Deleted — calls `FF_GetDictionaryCode` shared map |
| UCUM inline reverse table | Deleted — calls `FF_ResolveCode` |

### 2.5 What Stays

| Artifact | Role |
|---|---|
| `FF_UCUM_Codes.hpp` | `FF_UCUM_CODES` enum — values sourced from dictionary IDs |
| `FF_GetUCUMCode` / `FF_ResolveUCUMCode` | Thin inline wrappers |
| `FF_CodeEntry` struct | Unchanged |
| `FF_GetDictionaryCode` / `FF_ResolveCode` | Core lookup — now O(1) both directions |

---

## 3. Master JSON Data Format

### 3.1 Schema

```json
{
  "version": 1,
  "generated": "2025-07-18T00:00:00Z",
  "source": "FHIR R4+R5 ValueSets + UCUM essence v2.2 + FHIR R5 UCUM expansion",
  "codes": [
    {
      "id": 1,
      "label": "%",
      "versions": [],
      "enums": ["FF_UCUM_CODES::UCUM_PERCENT"]
    },
    {
      "id": 2,
      "label": "%/100{WBC}",
      "versions": [],
      "enums": ["FF_UCUM_CODES::UCUM_PERCENT_PER_100_WBC"]
    },
    {
      "id": 15723,
      "label": "required",
      "versions": ["R4", "R5"],
      "enums": []
    },
    {
      "id": 18100,
      "label": "active",
      "versions": ["R4", "R5"],
      "enums": []
    },
    {
      "id": 20771,
      "label": "cm",
      "versions": [],
      "enums": ["FF_UCUM_CODES::UCUM_CM"]
    }
  ]
}
```

| Field | Type | Description |
|---|---|---|
| `id` | `uint32_t` | Permanent 31-bit ID. Sorted alphabetically by `label`. |
| `label` | `string` | Code string. Unique across all entries. |
| `versions` | `string[]` | Which FHIR versions this code appears in. |
| `enums` | `string[]` | Named enum identifiers mapping to this code. |

### 3.2 Deduplication Rules (UCUM > R4 > R5)

Codes may appear in multiple sources.  The generator applies these rules
in order when building the JSON:

| Priority | Source | Rule |
|---|---|---|
| 1 (highest) | **UCUM** | All 1,613 UCUM codes are included.  If a UCUM code also appears in an R4 or R5 ValueSet, it is REMOVED from the R4/R5 version lists — UCUM owns it.  The code gets an `enums` entry but an empty `versions` array. |
| 2 | **R4** | All R4 ValueSet codes that are NOT already in UCUM are included.  They get `"versions": ["R4"]`. |
| 3 (lowest) | **R5** | All R5 ValueSet codes that are NOT in UCUM AND NOT in R4 are included.  They get `"versions": ["R5"]`.  Codes present in BOTH R4 and R5 (but not UCUM) get `"versions": ["R4", "R5"]` and are included under R4 priority. |

**Result:** Every code string appears exactly once in the JSON.  The `versions`
and `enums` fields are mutually exclusive — a code owned by UCUM has an empty
`versions` array, and a code owned by R4/R5 has an empty `enums` array.

**Example:** `"%"` appears in UCUM (as `UCUM_PERCENT`) AND in R4/R5 ValueSets.
After dedup, it appears once with `"enums": ["FF_UCUM_CODES::UCUM_PERCENT"]`
and `"versions": []`.  R4 and R5 do NOT include `"%"` in their code lists.

### 3.3 Per-Version Code Lists

From the deduplicated JSON, the generator produces:

| Output | Contents |
|---|---|
| `FF_R4_Dictionary.cpp` | Codes where `versions` includes `"R4"` |
| `FF_R5_Dictionary.cpp` | Codes where `versions` includes `"R5"` |
| `FF_UCUM_Codes.hpp` | Codes where `enums` is non-empty with `FF_UCUM_CODES::*` |

A UCUM code is NOT in the R4 or R5 code lists.  It's resolved via
`FF_GetDictionaryCode` (which searches the FULL master string table)
or `FF_GetUCUMCode` (the typed wrapper).

### 3.4 ID Assignment

1. Sort ALL labels alphabetically.
2. Assign sequential IDs from 1.
3. Existing IDs preserved from committed dictionaries.
4. New codes (UCUM expansions, future additions) get IDs after current max.
5. Once assigned, an ID is permanent — the drift detector enforces this.

### 3.5 Drift Detection

Before writing any output, the generator:
1. Reads committed files via `git show HEAD`.
2. Verifies existing codes have same `id` and `label`.
3. Verifies existing enum identifiers have not changed.
4. Aborts with detailed diff on drift.
5. Only new codes may be added.

---

## 4. Generator Refactor

### 4.1 New Pipeline Step

```
pipeline.py::run()
  ├─ fetch_fhir_specs()
  ├─ build_master_json()           ← NEW
  │    ├─ Reads committed dictionaries via git show HEAD
  │    ├─ Scans FHIR ValueSet bundles for new codes
  │    ├─ Merges UCUM codes from FHIR expansion scrape
  │    ├─ Assigns permanent IDs
  │    ├─ Verifies no drift
  │    └─ Writes master_codes.json
  ├─ generate_from_master_json()   ← NEW
  │    ├─ Reads master_codes.json
  │    ├─ Writes FF_Dictionary_Strings.cpp
  │    ├─ Writes FF_R4_Dictionary.cpp
  │    ├─ Writes FF_R5_Dictionary.cpp
  │    └─ Writes FF_UCUM_Codes.hpp
  ├─ generate_code_systems()       ← unchanged
  └─ compile_fhir_library()        ← unchanged
```

### 4.2 New Module

`generator/emit/master_dictionary.py` — replaces `dictionary.py`:
- `build_master_json(input_dir, output_dir)` — builds the JSON
- `generate_from_master_json(json_path, output_dir)` — emits all C++ files

---

## 5. Runtime Lookup Refactor

### 5.1 FF_Dictionary.hpp

Add string table declaration:
```cpp
extern const char* const FF_DICTIONARY_STRINGS[];
extern const size_t      FF_DICTIONARY_STRINGS_SIZE;
```

`FF_ResolveCode` becomes O(1):
```cpp
inline const char* FF_ResolveCode(uint32_t code, uint32_t version) noexcept {
    if (code == FF_CODE_NULL || code >= FF_DICTIONARY_STRINGS_SIZE)
        return nullptr;
    return FF_DICTIONARY_STRINGS[code];
}
```

### 5.2 FF_UCUM_Codes.hpp

Becomes thin wrappers:
```cpp
inline FF_UCUM_CODES FF_GetUCUMCode(std::string_view label) noexcept {
    return static_cast<FF_UCUM_CODES>(
        FF_GetDictionaryCode(std::string(label), FHIR_VERSION_R5));
}

inline const char* FF_ResolveUCUMCode(FF_UCUM_CODES code) noexcept {
    return FF_ResolveCode(static_cast<uint32_t>(code), FHIR_VERSION_R5);
}
```

### 5.3 FF_Dictionary.cpp

`FF_GetDictionaryCode` already uses a lazy static `unordered_map`. Change
to `static const` lambda initialization pattern (no raw `new`).

`FF_ResolveCode` body moves to header as inline (O(1) array lookup).

---

## 6. Enum Generation

From `master_codes.json`, for each unique enum prefix (`FF_UCUM_CODES`):
1. Collect entries where `enums` contains that prefix.
2. Sort by `id` ascending.
3. Generate `enum class FF_UCUM_CODES : uint64_t { IDENT = id, ... };`

Identifier generation rules (UCUM expression → UPPER_SNAKE_CASE) unchanged.
Collision handling: first by ID keeps base name, suffixes `_2`, `_3`, etc.

---

## 7. Migration Sequence

### Phase 1: Master JSON (no file changes)
1. Write `build_master_json()` in `generator/emit/master_dictionary.py`.
2. Run once to produce `master_codes.json`.
3. Verify IDs match committed dictionaries via `git diff`.
4. Commit `master_codes.json` as source of truth.

### Phase 2: String Table
1. Write `generate_from_master_json()`.
2. Update `FF_Dictionary.hpp` with string table declarations.
3. Regenerate `FF_R4_Dictionary.cpp` / `FF_R5_Dictionary.cpp`.
4. Update `FF_ResolveCode` to O(1) array lookup.
5. Commit.

### Phase 3: UCUM Merge
1. Add UCUM codes to `master_codes.json` with `enums` field.
2. Regenerate all files.
3. Delete `dictionaries/FF_UCUM_Concepts.cpp`.
4. Simplify `FF_UCUM_Codes.hpp` to thin wrappers.
5. Update `FF_CodeableConcept.cpp`.
6. Commit.

### Phase 4: Cleanup
1. Remove UCUM-specific includes and globals from `FF_CodeableConcept.cpp`.
2. Delete `generator/emit/dictionary.py`.
3. Update `pipeline.py` to use new module.
4. Regenerate everything.
5. `git diff` to verify zero drift.

---

## 8. static_assert Guards

| Guard | Location |
|---|---|
| String table size matches declared size | `FF_Dictionary_Strings.cpp` |
| Last R4 entry's code < string table size | `FF_R4_Dictionary.cpp` |
| Last R5 entry's code < string table size | `FF_R5_Dictionary.cpp` |
| Enum count documented in comment | `FF_UCUM_Codes.hpp` |

Drift detection by the generator (`_verify_no_drift`) is the primary guard
for semantic correctness — `static_assert` catches structural mismatches.

---

## Appendix A: File Inventory After Migration

| File | Status |
|---|---|
| `dictionaries/master_codes.json` | NEW |
| `dictionaries/FF_Dictionary_Strings.cpp` | NEW |
| `dictionaries/FF_R4_Dictionary.cpp` | UPDATED |
| `dictionaries/FF_R5_Dictionary.cpp` | UPDATED |
| `dictionaries/FF_UCUM_Codes.hpp` | UPDATED |
| `dictionaries/FF_LOINC_Concepts.cpp` | DELETED |
| `dictionaries/FF_UCUM_Concepts.cpp` | DELETED |
| `dictionaries/FF_SNOMED_Concepts.cpp` | KEPT (self-encoding) |
| `include/FF_Dictionary.hpp` | UPDATED |
| `include/FF_CodeableConcept.hpp` | UPDATED |
| `src/FF_Dictionary.cpp` | UPDATED |
| `src/FF_CodeableConcept.cpp` | UPDATED |
| `generator/emit/master_dictionary.py` | NEW |
| `generator/emit/dictionary.py` | DELETED |

> **Status:** Architecture Proposal  
> **Scope:** Merge `FF_UCUM_Concepts` into the master R4/R5 dictionary system.
> Eliminate code duplication, unify lookup paths, and introduce a shared
> string table with compile-time safety guards.

---

## 1. Current State Analysis

### 1.1 Three separate data stores

| Store | Entries | Key Type | Lookup |
|---|---|---|---|
| `FF_R4_Dictionary.cpp` | 20,770 | `uint32_t` (31-bit) | Binary search + lazy hash map |
| `FF_R5_Dictionary.cpp` | 6,225 | `uint32_t` (31-bit) | Binary search + lazy hash map |
| `FF_UCUM_Concepts.cpp` | 1,613 | `FF_UCUM_CODES` (56-bit enum) | Binary search + lazy hash map + string table |

### 1.2 Known overlap

The R4 dictionary already contains UCUM codes that appear in FHIR ValueSets:

| UCUM Code | In R4 Dict? | In FF_UCUM_Concepts? |
|---|---|---|
| `"%"` | Yes | Yes |
| `"cm"` | Yes | Yes |
| `"mg/dL"` | Yes | Yes |
| `"[in_i]"` | Yes | Yes |
| `"%{Abnormal}"` | Yes | Yes |

Every UCUM code in the dictionary has TWO permanent IDs — one from the
dictionary (31-bit, shared across all code systems) and one from
`FF_UCUM_CODES` (56-bit, UCUM-only).  We have no way to reconcile them.

### 1.3 Structural duplication

- **String duplication between R4 and R5**: codes present in both versions
  store their label string twice — once in each `.cpp` file.
- **String duplication between dictionary and UCUM**: the same label string
  appears in `FF_R4_Dictionary.cpp` and `FF_UCUM_Concepts.cpp`.
- **Two hash maps**: `FF_GetDictionaryCode` builds a lazy
  `unordered_map<string_view, uint32_t>`.  `FF_GetUCUMCode` builds a
  separate lazy `unordered_map<string_view, FF_UCUM_CODES>`.

### 1.4 No compile-time guards

The R4/R5 dictionary generator verifies drift via `git show HEAD` at
generation time, but there is no `static_assert` at compile time.  UCUM
has `static_assert` on the string table size — but this should apply to
the entire dictionary, not just one code system.

---

## 2. Target Architecture

### 2.1 Master string table

One `extern const char* const FF_DICTIONARY_STRINGS[]` array, indexed by
permanent 31-bit dictionary ID.  Each code's label string appears exactly
once in the entire codebase.

```
Index 0:   nullptr
Index 1:   "1"              (FHIR code)
Index 42:  "%"              (UCUM code, also a FHIR code)
Index 157: "active"         (FHIR code)
...
Index 22167: "wk"           (UCUM code)
```

### 2.2 Unified FF_CodeEntry

```cpp
struct FF_CodeEntry {
    uint32_t    code;    // permanent 31-bit ID (= index into string table)
    // label is FF_DICTIONARY_STRINGS[code]
};
```

The `label` pointer is derived from the string table at runtime.  The
entry itself is 4 bytes — down from 16 (8-byte pointer + 4-byte code +
padding).

### 2.3 Per-version code lists

R4 and R5 each define which codes are present in that version.  Instead
of duplicating the string data, they store only the 31-bit code values:

```cpp
// dictionaries/FF_R4_Dictionary.cpp
extern const uint32_t FF_R4_CODES[] = { 1, 3, 5, 42, 157, ... };
extern const size_t   FF_R4_CODES_SIZE = ...;
```

The `FF_ResolveCode` function does a binary search on `FF_R4_CODES` to
verify the code exists in the requested version, then reads
`FF_DICTIONARY_STRINGS[code]` to get the string.

### 2.4 Per-system typed enums

Each external code system gets a typed enum whose integer values are the
dictionary's permanent 31-bit IDs:

```cpp
enum class FF_UCUM_CODES : uint32_t {
    UCUM_INVALID = 0,
    UCUM_PERCENT = 42,           // dictionary ID of "%"
    UCUM_MG_PER_DL = 8432,       // dictionary ID of "mg/dL"
    ...
};
```

The enum values ARE the dictionary IDs.  No separate numbering.

### 2.5 Unified lookup functions

| Function | Implementation |
|---|---|
| `FF_GetDictionaryCode(string)` | Lazy `static const unordered_map` (unchanged) |
| `FF_ResolveCode(code, version)` | Binary search version code list → `FF_DICTIONARY_STRINGS[code]` |
| `FF_GetUCUMCode(string)` | `return static_cast<FF_UCUM_CODES>(FF_GetDictionaryCode(string))` |
| `FF_ResolveUCUMCode(code)` | `return FF_DICTIONARY_STRINGS[static_cast<uint32_t>(code)]` |
| `FF_GetLOINCCode(string)` | (future) same pattern |
| `FF_ResolveLOINCCode(code)` | (future) same pattern |

### 2.6 Compile-time guards

```cpp
// dictionaries/FF_Dictionary_Strings.cpp
static_assert(sizeof(FF_DICTIONARY_STRINGS) / sizeof(FF_DICTIONARY_STRINGS[0])
              == FF_CODE_DICTIONARY_MAX + 1,
              "string table size mismatch with dictionary capacity");
```

Per-version code lists also get `static_assert` guards verifying their
size matches the generator output.

---

## 3. JSON Data Format

The generator reads a master JSON file that describes every code in the
dictionary.  This file is the single source of truth.

### 3.1 Schema

```jsonc
{
  "version": 1,
  "systems": {
    "ucum": {
      "url": "http://unitsofmeasure.org",
      "export_enum": "FF_UCUM_CODES",
      "export_header": "dictionaries/FF_UCUM_Codes.hpp"
    }
    // future: "loinc", "snomed", "bcp47", "mime"
  },
  "codes": [
    {
      "id": 1,                    // permanent 31-bit ID
      "label": "1",               // FHIR code string
      "versions": ["R4"],         // which FHIR versions include this code
      "system": null              // no external code system (plain FHIR code)
    },
    {
      "id": 42,
      "label": "%",
      "versions": ["R4", "R5"],
      "system": "ucum",
      "enum_identifier": "UCUM_PERCENT"
    },
    {
      "id": 157,
      "label": "active",
      "versions": ["R4", "R5"],
      "system": null
    },
    {
      "id": 8432,
      "label": "mg/dL",
      "versions": ["R4", "R5"],
      "system": "ucum",
      "enum_identifier": "UCUM_MG_PER_DL"
    }
  ]
}
```

### 3.2 Field descriptions

| Field | Type | Description |
|---|---|---|
| `id` | `uint32_t` | Permanent dictionary ID. Never reused. |
| `label` | `string` | The exact FHIR / UCUM code string. |
| `versions` | `string[]` | Which FHIR versions include this code. `["R4"]`, `["R5"]`, `["R4","R5"]`. |
| `system` | `string?` | External code system key (from `systems` map), or `null` for plain FHIR codes. |
| `enum_identifier` | `string?` | C++ enumerator name. Required when `system` is non-null. |

### 3.3 Generator outputs

The Python generator (`generator/emit/dictionary.py`) reads the JSON and
produces:

| Output | Content |
|---|---|
| `dictionaries/FF_Dictionary_Strings.cpp` | `FF_DICTIONARY_STRINGS[]` — master string table + `static_assert` |
| `dictionaries/FF_R4_Codes.cpp` | `FF_R4_CODES[]` — sorted list of codes present in R4 |
| `dictionaries/FF_R5_Codes.cpp` | `FF_R5_CODES[]` — sorted list of codes present in R5 |
| `dictionaries/FF_UCUM_Codes.hpp` | `FF_UCUM_CODES` enum — values = dictionary IDs + `inline` lookup wrappers |

---

## 4. Refactor Sequence

### Phase 1 — Master string table (no behavior change)

1. Create `dictionaries/FF_Dictionary_Strings.cpp` with
   `FF_DICTIONARY_STRINGS[]` extracted from the existing
   `FF_R4_Dictionary.cpp` (which has the superset of codes).

2. Change `FF_CodeEntry` in `FF_Dictionary.hpp`:
   ```cpp
   struct FF_CodeEntry {
       uint32_t code;  // permanent 31-bit ID (= index into FF_DICTIONARY_STRINGS)
   };
   ```
   Remove the `label` pointer.

3. Update `FF_R4_Dictionary.cpp` and `FF_R5_Dictionary.cpp` to use the new
   `FF_CodeEntry` format (code-only, no label duplication).

4. Update `FF_ResolveCode`:
   ```cpp
   const char* FF_ResolveCode(uint32_t code, uint32_t version) noexcept {
       if (code >= FF_DICTIONARY_STRINGS_SIZE) return nullptr;
       // verify code exists in requested version via binary search
       if (!code_in_version(code, version)) return nullptr;
       return FF_DICTIONARY_STRINGS[code];
   }
   ```

5. Update `FF_GetDictionaryCode` — the lazy hash map now maps
   `string_view → uint32_t` (code index), and the resolved string comes
   from `FF_DICTIONARY_STRINGS[code]`.

6. `static_assert` the string table size against the max dictionary code.

### Phase 2 — Merge UCUM codes

1. Add all 1,613 UCUM codes to the master JSON.  Existing codes (those
   already in the dictionary) keep their permanent IDs.  New codes get IDs
   assigned at the end of the sequence.

2. Regenerate `FF_Dictionary_Strings.cpp` with the merged set.

3. Generate `FF_UCUM_Codes.hpp` with enum values = dictionary IDs.

4. Reimplement `FF_GetUCUMCode` as a wrapper:
   ```cpp
   inline FF_UCUM_CODES FF_GetUCUMCode(std::string_view label) noexcept {
       return static_cast<FF_UCUM_CODES>(FF_GetDictionaryCode(label));
   }
   ```

5. Reimplement `FF_ResolveUCUMCode`:
   ```cpp
   inline const char* FF_ResolveUCUMCode(FF_UCUM_CODES code) noexcept {
       return FF_DICTIONARY_STRINGS[static_cast<uint32_t>(code)];
   }
   ```

6. Kill `dictionaries/FF_UCUM_Concepts.cpp` and `FF_UCUM_STRINGS`.

### Phase 3 — Generator migration

1. Create `tools/generator/dictionary_data.json` — the master JSON file
   populated from the existing dictionaries + UCUM concepts.

2. Rewrite `generator/emit/dictionary.py` to read the JSON and produce
   the new output files (string table, per-version code lists, per-system
   enum headers).

3. The generator retains drift detection via `git show HEAD` — any change
   to a committed permanent ID is a hard error.

### Phase 4 — Per-system enum generation

1. For each system in `systems` with `export_enum` set, generate a
   header file with the typed enum and inline lookup wrappers.

2. The enum values are extracted from the JSON: every code entry where
   `system == "ucum"` gets an enumerator with the specified
   `enum_identifier` and the code's permanent `id` as the value.

3. Future systems (LOINC, SNOMED, BCP-47) follow the same pattern — just
   add entries to the JSON and a system definition.

---

## 5. File Manifest

### Before (current)

```
dictionaries/
  FF_R4_Dictionary.cpp       FF_CodeEntry{uint32_t, const char*}[]
  FF_R5_Dictionary.cpp       FF_CodeEntry{uint32_t, const char*}[]
  FF_UCUM_Concepts.cpp       FF_ConceptEntry{FF_UCUM_CODES, const char*}[]
  FF_UCUM_Codes.hpp          FF_UCUM_CODES enum + inline lookup
  FF_LOINC_Concepts.cpp      placeholder
  FF_SNOMED_Concepts.cpp     placeholder

include/
  FF_Dictionary.hpp          FF_CodeEntry struct + extern decls + function decls
  FF_CodeableConcept.hpp     FF_PackCode / FF_UnpackCode decls

src/
  FF_Dictionary.cpp          FF_ResolveCode + FF_GetDictionaryCode impl
  FF_CodeableConcept.cpp     FF_PackCode / FF_UnpackCode impl
```

### After (target)

```
dictionaries/
  FF_Dictionary_Strings.cpp  FF_DICTIONARY_STRINGS[] + static_assert
  FF_R4_Codes.cpp            FF_R4_CODES[] (uint32_t array)
  FF_R5_Codes.cpp            FF_R5_CODES[] (uint32_t array)
  FF_UCUM_Codes.hpp          FF_UCUM_CODES enum + inline wrappers

include/
  FF_Dictionary.hpp          FF_CodeEntry (code-only) + extern decls + function decls

src/
  FF_Dictionary.cpp          FF_ResolveCode + FF_GetDictionaryCode impl

generator/
  emit/dictionary.py         reads dictionary_data.json, produces all outputs
  dictionary_data.json        single source of truth

KILLED:
  dictionaries/FF_R4_Dictionary.cpp
  dictionaries/FF_R5_Dictionary.cpp
  dictionaries/FF_UCUM_Concepts.cpp
  dictionaries/FF_LOINC_Concepts.cpp
  src/FF_CodeableConcept.cpp  (FF_PackCode/FF_UnpackCode move to FF_Dictionary.cpp)
  include/FF_CodeableConcept.hpp
```

---

## 6. Lookup Function Signatures (Final)

```cpp
// ── Generic dictionary ──────────────────────────────────────────

/// string → permanent 31-bit ID.  O(1) via lazy hash map.
/// Returns 0 if not found.
uint32_t FF_GetDictionaryCode(std::string_view label) noexcept;

/// 31-bit ID → string.  O(1) via string table.
/// Returns nullptr if ID is not present in the requested FHIR version.
const char* FF_ResolveCode(uint32_t code, uint32_t version) noexcept;

// ── UCUM typed wrappers ─────────────────────────────────────────

/// UCUM expression → typed enum.  O(1).
inline FF_UCUM_CODES FF_GetUCUMCode(std::string_view label) noexcept {
    return static_cast<FF_UCUM_CODES>(FF_GetDictionaryCode(label));
}

/// Typed enum → UCUM expression string.  O(1).
inline const char* FF_ResolveUCUMCode(FF_UCUM_CODES code) noexcept {
    extern const char* const FF_DICTIONARY_STRINGS[];
    return FF_DICTIONARY_STRINGS[static_cast<uint32_t>(code)];
}

// ── Future systems ──────────────────────────────────────────────

// inline FF_LOINC_CODES FF_GetLOINCCode(std::string_view label) noexcept { ... }
// inline const char* FF_ResolveLOINCCode(FF_LOINC_CODES code) noexcept { ... }
```
