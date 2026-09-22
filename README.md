
FastFHIR
===========

[![Release](https://img.shields.io/github/v/release/RyanLandvater/FastFHIR?label=version&color=blue)](https://github.com/RyanLandvater/FastFHIR/releases)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)
![FHIR R4/R5](https://img.shields.io/badge/FHIR-R4%20%7C%20R5-blueviolet)
![License](https://img.shields.io/badge/license-MPL--2.0-brightgreen)

### Clinical meaning should not be tied to text syntax

Healthcare's high-reliability aspirations call for less implementation variation,
less unnecessary data conversion, and better ways to detect and recover from
structural damage. Clinical software should not have to rebuild the same
serialization machinery at every interface.

**FastFHIR is a freely available, schema-generated binary representation and
shared implementation for FHIR-defined clinical data.** It generates typed APIs
and an offset-based layout from HL7 FHIR StructureDefinitions. Applications can
access fields directly in the serialized buffer, without first unpacking the
whole message into a second object graph. Redundant location and type information
supports structural checking and recovery; the same native core serves C/C++
and Python consumers.

The goal is to make valid fields discoverable in the IDE, put serialization in
one reusable library, and support efficient reads and updates throughout clinical
middleware and exchange. This brings the established benefits of generated binary
formats to FHIR while adding deliberate structural redundancy. Technical merit
should be evaluated through fidelity, developer effort, performance, compatibility,
and failure behavior, not the installed base of a competing representation.

**Evidence and scope.** [Published benchmarks](https://github.com/ryanlandvater/FastFHIR-benchmark/releases)
compare selected operations and controlled structural-corruption recovery against
HL7v2, FHIR JSON, and Google FHIR protobuf. Results are implementation- and
workload-specific; they do not establish clinical error reduction, hardware-cost
savings, achieved Six Sigma reliability, or guaranteed integrity. FastFHIR is a
serialization library, not a complete FHIR server or a claim of HL7 endorsement
of its binary wire format.

## FAQ

### Does FastFHIR replace FHIR?

_**A**: **No.** It is FHIR-derived clinical data without relying on text escape characters or needing a JSON parser for direct binary reads. It is built directly from the FHIR definitions and pulls them from HL7 International while building the library._

### Can I convert a FastFHIR stream back into a standard FHIR JSON document?

_**A**: **Yes**, the library can translate the binary representation into human-readable JSON at different levels of granularity: the whole bundle, a single resource, or a supported sub-element of that resource._

The supported field surface and extension policy matter. Unknown extension URL
retention alone does not preserve its complete payload; see
[Extensions and modifierExtensions](#extensions-and-modifierextensions).

### Why do you want to reduce the need for connectathons?

_**A**: I (Ryan, who originally wrote FastFHIR) am both an MD and a programmer. I do not like hunting for information -- I like it to be presented to me. As a result, I added generated field definitions so that your programmers can be presented with valid FHIR data elements within the hierarchical / nested ontology as they attempt to access those fields._

With a generic dictionary, a made-up field name is just another string:

```text
patient["myFakeMadeUpField"] = ...
```

With generated keys, the IDE can present the field names to you:

<!-- ff-compile: expressions -->
```cpp
FastFHIR::Fields::PATIENT::ID
FastFHIR::Fields::PATIENT::NAME
```

<!-- readme-test: skip -- field-discovery illustration; executable examples are in python/README.md -->
```py
from fastfhir.fields import Patient

Patient.ID
Patient.NAME
```

**Additionally, FastFHIR is a freely available open-source implementation. This is huge. It provides both an API and a C-facing ABI, so your vendors and researchers can use the same core instead of paying programmers to write near-identical serialization code and then spending time at connectathons making sure it is nearly identical.**

Sharing the implementation does not eliminate clinical mapping, profile, or
workflow testing. It is intended to reduce the structural implementation work
that gets repeated before those questions can even be addressed. Typed libraries
also exist for HL7v2 and FHIR JSON; FastFHIR combines that developer experience
with direct binary access and structural redundancy.

### What programming languages are supported?

**A: Right now I support C/C++ and Python, with additional languages planned, starting with JavaScript via WASM. The goal is to support them as first-class languages. The Python library is compiled from the same native core, rather than being a separate Python implementation of the serializer.**

The [Python API guide](python/README.md) and
[C API example](#8--calling-from-c-fastfhirh) show the current interfaces.
Language wrappers and value conversions can still affect end-to-end performance;
sharing a core is not a promise of identical timings in every language.

### How does this work with FHIR extensions?

_**A**: I want to be stricter about extension registration and governance. We are planning an online extension repository that takes registered JSON extension definitions and compiles them into WASM modules. This has two goals:_

1. Make the extension's structure a shared, checked implementation that consumers can load, rather than something each company has to interpret and rebuild independently.
2. Provide versioned definitions and modules that remain available for reading historical data. The Z-segment / "when you've seen one HL7, you've seen just one HL7" stream issue needs to be put to bed.

FHIR already has a formal mechanism for defining extensions. The added goal here
is to connect registration and governance to compiled implementations. The public
upload/compilation service is planned; it is distinct from the optional codec
host and from JavaScript/WASM language bindings. Compilation cannot by itself
guarantee clinical meaning, permanent availability, or compatibility with every
reader. See the [extension details](#extensions-and-modifierextensions).

## Binary Encoding and Reliability

Direct-access binary formats avoid unpacking an entire message just to read a
few fields. This is the motivation behind [FlatBuffers](https://flatbuffers.dev/);
[Protobuf](https://protobuf.dev/overview/) also provides compact encoding and
generated APIs, but normally parses messages into a separate representation.
FastFHIR applies direct buffer access to FHIR-defined clinical data and adds
structural recovery metadata. "Zero-copy" describes the direct read path, not
network transport, JSON conversion, validation, or owning-value materialization.

### Binary payloads and text encoding

Arbitrary binary can contain HL7v2 delimiters such as pipes, carets, or segment
terminators. Valid HL7v2 ED Base64 avoids these collisions with the usual
delimiters, at approximately one-third expansion before compression plus
encoding/decoding work. FHIR JSON `Attachment.data` also uses Base64; external
references and native FHIR Binary HTTP responses are alternatives.

A length-delimited binary representation can avoid that text-encoding step,
but a binary container alone does not establish an end-to-end raw-payload API.
FastFHIR's current FHIR `base64Binary` ingestion is string-like. Native BAM/DICOM
insertion, extraction, and byte-identical round trips need explicit validation
before claiming that workflow avoids Base64. Structural recovery is not repair
of a damaged compressed genomic or image payload.

### Structural redundancy

Location and type information provide structural evidence for checking and
recovering a damaged stream. Like the redundancy in a 2D barcode, the purpose
is to retain useful information despite damage. The mechanism is different:
FastFHIR does not provide a general payload error-correcting code. Recovery can
fail or produce incorrect or extra values. Recovered content needs independent
validation before clinical use; correct-field yield is not whole-record integrity.

---

## Table of Contents

- [FAQ](#faq)
- [Binary Encoding and Reliability](#binary-encoding-and-reliability)
- [Why FastFHIR?](#why-fastfhir)
- [Quick Start](#quick-start)
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
    - [8 — Calling from C](#8--calling-from-c-fastfhirh)
- [CLI Tools](#command-line-interface-tools)
  - [ff\_ingest](#ff_ingest)
  - [ff\_export](#ff_export)
  - [ff\_compact](#ff_compact)
- [Reference](#reference)
    - [Typed Resource Keys](#typed-resource-keys--fastfhirfieldsresourcefield)
    - [Code Assignment Semantics](#code-assignment-semantics)
    - [Date/Time Assignment Semantics](#datetime-assignment-semantics)
    - [Polymorphic Choice Assignment and Readback](#polymorphic-choice-assignment-and-readback)
    - [Checksum Algorithms](#checksum-algorithms)
    - [FHIR Versions](#fhir-versions)
- [Extensions and modifierExtensions](#extensions-and-modifierextensions)
- [Generator Architecture](#generator-architecture)
- [Design Notes](#design-notes)
- [License](#license)

---

## Why FastFHIR?

### 1. Direct Access and Compact Archives
FastFHIR turns field navigation into offset-based buffer access.
* **Direct Field Access:** Follow known field offsets without a whole-message parse. Nested paths still require traversal, and finding a matching resource can require a scan or index. Parsed or indexed JSON/HL7v2 readers are distinct comparison cases.
* **Non-Owning Views:** Field metadata uses `std::span` views over static tables; a `Node` views the encoded buffer. Owning strings, materialized objects, and `entries()` vectors can allocate.
* **No Unpacking for Direct Reads:** Access supported fields without constructing a second message object graph. Conversion and validation have their own costs. See [FastFHIR-benchmark](https://github.com/ryanlandvater/FastFHIR-benchmark) for measured workloads and timing boundaries.
* **Optional Compaction:** Compact archives strip absent fields with presence bitmasks and dense packing. Size savings depend on sparsity and workload; see [Compact Archives](#7--compact-archives).

### 2. Type Safety & Validated FHIR Format
* **Generated Types and Structural Checks:** APIs and layouts derive from HL7 StructureDefinitions. Runtime offset, bounds, and recovery-tag checks support typed access. Schema-derived code does not by itself establish that every resource satisfies its clinical profile or that arbitrary hostile input is safe.
* **Native Polymorphic Types:** FastFHIR handles FHIR's polymorphic fields as the spec defines them — choice elements such as `valueQuantity` and `valueString`, and resource-bearing slots such as `Bundle.entry.resource`. Concrete payload types keep their identity through ingest, traversal, mutation, and re-export.
* **Structured Codes & Extensions:** Codes derive from FHIR CodeSystems; extension URLs are classified during ingestion. The optional WASM codec path and planned registry are described below. Unknown-URL retention does not guarantee preservation of unknown extension content.
* **Primitive Extensions:** FHIR's underscore-prefixed primitive extension model works end to end — extensions on scalar primitives survive ingest, validation, traversal, and re-export. Generic protobuf JSON tools do not implement this.
* **Conformance Checking Is Opt-In:** Structural validation (a block sits at its own offset, carries its tag, fits in the arena) always runs. *Conformance* is a separate, attachable layer generated from the HL7 StructureDefinitions: required elements like `Observation.status`, cardinality, and a queryable record of every FHIRPath invariant it does not evaluate. Build with `-DFASTFHIR_BUILD_CONFORMANCE=ON` and attach it with `FF_BuilderAttachLayer`. Attached, it writes a byte-identical stream; detached, it costs one null check. See `examples/conformance_layer.cpp`.

> **Scope.** FastFHIR is a serialization library, not a FHIR server. No REST API, no SMART on FHIR, no OAuth. The conformance layer checks *resources*, not *interactions* — it will tell you an `Observation` is missing its required `status`, and nothing about `$validate`, search parameters, or capability statements.

### 3. Structural Checking and Recovery
* **Arena-Based Storage:** Virtual memory arenas keep buffer addresses stable within their documented lifetime and capacity rules. OS mapping does not replace bounds checks or establish immunity to injection or memory-safety defects.
* **Location and Type Evidence:** Self-offsets, references, and recovery tags support structural validation and recovery. Their redundancy is not a guarantee that every damaged edge can be reconstructed correctly.
* **Integrity Footers:** Optional CRC32/MD5/SHA-256 footers support corruption detection. An unkeyed checksum is not authenticity: a writer can alter the data and recompute it. MD5 is not suitable for adversarial integrity claims. Signed archives remain roadmap work.
* **Explicit Safety Boundary:** Malformed-input checks and structural-recovery tests are not hostile-input fuzzing or clinical validation. Reject, quarantine, or independently validate damaged/recovered data as appropriate; do not infer safe clinical use from a high recovery percentage.

### 4. Clinical Informatics: Lock-Free Enrichment & Custom Profiles
* **Selective Reads and Updates:** Read a field without unpacking unrelated resources. Append an observation without reserializing every resource; entry arrays, links, headers, and checksums can still need rewriting. Constant stream growth does not imply constant write traffic.
* **Concurrent Construction:** The builder provides atomic allocation/publication mechanisms for the supported construction patterns below. This does not make arbitrary concurrent mutation, readers during mutation, or application-level coordination automatically safe.
* **Custom Implementation Guides:** The generator composes profiles at build time from official HL7 bundles — US Core, UK Core, claims, or your own. A resource outside your profile still round-trips intact (see [Resource groupings](#resource-groupings)).

### 5. Developer Ergonomics & Cross-Language Support
Native C++ with a C-facing API and compiled Python bindings share core implementation code.
* **Static Keys:** Compiled O(1) typed keys such as `FastFHIR::Fields::PATIENT::ACTIVE` replace runtime string hashing.
* **Assign to C++ Types:** Implicit conversion works directly (`std::string_view id = node[FastFHIR::Fields::PATIENT::ID]`), or materialize a whole struct (`PatientData patient = parser.root()`).
* **JSON-Style Traversal:** `root[FastFHIR::Fields::PATIENT::NAME][0]` walks the tree.
* **FHIR-Aware JSON:** `ff_ingest`, `ff_export`, and the APIs handle FHIR-specific choices, resource slots, and primitive extensions. Check profile coverage and extension policy for the required round trip. Generic protobuf JSON helpers like `google::protobuf::util::MessageToJsonString` are not substitutes for FHIR-aware conversion; Google FHIR provides its own utilities.

---

# Quick Start

> [!TIP]
> ## Using Python instead of C/C++?
>
> #### **→ [Read the Python API guide](python/README.md)**
>
> Python's compiled module reuses the native core and exposes generated field
> keys, buffer access, and construction APIs. Python values and conversions can
> still allocate; consult the guide for supported operations and lifetime rules.
> 

## Build From Source Prerequisites
* **Python 3.11+** — generator only (it uses PEP 604 `X | None` annotations, so CMake
  enforces this floor; the macOS Command Line Tools ship 3.9 and fail at import).
* Clang, GCC, or MSVC with C++20 support
* CMake 3.20+ — **3.25+ to use the presets below**
* Network access (generator fetches FHIR bundles from HL7)

### Build

The presets are the supported configurations; each encodes the options its workflow needs:

```bash
cmake --preset ninja && cmake --build --preset ninja        # CLI / CI       -> build/
cmake --preset xcode && cmake --build --preset xcode        # Xcode IDE      -> build-xcode/
cmake --preset xcode-asan                                   # + ASan/UBSan   -> build-xcode-asan/
```

Or configure by hand:

```bash
cmake -S . -B build
cmake --build build --target build_all -j
```

Run the suites:

```bash
ctest --preset ninja          # C++ and Python integration tests
pytest tests/generator -q     # wire-format gate (permanent constants)
```

On first configure, CMake automatically runs the code generator — downloading FHIR R4/R5 specification bundles from HL7 and emitting strongly-typed C++ source into `generated_src/`. No manual generator step is needed.

The `build_all` target builds every enabled component: the core library (`libfastfhir`), the ingestor, the `ff_export` and `ff_compact` CLI tools, and the C++ test suite. Omit `--target build_all` to build only `libfastfhir`, `ff_export`, and `ff_compact`.

### CMake Options

| Option | Default | Description |
|---|---|---|
| `FASTFHIR_PRODUCTION_PROFILE` | `us-core` | Comma-separated resource groupings to compile; the generator builds their **union**. See [Resource groupings](#resource-groupings) below. |
| `FASTFHIR_BUILD_SHARED` | `ON` | Build `libfastfhir` as a shared library; set `OFF` for a static archive |
| `FASTFHIR_BUILD_INGESTOR` | `ON` | Build the JSON→binary ingest library and `ff_ingest` CLI (requires simdjson) |
| `FASTFHIR_BUILD_PYTHON_BINDINGS` | `OFF` | Build the pybind11 `_core` extension module |
| `FASTFHIR_BUILD_TESTS` | `ON` | Build the C++ test suite; requires `FASTFHIR_BUILD_INGESTOR` |
| `FASTFHIR_RUN_GENERATOR` | `ON` | Run the Python code generator at configure time |
| `FASTFHIR_GENERATE_ON_BUILD` | `OFF` | Re-run the generator before every build (may invalidate the PCH) |
| `FASTFHIR_ENABLE_EXTENSIONS` | `OFF` | Enable the WASM extension codec host |

Example — UK Core profile with ingestor, tests, and Python bindings:

```bash
cmake -S . -B build \
  -DFASTFHIR_PRODUCTION_PROFILE=uk-core \
  -DFASTFHIR_BUILD_INGESTOR=ON \
  -DFASTFHIR_BUILD_TESTS=ON \
  -DFASTFHIR_BUILD_PYTHON_BINDINGS=ON
cmake --build build --target build_all -j
```

#### Resource groupings

`FASTFHIR_PRODUCTION_PROFILE` takes a **comma-separated list** — real deployments
compose (a payer needs US Core *and* claims):

| Grouping | Resources | Covers |
|---|---|---|
| `us-core` *(default)* | 28 | [US Core](https://hl7.org/fhir/us/core/) — provider/EHR clinical data; realizes USCDI (defined by ONC/ASTP) |
| `uk-core` | 23 | [UK Core](https://simplifier.net/hl7fhirukcorer4) |
| `billing` | 5 | Payer/claims: `ExplanationOfBenefit`, `Claim`, `ClaimResponse`, `PaymentNotice`, `PaymentReconciliation` — the [CARIN Blue Button](https://hl7.org/fhir/us/carin-bb/) / Da Vinci PAS core |
| `medication-admin` | 1 | `MedicationAdministration` — the "was it actually given" event US Core omits from the medication chain |
| `supply` | 2 | `SupplyDelivery`, `SupplyRequest` |
| `imaging` | 1 | `ImagingStudy` — the DICOM study/series/instance structure behind a `DiagnosticReport` |
| `all` | 275 | Every concrete resource in the FHIR packages; also accepts any unknown name. **Very large build.** |

```bash
-DFASTFHIR_PRODUCTION_PROFILE=us-core,billing    # US Core + claims
-DFASTFHIR_PRODUCTION_PROFILE=us-core,uk-core    # both realms
```

`us` and `uk` remain accepted as aliases for `us-core` / `uk-core`.

**`ExplanationOfBenefit` is deliberately not in `us-core`.** US Core is
clinical/EHR scope; EOB is a payer artifact from CARIN Blue Button. Building
payer-side, use `us-core,billing`.

> [!NOTE]
> **The profile decides which resources this build can *binary-encode* — not what a stream can carry.**
> A resource outside your compiled profile uses an opaque JSON fallback rather
> than generated typed storage. This resource-level fallback is not a guarantee
> of lossless round trips for all content; extension policies still matter for
> typed resources. What the opaque fallback does not provide is *typed access*:
> with no V-Table there is no `Node` navigation, no query, and no interior compaction — it behaves like ordinary FHIR. Pick a profile for the resources you
> want FastFHIR-native; you do not have to enumerate every type in existence.

> [!NOTE]
> **Profile choice does not affect `RESOURCE` tag interoperability.**
> Every resource gets a permanent, git-tracked `RECOVERY_TAG` — plus one per nested
> BackboneElement — and tag discovery is profile-independent: the ledger
> `dictionaries/master_tags.json` covers the whole R4 ∪ R5 spec, so
> `generated_src/FF_RecoveryTags.hpp` is byte-identical whichever groupings you
> compile. Switching profile changes only which resources get C++ emitted. When a
> newer FHIR package adds types the ledger has never seen, the generator appends
> them at the next free value in their band and rewrites the ledger — an append-only
> change to a committed wire file, so review that diff. See TASKS.md A27.

See [Generator Architecture](#generator-architecture) for details on profiles and the generation pipeline.

---

# Getting Started

These three steps take you from plain FHIR JSON to a working binary FastFHIR workflow.
Start at whichever step matches your use case — you do not have to do all three.

Every `cpp` block on this page is checked by the test suite as published. Two gates, because
"it compiles" and "it works" are different claims:

| Gate | What it does | Covers |
|---|---|---|
| `ctest -R py_readme_compiles` | Extracts each block and builds it `-fsyntax-only` — C++ blocks as C++20, the C block as C11 | all 25 buildable blocks |
| `ctest -R cpp_readme_` | **Extracts and runs** the block, then asserts on the result | the 8 end-to-end examples |

The second gate matters most. The runner is generated from this file, so the code it runs is
the text above, verbatim — not a re-implementation. Change a value in an example and the
suite goes red, naming that block.

This exists because only a hand-written parallel copy used to exist
([tests/cpp/test_readme.cpp](tests/cpp/test_readme.cpp)). It proved the examples worked and
said nothing about this page, so the two drifted: every C++ block here called an API that no
longer existed while `ctest` stayed green. Running the published bytes closed that gap, and
found four more defects the compile gate could not see — including examples that parsed a
stream before it was sealed, and one that assigned a field in a way that throws.

> **Editing a block?** The HTML comment above each fence configures the gates
> (`program`, `fragment`, `expressions`, `needs=`, `requires=`, `run=`) and is invisible when
> rendered. Adding `run=<id>` makes a block execute; its fixtures and assertions go in
> `tests/readme/expect.hpp`, never on this page. `--dump <n>` on
> `tests/python/test_readme_compiles.py` prints exactly what was compiled.

---

## Step 1 — Parse raw bytes

`Parser` is the read-only entry point. Below is a minimal file-based path that opens an
existing `.ffhr` archive in read-only mode, loads it into memory, and prints fields only
when they are present.

<!-- ff-compile: program run=step1_parse -->
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

int main() {
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
return 0;
}
```

That is the complete read path.

---

## Step 2 — Create a `Memory` arena

FastFHIR's Virtual Memory Arena (VMA) is the backing store used by the `Builder` and the
streaming ingestion path. There are three flavours:

### Anonymous RAM arena (in-process only)

<!-- ff-compile: fragment -->
```cpp
#include <FastFHIR.hpp>

// Reserve a 256 MB sparse virtual-address window backed by anonymous RAM.
// Physical pages are only committed by the OS as you actually write to them.
auto mem = FastFHIR::Memory::create();
```

### File-backed arena (persistent storage)

<!-- ff-compile: fragment -->
```cpp
#include <FastFHIR.hpp>

// Map the arena straight to a file on disk.
// Every write goes directly into the OS page cache — no separate write() call needed.
// The file is created (or reopened) and grown to the requested capacity.
auto mem = FastFHIR::Memory::createFromFile("patient.ffhr");
```

### Using a FastFHIR Memory Arena

Once a `Memory` object exists, it can parse, build, or ingest FHIR resources.

<!-- ff-compile: fragment needs=arena -->
```cpp
// Parse memory like Step 1 above, but in a much easier path
FastFHIR::Parser parser(mem);
auto root = parser.root();
```

> [!TIP]
> **A `Memory` arena does more than a filestream or buffer.** A `Memory::StreamHead` lets a
> network socket stream directly into it, and multiple threads can write into the same
> archive safely without locks. If unsure, use a `FastFHIR::Memory` arena.

---

## Step 3 — Build a FastFHIR record from FHIR JSON

`FF_Builder` writes binary data into the `Memory` arena; `FF_Ingestor` converts FHIR JSON into
the binary layout for you. Together they replace the traditional parse → validate →
serialize pipeline with a single in-place ingestion pass.

<!-- ff-compile: fragment run=step3_build -->
```cpp
#include <FastFHIR.hpp>
#include <FF_FieldKeys.hpp>
#include <FF_Ingestor.hpp>

// Use an anonymous arena for this example — swap in createFromFile() to persist to disk.
auto mem = FastFHIR::Memory::create(/*Optionally provide arena upper bounds (something like 4 GB)*/);

FastFHIR::FF_BuilderCreateInfo builder_info;
builder_info.arena   = mem;
builder_info.version = FHIR_VERSION_R5;
FF_Builder builder;
FastFHIR::FF_CreateBuilder(builder_info, builder);

FastFHIR::FF_IngestorCreateInfo ingestor_info;
FF_Ingestor ingestor;
FastFHIR::FF_CreateIngestor(ingestor_info, ingestor);

// Any valid FHIR R4/R5 Patient JSON string.
std::string json = R"({
    "resourceType": "Patient",
    "id": "patient-1",
    "gender": "male",
    "name": [{"family": "Smith", "given": ["John"]}]
})";

// Ingest: converts JSON → binary in a single pass, writes into the arena.
FastFHIR::Reflective::ObjectHandle patient_handle;
Size parsed_count = 0;
FastFHIR::FF_Ingest(FastFHIR::FF_IngestInfo{
    .ingestor    = ingestor,
    .builder     = builder,
    .source_type = FF_SOURCE_FHIR_JSON,
    .payload     = json,
}, patient_handle, parsed_count);

// Enrich fields using typed resource keys.
patient_handle[FastFHIR::Fields::PATIENT::ACTIVE] = true;
patient_handle[FastFHIR::Fields::PATIENT::BIRTH_DATE] = std::string_view("1990-03-21");

// `gender` is already set — the ingest pass above encoded it from the JSON.
// NOTE: assigning a `code` field through a handle is NOT yet supported and
// throws (CAPI-16). Set code fields at ingest, as this example does.

// Seal the stream (no checksum for brevity; see API Examples for SHA-256).
FastFHIR::FF_BuilderSetRoot(FastFHIR::FF_BuilderSetRootInfo{
    .builder = builder,
    .root    = patient_handle,
});
FastFHIR::Memory::View view;          // a lifetime-safe window over the sealed bytes
FastFHIR::FF_BuilderFinalize(FastFHIR::FF_BuilderFinalizeInfo{.builder = builder}, view);

// Read it back immediately — zero copies, same arena pages.
FastFHIR::Parser parser(view.data(), view.size());
auto root              = parser.root();
std::string_view id       = root[FastFHIR::Fields::PATIENT::ID];         // "patient-1"
bool             active   = root[FastFHIR::Fields::PATIENT::ACTIVE].as<bool>();  // true
std::string_view gender   = root[FastFHIR::Fields::PATIENT::GENDER];     // "male"
// birthDate is a PACKED date/time slot — reading it as a string_view throws
// ("Node is not a string or code"). Test presence, and use print_json when the
// text itself is needed. See "Open and read a .ffhr file" below, and CAPI-4.
bool has_birthdate = static_cast<bool>(root[FastFHIR::Fields::PATIENT::BIRTH_DATE]);
std::cout << "id=" << id << "  active=" << active
          << "  gender=" << gender << "  birthDate set=" << has_birthdate << "\n";
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

<!-- ff-compile: fragment needs=readfile run=example_1_ingest -->
```cpp
#include <FastFHIR.hpp>
#include <FF_FieldKeys.hpp>
#include <openssl/sha.h>

// Map the arena straight to a file — every write goes directly to disk
auto mem = FastFHIR::Memory::createFromFile("patient.ffhr", 64 * 1024 * 1024);

FastFHIR::FF_BuilderCreateInfo builder_info;
builder_info.arena   = mem;
builder_info.version = FHIR_VERSION_R5;
FF_Builder builder;
FastFHIR::FF_CreateBuilder(builder_info, builder);

FastFHIR::FF_IngestorCreateInfo ingestor_info;
FF_Ingestor ingestor;
FastFHIR::FF_CreateIngestor(ingestor_info, ingestor);

std::vector<uint8_t> raw = open_read_only_file("patient.json");
std::string json_string(reinterpret_cast<const char*>(raw.data()), raw.size());

FastFHIR::Reflective::ObjectHandle patient_handle;
Size parsed_count = 0;
FastFHIR::FF_Ingest(FastFHIR::FF_IngestInfo{
    .ingestor    = ingestor,
    .builder     = builder,
    .source_type = FF_SOURCE_FHIR_JSON,
    .payload     = json_string,
}, patient_handle, parsed_count);

// Inspect while the stream is still open — zero heap allocations.
// Read through the handle the ingest returned, NOT through a Parser: until
// FF_BuilderSetRoot and FF_BuilderFinalize have run there is no FF_HEADER and no
// root pointer in the arena, so `Parser(mem)` fails header validation and
// FF_BuilderQuery has no root to hand back.
auto root = patient_handle.as_node();
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
FastFHIR::FF_BuilderSetRoot(FastFHIR::FF_BuilderSetRootInfo{
    .builder = builder,
    .root    = patient_handle,
});
FastFHIR::Memory::View view;
FastFHIR::FF_BuilderFinalize(FastFHIR::FF_BuilderFinalizeInfo{
    .builder   = builder,
    .algorithm = FF_CHECKSUM_SHA256,
    .hasher    = [](const unsigned char* data, size_t len) {
        std::vector<uint8_t> hash(SHA256_DIGEST_LENGTH);
        SHA256(data, len, hash.data());
        return hash;
    },
}, view);
// patient.ffhr is now a valid, portable FastFHIR archive
```

---

## 2 — Open and read a `.ffhr` file

Mount an existing archive and traverse directly via `Parser::root()`.

<!-- ff-compile: fragment run=example_2_read -->
```cpp
#include <FastFHIR.hpp>
#include <FF_FieldKeys.hpp>

// openReadOnly maps exactly the bytes on disk — never created, grown or written
auto mem    = FastFHIR::Memory::openReadOnly("patient.ffhr");
auto parser = FastFHIR::Parser(mem);
auto root   = parser.root();

// Typed resource checks (no direct RECOVERY_TAG usage required)
bool parser_says_patient = parser.is_root<FastFHIR::RESOURCETYPE::PATIENT>();
bool root_is_patient     = root.is<FastFHIR::RESOURCETYPE::PATIENT>();
if (!parser_says_patient || !root_is_patient)
    throw std::runtime_error("Expected Patient root resource");

// Scalars coerce directly to C++ types — zero heap allocations
std::string_view id     = root[FastFHIR::Fields::PATIENT::ID];           // std::string_view
std::string_view gender = root[FastFHIR::Fields::PATIENT::GENDER];       // Code Field
bool             active = root[FastFHIR::Fields::PATIENT::ACTIVE].as<bool>(); // bool
// Date/time slots (birthDate, issued, effective) are PACKED — the string-view
// reader does not decode them and throws on them ("Node is not a string or
// code"). Check presence via the slot's truthiness; use print_json when the
// text is actually required. Zero-copy date reading is tracked upstream as
// CAPI-4.
bool has_birth_date = static_cast<bool>(root[FastFHIR::Fields::PATIENT::BIRTH_DATE]); // presence

// Walk structured arrays
for (auto& name_node : root[FastFHIR::Fields::PATIENT::NAME].entries()) {
    std::string_view family = name_node[FastFHIR::Fields::HUMANNAME::FAMILY];
    for (auto& g : name_node[FastFHIR::Fields::HUMANNAME::GIVEN].entries())
        std::cout << g.as<std::string_view>() << " ";
    std::cout << family << "\n";
}

// Eagerly materialize into a generated C++ struct (strict schema validation).
// Reach for this ONLY when you need the whole resource or its strict
// validation. For a query that reads a few fields it is the anti-pattern: it
// deserializes every field of the abstraction — strings, vectors, sub-objects —
// the exact O(N) work the zero-copy read path exists to avoid (see "Reading data"
// below).
PatientData patient_data = root;

// Same API works for polymorphic resource slots (e.g. Bundle.entry.resource):
// if (resource_node.is<FastFHIR::RESOURCETYPE::OBSERVATION>()) { ... }
```

### Reading data — the zero-copy pattern

FastFHIR gives you **two independent readers over the same bytes**. Choosing
between them is the most consequential read-path decision you make:

| | how you use it | allocates? |
|---|---|---|
| **Reflective lens** | `node[FastFHIR::Fields::OBSERVATION::CODE]` | no — reads arena bytes in place |
| **Abstraction** | `node.as<ObservationData>()` | yes — copies into a struct |

The **abstraction types** are the generated data structs — `PatientData`,
`ObservationData`, `CodeableConceptData` — ordinary C++ values with public fields
and no behaviour. `node.as<T>()` and `PatientData p = node;` are the same
operation: walk the entire block and copy every field out of the arena into one.

The lens hands back a *view* instead. An `Entry`/`Node` is a coordinate into the
mapped file, so a field read is a vtable-slot lookup plus a bounds check, and the
`std::string_view` it yields points at the arena's own bytes — nothing is copied
and nothing is freed.

**Do** — lens reads: navigate to exactly the fields you need and coerce them to
`std::string_view` / scalars. Nodes are views over the arena, not copies.
Index-walk arrays (`node[i]`) instead of `entries()` when you don't need the
materialized list — the index walk allocates nothing.

**Don't** — materialize the whole resource (`PatientData p = root;`) to answer a
query that needs two fields. The whole abstraction gets deserialized — every field
the query never touches — which is precisely the O(N) work the format exists to
avoid, and it is invisible in the timing if you only measure the query. This is
the single most common misuse of the read API; it is why the benchmark's
query stage originally ran at parity with a DOM parser despite the O(1) per-field
access underneath. Whole-resource materialization is for validation and
whole-record consumers, not for field access.

Two slot kinds need care:

- **Date/time slots** (`birthDate`, `issued`, `effective`) are packed; the
  string-view reader throws on them. Check presence with the slot's truthiness
  and use `print_json` only when the text is required (CAPI-4).
- **Choice slots** (`value[x]`, `effective[x]`) carry their variant type in the
  slot's recovery tag — read `entry.target_recovery` without expanding the node.

---

## 3 — Re-open a `.ffhr` file and enrich it in place

FastFHIR's arena is memory-mapped. Variable-size updates can append new blocks
and redirect field slots rather than reserialize the whole resource. Existing
slots, entry arrays, headers, and checksum metadata may still be modified.
The OS writes dirty pages; stream growth and total bytes written are different
costs. Resealing can also read the existing stream to calculate its checksum.

<!-- ff-compile: fragment run=example_3_enrich -->
```cpp
#include <FastFHIR.hpp>
#include <FF_FieldKeys.hpp>
#include <openssl/sha.h>

auto mem = FastFHIR::Memory::createFromFile("patient.ffhr", 64 * 1024 * 1024);

FastFHIR::FF_BuilderCreateInfo builder_info;
builder_info.arena   = mem;
builder_info.version = FHIR_VERSION_R5;
FF_Builder builder;
FastFHIR::FF_CreateBuilder(builder_info, builder);

FastFHIR::FF_IngestorCreateInfo ingestor_info;
FF_Ingestor ingestor;
FastFHIR::FF_CreateIngestor(ingestor_info, ingestor);

// Obtain a mutable handle to the existing root
auto patient = builder->root_handle();

// Append or overwrite scalar fields (new bytes appended; field pointer amended)
patient[FastFHIR::Fields::PATIENT::BIRTH_DATE] = std::string_view("1990-03-21");
patient[FastFHIR::Fields::PATIENT::ACTIVE]     = true;

// Append a structured sub-object via the ingestor
FastFHIR::FF_IngestInsertAtField(FastFHIR::FF_IngestInsertInfo{
    .ingestor = ingestor,
    .parent   = patient,
    .key      = FastFHIR::Fields::PATIENT::TELECOM,
    .payload  = R"({"system":"phone","value":"555-0199","use":"mobile"})",
});

// Re-seal with an updated checksum — original data untouched, new tail written
FastFHIR::Memory::View view;
FastFHIR::FF_BuilderFinalize(FastFHIR::FF_BuilderFinalizeInfo{
    .builder   = builder,
    .algorithm = FF_CHECKSUM_SHA256,
    .hasher    = [](const unsigned char* data, size_t len) {
        std::vector<uint8_t> hash(SHA256_DIGEST_LENGTH);
        SHA256(data, len, hash.data());
        return hash;
    },
}, view);
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

<!-- ff-compile: fragment -->
```cpp
#include <FastFHIR.hpp>
#include <FF_FieldKeys.hpp>
#include <asio.hpp>

auto mem = FastFHIR::Memory::create(256 * 1024 * 1024);   // 256 MB anonymous arena

FastFHIR::FF_IngestorCreateInfo ingestor_info;
FF_Ingestor ingestor;
FastFHIR::FF_CreateIngestor(ingestor_info, ingestor);

asio::io_context io;
asio::ip::tcp::socket conn(io);
// conn is assumed to be connected (acceptor/connect bootstrap omitted here)

// ── Step 1: receive FHIR JSON directly into the arena (zero-copy ingest) ──
std::string raw_json;
{
    auto head = mem->try_acquire_stream();   // exclusive stream lock
    if (!head) throw std::runtime_error("stream busy");

    std::array<char, 65536> buf{};
    size_t n = conn.read_some(asio::buffer(buf));
    head->commit(n);
    raw_json.assign(buf.data(), n);
}

// ── Step 2: ingest and enrich ──
auto mem2 = FastFHIR::Memory::create(256 * 1024 * 1024);
FastFHIR::FF_BuilderCreateInfo builder_info;
builder_info.arena   = mem2;
builder_info.version = FHIR_VERSION_R5;
FF_Builder builder;
FastFHIR::FF_CreateBuilder(builder_info, builder);

FastFHIR::Reflective::ObjectHandle patient_handle;
Size count = 0;
FastFHIR::FF_Ingest(FastFHIR::FF_IngestInfo{
    .ingestor    = ingestor,
    .builder     = builder,
    .source_type = FF_SOURCE_FHIR_JSON,
    .payload     = raw_json,
}, patient_handle, count);

patient_handle[FastFHIR::Fields::PATIENT::ACTIVE]  = true;
// Patch a parsed sub-object into one field of an object already in the arena.
FastFHIR::FF_IngestInsertAtField(FastFHIR::FF_IngestInsertInfo{
    .ingestor = ingestor,
    .parent   = patient_handle,
    .key      = FastFHIR::Fields::PATIENT::TELECOM,
    .payload  = R"({"system":"phone","value":"555-0199","use":"mobile"})",
});

// ── Step 3: seal and send back — view reads straight from the arena ──
FastFHIR::FF_BuilderSetRoot(FastFHIR::FF_BuilderSetRootInfo{
    .builder = builder,
    .root    = patient_handle,
});
FastFHIR::Memory::View view;
FastFHIR::FF_BuilderFinalize(FastFHIR::FF_BuilderFinalizeInfo{
    .builder   = builder,
    .algorithm = FF_CHECKSUM_CRC32,
}, view);

asio::write(conn, asio::buffer(view.data(), view.size())); // zero-copy egress
```

---

## 5 — Surgically edit one patient in a 5 GB bundle and reseal

The bundle is memory-mapped, so field access need not materialize every resource.
Finding a patient can scan entries, and recalculating the stream checksum can
read the full mapped range. Appending does not require reserializing existing resources:
`FF_BundleAppendEntries` writes the new Observation, then writes the
`Bundle.entry` array (one 84-byte entry per resource) after it.
- **Array at the end of the stream** (built collect-then-serialize, Example 6a):
  the array is rewritten in place, and the stream grows by exactly the
  Observation plus one entry.
- **Array at the front** (built backfill, Example 6b): the array is moved to
  the end once.

<!-- ff-compile: fragment run=example_5_surgical -->
```cpp
#include <FastFHIR.hpp>
#include <FF_BundleAppend.hpp>
#include <FF_FieldKeys.hpp>
#include <openssl/sha.h>

// Map the entire bundle — address space reserved, pages not loaded until accessed
auto mem = FastFHIR::Memory::createFromFile("bundle.ffhr", 8ULL * 1024 * 1024 * 1024);

FastFHIR::FF_BuilderCreateInfo builder_info;
builder_info.arena   = mem;
builder_info.version = FHIR_VERSION_R5;
FF_Builder builder;
FastFHIR::FF_CreateBuilder(builder_info, builder);

FastFHIR::FF_IngestorCreateInfo ingestor_info;
FF_Ingestor ingestor;
FastFHIR::FF_CreateIngestor(ingestor_info, ingestor);

auto parser = FastFHIR::Parser(mem);
auto bundle = parser.root();

// Walk bundle.entry; the OS faults in only the pages we read.
// concrete_recovery() reads the type recorded BESIDE the offset, so the scan
// filters by resource type without dereferencing every entry.
bool found = false;
for (auto& entry_node : bundle[FastFHIR::Fields::BUNDLE::ENTRY].entries()) {
    auto resource = entry_node[FastFHIR::Fields::BUNDLE_ENTRY::RESOURCE];
    if (!resource) continue;
    if (resource.concrete_recovery() != RECOVER_FF_PATIENT) continue;
    std::string_view id = resource.as_node()[FastFHIR::Fields::PATIENT::ID];
    if (id == "patient-42") { found = true; break; }
}
if (!found) throw std::runtime_error("patient-42 not found");

// Append a new Observation to Bundle.entry. The callback writes the resource
// and hands back its entry; the existing entries keep their order.
FastFHIR::FF_BundleAppendResult appended;
FastFHIR::FF_BundleAppendEntries(FastFHIR::FF_BundleAppendInfo{
    .builder = builder,
    .append  = [&](FastFHIR::Builder_t&, std::vector<BundleentryData>& new_entries) {
        FastFHIR::Reflective::ObjectHandle obs_handle;
        Size count = 0;
        FastFHIR::FF_Ingest(FastFHIR::FF_IngestInfo{
            .ingestor    = ingestor,
            .builder     = builder,
            .source_type = FF_SOURCE_FHIR_JSON,
            .payload     = R"({
                "resourceType": "Observation",
                "status": "final",
                "code": {"coding": [{"system": "http://loinc.org", "code": "2345-7", "display": "Glucose"}]},
                "subject": {"reference": "Patient/patient-42"},
                "valueQuantity": {"value": 94.0, "unit": "mg/dL", "system": "http://unitsofmeasure.org"}
            })",
        }, obs_handle, count);
        new_entries.push_back(BundleentryData{.resource = static_cast<ResourceReference>(obs_handle)});
    },
}, appended);

// Amend the ROOT record — the handle the stream already owns
auto root_handle = builder->root_handle();
root_handle[FastFHIR::Fields::BUNDLE::TIMESTAMP] = std::string_view("2026-09-09T00:00:00Z");

// Reseal — restamps the header and writes a new checksum
FastFHIR::Memory::View view;
FastFHIR::FF_BuilderFinalize(FastFHIR::FF_BuilderFinalizeInfo{
    .builder   = builder,
    .algorithm = FF_CHECKSUM_SHA256,
    .hasher    = [](const unsigned char* data, size_t len) {
        std::vector<uint8_t> hash(SHA256_DIGEST_LENGTH);
        SHA256(data, len, hash.data());
        return hash;
    },
}, view);
// bundle.ffhr updated; 5 GB of untouched entries were never copied
```

---

## 6 — Lock-Free Concurrent Generation

FastFHIR supports **two ways to fill an array**, and both are first-class. They produce
the same logical `Bundle`, and readers cannot tell them apart. The difference is who
tracks the array while it is being filled, and where it lands in the stream.

| | **6a — Collect, then serialize** | **6b — Allocate, then backfill** |
|---|---|---|
| While filling | You track a plain `std::vector<BundleentryData>` in memory | The array already exists in the arena; each worker assigns its own slot |
| Serialized | Once, after every element is known | Up front, as N empty elements |
| Stream layout | `[resources … \| Bundle \| entry[N]]`: the array is at the **tail** | `[Bundle \| entry[N] \| resources …]`: the array is at the **front** |
| Best for | Any producer that already holds its elements as `*Data` values | Native multithreaded writes: workers never hand results back, they write them into the stream |

In a Bundle, each element is a fixed 84-byte `Bundle.entry` block that carries the
resource's `(offset, type)`. Either way, the resources themselves are appended
concurrently into one shared lock-free arena.

### 6a — Collect, then serialize

Each worker appends one `Observation` and returns its reference. The caller keeps those
references in a `std::vector<BundleentryData>`, then serializes the whole array, and the
root `Bundle` with it, in one shot at the end, before sealing with a checksum. The
parallel STL backend is typically oneTBB.

<!-- ff-compile: program run=example_6_concurrent -->
```cpp
#include <FastFHIR.hpp>
#include <FF_Bundle.hpp>
#include <FF_Observation.hpp>
#include <algorithm>
#include <execution>
#include <vector>

std::vector<uint8_t> serialize_bundle_parallel(const std::vector<ObservationData>& raw_observations) {
    auto mem = FastFHIR::Memory::create(256 * 1024 * 1024); // Allocate 256 MB VMA arena
    FastFHIR::FF_BuilderCreateInfo builder_info;
    builder_info.arena   = mem;
    builder_info.version = FHIR_VERSION_R5;
    FF_Builder builder;
    FastFHIR::FF_CreateBuilder(builder_info, builder);

    // 1) Concurrently append Observation resources into one shared lock-free stream.
    std::vector<BundleentryData> entries(raw_observations.size());
    auto to_entry = [&builder](const ObservationData& obs) -> BundleentryData {
        BundleentryData entry{};
        entry.resource = static_cast<ResourceReference>(
            FastFHIR::FF_BuilderAppendObject(builder, obs)
        );
        return entry;
    };
    // __cpp_lib_parallel_algorithm is the feature-test macro for the parallel
    // OVERLOADS. __cpp_lib_execution only promises the policy TYPES exist --
    // libc++ defines that one and provides no parallel overloads, so testing it
    // picks an overload that is not there.
#if defined(__cpp_lib_parallel_algorithm)
    std::transform(std::execution::par_unseq,
                   raw_observations.begin(), raw_observations.end(),
                   entries.begin(), to_entry);
#else
    // No parallel backend in this standard library (libc++ today). The appends
    // are lock-free either way; this loses the concurrency, not the result.
    std::transform(raw_observations.begin(), raw_observations.end(),
                   entries.begin(), to_entry);
#endif

    // 2) Assemble the Bundle root once after all parallel appends complete.
    BundleData bundle{};
    bundle.type = FF_BundleType::Collection;
    bundle.entry = std::move(entries);

    FastFHIR::FF_BuilderSetRoot(FastFHIR::FF_BuilderSetRootInfo{
        .builder = builder,
        .root    = FastFHIR::FF_BuilderAppendObject(builder, bundle),
    });
    FastFHIR::Memory::View view;
    FastFHIR::FF_BuilderFinalize(FastFHIR::FF_BuilderFinalizeInfo{
        .builder   = builder,
        .algorithm = FF_CHECKSUM_SHA256,
    }, view);

    const auto* first = reinterpret_cast<const uint8_t*>(view.data());
    return std::vector<uint8_t>(first, first + view.size());
}
```

`std::execution::par_unseq` uses your toolchain's parallel backend (commonly oneTBB on
libstdc++). Link your target against TBB (and threads) explicitly:

```cmake
find_package(TBB REQUIRED)
find_package(Threads REQUIRED)

target_link_libraries(your_target
  PRIVATE
    fastfhir
    TBB::tbb
    Threads::Threads
)
```

### 6b — Allocate, then backfill

Append the `Bundle` first with N empty entries: the array is written as one contiguous
block. Workers then append resources and assign each one straight into its own slot,
`entries[i][RESOURCE]`. No two workers touch the same slot, so nothing is collected or
merged afterwards, and the only shared state is the arena's lock-free write head. This
is the layout FastFHIR's own concurrent JSON ingestor uses.

<!-- ff-compile: program run=example_6_backfill -->
```cpp
#include <FastFHIR.hpp>
#include <FF_Bundle.hpp>
#include <FF_Observation.hpp>
#include <thread>
#include <vector>

std::vector<uint8_t> serialize_bundle_backfill(const std::vector<ObservationData>& raw_observations,
                                               unsigned workers) {
    auto mem = FastFHIR::Memory::create(256 * 1024 * 1024);
    FastFHIR::FF_BuilderCreateInfo builder_info;
    builder_info.arena   = mem;
    builder_info.version = FHIR_VERSION_R5;
    FF_Builder builder;
    FastFHIR::FF_CreateBuilder(builder_info, builder);

    // 1) Allocate: the Bundle and N empty entries, written as one contiguous array.
    BundleData bundle{};
    bundle.type  = FF_BundleType::Collection;
    bundle.entry = std::vector<BundleentryData>(raw_observations.size());
    FastFHIR::Reflective::ObjectHandle root = FastFHIR::FF_BuilderAppendObject(builder, bundle);
    FastFHIR::Reflective::ObjectHandle entries = root[FastFHIR::Fields::BUNDLE::ENTRY];

    // 2) Backfill: each worker appends its resources and writes each into its own slot.
    const size_t n = raw_observations.size();
    std::vector<std::thread> pool;
    for (unsigned w = 0; w < workers; ++w) {
        pool.emplace_back([&, w] {
            for (size_t i = n * w / workers; i < n * (w + 1) / workers; ++i) {
                entries[i][FastFHIR::Fields::BUNDLE_ENTRY::RESOURCE] =
                    FastFHIR::FF_BuilderAppendObject(builder, raw_observations[i]);
            }
        });
    }
    for (auto& t : pool) t.join();

    // 3) The Bundle was appended first, so it is already the root: set it and seal.
    FastFHIR::FF_BuilderSetRoot(FastFHIR::FF_BuilderSetRootInfo{
        .builder = builder,
        .root    = root,
    });
    FastFHIR::Memory::View view;
    FastFHIR::FF_BuilderFinalize(FastFHIR::FF_BuilderFinalizeInfo{
        .builder   = builder,
        .algorithm = FF_CHECKSUM_SHA256,
    }, view);

    const auto* first = reinterpret_cast<const uint8_t*>(view.data());
    return std::vector<uint8_t>(first, first + view.size());
}
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

> [!TIP]
> **Use compact archives when a stream is finalized, unlikely to be mutated, and will
> be stored or transmitted at scale.** Compact archives are just as fast and fully 
> traversable via `Parser` using the identical typed-key API as standard streams — 
> no code changes needed on the read side. They **should** be used for long term
> archiving of data - there is no reason not to archive if not actively editing a resource. 

### Stream Format Comparison

| Layout | Field storage | Absent fields | Traversal |
|--------|--------------|---------------|-----------|
| Standard | Fixed absolute offsets per field | Null sentinel per slot | O(1) direct jump |
| Compact | Presence bitmask + densely-packed slots | **0 bytes** | O(1) via SIMD bitmask scan |

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

<!-- ff-compile: fragment -->
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
auto compact_view = FastFHIR::Compactor::archive(FastFHIR::Compactor::ArchiveInfo{
    .source = src, .destination = compact_mem});

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

## 8 — Calling from C (`FastFHIR.h`)

Everything above is the C++ API (`FastFHIR.hpp`). There is also a **pure-C** surface,
`FastFHIR.h`, so a C program can link `libfastfhir` with no C++ compiler. It is a
separate header, not a re-export: the C++ side keeps its classes, references and
templates, and the two name the same concepts with different types. Include whichever
one your translation unit needs.

The C surface uses **opaque handles** — you only ever see a pointer, and the library owns
the object behind it. Every factory has a matching `FF_Destroy*`, every `FF_Destroy*`
accepts `NULL`, and every entry point takes a parameter struct or a handle, never a long
positional argument list. The `FF_*Info` structs mirror their `FastFHIR.hpp` counterparts
field for field, so porting between the two surfaces is renaming, not redesigning.

```c
#include <FastFHIR.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int seal_patient(void)
{
    /* 1. A builder, on an arena it owns. */
    FF_BuilderCreateInfo binfo = {0};
    binfo.capacity     = 1u << 20;
    binfo.fhir_version = FF_FHIR_R5;

    /* Every handle starts NULL and is destroyed once, at the end. FF_Destroy*
     * ignores NULL, so one cleanup path serves every way out of this function. */
    FF_BuilderHandle builder = NULL;
    FF_ObjectHandle  root    = NULL;
    FF_ViewHandle    sealed  = NULL;
    FF_ParserHandle  parser  = NULL;
    char            *out     = NULL;
    int              status  = 1;

    FF_ResultInfo r = FF_CreateBuilder(&binfo, &builder);
    if (FF_ResultFailed(&r)) { fprintf(stderr, "%s\n", r.message); goto done; }

    /* 2. Retain a resource and make it the root. The struct literal names each
     *    field, so a new field is a new line, not a new argument. */
    const char *json = "{\"resourceType\":\"Patient\",\"id\":\"p1\"}";
    r = FF_BuilderAppendOpaqueJson(
        &(FF_BuilderAppendOpaqueJsonInfo){
            .builder = builder, .json = json, .length = (uint64_t)strlen(json)},
        &root);
    if (FF_ResultFailed(&r)) { fprintf(stderr, "%s\n", r.message); goto done; }

    r = FF_BuilderSetRoot(&(FF_BuilderSetRootInfo){.builder = builder, .root = root});
    if (FF_ResultFailed(&r)) { fprintf(stderr, "%s\n", r.message); goto done; }

    /* 3. Seal. The view owns the bytes and outlives the builder. */
    r = FF_BuilderFinalize(
        &(FF_BuilderFinalizeInfo){.builder = builder, .algorithm = FF_CHECKSUM_ALGO_NONE},
        &sealed);
    if (FF_ResultFailed(&r)) { fprintf(stderr, "%s\n", r.message); goto done; }

    /* 4. Read it back: parse the sealed bytes, validate, export JSON. */
    r = FF_CreateParserFromBuffer(FF_ViewData(sealed), FF_ViewSize(sealed), &parser);
    if (FF_ResultFailed(&r)) { fprintf(stderr, "%s\n", r.message); goto done; }

    r = FF_ValidateStream(parser);
    if (FF_ResultFailed(&r)) { fprintf(stderr, "%s\n", r.message); goto done; }

    /* Export is two calls: one to size, one to fill a buffer of that size. */
    uint64_t needed = 0;
    r = FF_ExportJson(&(FF_ExportJsonInfo){.parser = parser}, &needed);
    if (FF_ResultFailed(&r)) { fprintf(stderr, "%s\n", r.message); goto done; }

    out = (char *)malloc((size_t)needed);
    if (out) {
        r = FF_ExportJson(&(FF_ExportJsonInfo){.parser = parser, .buffer = out,
                                               .capacity = needed}, &needed);
        if (FF_ResultFailed(&r)) { fprintf(stderr, "%s\n", r.message); goto done; }
        puts(out);
    }
    status = 0;

done:
    free(out);
    FF_DestroyParser(parser);
    FF_DestroyView(sealed);
    FF_DestroyObjectHandle(root);
    FF_DestroyBuilder(builder);
    return status;
}
```

Two things to know:

- **`FF_ResultInfo`** carries the status: check `FF_ResultSucceeded(&r)` /
  `FF_ResultFailed(&r)`. `r.message` points into a **thread-local** buffer owned by the
  library, valid only until the next FastFHIR C call on the same thread — copy it if it
  must outlive that.
- **Errors never throw across the boundary.** Everything that can fail returns an
  `FF_ResultInfo`; the `FF_Destroy*` functions and the size getters cannot fail and are
  safe on `NULL`.

The C declarations and the C++ `FF_*Info` structs are kept in lock-step by a gate
(`tests/generator/test_c_abi.py`): the shared facts — struct fields and enum values —
live once in `tests/generator/c_abi_spec.py` and are checked against both headers. A
worked consumer is compiled and run as part of `install_smoke`.

---

# Command Line Interface Tools

FastFHIR ships three standalone command-line tools. All three build by default:

| Tool | Requires | Binary name |
|---|---|---|
| Ingestor | `FASTFHIR_BUILD_INGESTOR` *(default `ON`)* | `ff_ingest` |
| Exporter | _(always built)_ | `ff_export` |
| Compactor | _(always built; OpenSSL for SHA-256 re-sealing)_ | `ff_compact` |

> `ff_ingest` depends on **simdjson**, which is why it sits behind
> `FASTFHIR_BUILD_INGESTOR` — that option defaults to `ON`, so set it `OFF` if you want a
> parse/export-only build without the JSON ingest path.

```bash
# Everything (recommended)
cmake --preset ninja && cmake --build --preset ninja

# Or just the tools
cmake --build build --target ff_ingest ff_export ff_compact -j
```

---

## `ff_ingest`

Converts a FHIR JSON record into a sealed FastFHIR binary stream.

```
Usage: ff_ingest [input | -]  [-o output.ffhr]
```

| Argument | Description |
|---|---|
| `input` | Path to a FHIR JSON file. Use `-` or omit to read from stdin. |
| `-o output` | Path to write the `.ffhr` binary. Omit to write to stdout. |
| `-h, --help` | Show help. |

```bash
# Ingest from a file
./ff_ingest patient.json -o patient.ffhr

# Pipeline: download → ingest → store
curl -s https://example.com/bundle.json | ./ff_ingest -o bundle.ffhr
```

---

## `ff_export`

Converts a sealed FastFHIR stream back to minified JSON.

```
Usage: ff_export [-i input.ffhr]  [-o output.json]
```

| Argument | Description |
|---|---|
| `-i input` | Path to a `.ffhr` file. Omit to read from stdin. |
| `-o output` | Path to write JSON output. Omit to write to stdout. |
| `-h` | Show help. |

```bash
# Export to a file
./ff_export -i patient.ffhr -o patient.json

# Pipeline: ingest → export round-trip
cat patient.json | ./ff_ingest | ./ff_export
```

---

## `ff_compact`

Compacts a sealed FastFHIR standard stream into a dense presence-bitmap compact archive and re-seals it. The input file is never modified.

```
Usage: ff_compact [input.ffhr | -]  [-o output | -o -]  [--no-checksum]
```

| Argument | Description |
|---|---|
| `input` | Path to a sealed `.ffhr` stream. Use `-` or omit to read from stdin. |
| `-o output` | Output path. Omit to auto-derive `<stem>.compact.ffhr`. Use `-o -` to force stdout. |
| `--no-checksum` | Skip SHA-256 re-seal (checksum field left zero). |
| `-h, --help` | Show help. |

When OpenSSL is present at build time, the compact archive is automatically re-sealed with a SHA-256 checksum. Pass `--no-checksum` to suppress this.

```bash
# Compact a file — writes patient.compact.ffhr
./ff_compact patient.ffhr

# Explicit output path
./ff_compact patient.ffhr -o archive.compact.ffhr

# Full pipeline: ingest → compact → export
cat bundle.json | ./ff_ingest | ./ff_compact | ./ff_export > bundle.json
```

> Compact archives are immutable. To modify a compact archive, open the original standard
> stream, enrich it, re-finalize, and re-compact.

---

# Reference

### Typed Resource Keys — `FastFHIR::Fields::<RESOURCE>::<FIELD>`

For C++ code, prefer typed resource keys. They carry owner recovery, field kind, and byte offset metadata for safer mutation and traversal.

<!-- ff-compile: expressions -->
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

### Code Assignment Semantics

When a string is stored into a coded field (for example `Patient.gender`,
`Observation.status`, or `Coding.code`), FastFHIR encodes it using this order:

1. Dictionary lookup (`FF_GetDictionaryCode`)
2. Custom string fallback (`FF_STRING` + relative pointer)
3. Null sentinel (`FF_CODE_NULL`) for empty input

> ⚠ **This happens on the INGEST path today, not through a mutable handle.**
> `handle[Fields::PATIENT::GENDER] = std::string_view("male")` **throws**
> — `MutableEntry::operator=` has a special case for `FF_FIELD_DATETIME` and none
> for `FF_FIELD_CODE`, so the code slot falls through to the generic
> pointer-patch path and fails its schema check. There is no `amend_code` on the
> builder. Tracked as **CAPI-16**; the three blocks below describe the encoding
> the ingestor performs and the API that assignment should acquire, so they are
> compiled but not executed.

#### 1) Dictionary lookup

FastFHIR first attempts to map the incoming code string using
`FF_GetDictionaryCode`. If found, the field stores that dictionary ID directly.

<!-- ff-compile: fragment needs=handles -->
```cpp
patient_handle[FastFHIR::Fields::PATIENT::GENDER] = std::string_view("male");
// "male" is typically dictionary-resolved and stored as a dict_code
```

#### 2) Custom string fallback

If lookup fails, FastFHIR writes the raw string into the arena as an `FF_STRING`
block, computes the relative offset from the current data block to that string,
and stores that offset with `FF_CODED_VALUE_FLAG` (`0x80000000`) set in the MSB.

This marks the value as a custom-string reference instead of a dictionary ID.

<!-- ff-compile: fragment needs=handles -->
```cpp
patient_handle[FastFHIR::Fields::PATIENT::GENDER] = std::string_view("org-local-code-91827");
// Not in dictionary -> stored as CodeableConcept block with FF_CODED_VALUE_FLAG
```

#### 3) Null handling

If the assigned string is empty, FastFHIR stores `FF_CODE_NULL`.

<!-- ff-compile: fragment needs=handles -->
```cpp
patient_handle[FastFHIR::Fields::PATIENT::GENDER] = std::string_view("");
// Stored as FF_CODE_NULL
```

#### Read path (inverse operation)

On read, converting a code field to `std::string_view` performs the inverse
resolution order:

1. Try dictionary resolution via `FF_ResolveCode`
2. If unresolved, check `FF_CODED_VALUE_FLAG` and follow the pointer — signed
   and **relative to the containing block** — to the `FF_CODED_VALUE` block
3. Decode that block per its system discriminator and return the label

The relative pointer is resolved while the containing block is still known: on
the fast path by `Entry`, which holds both coordinates, and otherwise at node
construction. A `Node` keeps only its own offset, so nothing downstream of that
point can redo the arithmetic.

This is why code fields can be assigned with normal strings while still keeping
fast dictionary-backed storage for known values.

### Date/Time Assignment Semantics

> **Status:** the representation and its primitives are implemented and
> unit-tested, but **not yet wired into the generator**. `date`, `dateTime`,
> `instant` and `time` are still stored as strings today — `Patient.birthDate`
> and `Observation.issued` below are both `FF_FIELD_STRING` in the current
> generated headers. The section describes the encoding they move to; **the
> assignment and read API does not change when they do**, only the bytes.
> Tracked as DT-2/DT-3 in `TASKS.md`.

`date`, `dateTime`, `instant` and `time` use an 8-byte slot that works exactly
like the 4-byte code slot above — same discriminator idea, same relative-offset
rule, same null convention, one width up:

1. Packed inline (the fast path, MSB clear)
2. Original-text fallback (`FF_STRING` + relative pointer, MSB set)
3. Null sentinel (`FF_DATETIME_NULL`) for empty input

#### 1) Packed inline

The value is packed into 63 bits as **civil time plus precision plus UTC
offset** — not an instant. FHIR forces this: `"2024"` is not
`"2024-01-01T00:00:00Z"`, `date` never carries a timezone, `time` has no date,
and a leap second (`:60`) is legal and must survive. Comparison for equality
becomes an integer compare instead of a string compare, and a value costs 8
bytes instead of an 8-byte pointer plus a 14-byte block header plus the text.

<!-- ff-compile: fragment needs=handles -->
```cpp
patient_handle[FastFHIR::Fields::PATIENT::BIRTH_DATE] = std::string_view("1969-07-20");
obs_handle[FastFHIR::Fields::OBSERVATION::ISSUED]     = std::string_view("2024-01-15T13:45:30Z");
// Packed inline: no child block, no pointer chase.
```

Precision round-trips, so a partial date stays partial — `"2024"` reads back as
`"2024"`, never as a fabricated January 1st. So does the spelling of a zero
offset: `Z` and `+00:00` are the same instant and stay distinct texts.

#### 2) Original-text fallback

If the value cannot be packed — more than three fractional digits, or text that
is not legal for that FHIR type — FastFHIR writes the **original string** into
the arena as an `FF_STRING`, and stores the relative offset with
`FF_DATETIME_FALLBACK_FLAG` (bit 63) set, exactly as an unknown code is stored.

<!-- ff-compile: fragment needs=handles -->
```cpp
obs_handle[FastFHIR::Fields::OBSERVATION::ISSUED] = std::string_view("2024-01-15T13:45:30.123456Z");
// 6 fractional digits -> FF_STRING fallback; the text is preserved byte-for-byte.
```

The round trip is byte-exact on **both** paths. The fallback is not a data-loss
path and not an error path: unparseable text is preserved rather than rejected,
for the same reason an unknown code becomes a block instead of an exception.

#### 3) Null handling

An empty string stores `FF_DATETIME_NULL` (all ones), the 8-byte counterpart of
`FF_CODE_NULL`.

#### Read path (inverse operation)

1. All ones → the field is absent
2. Bit 63 clear → unpack the civil fields and render at the stored precision
3. Bit 63 set → sign-extend the relative offset, follow it to the `FF_STRING`,
   and return the original text

Full layout, rationale, and the constraints that fix the epoch and field widths
are in [architecture.md §6.3](architecture.md).

### Polymorphic Choice Assignment and Readback

FastFHIR choice fields (`[x]`, for example `Observation.value[x]`) use a fixed
binary slot plus reflection-aware resolution.

#### Binary representation

Each choice slot is 10 bytes:

- 8-byte value area
- 2-byte `RECOVERY_TAG` indicating the concrete active type

The 8-byte value area is interpreted by tag:

- Inline scalar path: stores raw scalar bits directly (`bool`, integer, `double`)
- Complex path: stores an 8-byte absolute offset to the target data block
  (`FF_STRING`, `Quantity`, `Address`, etc.)

#### Assignment (write path)

When ingesting/building a choice field, FastFHIR follows this sequence:

1. Type detection: key suffix determines concrete member (for example
    `valueBoolean`, `valueQuantity`, `valueString`)
2. Staging: value + type are staged (commonly through a `ChoiceEntry`
    `std::variant` + `RECOVERY_TAG`)
3. Safe amendment: `Builder::amend_variant` writes the 8-byte value area and
    2-byte tag; for complex payloads, the object/string block is appended first
    and its absolute offset is written into the slot
4. Compact mode: compact archives select slot width by concrete payload
    representation (native scalar widths for scalar payloads, offset payload for
    complex values)

Concrete examples:

<!-- ff-compile: fragment needs=handles -->
```cpp
// Scalar choice assignment (Observation.valueBoolean)
observation_handle[FastFHIR::Fields::OBSERVATION::VALUE] = true;

// Complex choice assignment (Observation.valueString)
observation_handle[FastFHIR::Fields::OBSERVATION::VALUE] = std::string_view("normal");

// Complex object choice assignment (Observation.valueQuantity)
QuantityData q{};
q.value = 94.0;
q.unit = "mg/dL";
q.system = "http://unitsofmeasure.org";
observation_handle[FastFHIR::Fields::OBSERVATION::VALUE] = q;
```

The same storage model also applies to other choice fields such as
`Patient.deceased[x]` (`deceasedBoolean` vs `deceasedDateTime`) and
`MedicationRequest.medication[x]`.

#### Read path (parse + reflect)

Choice reads are a two-stage process:

1. Resolution (`Reflective::Node::resolve_choice`)
    - Read the slot `RECOVERY_TAG`
    - Scalar tag: return a node over the in-slot scalar bytes
    - Complex tag: follow the 8-byte offset (pointer hop) and return a node at
      the target block
2. Typed access (`Node::as<T>()`)
    - Prefer branching on `node.kind()` first (smaller, stable enum surface)
    - Use `node.recovery()` only when you need fine-grained subtype detail
    - Extract with `node.as<T>()`; runtime tag checks enforce type-safe casts

Example:

<!-- ff-compile: fragment needs=obsroot -->
```cpp
auto value_node = observation_root[FastFHIR::Fields::OBSERVATION::VALUE];

switch (value_node.kind) {
    case FF_FIELD_BOOL: {
        bool v = value_node.as<bool>();
        (void)v;
        break;
    }
    case FF_FIELD_STRING: {
        std::string_view v = value_node.as<std::string_view>();
        (void)v;
        break;
    }
    case FF_FIELD_BLOCK: {
        // Complex datatype branch (Quantity, CodeableConcept, Address, etc.).
        // Use recovery() here only if you need to distinguish concrete block types.
        QuantityData q = value_node;
        (void)q;
        break;
    }
    default:
        break;
}
```

During JSON export, the serializer uses choice-type suffix resolution (for
example `get_choice_suffix`) to emit the correct FHIR key name such as
`valueBoolean`, `valueString`, or `valueQuantity`.

### Checksum Algorithms

| Constant | Description |
|---|---|
| `FF_CHECKSUM_NONE` | No footer |
| `FF_CHECKSUM_CRC32` | CRC-32 |
| `FF_CHECKSUM_MD5` | MD5 |
| `FF_CHECKSUM_SHA256` | SHA-256 |

The `FF_HEADER` checksum offset points to an `FF_CHECKSUM` block containing the selected algorithm and a zero-copy `std::string_view` over the raw hash bytes. Header validation checks the checksum block when present.

### FHIR Versions

<!-- ff-compile: expressions -->
```cpp
FHIR_VERSION_R4   // HL7 FHIR R4
FHIR_VERSION_R5   // HL7 FHIR R5 (default)
```

---

## Extensions and modifierExtensions

> FHIR extensibility specification: [https://www.hl7.org/fhir/extensibility.html](https://www.hl7.org/fhir/extensibility.html)

FHIR `extension` and `modifierExtension` arrays contain elements whose `url` field identifies the
extension type. FastFHIR resolves each URL at ingest time and takes one of three paths, encoded in
a single 4-byte routing word called `EXT_REF` stored at the binary `FF_EXTENSION::EXT_REF` slot.

The routing decision is made once — during predigestion — and baked into the binary record.
Subsequent reads pay no URL-lookup cost at all.

| Condition | `EXT_REF` value | Stored as |
|---|---|---|
| URL resolves to a **registered WASM module** | `MSB = 1` → `MODULE_IDX` | Codec module indexed in `FF_MODULE_REGISTRY`; encoding depends on the module |
| URL is **unknown** at ingest time | `MSB = 0` → `URL_IDX` | URL indexed in `FF_URL_DIRECTORY`; full unknown payload preservation is not guaranteed |
| URL is a **known/filtered** native extension | `FF_NULL_UINT32` (`0xFFFFFFFF`) | Block is suppressed — no arena bytes written |

### EXT_REF bit layout

```
Bit 31 (MSB)  0 → URL_IDX    (index into FF_URL_DIRECTORY)
              1 → MODULE_IDX (index into FF_MODULE_REGISTRY)
Bits 30–0       index payload
0xFFFFFFFF      sentinel — extension filtered/suppressed
```

Helper predicates in `FF_Primitives.hpp`:

<!-- ff-compile: expressions needs=extref -->
```cpp
ff_ext_ref_is_module(ref)   // true  → WASM path (MSB = 1)
ff_ext_ref_is_url(ref)      // true  → passive URL path (MSB = 0, not null)
ff_ext_ref_index(ref)       // extract the lower 31-bit index
```

---

### Condition 1 — Registered WASM modules — extension codecs

The optional WASM host registers codec modules against extension URLs. A codec
provides sizing, encoding, and decoding operations for the extension. Enable
`FASTFHIR_ENABLE_EXTENSIONS` to build this subsystem and its runtime dependencies.

The **public extension registry is planned**. The intended workflow registers
uploaded JSON extension definitions, compiles versioned WASM modules, and makes
the definitions and modules available to consumers. This can reduce the private
schema interpretation and repeated codec implementation associated with custom
interfaces. Registration must also address canonical identifiers, terminology,
review, compatibility, deprecation, and historical readability.

The current remote-fetch functions in `src/FF_Extensions.cpp` are stubs; the
`registry.fastfhir.org` address in the source is not evidence of a deployed
service. Local registration and cache logic should not be confused with a
completed upload, compilation, and distribution workflow.

#### Why WebAssembly?

WASM provides a portable target for generated extension codecs, allowing a
shared host interface rather than a separately rewritten codec in every client
language. The codec representation and host boundary determine allocation,
copying, and execution costs. Performance parity with built-in fields must be
measured; it does not follow merely from compiling a module to WASM.

#### Sandbox and trust boundaries

The host uses guest linear memory and explicit codec calls to constrain data
exchange. This is a useful isolation mechanism, not a guarantee that arbitrary
community modules are safe. Host/runtime defects, exposed imports, resource
exhaustion, and incorrect codec output remain relevant risks.

Treat module provenance, allowed imports, memory/execution limits, version
pinning, output validation, and runtime security maintenance as deployment
requirements. A content hash can detect a mismatch with expected bytes; it does
not by itself authenticate the publisher or validate clinical meaning.

#### Module registration

Codec modules are registered against their extension URL before ingestion begins. Once registered,
`FF_PredigestExtensionURLs()` will route all matching URLs to that module, writing a
`MODULE_IDX`-tagged `EXT_REF` into every matching extension block.

<!-- ff-compile: fragment needs=readfile requires=extensions -->
```cpp
#include <FF_Extensions.hpp>   // FF_WasmExtensionHost — not pulled in by FastFHIR.hpp

// The bytes are AOT-compiled and copied, so the buffer need not outlive the call.
std::vector<uint8_t> wasm = open_read_only_file("codecs/us_core_race.wasm");
FastFHIR::Extensions::FF_WasmExtensionHost::get().register_module(
    "http://hl7.org/fhir/us/core/StructureDefinition/us-core-race",
    wasm.data(),
    static_cast<uint32_t>(wasm.size())
);
```

The resolution API checks registration/cache state and contains a remote-fetch
path. Remote retrieval is currently stubbed, so this example must not be read as
a working public-registry download. Check its return value and apply an explicit
policy when the required codec is unavailable:

<!-- ff-compile: fragment requires=extensions -->
```cpp
#include <FF_Extensions.hpp>   // FF_WasmExtensionHost — not pulled in by FastFHIR.hpp

// Resolves in order: in-memory cache, then the on-disk cache under
// ~/.cache/fastfhir/modules/, then the remote registry. Returns false and logs
// a warning when offline or invalid — the caller then stores the raw
// FF_EXTENSION block instead.
bool loaded = FastFHIR::Extensions::FF_WasmExtensionHost::get()
    .resolve_or_fetch_module(
        "http://hl7.org/fhir/us/core/StructureDefinition/us-core-race");
```

---

### Condition 2 — Unknown extensions — URL retention

When an extension URL has not been seen before and no WASM module is registered for it, FastFHIR
records the URL in the stream-level `FF_URL_DIRECTORY` (a chained-segment trie that deduplicates
shared URL prefixes). This preserves the extension identifier for lookup and module registration
workflows, but the current predigestion/export pipeline does **not** preserve the full unknown
extension JSON payload as an opaque blob for automatic re-emission. In other words, unknown
extension URLs can be retained, but this should not be interpreted as a **lossless round-trip**
guarantee for arbitrary, unhandled extension content.

`FF_URL_DIRECTORY` uses a chained-segment model so that many URLs sharing a common prefix (e.g.
`http://example.org/fhir/StructureDefinition/`) store that prefix only once:

```
Entry 0: prior = NONE  seg = "http://example.org/fhir/StructureDefinition"
Entry 1: prior = 0     seg = "race"      → full URL: "http://…/StructureDefinition/race"
Entry 2: prior = 0     seg = "ethnicity" → full URL: "http://…/StructureDefinition/ethnicity"
```

Reconstruct any URL at read time:

<!-- ff-compile: fragment needs=arena -->
```cpp
Parser parser(mem);
if (parser.has_url_directory()) {
    FF_URL_DIRECTORY dir = parser.url_directory();
    uint32_t n = dir.entry_count(parser.data());
    for (uint32_t i = 0; i < n; ++i)
        std::cout << dir.get_url(parser.data(), i) << "\n";
}
```

---

### Condition 3 — Filtered / suppressed extensions

Configured extension filters can suppress selected content, including fields
intended to be represented natively. Whether suppression preserves the required
clinical information must be verified for the profile and workflow; a
data-absent-reason or modifier extension is not generically disposable.
For filtered extensions, FastFHIR writes `FF_NULL_UINT32` into `EXT_REF` at predigestion time and skips the
block entirely during the ingest pass. No memory is allocated, no bytes are written to the arena,
and no pointer appears in the binary record.

The filter table is generated from the official HL7 FHIR spec bundles during code generation and
is baked into the library. Extensions can also be registered as filtered at runtime before
ingestion begins.

| Filter mode | Effect |
|---|---|
| `FILTER_ALL_KNOWN` *(default)* | Suppresses all profile-native and HL7-informational-only extensions; unknown URLs are interned and preserved |
| `FILTER_NONE` | Every URL is interned; nothing is suppressed |

At ingest time `FF_PredigestExtensionURLs()` runs before any resource data is written. It scans
the full payload, classifies each URL against the filter table, and builds the intern state
consumed by all subsequent worker threads.

`modifierExtension` elements use the same routing machinery as `extension`, but
their clinical meaning is not interchangeable: a modifier can change how the
containing element must be interpreted. Unknown-URL retention does not guarantee
payload preservation or understanding. Applications must detect unsupported
modifier extensions before relying on the affected data; reject or quarantine
when the required interpretation cannot be established.

---

## Generator Architecture

The generator lives in `generator/`. See
[`generator/README.md`](generator/README.md) for the module map and pipeline
stages, and [`dictionaries/README.md`](dictionaries/README.md) for the code-ID
rules — read that one before touching anything that assigns an ID.

| Module | Purpose |
|---|---|
| `generator/pipeline.py` | Orchestrator -- runs the full generation pipeline |
| `generator/specs.py` | FHIR spec download/extraction |
| `generator/library.py` | Library compilation driver |
| `generator/model/types.py` | Field, Block dataclasses |
| `generator/model/type_map.py` | TYPE_MAP, PRODUCTION_TYPES, STRING_TYPES |
| `generator/model/structure.py` | StructureDefinition extraction, type resolvers |
| `generator/model/merge.py` | FHIR version merging (R4/R5 unification) |
| `generator/emit/header.py` | write_if_changed, auto_header |
| `generator/emit/deserialize.py` | Eager deserializer generation |
| `generator/emit/store.py` | SIZE and STORE function generation |
| `generator/emit/views.py` | Lazy view structs, reflection dispatch |
| `generator/emit/code_ids.py` | **The permanent code-ID ledger** (`dictionaries/master_codes.json`) + `generated_src/*Dictionary*.cpp` emission |
| `generator/emit/code_names.py` | `generated_src/FF_Codes.hpp` — named code constants |
| `generator/emit/codesystems.py` | FF_CodeSystems.hpp enum generation |
| `generator/emit/traits.py` | Resource traits header |
| `generator/emit/ingest_mappings.py` | Ingest mapping generation |
| `generator/emit/extensions_known.py` | Known extensions filter table |
| `generator/emit/extensions_wasm.py` | WASM extension codec generation |
| `generator/bindings/python_fields.py` | Python field/AST/stub emission |

### Windows Build Prerequisites

OpenSSL is required for SHA-256 checksums. On Windows, install via vcpkg:

```powershell
git clone https://github.com/microsoft/vcpkg.git
cd vcpkg
.\bootstrap-vcpkg.bat
.\vcpkg install openssl --triplet x64-windows
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE="path/to/vcpkg/scripts/buildsystems/vcpkg.cmake"
cmake --build build --config Release --target fastfhir_obj -- /m:4
```

### Profile Selection

Resource scope constants live in `generator/model/type_map.py`:

| Constant | Contents |
|---|---|
| `PRODUCTION_TYPES` | Core FHIR datatypes always generated (Coding, Quantity, Identifier, Age, Count, Money, Availability, ExtendedContactDetail, …). **Not profile-dependent** — every build emits the same datatypes, so a type missing here breaks any profile whose resources reference it. |
| `US_CORE_RESOURCES` | 28 resources for US Core IG interoperability |
| `UK_CORE_RESOURCES` | 23 resources for UK Core IG interoperability |
| `BILLING_RESOURCES` | 5 payer/claims resources (CARIN Blue Button / Da Vinci PAS core) |
| `MEDICATION_ADMIN_RESOURCES` / `SUPPLY_RESOURCES` / `IMAGING_RESOURCES` | 1 / 2 / 1 — the smaller composable groupings |
| `RESOURCE_GROUPINGS` | The accepted profile names. Adding a grouping means adding one entry here; nothing else in the generator changes. |

## Design Notes
* Resource and datatype structs are generated from official HL7 StructureDefinitions.
* Version-specific fields are guarded by generated version checks.
* The top-level file container is `FF_HEADER`, which stores file magic, version, checksum offset, root offset, and payload size.
* `Parser::root()` and `Node` traversal use offset/recovery checks to validate data block integrity.
* Full recursive validation is available via typed struct assignment such as `PatientData patient = parser.root()`.

---

# License

This project is licensed under the **Mozilla Public License, v. 2.0 (MPL-2.0)** — see the
[LICENSE](LICENSE) file.

What that means in practice:

- **Use it anywhere.** You may use, link, embed, and statically compile FastFHIR into
  commercial and proprietary products with no obligations on your own code.
- **Improvements flow back.** If you modify FastFHIR's source files and distribute the
  result, those modified files must be made available under the MPL. Private divergent
  forks of the core cannot be shipped closed — this protects the interoperability of the
  `.ffhr` format for everyone.
- **The name is separate.** The MPL covers the code, not the name. "FastFHIR" and claims
  of FastFHIR compatibility are governed by the conformance policy in
  [TRADEMARK.md](TRADEMARK.md): implementations claiming compatibility must pass the
  official conformance suite. Forks are welcome under a different name.
- **Attribution** is carried in the [NOTICE](NOTICE) file — please preserve it.

Contributions are accepted under MPL-2.0 with a DCO sign-off — see
[CONTRIBUTING.md](CONTRIBUTING.md). The project roadmap lives in [TASKS.md](TASKS.md).

---
**Attribution**: The design of FastFHIR is based upon the [Iris File Extension](https://www.sciencedirect.com/science/article/pii/S2153353925000471) (by Ryan Landvater) and [FlatBuffers](https://github.com/google/flatbuffers) (by Wouter van Oortmerssen and the Google Fun Propulsion Labs team).