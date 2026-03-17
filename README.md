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

Direct generator entrypoint (equivalent):

```bash
python3 tools/generator/make_lib.py
```

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
