## CMakeLists.txt for FastFHIR

cmake_minimum_required(VERSION 3.20)

# ── Version (from env or defaults) ──────────────────────────────
if(DEFINED ENV{FASTFHIR_VERSION_MAJOR})
    set(FASTFHIR_VERSION_MAJOR "$ENV{FASTFHIR_VERSION_MAJOR}")
else()
    set(FASTFHIR_VERSION_MAJOR "2026")
endif()
if(DEFINED ENV{FASTFHIR_VERSION_MINOR})
    set(FASTFHIR_VERSION_MINOR "$ENV{FASTFHIR_VERSION_MINOR}")
else()
    set(FASTFHIR_VERSION_MINOR "0")
endif()
if(DEFINED ENV{FASTFHIR_VERSION_BUILD})
    set(FASTFHIR_VERSION_BUILD "$ENV{FASTFHIR_VERSION_BUILD}")
else()
    set(FASTFHIR_VERSION_BUILD "0")
endif()

project(FastFHIR
    VERSION "${FASTFHIR_VERSION_MAJOR}.${FASTFHIR_VERSION_MINOR}.${FASTFHIR_VERSION_BUILD}"
    LANGUAGES CXX
)
message(STATUS "FastFHIR ${PROJECT_VERSION} — profile ${FASTFHIR_PRODUCTION_PROFILE}")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type (Debug|Release|RelWithDebInfo)")

# ── IDE organisation (Xcode / Visual Studio; no-op for Ninja & Make) ──
# Targets are grouped into folders instead of one flat list, and CMake's own
# bookkeeping targets (ALL_BUILD, ZERO_CHECK, install, RUN_TESTS) are pushed out
# of the way. File-level grouping is done by the source_group(TREE) call at the
# bottom of this file, which mirrors the real directory layout.
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "CMake")

# ── Options ─────────────────────────────────────────────────────
option(FASTFHIR_BUILD_SHARED       "Build as shared library" ON)
option(FASTFHIR_BUILD_INGESTOR     "Build JSON Ingestor (requires simdjson)" ON)
option(FASTFHIR_BUILD_PYTHON_BINDINGS "Build Python bindings" OFF)
option(FASTFHIR_BUILD_TESTS        "Build C++ test suite" ON)
option(FASTFHIR_RUN_GENERATOR      "Run code generator at configure time" ON)
option(FASTFHIR_GENERATE_ON_BUILD  "Regenerate sources on every build" OFF)
option(FASTFHIR_ENABLE_EXTENSIONS  "Enable WASM extension codec" OFF)

set(FASTFHIR_PRODUCTION_PROFILE "us" CACHE STRING "Generator: us|uk|all")
set_property(CACHE FASTFHIR_PRODUCTION_PROFILE PROPERTY STRINGS us uk all)

# ── OpenSSL (system or ExternalProject) ──────────────────────
include(cmake/openssl.cmake)

# ── Paths ──────────────────────────────────────────────────────
set(FASTFHIR_INCLUDE_DIR   "${CMAKE_CURRENT_SOURCE_DIR}/include")
set(FASTFHIR_DICT_DIR      "${CMAKE_CURRENT_SOURCE_DIR}/dictionaries")
set(FASTFHIR_GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated_src")

# ── Source files ────────────────────────────────────────────────
file(GLOB FASTFHIR_CORE_SOURCES CONFIGURE_DEPENDS src/*.cpp)
file(GLOB FASTFHIR_DICT_SOURCES CONFIGURE_DEPENDS dictionaries/*.cpp)
file(GLOB FASTFHIR_CORE_HEADERS CONFIGURE_DEPENDS include/*.hpp)
list(REMOVE_ITEM FASTFHIR_CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/FF_Ingestor.cpp")

# ── Code generator ─────────────────────────────────────────────
# Use EXISTS (not a GLOB variable) to check whether generated files are
# present — the GLOB variable hasn't been set yet, so `NOT` on it would
# always be True on the first configure, making the generator run even
# when FASTFHIR_RUN_GENERATOR=OFF.
if(FASTFHIR_RUN_GENERATOR OR NOT EXISTS "${FASTFHIR_GENERATED_DIR}/FF_FieldKeys.hpp")
    # 3.11 is the floor pyproject.toml already targets (ruff/black target-version
    # = py311). Without it CMake takes whatever `python3` it finds first — on macOS
    # that is the Xcode Command Line Tools 3.9, which cannot evaluate the PEP 604
    # `X | None` annotations in emit/codesystems.py, emit/dictionary.py and
    # emit/extensions_known.py (the three modules that both use PEP 604 and lack
    # `from __future__ import annotations`; 15 of 25 modules lack the import, so
    # any of them could join that list silently). The failure is a TypeError at
    # import, far from its cause.
    find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env
            FASTFHIR_PRODUCTION_PROFILE=${FASTFHIR_PRODUCTION_PROFILE}
            ${Python3_EXECUTABLE} -m generator
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        RESULT_VARIABLE _GEN_RESULT
    )
    if(NOT _GEN_RESULT EQUAL 0)
        message(FATAL_ERROR "Code generator failed — see output above")
    endif()
endif()

# Single GLOB, always after the generator (or confirmed existing files).
# Only GLOB once per configure — a second GLOB on the same expression
# after the directory changed would error with "directory contents have
# changed during configuration run".
file(GLOB FASTFHIR_GENERATED_SOURCES CONFIGURE_DEPENDS generated_src/*.cpp)
file(GLOB FASTFHIR_GENERATED_HEADERS CONFIGURE_DEPENDS generated_src/*.hpp)
list(REMOVE_ITEM FASTFHIR_GENERATED_SOURCES "${FASTFHIR_GENERATED_DIR}/FF_IngestMappings.cpp")

if(NOT FASTFHIR_GENERATED_SOURCES)
    message(FATAL_ERROR "No generated sources. Enable FASTFHIR_RUN_GENERATOR or run generator manually.")
endif()
if(NOT FASTFHIR_GENERATED_HEADERS)
    message(FATAL_ERROR "No generated headers. Enable FASTFHIR_RUN_GENERATOR or run generator manually.")
endif()
# –– END generator section –––––––––––––––––––––––––––––––––––––––

# ── Core object library ─────────────────────────────────────────
add_library(fastfhir_obj OBJECT
    ${FASTFHIR_CORE_SOURCES}
    ${FASTFHIR_DICT_SOURCES}
    ${FASTFHIR_GENERATED_SOURCES}
    ${FASTFHIR_CORE_HEADERS}
    ${FASTFHIR_GENERATED_HEADERS}
)
target_include_directories(fastfhir_obj PUBLIC
    $<BUILD_INTERFACE:${FASTFHIR_INCLUDE_DIR}>
    $<BUILD_INTERFACE:${FASTFHIR_DICT_DIR}>
    $<BUILD_INTERFACE:${FASTFHIR_GENERATED_DIR}>
)
target_compile_features(fastfhir_obj PUBLIC cxx_std_20)
target_compile_definitions(fastfhir_obj PUBLIC
    FASTFHIR_VERSION_MAJOR=${FASTFHIR_VERSION_MAJOR}
    FASTFHIR_VERSION_MINOR=${FASTFHIR_VERSION_MINOR}
    FASTFHIR_VERSION_BUILD=${FASTFHIR_VERSION_BUILD}
)
if(FASTFHIR_BUILD_SHARED)
    target_compile_definitions(fastfhir_obj PRIVATE FF_BUILDING_DLL)
endif()

# ── Core library (shared or static) ─────────────────────────────
# The Xcode generator produces NO product for a target whose only sources are
# $<TARGET_OBJECTS:...> — the build reports success and silently emits nothing,
# so dependents fail at link with "no such file or directory: libfastfhir.dylib".
# Xcode requires at least one real source file per target. `fastfhir` and
# `fastfhir_ingestor` are both object-only, so both get this empty TU. Ninja and
# Makefiles do not need it and are unaffected (the file is simply empty).
set(_FF_XCODE_ANCHOR "")
if(CMAKE_GENERATOR STREQUAL "Xcode")
    set(_FF_XCODE_ANCHOR "${CMAKE_BINARY_DIR}/ff_xcode_anchor.cpp")
    file(WRITE "${_FF_XCODE_ANCHOR}"
         "// Generated by CMakeLists.txt: empty TU so the Xcode generator emits a\n"
         "// product for object-only library targets. Not part of the source tree.\n")
endif()

function(_ff_configure_core_library target_name)
    target_compile_features(${target_name} PUBLIC cxx_std_20)
    target_include_directories(${target_name} PUBLIC
        $<BUILD_INTERFACE:${FASTFHIR_INCLUDE_DIR}>
        $<BUILD_INTERFACE:${FASTFHIR_DICT_DIR}>
        $<BUILD_INTERFACE:${FASTFHIR_GENERATED_DIR}>
        $<INSTALL_INTERFACE:include>
    )
endfunction()

if(FASTFHIR_BUILD_SHARED)
    set(_LIBTYPE SHARED)
    set(_FF_XCODE_EXTRA_LIBTYPE STATIC)
    set(_FF_XCODE_EXTRA_TARGET fastfhir_static)
else()
    set(_LIBTYPE STATIC)
    set(_FF_XCODE_EXTRA_LIBTYPE SHARED)
    set(_FF_XCODE_EXTRA_TARGET fastfhir_shared)
endif()
add_library(fastfhir ${_LIBTYPE} $<TARGET_OBJECTS:fastfhir_obj> ${_FF_XCODE_ANCHOR})
add_library(FastFHIR::fastfhir ALIAS fastfhir)
_ff_configure_core_library(fastfhir)
if(WIN32 AND FASTFHIR_BUILD_SHARED)
    set_target_properties(fastfhir PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()
if(CMAKE_GENERATOR STREQUAL "Xcode")
    add_library(${_FF_XCODE_EXTRA_TARGET} ${_FF_XCODE_EXTRA_LIBTYPE}
        $<TARGET_OBJECTS:fastfhir_obj>
        ${_FF_XCODE_ANCHOR}
    )
    _ff_configure_core_library(${_FF_XCODE_EXTRA_TARGET})
endif()

# ── WAMR extensions (optional) ──────────────────────────────────
if(FASTFHIR_ENABLE_EXTENSIONS)
    include(FetchContent)
    FetchContent_Declare(wamr
        GIT_REPOSITORY https://github.com/bytecodealliance/wasm-micro-runtime.git
        GIT_TAG WAMR-2.2.0 GIT_SHALLOW ON
    )
    set(WAMR_BUILD_INTERP ON WAMR_BUILD_AOT ON WAMR_BUILD_LIBC_BUILTIN ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(wamr)
    target_sources(fastfhir_obj PRIVATE src/FF_Extensions.cpp)
    target_compile_definitions(fastfhir_obj PUBLIC FASTFHIR_ENABLE_EXTENSIONS)
    target_include_directories(fastfhir_obj PRIVATE ${wamr_SOURCE_DIR}/core/iwasm/include)
    target_link_libraries(fastfhir_obj PUBLIC vmlib)
endif()

# ── Ingestor module ─────────────────────────────────────────────
if(FASTFHIR_BUILD_INGESTOR)
    include(FetchContent)
    set(SIMDJSON_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(SIMDJSON_JUST_LIBRARY ON CACHE BOOL "" FORCE)
    FetchContent_Declare(simdjson
        GIT_REPOSITORY https://github.com/simdjson/simdjson.git
        GIT_TAG v3.9.5
    )
    FetchContent_MakeAvailable(simdjson)

    add_library(fastfhir_ingest_obj OBJECT
        src/FF_Ingestor.cpp
        ${FASTFHIR_GENERATED_DIR}/FF_IngestMappings.cpp
    )
    target_include_directories(fastfhir_ingest_obj PUBLIC
        ${FASTFHIR_INCLUDE_DIR} ${FASTFHIR_DICT_DIR} ${FASTFHIR_GENERATED_DIR}
    )
    target_link_libraries(fastfhir_ingest_obj PUBLIC fastfhir_obj simdjson::simdjson)

    # Always static — small convenience wrapper, not a real DLL boundary.
    # Keeping it SHARED on Windows requires dllexport annotations or
    # WINDOWS_EXPORT_ALL_SYMBOLS just to produce the import .lib.
    add_library(fastfhir_ingestor STATIC
        $<TARGET_OBJECTS:fastfhir_ingest_obj>
        ${_FF_XCODE_ANCHOR}   # see the note at the `fastfhir` target above
    )
    target_link_libraries(fastfhir_ingestor PUBLIC fastfhir simdjson::simdjson)
    add_library(FastFHIR::ingestor ALIAS fastfhir_ingestor)

    add_executable(ff_ingest tools/ingestor/FF_Ingest.cpp)
    target_include_directories(ff_ingest PRIVATE ${FASTFHIR_INCLUDE_DIR} ${FASTFHIR_DICT_DIR} ${FASTFHIR_GENERATED_DIR})
    target_link_libraries(ff_ingest PRIVATE fastfhir_ingestor OpenSSL::Crypto)
endif()

# ── CLI tools ───────────────────────────────────────────────────
add_executable(ff_export tools/exporter/FF_Export.cpp)
target_include_directories(ff_export PRIVATE ${FASTFHIR_INCLUDE_DIR} ${FASTFHIR_DICT_DIR} ${FASTFHIR_GENERATED_DIR})
target_link_libraries(ff_export PRIVATE $<TARGET_OBJECTS:fastfhir_obj>)

add_executable(ff_compact tools/compactor/FF_Compact.cpp)
target_include_directories(ff_compact PRIVATE ${FASTFHIR_INCLUDE_DIR} ${FASTFHIR_DICT_DIR} ${FASTFHIR_GENERATED_DIR})
target_link_libraries(ff_compact PRIVATE $<TARGET_OBJECTS:fastfhir_obj> OpenSSL::Crypto)
target_compile_definitions(ff_compact PRIVATE FASTFHIR_HAS_OPENSSL)

# ── C++ tests ──────────────────────────────────────────────────
if(FASTFHIR_BUILD_TESTS)
    # enable_testing() alone, deliberately: include(CTest) additionally defines the
    # dashboard targets (Continuous, Experimental, Nightly, NightlyMemoryCheck),
    # which this project never uses and which show up as four more Xcode schemes.
    enable_testing()

    # Helper: create a cpp test executable + CTest entry
    function(add_ff_cpp_test NAME SOURCE)
        add_executable(${NAME} ${SOURCE})
        target_include_directories(${NAME} PRIVATE
            ${FASTFHIR_INCLUDE_DIR} ${FASTFHIR_DICT_DIR} ${FASTFHIR_GENERATED_DIR}
        )
        target_compile_definitions(${NAME} PRIVATE
            FF_TEST_ARTIFACT_DIR="${CMAKE_BINARY_DIR}/tests/cpp"
        )
        target_link_libraries(${NAME} PRIVATE fastfhir_obj)
    endfunction()

    # Asio for HTTP round-trip tests
    find_path(ASIO_INCLUDE_DIR asio.hpp)
    if(NOT ASIO_INCLUDE_DIR)
        FetchContent_Declare(asio
            GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
            GIT_TAG asio-1-30-2
        )
        FetchContent_MakeAvailable(asio)
        set(ASIO_INCLUDE_DIR "${asio_SOURCE_DIR}/asio/include")
    endif()

    # Synthea sample data
    option(FASTFHIR_DOWNLOAD_SYNTHEA "Download Synthea sample data" ON)
    set(_SYNTHEA_DIR "${CMAKE_CURRENT_BINARY_DIR}/synthea_fhir_r4")
    if(FASTFHIR_DOWNLOAD_SYNTHEA AND NOT EXISTS "${_SYNTHEA_DIR}/fhir")
        file(DOWNLOAD
            "https://synthetichealth.github.io/synthea-sample-data/downloads/latest/synthea_sample_data_fhir_latest.zip"
            "${CMAKE_CURRENT_BINARY_DIR}/synthea_fhir_latest.zip" SHOW_PROGRESS
            STATUS _DL)
        list(GET _DL 0 _DL_RES)
        if(_DL_RES EQUAL 0)
            file(ARCHIVE_EXTRACT INPUT "${CMAKE_CURRENT_BINARY_DIR}/synthea_fhir_latest.zip"
                 DESTINATION "${_SYNTHEA_DIR}")
            message(STATUS "Synthea data extracted to ${_SYNTHEA_DIR}")
        endif()
    endif()

    # ── test_readme (full examples suite) ──────────────────────────
    add_executable(ff_test_readme tests/cpp/test_readme.cpp)
    target_include_directories(ff_test_readme PRIVATE
        ${FASTFHIR_INCLUDE_DIR} ${FASTFHIR_DICT_DIR} ${FASTFHIR_GENERATED_DIR} ${ASIO_INCLUDE_DIR}
    )
    target_compile_definitions(ff_test_readme PRIVATE
        ASIO_STANDALONE
        FASTFHIR_TEST_ARTIFACT_DIR="${CMAKE_BINARY_DIR}/tests/cpp"
        $<$<BOOL:${FASTFHIR_DOWNLOAD_SYNTHEA}>:FASTFHIR_SYNTHEA_DIR="${_SYNTHEA_DIR}">
    )
    if(WIN32)
        target_compile_definitions(ff_test_readme PRIVATE _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX)
    endif()
    target_link_libraries(ff_test_readme PRIVATE fastfhir_ingestor OpenSSL::Crypto)
    # ── Round-trip harness (invoked by Python DOM parity tests) ──
    add_executable(ff_roundtrip tests/cpp/ff_roundtrip.cpp)
    target_include_directories(ff_roundtrip PRIVATE
        ${FASTFHIR_INCLUDE_DIR} ${FASTFHIR_DICT_DIR} ${FASTFHIR_GENERATED_DIR}
    )
    target_link_libraries(ff_roundtrip PRIVATE fastfhir_ingestor OpenSSL::Crypto)

    if(WIN32)
        target_link_libraries(ff_test_readme PRIVATE ws2_32)
    endif()

    # ── Unit tests ─────────────────────────────────────────────────
    add_ff_cpp_test(ff_test_primitives tests/cpp/test_primitives.cpp)
    add_ff_cpp_test(ff_test_memory     tests/cpp/test_memory.cpp)
    add_ff_cpp_test(ff_test_amend      tests/cpp/test_amend.cpp)
    add_ff_cpp_test(ff_test_cc         tests/cpp/test_codeable_concept.cpp)
    add_ff_cpp_test(ff_test_bundle     tests/cpp/test_bundle_ingest.cpp)
    # Unlike the other standalone suites this one drives the JSON ingestor.
    target_link_libraries(ff_test_bundle PRIVATE fastfhir_ingestor simdjson::simdjson)
    add_ff_cpp_test(ff_test_simd       tests/cpp/test_simd.cpp)

    # ── CTest entries ──────────────────────────────────────────────
    # Standalone self-contained suites. These were built but never registered,
    # so they compiled and never ran; add_ff_cpp_test only creates the target.
    foreach(_standalone ff_test_primitives ff_test_memory ff_test_simd ff_test_amend ff_test_cc ff_test_bundle)
        add_test(NAME "cpp_${_standalone}" COMMAND ${_standalone})
    endforeach()

    macro(_add_cpp_test NAME FILTER)
        add_test(NAME "cpp_${NAME}"
            COMMAND ff_test_readme --filter "${FILTER}")
    endmacro()

    _add_cpp_test(getting_started "Getting Started — Step 2 -> Step 3 -> Step 1")
    _add_cpp_test(test_1  "Example 1 — Ingest patient.json → save patient.ffhr")
    _add_cpp_test(test_2  "Example 2 — Open and read patient.ffhr")
    _add_cpp_test(test_3  "Example 3 — Re-open patient.ffhr and enrich in place")
    _add_cpp_test(test_4  "Example 4 — In-memory ingest, enrich, finalize, re-parse")
    _add_cpp_test(test_5  "Example 5 — Surgically edit patient in a bundle and reseal")
    _add_cpp_test(test_6  "Example 6 — Lock-free concurrent generation")
    _add_cpp_test(test_7  "Example 7 — Post-finalize archival compaction")
    _add_cpp_test(test_8  "Example 8 — Standard array-tagged field key coverage")
    _add_cpp_test(test_9  "Example 9 — Compact nested choice/resource coverage")
    _add_cpp_test(test_10 "Example 10 — Reuse patient.ffhr for another surgical edit")
    _add_cpp_test(test_11 "Example 11 — Extension URL directory filtering and reconstruction")
    _add_cpp_test(test_synthea "Example 12 — Synthea R4 patient bundle ingest and round-trip")

    # Sequential dependency chain
    set_tests_properties(cpp_test_2  PROPERTIES DEPENDS cpp_test_1)
    set_tests_properties(cpp_test_3  PROPERTIES DEPENDS cpp_test_2)
    set_tests_properties(cpp_test_7  PROPERTIES DEPENDS cpp_test_3)
    set_tests_properties(cpp_test_8  PROPERTIES DEPENDS cpp_test_1)
    set_tests_properties(cpp_test_9  PROPERTIES DEPENDS cpp_test_5)
    set_tests_properties(cpp_test_10 PROPERTIES DEPENDS cpp_test_3)

    # Resource locks for concurrent ctest -jN
    set_tests_properties(cpp_getting_started
        cpp_test_1 cpp_test_2 cpp_test_3 cpp_test_7 cpp_test_8 cpp_test_9 cpp_test_10 cpp_test_11
        PROPERTIES RESOURCE_LOCK ff_cpp_patient_ffhr)
    set_tests_properties(cpp_test_5 cpp_test_6
        PROPERTIES RESOURCE_LOCK ff_cpp_bundle_ffhr)

    # ── Python tests ───────────────────────────────────────────────
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_FOUND)
        set(_PY_DIR "${CMAKE_SOURCE_DIR}/tests/python")
        if(WIN32)
            set(_PY "${CMAKE_SOURCE_DIR}/.venv/Scripts/python.exe")
        else()
            set(_PY "${CMAKE_SOURCE_DIR}/.venv/bin/python")
        endif()
        if(NOT EXISTS "${_PY}")
            set(_PY "${Python3_EXECUTABLE}")
        endif()

        # Setup: clean artifacts
        add_test(NAME py_setup
            COMMAND "${_PY}" -c
                "import glob,os,tempfile; d=os.path.join(tempfile.gettempdir(),'fastfhir_test_artifacts'); os.makedirs(d,exist_ok=True); [os.remove(p) for p in glob.glob(os.path.join(d,'*.ffhr')) if os.path.isfile(p)]"
        )

        # Python test entries
        macro(_add_py_test NAME FN)
            add_test(NAME "py_${NAME}"
                COMMAND "${_PY}" -m pytest "${_PY_DIR}/test_readme.py::${FN}" -v)
        endmacro()

        _add_py_test(getting_started test_getting_started)
        foreach(N RANGE 1 10)
            _add_py_test("test_${N}" "test_${N}")
        endforeach()

        set_tests_properties(py_getting_started PROPERTIES DEPENDS py_setup)
        set_tests_properties(py_test_1          PROPERTIES DEPENDS py_getting_started)
        set_tests_properties(py_test_2          PROPERTIES DEPENDS py_test_1)
        set_tests_properties(py_test_3          PROPERTIES DEPENDS py_test_2)
        set_tests_properties(py_test_4          PROPERTIES DEPENDS py_test_1)
        set_tests_properties(py_test_5          PROPERTIES DEPENDS py_test_3)
        set_tests_properties(py_test_6          PROPERTIES DEPENDS py_test_1)
        set_tests_properties(py_test_7          PROPERTIES DEPENDS py_setup)
        set_tests_properties(py_test_8          PROPERTIES DEPENDS py_test_5)
        set_tests_properties(py_test_9          PROPERTIES DEPENDS "py_test_3;py_test_6")
        set_tests_properties(py_test_10         PROPERTIES DEPENDS py_setup)
        # Round-trip DOM parity test (Synthea fixtures)
        add_test(NAME py_roundtrip
            COMMAND "${_PY}" "${_PY_DIR}/test_roundtrip.py"
                --synthea-dir "${_SYNTHEA_DIR}"
                --harness "${CMAKE_CURRENT_BINARY_DIR}/ff_roundtrip"
        )
        set_tests_properties(py_roundtrip PROPERTIES
            DEPENDS "py_setup;ff_roundtrip"
        )


        set_tests_properties(py_setup py_getting_started
            py_test_1 py_test_2 py_test_3 py_test_4 py_test_5 py_test_7 py_test_8 py_test_9
            PROPERTIES RESOURCE_LOCK ff_py_patient_ffhr)
        set_tests_properties(py_setup
            py_test_6 py_test_9 py_test_10
            PROPERTIES RESOURCE_LOCK ff_py_bundle_ffhr)
    endif()
endif()

# ── Python bindings ────────────────────────────────────────────
if(FASTFHIR_BUILD_PYTHON_BINDINGS)
    find_package(Python3 REQUIRED COMPONENTS Interpreter Development)
    set(PYBIND11_FINDPYTHON ON)
    FetchContent_Declare(pybind11
        GIT_REPOSITORY https://github.com/pybind/pybind11.git
        GIT_TAG v2.11.1 GIT_SHALLOW ON
    )
    FetchContent_MakeAvailable(pybind11)

    pybind11_add_module(fastfhir_python
        $<TARGET_OBJECTS:fastfhir_obj>
        $<TARGET_OBJECTS:fastfhir_ingest_obj>
        python/FF_PythonBindings.cpp
    )
    set_property(TARGET fastfhir_python PROPERTY OUTPUT_NAME _core)
    target_include_directories(fastfhir_python PRIVATE
        ${FASTFHIR_INCLUDE_DIR} ${FASTFHIR_DICT_DIR} ${FASTFHIR_GENERATED_DIR}
    )
    target_compile_definitions(fastfhir_python PRIVATE
        FASTFHIR_VERSION_MAJOR=${FASTFHIR_VERSION_MAJOR}
        FASTFHIR_VERSION_MINOR=${FASTFHIR_VERSION_MINOR}
        FASTFHIR_VERSION_BUILD=${FASTFHIR_VERSION_BUILD}
    )
    target_link_libraries(fastfhir_python PRIVATE simdjson::simdjson OpenSSL::Crypto)

    install(FILES python/fastfhir/__init__.py
        DESTINATION ${CMAKE_INSTALL_PYTHON_LIBDIR}/fastfhir)
    install(DIRECTORY generated_src/python/fields
        DESTINATION ${CMAKE_INSTALL_PYTHON_LIBDIR}/fastfhir)
    install(TARGETS fastfhir_python
        DESTINATION ${CMAKE_INSTALL_PYTHON_LIBDIR}/fastfhir)
endif()

# ── Install rules ──────────────────────────────────────────────
install(TARGETS fastfhir ff_export EXPORT FastFHIRTargets
    LIBRARY DESTINATION lib ARCHIVE DESTINATION lib RUNTIME DESTINATION bin
    INCLUDES DESTINATION include
)
install(FILES
    ${FASTFHIR_INCLUDE_DIR}/FastFHIR.hpp
    ${FASTFHIR_INCLUDE_DIR}/FF_Parser.hpp
    ${FASTFHIR_INCLUDE_DIR}/FF_Builder.hpp
    ${FASTFHIR_INCLUDE_DIR}/FF_Primitives.hpp
    ${FASTFHIR_GENERATED_DIR}/FF_FieldKeys.hpp
    ${FASTFHIR_INCLUDE_DIR}/FF_Recovery.hpp
    ${FASTFHIR_GENERATED_DIR}/FF_ResourceTypes.hpp
    DESTINATION include
)
if(FASTFHIR_BUILD_INGESTOR)
    install(TARGETS fastfhir_ingestor ff_ingest
        EXPORT FastFHIRTargets
        LIBRARY DESTINATION lib ARCHIVE DESTINATION lib RUNTIME DESTINATION bin)
endif()
install(EXPORT FastFHIRTargets
    FILE FastFHIRTargets.cmake NAMESPACE FastFHIR::
    DESTINATION lib/cmake/FastFHIR)

# ── Convenience target ─────────────────────────────────────────
set(_BUILD_ALL fastfhir ff_export ff_compact)
if(FASTFHIR_BUILD_INGESTOR)
    list(APPEND _BUILD_ALL fastfhir_ingestor ff_ingest)
endif()
if(FASTFHIR_BUILD_TESTS)
    # Must match the ctest registration list at the `foreach(_standalone ...)` above:
    # a target registered as a test but absent here builds nothing and ctest reports
    # "Not Run". add_ff_cpp_test() does neither for you.
    list(APPEND _BUILD_ALL ff_test_readme ff_roundtrip
         ff_test_primitives ff_test_memory ff_test_simd
         ff_test_amend ff_test_cc ff_test_bundle)
endif()
if(FASTFHIR_BUILD_PYTHON_BINDINGS)
    list(APPEND _BUILD_ALL fastfhir_python)
endif()
add_custom_target(build_all DEPENDS ${_BUILD_ALL})

# ══════════════════════════════════════════════════════════════════
# IDE PROJECT LAYOUT (Xcode / Visual Studio; ignored by Ninja & Make)
# ══════════════════════════════════════════════════════════════════
# Everything below is presentation only — it must never change what is built.
# Kept in one block at the end so the build logic above stays readable.

# ── Files: mirror the on-disk directory tree ───────────────────────
# PREFIX "" puts src/, include/, dictionaries/ and generated_src/ at the top
# level of the navigator instead of burying them under "Source Files".
# source_group is a directory-scoped property, so this one call covers every
# target defined in this file.
# Tool, test and binding sources are added to their targets as literal relative
# paths, so they must be listed here too or they land in a default "Source Files"
# group per target — twelve of them, which is most of the mess this block exists
# to remove.
file(GLOB FASTFHIR_TOOL_SOURCES CONFIGURE_DEPENDS tools/*/*.cpp)
file(GLOB FASTFHIR_TEST_SOURCES CONFIGURE_DEPENDS tests/cpp/*.cpp tests/cpp/*.hpp)

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" PREFIX "" FILES
    ${FASTFHIR_CORE_SOURCES}
    ${FASTFHIR_CORE_HEADERS}
    ${FASTFHIR_DICT_SOURCES}
    ${FASTFHIR_GENERATED_SOURCES}
    ${FASTFHIR_GENERATED_HEADERS}
    ${FASTFHIR_TOOL_SOURCES}
    ${FASTFHIR_TEST_SOURCES}
    "${CMAKE_CURRENT_SOURCE_DIR}/src/FF_Ingestor.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/generated_src/FF_IngestMappings.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/python/FF_PythonBindings.cpp"
)

# The generated anchor TU lives in the build tree, not the source tree, so it
# cannot go through the TREE call above (source_group rejects files outside the
# root). Give it its own group rather than letting it land at the top level.
if(_FF_XCODE_ANCHOR)
    source_group("CMake" FILES "${_FF_XCODE_ANCHOR}")
endif()

# ── Targets: group by role ─────────────────────────────────────────
set_target_properties(fastfhir fastfhir_obj PROPERTIES FOLDER "Libraries")
if(CMAKE_GENERATOR STREQUAL "Xcode")
    set_target_properties(${_FF_XCODE_EXTRA_TARGET} PROPERTIES FOLDER "Libraries")
endif()
set_target_properties(ff_export ff_compact  PROPERTIES FOLDER "Tools")
set_target_properties(build_all             PROPERTIES FOLDER "CMake")
if(FASTFHIR_BUILD_INGESTOR)
    set_target_properties(fastfhir_ingestor fastfhir_ingest_obj
        PROPERTIES FOLDER "Libraries")
    set_target_properties(ff_ingest PROPERTIES FOLDER "Tools")
    if(TARGET simdjson)
        set_target_properties(simdjson PROPERTIES FOLDER "ThirdParty")
    endif()
endif()
if(FASTFHIR_BUILD_PYTHON_BINDINGS AND TARGET fastfhir_python)
    set_target_properties(fastfhir_python PROPERTIES FOLDER "Libraries")
endif()
if(FASTFHIR_BUILD_TESTS)
    foreach(_t ff_test_readme ff_roundtrip ff_test_primitives ff_test_memory
               ff_test_simd ff_test_amend ff_test_cc ff_test_bundle)
        if(TARGET ${_t})
            set_target_properties(${_t} PROPERTIES FOLDER "Tests")
        endif()
    endforeach()
endif()

# ── Non-build files: make the repo browsable from the IDE ──────────
# A utility target whose SOURCES are never compiled — it exists so the Python
# generator and the project docs appear in the navigator under their real paths
# and can be edited without leaving the IDE. Excluded from ALL_BUILD so it costs
# nothing at build time.
if(CMAKE_GENERATOR STREQUAL "Xcode" OR CMAKE_GENERATOR MATCHES "Visual Studio")
    file(GLOB_RECURSE FASTFHIR_GENERATOR_PY CONFIGURE_DEPENDS generator/*.py)
    file(GLOB FASTFHIR_TEST_PY CONFIGURE_DEPENDS tests/generator/*.py tests/python/*.py)
    set(FASTFHIR_PROJECT_DOCS
        "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt"
        "${CMAKE_CURRENT_SOURCE_DIR}/CMakePresets.json"
        "${CMAKE_CURRENT_SOURCE_DIR}/CLAUDE.md"
        "${CMAKE_CURRENT_SOURCE_DIR}/TASKS.md"
        "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
        "${CMAKE_CURRENT_SOURCE_DIR}/architecture.md"
    )
    add_custom_target(project_files SOURCES
        ${FASTFHIR_GENERATOR_PY} ${FASTFHIR_TEST_PY} ${FASTFHIR_PROJECT_DOCS})
    set_target_properties(project_files PROPERTIES
        FOLDER "CMake" EXCLUDE_FROM_ALL TRUE XCODE_GENERATE_SCHEME OFF)
    source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" PREFIX "" FILES
        ${FASTFHIR_GENERATOR_PY} ${FASTFHIR_TEST_PY} ${FASTFHIR_PROJECT_DOCS})
endif()

# ── Schemes: only for things worth opening directly in Xcode ───────────────────
# The preset turns scheme generation off globally; re-enable it per target so the
# scheme menu keeps the debuggable libraries, tools and tests while still hiding
# ZERO_CHECK, project_files, and the object-only implementation targets.
#
# RUN_TESTS and ALL_BUILD cannot be included here: the Xcode generator creates
# them at *generate* time, so `if(TARGET RUN_TESTS)` is false while this file is
# being configured and the property is silently dropped (verified). Run the suite
# with `ctest --preset xcode` instead, or flip CMAKE_XCODE_GENERATE_SCHEME back to
# ON in CMakePresets.json to get a scheme for every target.
foreach(_t build_all fastfhir fastfhir_ingestor ff_ingest ff_export ff_compact ff_roundtrip ff_test_readme
           ff_test_primitives ff_test_memory ff_test_simd ff_test_amend
           ff_test_cc ff_test_bundle)
    if(TARGET ${_t})
        set_target_properties(${_t} PROPERTIES XCODE_GENERATE_SCHEME ON)
    endif()
endforeach()
if(CMAKE_GENERATOR STREQUAL "Xcode" AND TARGET ${_FF_XCODE_EXTRA_TARGET})
    set_target_properties(${_FF_XCODE_EXTRA_TARGET} PROPERTIES XCODE_GENERATE_SCHEME ON)
endif()
