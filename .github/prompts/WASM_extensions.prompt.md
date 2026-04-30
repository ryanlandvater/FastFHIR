# FastFHIR Extension Subsystem: Implementation Plan

**Context.** An "Open World" FHIR extension subsystem for the FastFHIR C++ library, built on a lock-free, zero-copy, offset-based memory arena. Codec dispatch goes through the WebAssembly Micro Runtime (WAMR). This document also covers the URL deduplication design that is a prerequisite for the WASM work.

---

## 1. Purpose of a WASM Extension Module

**A WASM extension module is a drop-in vtable codec for one specific FHIR extension URL.** It plays the same role for an unknown extension that the generated C++ routines (`SIZE_FF_<TYPE>`, `STORE_FF_<TYPE>`, `FF_DESERIALIZE_<TYPE>`, `<TYPE>View<VERSION>::get_*`) play for built-in resources. The host (WAMR) treats the module as a function-pointer triple keyed by extension URL.

| Native generated C++                                  | WASM export                                                          |
|-------------------------------------------------------|----------------------------------------------------------------------|
| `Size SIZE_FF_<TYPE>(const Data&, uint32_t version)`  | `ext_size(uint32_t staging_ptr, uint32_t version) -> uint32_t`       |
| `Offset STORE_FF_<TYPE>(BYTE*, Offset, const Data&, uint32_t)` | `ext_encode(uint32_t staging_ptr, uint32_t version) -> uint32_t` (bytes written) |
| `Data FF_DESERIALIZE_<TYPE>(const BYTE*, Offset, Size, uint32_t)` | `ext_decode(uint32_t staging_ptr, uint32_t version) -> uint32_t` (writes typed payload to staging) |
| `<TYPE>View<VERSION>::get_field()`                    | not needed — host reads decoded staging payload directly             |

**Out of scope for the module:**
- No `ok / unknown / error` recognition codes — URL→module binding happens at parse time via the registry; if `ext_decode` is called, the URL was already matched.
- No host imports for logging or field access. Linear-memory loads/stores only.
- No WIT / Component Model / `borrow<>` / resource types. Plain WAMR core-WASM exports with `i32`/`i64` parameters.
- No host-heap allocations. The host pre-allocates a per-thread 64 KiB staging region inside the guest's linear memory.
- No visibility into `FF_URL_DIRECTORY`, the FastFHIR arena, or any `Offset > 4 GiB`. The module is a self-contained codec for one URL's vtable layout.

---

## 2. Ground Truth: Binary Representations

### FF_EXTENSION vtable (44 bytes)
```
VALIDATION (8) | RECOVERY (2) | ID (8) | EXTENSION (8) | EXT_REF (4) | EXT_VERSION (4) | VALUE (10)
```
`EXT_REF` is a 4-byte **discriminated-union routing word**. Bit 31 is the routing flag; the lower 31 bits are an index payload:

| MSB | Meaning              | Lower-31 bits | Lookup target        | Codec path           |
|-----|----------------------|---------------|----------------------|----------------------|
| `0` | **Passive** (Path B) | `URL_IDX`     | `FF_URL_DIRECTORY`   | Opaque raw-JSON blob |
| `1` | **Active**  (Path A) | `MODULE_IDX`  | `FF_MODULE_REGISTRY` | WASM `ext_*` triplet |

Helpers in `FF_Primitives.hpp`:
```cpp
constexpr uint32_t FF_EXT_REF_MSB        = 0x80000000u;
constexpr uint32_t FF_EXT_REF_INDEX_MASK = 0x7FFFFFFFu;
constexpr uint32_t FF_EXT_REF_NULL       = FF_NULL_UINT32;

inline bool     ff_ext_ref_is_module(uint32_t r) { return (r & FF_EXT_REF_MSB) != 0; }
inline uint32_t ff_ext_ref_index    (uint32_t r) { return  r & FF_EXT_REF_INDEX_MASK; }
inline uint32_t ff_make_module_ref  (uint32_t i) { return  i | FF_EXT_REF_MSB; }
inline uint32_t ff_make_url_ref     (uint32_t i) { return  i & FF_EXT_REF_INDEX_MASK; }
```

This replaces the old 8-byte `Offset → FF_STRING` field, saves 4 bytes per extension entry, and folds the URL-vs-module routing decision into a single load.

`EXT_VERSION` is a 4-byte **per-instance codec version stamp** with the same MAJOR(2) / MINOR(2) layout used by `FF_HEADER::VERSION`:

```cpp
constexpr uint32_t FF_EXT_VERSION_MAJOR_MASK = 0x3FFFu;     // mirrors FF_ENGINE_MAJOR
inline uint16_t ff_ext_major(uint32_t v) { return  v        & 0x3FFFu; }
inline uint16_t ff_ext_minor(uint32_t v) { return (v >> 16) & 0xFFFFu; }
inline uint32_t ff_make_ext_version(uint16_t maj, uint16_t min) {
    return (uint32_t(maj) & 0x3FFFu) | (uint32_t(min) << 16);
}
```

The version stamped here is the **codec's** declared version at the moment `ext_encode` produced the bytes — *not* the FastFHIR engine version. It is what `EXTENSIONView::get_header_size(ext_version)` keys off to decide how many bytes the encoded vtable occupies, mirroring the version-aware sizing pattern already used by `FF_HEADER`, `FF_CHECKSUM`, `FF_ARRAY`, `FF_STRING`, `FF_URL_DIRECTORY`, and `FF_MODULE_REGISTRY`. See § 2a *Extension Versioning Rules* below.

### ChoiceEntry (10 bytes inline)
```
raw bits (8) | RECOVERY_TAG (2)
```
The `RECOVERY_TAG` distinguishes payload kind **without consulting `EXT_REF` or the registry** — required for safe surgical reads, `ff_export` dispatch, and partial-recovery scenarios:

| Tag                       | Path | Payload                                                       |
|---------------------------|------|---------------------------------------------------------------|
| `RECOVER_FF_OPAQUE_JSON`  | B    | Stored raw FHIR-JSON sub-tree as `FF_STRING`.                 |
| `RECOVER_FF_WASM_PAYLOAD` | A    | WASM-encoded vtable bytes; structure described by descriptor. |
| (any native scalar tag)   | —    | Primitive-valued extension stored inline like a native scalar.|

### FF_URL_DIRECTORY block
```
VALIDATION (8) | RECOVERY (2) | PREFIX_COUNT (2) | ENTRY_COUNT (4)     ← 16-byte header
PREFIX_TABLE: PREFIX_COUNT × 8-byte Offsets → FF_STRING blocks
ENTRY_TABLE:  ENTRY_COUNT  × 12-byte URLEntry { prefix_idx(2) | pad(2) | leaf_offset(8) }
```
URL split rule: at the last `/`. No-`/` edge case → `NO_PREFIX (0xFFFF)`, full URL stored as leaf only.

### FF_MODULE_REGISTRY block (88 bytes per entry)
```
VALIDATION (8) | RECOVERY (2) | ENTRY_COUNT (4) | PAD (2)              ← 16-byte header
ENTRY_TABLE: ENTRY_COUNT × 88-byte entries
  URL_IDX(4) | KIND(2) | WASM_BLOB_OFFSET(8) | WASM_BLOB_SIZE(4) | PAD2(4)
            | MODULE_HASH(32) | SCHEMA_HASH(32)
```

**KIND field (offset 4, 2 bytes):** Discriminates the codec path:
- `KIND = 0` (DYNAMIC) — WASM codec path; `WASM_BLOB_OFFSET` and `WASM_BLOB_SIZE` are valid payload pointers.
- `KIND = 1` (STATIC) — Compiled C++ extension path; `WASM_BLOB_OFFSET` is `FF_NULL_OFFSET`; recovery tag identifies the generated struct.
- `KIND ≥ 2` reserved for future extension mechanisms.

Backward compatibility: Streams with 56-byte entries (older engine, no KIND field) implicitly have KIND=DYNAMIC when read by a newer parser.

Two content-addressed identities per module:

- `MODULE_HASH` — SHA-256 of the raw `.wasm` bytes. Identifies the **codec implementation**.
- `SCHEMA_HASH` — SHA-256 of the canonical descriptor blob (`.ffd`, see § 5). Identifies the **wire schema** independently of the binary, so a reader can verify schema compatibility without invoking WAMR.

Constants/accessors: `REG_ENTRY_MODULE_HASH = 24`, `REG_ENTRY_SCHEMA_HASH = 56`, `REG_ENTRY_HASH_SIZE = 32`, `REG_ENTRY_SIZE = 88`. `module_hash(base, idx)` and `schema_hash(base, idx)` return `std::string_view` (32 bytes).

> Both hashes travel inside every FF stream that uses an active module — streams are fully self-describing for extension content, even offline.

### Per-instance vs per-module footprint

| Stored once per module (indexed by `MODULE_IDX`)                  | Stored per `FF_EXTENSION` instance              |
|-------------------------------------------------------------------|-------------------------------------------------|
| URL string (as `FF_STRING`, deduped in `FF_URL_DIRECTORY`)        | 44-byte `FF_EXTENSION` vtable                   |
| 88-byte `FF_MODULE_REGISTRY` row (URL_IDX, blob ptr, both hashes) | 4-byte `EXT_VERSION` (codec version at write time) |
| WASM blob bytes                                                   | 10-byte VALUE ChoiceEntry (Layer-1 tag + bits)  |
| `FF_EXT_SCHEMA` descriptor block (field table)                    | encoded WASM payload bytes (the actual data)    |

Every `FF_EXTENSION` carries `MODULE_IDX`; **N** instances of the same extension share one URL row, one registry row, one binary, and one descriptor.

---

## 2a. Extension Versioning Rules

Extensions follow the **same MAJOR/MINOR contract that governs the FastFHIR engine itself**. The version is stamped per-instance in `FF_EXTENSION::EXT_VERSION` at write time and consulted at read time by `EXTENSIONView::get_header_size(ext_version)` to determine the encoded payload's byte size.

### Compatibility contract — append-only fields

A registry update may **only add new fields at the end of the schema** and **must bump MINOR**. Removing a field, reordering fields, narrowing a field's type, or changing a field's `RECOVERY_TAG` is a **MAJOR** bump and requires re-publishing under a new module identity (new `MODULE_HASH` *and* new `SCHEMA_HASH`).

| Change                                              | Version bump                       | Stream-level effect                                                              |
|-----------------------------------------------------|------------------------------------|----------------------------------------------------------------------------------|
| Append new optional field at end of schema          | MINOR                              | Older readers stop at the old `vtable_size`; new readers see the new field.      |
| Append new required field                           | MAJOR                              | Old encoded instances are not upgradable in place — re-ingest required.          |
| Reorder / remove / retype a field                   | MAJOR                              | Forbidden silently — generator rejects publish.                                  |
| Change `recovery` tag of an existing field          | MAJOR                              | Layer-3 type contract broken; re-publish under fresh identity.                   |
| Patch codec implementation, schema unchanged        | none (binary patch only)           | New `MODULE_HASH`, identical `SCHEMA_HASH` → cached `.ffd` reused.               |

This mirrors the rule already enforced for FastFHIR's primitive blocks: header layouts may only **grow** between MINOR versions, never shrink or reorder, so that `get_header_size(version)` is a monotone function of MINOR within a MAJOR.

---

## 3. Phase A — Compiled Extension Tier

The compiled extension tier adds a new path for extensions derived from official FHIR StructureDefinitions (e.g. HL7's US Core, UK Core, IPS). Compiled extensions are generated as C++ types (similar to built-in resources) and stored as native arena blocks, achieving native-speed read access without WASM staging.

### Phase A work-items (7 sub-phases)

Phase A is implemented in parallel with existing WASM and passive-JSON paths, with no breaking changes:

| Phase | Work item | Duration est. | Depends on | Deliverable |
|---|---|---|---|---|
| A1 | architecture.md Section 10 + WASM_extensions.prompt.md KIND/Phase A docs | 1 day | None | Design doc complete (THIS FILE) |
| A2 | FF_Primitives.hpp KIND field + accessors; CMakeLists.txt test updates | 1 day | A1 | Compile-time constants and helper functions |
| A3 | ffc.py `compile_extension_library()` pass; make_lib.py integration | 2 days | A1 | FF_CompiledExtensions.hpp emission |
| A4 | FF_Ingestor.cpp Phase 7 compiled extension probe + routing | 1 day | A2, A3 | Predigest routing logic |
| A5 | FF_Parser.hpp KIND-aware dispatch in Node field resolution | 1 day | A2 | Parser read path |
| A6 | tests/cpp/ test_compiled_extensions covering at least us-core-race | 1 day | A2–A5 | Regression test suite |
| A7 | README.md Section 3 (Compiled Extensions subsection) + test coverage reporting | 1 day | A6 | User-facing documentation |

### Affected files (Phase A scope)

**Include headers:**
- `include/FF_Primitives.hpp` — KIND constants (`REG_ENTRY_KIND*`), accessor `ff_registry_entry_kind()`, recovery tag range constants (`RECOVER_FF_EXTENSION_COMPILED_MIN/MAX`), recovery tag partition in `Recovery_to_Kind()`.
- `include/FF_Parser.hpp` — KIND-aware dispatch in `Node::as<T>()` and field resolution.
- `include/FF_Ingestor.hpp` — no API change, but internal Phase 7 signature may be revised to accept compiled extension table.

**Runtime sources:**
- `src/FF_Ingestor.cpp` — Phase 7 extended with compiled extension probe.
- `src/FF_Parser.cpp` — KIND-aware dispatch implementation (if any).

**Generator:**
- `tools/generator/ffc.py` — new `compile_extension_library()` function; integration in `compile_fhir_library()`.
- `tools/generator/make_lib.py` — wire `compile_extension_library()` call; update output manifest.

**Generated artifacts:**
- `generated_src/FF_CompiledExtensions.hpp` — (new) typed extension structs, recovery tags, TypeTraits, lookup table.
- `generated_src/FF_Recovery.hpp` — updated with STATIC extension tags.
- `generated_src/FF_Reflection.hpp/cpp` — ParserOps entries for compiled extensions (if needed).

**Build:**
- `CMakeLists.txt` — add test target `test_compiled_extensions`, integrate `FF_CompiledExtensions.hpp` generation into the codegen chain.

**Documentation:**
- `architecture.md` — Section 10 (this document, A1 deliverable).
- `README.md` — new Compiled Extensions subsection (A7 deliverable).
- `WASM_extensions.prompt.md` — Phase A docs (A1 deliverable, i.e., this file).

### Implications for the descriptor (`FF_EXT_SCHEMA`)

The schema descriptor is the source of truth for "which fields exist in version V." Each field-table entry carries an `intro_version (4)` stamp recording the MAJOR.MINOR in which that field was introduced. At read time, the host iterates the field table filtered by `intro_version <= instance.EXT_VERSION`.

```
descriptor.fields_visible_at(ext_version) =
    [ f for f in descriptor.fields if f.intro_version <= ext_version ]
```

The descriptor's own header records the **highest known version** it describes (`SCHEMA_VERSION_HIGH(4)`). A reader whose loaded descriptor has `SCHEMA_VERSION_HIGH < instance.EXT_VERSION` knows the on-disk descriptor is stale and must refetch by `SCHEMA_HASH`.

### Implications for `get_header_size`

`EXTENSIONView::get_header_size(uint32_t ext_version) const` returns:

- The fixed 44-byte `FF_EXTENSION` vtable size when the encoded payload is inline (the common case — vtable-only fields).
- For variable-length payload extensions, the encoded vtable byte count for that exact `ext_version`. Behaviour is identical to `FF_HEADER::get_header_size(engine_version)` and the other primitives' version-aware sizers.

Because field append is monotone, `get_header_size(v_old) ≤ get_header_size(v_new)` whenever both share the same MAJOR.

### Implications for `ext_encode` / `ext_decode`

- The codec module exports an additional integer constant `EXT_CODEC_VERSION` (a `__attribute__((export_name("ext_codec_version")))` `uint32_t` returning function with no parameters) reporting the codec's own MAJOR/MINOR.
- The host stamps the result of `ext_codec_version()` into `FF_EXTENSION::EXT_VERSION` immediately after `ext_encode()` returns.
- On decode, the host calls `ext_decode(staging_ptr, ext_version)` passing the per-instance version so the codec can ignore fields not yet introduced in that version (zero-initialise them in staging).

### Manifest extension

The manifest gains a `codec_version` field (MAJOR.MINOR). Re-publishes that increment MINOR keep the same URL slot; MAJOR bumps require a new URL or are rejected by `register_module()` to enforce the append-only invariant locally as well as in the registry.

### Asymmetric resolver — newer hashes pull, older hashes reuse

Because MAJOR identity is encoded by `SCHEMA_HASH` (any non-append change forces a new descriptor identity), and MINOR evolution within a MAJOR is **strictly append-only**, a cached codec at MINOR `M_cache` is guaranteed to correctly decode any in-stream instance whose `EXT_VERSION` MINOR is `≤ M_cache` — the older instance simply uses a prefix of the field table the cached descriptor describes. This permits an asymmetric resolver policy that eliminates redundant downloads for backfill / archival / replay workloads:

| In-stream `(SCHEMA_HASH, EXT_VERSION_minor)` vs cached `(SCHEMA_HASH_cache, MINOR_HIGH_cache)` | Action                                                                                                 | Rationale                                                                                  |
|--------------------------------------------------------------------------------------------------|--------------------------------------------------------------------------------------------------------|--------------------------------------------------------------------------------------------|
| `SCHEMA_HASH == SCHEMA_HASH_cache` and `minor ≤ MINOR_HIGH_cache`                                | **Use cache.** No network. Decode at native speed.                                                     | Same MAJOR lineage, instance is at or behind cache. Append-only guarantees correctness.    |
| `SCHEMA_HASH == SCHEMA_HASH_cache` and `minor >  MINOR_HIGH_cache`                                | **Pull upgrade**, refresh `MINOR_HIGH`, then prune superseded same-lineage cache artefacts.           | Same MAJOR but instance was written by a newer codec; old same-lineage blobs are redundant.|
| `SCHEMA_HASH != SCHEMA_HASH_cache` and the unknown hash advertises a manifest                    | **Pull new lineage** as a parallel cache slot (does not evict other lineages).                         | Different MAJOR identity; lineages may legitimately coexist for different streams.         |
| `SCHEMA_HASH != SCHEMA_HASH_cache`, no manifest reachable                                        | **Downgrade** to opaque-payload passthrough; record in `Parser::unresolved_extensions()`.             | Identity unverifiable; refuse to silently substitute a different lineage's codec.          |

Key invariant: the resolver **never** trades a known-good cached lineage for a different `SCHEMA_HASH`. Different lineages remain side-by-side. Within one lineage, however, a successful MINOR upgrade triggers in-place GC of superseded artefacts (old `.ffd`, old `.wasm`, stale sidecar pointer) after the new module has been validated and committed.

Implementation lives in `resolve_or_fetch_module()` (`src/FF_Extensions.cpp`):

```cpp
ResolveResult FF_WasmExtensionHost::resolve_or_fetch_module(
    std::string_view url,
    std::span<const uint8_t, 32> in_stream_schema_hash,   // from FF_MODULE_REGISTRY
    uint16_t                     in_stream_minor)         // from FF_EXTENSION::EXT_VERSION (highest seen across instances)
{
    // 1. In-memory hit on the SAME lineage (SCHEMA_HASH match).
    if (auto* m = m_modules.find_by_schema_hash(in_stream_schema_hash)) {
        if (in_stream_minor <= m->schema->minor_high)
            return { m, ResolveResult::CACHE_HIT_SUFFICIENT };       // ← cheap fast path
        auto r = fetch_descriptor_upgrade(m, in_stream_schema_hash); // same lineage, newer descriptor
        if (r.ok())
            gc_superseded_same_lineage(*r.module);                   // delete old same-lineage cache artefacts
        return r;
    }
    // 2. New lineage — install side-by-side, never evict.
    return fetch_new_lineage(url, in_stream_schema_hash);
}
```

This policy means a fleet that has *ever* loaded the latest MINOR of a codec can subsequently re-ingest, re-export, or surgically read any historical stream produced by any earlier MINOR of the same lineage, fully offline, while keeping cache footprint bounded to the newest artefacts per lineage.

---

## 3. Three-Layer Recovery Model

Recovery for an extension is enforced at three independent layers. Each answers a different question and degrades gracefully when the layer above is unavailable. **All three are checkable without invoking WAMR.**

| Layer | Lives in                          | Identifies                                | Granularity         | Mechanism                            |
|-------|-----------------------------------|-------------------------------------------|---------------------|--------------------------------------|
| 1     | `FF_EXTENSION::VALUE` ChoiceEntry | "opaque JSON" vs "WASM payload" vs scalar | per-instance        | static `RECOVERY_TAG` enum           |
| 2     | `FF_MODULE_REGISTRY` entry        | schema version of this codec              | per-loaded-module   | 32-byte content hash (`SCHEMA_HASH`) |
| 3     | `FF_EXT_SCHEMA::FIELD_TABLE[i]`   | typed schema of one field                 | per-field           | static `RECOVERY_TAG` enum           |

- **Layer 1** lets the validator, `ff_export`, and `Compactor` dispatch correctly without touching the registry, and lets a reader without the matching WASM module refuse to lie about the bytes.
- **Layer 2** is what makes typed surgical access (`Node::extension(url)["field"].as<T>()`) safe across module updates: in-stream `SCHEMA_HASH` matches local descriptor → typed reads/writes at native speed; mismatch → fetch matching descriptor by hash, or downgrade to "opaque blob, codec unsafe".
- **Layer 3** type-checks per-field reads. The recovery tag stored in the descriptor is identical to what `TypeTraits<T>::recovery` would return for native fields.

> **Why a schema hash is not a recovery tag.** Recovery tags are 16-bit enumerators in a closed namespace owned by the generator; schema hashes are 256-bit fingerprints in an open namespace. Same conceptual role at radically different scales.

---

## 4. WAMR Core-WASM ABI — Architectural Constraints

This subsystem is **WAMR-only** and uses **core WebAssembly** exports. It deliberately rejects Wasmtime, the Component Model, WIT-generated bindings, and `borrow<>`/resource types — those impose runtime-driven memory ownership and indirect dispatch that defeat zero-copy/lock-free guarantees. Every export takes only `i32`/`i64`; every guest read/write is a plain load/store against its own linear memory.

- **Do NOT** alias the FastFHIR arena into WAMR linear memory. WAMR cannot remap its own allocation, and 64-bit `Offset` does not fit a 32-bit WASM address space.
- **Do NOT** pass 64-bit `Offset` as WASM parameters. The guest sees `uint32_t` staging-buffer-relative pointers; the host owns the arena↔staging copy.

**Staging ping-pong:**
1. At module instantiation, allocate one 64 KiB staging buffer inside the guest's linear memory; record the `uint32_t` guest address; reuse it for every call to that module.
2. **Encode (ingest):** host writes source payload into staging → `ext_size` / `ext_encode` → host reads encoded vtable bytes → blits them into the FastFHIR arena. Mirrors `SIZE_FF_<TYPE>` / `STORE_FF_<TYPE>`.
3. **Decode (parser):** host copies vtable bytes from arena into staging → `ext_decode` → host reads typed payload back. Mirrors `FF_DESERIALIZE_<TYPE>`.
4. The guest never sees an arena pointer; the host never sees a guest pointer. The boundary is the staging buffer, exchanged as a single `i32`.

---

## 5. Hashing — Two Distinct Roles

Two different hashes serve two different purposes.

| Role                            | Algorithm     | Input         | Where it lives                                                                        | In FF stream? |
|---------------------------------|---------------|---------------|---------------------------------------------------------------------------------------|---------------|
| **1 — URL hash** (cache slot)   | FNV-1a 64-bit | URL string    | Sidecar filename `~/.cache/fastfhir/modules/meta/<url_fnv64_hex>.meta`                | **No**        |
| **2 — Binary hash** (codec ID)  | SHA-256       | `.wasm` bytes | `CompiledModule::content_hash`; `FF_MODULE_REGISTRY::MODULE_HASH`; binary filename; sidecar contents | **Yes**       |
| **Schema hash** (Layer 2)       | SHA-256       | `.ffd` bytes  | `CompiledModule::schema_hash`; `FF_MODULE_REGISTRY::SCHEMA_HASH`; descriptor filename | **Yes**       |

URL hash answers *"where is this module's metadata slot on disk?"* — 64 bits of collision space is sufficient for a cache of a few thousand URLs, and avoids running SHA-256 in the predigestion hot path. Binary hash answers *"is the module on disk the version I expect?"* — it must be cryptographic because it gates execution of downloaded code. Schema hash answers *"does this codec produce the wire format my reader binds against?"* — same cryptographic strength, separate identity, so a codec re-publish that keeps the schema unchanged reuses the cached descriptor.

A module can be re-published at the same URL with new `.wasm` bytes: URL hash stable (same sidecar slot), binary hash changes (new file, new sidecar contents). The manifest endpoint compares binary hashes to detect updates.

---

## 6. Known-Extension Filter

Two categories of URLs are suppressed during predigestion:

1. **Profile-native** — base FHIR + US Core + UK Core extensions auto-discovered by `make_lib.py` from spec-bundle URL domains. Suppressed because the data is already stored in the resource's native vtable fields.
2. **HL7-known safe** — small curated list of pure metadata/display hints (`data-absent-reason`, `geolocation`, ISO qualifiers). **Not** clinical or demographic.

Both compile into `generated_src/FF_KnownExtensions.hpp` as `constexpr` sorted `string_view` arrays; lookup is `std::lower_bound` — no hashing.

`FF_ExtensionFilterMode`: `FILTER_ALL_KNOWN` (default), `FILTER_NATIVE_ONLY`, `FILTER_NONE`.

---

## 7. Predigestion + Async AOT — Core Architectural Principle

**The URL directory and the per-stream module registry are built in a dedicated predigestion pass before any resource data is written.** This decouples URL interning *and* WASM module discovery from ingestion. The main ingest workers receive a const, immutable `url_to_ext_ref` map and never block on network I/O or take a mutex for URL lookup.

```
PREDIGESTION (single-threaded, before workers start)
  1. Claim FF_HEADER (54 bytes) at arena offset 0
  2. simdjson scan: collect all extension URL strings
  3. Filter known URLs (sorted-string binary search → skip block)
  4. Probe FF_WasmExtensionHost cache (memory + ~/.cache/fastfhir):
       hit  → MODULE_IDX (MSB=1, Path A — active WASM)
       miss → URL_IDX    (MSB=0, Path B — passive blob) + enqueue background AOT
  5. Write FF_STRING blocks (prefixes, then leaves)
  6. Write FF_URL_DIRECTORY
  7. Write FF_MODULE_REGISTRY (one 88-byte entry per active module)
  8. Back-patch FF_HEADER::URL_DIR_OFFSET / MODULE_REG_OFFSET
  9. Return immutable FF_UrlInternState (url_to_ext_ref)

MAIN INGEST (concurrent, lock-free, network-free)
  ext_ref = url_to_ext_ref[url]
  if ext_ref == FF_NULL_UINT32  : skip block
  elif is_module(ext_ref)       : Path A — staging ping-pong via FF_WasmExtension{Size,Store}
  else                          : Path B — store raw JSON sub-tree as FF_STRING in VALUE

ASYNC AOT WORKER (background)
  - dequeue URLs enqueued during predigestion
  - probe manifest endpoint → latest binary_hash (Role 2)
  - if binary on disk: load & register; else: HTTP GET, verify sha256, cache
  - WAMR AOT-compile, register in in-memory module map
  - next stream sees Path A for these URLs
```

The predigestion scan extracts only `"url"` string values under any `extension`/`modifierExtension` array at any depth — JSON-correct but nearly as fast as a raw byte scan. **No hashing happens in the predigestion hot path.** Filter lookup is a sorted string binary search; cache probing is `std::unordered_map<string, CompiledModule>` keyed by raw URL. Binary hashing only happens inside `register_module()` and `write_module_registry()`.

New URLs always take Path B for *this* stream; the AOT worker populates the cache so subsequent streams upgrade to Path A.

---

## 8. Implementation Phases

### Phases 5–10 (✅ Complete) — Reference Summary

| # | Title                                                           | Status |
|---|-----------------------------------------------------------------|--------|
| 5 | Generator: `FF_KnownExtensions` (spec-driven, sorted string arrays) | ✅ |
| 6 | WAMR host integration (`FF_WasmExtensionHost`, staging ping-pong, three host wrappers `FF_WasmExtension{Size,Store,Deserialize}`) | ✅ |
| 7 | Registry fetching + version-aware caching (in-memory map + two-file disk cache: content-addressed `.wasm` + URL-keyed sidecar `.meta`) | ✅ |
| 8 | `ffc.py --wasm` mode (emits `ext_size`/`ext_encode`/`ext_decode` per extension type; per-resource `dispatch_extensions` helpers; zero-cost when extensions disabled) | ✅ |
| 9 | `EXT_REF` MSB routing + async AOT worker + Path B passive storage (raw JSON sub-tree captured via `simdjson::raw_json()`, stored as `FF_STRING` tagged `RECOVER_FF_OPAQUE_JSON`) | ✅ |
| 10 | `MODULE_HASH` written into `FF_MODULE_REGISTRY` at ingest; readers detect newer binary versions via manifest comparison | ✅ |

**Key shapes from these phases (still in force):**

- WASM exports: `ext_size(staging_ptr, version)`, `ext_encode(staging_ptr, version)`, `ext_decode(staging_ptr, version)` — all `uint32_t → uint32_t`. Compile flags: `--target=wasm32-wasi -O3 -fno-exceptions -fno-rtti -nostdlib++ -Wl,--no-entry`.
- Host wrappers: `FF_WasmExtensionSize / Store / Deserialize` mirror `SIZE_FF_<TYPE>` / `STORE_FF_<TYPE>` / `FF_DESERIALIZE_<TYPE>`.
- Disk cache (Phase 7):
  - `~/.cache/fastfhir/modules/<binary_hash_hex>.wasm` (content-addressed, immutable)
  - `~/.cache/fastfhir/modules/meta/<url_fnv64_hex>.meta` → `binary_hash_hex\nfetched_epoch\n` (TTL 24h)
- Endpoints: `GET /v1/modules/<binary_hash_hex>.wasm`; `GET /v1/modules/<url_fnv64_hex>/latest`.
- `resolve_or_fetch_module(url)` — in-memory hit, then sidecar (load by content hash, no download if binary unchanged), then manifest, then offline fallback (skip + log + record in `Parser::unresolved_extensions()`). Never blocks ingestion.

---

### Phase 11 — Typed Extension Decoding (🔧 Planned)

> **Motivation.** Phases 6–10 made the codec round-trip work, but the staging boundary is a typed binary struct on the guest side and *opaque bytes on the host side*. Without a host-side schema, C++ callers can only JSON round-trip — defeating the binary substrate. Phase 11 adds a runtime equivalent of `TypeTraits<T>` for extension codecs.

#### 11.1 Design Decision — Option B (Schema Descriptor) Selected

| Option | Mechanism | Outcome |
|--------|-----------|---------|
| **A** — Generated C++ header + `TypeTraits<>` linked in. | `ffc.py --wasm` emits `.hpp` alongside `.wasm`. | Demoted to optional AOT mode (`--emit-cpp`); kills runtime drop-in story. |
| **B** — Binary schema descriptor (`.ffd`) shipped with WASM. | Registry serves codec + descriptor; cached per `CompiledModule`. | **Selected.** Decouples schema from build, fingerprintable, inspectable without WAMR. |
| **C** — Self-describing `ext_describe` export. | Module exports a fourth function returning a packed schema blob. | **Rejected.** Schema invisible until WAMR-loaded; mixes concerns; bindings would need WAMR just to read field names. |

Mental model: *a WASM extension module is a `TypeTraits<T>` whose `T` is delivered as a binary descriptor at runtime instead of a C++ template at compile time.*

#### 11.2 New First-Class FF Block — `FF_EXT_SCHEMA`

```
FF_EXT_SCHEMA (variable size)
  VALIDATION (8) | RECOVERY (2) | URL_LEAF_OFFSET (8)
  STAGING_SIZE (4) | VTABLE_SIZE (4) | FIELD_COUNT (2) | SCHEMA_VERSION_HIGH (4) | PAD (4)   ← 36-byte header
  FIELD_TABLE: FIELD_COUNT × 36-byte FF_EXT_SCHEMA_FIELD entries

FF_EXT_SCHEMA_FIELD (36 bytes)
  name_offset   (8) → FF_STRING       — field name
  fhir_type     (2)                    — FF_FieldKind (string/code/decimal/...)
  recovery      (2)                    — RECOVERY_TAG (Layer 3)
  staging_off   (4) | staging_sz (4)   — offset/size in staging
  arena_off     (4) | arena_sz   (4)   — offset/size in encoded vtable
  flags         (4)                    — cardinality, optionality, kind bits
  intro_version (4)                    — MAJOR.MINOR in which this field was introduced (§ 2a)
```
Block recovery tag: new enumerator `RECOVER_FF_EXT_SCHEMA`. Validation: every field's `recovery` must be a known scalar tag; sum of `staging_sz` (filtered by `intro_version <= SCHEMA_VERSION_HIGH`) must equal `STAGING_SIZE` within padding tolerance; field `intro_version` values must be monotone non-decreasing in table order (enforces the append-only contract).

#### 11.3 Generator — `ffc.py --wasm` emits two artefacts

For each extension `StructureDefinition`:

- `<safe_name>_codec.c` → compiled to `<binary_hash>.wasm` (unchanged from Phase 8).
- `<safe_name>.ffd` → serialised `FF_EXT_SCHEMA` block built from the same `master_blocks[ext_path]['layout']` table that drives the codec emitter. **Both artefacts derive from one source — drift is impossible.**

Optional `--emit-cpp` (Option A as a build-mode optimisation): for closed deployments, additionally emit `<safe_name>.hpp` with `TypeTraits<>` specialisation frozen against the descriptor.

#### 11.4 Manifest + Cache Extensions

Manifest gains two non-breaking fields (older clients ignore and stay on Path B):
```json
{ "binary_hash": "...", "binary_size": N,
  "schema_hash": "...", "schema_size": N,
  "codec_version": "MAJOR.MINOR" }
```
`codec_version` mirrors the `EXT_CODEC_VERSION` constant exported by the WASM module (§ 2a). `register_module()` rejects a publish whose MAJOR differs from the previously cached MAJOR for the same URL — the append-only invariant is enforced both server-side at the registry and client-side on cache update.

New endpoint: `GET /v1/modules/<binary_hash_hex>.ffd` — content-addressed by **binary** hash, so one `.wasm` always pairs with one canonical `.ffd`.

Disk cache adds `~/.cache/fastfhir/modules/<binary_hash_hex>.ffd`. Verification: `sha256(.ffd) == manifest.schema_hash`. Binary and schema hashes are independent — a re-published codec with unchanged schema reuses the cached descriptor.

#### 11.5 Host Runtime — `FF_ExtensionSchema`

```cpp
struct FF_ExtensionField {
    std::string_view name;
    uint16_t         fhir_type;     // FF_FieldKind
    RECOVERY_TAG     recovery;      // Layer 3
    uint32_t         staging_off, staging_sz;
    uint32_t         arena_off,   arena_sz;
    uint32_t         flags;
    uint32_t         intro_version; // MAJOR.MINOR — see § 2a
};

struct FF_ExtensionSchema {                                  // one per loaded module
    std::string                          url;
    std::array<uint8_t, 32>              schema_hash;        // Layer 2 fingerprint
    uint32_t                             schema_version_high;// highest MAJOR.MINOR described
    uint32_t                             staging_size, vtable_size;
    std::vector<FF_ExtensionField>       fields;             // sorted by intro_version
    std::unordered_map<std::string_view, uint16_t> name_to_idx;

    // Field-iteration helpers honouring the append-only contract.
    bool field_visible_at(uint16_t idx, uint32_t ext_version) const noexcept;
};
```
`CompiledModule` gains `std::unique_ptr<FF_ExtensionSchema> schema;`, populated by `register_module()` after both `.wasm` and `.ffd` are loaded and verified.

#### 11.6 Public API — Typed Surgical Access

```cpp
class DynamicExtensionView {                                 // read path
    FF_WasmModuleInstance*       m_inst;
    const FF_ExtensionSchema*    m_schema;
    uint32_t                     m_ext_version;              // FF_EXTENSION::EXT_VERSION
    std::array<uint8_t, FF_STAGING_BUFFER_SIZE> m_decoded;   // populated by ext_decode lazily
public:
    template<typename T> T    get(std::string_view field) const;       // Layer-3 + intro_version checked
    bool                      has(std::string_view field) const;       // false if intro_version > m_ext_version
    uint32_t                  ext_version() const noexcept { return m_ext_version; }
    const FF_ExtensionSchema& schema() const noexcept;
};

class MutableExtensionView {                                 // write path
    Builder&                     m_builder;
    FF_WasmModuleInstance*       m_inst;
    const FF_ExtensionSchema*    m_schema;
    uint32_t                     m_ext_version;              // codec's reported ext_codec_version()
    std::array<uint8_t, FF_STAGING_BUFFER_SIZE> m_staging;
public:
    template<typename T> void set(std::string_view field, T value);    // rejects fields with intro_version > m_ext_version
    bool                      commit(Offset hdr_off);                  // ext_encode + stamp EXT_VERSION + memcpy
};

DynamicExtensionView Node::extension(std::string_view url) const;
MutableExtensionView Builder::extend(ObjectHandle&, std::string_view url);
```

`MutableExtensionView::commit()` queries `ext_codec_version()` once, calls `ext_encode()`, and writes the resulting MAJOR.MINOR into `FF_EXTENSION::EXT_VERSION` before blitting the encoded payload to the arena. `DynamicExtensionView` reads `FF_EXTENSION::EXT_VERSION` at construction and uses it for every subsequent `has()`/`get()` to mask away fields not yet introduced.

Read: `patient_node.extension("https://hospital.org/onco-stage").get<std::string_view>("clinical_stage")`.
Write: `builder.extend(handle, url).set("clinical_stage", "T2N0M0").set("staged_at", "2026-04-29").commit(hdr_off)`.

#### 11.7 Compatibility

- **Old streams** (Phase 10, 56-byte registry entries) load read-only; the reader detects the older entry size from the registry header and treats `SCHEMA_HASH` as 32 zero bytes (forces manifest fetch by URL, same as a fresh install).
- **Cached `.ffd` matches in-stream `SCHEMA_HASH`** → typed access at native speed.
- **Mismatch on `SCHEMA_HASH`** (different lineage) → typed access refused for that extension; downgraded to opaque-payload passthrough; logged via `FF_Logger`; appended to `Parser::unresolved_extensions()`. **The cache is never overwritten** — a different `SCHEMA_HASH` is a different lineage and is installed side-by-side if its manifest resolves (§ 2a *Asymmetric resolver*).
- **Same `SCHEMA_HASH`, instance MINOR newer than cached `MINOR_HIGH`** → reader recognises the descriptor on disk is stale, refetches the descriptor by `SCHEMA_HASH`, advances `MINOR_HIGH`, retries the typed read, then deletes superseded same-lineage artefacts from disk cache (`.ffd`, and `.wasm` when not referenced by the refreshed sidecar).
- **Same `SCHEMA_HASH`, instance MINOR older than cached `MINOR_HIGH`** → **no network**, typed read proceeds against the cached (newer) descriptor; fields with `intro_version > EXT_VERSION` are reported as absent (`has()` returns false). This is the dominant case for archival / replay workloads.
- **`ff_export`** of an active extension uses `DynamicExtensionView` to reconstruct JSON field-by-field through the schema, never via ad-hoc string formatting; emits only fields visible at the instance's `EXT_VERSION`.

#### 11.8 IDE Support — Generated Field-Name Headers

A binary descriptor at runtime is opaque to clangd. To give developers autocomplete, go-to-definition, and rename refactors over extension field names without forcing schemas into the build graph, every `.ffd` that lands in the local cache is paired with a generated C++ header. **The header is ergonomic glue only — no decoder logic; the runtime path stays fully dynamic.**

For URL `https://hospital.org/onco-stage` with fields `clinical_stage`, `staged_at`:
```
~/.cache/fastfhir/modules/<binary_hash_hex>.ffd     ← binary descriptor (canonical)
~/.cache/fastfhir/modules/include/onco_stage.hpp    ← generated IDE glue (NEW)
```
```cpp
// AUTO-GENERATED from <binary_hash_hex>.ffd — do not edit.
// Schema hash:           <schema_hash_hex>
// Schema version (high): MAJOR.MINOR
#pragma once
#include <string_view>
#include <array>

namespace ff::ext::onco_stage {
inline constexpr std::string_view URL = "https://hospital.org/onco-stage";
inline constexpr std::array<uint8_t, 32> SCHEMA_HASH = { /* 32 bytes */ };
inline constexpr uint32_t SCHEMA_VERSION_HIGH = /* MAJOR<<0 | MINOR<<16 */;

inline constexpr std::string_view clinical_stage = "clinical_stage";  // intro v1.0
inline constexpr std::string_view staged_at      = "staged_at";       // intro v1.0
inline constexpr std::string_view tnm_grade      = "tnm_grade";       // intro v1.1 (MINOR bump)

struct View { FastFHIR::DynamicExtensionView raw;
    auto get_clinical_stage() const { return raw.get<std::string_view>(clinical_stage); }
    auto get_staged_at()      const { return raw.get<std::string_view>(staged_at); }
    auto get_tnm_grade()      const { return raw.get<std::string_view>(tnm_grade); } // returns absent for v1.0 instances
};
struct Mut  { FastFHIR::MutableExtensionView raw;
    void set_clinical_stage(std::string_view v) { raw.set(clinical_stage, v); }
    void set_staged_at     (std::string_view v) { raw.set(staged_at,      v); }
    void set_tnm_grade     (std::string_view v) { raw.set(tnm_grade,      v); } // requires codec >= v1.1
    bool commit(FastFHIR::Offset hdr_off)       { return raw.commit(hdr_off); }
};
} // namespace ff::ext::onco_stage
```

Three ergonomic levels coexist:
```cpp
ext.get<std::string_view>("clinical_stage");                                  // Level 0 — runtime, no header
ext.get<std::string_view>(ff::ext::onco_stage::clinical_stage);               // Level 1 — typo-checked constant
ff::ext::onco_stage::View v{patient_node.extension(ff::ext::onco_stage::URL)};// Level 2 — typed wrapper
auto stage = v.get_clinical_stage();
```

**Emitter:** new CLI tool `ff_extbind` (same pattern as `ff_compact`/`ff_ingest`/`ff_export`):
- *Pull mode* — `register_module()` calls `ff_extbind::emit_header(ffd_path)` once per fresh descriptor.
- *Sweep mode* — `ff_extbind --sweep ~/.cache/fastfhir/modules/` regenerates every header from every cached `.ffd`. Idempotent.

**Wiring:** the cache include root `~/.cache/fastfhir/modules/include/` is published as the CMake cache variable `FASTFHIR_EXT_INCLUDE_DIR`. Consumers add it via `target_include_directories`, via `compile_commands.json` flags (so clangd auto-discovers every cached extension), or with manual `-I`.

**Naming:** namespace `ff::ext::<safe_name>` reusing `ffc.py`'s sanitiser. Reserved-keyword fields get a trailing `_` (string literal stays canonical). On `safe_name` collision (rare for reverse-DNS URLs), the second header gets a `_<short_url_hash>` suffix.

**What this is *not*:** not a `TypeTraits<>` binding (that's `--emit-cpp`); not load-bearing (stripping the include dir only regresses to Level 0); not a substitute for `SCHEMA_HASH` verification.

#### 11.9 Registry CLI — `ffhr_registry install` / `ffhr_registry upgrade`

To make module lifecycle explicit outside ingest, add a dedicated CLI:

```bash
ffhr_registry install <url>
ffhr_registry upgrade
```

`install <url>` workflow:

1. Resolve latest manifest for `<url>` (`/v1/modules/<url_fnv64_hex>/latest`).
2. Download/verify `<binary_hash>.wasm` and `<binary_hash>.ffd` (SHA-256 must match manifest).
3. Register module in local cache (same path as runtime `register_module()`), including `SCHEMA_HASH`, `MINOR_HIGH`, and `codec_version` metadata.
4. Emit IDE glue header through `ff_extbind::emit_header()`.
5. Update URL sidecar atomically to point at the installed hashes.

`upgrade` workflow:

1. Enumerate all URL sidecars under `~/.cache/fastfhir/modules/meta/`.
2. For each URL, resolve latest manifest.
3. If manifest is unchanged, no-op.
4. If same-lineage (`SCHEMA_HASH` unchanged) and newer MINOR, pull upgrade then run `gc_superseded_same_lineage()` (delete old `.ffd`; delete old `.wasm` only when no sidecar references its `MODULE_HASH`).
5. If lineage changed (`SCHEMA_HASH` differs), install as a parallel lineage slot; do not evict the existing lineage.

CLI exits non-zero on any failed verify or failed atomic sidecar write; partial success is reported per URL in a machine-readable summary (`--json`) and a human summary by default.

---

## 9. Affected Files

| File                                                         | Change                                                                                                   |
|--------------------------------------------------------------|----------------------------------------------------------------------------------------------------------|
| `tools/generator/ffc.py`                                     | `EXT_REF` layout (Phase 9); `--wasm` mode emits `<safe_name>_codec.c` + `<safe_name>.ffd` (Phase 11.3); optional `--emit-cpp` |
| `tools/generator/make_lib.py`                                | Emit `FF_KnownExtensions.hpp` (Phase 5)                                                                  |
| `generated_src/FF_KnownExtensions.hpp`                       | Generated sorted URL arrays (Phase 5)                                                                    |
| `include/FF_Extensions.hpp` / `src/FF_Extensions.cpp`        | WAMR host, dispatch, registry fetch, binary-hash cache (Phases 6–7); descriptor loader (Phase 11.5)      |
| `include/FF_Primitives.hpp` / `src/FF_Primitives.cpp`        | `FF_MODULE_REGISTRY` 88-byte entries with `SCHEMA_HASH`; `FF_EXT_SCHEMA` block; `RECOVER_FF_EXT_SCHEMA` + `RECOVER_FF_WASM_PAYLOAD` enumerators |
| `include/FF_ExtensionSchema.hpp` / `.cpp`                    | NEW — `FF_ExtensionField`, `FF_ExtensionSchema`, descriptor parser (Phase 11.5)                          |
| `include/FF_ExtensionView.hpp` / `.cpp`                      | NEW — `DynamicExtensionView`, `MutableExtensionView` (Phase 11.6)                                        |
| `include/FF_Node.hpp` / `include/FF_Builder.hpp`             | `Node::extension(url)`, `Builder::extend(handle, url)` (Phase 11.6)                                      |
| `tools/extbind/FF_ExtBind.cpp`                               | NEW — `ff_extbind` CLI; pull + sweep modes (Phase 11.8)                                                  |
| `tools/registry/FF_Registry.cpp`                             | NEW — `ffhr_registry` CLI (`install <url>`, `upgrade`, optional `--json`)                                 |
| `~/.cache/fastfhir/modules/include/<safe_name>.hpp`          | NEW (auto-generated per cached `.ffd`) (Phase 11.8)                                                      |
| `CMakeLists.txt`                                             | `FASTFHIR_ENABLE_EXTENSIONS` flag, WAMR `FetchContent`; `ff_extbind` + `ffhr_registry` targets; exported `FASTFHIR_EXT_INCLUDE_DIR` |

---

## 10. Progress

| Phase | Status |
|-------|--------|
| 1 — Binary structures                                                | ✅ Complete |
| 2 — `FF_EXTENSION` vtable in `ffc.py`                                | ✅ Complete (regenerate generated_src/ once FHIR specs land) |
| 3 — Predigestion + ingest integration                                | ✅ Complete |
| 4 — Parser read path                                                 | ✅ Complete |
| 5 — `FF_KnownExtensions` generator (spec-driven)                     | ✅ Complete |
| 6 — WAMR host                                                        | ✅ Complete |
| 7 — Registry fetch + version-aware binary-hash caching               | ✅ Complete |
| 8 — `ffc.py --wasm` mode                                             | ✅ Complete |
| 9 — `EXT_REF` MSB routing + async AOT + Path B fallback              | ✅ Complete |
| 10 — Binary hash in `FF_MODULE_REGISTRY` + version detection         | ✅ Complete |
| **11 — Typed extension decoding via schema descriptor (Option B)**   | ⬜ **Planned** |

---

## 11. Remaining Work

### Existing Phase 6–10 follow-ups

| #  | Item                                                                                                  | Priority |
|----|-------------------------------------------------------------------------------------------------------|----------|
| 0  | Reassess Python bindings — handle both URL and binary modules correctly                               | High     |
| 1  | Implement `http_get_manifest()` — real TLS HTTP GET                                                   | High     |
| 2  | Implement `http_get_wasm()` — real TLS HTTP download by content hash                                  | High     |
| 3  | TLS support for HTTP fetch (replace plain TCP stub)                                                   | High     |
| 4  | `FF_IsKnownExtension()` / `FF_IsNativeExtension()` URL string binary search                           | High     |
| 5  | Apply `FF_ExtensionFilterMode` in predigestion hot path                                               | High     |
| 6  | `Parser::unresolved_extensions()` — collect offline-fallback skip log                                 | Medium   |
| 7  | Path A ingest dispatch in concurrent workers (`FF_WasmExtension{Size,Store}`)                         | Medium   |
| 8  | Path B round-trip export — emit stored raw JSON verbatim                                              | Medium   |
| 9  | End-to-end Synthea ingest — verify URL directory, zero known-ext blocks, binary hash in registry      | Medium   |
| 10 | Phase 9 verification tests — MSB=0/1 routing, AOT enqueue, round-trip                                 | Medium   |
| 11 | First real WASM codec module — `geolocation` test case with wasi-sdk                                  | Low      |
| 12 | `ffc.py --wasm` codec emission from `StructureDefinition`                                             | Low      |
| 13 | Binary file GC — evict superseded `.wasm` files                                                       | Low      |

### Phase 11 — Typed decoding

| #  | Item                                                                                                                          | Priority |
|----|-------------------------------------------------------------------------------------------------------------------------------|----------|
| 14 | Add `RECOVER_FF_WASM_PAYLOAD` enumerator (Layer 1); stamp it from Path A workers                                              | High     |
| 15 | Extend `FF_MODULE_REGISTRY` entry to 88 bytes; add `REG_ENTRY_SCHEMA_HASH = 56`, `schema_hash()` accessor (Layer 2)            | High     |
| 16 | Define `FF_EXT_SCHEMA` block + `RECOVER_FF_EXT_SCHEMA` tag + 32-byte field-table entry layout (Layer 3)                       | High     |
| 17 | `ffc.py --wasm` emits `<safe_name>.ffd` from `master_blocks[ext_path]['layout']`                                              | High     |
| 18 | Manifest extension — `schema_hash`/`schema_size` fields; `/v1/modules/<binary_hash_hex>.ffd` endpoint and disk cache          | Medium   |
| 19 | `FF_ExtensionSchema` runtime type + descriptor loader/verifier wired into `register_module()` and `CompiledModule`            | Medium   |
| 20 | `DynamicExtensionView` / `MutableExtensionView`; anchor on `Node::extension(url)` and `Builder::extend(handle, url)`          | Medium   |
| 21 | `ff_export` of active extensions uses `DynamicExtensionView`; typed round-trip test (set → finalize → re-parse → get)          | Medium   |
| 22 | Backward-compat — readers detect 56- vs 88-byte registry entries; missing `SCHEMA_HASH` treated as zero (forces manifest fetch) | Medium   |
| 23 | Optional `ffc.py --emit-cpp` mode — emit `<safe_name>.hpp` + `TypeTraits<>` for closed deployments                            | Low      |
| 27 | § 2a — Add `EXT_VERSION (4)` field to `FF_EXTENSION` (vtable 40 → 44 bytes); helper inlines `ff_make_ext_version` / `ff_ext_major` / `ff_ext_minor` | High |
| 28 | § 2a — Add `intro_version (4)` to `FF_EXT_SCHEMA_FIELD` (32 → 36 bytes) and `SCHEMA_VERSION_HIGH (4)` to `FF_EXT_SCHEMA` header (32 → 36 bytes); enforce monotone non-decreasing `intro_version` in validator | High |
| 29 | § 2a — Implement `EXTENSIONView::get_header_size(uint32_t ext_version)` mirroring the version-aware sizers used by `FF_HEADER` / `FF_CHECKSUM` / `FF_ARRAY` / `FF_STRING` / `FF_URL_DIRECTORY` / `FF_MODULE_REGISTRY` | High |
| 30 | § 2a — Codec module exports `uint32_t ext_codec_version()`; host queries it once per module load; `MutableExtensionView::commit()` stamps the result into `EXT_VERSION` | High |
| 31 | § 2a — `register_module()` enforces the append-only invariant on cache update: rejects a publish whose MAJOR differs from the cached MAJOR for the same URL; logs and surfaces a typed error | Medium |
| 32 | § 2a — Manifest `codec_version` field round-tripped through the registry; downloads carrying older MINOR than cached are accepted (rollback allowed); MAJOR mismatch hard-fails | Medium |
| 33 | § 2a — `DynamicExtensionView::has(field)` returns `false` for fields whose `intro_version > m_ext_version`; `MutableExtensionView::set(field, …)` rejects same | Medium |
| 34 | § 2a — `ffc.py --wasm` derives `intro_version` per field from `StructureDefinition` revision metadata (or an explicit `--codec-version MAJOR.MINOR` flag for hand-authored codecs); refuses to publish a schema that removes / reorders / retypes / re-tags an existing field | High |
| 35 | § 2a — `resolve_or_fetch_module()` keyed on `SCHEMA_HASH` (not URL); maintains `m_modules.find_by_schema_hash()` index alongside the URL→module map; different lineages live side-by-side | High |
| 36 | § 2a — Asymmetric resolver fast path: if `in_stream_minor <= cached.minor_high` for matching `SCHEMA_HASH`, return cache hit without touching the manifest endpoint or the network | High |
| 37 | § 2a — `fetch_descriptor_upgrade(SCHEMA_HASH)` — pulls only the newer `.ffd` for an existing lineage; binary may be reused if `MODULE_HASH` unchanged or refetched if the manifest reports a new `MODULE_HASH` for the same `SCHEMA_HASH` | Medium |
| 38 | § 2a — Predigestion threads `(SCHEMA_HASH, max EXT_VERSION_minor)` per URL through to the resolver so the asymmetric decision is made once per stream, not once per instance | Medium |
| 39 | § 2a — `gc_superseded_same_lineage()` removes old MINOR artefacts after successful upgrade: write new sidecar atomically, then unlink superseded `.ffd`; unlink old `.wasm` only if no sidecar still references its `MODULE_HASH`; keep in-memory handles until stream end | Medium |

### Phase 11.8 — IDE Support

| #  | Item                                                                                                                          | Priority |
|----|-------------------------------------------------------------------------------------------------------------------------------|----------|
| 24 | `ff_extbind` CLI — emit IDE glue header from `.ffd`; pull mode in `register_module()`, sweep mode for cache rebuild          | High     |
| 25 | Publish `FASTFHIR_EXT_INCLUDE_DIR` (default `~/.cache/fastfhir/modules/include/`); document clangd / `compile_commands.json` wiring | Medium |
| 26 | Naming/collision rules — shared `safe_name` sanitiser; reserved-keyword field suffix; URL-hash disambiguation suffix         | Medium   |

### Phase 11.9 — Registry CLI

| #  | Item                                                                                                                          | Priority |
|----|-------------------------------------------------------------------------------------------------------------------------------|----------|
| 40 | Add `ffhr_registry install <url>` command: manifest resolve, wasm/ffd fetch, SHA-256 verify, atomic sidecar write          | High     |
| 41 | Add `ffhr_registry upgrade` command: iterate URL sidecars, compare manifest hashes, apply same-lineage upgrade policy       | High     |
| 42 | Integrate CLI path with `register_module()` to share validation, parsing, and cache write logic (single implementation path) | High     |
| 43 | CLI output contract: default human summary + `--json` machine output (`status`, `url`, `old_hash`, `new_hash`, `action`)    | Medium   |
| 44 | Upgrade cleanup integration: invoke `gc_superseded_same_lineage()` after successful same-lineage MINOR upgrade              | Medium   |

---

## 12. Verification (End-to-End Acceptance)

**Phases 5–10 (regression):**
1. `cmake --build build --target build_all -j` — clean build with all extension phases.
2. Ingest the Synthea patient record; dump arena: confirm `FF_URL_DIRECTORY` present, each unique URL stored exactly once as `FF_STRING`.
3. `EXTENSIONView::get_url()` reconstructs `geolocation` and Synthea-specific URLs correctly.
4. Known extensions (`us-core-race`, `us-core-ethnicity`, `patient-birthPlace`) produce zero `FF_EXTENSION` blocks.
5. 50-resource Synthea bundle: >10× reduction in extension URL bytes vs un-deduped.
6. WASM round-trip: minimal `geolocation` codec built with wasi-sdk; `ext_size` matches host expectation, `ext_encode` byte-identical to native C++ generated path, `ext_decode` round-trips.

**Phase 11:**
7. `FF_MODULE_REGISTRY` entry size = 88 bytes; `MODULE_HASH` matches loaded binary; `SCHEMA_HASH` matches `sha256_bytes(.ffd)`; `FF_ExtensionSchema` exposes the expected fields.
8. Three-Layer Recovery end-to-end: (a) every Path A `VALUE` ChoiceEntry carries `RECOVER_FF_WASM_PAYLOAD`; (b) every registry entry's in-stream `SCHEMA_HASH` matches the loaded descriptor; (c) `DynamicExtensionView::get<T>(field)` honours per-field Layer-3 `RECOVERY_TAG` and refuses type-mismatched reads.
9. Surgical typed write: `Builder::extend(handle, url).set("latitude", 42.36).set("longitude", -71.06).commit(hdr_off)` → finalize → re-parse → `Node::extension(url).get<double>("latitude")` returns `42.36` byte-for-byte.
10. Mismatch-tolerance: tamper with cached `.ffd`; reader detects `SCHEMA_HASH` mismatch, skips typed access, appends to `Parser::unresolved_extensions()`, leaves the rest of the stream usable.

**§ 2a — Extension versioning:**
13. Confirm `FF_EXTENSION` size = 44 bytes; `EXT_VERSION` round-trips through ingest → finalize → re-parse byte-for-byte; `EXTENSIONView::get_header_size(ext_version)` returns the correct encoded size for both v1.0 and v1.1 instances of the same module.
14. Append-only contract: publish v1.0 codec → ingest a Patient → bump generator to v1.1 (one new field) → re-publish → re-ingest using the new codec; old v1.0 instances still parse, expose only the v1.0 fields, `has(new_field)` returns `false`. New v1.1 instances expose all fields.
15. Append-only enforcement: simulate a v1.1 codec that *removes* a v1.0 field; `register_module()` (or the `--wasm` emitter) rejects with a typed error before anything is written to the cache or stream.
16. MAJOR bump rejection: simulate a publish that increments MAJOR with the same URL; `register_module()` refuses to update the cache; `Parser::unresolved_extensions()` records the conflict; existing streams remain readable against the cached MAJOR.
17. Cross-version export: a stream containing a mix of v1.0 and v1.1 instances of the same extension exports correctly via `ff_export` — each instance emits exactly the fields visible at its own `EXT_VERSION`.
18. Asymmetric resolver — older instance against newer cache: pre-load codec at MINOR 1.2, disconnect network, ingest a stream whose instances were written at MINOR 1.0 of the same `SCHEMA_HASH`. Expect zero network calls, typed access succeeds, fields introduced after MINOR 1.0 report `has()==false`.
19. Asymmetric resolver — newer instance against older cache: pre-load codec at MINOR 1.0, ingest a stream whose instances were written at MINOR 1.2 of the same `SCHEMA_HASH`. Expect exactly one descriptor refetch (the `.ffd`, not the `.wasm` unless `MODULE_HASH` changed); cached `MINOR_HIGH` advances to 1.2; superseded same-lineage cache artefacts are deleted; subsequent same-lineage streams require zero further network calls.
20. Asymmetric resolver — different lineage: ingest a stream whose `SCHEMA_HASH` differs from the cached one. Expect side-by-side install (cache now holds two entries for the same URL, keyed by `SCHEMA_HASH`); the previously cached lineage remains usable for streams that reference it.
21. Upgrade GC safety: run two parses concurrently while one triggers a MINOR upgrade; verify GC only unlinks on-disk superseded artefacts after atomic sidecar swap and never invalidates in-memory module handles used by the active parse.

**Phase 11.8:**
11. After `register_module()` lands a fresh `.ffd`, `~/.cache/fastfhir/modules/include/<safe_name>.hpp` exists with `URL`, `SCHEMA_HASH`, and one `string_view` per field. `ff_extbind --sweep` is byte-idempotent.
12. A TU using `ff::ext::<safe_name>::clinical_stage` as the subscript key compiles; clangd offers go-to-definition into the generated header; rename refactors propagate.

**Phase 11.9:**
22. `ffhr_registry install https://hospital.org/onco-stage` on an empty cache downloads `.wasm` + `.ffd`, verifies hashes, writes sidecar atomically, and emits the glue header.
23. Re-running `ffhr_registry install` for the same URL when manifest is unchanged is a no-op (no redundant download, exit status indicates unchanged).
24. `ffhr_registry upgrade` upgrades URLs with newer same-lineage MINOR, deletes superseded same-lineage artefacts, and leaves unchanged URLs untouched.
25. `ffhr_registry upgrade` encountering a new lineage (`SCHEMA_HASH` changed) installs in parallel without evicting the existing lineage.
26. `ffhr_registry upgrade --json` emits one record per URL with deterministic keys and values suitable for CI policy checks.

---

## Appendix — Header Redesign (2026-04-28, ✅ Complete)

`FF_HEADER` was expanded from 38 to 54 bytes. The 16-byte stream preamble was eliminated; `URL_DIR_OFFSET` and `MODULE_REG_OFFSET` are now proper vtable fields back-patched at finalize time, following the same pattern as `CHECKSUM_OFFSET`.

**`FF_HEADER` layout at arena offset 0:**
```
 0– 3 : MAGIC             (4)
 4– 5 : RECOVERY          (2) — RECOVER_FF_HEADER
 6– 7 : FHIR_REV          (2)
 8–15 : STREAM_SIZE       (8)
16–23 : ROOT_OFFSET       (8)
24–25 : ROOT_RECOVERY     (2)
26–33 : CHECKSUM_OFFSET   (8)
34–41 : URL_DIR_OFFSET    (8) — FF_NULL_OFFSET if absent
42–49 : MODULE_REG_OFFSET (8) — FF_NULL_OFFSET if absent
50–53 : VERSION           (4)
```

**Touched files:** `include/FF_Primitives.hpp` (`FF_HEADER` struct, `STORE_FF_HEADER` signature, `HEADER_SIZE = 54`), `src/FF_Primitives.cpp` (`get_url_dir_offset()` / `get_module_reg_offset()`, `STORE_FF_HEADER` impl), `include/FF_Memory.hpp` (`STREAM_HEADER_SIZE = FF_HEADER::HEADER_SIZE`, derived), `include/FF_Builder.hpp` / `src/FF_Builder.cpp` (constructor claims `HEADER_SIZE` only, new `m_url_dir_offset` / `m_module_reg_offset` members and `set_url_dir_offset()` API), `src/FF_Ingestor.cpp` (back-patch via `FF_HEADER::URL_DIR_OFFSET`), `src/FF_Compactor.cpp` (passes `FF_NULL_OFFSET` for both — compact archives carry no extension data), `include/FF_Parser.hpp` / `src/FF_Parser.cpp` (single unconditional read path, new `has_module_registry()` / `module_registry_offset()` accessors).

**Binary compatibility:** hard format break vs the unreleased preamble layout. All on-disk fixtures must be re-ingested.
