
FastFHIR
===========

**FastFHIR** is a wildly fast, lock-free binary serializer and C++20 code generation pipeline for HL7 FHIR resources (R4 and R5).

Healthcare interoperability has historically relied on formats that are inherently unsafe, computationally expensive, or structurally brittle. FastFHIR replaces traditional parsing with a mathematically strict, offset-based binary layout that guarantees safety, in-stream HL7 enrichment, and blistering speed. It generates strongly-typed C++ structs and a mathematically strict, zero-copy binary architecture directly from official HL7 FHIR Structure Definitions.

A Python binding is available under [`python/`](python/) — see [`python/README.md`](python/README.md) for Python API examples.

## Why FastFHIR?

### 1. Extreme Performance
FastFHIR turns data traversal into pure pointer arithmetic, fundamentally outpacing both legacy text formats and modern serialized binaries.
* **O(1) Random Access:** Jump instantly to any deeply nested FHIR field — completely bypassing the O(N) linear scanning of HL7v2 and the O(N) string-hashing and DOM construction of JSON.
* **Zero-Heap Allocation:** Reading a FastFHIR stream requires 0 heap allocations. A lightweight `Node` viewing lens is passed directly over the raw memory buffer, enabling nanosecond read times from the instant the message hits RAM.
* **Zero-Copy Engine:** FastFHIR outperforms even Google Research's Protobuf FHIR implementation because it skips the deserialization phase entirely, operating natively at the absolute memory-bandwidth limit without unpacking varints or allocating C++ message objects.

### 2. Hardware-Level Safety & Security
Legacy text standards expose systems to XML injection (CDA) and heap fragmentation (JSON). FastFHIR guarantees deterministic memory and structural integrity.
* **OS-Protected Memory:** By utilizing Virtual Memory Arenas (via POSIX `mmap` or Win32 `VirtualAlloc`), FastFHIR ensures pointers remain perfectly stable and memory access is protected by the OS kernel.
* **Strict Schema Validation:** The binary layout embeds explicit `RECOVERY_TAG` metadata for every object. This provides guaranteed safe polymorphic resolution and strict C++ type checking at runtime, preventing incorrect information context, garbage reads, and buffer overflows.
* **Cryptographic Sealing:** Built-in checksum footers (CRC32/MD5/SHA-256/SHA-512) guarantee record immutability, providing a hardware-verified security layer for clinical data lakes.

### 3. Clinical Informatics: Lock-Free Enrichment
* **In-Stream Lazy Enrichment:** Read a `Patient.id` or route a payload in nanoseconds without parsing the other 9,999 fields in a `Bundle`. You only pay for the exact bytes you traverse. Append a new laboratory result for a patient without touching any other byte in the record — simply add the new result and reseal before passing the message downstream.
* **Concurrent Mutex-Free Generation:** Serialize thousands of resources simultaneously across a thread pool. FastFHIR's atomic pointer-patching architecture allows surgical data appends (like NLP annotations) into a single contiguous stream without a single lock.

### 4. Developer Ergonomics
You do not have to sacrifice a clean API for bare-metal performance.
* **IDE-Friendly Static Keys:** Zero-overhead, compiled O(1) field keys (e.g., `FastFHIR::FieldKeys::FF_STATUS`) completely bypass runtime string hashing.
* **Polymorphic Type Safety:** Extract primitives or complex structs with strict runtime schema validation using `node.as<PatientData>()` or `node.as<std::string_view>()`.
* **JSON-Style Traversal:** Walk complex trees using native C++ `[]` operators (e.g., `root[FF_NAME][0]`).

---

# API Examples

## 1 — Ingest a FHIR JSON file and save as `.ffhr`

The arena is memory-mapped directly to the destination file. Every byte the ingestor
writes goes straight to the OS page cache — no intermediate buffer, no `write()` call,
and no copy at `finalize()`. When `finalize()` returns, `patient.ffhr` is a complete,
sealed FastFHIR archive on disk.

```cpp
#include <FastFHIR.hpp>
#include <openssl/sha.h>
using namespace FastFHIR::FieldKeys;

// Map the arena straight to a file — every write goes directly to disk
auto mem = FastFHIR::Memory::createFromFile("patient.ffhr", 64 * 1024 * 1024);

FastFHIR::Builder          builder(mem, FHIR_VERSION_R5);
FastFHIR::Ingest::Ingestor ingestor;

std::string json_string = /* read patient.json */;

FastFHIR::Reflective::ObjectHandle patient_handle;
size_t parsed_count = 0;
ingestor.ingest({builder, FastFHIR::Ingest::SourceType::FHIR_JSON, json_string},
                patient_handle, parsed_count);

// Inspect while the stream is still open — zero heap allocations
auto root = FastFHIR::Parser(mem).root();
auto id     = root[FF_ID].as<std::string_view>();       // "patient-1"
auto gender = root[FF_GENDER].as<std::string_view>();   // "male"
auto active = root[FF_ACTIVE].as<bool>();               // true

// Walk the name array
for (auto& name_entry : root[FF_NAME].entries()) {
    auto family = name_entry[FF_FAMILY].as<std::string_view>();
    for (auto& given_entry : name_entry[FF_GIVEN].entries())
        std::cout << given_entry.as<std::string_view>() << " ";
    std::cout << family << "\n";
}

// Seal with a SHA-256 footer — writes header + hash directly into the mapped pages
builder.set_root(patient_handle);
builder.finalize(FF_CHECKSUM_SHA256, [](const unsigned char* data, size_t len) {
    std::vector<uint8_t> hash(SHA256_DIGEST_LENGTH);
    SHA256(data, len, hash.data());
    return hash;
});
// patient.ffhr is now a valid, portable FastFHIR archive
```

---

## 2 — Open and read a `.ffhr` file

Mount an existing archive and traverse directly via `Parser::root()`.

```cpp
#include <FastFHIR.hpp>
using namespace FastFHIR::FieldKeys;

auto mem    = FastFHIR::Memory::createFromFile("patient.ffhr", 64 * 1024 * 1024);
auto parser = FastFHIR::Parser(mem);
auto root   = parser.root();

// Scalars coerce directly to C++ types — zero heap allocations
auto id     = root[FF_ID].as<std::string_view>();         // std::string_view
auto gender = root[FF_GENDER].as<std::string_view>();     // std::string_view
auto active = root[FF_ACTIVE].as<bool>();                  // bool
auto dob    = root[FF_BIRTH_DATE].as<std::string_view>(); // std::string_view

// Walk structured arrays
for (auto& name_node : root[FF_NAME].entries()) {
    auto family = name_node[FF_FAMILY].as<std::string_view>();
    for (auto& g : name_node[FF_GIVEN].entries())
        std::cout << g.as<std::string_view>() << " ";
    std::cout << family << "\n";
}

// Eagerly materialize into a generated C++ struct (strict schema validation)
auto patient_data = root.as<FastFHIR::PatientData>();
```

---

## 3 — Re-open a `.ffhr` file and enrich it in place

FastFHIR's arena is append-only and memory-mapped. Writing new fields appends new bytes
to the tail and amends only the field pointers in the header — the original record bytes
are never touched. The OS page cache flushes only the dirty pages (new tail + updated
pointers). The file grows solely by the delta; no copy of existing data is ever made.

```cpp
#include <FastFHIR.hpp>
#include <openssl/sha.h>
using namespace FastFHIR::FieldKeys;

auto mem = FastFHIR::Memory::createFromFile("patient.ffhr", 64 * 1024 * 1024);

FastFHIR::Builder          builder(mem, FHIR_VERSION_R5);
FastFHIR::Ingest::Ingestor ingestor;

// Obtain a mutable handle to the existing root
auto patient = builder.snapshot().root_handle();

// Append or overwrite scalar fields (new bytes appended; field pointer amended)
patient[FF_BIRTH_DATE] = "1990-03-21";
patient[FF_ACTIVE]     = true;

// Append a structured sub-object via the ingestor
ingestor.insert_at_field(patient, FF_TELECOM,
    R"({"system":"phone","value":"555-0199","use":"mobile"})");

// Re-seal with an updated checksum — original data untouched, new tail written
builder.finalize(FF_CHECKSUM_SHA256, [](const unsigned char* data, size_t len) {
    std::vector<uint8_t> hash(SHA256_DIGEST_LENGTH);
    SHA256(data, len, hash.data());
    return hash;
});
// patient.ffhr now contains the enriched record
```

---

## 4 — Receive over a socket, enrich, and send back

This example uses standalone Asio so the same socket flow is portable across
Windows, macOS, and Linux.

The OS writes network bytes directly into the arena — zero copies on ingest.
After enrichment, `finalize()` returns a `Memory::View` that the socket layer
reads straight from the same arena pages — zero copies on egress.

`tests/cpp/test_readme.cpp` validates this path end-to-end with an in-process
loopback TCP transport.

```cpp
#include <FastFHIR.hpp>
#include <asio.hpp>
using namespace FastFHIR::FieldKeys;

auto mem = FastFHIR::Memory::create(256 * 1024 * 1024);   // 256 MB anonymous arena

FastFHIR::Ingest::Ingestor ingestor;

asio::io_context io;
asio::ip::tcp::socket conn(io);
// conn is assumed to be connected (acceptor/connect bootstrap omitted here)

// ── Step 1: receive FHIR JSON directly into the arena (zero-copy ingest) ──
std::string raw_json;
{
    auto head = mem.try_acquire_stream();   // exclusive stream lock
    if (!head) throw std::runtime_error("stream busy");

    std::array<char, 65536> buf{};
    size_t n = conn.read_some(asio::buffer(buf));
    head->commit(n);
    raw_json.assign(buf.data(), n);
}

// ── Step 2: ingest and enrich ──
auto mem2 = FastFHIR::Memory::create(256 * 1024 * 1024);
FastFHIR::Builder builder(mem2, FHIR_VERSION_R5);
FastFHIR::Reflective::ObjectHandle patient_handle;
size_t count = 0;
ingestor.ingest({builder, FastFHIR::Ingest::SourceType::FHIR_JSON, raw_json},
                patient_handle, count);

patient_handle[FF_ACTIVE]  = true;
ingestor.insert_at_field(patient_handle, FF_TELECOM,
    R"({"system":"phone","value":"555-0199","use":"mobile"})");

// ── Step 3: seal and send back — view reads straight from the arena ──
builder.set_root(patient_handle);
auto view = builder.finalize(FF_CHECKSUM_CRC32);

asio::write(conn, asio::buffer(view.data(), view.size())); // zero-copy egress
```

---

## 5 — Surgically edit one patient in a 5 GB bundle and reseal

The bundle is memory-mapped — the OS pages only the entries you actually touch.
Finding one patient, appending a lab result, and resealing never loads the other
5 GB into RAM. Only the dirty pages (new Observation tail + updated pointers)
are ever written back to disk.

```cpp
#include <FastFHIR.hpp>
#include <openssl/sha.h>
using namespace FastFHIR::FieldKeys;

// Map the entire bundle — address space reserved, pages not loaded until accessed
auto mem = FastFHIR::Memory::createFromFile("bundle.ffhr", 8ULL * 1024 * 1024 * 1024);

FastFHIR::Builder          builder(mem, FHIR_VERSION_R5);
FastFHIR::Ingest::Ingestor ingestor;

auto parser = FastFHIR::Parser(mem);
auto bundle = parser.root();

// Walk bundle.entry; the OS faults in only the pages we read
FastFHIR::Reflective::ObjectHandle target_patient;
for (auto& entry_node : bundle[FF_ENTRY].entries()) {
    auto resource = entry_node[FF_RESOURCE];
    if (!resource) continue;
    if (resource.recovery() != RECOVERY_TAG::Patient) continue;
    if (resource[FF_ID].as<std::string_view>() == "patient-42") {
        target_patient = builder.mutable_handle(resource);
        break;
    }
}

if (!target_patient) throw std::runtime_error("patient-42 not found");

// Append a new Observation — every other entry in the bundle is untouched
FastFHIR::Reflective::ObjectHandle obs_handle;
size_t count = 0;
ingestor.ingest({builder, FastFHIR::Ingest::SourceType::FHIR_JSON, R"({
    "resourceType": "Observation",
    "status": "final",
    "code": {"coding": [{"system": "http://loinc.org", "code": "2345-7", "display": "Glucose"}]},
    "subject": {"reference": "Patient/patient-42"},
    "valueQuantity": {"value": 94.0, "unit": "mg/dL", "system": "http://unitsofmeasure.org"}
})"}, obs_handle, count);

// Amend the patient record — only this entry's pages are dirtied
target_patient[FF_TELECOM] = /* ... */;

// Reseal — rewrites only the header + checksum pages, nothing else
builder.set_root(builder.mutable_handle(bundle));
builder.finalize(FF_CHECKSUM_SHA256, [](const unsigned char* data, size_t len) {
    std::vector<uint8_t> hash(SHA256_DIGEST_LENGTH);
    SHA256(data, len, hash.data());
    return hash;
});
// bundle.ffhr updated; 5 GB of untouched entries were never copied
```

---

## 6 — Lock-Free Concurrent Generation

FastFHIR's Builder uses OS-level memory mapping and atomic offsets to allow massive
thread pools to write to a single contiguous binary stream simultaneously, without
any mutexes.

```cpp
#include <FastFHIR.hpp>
#include <thread>
#include <vector>

auto mem = FastFHIR::Memory::create(2ULL * 1024 * 1024 * 1024);  // 2 GB Virtual Arena
FastFHIR::Builder builder(mem, FHIR_VERSION_R5);

std::vector<std::thread> pool;

// 32 threads simultaneously serializing clinical data into the same stream
for (int i = 0; i < 32; ++i) {
    pool.emplace_back([&builder, i]() {
        FastFHIR::ObservationData obs;
        obs.status = "preliminary";

        // Single atomic claim — no mutex, no heap allocation, no pointer invalidation
        auto handle = builder.append_obj(obs);

        // Push handle to a lock-free queue to link into a Bundle later
    });
}

for (auto& t : pool) t.join();
```

---

# Reference

### Field Keys — `FastFHIR::FieldKeys::FF_<FIELD>`

All FHIR fields are generated as `constexpr FF_FieldKey` constants in the `FastFHIR::FieldKeys` namespace. Use them with `node[key]` for O(1) field access without any runtime string hashing.

```cpp
using namespace FastFHIR::FieldKeys;

FF_ID          // "id"          (scalar)
FF_ACTIVE      // "active"      (bool)
FF_GENDER      // "gender"      (code → string_view)
FF_BIRTH_DATE  // "birthDate"   (string_view)
FF_NAME        // "name"        (array of HumanName)
FF_FAMILY      // "family"      (string_view)
FF_GIVEN       // "given"       (array of string_view)
FF_TELECOM     // "telecom"     (array of ContactPoint)
FF_ADDRESS     // "address"     (array of Address)
FF_STATUS      // "status"      (code → string_view)
FF_CODE        // "code"        (CodeableConcept block)
FF_CODING      // "coding"      (array of Coding)
FF_SYSTEM      // "system"      (string_view)
FF_ENTRY       // "entry"       (bundle entry array)
FF_RESOURCE    // "resource"    (polymorphic resource)
```

### Checksum Algorithms

| Constant | Description |
|---|---|
| `FF_CHECKSUM_NONE` | No footer |
| `FF_CHECKSUM_CRC32` | CRC-32 |
| `FF_CHECKSUM_MD5` | MD5 |
| `FF_CHECKSUM_SHA256` | SHA-256 |

The `FF_HEADER` checksum offset points to an `FF_CHECKSUM` block containing the selected algorithm and a zero-copy `std::string_view` over the raw hash bytes. Header validation checks the checksum block when present.

### FHIR Versions

```cpp
FHIR_VERSION_R4   // HL7 FHIR R4
FHIR_VERSION_R5   // HL7 FHIR R5 (default)
```

---

# Quick Start

## Prerequisites
* Python 3.9+ (generator only)
* Clang, GCC, or MSVC with C++20 support
* CMake 3.20+
* Network access (generator fetches FHIR bundles from HL7)

## Build

FastFHIR includes a root `CMakeLists.txt`. Configure and build:

```bash
cmake -S . -B build
cmake --build build -j
```

This will:
1. Download and extract FHIR specs (R4/R5)
2. Build the master dictionary (`generated_src/FF_Dictionary.hpp`)
3. Build code system enums (`generated_src/FF_CodeSystems.hpp`)
4. Generate data type and resource structs under `generated_src/`
5. Clean temporary `fhir_specs/` downloads

By default CMake runs the generator during configure (`FASTFHIR_RUN_GENERATOR=ON`). Generated `.cpp` files are discovered from `generated_src/` and compiled into `libfastfhir.a`.

To skip generation if you have already run it manually:

```bash
python3 tools/generator/make_lib.py
cmake -S . -B build -DFASTFHIR_RUN_GENERATOR=OFF
cmake --build build -j
```

### Validate generated output

```bash
clang++ -c generated_src/FF_DataTypes.cpp -I. -std=c++20
clang++ -c generated_src/FF_Patient.cpp   -I. -std=c++20
clang++ -c generated_src/FF_Observation.cpp -I. -std=c++20
```

### Generator Architecture

The generator lives in `tools/generator/` and is split by responsibility:

| File | Purpose |
|---|---|
| `fetch_specs.py` | Downloads and extracts required FHIR bundles |
| `ffd.py` | Builds dictionary artifacts |
| `ffcs.py` | Builds code-system enums and mapping metadata |
| `ffc.py` | Emits C++ data/resource structs and read/write logic |
| `make_lib.py` | Orchestrates the full generation pipeline |

### Design Notes
* Resource and datatype structs are generated from official HL7 StructureDefinitions.
* Version-specific fields are guarded by generated version checks.
* The top-level file container is `FF_HEADER`, which stores file magic, version, checksum offset, root offset, and payload size.
* `Parser::root()` and `Node` traversal use offset/recovery checks to validate data block integrity.
* Full recursive validation is available via typed APIs such as `node.as<PatientData>()`.

---

# License
This project is licensed under the FastFHIR Shared Source License (FF-SSL) v1.2, which is based on the Apache License 2.0 with additional restrictions:

You may not modify or redistribute altered versions of the core implementation (see LICENSE for details).

Strict attribution to Dr. Ryan Erik Landvater and the FastFHIR project is required in all products, services, or derivative works.

You may not re-brand, rename, or claim authorship of the core implementation.

The "right to repair" clause allows minimal patching only if the author fails to address critical bugs or vulnerabilities within 30 business days of notification.

See the LICENSE file for the full legal text and compliance requirements.

---
**Attribution**: The design of FastFHIR is based upon the [Iris File Extension](https://www.sciencedirect.com/science/article/pii/S2153353925000471) (by Ryan Landvater) and [FlatBuffers](https://github.com/google/flatbuffers) (by Wouter van Oortmerssen and the Google Fun Propulsion Labs team).