# FastFHIR Extension Subsystem: Implementation Plan

**Context:** We are implementing an "Open World" FHIR extension subsystem for the FastFHIR C++ library. FastFHIR uses a lock-free, zero-copy architecture backed by an offset-based memory arena. We are using the WebAssembly Micro Runtime (WAMR). This document also covers the URL deduplication design that is a prerequisite for the WASM work.

---

## Purpose of a WASM Extension Module

**A WASM extension module is a drop-in vtable codec for one specific FHIR extension URL.** It is not a recognition probe — it does the same job for an unknown extension type that the generated C++ routines (`SIZE_FF_PROCEDURE`, `STORE_FF_PROCEDURE`, `FF_DESERIALIZE_PROCEDURE`, `PROCEDUREView<VERSION>::get_*`) do for built-in resource types. The host (WAMR) treats the module as a function pointer triple keyed by extension URL hash and dispatches to it from the exact same call sites that today dispatch to compiled C++ codecs.

Each module exports three pure functions — all matching the shapes already produced by `ffc.py` for native types:

| Generated C++ for `FF_PROCEDURE` | WASM export for an unknown extension |
|---|---|
| `Size SIZE_FF_PROCEDURE(const ProcedureData&, uint32_t version)` | `ext_size(uint32_t staging_ptr, uint32_t version) -> uint32_t` |
| `Offset STORE_FF_PROCEDURE(BYTE*, Offset, const ProcedureData&, uint32_t)` | `ext_encode(uint32_t staging_ptr, uint32_t version) -> uint32_t` (returns bytes written) |
| `ProcedureData FF_DESERIALIZE_PROCEDURE(const BYTE*, Offset, Size, uint32_t)` | `ext_decode(uint32_t staging_ptr, uint32_t version) -> uint32_t` (writes typed payload back to staging) |
| `PROCEDUREView<VERSION>::get_field()` accessors | not needed — host reads decoded staging payload directly |

What the module does **not** do:
- It does not return `ok / unknown / error` codes for "did I recognise this extension". URL→module binding is established at parse time via the registry; if the host calls `ext_decode`, the URL was already matched.
- It does not import `ff_log_warning` or any other host callback for field access. Linear-memory loads/stores only.
- It does not use the WIT / Component Model / `borrow<>` / `resource` types, and it is not a Wasmtime component. The ABI is plain WAMR core-WASM exports with `i32` / `i64` parameters.
- It does not allocate on the host heap. The host pre-allocates a per-thread 64 KiB staging region inside the guest's linear memory; the module reads/writes only inside that region.
- It does not see the `FF_URL_DIRECTORY`, the FastFHIR arena, or any `Offset` value > 4 GiB. The module is a self-contained codec for one URL's vtable layout.

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

---

## Architectural Constraint: WAMR Core-WASM ABI

This subsystem is **WAMR-only** and uses **core WebAssembly** exports. It deliberately rejects Wasmtime, the WebAssembly Component Model, WIT-generated bindings, and `borrow<extension-block>`-style resource types. Those abstractions impose runtime-driven memory ownership and indirect call dispatch that defeat the zero-copy, lock-free guarantees of FastFHIR. Every guest export takes only `i32` / `i64` scalars; every byte read/written by the guest goes through plain `i32.load` / `i64.store` against its own linear memory.

**Do NOT** alias the FastFHIR arena into WAMR guest linear memory. WAMR does not support remapping its own allocation, and the arena's 64-bit offsets do not fit a 32-bit WASM address space anyway.

**Do NOT** pass 64-bit `Offset` values as WASM parameters. `Offset` is `uint64_t`; the guest sees `uint32_t` staging-buffer-relative pointers. The host is responsible for the arena ↔ staging copy.

**The correct model — staging ping-pong:**
1. At module instantiation, allocate one 64 KiB staging buffer inside the guest's linear memory (`wasm_module_inst_malloc` or a reserved region). Record the `uint32_t` guest-relative address; reuse for every call to that module.
2. **Encode path (ingest):** host writes the source `ExtensionData`-shaped payload into the staging buffer → calls `ext_size` / `ext_encode` → reads the encoded vtable bytes back out of the staging buffer → blits them into the FastFHIR arena at the chosen `Offset`. Mirror of `SIZE_FF_PROCEDURE` / `STORE_FF_PROCEDURE`.
3. **Decode path (parser):** host copies the raw vtable bytes from the FastFHIR arena into the staging buffer → calls `ext_decode` → reads the typed payload back out of the staging buffer. Mirror of `FF_DESERIALIZE_PROCEDURE`.
4. The guest never sees an arena pointer; the host never sees a guest pointer. The boundary is the staging buffer, exchanged as a single `i32`.

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
│  1. Claim FF_HEADER space (54 bytes) at arena offset 0  │
│  2. Fast-scan raw JSON for all extension URL strings    │
│  3. Filter known URLs (SHA-256 binary search)           │
│  4. Deduplicate + prefix-compress surviving URLs        │
│  5. Write FF_STRING blocks (prefixes, then leaves)      │
│  6. Write FF_URL_DIRECTORY header + tables              │
│  7. Back-patch FF_HEADER::URL_DIR_OFFSET                │
│  8. Return immutable FF_UrlInternState (url_to_index)   │
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

**Step 17.** Codec dispatch — three thin host-side wrappers, one per generated-C++ counterpart. Each is called from the same site that today calls the generated `SIZE_FF_*` / `STORE_FF_*` / `FF_DESERIALIZE_*` for native types:

```cpp
// Mirror of SIZE_FF_<TYPE>: returns bytes the encoded vtable will occupy.
Size FF_WasmExtensionSize(WasmModuleInstance* inst,
                          const ExtensionData& src,
                          uint32_t version);

// Mirror of STORE_FF_<TYPE>: encodes src into arena at hdr_off, returns next free Offset.
Offset FF_WasmExtensionStore(WasmModuleInstance* inst,
                             BYTE* base, Offset hdr_off,
                             const ExtensionData& src,
                             uint32_t version);

// Mirror of FF_DESERIALIZE_<TYPE>: decodes a vtable block back into ExtensionData.
ExtensionData FF_WasmExtensionDeserialize(WasmModuleInstance* inst,
                                          const BYTE* base, Offset off, Size size,
                                          uint32_t version);
```
Each wrapper: (a) serialises its non-WASM input into the staging buffer using the same on-the-wire layout the WASM module expects, (b) invokes the matching guest export, (c) reads results back out of the staging buffer.

**Step 18.** WASM entry points — every extension module exports exactly three core-WASM functions matching the host wrappers above:
```c
// Source payload is laid out at staging_ptr by the host before the call;
// guest reads it, computes byte-size of the encoded vtable, returns it.
__attribute__((export_name("ext_size")))
uint32_t ext_size(uint32_t staging_ptr, uint32_t version);

// Source payload at staging_ptr; guest writes encoded vtable bytes back into
// staging_ptr (offset 0, length matching prior ext_size), returns bytes written.
__attribute__((export_name("ext_encode")))
uint32_t ext_encode(uint32_t staging_ptr, uint32_t version);

// Encoded vtable bytes at staging_ptr; guest decodes and writes the typed
// payload back into staging_ptr, returns bytes consumed.
__attribute__((export_name("ext_decode")))
uint32_t ext_decode(uint32_t staging_ptr, uint32_t version);
```
Compile flags: `--target=wasm32-wasi -O3 -fno-exceptions -fno-rtti -nostdlib++ -Wl,--no-entry`. No host imports required; no WASI imports beyond the linker stub.

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

**Step 20.** Add `--wasm` flag to `ffc.py`. For each extension type's `StructureDefinition`, emit one self-contained WASM translation unit containing the codec triple — exactly the same `SIZE_FF_*` / `STORE_FF_*` / `FF_DESERIALIZE_*` shapes the generator already produces for native resources, but renamed to the three exports `ext_size` / `ext_encode` / `ext_decode` and rewired to read/write the staging buffer instead of the FastFHIR arena. Heap-allocating types (`std::vector`, `std::unique_ptr`, `std::string`) must be replaced with stack-only equivalents whose byte layout matches the host's `ExtensionData` over the staging boundary; `ffc.py` already knows every field type so the rewrite is mechanical.

**Step 21.** Emit per-resource `dispatch_extensions` / `dispatch_modifier_extensions` integration helpers that route to `FF_WasmExtensionSize` / `FF_WasmExtensionStore` / `FF_WasmExtensionDeserialize` based on the extension URL hash. When `FASTFHIR_ENABLE_EXTENSIONS=OFF`, the helpers are empty `inline` functions and unknown extensions remain untouched as raw `FF_EXTENSION` blocks (zero-cost).

---

## Affected Files

| File | Change |
|---|---|
| `tools/generator/ffc.py` | Regenerate `FF_EXTENSION` with URL_IDX layout (pending FHIR specs) |
| `generated_src/` (all) | Full regeneration when FHIR specs available |
| `tools/generator/make_lib.py` | Emit `FF_KnownExtensions.hpp` (Phase 5) |
| `generated_src/FF_KnownExtensions.hpp` | New — generated known-extension SHA-256 set (Phase 5) |
| `include/FF_Extensions.hpp` / `src/FF_Extensions.cpp` | New — WAMR host, dispatch, registry fetch (Phase 6) |
| `CMakeLists.txt` | `FASTFHIR_ENABLE_EXTENSIONS` flag, WAMR `FetchContent` (Phase 6) |

---

## Progress

| Phase | Status |
|---|---|
| Phase 1 — Binary Structures | ✅ Complete |
| Phase 2 — FF_EXTENSION vtable (generator) | ✅ Complete in `ffc.py`; generated files pending regeneration |
| Phase 3 — Predigestion + ingest integration | ✅ Complete |
| Phase 4 — Parser read path | ✅ Complete |
| Phase 5 — FF_KnownExtensions generator | 🔲 Pending |
| Phase 6 — WAMR host | 🔲 Pending |
| Phase 7 — Registry fetch | 🔲 Pending |
| Phase 8 — ffc.py --wasm mode | 🔲 Pending |

---

## Verification

1. `cmake --build build --target build_all -j` — clean build with new `FF_EXTENSION` layout.
2. Ingest the Synthea patient record; dump arena: confirm `FF_URL_DIRECTORY` block present, each unique URL stored exactly once as an `FF_STRING`.
3. Verify `EXTENSIONView::get_url()` returns correct reconstructed URL for `geolocation` and a Synthea-specific URL.
4. Verify known extensions (`us-core-race`, `us-core-ethnicity`, etc.) produce zero `FF_EXTENSION` blocks in the output.
5. Profile arena bytes for a 50-resource Synthea bundle before/after — expect >10× reduction in extension URL storage.
6. WASM codec round-trip: compile a minimal codec module for `geolocation` with wasi-sdk; ingest a Patient with a `geolocation` extension; verify `ext_size` matches the host's expected size, `ext_encode` produces bytes byte-identical to the C++-only generated path on a native type of equivalent shape, and `ext_decode` round-trips back to the original `ExtensionData`.
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
