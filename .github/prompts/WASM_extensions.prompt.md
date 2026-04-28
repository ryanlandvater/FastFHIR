# FastFHIR Extension Subsystem: Implementation Plan

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
