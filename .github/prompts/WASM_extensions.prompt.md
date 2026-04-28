# Context: FastFHIR WASM Extension Subsystem

You are assisting with the development of FastFHIR, a high-performance, open-source C++ library for FHIR resources. FastFHIR utilizes a lock-free, zero-copy architecture backed by an offset-based memory arena.

Interprocess memory is managed via a shared memory bump allocator, where an `atomic_ref` of a `uint64_t` at the start of the virtual memory arena serves as the write head.

We are implementing an "Open World" extension system. Instead of native shared libraries or string-heavy parsing, we will use WebAssembly (WASM) modules hosted on fastfhir.org. These modules act as sandboxed "Nano-Services" that process FHIR extension blocks already resident in the FastFHIR binary arena, using the WASI 0.3 Component Model.

---

# Ground Truth: FastFHIR Extension Binary Representation

Before designing any ABI, understand how extensions are stored. The C++ representation is:

```cpp
// ExtensionData — staging struct for one FHIR extension element
struct ExtensionData {
    std::string_view id;                      // optional, stored as FF_STRING block in arena
    std::vector<ExtensionData> extension;     // nested sub-extensions (recursive)
    std::string_view url;                     // canonical extension URL, stored as FF_STRING
    ChoiceEntry value;                        // value[x] — discriminated union (see below)
};

// ChoiceEntry — 10-byte discriminated union encoding all FHIR value[x] types
struct ChoiceEntry {
    RECOVERY_TAG tag = FF_RECOVER_UNDEFINED;  // 2-byte type discriminant
    std::variant<
        std::monostate,     // tag == FF_RECOVER_UNDEFINED (absent/null)
        bool,               // tag == RECOVER_FF_BOOL
        int32_t,            // tag == RECOVER_FF_INT32
        uint32_t,           // tag == RECOVER_FF_UINT32
        int64_t,            // tag == RECOVER_FF_INT64
        uint64_t,           // tag == RECOVER_FF_UINT64
        double,             // tag == RECOVER_FF_FLOAT64
        std::string_view    // tag == RECOVER_FF_STRING — value is an Offset into the arena
    > value;
};
```

Strings (`url`, `id`, `string_view` values) are stored as `FF_STRING` blocks in the arena and
are accessed at read time via zero-copy `std::string_view` pointing into the mapped region.
Nested sub-extensions are stored as an `FF_ARRAY` block (contiguous entry list with an 8-byte header).
Each entry in the outer `extension` field of any FHIR resource is likewise an `FF_ARRAY` entry.

`modifierExtension` arrays exist alongside `extension` arrays in every FHIR resource and
backbone element. Their semantic rule is strict: **if a consumer encounters a `modifierExtension`
it does not understand, it MUST reject the resource**. The WASM module must signal whether it
understood a modifier extension.

---

# Implementation Phases

## Phase 1 — The Extension ABI (WIT Contract)

This is the foundation; all other phases depend on it.

- Define the WIT (Wasm Interface Type) contract between the C++ host and the WASM guest.
- The WASM Component Model does **not** support raw pointer sharing between guest and host linear memory. The guest cannot "view" a sub-region of the C++ arena. Instead:
  - The host exports typed **read-intrinsics** that the guest calls back to read individual fields from an opaque `extension-block` resource handle.
  - The guest receives a `borrow<extension-block>` — a read-only, lifetime-bound reference to an opaque arena region. It never receives a raw pointer or an owned memory region.
- Host intrinsics must cover all `ChoiceEntry` discriminants:
  - `read-url(borrow<extension-block>) -> string` 
  - `read-value-tag(borrow<extension-block>) -> value-tag` (the discriminant)
  - `read-value-bool(borrow<extension-block>) -> bool`
  - `read-value-i32(borrow<extension-block>) -> s32`
  - `read-value-u32(borrow<extension-block>) -> u32`
  - `read-value-i64(borrow<extension-block>) -> s64`
  - `read-value-u64(borrow<extension-block>) -> u64`
  - `read-value-f64(borrow<extension-block>) -> f64`
  - `read-value-string(borrow<extension-block>) -> string`
  - `read-child-count(borrow<extension-block>) -> u32` (nested sub-extensions)
  - `read-child(borrow<extension-block>, index: u32) -> borrow<extension-block>`
- The guest's exported function signature must accept a **batch handle** (a `borrow<extension-batch>`) plus:
  - `fhir-version: u32` — the FHIR version constant (`0x0400` = R4, `0x0500` = R5)
  - `is-modifier: bool` — whether this is a `modifierExtension` array
- The guest's return type must be a result discriminating between:
  - `ok` — extension understood and processed
  - `unknown` — extension not understood (not an error; host skips)
  - `reject-resource` — extension is a `modifierExtension` that was NOT understood (host must invalidate the resource)
  - `error(string)` — processing failed

## Phase 2 — WASM Host Environment (C++ Integration)

- Integrate a lightweight WASM runtime (Wasmtime preferred for Component Model support; WAMR as fallback) into the FastFHIR C++ core as a conditional dependency (`FASTFHIR_ENABLE_EXTENSIONS` CMake flag).
- Configure the runtime for the Component Model (WIT bindings generated via `wit-bindgen`).
- The host must **never** expose the 8-byte atomic write head to the guest. The `extension-block` resource handle encodes a (base_offset, length) pair that is bounds-checked by the host intrinsics before every read. The arena write head lies at offset 0 and is always excluded from valid handle ranges.
- Implement AOT compilation of WASM modules at load time, storing the compiled artifact alongside the `.wasm` cache file.

## Phase 3 — Remote Registry & Fetching Logic

- The registry lookup key is `SHA-256(canonical_extension_url)` — a fixed 32-byte hash — to avoid long-string comparison overhead. Extension URLs are interned per-bundle (one hash per unique URL, not per extension element).
- Implement a two-level cache:
  - **Memory cache**: in-process `std::unordered_map<ArrayOf32Bytes, CompiledModule>` guarded by a shared_mutex.
  - **Disk cache**: platform cache directory (e.g., `~/.cache/fastfhir/modules/`), keyed by the hex-encoded SHA-256 of the URL.
- On cache miss, fetch from `https://registry.fastfhir.org/v1/modules/{hex_url_hash}.wasm`.
- Verify the downloaded artifact against a **module manifest signature** (SHA-256 of the `.wasm` bytes, signed by the fastfhir.org registry key) before loading. Reject modules that fail verification.
- **Offline fallback**: if the registry is unreachable or verification fails, preserve the raw `ExtensionData` in the arena unchanged and emit a structured warning via `FF_Logger`. Do NOT block ingestion. Record unresolved URLs in a per-stream `unresolved_extensions` list accessible via the Parser.

## Phase 4 — Batch Processing Engine

- The extension collector runs as a **post-ingest pass** — after the `FF_Queue` ingestor workers have finished writing the resource to the arena, but before compaction. The ingestor's `IngestTask` completion callback is the integration hook.
- Instead of invoking a WASM context switch per extension element, collect all `FF_ARRAY` extension entry offsets that share the same interned URL hash into a single `extension-batch` resource.
- Pass the entire batch to the WASM module in one guest call. The guest iterates using `read-child` intrinsics.
- Keep the dispatch table as an `std::unordered_map<ArrayOf32Bytes, CompiledModule*>` populated lazily at first encounter of each URL.

## Phase 5 — Thread Safety & Concurrency

- WASM runtime initialization is performed once under `std::call_once`. Module compilation is protected by per-URL `std::shared_mutex` (shared for reads of the compiled module, exclusive during compilation).
- Each worker thread that processes extensions holds its own instantiated WASM store (Wasmtime `Store`) to avoid cross-thread contention on the runtime.
- All host intrinsics provide **read-only** access to the arena. No host intrinsic may write to the arena or advance the write head. The Component Model `borrow<>` lifetime enforcer catches use-after-free at the WIT binding layer.

## Phase 6 — Code Generator Integration (`ffc.py`)

- The generator (`tools/generator/ffc.py`) already emits `STORE_FF_EXTENSION` / `SIZE_FF_EXTENSION` and ingest mappings for extension arrays. It must also emit:
  - A per-resource `dispatch_extensions(const BYTE* base, Offset ext_array_offset, ExtensionDispatcher& dispatcher)` stub that walks the extension `FF_ARRAY` and calls the dispatcher.
  - The same stub for `modifierExtension`, passing `is_modifier = true`.
- This ensures extension dispatch is zero-cost when `FASTFHIR_ENABLE_EXTENSIONS=OFF` (stubs compile to no-ops).

---

Please begin by writing the complete WIT interface file (`fastfhir-extension.wit`) for Phase 1, incorporating all `ChoiceEntry` discriminants, the `borrow<extension-block>` resource model, the batch handle, FHIR version parameter, and the `reject-resource` return variant.