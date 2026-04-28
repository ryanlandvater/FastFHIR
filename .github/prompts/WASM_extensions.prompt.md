# FastFHIR Extension Subsystem: Implementation Plan

**Context:** We are implementing an "Open World" FHIR extension subsystem for the FastFHIR C++ library. FastFHIR uses a lock-free, zero-copy architecture backed by an offset-based memory arena. We are using the WebAssembly Micro Runtime (WAMR). This document also covers the URL deduplication design that is a prerequisite for the WASM work.

---

## ⚠️ URGENT TODO — Header Redesign (Required Before Phase 5+)

Two architectural corrections must be made before Phase 5+ work begins. Both affect binary format and the stream parsing path.

### 1. Eliminate the Stream Preamble — Move `URL_DIR_OFFSET` into `FF_HEADER`

The 16-byte preamble at byte 0 (`FF_STREAM_MAGIC | URL_DIR_OFFSET`) is the wrong abstraction. `FF_URL_DIRECTORY` is a data block exactly like `FF_CHECKSUM` — its offset belongs in `FF_HEADER` alongside `CHECKSUM_OFFSET`, not in a magic-byte prefix that forces the Parser to detect two different stream layouts.

**Required changes:**

- **`FF_HEADER`**: Add `URL_DIR_OFFSET` (8 bytes) between `CHECKSUM_OFFSET` and `VERSION`. New layout **(46 bytes total)**:
  ```
  MAGIC(4) | RECOVERY(2) | FHIR_REV(2) | STREAM_SIZE(8) | ROOT_OFFSET(8) |
  ROOT_RECOVERY(2) | CHECKSUM_OFFSET(8) | URL_DIR_OFFSET(8) | VERSION(4)
  ```
  `URL_DIR_OFFSET = FF_NULL_OFFSET` when no URL directory is present.

- **Remove** `FF_STREAM_MAGIC`, `FF_STREAM_PREAMBLE_SIZE`, `FF_STREAM_PREAMBLE_URL_DIR` constants from `FF_Primitives.hpp`.
- **`STORE_FF_HEADER`**: Add `Offset url_dir_offset = FF_NULL_OFFSET` parameter. Write it at the new vtable position.
- **`FF_Builder` constructor**: Remove the preamble `claim_space` + `STORE_U64` writes. `FF_HEADER` returns to offset 0. `FF_PredigestExtensionURLs` back-patches `URL_DIR_OFFSET` via a direct `STORE_U64` into the header field — same pattern as checksum back-patching.
- **`FF_Parser`**: Remove `m_header_base_offset` member entirely. Read `m_url_dir_offset` from the new `FF_HEADER` field — identical to how `m_checksum_offset` is read today. Remove the `FF_STREAM_MAGIC` detection branch from both constructors.
- **`FF_Ingestor.cpp`**: Replace `STORE_U64(base + FF_STREAM_PREAMBLE_URL_DIR, dir_off)` with a direct write to `FF_HEADER::URL_DIR_OFFSET`.

### 2. Add `MODULE_REGISTRY_OFFSET` to `FF_HEADER` — WASM Module Binding

Each unique extension URL in `FF_URL_DIRECTORY` must eventually map to a WASM binary from `registry.fastfhir.org`. The binding lives in a separate optional `FF_MODULE_REGISTRY` block pointed to by a second new `FF_HEADER` field. Add the field now so the header layout is stable before WASM work begins.

**`FF_HEADER` with both additions **(54 bytes total)**:**
```
MAGIC(4) | RECOVERY(2) | FHIR_REV(2) | STREAM_SIZE(8) | ROOT_OFFSET(8) |
ROOT_RECOVERY(2) | CHECKSUM_OFFSET(8) | URL_DIR_OFFSET(8) | MODULE_REG_OFFSET(8) | VERSION(4)
```
`MODULE_REG_OFFSET = FF_NULL_OFFSET` means no WASM module bindings stored — stream is fully valid without it.

**`FF_MODULE_REGISTRY` block layout (written in Phase 7):**
```
VALIDATION(8) | RECOVERY(2) | ENTRY_COUNT(2) | PAD(4)   ← 16-byte header
ENTRY_TABLE: ENTRY_COUNT × 40 bytes each
  ModuleEntry: url_idx(4) | pad(4) | module_hash(32)     ← SHA-256 of canonical URL
```
- `url_idx` maps directly into `FF_URL_DIRECTORY` entry index.
- `module_hash = SHA-256(canonical_url)` is both the registry lookup key and the on-disk cache filename (`~/.cache/fastfhir/{hex_hash}.wasm`).
- Storing the hash (not the URL string) avoids re-hashing on every cache lookup and provides an independent integrity check before loading into WAMR.

**Parser resolution model (Phase 7):**
On `parser.resolve_modules()` (explicit, non-blocking):
1. Read `MODULE_REG_OFFSET` — if `FF_NULL_OFFSET`, return immediately.
2. For each `ModuleEntry`: check disk cache; if missing, enqueue async fetch to `https://registry.fastfhir.org/v1/modules/{hex_hash}.wasm`.
3. Fetched bytes are SHA-256 verified against stored `module_hash` before loading into WAMR. Reject on mismatch.
4. Unresolved modules are logged; ingest and field access proceed normally without WASM dispatch for those URLs.

---

## Purpose of a WASM Extension Module

A WASM extension module has **one job**: decode the binary layout of a specific FHIR extension type to prove it recognizes the schema.

The module is compiled from the `EXTENSIONView` struct that `ffc.py` generates. It receives a staging buffer containing raw extension bytes, applies the View accessors, and returns whether it recognised the extension.

- **No writes** outside its own linear memory.
- **No host callbacks** for field access — all reads are direct loads from the staging buffer.
- **No heap allocation** — View structs are stack-only wrappers over `const BYTE*`.
- The only host import is `ff_log_warning(uint32_t msg_ptr, uint32_t len)`.
- `get_url()` is pre-resolved by the host before the WASM call — the staging buffer receives the concatenated URL string so the WASM guest never needs the URL directory.

Returns: `0=ok`, `1=unknown`, `2=reject-resource` (modifier not understood), `3=error`.

---

## Ground Truth: Binary Representations

### FF_EXTENSION vtable (40 bytes — updated layout)
```
VALIDATION (8) | RECOVERY (2) | ID (8) | EXTENSION (8) | URL_IDX (4) | VALUE (10)
```
`URL_IDX` is a `uint32_t` index into the stream-level `FF_URL_DIRECTORY`. This replaces the old 8-byte `Offset → FF_STRING` field, saving 4 bytes per extension entry.

### ChoiceEntry (10 bytes inline)
```
raw bits (8) | RECOVERY_TAG (2)
```
`get_value()` → `Decode::choice(base, offset + FF_EXTENSION::VALUE)`

### FF_URL_DIRECTORY block
```
VALIDATION (8) | RECOVERY (2) | PREFIX_COUNT (2) | ENTRY_COUNT (4)     ← 16-byte header
PREFIX_TABLE: PREFIX_COUNT × 8-byte Offsets → FF_STRING blocks          ← base URL paths
ENTRY_TABLE: ENTRY_COUNT × 12-byte URLEntry inline structs
  URLEntry: prefix_idx (2) | pad (2) | leaf_offset (8) → FF_STRING
```
`get_url(uint32_t idx)` → prefix `string_view` + "/" + leaf `string_view` concatenated into `std::string`.

URL split rule: at the last `/`. Edge case (no `/`): `NO_PREFIX` (0xFFFF) sentinel; full URL stored as leaf only.

### Stream Preamble (16 bytes at byte 0 of every FastFHIR binary stream)
```
STREAM_MAGIC (8) — fixed constant: format identifier + version
URL_DIR_OFFSET (8) — Offset to FF_URL_DIRECTORY; FF_NULL_OFFSET until back-patched at stream close
```
Not a vtable struct — fixed-position header the Parser reads before anything else.
`FF_HEADER` is written immediately after the preamble at offset 16.

---

## Architectural Constraint: Batch Buffer Model

**Do NOT** alias the FastFHIR arena into WAMR guest linear memory. WAMR does not support remapping its own allocation.

**Do NOT** pass 64-bit `Offset` values as WASM parameters. `Offset` is `uint64_t`; standard WASM is 32-bit.

**The correct model:**
1. At module instantiation, allocate a staging buffer inside WAMR guest linear memory (`wasm_module_inst_malloc` or a reserved region). Store the `uint32_t` guest-side address.
2. Before each batch dispatch, the host: (a) resolves the URL string from the directory, (b) copies extension block bytes + resolved URL string into the staging buffer — one `memcpy` each.
3. The guest receives a `uint32_t` linear-memory pointer. `LOAD_U64(ptr)` compiles to `i64.load`.

Write head safety is automatic: only extension block bytes enter the staging buffer.

---

## Known-Extension Filter

Three categories of URLs are suppressed:

1. **Profile-native** — extensions already captured as native fields by the compiled US Core / UK CORE / base profile (e.g. `us-core-race`, `us-core-ethnicity`, `us-core-birthsex`). The data is already stored in the resource's own vtable.
2. **HL7-known safe** — extensions in the HL7 base spec known to be informational and not modifier-semantics (e.g. `patient-mothersMaidenName`, `geolocation`).

Both categories are compiled into `generated_src/FF_KnownExtensions.hpp` as a `constexpr` sorted array of `SHA-256(url)` hashes. Lookup is `O(log n)` binary search.

`FF_ExtensionFilterMode` enum:
- `FILTER_ALL_KNOWN` (default) — suppress both categories
- `FILTER_NATIVE_ONLY` — suppress only profile-native (category 1)
- `FILTER_NONE` — store all extensions, dispatch everything to WASM

---

## Core Architectural Principle: Predigestion

**The URL directory is built in a dedicated predigestion pass before any resource data is written.** This decouples URL interning from ingestion entirely. The main ingest workers (including concurrent Bundle entry workers) never acquire a mutex for URL lookups — they receive a const, immutable `url_to_index` map and do a single read.

```
┌─────────────────────────────────────────────────────────┐
│  PREDIGESTION (single-threaded, before workers start)   │
│  1. Write stream preamble (16 bytes) at arena offset 0  │
│  2. Claim space for FF_HEADER (38 bytes) at offset 16   │
│  3. Fast-scan raw JSON for all extension URL strings    │
│  4. Filter known URLs (SHA-256 binary search)           │
│  5. Deduplicate + prefix-compress surviving URLs        │
│  6. Write FF_STRING blocks (prefixes, then leaves)      │
│  7. Write FF_URL_DIRECTORY header + tables              │
│  8. Back-patch stream preamble URL_DIR_OFFSET           │
│  9. Return immutable FF_UrlInternState (url_to_index)   │
└─────────────────────────────────────────────────────────┘
         │
         │ const FF_UrlInternState& (read-only)
         ▼
┌─────────────────────────────────────────────────────────┐
│  MAIN INGEST (concurrent workers, lock-free)            │
│  For each extension url string in JSON:                 │
│    lookup url_to_index → FF_NULL_UINT32: skip block     │
│                        → valid index: write URL_IDX     │
└─────────────────────────────────────────────────────────┘
```

The predigestion pass uses simdjson to do a structural scan for `"url"` values under any `extension` or `modifierExtension` array at any nesting depth. It does not fully parse every field — it only extracts string values for keys named `"url"`. This makes it nearly as fast as a raw byte scan while being JSON-correct.

---

## Implementation Phases

### Phase 1 — New Binary Structures (Prerequisites) ✅ COMPLETE

**Step 1.** Add `RECOVER_FF_URL_DIRECTORY = 0x021D` to `generated_src/FF_Recovery.hpp` and its generator `tools/generator/ffd.py`.

**Step 2.** Add `FF_URL_DIRECTORY` to `include/FF_Primitives.hpp` (hand-written):
- vtable: `VALIDATION(8) | RECOVERY(2) | PREFIX_COUNT(2) | ENTRY_COUNT(4)` = 16-byte header
- `uint16_t prefix_count(const BYTE* base) const`
- `uint32_t entry_count(const BYTE* base) const`
- `std::string_view prefix_string(const BYTE* base, uint16_t idx) const`
- `std::string_view leaf_string(const BYTE* base, uint32_t entry_idx) const`
- `std::string get_url(const BYTE* base, uint32_t entry_idx) const`
- Implement all methods in `src/FF_Primitives.cpp`.

**Step 3.** Add stream preamble constants to `include/FF_Primitives.hpp`:
```cpp
constexpr uint64_t FF_STREAM_MAGIC        = 0x46465354524D0001ULL; // "FFSTRM\x00\x01"
constexpr Size     FF_STREAM_PREAMBLE_SIZE = 16;
constexpr Offset   FF_STREAM_PREAMBLE_URL_DIR = 8;
```

---

### Phase 2 — FF_EXTENSION Vtable Change ✅ COMPLETE (in ffc.py; pending regeneration)

**Step 4.** `tools/generator/ffc.py`: `BLOCK_FIELD_OVERRIDES` maps `('Extension', 'url')` → `URL_IDX`, `uint32_t`, 4 bytes. `VIEW_EXTRA_METHODS` injects the `get_url(url_base, dir)` convenience method on `EXTENSIONView`. `INGEST_FIELD_OVERRIDES` maps `('Extension', 'url')` to a snippet that calls `state.url_to_index` lookup (see Phase 3). These changes are durable in the generator; generated files must be regenerated when specs are available.

**Step 5–6.** Regenerate all `generated_src/` files when FHIR specs are available. The `HEADER_R5_SIZE` shrinks from 44 → 40 automatically via the offset accumulator. `VALUE` offset moves from 34 → 30. All `*_internal.hpp` files that reference `FF_EXTENSION::VALUE` update automatically.

---

### Phase 3 — Predigestion + Ingest Integration

**Step 7.** Define `FF_UrlInternState` in `include/FF_Ingestor.hpp`:
```cpp
struct FF_UrlInternState {
    // Populated during predigestion; consumed read-only during main ingest.
    std::unordered_map<std::string, uint32_t> url_to_index;
    // FF_NULL_UINT32 value means: URL is known/filtered — skip the FF_EXTENSION block.
};
```
The predigestion function lives in `src/FF_Ingestor.cpp` (or `src/FF_Builder.cpp`):
```cpp
FF_UrlInternState FF_PredigestExtensionURLs(
    std::string_view json_payload,
    Builder& builder,
    FF_ExtensionFilterMode mode = FILTER_ALL_KNOWN);
```

**Step 8.** Implement `FF_PredigestExtensionURLs`:
1. Write 16-byte stream preamble at arena offset 0: `STORE_U64(base, FF_STREAM_MAGIC)`, `STORE_U64(base + 8, FF_NULL_OFFSET)`. Advance write head by `FF_STREAM_PREAMBLE_SIZE`.
2. (FF_HEADER space is claimed separately by the Builder constructor at offset 16 — now shifted by `FF_STREAM_PREAMBLE_SIZE`.)
3. Use simdjson to scan the JSON. Recursively descend into `extension` and `modifierExtension` arrays. For each array element, read the `"url"` string field only — do not parse the rest of the element.
4. For each URL string:
   - Check `FF_IsKnownExtension(url)` against the compile-time filter. If known and `mode != FILTER_NONE` → insert `{url, FF_NULL_UINT32}` into `url_to_index`; continue.
   - Check `url_to_index` — if already present, skip (deduplication).
   - Split at last `/` → `(prefix, leaf)`. Use an internal `prefix_map` to deduplicate prefix strings; write each unique prefix as an `FF_STRING` to the arena once. Write leaf as a new `FF_STRING`.
   - Insert `{url, next_index}` into `url_to_index`.
5. After all URLs collected: write the `FF_URL_DIRECTORY` block contiguously:
   - Write 16-byte header: `VALIDATION`, `RECOVERY`, `PREFIX_COUNT`, `ENTRY_COUNT`.
   - Write prefix table: `PREFIX_COUNT` × 8-byte `Offset` entries (already written to arena in step 4).
   - Write entry table: `ENTRY_COUNT` × 12-byte `URLEntry` structs (`prefix_idx(2) | pad(2) | leaf_offset(8)`).
6. Back-patch: `STORE_U64(base + FF_STREAM_PREAMBLE_URL_DIR, dir_offset)`.
7. Return `FF_UrlInternState` (only `url_to_index` field needed by callers).

**Step 9.** Thread `const FF_UrlInternState*` into the generated ingest functions. The `INGEST_FIELD_OVERRIDES` entry for `('Extension', 'url')` emits:
```cpp
else if (key == "url") {
    std::string_view url_sv;
    if (field.value().get_string().get(url_sv) == simdjson::SUCCESS) {
        auto it = intern_state ? intern_state->url_to_index.find(std::string(url_sv))
                               : intern_state->url_to_index.end();
        data.url_idx = (it != intern_state->url_to_index.end())
                       ? it->second : FF_NULL_UINT32;
    }
}
```
Where `FF_NULL_UINT32` in `url_idx` signals the caller to skip writing the `FF_EXTENSION` block.

**Step 10.** In `Extension_from_json`, after the key loop, if `data.url_idx == FF_NULL_UINT32`: return a sentinel `ExtensionData` and let the caller discard it (do not append to the builder or the parent's extension array).

**Step 11.** Update the Builder constructor to account for the preamble:
- Fresh streams: claim `FF_STREAM_PREAMBLE_SIZE + FF_HEADER::HEADER_SIZE` bytes upfront (not just `FF_HEADER::HEADER_SIZE`).
- `STORE_FF_HEADER` writes to `base + FF_STREAM_PREAMBLE_SIZE` instead of `base + 0`.
- Adjust all `STORE_FF_HEADER` call sites accordingly.

---

### Phase 4 — Parser Read Path

**Step 12.** Update both `Parser` constructors (`const void*` and `const Memory&`):
1. If `size >= 8` and `LOAD_U64(base) == FF_STREAM_MAGIC`:
   - This is a new-format stream. Read `URL_DIR_OFFSET = LOAD_U64(base + 8)`.
   - Parse `FF_HEADER` at `base + FF_STREAM_PREAMBLE_SIZE` (offset 16).
   - If `URL_DIR_OFFSET != FF_NULL_OFFSET`, construct `FF_URL_DIRECTORY` view and store pointer.
2. Else: legacy stream — parse `FF_HEADER` at `base + 0` as before.

**Step 13.** Add to `Parser`:
```cpp
const FF_URL_DIRECTORY* url_directory() const;  // nullptr if no preamble / not present
```
Store `m_url_dir_offset` (type `Offset`, default `FF_NULL_OFFSET`) as a new private member. `url_directory()` returns `nullptr` when `FF_NULL_OFFSET`, otherwise wraps and returns an `FF_URL_DIRECTORY` view.

---

### Phase 5 — Generator: FF_KnownExtensions

**Step 14.** Update `tools/generator/make_lib.py` to emit `generated_src/FF_KnownExtensions.hpp`:
- Collect all extension URLs from compiled profile StructureDefinition JSONs (native fields) and from the HL7 extension registry JSON.
- Compute `SHA-256(url)` for each. Sort ascending. Emit:
```cpp
constexpr std::array<std::array<uint8_t,32>, N> FF_KNOWN_EXTENSION_HASHES = {{ ... }};
inline bool FF_IsKnownExtension(std::string_view url) {
    // SHA-256 url on stack, std::binary_search
}
```
- Separate arrays / search functions for `FILTER_NATIVE_ONLY` vs `FILTER_ALL_KNOWN` modes.

---

### Phase 6 — WAMR Host Integration

**Step 15.** Integrate WAMR behind `FASTFHIR_ENABLE_EXTENSIONS` CMake flag. Add `wamr` as a conditional `FetchContent` dependency.

**Step 16.** Implement `FF_WasmExtensionHost` in `include/FF_Extensions.hpp` / `src/FF_Extensions.cpp`:
- `std::call_once` engine init.
- Per-module `std::shared_mutex` protecting compilation (`.wasm` → AOT).
- Per-thread WAMR module instance + 64KB staging buffer.

**Step 17.** Batch dispatch function:
```cpp
FF_ExtensionResult dispatch_extension_batch(
    const BYTE* base,
    Offset ext_array_offset,
    uint32_t count,
    uint32_t fhir_version,
    bool is_modifier,
    WasmModuleInstance* inst,
    const FF_URL_DIRECTORY* dir);
```
Steps: for each extension entry, resolve URL via `dir->get_url(base, url_idx)` → copy `FF_EXTENSION` block bytes + URL string bytes into staging buffer → call guest `process_extension_batch(batch_ptr, count, fhir_version, is_modifier)` → map return to `FF_ExtensionResult`.

**Step 18.** WASM entry point compiled into every extension module:
```c
__attribute__((export_name("process_extension_batch")))
uint32_t process_extension_batch(uint32_t batch_ptr, uint32_t count,
                                 uint32_t fhir_version, uint32_t is_modifier);
```
Compile flags: `--target=wasm32-wasi -O3 -fno-exceptions -fno-rtti -nostdlib++`

---

### Phase 7 — Registry Fetching & Verification

**Step 19.** Implement registry fetch in `src/FF_Extensions.cpp`:
- Lookup key: `SHA-256(canonical_extension_url)`.
- Two-level cache: memory (`std::unordered_map<std::array<uint8_t,32>, CompiledModule>` + `std::shared_mutex`) and disk (`~/.cache/fastfhir/modules/{hex_hash}.wasm` + `.aot`).
- Fetch endpoint: `https://registry.fastfhir.org/v1/modules/{hex_hash}.wasm`.
- Verify SHA-256 of `.wasm` bytes against registry manifest signature before loading. Reject on failure.
- **Offline fallback**: log via `FF_Logger`, skip WASM dispatch for that URL, append to `Parser::unresolved_extensions()`. Do NOT block ingestion.

---

### Phase 8 — ffc.py WASM Mode

**Step 20.** Add `--wasm` flag to `ffc.py`. Emit one WASM-compatible translation unit per extension type containing only the View structs (`EXTENSIONView` + any backbone View types). Exclude all heap-allocating types.

**Step 21.** Emit per-resource `dispatch_extensions` / `dispatch_modifier_extensions` stubs. When `FASTFHIR_ENABLE_EXTENSIONS=OFF`, stubs are empty `inline` functions (zero-cost).

---

## Affected Files

| File | Change |
|---|---|
| `include/FF_Primitives.hpp` | `FF_URL_DIRECTORY` struct + stream preamble constants |
| `src/FF_Primitives.cpp` | `FF_URL_DIRECTORY` method implementations |
| `tools/generator/ffd.py` | Add `RECOVER_FF_URL_DIRECTORY` |
| `generated_src/FF_Recovery.hpp` | Add `RECOVER_FF_URL_DIRECTORY` (regenerated) |
| `include/FF_Ingestor.hpp` | `FF_UrlInternState`, `FF_ExtensionFilterMode` |
| `src/FF_Ingestor.cpp` | `FF_PredigestExtensionURLs()` implementation |
| `src/FF_Builder.cpp` | Write preamble + shift `FF_HEADER` to offset 16 for new streams |
| `include/FF_Parser.hpp` | `m_url_dir_offset`, `url_directory()` accessor |
| `src/FF_Parser.cpp` | Read and branch on `FF_STREAM_MAGIC`; legacy-compat fallback |
| `include/FF_Extensions.hpp` / `src/FF_Extensions.cpp` | New — WAMR host, dispatch, registry fetch |
| `tools/generator/ffc.py` | `BLOCK_FIELD_OVERRIDES`, `VIEW_EXTRA_METHODS`, `INGEST_FIELD_OVERRIDES` for Extension |
| `tools/generator/make_lib.py` | Emit `FF_KnownExtensions.hpp` |
| `generated_src/` (all) | Regenerate when FHIR specs available |
| `generated_src/FF_KnownExtensions.hpp` | New — known-extension SHA-256 filter |
| `CMakeLists.txt` | `FASTFHIR_ENABLE_EXTENSIONS` flag, WAMR FetchContent |

---

## Progress

| Phase | Status |
|---|---|
| Phase 1 — Binary Structures | ✅ Complete |
| Phase 2 — FF_EXTENSION vtable (generator) | ✅ Complete in `ffc.py`; generated files pending regeneration |
| Phase 3 — Predigestion + ingest integration | ⏳ Next |
| Phase 4 — Parser read path | ⏳ Next |
| Phase 5 — FF_KnownExtensions generator | 🔲 Pending |
| Phase 6 — WAMR host | 🔲 Pending |
| Phase 7 — Registry fetch | 🔲 Pending |
| Phase 8 — ffc.py --wasm mode | 🔲 Pending |

---

## Verification

1. `cmake --build build --target build_all -j` — clean build with new `FF_EXTENSION` layout.
2. Ingest a Synthea patient record. Confirm `FF_URL_DIRECTORY` block present at preamble-indicated offset; each unique URL stored exactly once.
3. Verify `EXTENSIONView::get_url()` returns correct reconstructed URL for `geolocation` and a Synthea-specific URL.
4. Verify known extensions (`us-core-race`, `us-core-ethnicity`, etc.) produce zero `FF_EXTENSION` blocks in the output.
5. Profile arena bytes for a 50-resource Synthea bundle before/after — expect >10× reduction in extension URL storage.
6. WASM: compile a minimal `EXTENSIONView` recognition module with wasi-sdk and verify `process_extension_batch` returns `0` (ok) for a known URL and `1` (unknown) for an unrecognised one.


**Context:** We are implementing an "Open World" FHIR extension subsystem for the FastFHIR C++ library. FastFHIR uses a lock-free, zero-copy architecture backed by an offset-based memory arena. We are using the WebAssembly Micro Runtime (WAMR). This document also covers the URL deduplication design that is a prerequisite for the WASM work.

---

## Purpose of a WASM Extension Module

A WASM extension module has **one job**: decode the binary layout of a specific FHIR extension type to prove it recognizes the schema.

The module is compiled from the `EXTENSIONView` struct that `ffc.py` generates. It receives a staging buffer containing raw extension bytes, applies the View accessors, and returns whether it recognised the extension.

- **No writes** outside its own linear memory.
- **No host callbacks** for field access — all reads are direct loads from the staging buffer.
- **No heap allocation** — View structs are stack-only wrappers over `const BYTE*`.
- The only host import is `ff_log_warning(uint32_t msg_ptr, uint32_t len)`.
- `get_url()` is pre-resolved by the host before the WASM call — the staging buffer receives the concatenated URL string so the WASM guest never needs the URL directory.

Returns: `0=ok`, `1=unknown`, `2=reject-resource` (modifier not understood), `3=error`.

---

## Ground Truth: Binary Representations

### FF_EXTENSION vtable (40 bytes — updated layout)
```
VALIDATION (8) | RECOVERY (2) | ID (8) | EXTENSION (8) | URL_IDX (4) | VALUE (10)
```
`URL_IDX` is a `uint32_t` index into the stream-level `FF_URL_DIRECTORY`. This replaces the old 8-byte `Offset → FF_STRING` field, saving 4 bytes per extension entry.

### ChoiceEntry (10 bytes inline)
```
raw bits (8) | RECOVERY_TAG (2)
```
`get_value()` → `Decode::choice(base, offset + FF_EXTENSION::VALUE)`

### FF_URL_DIRECTORY block
```
VALIDATION (8) | RECOVERY (2) | PREFIX_COUNT (2) | ENTRY_COUNT (4)     ← 16-byte header
PREFIX_TABLE: PREFIX_COUNT × 8-byte Offsets → FF_STRING blocks          ← base URL paths
ENTRY_TABLE: ENTRY_COUNT × 12-byte URLEntry inline structs
  URLEntry: prefix_idx (2) | pad (2) | leaf_offset (8) → FF_STRING
```
`get_url(uint32_t idx)` → prefix `string_view` + "/" + leaf `string_view` concatenated into `std::string`.

URL split rule: at the last `/`. Edge case (no `/`): empty prefix, full URL stored as leaf.

### Stream Preamble (16 bytes at byte 0 of every FastFHIR binary stream)
```
STREAM_MAGIC (8) — fixed constant: format identifier + version
URL_DIR_OFFSET (8) — Offset to FF_URL_DIRECTORY; FF_NULL_OFFSET until back-patched at stream close
```
Not a vtable struct — fixed-position header the Parser reads before anything else.

---

## Architectural Constraint: Batch Buffer Model

**Do NOT** alias the FastFHIR arena into WAMR guest linear memory. WAMR does not support remapping its own allocation.

**Do NOT** pass 64-bit `Offset` values as WASM parameters. `Offset` is `uint64_t`; standard WASM is 32-bit.

**The correct model:**
1. At module instantiation, allocate a staging buffer inside WAMR guest linear memory (`wasm_module_inst_malloc` or a reserved region). Store the `uint32_t` guest-side address.
2. Before each batch dispatch, the host: (a) resolves the URL string from the directory, (b) copies extension block bytes + resolved URL string into the staging buffer — one `memcpy` each.
3. The guest receives a `uint32_t` linear-memory pointer. `LOAD_U64(ptr)` compiles to `i64.load`.

Write head safety is automatic: only extension block bytes enter the staging buffer.

---

## Known-Extension Filter

Before writing any `FF_EXTENSION` block to the arena, the ingestor checks the URL against a compile-time filter. Three categories of URLs are suppressed:

1. **Profile-native** — extensions already captured as native fields by the compiled US Core / UK CORE / base profile (e.g. `us-core-race`, `us-core-ethnicity`, `us-core-birthsex`). The data is already stored in the resource's own vtable.
2. **HL7-known safe** — extensions in the HL7 base spec known to be informational and not modifier-semantics (e.g. `patient-mothersMaidenName`, `geolocation`).

Both categories are compiled into `FF_KnownExtensions.hpp` as a `constexpr` sorted array of `SHA-256(url)` hashes. Lookup is `O(log n)` binary search, performed once per extension element at ingest time. Match → skip the `FF_EXTENSION` write entirely. No WASM dispatch occurs for known extensions.

`FF_ParserOptions` exposes:
- `FILTER_ALL_KNOWN` (default) — suppress both categories
- `FILTER_NATIVE_ONLY` — suppress only profile-native (category 1)
- `FILTER_NONE` — store all extensions, dispatch everything to WASM

---

## Implementation Phases

### Phase 1 — New Binary Structures (Prerequisites)

**Step 1.** Add `RECOVER_FF_URL_DIRECTORY` to `FF_Recovery.hpp` (or its `ffd.py` generator). Tag value: next available in the `0x02xx` data-types range.

**Step 2.** Add `FF_URL_DIRECTORY` to `include/FF_Primitives.hpp` — hand-written, not generated. Provide:
- `uint16_t prefix_count(const BYTE* base) const`
- `uint32_t entry_count(const BYTE* base) const`
- `std::string_view prefix_string(const BYTE* base, uint16_t idx) const`
- `std::string_view leaf_string(const BYTE* base, uint32_t entry_idx) const`
- `std::string get_url(const BYTE* base, uint32_t entry_idx) const` — returns prefix + "/" + leaf (or just leaf if prefix_idx == 0xFFFF sentinel for "no prefix")

**Step 3.** Add stream preamble constants to `include/FF_Primitives.hpp`:
```cpp
constexpr uint64_t FF_STREAM_MAGIC = 0x46465354524D0001ULL; // "FFSTRM\x00\x01"
constexpr Size     FF_STREAM_PREAMBLE_SIZE = 16;
constexpr Offset   FF_STREAM_PREAMBLE_URL_DIR = 8;  // offset of URL_DIR_OFFSET field
```

---

### Phase 2 — FF_EXTENSION Vtable Change

**Step 4.** Update `tools/generator/ffc.py`: change the `URL` field from `FF_FIELD_STRING` (8 bytes) to `FF_FIELD_UINT32` (4 bytes) in the `FF_EXTENSION` field list. Field name: `URL_IDX`. The offset accumulator automatically cascades the 4-byte saving to `VALUE` (moves from 34 → 30) and total struct size (44 → 40).

**Step 5.** Regenerate `generated_src/FF_DataTypes.hpp` and `generated_src/FF_DataTypes_internal.hpp`. The `EXTENSIONView::get_url()` signature changes to:
```cpp
std::string get_url(const BYTE* base, const FF_URL_DIRECTORY& dir) const;
```
Implementation: `dir.get_url(base, LOAD_U32(base + __offset + FF_EXTENSION::URL_IDX))`.

**Step 6.** Regenerate all `generated_src/*_internal.hpp` files where `EXTENSIONView::get_value()` uses `FF_EXTENSION::VALUE`. The constant moves from 34 → 30; regeneration is sufficient.

---

### Phase 3 — Ingestor Integration

**Step 7.** Add URL intern state to ingest context (`include/FF_Ingestor.hpp` / `src/FF_Ingestor.cpp`):
```cpp
struct FF_UrlInternState {
    std::unordered_map<std::string, uint16_t> prefix_map;   // prefix → prefix table index
    std::unordered_map<std::string, uint32_t> url_map;      // full URL → entry index
    std::vector<Offset>                        prefix_offsets; // arena Offsets of prefix FF_STRINGs
    std::vector<std::pair<uint16_t, Offset>>   url_entries;   // (prefix_idx, leaf Offset)
};
```
The maps live only during ingest. The vectors are flushed to the `FF_URL_DIRECTORY` block at stream close.

**Step 8.** Add `intern_url(std::string_view url, FF_UrlInternState& state, FF_Builder& builder) -> uint32_t`:
1. Check `state.url_map` — if found, return cached index immediately.
2. Split `url` at last `/` into `(prefix, leaf)`.
3. Intern prefix: check `state.prefix_map`; if new, write an `FF_STRING` to the arena, append its Offset to `state.prefix_offsets`, assign next index.
4. Write leaf as new `FF_STRING` to the arena.
5. Append `(prefix_idx, leaf_offset)` to `state.url_entries`, store in `state.url_map`, return new index.

**Step 9.** Update extension ingestion in `FF_IngestMappings.cpp` (or its generator). For each `extension` / `modifierExtension` JSON array element, before writing the `FF_EXTENSION` block:
1. Read the `url` JSON field.
2. Check against the compile-time known-extension filter (`FF_KnownExtensions.hpp`). If known → skip element entirely; do not write `FF_EXTENSION`.
3. If unknown: call `intern_url()` → get `uint32_t` index.
4. Store the 4-byte index at `FF_EXTENSION::URL_IDX` in the block.

**Step 10.** Write stream preamble at the start of every ingest session (`src/FF_Ingestor.cpp` or `FF_Builder`):
- Write `FF_STREAM_MAGIC` at offset 0.
- Write `FF_NULL_OFFSET` at offset 8 (URL_DIR placeholder).
- Advance write head by 16.

**Step 11.** Back-patch the URL directory at stream close:
1. Write the `FF_URL_DIRECTORY` block to the arena: header, prefix Offset table, then `ENTRY_COUNT` × 12-byte URLEntry structs.
2. Write the directory's arena Offset into the stream preamble at `FF_STREAM_PREAMBLE_URL_DIR`.
3. Clear the intern maps.

---

### Phase 4 — Parser Read Path

**Step 12.** Update `FF_Parser` to read the stream preamble on open:
1. Read bytes 0–7; compare to `FF_STREAM_MAGIC`. If mismatch, attempt legacy parse (backward compat).
2. Read bytes 8–15: `URL_DIR_OFFSET`. If not `FF_NULL_OFFSET`, wrap `FF_URL_DIRECTORY` view around that Offset.
3. Expose `const FF_URL_DIRECTORY* url_directory() const` on the Parser.

---

### Phase 5 — Generator: FF_KnownExtensions

**Step 13.** Update `tools/generator/make_lib.py` to emit `generated_src/FF_KnownExtensions.hpp`:
- Collect all extension URLs from compiled profile StructureDefinition JSONs (native fields) and from the HL7 extension registry JSON.
- Compute `SHA-256(url)` for each.
- Emit a `constexpr std::array<std::array<uint8_t,32>, N>` sorted ascending.
- Emit `inline bool FF_IsKnownExtension(std::string_view url)` — computes SHA-256 on the stack, `std::binary_search`.

---

### Phase 6 — WAMR Host Integration

**Step 14.** Integrate WAMR behind `FASTFHIR_ENABLE_EXTENSIONS` CMake flag. Add `wamr` as a conditional `FetchContent` dependency.

**Step 15.** Implement `FF_WasmExtensionHost` in `include/FF_Extensions.hpp` / `src/FF_Extensions.cpp`:
- `std::call_once` engine init.
- Per-module `std::shared_mutex` protecting compilation (`.wasm` → AOT).
- Per-thread WAMR module instance + 64KB staging buffer.

**Step 16.** Batch dispatch function:
```cpp
FF_ExtensionResult dispatch_extension_batch(
    const BYTE* base,
    Offset ext_array_offset,
    uint32_t count,
    uint32_t fhir_version,
    bool is_modifier,
    WasmModuleInstance* inst);
```
Steps: resolve URL via directory → copy `FF_EXTENSION` block bytes + URL string bytes into staging buffer → call guest `process_extension_batch(batch_ptr, count, fhir_version, is_modifier)` → map return to `FF_ExtensionResult`.

**Step 17.** WASM entry point compiled into every extension module:
```c
__attribute__((export_name("process_extension_batch")))
uint32_t process_extension_batch(uint32_t batch_ptr, uint32_t count,
                                 uint32_t fhir_version, uint32_t is_modifier);
```
Compile flags: `--target=wasm32-wasi -O3 -fno-exceptions -fno-rtti -nostdlib++`

---

### Phase 7 — Registry Fetching & Verification

**Step 18.** Implement registry fetch in `src/FF_Extensions.cpp`:
- Lookup key: `SHA-256(canonical_extension_url)`.
- Two-level cache: memory (`std::unordered_map<std::array<uint8_t,32>, CompiledModule>` + `std::shared_mutex`) and disk (`~/.cache/fastfhir/modules/{hex_hash}.wasm` + `.aot`).
- Fetch endpoint: `https://registry.fastfhir.org/v1/modules/{hex_hash}.wasm`.
- Verify SHA-256 of `.wasm` bytes against registry manifest signature before loading. Reject on failure.
- **Offline fallback**: log via `FF_Logger`, skip WASM dispatch for that URL, append to `Parser::unresolved_extensions()`. Do NOT block ingestion.

---

### Phase 8 — ffc.py WASM Mode

**Step 19.** Add `--wasm` flag to `ffc.py`. Emit one WASM-compatible translation unit per extension type containing only the View structs (`EXTENSIONView` + any backbone View types). Exclude all heap-allocating types.

**Step 20.** Emit per-resource `dispatch_extensions` / `dispatch_modifier_extensions` stubs. When `FASTFHIR_ENABLE_EXTENSIONS=OFF`, stubs are empty `inline` functions (zero-cost).

---

## Affected Files

| File | Change |
|---|---|
| `include/FF_Primitives.hpp` | Add `FF_URL_DIRECTORY`, stream preamble constants, `intern_url()` |
| `include/FF_Recovery.hpp` (or `ffd.py`) | Add `RECOVER_FF_URL_DIRECTORY` |
| `include/FF_Ingestor.hpp` | Add `FF_UrlInternState` to ingest context |
| `src/FF_Ingestor.cpp` | Write preamble, intern URLs, back-patch directory at close |
| `include/FF_Parser.hpp` / `src/FF_Parser.cpp` | Read preamble, expose `url_directory()` |
| `include/FF_Extensions.hpp` / `src/FF_Extensions.cpp` | New — WAMR host, dispatch, registry fetch |
| `tools/generator/ffc.py` | `FF_EXTENSION` URL_IDX offset, `get_url()` codegen, `--wasm` mode |
| `tools/generator/make_lib.py` | Emit `FF_KnownExtensions.hpp` |
| `generated_src/FF_DataTypes.hpp` / `FF_DataTypes_internal.hpp` | Regenerate with new layout |
| All `generated_src/*_internal.hpp` | Regenerate (FF_EXTENSION::VALUE offset 34 → 30) |
| `generated_src/FF_IngestMappings.cpp` | Extension ingest: known filter + intern_url |
| `generated_src/FF_KnownExtensions.hpp` | New — generated known-extension SHA-256 set |
| `CMakeLists.txt` | `FASTFHIR_ENABLE_EXTENSIONS` flag, WAMR FetchContent |

---

## Verification

1. `cmake --build build --target build_all -j` — clean build with new `FF_EXTENSION` layout.
2. Ingest the Synthea patient record; dump arena: confirm `FF_URL_DIRECTORY` block present, each unique URL stored exactly once as an `FF_STRING`.
3. Verify `EXTENSIONView::get_url()` returns correct reconstructed URL for `geolocation` and a Synthea-specific URL.
4. Verify known extensions (`us-core-race`, `us-core-ethnicity`, etc.) produce zero `FF_EXTENSION` blocks in the output.
5. Profile arena bytes for a 50-resource Synthea bundle before/after — expect >10× reduction in extension URL storage.
6. WASM: compile a minimal `EXTENSIONView` recognition module with wasi-sdk and verify `process_extension_batch` returns `0` (ok) for a known URL and `1` (unknown) for an unrecognised one.

---

## Implementation Progress — Session Log (2026-04-28)

### URGENT TODO: Header Redesign — ✅ COMPLETE

Both architectural corrections from the URGENT TODO section have been implemented. The project builds cleanly (`ff_export` + `libfastfhir.dylib`, zero warnings or errors).

---

### Change 1 — `FF_HEADER` Vtable Expanded (38 → 54 bytes)

**What changed:** The 16-byte stream preamble (`FF_STREAM_MAGIC | URL_DIR_OFFSET`) has been eliminated. `URL_DIR_OFFSET` and `MODULE_REG_OFFSET` are now proper vtable fields inside `FF_HEADER`, following the same back-patch pattern already used by `CHECKSUM_OFFSET`. Constants `FF_STREAM_MAGIC`, `FF_STREAM_PREAMBLE_SIZE`, and `FF_STREAM_PREAMBLE_URL_DIR` have been removed from `FF_Primitives.hpp`.

**Binary layout — `FF_HEADER` at arena offset 0:**
```
Byte  0– 3 : MAGIC             (4) — format stamp
Byte  4– 5 : RECOVERY          (2) — RECOVER_FF_HEADER
Byte  6– 7 : FHIR_REV          (2) — R4/R5
Byte  8–15 : STREAM_SIZE       (8) — total committed bytes
Byte 16–23 : ROOT_OFFSET       (8) — root resource block offset
Byte 24–25 : ROOT_RECOVERY     (2) — root resource recovery tag
Byte 26–33 : CHECKSUM_OFFSET   (8) — FF_CHECKSUM block offset
Byte 34–41 : URL_DIR_OFFSET    (8) — FF_URL_DIRECTORY offset; FF_NULL_OFFSET if absent  ← NEW
Byte 42–49 : MODULE_REG_OFFSET (8) — FF_MODULE_REGISTRY offset; FF_NULL_OFFSET if absent ← NEW
Byte 50–53 : VERSION           (4) — engine version + stream layout flag
```

**Files changed:**

| File | Lines | Description |
|---|---|---|
| [include/FF_Primitives.hpp](include/FF_Primitives.hpp#L358-L434) | 358–434 | `FF_HEADER` struct: added `URL_DIR_OFFSET_S`, `MODULE_REG_OFFSET_S` to `vtable_sizes`; added `URL_DIR_OFFSET` (byte 34), `MODULE_REG_OFFSET` (byte 42) to `vtable_offsets`; `HEADER_SIZE` = 54. New accessors `get_url_dir_offset()`, `get_module_reg_offset()` declared. `STORE_FF_HEADER` gains two defaulted `Offset` parameters. Block comment above struct explains full layout and rationale. |
| [src/FF_Primitives.cpp](src/FF_Primitives.cpp#L105-L106) | 105–106 | New accessor definitions: `get_url_dir_offset()`, `get_module_reg_offset()` — `LOAD_U64` at the new vtable offsets. |
| [src/FF_Primitives.cpp](src/FF_Primitives.cpp#L120-L143) | 120–143 | `STORE_FF_HEADER` implementation: two new `STORE_U64` calls for `URL_DIR_OFFSET` and `MODULE_REG_OFFSET`. |

---

### Change 2 — `Memory::STREAM_HEADER_SIZE` Made Derived

**What changed:** Was hardcoded to `38`; is now `= FF_HEADER::HEADER_SIZE` so it automatically tracks any future header growth. The existing `static_assert` in `FF_Memory.cpp` caught the mismatch during the first build and confirmed the new value is 54.

| File | Line | Description |
|---|---|---|
| [include/FF_Memory.hpp](include/FF_Memory.hpp#L31) | 31 | `STREAM_HEADER_SIZE = FF_HEADER::HEADER_SIZE` (derived constant, was hardcoded `38`) |

---

### Change 3 — Builder: Preamble Removed; URL Dir Tracking Added

**What changed:** Builder constructor no longer writes a 16-byte preamble. It claims exactly `FF_HEADER::HEADER_SIZE` bytes. `m_url_dir_offset` and `m_module_reg_offset` are new private members that flow into `STORE_FF_HEADER` at finalize time. `set_url_dir_offset()` is exposed so `FF_PredigestExtensionURLs` can record the URL directory's arena offset before finalize.

| File | Lines | Description |
|---|---|---|
| [include/FF_Builder.hpp](include/FF_Builder.hpp#L43-L44) | 43–44 | New private members `m_url_dir_offset`, `m_module_reg_offset` (both default `FF_NULL_OFFSET`) |
| [include/FF_Builder.hpp](include/FF_Builder.hpp#L71-L72) | 71–72 | New public API: `set_url_dir_offset(Offset)`, `url_dir_offset() const` |
| [src/FF_Builder.cpp](src/FF_Builder.cpp#L76-L84) | 76–84 | Constructor: `claim_space(FF_HEADER::HEADER_SIZE)` only — preamble writes removed |
| [src/FF_Builder.cpp](src/FF_Builder.cpp#L258-L271) | 258–271 | `finalize()`: `STORE_FF_HEADER(m_base, ..., m_url_dir_offset, m_module_reg_offset)` |

---

### Change 4 — Ingestor: Back-Patch Target Updated

**What changed:** `FF_PredigestExtensionURLs` previously wrote the directory offset to the preamble byte at offset 8. It now writes directly to `FF_HEADER::URL_DIR_OFFSET` (byte 34 of the arena) and also calls `builder.set_url_dir_offset()` so finalize re-stamps it via `STORE_FF_HEADER`.

| File | Lines | Description |
|---|---|---|
| [src/FF_Ingestor.cpp](src/FF_Ingestor.cpp#L235-L241) | 235–241 | Phase 6 back-patch: `STORE_U64(base + FF_HEADER::URL_DIR_OFFSET, dir_off)` + `builder.set_url_dir_offset(dir_off)` |

---

### Change 5 — Compactor: New Parameters Forwarded

**What changed:** `STORE_FF_HEADER` in the compactor receives `FF_NULL_OFFSET` for both new offset fields. Compact archives do not carry extension data. Inline comments document this.

| File | Lines | Description |
|---|---|---|
| [src/FF_Compactor.cpp](src/FF_Compactor.cpp#L393-L402) | 393–402 | `FF_NULL_OFFSET` for `url_dir_offset` and `module_reg_offset` with explanatory comments |

---

### Change 6 — Parser: Magic-Byte Detection Branch Removed

**What changed:** Both `Parser` constructors previously branched on `LOAD_U64(m_base) == FF_STREAM_MAGIC` to detect the preamble format and shift the header base offset. Now `FF_HEADER` is unconditionally at offset 0. `m_header_base_offset` member deleted. `m_url_dir_offset` and `m_module_reg_offset` are read directly from the header fields after `validate_full`.

New accessor additions:
- `has_module_registry()` — predicate for Phase 7 (symmetric with `has_url_directory()`)
- `module_registry_offset()` — raw offset for Phase 7 reader

| File | Lines | Description |
|---|---|---|
| [include/FF_Parser.hpp](include/FF_Parser.hpp#L60-L61) | 60–61 | `m_url_dir_offset`, `m_module_reg_offset` members (replaced `m_header_base_offset`) |
| [include/FF_Parser.hpp](include/FF_Parser.hpp#L109-L125) | 109–125 | `has_url_directory()`, `has_module_registry()`, `module_registry_offset()`, `url_directory()` |
| [src/FF_Parser.cpp](src/FF_Parser.cpp#L425-L460) | 425–460 | Both constructors: single unconditional read path; comment block above explains the rationale |

---

### Binary Compatibility Note

This is a **hard binary format break**. Streams written before this session (with the old 16-byte preamble) are not parseable by the new code, and vice versa. This was intentional — the preamble format was unreleased. All on-disk FastFHIR test fixtures or development streams must be re-ingested from source JSON.

---

### What Remains (Next Sessions)

| Phase | Status |
|---|---|
| URGENT TODO — Header Redesign | ✅ Complete |
| Phase 1 — Binary Structures (`FF_URL_DIRECTORY` etc.) | ✅ Complete |
| Phase 2 — `FF_EXTENSION` vtable change (`ffc.py`) | ✅ Complete in generator; generated files pending regeneration |
| Phase 3 — Predigestion + ingest integration | ✅ Complete (`FF_PredigestExtensionURLs`) |
| Phase 4 — Parser read path | ✅ Complete |
| Phase 5 — `FF_KnownExtensions` generator (`make_lib.py`) | 🔲 Pending |
| Phase 6 — WAMR host integration | 🔲 Pending |
| Phase 7 — Registry fetch + `FF_MODULE_REGISTRY` block | 🔲 Pending |
| Phase 8 — `ffc.py --wasm` mode | 🔲 Pending |
