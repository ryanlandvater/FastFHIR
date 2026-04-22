# FastFHIR System Architecture

## Maintenance Rule

Whenever a task changes the system architecture (components, data flow, binary layout, build topology, generated-code contracts, or public architectural invariants), update this file in the same change.

Architecture-affecting PRs are not complete until this document reflects the new design.

## Overview

FastFHIR is a **lock-free, zero-copy binary serializer** and **C++20 code generation pipeline** for HL7 FHIR resources (R4 and R5). It replaces traditional JSON/XML FHIR processing with a mathematically strict, offset-based binary layout that provides:

- **O(1) random access** to any deeply nested FHIR field via pure pointer arithmetic
- **Zero heap allocation** at parse time — a lightweight `Node` viewing lens passes directly over raw memory
- **Zero-copy parsing** — no deserialization phase, no varint unpacking, no DOM construction
- **Lock-free concurrent serialization** — multiple threads write to a single contiguous arena using a single atomic instruction per claim
- **Cryptographic sealing** — SHA-256/512 checksum footers embedded in the binary footer
- **In-stream lazy enrichment** — append new sub-resources (e.g., new lab results) without reparsing the rest of the bundle

**License:** FastFHIR Shared Source License (FF-SSL)  
**C++ Standard:** C++20  
**FHIR Versions Supported:** R4 (`0x0400`), R5 (`0x0500`)

---

## Repository Structure

```
FastFHIR/
├── include/                  # Hand-written public headers (the stable API surface)
│   ├── FastFHIR.hpp          # Single consumer-facing include; pulls in FieldKeys.hpp
│   ├── FF_Builder.hpp        # Lock-free concurrent stream builder
│   ├── FF_Parser.hpp         # Zero-copy parser + Reflective::Node
│   ├── FF_Ingestor.hpp       # JSON/HL7 → FastFHIR ingestion engine
│   ├── FF_Memory.hpp         # Virtual Memory Arena (VMA) handle/body
│   ├── FF_Primitives.hpp     # Core types, constants, enums, FF_HEADER, FF_ARRAY, FF_STRING
│   ├── FF_Utilities.hpp      # Endian-safe LOAD_*/STORE_* macros, architecture detection
│   ├── FF_Queue.hpp          # Lock-free power-of-2 FIFO queue
│   └── FF_Logger.hpp         # Lock-free concurrent text logger for ingestion warnings
├── src/                      # Hand-written implementation files
│   ├── FF_Builder.cpp
│   ├── FF_Ingestor.cpp
│   ├── FF_Memory.cpp
│   ├── FF_Parser.cpp
│   └── FF_Primitives.cpp
├── generated_src/            # Auto-generated C++ (output of code generation pipeline)
│   ├── FF_Recovery.hpp       # RECOVERY_TAG enum (all resource/type IDs)
│   ├── FF_FieldKeys.hpp/.cpp # Compiled O(1) field key constants + registry
│   ├── FF_DataTypes.hpp/.cpp # TypeTraits + Data structs for all universal FHIR types
│   ├── FF_CodeSystems.hpp    # C++ enums from FHIR ValueSets
│   ├── FF_AllTypes.hpp       # Master include: DataTypes + FieldKeys + Reflection
│   ├── FF_Reflection.hpp/.cpp# Runtime schema reflector (field enumeration, key lookup)
│   ├── FF_IngestMappings.hpp/.cpp # simdjson dispatch tables for JSON ingestion
│   ├── FF_Dictionary.hpp     # Forward declaration umbrella for R4/R5 dictionaries
│   ├── FF_R4_Dictionary.hpp/.cpp  # Enum↔string parse/serialize helpers for R4 ValueSets
│   ├── FF_R5_Dictionary.hpp/.cpp  # Enum↔string parse/serialize helpers for R5 ValueSets
│   ├── FF_Bundle.hpp/.cpp
│   ├── FF_DiagnosticReport.hpp/.cpp
│   ├── FF_Encounter.hpp/.cpp
│   ├── FF_Observation.hpp/.cpp
│   └── FF_Patient.hpp/.cpp
├── python/
│   ├── FF_PythonBindings.cpp # pybind11 module exposing Memory, Builder, Parser, Ingestor
│   └── fastfhir/__init__.py  # Python package init
├── tools/
│   ├── generator/            # Python code generation pipeline
│   │   ├── make_lib.py       # Orchestrator: fetch → dictionary → code systems → compile
│   │   ├── fetch_specs.py    # Downloads FHIR R4/R5 StructureDefinition bundles from HL7
│   │   ├── ffd.py            # Dictionary generator: ValueSets → C++ enums + helpers
│   │   ├── ffcs.py           # Code system generator: ValueSet annotations for code fields
│   │   └── ffc.py            # Main compiler: StructureDefinitions → C++ structs/vtables
│   ├── exporter/
│   │   └── FF_Export.cpp     # CLI tool: FastFHIR binary → minified FHIR JSON
│   └── ingestor/
│       └── FF_Ingest.cpp     # CLI tool: FHIR JSON / HL7v2 / HL7v3 → FastFHIR binary
├── build/                    # CMake out-of-tree build directory
└── CMakeLists.txt            # Build configuration
```

---

## Code Generation Pipeline

The code generation pipeline is the foundation of FastFHIR. It runs automatically during CMake configure when `FASTFHIR_RUN_GENERATOR=ON` (default).

### Pipeline Stages (`tools/generator/make_lib.py`)

```
1. fetch_specs.py   → Downloads HL7 definitions.json.zip for each FHIR version (R4, R5)
                      Extracts: profiles-resources.json, profiles-types.json, valuesets.json
                      Output: fhir_specs/R4/, fhir_specs/R5/

2. ffd.py           → Scans valuesets.json → Generates FF_R4_Dictionary.hpp/.cpp,
                      FF_R5_Dictionary.hpp/.cpp with enum classes and parse/serialize helpers.

3. ffcs.py          → Annotates code fields from StructureDefinitions with their
                      corresponding ValueSet enum types → code_enum_map dict.

4. ffc.py           → Main compiler pass:
                      - Parses StructureDefinitions for TARGET_TYPES and TARGET_RESOURCES
                      - Generates C++ vtable structs (FF_<RESOURCE>), Data structs (<Resource>Data),
                        View templates (RESOURCE_NAMEView<VERSION>), TypeTraits specializations,
                        FF_FieldKey constants, Reflection tables, and IngestMappings dispatch tables.
                      Output: generated_src/FF_*.hpp and FF_*.cpp

5. Cleanup          → Removes downloaded fhir_specs/ directory after successful generation
```

### Target Resources and Types

```python
TARGET_TYPES = [
    "Extension", "Coding", "CodeableConcept", "Quantity", "Identifier",
    "Range", "Period", "Reference", "Meta", "Narrative",
    "Annotation", "HumanName", "Address", "ContactPoint",
    "Attachment", "Ratio", "SampledData", "Duration",
    "Timing", "Dosage", "Signature", "CodeableReference", "VirtualServiceDetail"
]
TARGET_RESOURCES = [
    "Observation", "Patient", "Encounter", "DiagnosticReport", "Bundle"
]
```

### FHIR Type Mapping

| FHIR Type | C++ Storage | Wire Size | Notes |
|-----------|------------|-----------|-------|
| `boolean` | `uint8_t` | 1 | |
| `integer`, `unsignedInt`, `positiveInt` | `uint32_t` | 4 | |
| `decimal` | `double` | 8 | IEEE 754 with non-IEEE fallback |
| `code` | `uint32_t` (enum) | 4 | Encoded as enum ordinal; decoded via Dictionary |
| `string`, `id`, `uri`, `url`, `date`, etc. | `FF_STRING` offset | 8 | Offset into arena |
| Complex types / BackboneElement | `FF_<TYPE>` offset | 8 | Pointer offset into arena |
| `Resource` (polymorphic) | `ResourceReference` | 10 | 8-byte offset + 2-byte recovery tag |
| Choice (`[x]`) | `ChoiceEntry` | 10 | 8-byte raw_bits + 2-byte recovery tag |
| Arrays | `FF_ARRAY` offset | 8 | Offset to array header |

---

## Binary Format

All multi-byte integers are stored **little-endian**. Offsets are relative to the **arena base pointer** (8 bytes past the file start).

### File Layout

```
[FF_HEADER] (fixed size at offset 0)
[Payload Data Blocks]  (variable, written by Builder)
[FF_CHECKSUM footer]   (appended by Builder::finalize())
```

### `FF_HEADER`

| Field | Size | Description |
|-------|------|-------------|
| Magic | 4 bytes | `0x52484646` ("FFHR" LE) |
| FF Version | 2 bytes | Major.Minor library version |
| FHIR Version | 2 bytes | `0x0400` (R4) or `0x0500` (R5) |
| Checksum Algorithm | 2 bytes | `FF_Checksum_Algorithm` enum |
| Root Recovery Tag | 2 bytes | `RECOVERY_TAG` of the root resource |
| Root Offset | 8 bytes | Offset of the root resource block |

### `DATA_BLOCK` (vtable layout)

Every serialized FHIR resource or complex type starts with a fixed header (the vtable):

```
[0..7]   VALIDATION   — 8-byte canonical size sentinel for safe recovery
[8..9]   RECOVERY     — 2-byte RECOVERY_TAG identifying the struct type
[10+]    Field slots  — Fixed-size slots per field (8 bytes for offsets,
                        4 bytes for uint32/code, 1 byte for bool, etc.)
```

Field slot sizes are defined per-struct in `vtable_sizes` enums. Field slot offsets are in `vtable_offsets` enums. The vtable header is **version-aware**: each struct exposes a `get_header_size()` method that returns the correct size for R4 vs R5. Accessing a field beyond the current version's header size returns a null sentinel.

### `FF_ARRAY`

```
[0..7]   VALIDATION / count — upper 32 bits = count, lower 32 bits = element byte size or FF_ARRAY::OFFSET flag
[8..9]   RECOVERY_TAG       — semantic tag for the array element type
[10+]    Entries            — Packed entries (offsets or inline values)
```

### `FF_STRING`

```
[0..7]   VALIDATION — canonical size (= header_size + string byte length)
[8..9]   RECOVERY   — RECOVER_FF_STRING
[10+]    Raw UTF-8 bytes (NOT null-terminated in the binary)
```

### Recovery Tag System

`RECOVERY_TAG` is a `uint16_t` embedded in every block header. The high bit (`0x8000`) indicates an array. The low 15 bits identify the type. Tags are:

- `0x0001`–`0x0005`: Core primitives (HEADER, STRING, CODE, RESOURCE, CHECKSUM)
- `0x0100`–`0x010A`: Inline scalars (BOOL, INT32, UINT32, INT64, UINT64, FLOAT64, DATE, etc.)
- Higher values: Auto-generated for each resource and complex type

This enables safe polymorphic resolution and strict type checking at runtime without a separate schema lookup table.

---

## Memory Architecture — Virtual Memory Arena (VMA)

`FF_Memory` (handle) / `FF_Memory_t` (implementation) provide the core memory substrate.

### Design

- Backed by **POSIX `mmap`** (macOS/Linux) or **Win32 `VirtualAlloc`** (Windows)
- **Sparse allocation**: reserves a large virtual address space (default 4 GB) without consuming physical RAM until written
- Optionally **file-backed** (`createFromFile`) or **named shared memory** (`create(capacity, shm_name)`) for cross-process access
- `base()` returns the arena pointer (8 bytes past the mapping start, after the file header slot)

### Lock-Free Claiming

```cpp
uint64_t claim_space(size_t bytes);
```

Uses a single `std::atomic<uint64_t>` fetch-add to atomically reserve exclusive byte ranges. The high bit (`STREAM_LOCK_BIT = 1ULL << 63`) is used as a network ingestion lock. This makes concurrent multi-thread writes truly lock-free — each thread gets an exclusive slice of the arena in O(1) with no contention.

### `FF_Memory::View`

A lifetime-safe `string_view`-like wrapper that holds a `shared_ptr` reference to the underlying VMA core. This prevents use-after-free when passing the view to async sockets, cryptographic hashers, or database clients. Implicitly converts to `std::string_view` for drop-in compatibility.

### `FF_Memory::StreamHead`

An RAII guard returned by `try_acquire_stream()`. Provides exclusive ownership of the stream for framed network ingestion protocols, preventing partial writes from concurrent socket consumers.

---

## Core Library Components

### `FastFHIR::Builder` (`include/FF_Builder.hpp`)

The lock-free serialization engine. Non-copyable and non-movable.

**Key methods:**

| Method | Description |
|--------|-------------|
| `Builder(const Memory&, FHIR_VERSION)` | Binds to an existing VMA; writes the file header |
| `append<T>(data)` | Core template method: computes size via `TypeTraits<T>::size()`, claims space, writes via `TypeTraits<T>::store()`, returns `Offset` |
| `append_obj<T>(data)` | Wraps `append<T>` and returns a `ObjectHandle` for `[]` assignment |
| `append(vector<Offset>, RECOVERY_TAG)` | Overload for offset arrays with a semantic array tag |
| `amend_pointer(obj_offset, vtable_offset, new_target)` | Patches a field slot in an already-written block (for deferred pointer resolution) |
| `amend_resource(...)` | Patches a polymorphic resource slot |
| `amend_variant(...)` | Patches a choice (`[x]`) slot |
| `amend_scalar<T>(...)` | Patches an inline scalar slot |
| `set_root(handle)` | Writes root offset + recovery tag into the file header |
| `finalize(algo, hasher)` | Seals the file: bakes the header, invokes the hash callback, writes the checksum footer, returns `Memory::View` |
| `query()` | Returns a `Parser` snapshotting the current stream state for mid-build inspection |

**`ObjectHandle`** is a proxy returned by `append_obj`. It holds the `Builder*` and the written `Offset`. Its `operator[]` returns a `MutableEntry` for setting string/block fields by key after the initial write.

Reflective hierarchy notes:
- `Reflective::Entry` is the shared slot-coordinate base (`base`, `parent_offset`, `vtable_offset`, `target_recovery`, `kind`) for field traversal (~32B).
- `Reflective::MutableEntry` no longer inherits from `Entry`; it is a standalone thin handle storing 6 fields (`Builder*`, `base`, `parent_offset`, `vtable_offset`, `recovery`, `kind`), enabling lazy materialization and reducing embedded footprint.
- `Reflective::ObjectHandle` is a thin builder-bound handle (`Builder*`, `Offset`, `RECOVERY_TAG`) and no longer stores an embedded `Node` snapshot; read operations materialize a fresh `Node` on demand via `as_node()`.
- Public headers intentionally avoid top-level aliases like `FastFHIR::ObjectHandle` / `FastFHIR::MutableEntry` / `FastFHIR::Node`; callers should use `FastFHIR::Reflective::*` explicitly.
- **Lens-lightweighting pass (Phases 1–3):** 
  - **Phase 1:** Entry::vtable_offset narrowed to 32-bit storage; Node no longer carries a separate scalar-offset member; scalar decode paths reuse m_node_offset.
  - **Phase 2:** ObjectHandle refactored from Node subclass to thin 3-field handle (was ~64B, now ~24B).
  - **Phase 3:** MutableEntry refactored from Entry subclass to thin 6-field coordinate holder; forwarding methods delegate to as_handle() for lazy Node materialization.

**Mutation safety:** `try_begin_mutation()` / `end_mutation()` use an `atomic<uint64_t>` reference counter and an `atomic<bool> m_finalizing` flag to prevent appends after `finalize()` is called.

**Code Reuse via Delegation (Phase 4):** All read operations use a unified delegation chain to eliminate code duplication:
```
MutableEntry::is_array() ──→ as_handle().is_array()
ObjectHandle::is_array() ──→ as_node().is_array()
Node::is_array() ────────→ Direct bit-check on m_base

MutableEntry[key] ────────→ as_handle()[key]
ObjectHandle[key] ────────→ Direct coordinate lookup
MutableEntry[index] ──────→ as_handle()[index]
ObjectHandle[index] ──────→ Direct array geometry calculation
```
This pattern applies to all query methods (`size()`, `fields()`, `keys()`, `is_object()`, etc.), ensuring:
- No redundant traversal logic across types
- Lazy Node materialization only when needed
- Minimal per-instance overhead

### `FastFHIR::Parser` and `Reflective::Node` (`include/FF_Parser.hpp`)

Zero-copy read path. `Parser` is a value type; creation validates the file header.

**Key methods:**

| Method | Description |
|--------|-------------|
| `Parser(const void*, size_t)` | Binds to a raw byte buffer |
| `Parser(const Memory&)` | Binds to a VMA |
| `root()` | Returns the root `Reflective::Node` |
| `version()` | Returns the encoded version |
| `checksum()` | Returns `ChecksumValidation` struct for external hash verification |
| `print_json(ostream&)` | Streams the entire binary back as minified FHIR JSON |

**`Reflective::Node`** is a lightweight (non-owning) view over any value in the stream:

| Method | Description |
|--------|-------------|
| `operator[](FF_FieldKey)` | O(1) field access via compiled key |
| `operator[](string_view)` | Dynamic key lookup via reflection tables |
| `as<T>()` | Type-safe extraction; uses `TypeTraits<T>` |
| `as_string()`, `as_bool()`, `as_uint32()`, `as_float64()` | Primitive extractors |
| `is_empty()` | True if the node has no value (null sentinel) |

### `FastFHIR::Ingest::Ingestor` (`include/FF_Ingestor.hpp`)

The clinical data ingestion engine. **Requires simdjson.** Converts FHIR JSON (R4/R5), HL7 v2 (pipe-delimited ER7), or HL7 v3 (XML CDA) into a FastFHIR binary stream.

**Key methods:**

| Method | Description |
|--------|-------------|
| `Ingestor(logger_capacity, concurrency)` | Creates a pool of `simdjson::ondemand::parser` objects (one per thread) |
| `ingest(IngestRequest, out_root, out_count)` | Main entry: parses a full resource or bundle, writes into the provided `Builder` |
| `insert_at_field(parent_handle, FF_FieldKey, payload)` | Parses and inserts a sub-resource into a specific field of an existing object |
| `reset()` | Clears the logger and resets the fault flag; returns accumulated log strings |

Dispatch is performed by the auto-generated `FF_IngestMappings.cpp` which provides `dispatch_resource()` and `dispatch_block()` functions keyed by `resourceType` string and `RECOVERY_TAG` respectively.

### `FastFHIR::ConcurrentLogger` (`include/FF_Logger.hpp`)

A lock-free O(1) log buffer backed by a flat `unique_ptr<char[]>`. Uses `fetch_add` on an atomic head pointer for reservation. Has a configurable capacity (default 64 MB). Silently truncates if the buffer is exceeded.

### `FastFHIR::FIFO::Queue<T, CAPACITY>` (`include/FF_Queue.hpp`)

A power-of-2 capacity, lock-free (mostly) FIFO queue designed for passing `ObjectHandle`s between pipeline stages (e.g., from ingestor worker threads to a bundle assembler). Uses a node-registry with generation counters and atomic use-count tracking to safely retire nodes without locking.

---

## Generated Code Architecture

### `FF_Recovery.hpp`

Auto-generated `RECOVERY_TAG` enum covering every resource and complex type. Organized in blocks:
- `0x0001`–`0x00FF`: Core primitives
- `0x0100`–`0x01FF`: Inline scalars
- `0x8000` bit: Array flag (e.g., `RECOVER_FF_OBSERVATION | RECOVER_ARRAY_BIT` = array of observations)

Helper: `IsArrayTag()`, `GetTypeFromTag()`, `ToArrayTag()`.

### `FF_FieldKeys.hpp/.cpp`

Compiled `FF_FieldKey` constants for every field in every resource. Organized in namespaces:
- `FastFHIR::FieldKeys` — registry array + size
- `FastFHIR::Fields` — per-resource namespaces (e.g., `FastFHIR::Fields::OBSERVATION::STATUS`)

An `FF_FieldKey` encodes: the field name string, its vtable byte offset, its `FF_FieldKind`, and the parent resource's `RECOVERY_TAG`. This is what enables O(1) field access — the vtable byte offset is a compile-time constant.

### `FF_DataTypes.hpp/.cpp`

Contains:
- `TypeTraits<T>` specializations for all types (`std::string_view`, `ResourceReference`, every `*Data` struct, etc.)
- Each specialization provides: `recovery` (RECOVERY_TAG constant), `size(data, version)`, `store(base, offset, data, version)`
- Forward declarations for all `*Data` structs

### `FF_<Resource>.hpp/.cpp` (per resource)

Each generated resource file contains:

1. **`*Data` structs** — Plain C++ structs for deserialized data (heap-allocated, owning). Used for bulk writes. Example: `ObservationData`, `ObservationtriggeredByData`.

2. **`FF_<RESOURCE>` structs** (inheriting `DATA_BLOCK`) — Zero-copy views with:
   - `vtable_sizes` enum — byte size of each field slot
   - `vtable_offsets` enum — cumulative byte offset of each field slot
   - `get_header_size()` — returns correct header size for R4 vs R5
   - `validate_full()` — validates all offsets are within bounds
   - `deserialize()` — materializes a `*Data` struct from raw bytes
   - `find_field(string_view)` — finds a `FF_FieldInfo` by name

3. **`RESOURCE_NAMEView<VERSION>` templates** — Zero-overhead compile-time templated view structs with `get_<field>()` methods that do direct pointer arithmetic. No virtual dispatch. No allocations.

### `FF_CodeSystems.hpp`

C++ `enum class` types for every bounded FHIR code system (e.g., `ObservationStatus`, `HTTPVerb`, `TriggeredBytype`). Each has an `Unknown` sentinel.

### `FF_R4_Dictionary.hpp/.cpp` / `FF_R5_Dictionary.hpp/.cpp`

Parse and serialize helpers mapping between enum ordinals and their FHIR string representations. Generated from `valuesets.json` HL7 bundles.

### `FF_Reflection.hpp/.cpp`

Runtime schema reflection functions:
- `reflected_fields(recovery)` — returns `vector<FF_FieldInfo>` for a type
- `reflected_keys(recovery)` — returns `vector<string_view>` of field names
- `reflected_child_node(base, size, version, offset, recovery, key)` — returns a `Node` for a named child
- `reflected_resource_type(recovery)` — returns the FHIR `resourceType` string

### `FF_IngestMappings.hpp/.cpp`

Generated dispatch tables for `FF_Ingestor`:
- `dispatch_resource(resource_type, simdjson_obj, builder, logger)` — routes a `resourceType` string to the correct ingestor function
- `dispatch_block(expected_tag, simdjson_value, builder, logger)` — routes a complex sub-object by `RECOVERY_TAG`

---

## Build System

### CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `FASTFHIR_RUN_GENERATOR` | `ON` | Runs `make_lib.py` during CMake configure |
| `FASTFHIR_BUILD_SHARED` | `ON` | Builds as a shared library; `OFF` = static |
| `FASTFHIR_BUILD_INGESTOR` | `OFF` | Builds the Ingestor module (requires simdjson) |
| `FASTFHIR_BUILD_PYTHON_BINDINGS` | `OFF` | Builds pybind11 Python module (auto-enables Ingestor) |

### CMake Targets

| Target | Type | Description |
|--------|------|-------------|
| `fastfhir_obj` | OBJECT library | Core + generated sources; used as input for all downstream targets |
| `fastfhir` / `FastFHIR::fastfhir` | SHARED or STATIC | Main consumer library |
| `fastfhir_ingest_obj` | OBJECT library | `FF_Ingestor.cpp` + `FF_IngestMappings.cpp`; links simdjson |
| `fastfhir_ingestor` / `FastFHIR::ingestor` | SHARED or STATIC | Ingestor library; links `fastfhir` + simdjson |
| `fastfhir_python` | MODULE | pybind11 Python extension; links `fastfhir_ingestor` |
| `ff_export` | EXECUTABLE | CLI: `.ffhir` → FHIR JSON |
| `ff_ingest` | EXECUTABLE | CLI: FHIR JSON / HL7v2 / HL7v3 → `.ffhir`; links OpenSSL |
| `fastfhir_generate` | CUSTOM_TARGET | Re-runs the Python generator on demand |

### Dependencies

- **simdjson** (v3.9.5): Required for Ingestor. Auto-fetched via `FetchContent` if not found locally.
- **OpenSSL**: Required for `ff_ingest` CLI (SHA-256 sealing). Auto-built via `ExternalProject` if not found.
- **pybind11**: Required for Python bindings. Fetched via `FetchContent`.
- **Python3 Interpreter**: Required at configure time when `FASTFHIR_RUN_GENERATOR=ON`.

### Precompiled Headers

`fastfhir_obj` uses PCH for `<vector>`, `<string>`, `<string_view>`, `<cstdint>`, `<memory>`, and `FF_Primitives.hpp`.

---

## Python Bindings (`python/FF_PythonBindings.cpp`)

Exposes the full FastFHIR API to Python via **pybind11**. Key wrapper types:

| Python Type | Wraps | Notes |
|-------------|-------|-------|
| `PyMemory` | `FF_Memory` + `shared_ptr` | Supports `close()` to prevent Jupyter SIGBUS crashes |
| `PyStream` | `Builder` + `shared_ptr` | Carries shared ownership for thread safety |
| `PyStreamNode` | `ObjectHandle` + `shared_ptr<Builder>` | Extends Builder lifetime through handle |
| `PyMutableEntry` | `MutableEntry` + `shared_ptr<Builder>` | Same lifetime guarantee for field assignment |

Python `fields.py` / `fields/` in `generated_src/python/` provides the compiled field key constants for use in Python. The `PythonFieldProxy` struct bridges the Python field registry to the C++ `FF_FieldKey` system.

---

## CLI Tools

### `ff_ingest` (`tools/ingestor/FF_Ingest.cpp`)

```
Usage: ff_ingest [input | -] [-o output.ff]
```

- Auto-detects format by sniffing the first 512 bytes: `{` → FHIR JSON; `MSH|` → HL7v2; `<...hl7-org:v3...>` → HL7v3; `<...hl7.org/fhir...>` → FHIR XML
- Reads from stdin (`-`) or a file path
- Writes to a file (`-o`) or stdout (for pipeline chaining)
- Seals with SHA-256 via OpenSSL EVP

**Example pipeline:**
```sh
curl -s "https://s3.aws.com/hospital/bundle.json.gz" | gzip -d | ./ff_ingest | next_tool
```

### `ff_export` (`tools/exporter/FF_Export.cpp`)

Reads a `.ffhir` binary file (memory-mapped via POSIX `mmap` / Win32 `MapViewOfFile`), parses it with `FastFHIR::Parser`, and streams minified FHIR JSON to stdout or a file via `Parser::print_json()`.

---

## Type System Summary

### `FF_FieldKind` (in `FF_Primitives.hpp`)

Discriminant enum used by `Node` and `FF_FieldKey` to identify the storage class of a field:
`FF_FIELD_STRING`, `FF_FIELD_ARRAY`, `FF_FIELD_BLOCK`, `FF_FIELD_CODE`, `FF_FIELD_BOOL`, `FF_FIELD_INT32`, `FF_FIELD_UINT32`, `FF_FIELD_INT64`, `FF_FIELD_UINT64`, `FF_FIELD_FLOAT64`, `FF_FIELD_RESOURCE`, `FF_FIELD_CHOICE` (for `[x]` fields).

### `FF_Result`

Simple result type with `FF_Result_Code` (`FF_SUCCESS`, `FF_FAILURE`, `FF_VALIDATION_FAILURE`, `FF_WARNING`) and a `std::string message`. Implicitly convertible to `bool`.

### Null Sentinels

All null values use max-value sentinels: `FF_NULL_UINT8 = 0xFF`, `FF_NULL_UINT32 = 0xFFFFFFFF`, `FF_NULL_OFFSET = 0xFFFFFFFFFFFFFFFF`. This allows null checks without branching on a separate flag byte.

### `FF_CUSTOM_STRING_FLAG = 0x80000000`

High bit of a 4-byte code field slot. When set, the remaining 31 bits encode a custom string offset rather than an enum ordinal. Used for code values not covered by a known ValueSet.

---

## Key Design Invariants

1. **All offsets are relative to `Memory::base()`**, not the raw mapping start. The file header occupies the first 8 bytes of the mapping and is excluded from the arena.
2. **Vtable field access is pure pointer arithmetic**: `LOAD_U64(base + object_offset + FF_OBSERVATION::STATUS)` — no hash lookups, no virtuals.
3. **RECOVERY_TAG is always at bytes 8–9 of every block**. This is the universal recovery anchor for safe polymorphic resolution.
4. **The VALIDATION field (bytes 0–7) of every block stores the canonical total size**. Reading this before dereferencing any pointer prevents buffer overruns.
5. **Strings are never null-terminated in the binary**. `FF_STRING::read_view()` returns a `string_view` over the raw bytes.
6. **The generator is the source of truth**. Do not manually edit `generated_src/`. Re-run `make_lib.py` to regenerate from HL7 specs.
7. **Python bindings carry `shared_ptr<Builder>` through all node handles** to prevent use-after-free in Jupyter/interactive contexts.
