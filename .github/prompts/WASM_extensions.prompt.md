# FastFHIR Extension Subsystem: Implementation Plan

**Context:** We are implementing an "Open World" FHIR extension subsystem for the FastFHIR C++ library. FastFHIR uses a lock-free, zero-copy architecture backed by an offset-based memory arena. We are using the WebAssembly Micro Runtime (WAMR). This document also covers the URL deduplication design that is a prerequisite for the WASM work.

---

## Purpose of a WASM Extension Module

**A WASM extension module is a drop-in vtable codec for one specific FHIR extension URL.** It is not a recognition probe — it does the same job for an unknown extension type that the generated C++ routines (`SIZE_FF_PROCEDURE`, `STORE_FF_PROCEDURE`, `FF_DESERIALIZE_PROCEDURE`, `PROCEDUREView<VERSION>::get_*`) do for built-in resource types. The host (WAMR) treats the module as a function pointer triple keyed by extension URL and dispatches to it from the exact same call sites that today dispatch to compiled C++ codecs.

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
VALIDATION (8) | RECOVERY (2) | ID (8) | EXTENSION (8) | EXT_REF (4) | VALUE (10)
```
`EXT_REF` is a 4-byte **discriminated-union routing word**. Bit 31 (the MSB) is
the routing flag; the lower 31 bits are the index payload:

| MSB | Meaning                | Lower-31 bits         | Lookup target           | Codec path                |
|-----|------------------------|-----------------------|-------------------------|---------------------------|
| `0` | **Passive** (Path B)   | `URL_IDX`             | `FF_URL_DIRECTORY`      | Opaque raw-JSON blob      |
| `1` | **Active**  (Path A)   | `MODULE_IDX`          | `FF_MODULE_REGISTRY`    | WASM `ext_*` triplet      |

Helper constants and inlines in `FF_Primitives.hpp`:
```cpp
constexpr uint32_t FF_EXT_REF_MSB         = 0x80000000u;
constexpr uint32_t FF_EXT_REF_INDEX_MASK  = 0x7FFFFFFFu;
constexpr uint32_t FF_EXT_REF_NULL        = FF_NULL_UINT32; // unresolved sentinel

inline bool     ff_ext_ref_is_module(uint32_t r) { return (r & FF_EXT_REF_MSB) != 0; }
inline uint32_t ff_ext_ref_index    (uint32_t r) { return  r & FF_EXT_REF_INDEX_MASK; }
inline uint32_t ff_make_module_ref  (uint32_t i) { return  i | FF_EXT_REF_MSB; }
inline uint32_t ff_make_url_ref     (uint32_t i) { return  i & FF_EXT_REF_INDEX_MASK; }
```

This replaces the old 8-byte `Offset → FF_STRING` field, saving 4 bytes per
extension entry, and folds the URL-vs-module routing decision into a single
load with no branch on a separate flag byte.

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

### FF_MODULE_REGISTRY block (56 bytes per entry)
```
VALIDATION (8) | RECOVERY (2) | ENTRY_COUNT (4) | PAD (2)              ← 16-byte header
ENTRY_TABLE: ENTRY_COUNT × 56-byte entries
  Entry: URL_IDX(4) | PAD(4) | WASM_BLOB_OFFSET(8) | WASM_BLOB_SIZE(4) | PAD2(4) | MODULE_HASH(32)
```
`MODULE_HASH` is the **binary hash** (Role 2, see Hashing section): SHA-256 of the raw `.wasm` bytes.
- `REG_ENTRY_MODULE_HASH = 24` — byte offset within each entry
- `REG_ENTRY_HASH_SIZE = 32`
- Accessor: `FF_MODULE_REGISTRY::module_hash(base, entry_idx)` → `std::string_view` of 32 raw bytes

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

## Hashing — Two Distinct Roles

This subsystem uses SHA-256 in **two completely separate roles**. It is critical to keep them distinct in the code and in conversation.

### Role 1 — URL Hashing (Cache Slot Key)

**Purpose:** Provide a compact, fixed-length filesystem key for the per-URL sidecar metadata file.
**Algorithm:** FNV-1a 64-bit (non-cryptographic). 8 bytes → 16 hex characters.
**Input:** The raw extension URL string.
**Where computed:** `fnv1a_u64(url)` in `meta_path_for_url()` and when constructing the manifest endpoint path.
**Where stored:** Sidecar filename only: `~/.cache/fastfhir/modules/meta/<url_fnv64_hex>.meta`. Never written to any FF binary stream, never used as an in-memory lookup key.
**Lifecycle:** Ephemeral per-call.
**Why not SHA-256?** Full SHA-256 (32 bytes / 64 hex chars) is cryptographic overkill for a disk filename key over a cache of at most thousands of extension URLs. FNV-1a 64-bit has $2^{64}$ collision space — sufficient, fast, and requires no crypto library.

> **Why not use URL hashing in the filter?** The Known-Extension Filter (`FF_KnownExtensions.hpp`) uses sorted URL *string* comparison with `std::lower_bound` — not hashes. Computing SHA-256 for every extension URL during the hot predigestion scan would be more expensive than a string comparison against a small sorted table. URL hashing (Role 1) is reserved for the sidecar filesystem path only.

### Role 2 — Binary Hashing (Module Version Identity)

**Purpose:** Uniquely identify the exact version of a compiled `.wasm` binary, independent of what URL it is registered under. This is the canonical version identity for a module.
**Input:** The raw bytes of the `.wasm` file after download, before WAMR loading.
**Output:** A 32-byte SHA-256 digest that is the content-addressed key for that specific binary.
**Where computed:** In `register_module()` immediately after download, before the bytes are loaded by WAMR.
**Where stored (four locations):**
1. `CompiledModule::content_hash` — in-memory field, owned by `FF_WasmExtensionHost`.
2. `FF_MODULE_REGISTRY` entry at `REG_ENTRY_MODULE_HASH` — 32 bytes at offset 24 of each 56-byte entry, written to the FF binary stream by `write_module_registry()`. Persists in every FF stream that used this module.
3. On-disk binary filename: `~/.cache/fastfhir/modules/<binary_hash_hex>.wasm` — content-addressed, immutable once written. Two different URLs pointing to the same binary produce one cached file.
4. Sidecar metadata contents: `binary_hash_hex\nfetched_epoch\n` — the sidecar (keyed by URL hash, Role 1) records which binary version is current for this URL.

**Lifecycle:** Persists across process restarts. TTL = 24 hours for the sidecar; the binary file itself is permanent until explicitly evicted (GC TBD).
**Code:** `sha256_bytes(const void*, size_t)` helper in `FF_Extensions.cpp` → stored in `CompiledModule::content_hash`, written via `STORE` macros into `REG_ENTRY_MODULE_HASH`.

### Why Two Separate Hashes?

URL hashing (Role 1) answers: *"Where is this module's cache metadata slot on disk?"*
Binary hashing (Role 2) answers: *"Is the module binary I have on disk the current version?"*

They serve different purposes and appear in different places:

| | Role 1 — URL Hash | Role 2 — Binary Hash |
|---|---|---|
| Input | URL string | `.wasm` byte content |
| Stored in FF stream? | **No** | **Yes** — `FF_MODULE_REGISTRY` |
| Disk path role | Sidecar filename | Binary filename |
| Changes when? | URL changes (never, per-URL) | Binary content changes |
| In-memory map key? | No — map is keyed by raw URL string | Stored in `CompiledModule::content_hash` |

A module can be re-published at the same URL with a new `.wasm` binary. The URL hash is stable (same sidecar slot); the binary hash changes (new binary file, new sidecar contents). The manifest endpoint compares binary hashes to detect updates without re-downloading the binary when the version is unchanged.

---

## Known-Extension Filter

Two categories of URLs are suppressed during predigestion:

1. **Profile-native** — base FHIR + US Core + UK Core extensions auto-discovered from downloaded spec bundles by `make_lib.py` using URL domain pattern matching. Suppressed because the data is already stored in the resource's native vtable fields.
2. **HL7-known safe** — a strictly curated list of pure metadata/display hints with zero semantic weight (e.g. `data-absent-reason`, `geolocation`, ISO qualifiers). **Not** clinical or demographic extensions.

Both categories are compiled into `generated_src/FF_KnownExtensions.hpp` as compile-time sorted arrays of URL strings. Lookup is `O(log n)` binary search using `std::lower_bound` with `string_view` comparison.

`FF_ExtensionFilterMode` enum:
- `FILTER_ALL_KNOWN` (default) — suppress both categories
- `FILTER_NATIVE_ONLY` — suppress only profile-native (category 1)
- `FILTER_NONE` — store all extensions, dispatch everything to WASM

---

## Core Architectural Principle: Predigestion + Async AOT

**The URL directory and the per-stream module registry are built in a dedicated predigestion pass before any resource data is written.** This decouples URL interning *and* WASM module discovery from ingestion. The main ingest workers (including concurrent Bundle entry workers) never block on network I/O and never take a mutex for URL lookups — they receive a const, immutable `url_to_ext_ref` map and do a single read.

```
┌──────────────────────────────────────────────────────────────────┐
│  PREDIGESTION  (single-threaded, before workers start)           │
│  1. Claim FF_HEADER space (54 bytes) at arena offset 0           │
│  2. Fast simdjson scan: collect all extension URL strings        │
│  3. Filter known URLs (string binary search → skip block)        │
│  4. For each surviving URL, probe the local                      │
│     FF_WasmExtensionHost cache (memory + ~/.cache/fastfhir):     │
│        hit  → assign MODULE_IDX (MSB=1, Path A — active WASM)    │
│        miss → assign URL_IDX    (MSB=0, Path B — passive blob)   │
│               *and* enqueue a background AOT fetch+compile       │
│  5. Write FF_STRING blocks (prefixes, then leaves)               │
│  6. Write FF_URL_DIRECTORY                                       │
│  7. Write FF_MODULE_REGISTRY (one 56-byte entry per active mod.) │
│  8. Back-patch FF_HEADER::URL_DIR_OFFSET / MODULE_REG_OFFSET     │
│  9. Return immutable FF_UrlInternState (url_to_ext_ref)          │
└──────────────────────────────────────────────────────────────────┘
         │
         │ const FF_UrlInternState& (read-only)
         ▼
┌──────────────────────────────────────────────────────────────────┐
│  MAIN INGEST  (concurrent workers, lock-free, network-free)      │
│  For each extension JSON object:                                 │
│    ext_ref = url_to_ext_ref[url]                                 │
│    if ext_ref == FF_NULL_UINT32                : skip block      │
│    elif ff_ext_ref_is_module(ext_ref) (MSB=1)  : Path A          │
│        → staging ping-pong via FF_WasmExtension{Size,Store}      │
│    else (MSB=0)                                : Path B          │
│        → store the raw extension JSON sub-tree as an FF_STRING   │
│          inside this FF_EXTENSION's VALUE choice slot            │
└──────────────────────────────────────────────────────────────────┘

       (concurrently, on background AOT thread:)
┌──────────────────────────────────────────────────────────────────┐
│  ASYNC AOT WORKER                                                │
│  - dequeue url strings enqueued during predigestion              │
│  - probe manifest endpoint → get latest binary_hash (Role 2)    │
│  - if binary already on disk: load & register (no download)      │
│  - else: HTTP GET binary by content hash, verify sha256, cache   │
│  - WAMR AOT-compile, register in in-memory module map            │
│    (next stream sees Path A for these URLs)                      │
└──────────────────────────────────────────────────────────────────┘
```

The predigestion pass uses simdjson to do a structural scan for `"url"` values under any `extension` or `modifierExtension` array at any nesting depth. It does not fully parse every field — it only extracts string values for keys named `"url"`. This makes it nearly as fast as a raw byte scan while being JSON-correct.

**Path A vs Path B at ingest:** worker threads do not block on network. The predigestion pass already chose the route per URL. New URLs encountered for the first time always take Path B for *this* stream; the AOT worker populates the cache in time for subsequent streams to upgrade those same URLs to Path A.

**Hashing during predigestion:** the predigestion pass performs **no hashing**. Filter lookup is a sorted string binary-search in `FF_KnownExtensions.hpp`. Cache probing calls `has_module_cached(url)` which uses `std::unordered_map<string, CompiledModule>` keyed by raw URL string. Binary hashing (Role 2) only occurs inside `register_module()` and `write_module_registry()` — not in the hot predigestion scan path.

---

## Implementation Phases

### Phase 5 — Generator: FF_KnownExtensions ✅

**Step 14.** `tools/generator/make_lib.py` emits `generated_src/FF_KnownExtensions.hpp`:
- Category 1 (profile-native): auto-detected from downloaded spec bundles by URL domain pattern (`http://hl7.org/fhir/StructureDefinition/`, `http://hl7.org/fhir/us/core/`, `https://fhir.hl7.org.uk/`). No hardcoded list.
- Category 2 (HL7-known safe): small curated list of metadata/display-only URLs.
- Both emitted as `constexpr` sorted `string_view` arrays.
- `FF_IsKnownExtension(url)` / `FF_IsNativeExtension(url)` use `std::lower_bound` with string comparison — no hashing.

---

### Phase 6 — WAMR Host Integration ✅

**Step 15.** WAMR integrated behind `FASTFHIR_ENABLE_EXTENSIONS` CMake flag via `FetchContent`.

**Step 16.** `FF_WasmExtensionHost` implemented in `include/FF_Extensions.hpp` / `src/FF_Extensions.cpp`:
- `std::call_once` engine init.
- Per-module `std::shared_mutex` protecting `.wasm` → AOT compilation.
- Per-thread WAMR module instance + 64KB staging buffer.

**Step 17.** Three host wrappers exposing the staging ping-pong ABI:
```cpp
// Mirror of SIZE_FF_<TYPE>: calls ext_size, returns byte-count of encoded vtable.
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

**Step 18.** WASM entry points — every extension module exports exactly three core-WASM functions:
```c
__attribute__((export_name("ext_size")))
uint32_t ext_size(uint32_t staging_ptr, uint32_t version);

__attribute__((export_name("ext_encode")))
uint32_t ext_encode(uint32_t staging_ptr, uint32_t version);

__attribute__((export_name("ext_decode")))
uint32_t ext_decode(uint32_t staging_ptr, uint32_t version);
```
Compile flags: `--target=wasm32-wasi -O3 -fno-exceptions -fno-rtti -nostdlib++ -Wl,--no-entry`. No host imports required.

---

### Phase 7 — Registry Fetching & Version-Aware Caching ✅

**Step 19.** Registry fetch implemented in `src/FF_Extensions.cpp`.

**In-memory cache** (key: raw URL string):
- `std::unordered_map<std::string, CompiledModule>` protected by `std::shared_mutex`.
- `CompiledModule` carries `content_hash` (32-byte Role 2 binary hash), `hash_known` bool, `fetched_at` epoch.
- `has_module_cached(url)` probes under a `shared_lock`.

**On-disk cache (two files per module):**

| Path | Keyed by | Contents | Mutability |
|---|---|---|---|
| `~/.cache/fastfhir/modules/<binary_hash_hex>.wasm` | Role 2: binary hash | raw `.wasm` bytes | Immutable once written |
| `~/.cache/fastfhir/modules/meta/<url_fnv64_hex>.meta` | Role 1: FNV-1a 64-bit URL hash (stable slot) | `binary_hash_hex\nfetched_epoch\n` | Updated on TTL refresh |

**Endpoints:**
- Binary: `https://registry.fastfhir.org/v1/modules/<binary_hash_hex>.wasm`
- Manifest: `https://registry.fastfhir.org/v1/modules/<url_fnv64_hex>/latest` → `<binary_hash_hex>\n`

**Verification:** `sha256_bytes(downloaded_bytes, len)` must equal manifest-reported binary hash. Reject on mismatch.

**Version detection flow:**
```
resolve_or_fetch_module(url):
  1. In-memory: if cached && age < TTL → return true (fast path, no I/O)
  2. Read sidecar (keyed by url_hash/Role 1) → get binary_hash + fetched_at
  3. If sidecar fresh (age < 24h TTL):
       load binary from content-addressed path (keyed by binary_hash/Role 2)
       verify sha256(bytes) == binary_hash → register_module()
  4. If sidecar stale or missing:
       GET manifest → latest binary_hash_hex
       if that binary already on disk → refresh sidecar TTL, load & register (no download)
       else → GET binary by content hash, verify sha256, persist binary + sidecar
  5. Offline fallback: load stale disk copy if any; else skip — do NOT block ingestion
```

**Offline fallback:** log via `FF_Logger`, skip WASM dispatch for that URL, append to `Parser::unresolved_extensions()`.

---

### Phase 8 — ffc.py --wasm mode ✅

**Step 20.** `--wasm` flag added to `ffc.py`. For each extension type's `StructureDefinition`, emits one self-contained WASM translation unit containing the codec triple — exactly the same `SIZE_FF_*` / `STORE_FF_*` / `FF_DESERIALIZE_*` shapes the generator already produces for native resources, but renamed to `ext_size` / `ext_encode` / `ext_decode` and rewired to read/write the staging buffer instead of the FastFHIR arena. Heap-allocating types are replaced with stack-only equivalents whose byte layout matches `ExtensionData` over the staging boundary.

**Step 21.** Per-resource `dispatch_extensions` / `dispatch_modifier_extensions` helpers emitted that route to `FF_WasmExtension{Size,Store,Deserialize}` based on URL. When `FASTFHIR_ENABLE_EXTENSIONS=OFF`, helpers are empty `inline` functions — zero-cost.

---

### Phase 9 — EXT_REF MSB Routing, Async AOT Worker, Path B Fallback ✅

#### 9.1 EXT_REF Discriminated Union (FF_EXTENSION)
- Field renamed `URL_IDX` → `EXT_REF` in `ffc.py` `BLOCK_FIELD_OVERRIDES`.
- `FF_Primitives.hpp` adds `FF_EXT_REF_MSB`, `FF_EXT_REF_INDEX_MASK`, helper inlines.
- Generated `EXTENSIONView::get_url()` masks the MSB before consulting `FF_URL_DIRECTORY`.
- Generated `EXTENSIONView::is_active_module()` and `module_idx()` accessors added.

#### 9.2 Async AOT Worker
- `FF_WasmExtensionHost` owns a single background thread + condition-variable queue.
- `enqueue_resolve(url)` is non-blocking; idempotent (dedup via `m_in_flight` set).
- Worker runs `resolve_or_fetch_module` per URL; removes from `m_in_flight` on completion.
- Cancellation: dtor sets `m_aot_stop=true`, notifies CV, joins thread.

#### 9.3 Path B Passive Storage
- During predigestion, cache-miss URLs get `URL_IDX` (MSB=0). Raw JSON sub-tree captured via `simdjson::raw_json()` into `FF_UrlInternState::passive_payload` side-table.
- Concurrent ingest workers store the raw JSON slice as `FF_STRING` in the `VALUE` ChoiceEntry tagged `RECOVER_FF_OPAQUE_JSON (0x0008)`.
- Phase 7 of predigestion drops `passive_payload` entries for URLs promoted to Path A (memory efficiency).
- Round-trip export: MSB=0 extensions emit stored JSON verbatim.

---

### Phase 10 — Binary Hash in FF_MODULE_REGISTRY + Version Detection ✅

#### 10.1 FF_MODULE_REGISTRY Layout Expanded (24 → 56 bytes per entry)
Each registry entry is now 56 bytes:
```
URL_IDX(4) | PAD(4) | WASM_BLOB_OFFSET(8) | WASM_BLOB_SIZE(4) | PAD2(4) | MODULE_HASH(32)
```
`MODULE_HASH` is the **binary hash** (Role 2): SHA-256 of the raw `.wasm` bytes at ingest time.
- Accessor: `FF_MODULE_REGISTRY::module_hash(base, entry_idx)` → `std::string_view` of 32 raw bytes.
- Computed in `register_module()` via `sha256_bytes(wasm_bytes, len)`, stored in `CompiledModule::content_hash`.
- Written to the registry entry by `write_module_registry()` from the in-memory `CompiledModule::content_hash`.
- A reader opening an older FF stream sees the binary hash that was current at write time — enabling it to detect if a newer module version is now available by comparing against the manifest.

#### 10.2 Verification
1. Ingest stream with new URL → `EXT_REF` MSB=0, JSON blob in VALUE, AOT enqueued, `MODULE_HASH` = 32 zero bytes.
2. Re-ingest after AOT completes → `EXT_REF` MSB=1, `MODULE_HASH` = SHA-256 of downloaded binary.
3. Simulate module update: manifest returns new binary hash → new binary downloaded, sidecar updated, old binary file left in place (GC TBD).
4. Simulate binary corruption: `sha256(disk_bytes) != sidecar_binary_hash` → triggers re-fetch.
5. Confirm `FF_MODULE_REGISTRY` entry size = 56 bytes (`REG_ENTRY_SIZE` constant).
6. Confirm `FF_EXTENSION` vtable size unchanged = 40 bytes.

---

## Affected Files

| File | Change |
|---|---|
| `tools/generator/ffc.py` | Regenerate `FF_EXTENSION` with EXT_REF layout (pending FHIR specs) |
| `generated_src/` (all) | Full regeneration when FHIR specs available |
| `tools/generator/make_lib.py` | Emit `FF_KnownExtensions.hpp` (Phase 5) — dynamic spec-driven |
| `generated_src/FF_KnownExtensions.hpp` | New — generated known-extension URL sorted arrays (Phase 5) |
| `include/FF_Extensions.hpp` / `src/FF_Extensions.cpp` | WAMR host, dispatch, registry fetch, binary hash cache (Phases 6–7) |
| `include/FF_Primitives.hpp` | `FF_MODULE_REGISTRY` 56-byte entries, `REG_ENTRY_MODULE_HASH`, `module_hash()` |
| `src/FF_Primitives.cpp` | `module_hash()` implementation |
| `CMakeLists.txt` | `FASTFHIR_ENABLE_EXTENSIONS` flag, WAMR `FetchContent` (Phase 6) |

---

## Progress

| Phase | Status |
|---|---|
| Phase 1 — Binary Structures | ✅ Complete |
| Phase 2 — FF_EXTENSION vtable (generator) | ✅ Complete in `ffc.py`; generated files pending regeneration |
| Phase 3 — Predigestion + ingest integration | ✅ Complete |
| Phase 4 — Parser read path | ✅ Complete |
| Phase 5 — FF_KnownExtensions generator (spec-driven) | ✅ Complete |
| Phase 6 — WAMR host | ✅ Complete |
| Phase 7 — Registry fetch + version-aware binary-hash caching | ✅ Complete |
| Phase 8 — ffc.py --wasm mode | ✅ Complete |
| Phase 9 — EXT_REF MSB routing + async AOT + Path B fallback | ✅ Complete |
| Phase 10 — Binary hash in FF_MODULE_REGISTRY + version detection | ✅ Complete |

---

## Remaining Work (Open TODOs)

| # | Item | Priority |
|---|---|---|
| 0 | Reassess Python Bindings - ensure they handle both URLS and binary modules correctly | High |
| 1 | Implement `http_get_manifest()` — real TLS HTTP GET to manifest endpoint | High |
| 2 | Implement `http_get_wasm()` — real TLS HTTP download of binary by content hash | High |
| 3 | TLS support for HTTP fetch (replace plain TCP stub) | High |
| 4 | `FF_IsKnownExtension()` / `FF_IsNativeExtension()` — implement URL string binary search | High |
| 5 | `FF_ExtensionFilterMode` enum — apply in predigestion hot path | High |
| 6 | `Parser::unresolved_extensions()` list — collect offline-fallback skip log | Medium |
| 7 | Implement Path A ingest dispatch in concurrent workers (`FF_WasmExtension{Size,Store}`) | Medium |
| 8 | Implement Path B round-trip export — emit stored raw JSON verbatim | Medium |
| 9 | Ingest Synthea bundle end-to-end — verify URL directory, zero known-ext blocks, binary hash in registry | Medium |
| 10 | Phase 9 verification tests — MSB=0/1 correct, AOT enqueue, round-trip | Medium |
| 11 | Write first real WASM codec module — geolocation test case with wasi-sdk | Low |
| 12 | `ffc.py --wasm` mode — emit codec triples from StructureDefinition | Low |
| 13 | Binary file GC — evict old `.wasm` files from disk cache when superseded | Low |

---

## Verification (End-to-End Acceptance)

1. `cmake --build build --target build_all -j` — clean build with all extension phases.
2. Ingest the Synthea patient record; dump arena: confirm `FF_URL_DIRECTORY` block present, each unique URL stored exactly once as an `FF_STRING`.
3. Verify `EXTENSIONView::get_url()` returns correct reconstructed URL for `geolocation` and a Synthea-specific URL.
4. Verify known extensions (`us-core-race`, `us-core-ethnicity`, `patient-birthPlace`, etc.) produce zero `FF_EXTENSION` blocks in the output.
5. Profile arena bytes for a 50-resource Synthea bundle before/after — expect >10× reduction in extension URL storage.
6. WASM codec round-trip: compile a minimal codec module for `geolocation` with wasi-sdk; ingest a Patient with a `geolocation` extension; verify `ext_size` matches host's expected size, `ext_encode` is byte-identical to the C++ generated path, `ext_decode` round-trips back to original `ExtensionData`.
7. Verify `FF_MODULE_REGISTRY` entry size = 56 bytes and `MODULE_HASH` field contains correct SHA-256 of the loaded binary.

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
