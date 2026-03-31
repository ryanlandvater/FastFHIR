
FastFHIR
===========

**FastFHIR** is a wildly fast, lock-free binary serializer and C++20 code generation pipeline for HL7 FHIR resources (R4 and R5). 

Healthcare interoperability has historically relied on formats that are inherently unsafe, computationally expensive, or structurally brittle. FastFHIR replaces traditional parsing with a mathematically strict, offset-based binary layout that guarantees safety, in-stream HL7 enrichment, and blistering speed. It generates strongly-typed C++ structs and a mathematically strict, zero-copy binary architecture directly from official HL7 FHIR Structure Definitions.

## Why FastFHIR?

### 1. Extreme Performance
FastFHIR turns data traversal into pure pointer arithmetic, fundamentally outpacing both legacy text formats and modern serialized binaries.
* **O(1) Random Access:** Jump instantly to any deeply nested FHIR field. This completely bypasses the `O(N)` linear scanning of HL7v2 and the `O(N)` string-hashing and DOM construction of JSON.
* **Zero-Heap Allocation:** Reading a FastFHIR stream requires 0 heap allocations. A lightweight `Node` viewing lens is passed directly over the raw memory buffer, enabling nanosecond read times from the instant the message hits RAM.
* **Zero-Copy Engine:** FastFHIR outperforms even Google Research's Protobuf FHIR implementation because it skips the deserialization phase entirely, operating natively at the absolute memory-bandwidth limit without unpacking varints or allocating C++ message objects.

### 2. Hardware-Level Safety & Security
Legacy text standards expose systems to XML injection (CDA) and heap fragmentation (JSON). FastFHIR guarantees deterministic memory and structural integrity.
* **OS-Protected Memory:** By utilizing Virtual Memory Arenas (via POSIX `mmap` or Win32 `VirtualAlloc`), FastFHIR ensures pointers remain perfectly stable and memory access is protected by the OS kernel.
* **Strict Schema Validation:** The binary layout embeds explicit `RECOVERY_TAG` metadata for every object. This provides guaranteed safe polymorphic resolution and strict C++ type checking at runtime, preventing incorrect information context, garbage reads, and buffer overflows.
* **Cryptographic Sealing:** Built-in checksum footers (SHA-256/512) guarantee record immutability, providing a hardware-verified security layer for clinical data lakes.

### 3. Clinical Informatics: Lock-Free Enrichment
* **In-Stream Lazy Enrichment:** Read a `Patient.id` or route a payload in a nanosecond without parsing the other 9,999 fields in a `Bundle`. You only pay for the exact bytes you traverse. This means you can simply append a new laboratory result for a patient in the bundle without parsing any other aspect of the bundle record or rewritting it. Simply add the new result and reseal it before passing the entire message along to the rest of the clinical workflow. 
* **Concurrent Mutex-Free Generation:** Serialize thousands of resources simultaneously across a thread pool. FastFHIR's atomic pointer-patching architecture allows surgical data appends (like NLP annotations) into a single contiguous stream without a single lock.

### 4. Developer Ergonomics
You do not have to sacrifice a clean API for bare-metal performance.
* **IDE-Friendly Static Keys:** Utilize zero-overhead, compiled `O(1)` field keys (e.g., `FastFHIR::Fields::OBSERVATION::STATUS`) to completely bypass runtime string hashing.
* **Polymorphic Type Safety:** Extract primitives or complex structs effortlessly with strict runtime schema validation using `node.as<PatientData>()` or `node.as<std::string_view>()`.
* **JSON-Style Fallback:** Traverse complex trees seamlessly using native C++ `[]` operators (e.g., `root.get_dynamic("subject")`).

# API Usage Examples

### 1. Zero-Copy Parsing & Navigation
Read complex FHIR data instantly without parsing strings or allocating memory. 

```cpp
#include <FastFHIR.hpp>

// 1. Instant zero-copy initialization (O(1) time complexity)
auto parser = FastFHIR::Parser::create(payload.data(), payload.size());

// 2. Access the root resource view
FastFHIR::Node root = parser.root();

// Method A: IDE-Friendly Compiled Field Keys (Fastest - O(1) memory peek)
using namespace FastFHIR::Fields;
std::string_view status = root[OBSERVATION::STATUS].as<std::string_view>();

// Method B: Eagerly parse a specific sub-tree into a C++ struct (Strict Schema Validation)
FastFHIR::CodeableConceptData code = root[OBSERVATION::CODE].as<FastFHIR::CodeableConceptData>();
```

### 2. Lock-Free Concurrent Generation & Enrichment
FastFHIR's Builder uses OS-level memory mapping (Virtual Arena) and atomic offsets to allow massive thread pools to write to a single, contiguous binary stream simultaneously without locking.

```cpp
#include <FastFHIR.hpp>
#include <thread>
#include <vector>

// Initialize a Virtual Arena (Reserves address space, consumes no physical RAM until written)
FastFHIR::Builder builder; 
std::vector<std::thread> pool;

// 32 threads simultaneously serializing clinical data from multiple sources into the same stream
for (int i = 0; i < 32; ++i) {
    pool.emplace_back([&builder, i]() {
        // 1. Thread-local work (e.g., AI inference, simdjson text ingestion)
        FastFHIR::ObservationData local_obs;
        local_obs.status = "preliminary";
        
        // 2. Lock-free, 1-clock-cycle atomic claim and concurrent write
        // No mutexes. No heap allocations. No pointer invalidation.
        FastFHIR::ObjectHandle handle = builder.append_obj(local_obs);
        
        // 3. (Optional) push handle to a lock-free queue to link to a Bundle later
    });
}

for (auto& thread : pool) thread.join();
```

### 3. Cryptographic Sealing
Once all worker threads have drained, the main thread can seal the file and append a cryptographic hash directly into the binary footer using a clean callback.

```cpp
#include <FastFHIR.hpp>
#include <openssl/sha.h>

// Link the primary resource to the file header
builder.set_root(root_handle); 

// Seal the file and generate a SHA-256 hash
auto payload = builder.finalize(FastFHIR::FF_CHECKSUM_SHA256, [](const void* byte_start, size_t bytes_to_hash) {
    std::vector<uint8_t> hash(SHA256_DIGEST_LENGTH);
    SHA256(static_cast<const unsigned char*>(byte_start), bytes_to_hash, hash.data());
    return hash; // The Builder automatically writes this into the stream footer
});

// `payload` is now a std::string_view of the complete, network-ready binary stream
// Asio (for example) can then stream it

// And server side it can be parsed with (see earlier code block):
auto server_parser = FastFHIR::Parser::create(payload.data(), payload.size());
```

# Quick Start
## Prerequisites
* Python 3.9+
* Clang, GCC, or MSCV with C++20 support
* CMake 3.20+ (for the CMake workflow)
* Network access (generator fetches FHIR bundles from HL7)

## Installation

FastFHIR includes a root `CMakeLists.txt`.

Configure and build:

```Bash
cmake -S . -B build
cmake --build build -j
```
What this does:

1. Downloads and extracts FHIR specs (R4/R5)

2. Builds master dictionary (generated_src/FF_Dictionary.hpp)

3. Builds code system enums (generated_src/FF_CodeSystems.hpp)

4. Generates data types and resources under generated_src/

5. Cleans temporary fhir_specs/

Notes:
1. By default, CMake is configured to run generation during configure/build (FASTFHIR_RUN_GENERATOR=ON).

2. Generated .cpp files are discovered from generated_src/ and built into libfastfhir.a.

Optional: disable generator execution in CMake if you've already generated files manually:

```Bash
python3 generator/make_lib.py
cmake -S . -B build -DFASTFHIR_RUN_GENERATOR=OFF
cmake --build build -j
```

### Manual Compile Check
Validate generated output with:

```Bash
clang++ -c generated_src/FF_DataTypes.cpp -I. -std=c++20
clang++ -c generated_src/FF_Patient.cpp -I. -std=c++20
clang++ -c generated_src/FF_Observation.cpp -I. -std=c++20
``` 
### Generator Architecture
The generator lives in `tools/generator/` and is split by responsibility:

* fetch_specs.py: Downloads and extracts required FHIR bundles

* ffd.py: Builds dictionary artifacts

* ffcs.py: Builds code-system enums and mapping metadata

* ffc.py: Emits C++ data/resource structs and read/write logic

* make_lib.py: Orchestrates full generation pipeline

### Design Notes
* Resource and datatype structs are generated from official StructureDefinitions

* Version-specific fields are guarded by generated version checks

* Binary layout uses explicit offsets and recovery tags

* The top-level file container is `FF_HEADER`, which stores file magic, version, checksum offset, root offset, and payload size

* String/code handling supports lock-free style emitter flow in primitives

### Validation Model
* `Parser::root()` and Node traversal use simple 10 byte offset/recovery checks to validate the integrity of data blocks.

* Full recursive checks are explicit via typed APIs like `view_root_full<T_Block>()` and `read_root_full<T_Block>()`.

## Checksum Support
FastFHIR includes an optional checksum block for top-level file integrity.

The top-level `FF_HEADER`can point to an `FF_CHECKSUM` block through `CHECKSUM_OFFSET`.

Supported algorithms:

* FF_CHECKSUM_NONE

* FF_CHECKSUM_CRC32

* FF_CHECKSUM_MD5

* FF_CHECKSUM_SHA256

### Implementation notes:

1. FF_HEADER stores the checksum block offset, or FF_NULL_OFFSET when no checksum is present.

2. FF_CHECKSUM stores the selected algorithm and an offset to the raw hash bytes.

3. Hash bytes are stored as an `FF_STRING payload` and can be accessed as a zero-copy `std::string_view`.

4. Header validation now checks the checksum block if one is present.

# License
This project is licensed under the FastFHIR Shared Source License (FF-SSL) v1.2, which is based on the Apache License 2.0 with additional restrictions:

You may not modify or redistribute altered versions of the core implementation (see LICENSE for details).

Strict attribution to Dr. Ryan Erik Landvater and the FastFHIR project is required in all products, services, or derivative works.

You may not re-brand, rename, or claim authorship of the core implementation.

The "right to repair" clause allows minimal patching only if the author fails to address critical bugs or vulnerabilities within 30 business days of notification.

See the LICENSE file for the full legal text and compliance requirements.

---
**Attribution**: The design of FastFHIR is based upon the [Iris File Extension](https://www.sciencedirect.com/science/article/pii/S2153353925000471) (by Ryan Landvater) and [FlatBuffers](https://github.com/google/flatbuffers) (by Wouter van Oortmerssen and the Google Fun Propulsion Labs team).