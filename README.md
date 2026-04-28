
FastFHIR
===========

[![Release](https://img.shields.io/github/v/release/RyanLandvater/FastFHIR?label=version&color=blue)](https://github.com/RyanLandvater/FastFHIR/releases)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)
![FHIR R4/R5](https://img.shields.io/badge/FHIR-R4%20%7C%20R5-blueviolet)
![License](https://img.shields.io/badge/license-FF--SSL-orange)

**FastFHIR** is a **wildly fast binary HL7 format** comprising a lock-free binary serializer and C++20 code generation pipeline targeting the HL7 FHIR resources (R4 and R5).

Healthcare interoperability has historically relied on formats that are **inherently unsafe**, **computationally expensive**, and **structurally brittle**. FastFHIR replaces traditional parsing with a mathematically strict, offset-based binary layout that guarantees tye safety, data recovery, in-stream HL7 enrichment, and blistering speed. It generates strongly-typed C++ structs and a mathematically strict, zero-copy binary architecture directly from official HL7 FHIR Structure Definitions.

A Python binding is available — see [`The Python Readme`](python/README.md) for Python API examples.

---

## Table of Contents

- [Why FastFHIR?](#why-fastfhir)
- [Quick Start](#quick-start)
  - [Prerequisites](#prerequisites)
  - [Build](#build)
  - [Production Profile](#production-profile)
- [Getting Started](#getting-started)
  - [Step 1 — Parse raw bytes](#step-1--parse-raw-bytes)
  - [Step 2 — Create a Memory arena](#step-2--create-a-memory-arena)
  - [Step 3 — Build a FastFHIR record from FHIR JSON](#step-3--build-a-fastfhir-record-from-fhir-json)
- [API Examples](#api-examples)
  - [1 — Ingest a FHIR JSON file and save as `.ffhr`](#1--ingest-a-fhir-json-file-and-save-as-ffhr)
  - [2 — Open and read a `.ffhr` file](#2--open-and-read-a-ffhr-file)
  - [3 — Re-open a `.ffhr` file and enrich it in place](#3--re-open-a-ffhr-file-and-enrich-it-in-place)
  - [4 — Receive over a socket, enrich, and send back](#4--receive-over-a-socket-enrich-and-send-back)
  - [5 — Surgically edit one patient in a 5 GB bundle and reseal](#5--surgically-edit-one-patient-in-a-5-gb-bundle-and-reseal)
  - [6 — Lock-Free Concurrent Generation](#6--lock-free-concurrent-generation)
  - [7 — Compact Archives](#7--compact-archives)
- [Reference](#reference)
  - [Typed Resource Keys](#typed-resource-keys--fastfhirfieldsresourcefield)
  - [Checksum Algorithms](#checksum-algorithms)
  - [FHIR Versions](#fhir-versions)
- [Generator Architecture](#generator-architecture)
- [Design Notes](#design-notes)
- [License](#license)

---

## Why FastFHIR?

### 1. Extreme Performance
FastFHIR turns data traversal into pure pointer arithmetic, **fundamentally outpacing both legacy text formats and modern serialized binaries**.
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
* **IDE-Friendly Static Keys:** Zero-overhead, compiled O(1) typed keys (e.g., `FastFHIR::Fields::PATIENT::ACTIVE`) completely bypass runtime string hashing.
* **Polymorphic Type Safety:** Assign fields directly to C++ types with zero-overhead implicit conversion (e.g., `std::string_view id = node[FastFHIR::Fields::PATIENT::ID]`), or eagerly materialize an entire struct (`PatientData patient = parser.root()`).
* **JSON-Style Traversal:** Walk complex trees using native C++ `[]` operators (e.g., `root[FastFHIR::Fields::PATIENT::NAME][0]`).

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

### Production Profile

The generator selects a curated set of FHIR resources based on a *production profile*. The profile is controlled by the CMake cache variable `FASTFHIR_PRODUCTION_PROFILE` (default: `us`) or the environment variable `FASTFHIR_PRODUCTION_PROFILE`.

| Profile | Value | Resources |
|---------|-------|----------|
| US Core (default) | `us` | 27 resources matching the US Core IG (Epic MyChart, etc.) |
| UK Core | `uk` | 22 resources matching the UK Core IG (NHS) |
| All resources | `all` | All concrete FHIR resources discovered from HL7 bundles |

To build with a different profile at configure time:

```bash
cmake -S . -B build -DFASTFHIR_PRODUCTION_PROFILE=uk
cmake --build build -j
```

Or via environment variable when running the generator directly:

```bash
FASTFHIR_PRODUCTION_PROFILE=all python tools/generator/make_lib.py
```

The `fastfhir_generate` custom CMake target can be re-run manually at any time to regenerate sources with the current profile. By default, sources are only regenerated during CMake configure to avoid invalidating the precompiled header on every build. Set `FASTFHIR_GENERATE_ON_BUILD=ON` to rerun the generator before every build.

### Version injection

The library embeds `FASTFHIR_VERSION_MAJOR`, `FASTFHIR_VERSION_MINOR`, and `FASTFHIR_VERSION_BUILD` macros (available via `<FastFHIR.hpp>` as `FASTFHIR_VERSION_STRING`). CMake reads the values from environment variables of the same name before falling back to the version declared in `CMakeLists.txt`. This makes CI version stamping from a GitHub tag straightforward:

```yaml
# .github/workflows/release.yml (excerpt)
- name: Set version from tag
  run: |
    TAG="${GITHUB_REF_NAME#v}"          # strip leading 'v' from e.g. v1.2.3
    IFS='.' read MAJOR MINOR BUILD <<< "$TAG"
    echo "FASTFHIR_VERSION_MAJOR=$MAJOR" >> $GITHUB_ENV
    echo "FASTFHIR_VERSION_MINOR=$MINOR" >> $GITHUB_ENV
    echo "FASTFHIR_VERSION_BUILD=$BUILD" >> $GITHUB_ENV
```

The same env vars are consumed by the Python bindings (`fastfhir.__version__`) and any other tool that reads them during the build.

This will:
1. Download and extract FHIR specs (R4/R5)
2. Build the master dictionary (`generated_src/FF_Dictionary.hpp`)
3. Build code system enums (`generated_src/FF_CodeSystems.hpp`)
4. Generate data type and resource structs under `generated_src/`
5. Clean temporary `fhir_specs/` downloads

By default CMake runs the generator during configure (`FASTFHIR_RUN_GENERATOR=ON`). Generated `.cpp` files are discovered from `generated_src/` and compiled into `libfastfhir.a`.

---

# Getting Started

These three short steps walk you from zero to a fully-functioning FastFHIR workflow.
Start at whichever step matches your use-case — you do not have to use all three together.

All example code below is validated in [tests/cpp/test_readme.cpp](tests/cpp/test_readme.cpp) for reference.

---

## Step 1 — Parse raw bytes

`Parser` is the read-only entry point. Below is a minimal file-based path that opens an
existing `.ffhr` archive in read-only mode, loads it into memory, and prints fields only
when they are present.

```cpp
#include <FastFHIR.hpp>
#include <cstdio>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

static std::vector<uint8_t> open_read_only_file(const char* path) {
    std::FILE* fp = std::fopen(path, "rb");
    if (!fp) throw std::runtime_error("failed to open .ffhr file");

    if (std::fseek(fp, 0, SEEK_END) != 0) {
        std::fclose(fp);
        throw std::runtime_error("failed to seek file end");
    }

    long file_size = std::ftell(fp);
    if (file_size <= 0) {
        std::fclose(fp);
        throw std::runtime_error("empty or unreadable file");
    }
    std::rewind(fp);

    std::vector<uint8_t> raw_bytes(static_cast<size_t>(file_size));
    size_t bytes_read = std::fread(raw_bytes.data(), 1, raw_bytes.size(), fp);
    std::fclose(fp);

    if (bytes_read != raw_bytes.size())
        throw std::runtime_error("short read while loading .ffhr file");

    return raw_bytes;
}

auto raw_bytes = open_read_only_file("patient.ffhr");
// Bind the parser — validates the header immediately, zero heap allocations.
FastFHIR::Parser parser(raw_bytes.data(), raw_bytes.size());

// Access the root node.
auto root = parser.root();

// Read scalar fields only if present.
if (auto id_node = root[FastFHIR::Fields::PATIENT::ID])
    std::cout << "id=" << std::string_view(id_node) << "\n";
if (auto active_node = root[FastFHIR::Fields::PATIENT::ACTIVE])
    std::cout << "active=" << std::boolalpha << active_node.as<bool>() << "\n";
if (auto gender_node = root[FastFHIR::Fields::PATIENT::GENDER])
    std::cout << "gender=" << std::string_view(gender_node) << "\n";

// Walk arrays only if the parent field exists.
if (auto name_array = root[FastFHIR::Fields::PATIENT::NAME]) {
    for (auto& name_node : name_array.entries()) {
        if (auto family = name_node[FastFHIR::Fields::HUMANNAME::FAMILY])
            std::cout << "family=" << std::string_view(family) << "\n";
        if (auto given_array = name_node[FastFHIR::Fields::HUMANNAME::GIVEN]) {
            for (auto& given : given_array.entries())
                std::cout << "given=" << given.as<std::string_view>() << "\n";
        }
    }
}
```

That is the complete read path.

---

## Step 2 — Create a `Memory` arena

FastFHIR's Virtual Memory Arena (VMA) is the backing store used by the `Builder` and the
streaming ingestion path. There are three flavours:

### Anonymous RAM arena (in-process only)

```cpp
#include <FastFHIR.hpp>

// Reserve a 256 MB sparse virtual-address window backed by anonymous RAM.
// Physical pages are only committed by the OS as you actually write to them.
auto mem = FastFHIR::Memory::create();
```

### File-backed arena (persistent storage)

```cpp
#include <FastFHIR.hpp>

// Map the arena straight to a file on disk.
// Every write goes directly into the OS page cache — no separate write() call needed.
// The file is created (or reopened) and grown to the requested capacity.
auto mem = FastFHIR::Memory::createFromFile("patient.ffhr");
```

### Using a FastFHIR Memory Arena

Once a `Memory` object is created, it can be used to parse or parse, build, or ingest FHIR resources.

```cpp
// Parse memory like Step 1 above, but in a much easier path
FastFHIR::Parser parser(mem);
auto root = parser.root();
```
**A `Memory` arena is much more powerful than a simple fopen filestream. It can create a `Memory::Streamhead` into which a network socket can directly stream network data and the `Memory` can safely (and lockelessly) write FastFHIR data into the same archive from multiple concurrent threads.**

---

## Step 3 — Build a FastFHIR record from FHIR JSON

`Builder` writes binary data into the `Memory` arena; `Ingest::Ingestor` converts FHIR JSON into
the binary layout for you. Together they replace the traditional parse → validate →
serialize pipeline with a single in-place ingestion pass.

```cpp
#include <FastFHIR.hpp>
#include <FF_FieldKeys.hpp>
#include <FF_Ingestor.hpp>

// Use an anonymous arena for this example — swap in createFromFile() to persist to disk.
auto mem = FastFHIR::Memory::create(/*Optionally provide arena upper bounds (something like 4 GB)*/);

FastFHIR::Builder          builder(mem, FHIR_VERSION_R5);
FastFHIR::Ingest::Ingestor ingestor;

// Any valid FHIR R4/R5 Patient JSON string.
std::string json = R"({
    "resourceType": "Patient",
    "id": "patient-1",
    "gender": "male",
    "name": [{"family": "Smith", "given": ["John"]}]
})";

// Ingest: converts JSON → binary in a single pass, writes into the arena.
FastFHIR::Reflective::ObjectHandle patient_handle;
size_t parsed_count = 0;
ingestor.ingest({builder, FastFHIR::Ingest::SourceType::FHIR_JSON, json},
                patient_handle, parsed_count);

// Enrich fields using typed resource keys.
patient_handle[FastFHIR::Fields::PATIENT::ACTIVE] = true;
patient_handle[FastFHIR::Fields::PATIENT::BIRTH_DATE] = "1990-03-21";

// Seal the stream (no checksum for brevity; see API Examples for SHA-256).
builder.set_root(patient_handle);
auto view = builder.finalize();      // returns a lifetime-safe Memory::View

// Read it back immediately — zero copies, same arena pages.
FastFHIR::Parser parser(view.data(), view.size());
auto root              = parser.root();
std::string_view id       = root[FastFHIR::Fields::PATIENT::ID];         // "patient-1"
bool             active   = root[FastFHIR::Fields::PATIENT::ACTIVE].as<bool>();  // true
std::string_view gender   = root[FastFHIR::Fields::PATIENT::GENDER];     // "male"
std::string_view birthdate = root[FastFHIR::Fields::PATIENT::BIRTH_DATE]; // "1990-03-21"
std::cout << "id=" << id << "  active=" << active
          << "  gender=" << gender << "  birthdate=" << birthdate << "\n";
```

That is the complete write + read cycle. The advanced examples below show checksums,
file-backed persistence, socket transport, bundle editing, and lock-free concurrency.

---

# API Examples

## 1 — Ingest a FHIR JSON file and save as `.ffhr`

The arena is memory-mapped directly to the destination file. Every byte the ingestor
writes goes straight to the OS page cache — no intermediate buffer, no `write()` call,
and no copy at `finalize()`. When `finalize()` returns, `patient.ffhr` is a complete,
sealed FastFHIR archive on disk.

```cpp
#include <FastFHIR.hpp>
#include <FF_FieldKeys.hpp>
#include <openssl/sha.h>

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
std::string_view id     = root[FastFHIR::Fields::PATIENT::ID];    // "patient-1"
std::string_view gender = root[FastFHIR::Fields::PATIENT::GENDER]; // "male"
bool             active = root[FastFHIR::Fields::PATIENT::ACTIVE].as<bool>(); // true

// Walk the name array
for (auto& name_entry : root[FastFHIR::Fields::PATIENT::NAME].entries()) {
    std::string_view family = name_entry[FastFHIR::Fields::HUMANNAME::FAMILY];
    for (auto& given_entry : name_entry[FastFHIR::Fields::HUMANNAME::GIVEN].entries())
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
#include <FF_FieldKeys.hpp>

auto mem    = FastFHIR::Memory::createFromFile("patient.ffhr", 64 * 1024 * 1024);
auto parser = FastFHIR::Parser(mem);
auto root   = parser.root();

// Scalars coerce directly to C++ types — zero heap allocations
std::string_view id     = root[FastFHIR::Fields::PATIENT::ID];           // std::string_view
std::string_view gender = root[FastFHIR::Fields::PATIENT::GENDER];       // std::string_view
bool             active = root[FastFHIR::Fields::PATIENT::ACTIVE].as<bool>(); // bool
std::string_view dob    = root[FastFHIR::Fields::PATIENT::BIRTH_DATE];   // std::string_view

// Walk structured arrays
for (auto& name_node : root[FastFHIR::Fields::PATIENT::NAME].entries()) {
    std::string_view family = name_node[FastFHIR::Fields::HUMANNAME::FAMILY];
    for (auto& g : name_node[FastFHIR::Fields::HUMANNAME::GIVEN].entries())
        std::cout << g.as<std::string_view>() << " ";
    std::cout << family << "\n";
}

// Eagerly materialize into a generated C++ struct (strict schema validation)
PatientData patient_data = root;
```

---

## 3 — Re-open a `.ffhr` file and enrich it in place

FastFHIR's arena is append-only and memory-mapped. Writing new fields appends new bytes
to the tail and amends only the field pointers in the header — the original record bytes
are never touched. The OS page cache flushes only the dirty pages (new tail + updated
pointers). The file grows solely by the delta; no copy of existing data is ever made.

```cpp
#include <FastFHIR.hpp>
#include <FF_FieldKeys.hpp>
#include <openssl/sha.h>

auto mem = FastFHIR::Memory::createFromFile("patient.ffhr", 64 * 1024 * 1024);

FastFHIR::Builder          builder(mem, FHIR_VERSION_R5);
FastFHIR::Ingest::Ingestor ingestor;

// Obtain a mutable handle to the existing root
auto patient = builder.snapshot().root_handle();

// Append or overwrite scalar fields (new bytes appended; field pointer amended)
patient[FastFHIR::Fields::PATIENT::BIRTH_DATE] = "1990-03-21";
patient[FastFHIR::Fields::PATIENT::ACTIVE]     = true;

// Append a structured sub-object via the ingestor
ingestor.insert_at_field(patient, FastFHIR::Fields::PATIENT::TELECOM,
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
#include <FF_FieldKeys.hpp>
#include <asio.hpp>

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

patient_handle[FastFHIR::Fields::PATIENT::ACTIVE]  = true;
ingestor.insert_at_field(patient_handle, FastFHIR::Fields::PATIENT::TELECOM,
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
#include <FF_FieldKeys.hpp>
#include <openssl/sha.h>

// Map the entire bundle — address space reserved, pages not loaded until accessed
auto mem = FastFHIR::Memory::createFromFile("bundle.ffhr", 8ULL * 1024 * 1024 * 1024);

FastFHIR::Builder          builder(mem, FHIR_VERSION_R5);
FastFHIR::Ingest::Ingestor ingestor;

auto parser = FastFHIR::Parser(mem);
auto bundle = parser.root();

// Walk bundle.entry; the OS faults in only the pages we read
FastFHIR::Reflective::ObjectHandle target_patient;
for (auto& entry_node : bundle[FastFHIR::Fields::BUNDLE::ENTRY].entries()) {
    auto resource = entry_node[FastFHIR::Fields::BUNDLE_ENTRY::RESOURCE];
    if (!resource) continue;
    if (resource.recovery() != RECOVERY_TAG::Patient) continue;
    if (resource[FastFHIR::Fields::PATIENT::ID] == "patient-42") {
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
target_patient[FastFHIR::Fields::PATIENT::TELECOM] = /* ... */;

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

## 7 — Compact Archives

A standard FastFHIR stream uses a **fixed-offset layout**: every field slot is written at
a deterministic absolute offset regardless of whether the field is present. This makes
random access trivially O(1), but absent fields still occupy slot bytes in the stream.

`Compactor::archive()` performs a **post-finalize archival transform** that rewrites a
sealed stream into a **compact layout**: a presence bitmask followed by densely-packed
slots only for fields that are actually set. The resulting stream is a new file — the
original is not modified — and is **read-only** (decompact by rebuilding from the original
standard stream before mutation).

> **Use compact archives when a stream is finalized, unlikely to be mutated, and will
> be stored or transmitted at scale.** Compact archives are fully traversable via
> `Parser` using the identical typed-key API as standard streams — no code changes needed
> on the read side.

### Stream Format Comparison

| Layout | Field storage | Absent fields | Traversal |
|--------|--------------|---------------|-----------|
| Standard | Fixed absolute offsets per field | Null sentinel per slot | O(1) direct jump |
| Compact | Presence bitmask + densely-packed slots | **0 bytes** | O(1) via bitmask scan |

### Size Savings

Savings scale with field sparsity — the more absent fields, the greater the reduction.
Empirical results from the test suite (real FHIR resources, no artificial padding):

| Resource | Standard | Compact | Reduction |
|----------|----------|---------|-----------|
| `Patient` (id, gender, active, name/given/family) | 1 041 B | 356 B | **−66 %** |
| `Bundle` (`Patient` + `Observation` with components) | 1 799 B | 1 067 B | **−41 %** |

Reductions are larger for sparse resources (most FHIR resources have many optional fields
left unset) and smaller for dense records where most fields are populated.

### Usage

```cpp
#include <FastFHIR.hpp>
#include <FF_FieldKeys.hpp>
#include <FF_Compactor.hpp>

// 1. Build and finalize a standard stream as normal.
auto src_mem = FastFHIR::Memory::createFromFile("patient.ffhr", 64 * 1024 * 1024);
// ... build, ingest, finalize into src_mem ...

// 2. Archive to a new compact file — original stream is unchanged.
auto compact_mem = FastFHIR::Memory::createFromFile("patient.compact.ffhr",
                                                     64 * 1024 * 1024);
FastFHIR::Parser src(src_mem);
auto compact_view = FastFHIR::Compactor::archive(src, compact_mem);

// 3. Read the compact archive — identical typed-key API, zero copies.
FastFHIR::Parser compact(compact_mem);
auto root = compact.root();

std::string_view id     = root[FastFHIR::Fields::PATIENT::ID];          // "patient-1"
std::string_view gender = root[FastFHIR::Fields::PATIENT::GENDER];      // "male"
bool             active = root[FastFHIR::Fields::PATIENT::ACTIVE].as<bool>(); // true

for (auto& name_node : root[FastFHIR::Fields::PATIENT::NAME].entries()) {
    std::string_view family = name_node[FastFHIR::Fields::HUMANNAME::FAMILY];
    for (auto& g : name_node[FastFHIR::Fields::HUMANNAME::GIVEN].entries())
        std::cout << g.as<std::string_view>() << " ";
    std::cout << family << "\n";
}
```

> **Note:** Compact archives are immutable. To append or modify fields, open the original
> standard stream, enrich it, re-finalize, and re-compact.

---

# Reference

### Typed Resource Keys — `FastFHIR::Fields::<RESOURCE>::<FIELD>`

For C++ code, prefer typed resource keys. They carry owner recovery, field kind, and byte offset metadata for safer mutation and traversal.

```cpp
FastFHIR::Fields::PATIENT::ID          // "id"          (scalar)
FastFHIR::Fields::PATIENT::ACTIVE      // "active"      (bool)
FastFHIR::Fields::PATIENT::GENDER      // "gender"      (code)
FastFHIR::Fields::PATIENT::BIRTH_DATE  // "birthDate"   (string)
FastFHIR::Fields::PATIENT::NAME        // "name"        (array of HumanName)
FastFHIR::Fields::HUMANNAME::FAMILY    // "family"      (string)
FastFHIR::Fields::HUMANNAME::GIVEN     // "given"       (array of string)
FastFHIR::Fields::PATIENT::TELECOM     // "telecom"     (array of ContactPoint)
FastFHIR::Fields::OBSERVATION::STATUS  // "status"      (code)
FastFHIR::Fields::OBSERVATION::CODE    // "code"        (CodeableConcept)
FastFHIR::Fields::CODING::SYSTEM       // "system"      (string)
FastFHIR::Fields::BUNDLE::ENTRY        // "entry"       (bundle entry array)
FastFHIR::Fields::BUNDLE_ENTRY::RESOURCE // "resource"  (polymorphic resource)
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

## Generator Architecture

The generator lives in `tools/generator/` and is split by responsibility:

| File | Purpose |
|---|---|
| `fetch_specs.py` | Downloads and extracts required FHIR bundles |
| `ffd.py` | Builds dictionary artifacts |
| `ffcs.py` | Builds code-system enums and mapping metadata |
| `ffc.py` | Emits C++ data/resource structs and read/write logic |
| `make_lib.py` | Orchestrates the full generation pipeline |

**Resource scope constants** in `ffc.py`:

| Constant | Contents |
|---|---|
| `PRODUCTION_TYPES` | Core FHIR datatypes always generated (Coding, Quantity, Identifier, Age, Count, Availability, ExtendedContactDetail, …) |
| `US_CORE_RESOURCES` | 27 resources for US Core IG interoperability |
| `UK_CORE_RESOURCES` | 22 resources for UK Core IG interoperability |

## Design Notes
* Resource and datatype structs are generated from official HL7 StructureDefinitions.
* Version-specific fields are guarded by generated version checks.
* The top-level file container is `FF_HEADER`, which stores file magic, version, checksum offset, root offset, and payload size.
* `Parser::root()` and `Node` traversal use offset/recovery checks to validate data block integrity.
* Full recursive validation is available via typed struct assignment such as `PatientData patient = parser.root()`.

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