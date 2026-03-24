# FastFHIR

FastFHIR is a wildly fast, lock-free binary serializer and C++20 code generation pipeline for HL7 FHIR resources (R4 and R5). 

Designed for high-performance healthcare applications and heavy data ingestion, FastFHIR completely bypasses traditional JSON bottlenecks. It generates strongly-typed C++ structs and serialization code directly from official HL7 FHIR StructureDefinitions, offering:

* **Seriously High Performance**
    * **Zero-Copy Parsing:** Read massive FHIR payloads instantly using a pure offset-based binary layout.
    * **Lock-Free Concurrent Generation:** Serialize thousands of resources simultaneously across a thread pool using a Virtual Memory Arena with zero heap allocations, zero pointer invalidation, and without a single mutex.

* **Deterministic Memory Footprint**

    FastFHIR’s binary architecture provides a level of speed and safety that legacy text-based standards cannot achieve:

    * **FastFHIR vs. HL7 V2:** While V2 relies on brittle, sequential pipe-delimiters that require O(N) linear parsing, FastFHIR allows O(1) random access to any deeply nested field.

    * **FastFHIR vs. HL7 V3 (CDA):** By replacing verbose XML DOM trees with explicit binary offsets, FastFHIR reduces memory overhead by orders of magnitude and eliminates the risk of XML injection or buffer overflows inherent in complex V3 parsers.

    * **FastFHIR vs. Standard FHIR (JSON):** Standard JSON parsing is allocation-heavy and requires massive CPU cycles to transform text into objects. FastFHIR uses Zero-Copy Primitives, allowing your application to read clinical data the microsecond it hits RAM.

    * **Absolute Integrity:** Built-in cryptographic checksum sealing (SHA-256) guarantees that the record is immutable, providing a hardware-verified security layer that legacy plaintext standards lack.

    * **Hardware-Level Safety:** Utilizing Virtual Memory Arenas (via mmap or VirtualAlloc) ensures that pointers remain stable and memory access is protected by the OS kernel. This prevents the data corruption and "pointer chasing" common in the dynamic heap-allocated buffers used by traditional FHIR libraries.

* **Developer Ergonomics**

    * **JSON-Style Dynamic Navigation:** Traverse complex nested FHIR trees seamlessly using native C++ [] operators (e.g., root["subject"]["reference"]).

    * **IDE-Friendly Static Keys:** Utilize zero-overhead compiled field keys (e.g., FastFHIR::FieldKeys::Observation::status) to completely bypass runtime string hashing.

    * **Thread-Safe Object Assignment:**  Build complex relationships concurrently with parent["child"] = child_data syntax that automatically and safely patches V-Table pointers under the hood.

* **Platform & Pipeline Ready**

    * **Cross-Platform Virtual Arenas:** Native, zero-disk memory mapping via POSIX mmap (Linux/macOS) and Win32 VirtualAlloc (Windows).

    * **WASM / Emscripten Ready:** Built-in hooks for remote byte fetching (check_and_fetch_remote), allowing massive, gigabyte-scale pathology and/or radiology datasets to be queried directly in a web viewer without downloading the entire file.

## API Usage Examples

FastFHIR provides a seamless, high-level API over its bare-metal memory architecture.

### 1. Zero-Copy Parsing & Navigation

Read complex FHIR data instantly without parsing strings or allocating memory. FastFHIR supports both JSON-style dynamic string keys and lightning-fast, IDE-friendly compiled Field Keys.
```cpp
#include <FastFHIR.hpp>

// 1. Instant zero-copy initialization (O(1) time complexity)
auto parser = FastFHIR::Parser::create(payload.data(), payload.size());

// 2. Access the root resource
FastFHIR::Node root = parser.root();

// Method A: IDE-Friendly Typed Field Keys (Fastest - avoids runtime string scanning)
using FF_FK = FastFHIR::FieldKeys;
auto status = root[FF_FK::Observation::STATUS].value().as_string();

// Method B: JSON-Style dynamic navigation
auto nested_system = root["code"]["coding"][0]["system"].value().as_string();
```

### 2. Lock-Free Concurrent Generation
FastFHIR's `Builder` uses OS-level memory mapping (Virtual Arena) and atomic offsets to allow massive thread pools to write to a single, contiguous binary stream simultaneously without locking.

```cpp
#include <FastFHIR.hpp>
#include <thread>
#include <vector>

// Initialize a Virtual Arena (Reserves address space, consumes no physical RAM until written)
FastFHIR::Builder builder (/*Optional stream limits*/); 
std::vector<std::thread> pool;

// 32 threads simultaneously serializing clinical data from multiple sources into the same contiguous stream
for (int i = 0; i < 32; ++i) {
    pool.emplace_back([&builder, i]() {
        // 1. Thread-local work (e.g., AI inference, data fetching)
        ObservationData local_obs;
        local_obs.status = "preliminary";
        
        // 2. Lock-free, 1-clock-cycle atomic claim and concurrent write
        // No mutexes. No heap allocations. No pointer invalidation.
        auto handle = builder.append_obj(local_obs);
        
        // 3. (Optional) push handle.offset() to a lock-free queue to link to a Bundle later
    });
}

for (auto& thread : pool) thread.join();
```
### 2. Sealing and Cryptographic Checksums
Once all worker threads have drained, the main thread can seal the file and append a cryptographic hash directly into the binary footer using a clean callback.
```cpp
#include <FastFHIR.hpp>
#include <openssl/sha.h>

// Link the primary resource to the file header
builder.set_root(root_handle); 

// Seal the file and generate a SHA-256 hash
auto payload = builder.finalize(FF_CHECKSUM_SHA256, [](const void* byte_start, size_t bytes_to_hash) {
    std::vector<uint8_t> hash(SHA256_DIGEST_LENGTH);
    SHA256(static_cast<const unsigned char*>(byte_start), bytes_to_hash, hash.data());
    return hash; // The Builder automatically writes this into the stream footer
});

// `payload` is now a std::string_view of the complete, network-ready binary stream and can be ingested as shown above
auto parser = FastFHIR::Parser::create(payload.data(), payload.size());
```

## Prerequisites

- Python 3.9+
- Clang or GCC with C++20 support
- CMake 3.20+ (for the CMake workflow)
- Network access (generator fetches FHIR bundles from HL7)

## Quick Start

FastFHIR includes a root `CMakeLists.txt`.

Configure and build:

```bash
cmake -S . -B build
cmake --build build -j
```

What this does:

1. Downloads and extracts FHIR specs (R4/R5)
2. Builds master dictionary (`generated_src/FF_Dictionary.hpp`)
3. Builds code system enums (`generated_src/FF_CodeSystems.hpp`)
4. Generates data types and resources under `generated_src/`
5. Cleans temporary `fhir_specs/`
6. 

Notes:

1. By default, CMake is configured to run generation during configure/build (`FASTFHIR_RUN_GENERATOR=ON`).
2. Generated `.cpp` files are discovered from `generated_src/` and built into `libfastfhir.a`.

Optional: disable generator execution in CMake if you've already generated files manually:

```bash
cmake -S . -B build -DFASTFHIR_RUN_GENERATOR=OFF
cmake --build build -j
```

## Build with Xcode (macOS Alternative)

macOS users can use the included Xcode project instead of CMake:

1. run python3 generator/make_lib.py
1. Open `FastFHIR.xcodeproj` in Xcode.
2. Select your target/scheme.
3. Build with Product > Build.

This is useful if you prefer native Xcode workflows for editing, debugging, and signing.

## Manual Compile Check

Validate generated output with:

```bash
clang++ -c generated_src/FF_DataTypes.cpp -I. -std=c++20
clang++ -c generated_src/FF_Patient.cpp -I. -std=c++20
clang++ -c generated_src/FF_Observation.cpp -I. -std=c++20
```

## Generator Architecture

The generator lives in `tools/generator/` and is split by responsibility:

- `fetch_specs.py`: Downloads and extracts required FHIR bundles
- `ffd.py`: Builds dictionary artifacts
- `ffcs.py`: Builds code-system enums and mapping metadata
- `ffc.py`: Emits C++ data/resource structs and read/write logic
- `make_lib.py`: Orchestrates full generation pipeline

## Design Notes

- Resource and datatype structs are generated from official StructureDefinitions
- Version-specific fields are guarded by generated version checks
- Binary layout uses explicit offsets and recovery tags
- The top-level file container is `FF_HEADER`, which stores file magic, version, checksum offset, root offset, and payload size
- String/code handling supports lock-free style emitter flow in primitives

## Parser Access Patterns

FastFHIR supports two complementary read styles:

1. Strongly typed generated blocks (`FF_OBSERVATION`, `FF_PATIENT`, etc.)
2. JSON-like dynamic navigation through `FastFHIR::Node`

### JSON-Style Navigation

Use `Parser::root()` to get a lightweight `Node`, then navigate object fields and arrays with `[]`.

```cpp
FastFHIR::Parser parser(buffer, size);
auto root = parser.root();

auto status = root["status"].as_string();
auto firstCoding = root["code"]["coding"][0];
auto system = firstCoding["system"].as_string();
```

`Node` helpers include:

- `keys()` / `fields()` for object introspection
- `entries()` / `size()` for array traversal
- scalar reads: `as_string()`, `as_bool()`, `as_uint32()`, `as_float64()`

### IDE-Friendly Typed Field Keys (Fast Path)

Generated constants in `generated_src/FF_FieldKeys.hpp` provide compile-time, block-scoped keys:

```cpp
using namespace FastFHIR;

auto root = parser.root();
auto code = root[Fields::OBSERVATION::CODE];
auto coding0 = code[Fields::CODEABLECONCEPT::CODING][0];
auto system = coding0[Fields::CODING::SYSTEM].as_string();
```

This path avoids runtime string scanning by using precomputed field metadata (owner recovery, field kind, header offset, child recovery).

### Validation Model

- `Parser::root()` and Node traversal use simple offset/recovery checks.
- Full recursive checks are explicit via typed APIs like `view_root_full<T_Block>()` and `read_root_full<T_Block>()`.

## License

This project is licensed under the FastFHIR Shared Source License (FF-SSL) v1.2, which is based on the Apache License 2.0 with additional restrictions:

- You may not modify or redistribute altered versions of the core implementation (see LICENSE for details).
- Strict attribution to Dr. Ryan Erik Landvater and the FastFHIR project is required in all products, services, or derivative works.
- You may not re-brand, rename, or claim authorship of the core implementation.
- The "right to repair" clause allows minimal patching only if the author fails to address critical bugs or vulnerabilities within 30 business days of notification.

See the LICENSE file for the full legal text and compliance requirements.

## Checksum Support

FastFHIR now includes an optional checksum block for top-level file integrity.

The top-level `FF_HEADER` can point to an `FF_CHECKSUM` block through `CHECKSUM_OFFSET`.

Supported algorithms:

- `FF_CHECKSUM_NONE`
- `FF_CHECKSUM_CRC32`
- `FF_CHECKSUM_MD5`
- `FF_CHECKSUM_SHA256`
- `FF_CHECKSUM_SHA512`

Implementation notes:

1. `FF_HEADER` stores the checksum block offset, or `FF_NULL_OFFSET` when no checksum is present.
2. `FF_CHECKSUM` stores the selected algorithm and an offset to the raw hash bytes.
3. Hash bytes are stored as an `FF_STRING` payload and can be accessed as a zero-copy `std::string_view`.
4. Header validation now checks the checksum block if one is present.