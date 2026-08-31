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

# Debug-only diagnostics: FF_Queue.hpp's `#if FASTFHIR_DEBUG` blocks (the Node
# destruction canary and the double-free guard) must actually compile in debug
# builds, or the guards are dead code. The macro is prefixed, not bare DEBUG:
# the generated FF_Codes.hpp contains a code value literally named DEBUG
# (FF_CODE_DEF DEBUG = 1813), so a global DEBUG macro breaks the build. The
# generator expression covers both single-config (ninja: CMAKE_BUILD_TYPE=Debug)
# and multi-config (Xcode) generators.
add_compile_definitions($<$<CONFIG:Debug>:FASTFHIR_DEBUG>)

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

# Comma-separated LIST of resource groupings; the generator compiles their union.
# Real deployments compose -- a payer needs US Core *and* claims. Accepted names
# live in RESOURCE_GROUPINGS (generator/model/type_map.py); "us"/"uk" are legacy
# aliases. NOTE: commas, not semicolons -- CMake treats ';' as its list separator.
# Adding a grouping requires its RECOVERY_TAGs in generated_src/FF_RecoveryTags.hpp first
# (permanent wire constants); the generator fails loudly and names the missing
# ones. See TASKS.md A27.
set(FASTFHIR_PRODUCTION_PROFILE "us-core" CACHE STRING
    "Generator resource groupings, comma-separated: us-core,uk-core,billing,medication-admin,supply,imaging (or 'all')")
set_property(CACHE FASTFHIR_PRODUCTION_PROFILE PROPERTY STRINGS
    us-core uk-core billing medication-admin supply imaging
    us-core,billing us-core,uk-core us-core,billing,medication-admin,supply all)

# ── OpenSSL (system or ExternalProject) ──────────────────────
include(cmake/openssl.cmake)

# ── Paths ──────────────────────────────────────────────────────
set(FASTFHIR_INCLUDE_DIR   "${CMAKE_CURRENT_SOURCE_DIR}/include")
# dictionaries/ now holds ONLY the JSON ledgers + README. The C++ they project
# into is generator output and lives in generated_src/, already on every include
# path, so no separate dictionary include dir is needed.
set(FASTFHIR_GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated_src")

# ── Source files ────────────────────────────────────────────────
file(GLOB FASTFHIR_CORE_SOURCES CONFIGURE_DEPENDS src/*.cpp)
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
    # `X | None` annotations in emit/codesystems.py, emit/code_ids.py and
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
    ${FASTFHIR_GENERATED_SOURCES}
    ${FASTFHIR_CORE_HEADERS}
    ${FASTFHIR_GENERATED_HEADERS}
)
target_include_directories(fastfhir_obj PUBLIC
    $<BUILD_INTERFACE:${FASTFHIR_INCLUDE_DIR}>
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
        ${FASTFHIR_INCLUDE_DIR} ${FASTFHIR_GENERATED_DIR}
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
    target_include_directories(ff_ingest PRIVATE ${FASTFHIR_INCLUDE_DIR} ${FASTFHIR_GENERATED_DIR})
    target_link_libraries(ff_ingest PRIVATE fastfhir_ingestor OpenSSL::Crypto)
endif()

# ── CLI tools ───────────────────────────────────────────────────
add_executable(ff_export tools/exporter/FF_Export.cpp)
target_include_directories(ff_export PRIVATE ${FASTFHIR_INCLUDE_DIR} ${FASTFHIR_GENERATED_DIR})
target_link_libraries(ff_export PRIVATE $<TARGET_OBJECTS:fastfhir_obj>)

add_executable(ff_compact tools/compactor/FF_Compact.cpp)
target_include_directories(ff_compact PRIVATE ${FASTFHIR_INCLUDE_DIR} ${FASTFHIR_GENERATED_DIR})
target_link_libraries(ff_compact PRIVATE $<TARGET_OBJECTS:fastfhir_obj> OpenSSL::Crypto)
target_compile_definitions(ff_compact PRIVATE FASTFHIR_HAS_OPENSSL)

# ── C++ & Python tests ───────────────────────────────────────
# All test registration — executables, CTest entries, dependency chains,
# resource locks, and the Python suite — lives in tests/tests.cmake so this
# file stays a readable build definition. Self-gating on FASTFHIR_BUILD_TESTS.
include(tests/tests.cmake)
# ── Python bindings ────────────────────────────────────────────
if(FASTFHIR_BUILD_PYTHON_BINDINGS)
    find_package(Python3 REQUIRED COMPONENTS Interpreter Development)
    # Default the Python package install dir to the interpreter's own
    # site-packages layout under the install prefix. Previously undefined, so
    # `install` collapsed DESTINATION to an absolute "/fastfhir".
    if(NOT CMAKE_INSTALL_PYTHON_LIBDIR)
        set(CMAKE_INSTALL_PYTHON_LIBDIR
            "lib/python${Python3_VERSION_MAJOR}.${Python3_VERSION_MINOR}/site-packages")
    endif()
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
        ${FASTFHIR_INCLUDE_DIR} ${FASTFHIR_GENERATED_DIR}
    )
    target_compile_definitions(fastfhir_python PRIVATE
        FASTFHIR_VERSION_MAJOR=${FASTFHIR_VERSION_MAJOR}
        FASTFHIR_VERSION_MINOR=${FASTFHIR_VERSION_MINOR}
        FASTFHIR_VERSION_BUILD=${FASTFHIR_VERSION_BUILD}
    )
    target_link_libraries(fastfhir_python PRIVATE simdjson::simdjson OpenSSL::Crypto)

    # ── Assemble an importable package in the build tree (A21) ───────────────
    # fastfhir/__init__.py imports relatively (from . import _core;
    # from .fields import *), so the extension and the generated field modules
    # must sit inside the package directory. Mirror the install layout into
    # build/python/fastfhir so ctest py_* tests — and users who point
    # PYTHONPATH at it — import the same shape that gets installed.
    set(_PY_STAGE "${CMAKE_CURRENT_BINARY_DIR}/python/fastfhir")
    add_custom_command(TARGET fastfhir_python POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_PY_STAGE}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/python/fastfhir/__init__.py" "${_PY_STAGE}/__init__.py"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${FASTFHIR_GENERATED_DIR}/python/fields" "${_PY_STAGE}/fields"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:fastfhir_python>" "${_PY_STAGE}/$<TARGET_FILE_NAME:fastfhir_python>"
        VERBATIM)

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
    ${FASTFHIR_INCLUDE_DIR}/FF_Utilities.hpp
    ${FASTFHIR_INCLUDE_DIR}/FF_Dictionary.hpp
    ${FASTFHIR_INCLUDE_DIR}/FF_Memory.hpp
    ${FASTFHIR_INCLUDE_DIR}/FF_Recovery.hpp
    ${FASTFHIR_INCLUDE_DIR}/FF_Version.hpp
    ${FASTFHIR_GENERATED_DIR}/FF_FieldKeys.hpp
    ${FASTFHIR_GENERATED_DIR}/FF_RecoveryTags.hpp
    ${FASTFHIR_GENERATED_DIR}/FF_ResourceTypes.hpp
    ${FASTFHIR_GENERATED_DIR}/FF_Reflection.hpp
    ${FASTFHIR_GENERATED_DIR}/FF_Codes.hpp
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
         ff_test_amend ff_test_cc ff_test_bundle ff_test_compactor
         ff_test_graph_bounds ff_test_datetime ff_test_api ff_test_dictionary
         ff_test_roundtrip_validate ff_test_compact_roundtrip ff_test_queue
         ff_test_abstraction_parity ff_test_recovery)
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
               ff_test_simd ff_test_amend ff_test_cc ff_test_bundle
               ff_test_compactor ff_test_graph_bounds ff_test_datetime
               ff_test_roundtrip_validate ff_test_compact_roundtrip ff_test_queue
               ff_test_api ff_test_dictionary ff_test_abstraction_parity
               ff_test_recovery)
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
           ff_test_cc ff_test_bundle ff_test_compactor ff_test_graph_bounds
           ff_test_roundtrip_validate ff_test_compact_roundtrip ff_test_queue
           ff_test_datetime ff_test_api ff_test_dictionary ff_test_abstraction_parity
           ff_test_recovery)
    if(TARGET ${_t})
        set_target_properties(${_t} PROPERTIES XCODE_GENERATE_SCHEME ON)
    endif()
endforeach()
if(CMAKE_GENERATOR STREQUAL "Xcode" AND TARGET ${_FF_XCODE_EXTRA_TARGET})
    set_target_properties(${_FF_XCODE_EXTRA_TARGET} PROPERTIES XCODE_GENERATE_SCHEME ON)
endif()
