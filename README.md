# FastFHIR

FastFHIR is a binary serializer and code generation pipeline for HL7 FHIR resources.

It generates strongly-typed C++ structs and serialization code from official FHIR spec bundles
(currently R4 and R5), with an emphasis on:

- High performance
- Deterministic generation
- Zero-copy friendly primitives where possible
- Simple, explicit binary layout

## What This Repo Contains

- `include/`: Core C++ primitive headers (`FF_Primitives.hpp`, utilities)
- `src/`: Core C++ primitive implementations
- `tools/generator/`: Python code generation pipeline
- `generated_src/`: Generated C++ output (data types/resources/dictionary/code systems)
- `make_lib.py`: Root compatibility launcher for generation

## Prerequisites

- Python 3.9+
- Clang or GCC with C++20 support
- CMake 3.20+ (for the CMake workflow)
- Network access (generator fetches FHIR bundles from HL7)

## Quick Start

Generate all artifacts:

```bash
python3 make_lib.py
```

This command:

1. Downloads and extracts FHIR specs (R4/R5)
2. Builds master dictionary (`generated_src/FF_Dictionary.hpp`)
3. Builds code system enums (`generated_src/FF_CodeSystems.hpp`)
4. Generates data types and resources under `generated_src/`
5. Cleans temporary `fhir_specs/`


## Build with CMake

FastFHIR now includes a root `CMakeLists.txt`.

Configure and build:

```bash
cmake -S . -B build
cmake --build build -j
```

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

Root `make_lib.py` is a compatibility launcher so existing workflows continue to work.

## Design Notes

- Resource and datatype structs are generated from official StructureDefinitions
- Version-specific fields are guarded by generated version checks
- Binary layout uses explicit offsets and recovery tags
- The top-level file container is `FF_HEADER`, which stores file magic, version, checksum offset, root offset, and payload size
- String/code handling supports lock-free style emitter flow in primitives

## Common Workflow

After changing generator logic:

```bash
python3 make_lib.py
cmake -S . -B build
cmake --build build -j
```

## License

See repository license and file headers for usage and copyright details.

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

Compatibility note:

`FF_FILE_HEADER` is retained as an alias to `FF_HEADER` for compatibility with older code.
