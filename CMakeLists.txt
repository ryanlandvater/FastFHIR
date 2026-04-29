## CMakeLists.txt for FastFHIR C++ Library
## Copyright (c) 2026 Ryan Landvater. All rights reserved.
## This CMake configuration defines the build process for the FastFHIR C++ library, including:
## - Configurable option to run the Python code generator at build time. 
## - Inclusion of generated source files and core primitives. 
## - Setting up include directories and compiler options for C++20 standard.
## - Optional building of a high-speed JSON Ingestor module that depends on simdjson.
cmake_minimum_required(VERSION 3.20) 

# =====================================================================
# Version Resolution
# =====================================================================
# CI sets FASTFHIR_VERSION_MAJOR / _MINOR / _BUILD from the GitHub release
# tag (e.g. v1.2.3 → 1, 2, 3) before invoking cmake.  Falls back to the
# version declared in project() above for local / offline builds.
if(DEFINED ENV{FASTFHIR_VERSION_MAJOR})
    set(FASTFHIR_VERSION_MAJOR "$ENV{FASTFHIR_VERSION_MAJOR}")
else()
    set(FASTFHIR_VERSION_MAJOR "2026")  # Default major version for local builds
endif()
if(DEFINED ENV{FASTFHIR_VERSION_MINOR})
    set(FASTFHIR_VERSION_MINOR "$ENV{FASTFHIR_VERSION_MINOR}")
else()
    set(FASTFHIR_VERSION_MINOR "0")  # Default minor version for local builds
endif()
if(DEFINED ENV{FASTFHIR_VERSION_BUILD})
    set(FASTFHIR_VERSION_BUILD "$ENV{FASTFHIR_VERSION_BUILD}")
else()
    set(FASTFHIR_VERSION_BUILD "0")  # Default build version for local builds
endif()
message(STATUS "FastFHIR version: ${FASTFHIR_VERSION_MAJOR}.${FASTFHIR_VERSION_MINOR}.${FASTFHIR_VERSION_BUILD}")


# Define the project name, semantic version, and enabled language(s). 
project(FastFHIR
    VERSION ${FASTFHIR_VERSION_MAJOR}.${FASTFHIR_VERSION_MINOR}.${FASTFHIR_VERSION_BUILD}
    LANGUAGES CXX
)

# Make the language mode explicit for build tools and editor integrations.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Toggle for running Python code generation from CMake. 
option(FASTFHIR_RUN_GENERATOR "Run python code generation before building" ON)

option(FASTFHIR_GENERATE_ON_BUILD "Regenerate generated sources on each build" OFF)


# Production profile passed into the Python generator.
# Supported values: us, uk, all
set(FASTFHIR_PRODUCTION_PROFILE "us" CACHE STRING "FastFHIR production profile for generator (us|uk)")
set_property(CACHE FASTFHIR_PRODUCTION_PROFILE PROPERTY STRINGS us uk all)
if(NOT FASTFHIR_PRODUCTION_PROFILE MATCHES "^(us|uk|all)$")
    message(FATAL_ERROR "Invalid FASTFHIR_PRODUCTION_PROFILE='${FASTFHIR_PRODUCTION_PROFILE}'. Use us, uk, or all.")
endif()
message(STATUS "FastFHIR production profile: ${FASTFHIR_PRODUCTION_PROFILE}")

# Build shared library by default; set OFF to build a static library.
option(FASTFHIR_BUILD_SHARED "Build FastFHIR as a shared library (dynamic linking)" ON)

# Toggle for building the high-speed JSON Ingestor module
option(FASTFHIR_BUILD_INGESTOR "Build the FastFHIR Ingestor (requires simdjson)" OFF)

# Toggle for building Python bindings using pybind11
option(FASTFHIR_BUILD_PYTHON_BINDINGS "Build Python bindings using pybind11" OFF)

# Toggle for WASM extension subsystem (requires WAMR)
option(FASTFHIR_ENABLE_EXTENSIONS "Enable WASM extension codec subsystem (requires WAMR)" OFF)

# Toggle for building the C++ test suite
option(FASTFHIR_BUILD_TESTS "Build the FastFHIR C++ test suite (requires FASTFHIR_BUILD_INGESTOR)" OFF)

# =====================================================================
# Dependency Enforcement
# =====================================================================
if(FASTFHIR_BUILD_PYTHON_BINDINGS AND NOT FASTFHIR_BUILD_INGESTOR)
    message(STATUS "FastFHIR: Python bindings require the Ingestor module. Automatically enabling FASTFHIR_BUILD_INGESTOR.")
    set(FASTFHIR_BUILD_INGESTOR ON CACHE BOOL "Build the FastFHIR Ingestor (requires simdjson)" FORCE)
endif()

if(FASTFHIR_BUILD_TESTS AND NOT FASTFHIR_BUILD_INGESTOR)
    message(STATUS "FastFHIR: Tests require the Ingestor module. Automatically enabling FASTFHIR_BUILD_INGESTOR.")
    set(FASTFHIR_BUILD_INGESTOR ON CACHE BOOL "Build the FastFHIR Ingestor (requires simdjson)" FORCE)
endif()

# Enable position-independent code globally for shared libs and ASLR-enabled EXEs.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Canonical include directory for hand-written public headers.
set(FASTFHIR_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")

# Directory where Python generator emits C++ sources/headers.
set(FASTFHIR_GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated_src")

# Hand-written C++ source files that are always part of the library.
file(GLOB FASTFHIR_CORE_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
)

# Hand-written public headers (added to targets for IDE/Xcode visibility).
file(GLOB FASTFHIR_CORE_HEADERS CONFIGURE_DEPENDS
    ${FASTFHIR_INCLUDE_DIR}/*.hpp
)

# Exclude the Ingestor from the pure serialization package
list(REMOVE_ITEM FASTFHIR_CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/FF_Ingestor.cpp")

# Conditionally include FF_Extensions.cpp (WAMR host) when extensions are enabled.
if(FASTFHIR_ENABLE_EXTENSIONS)
    message(STATUS "FastFHIR: Fetching WAMR via FetchContent...")
    include(FetchContent)
    FetchContent_Declare(wamr
        GIT_REPOSITORY https://github.com/bytecodealliance/wasm-micro-runtime.git
        GIT_TAG        WAMR-2.2.0
        GIT_SHALLOW    ON
    )
    set(WAMR_BUILD_INTERP    ON  CACHE BOOL "" FORCE)
    set(WAMR_BUILD_AOT       ON  CACHE BOOL "" FORCE)
    set(WAMR_BUILD_LIBC_BUILTIN ON CACHE BOOL "" FORCE)
    set(WAMR_BUILD_LIBC_WASI OFF CACHE BOOL "" FORCE)
    set(WAMR_BUILD_FAST_JIT  OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(wamr)
    list(APPEND FASTFHIR_CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/FF_Extensions.cpp")
endif()

# Optionally run generator logic when configuring the project.  
if(FASTFHIR_RUN_GENERATOR)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)  

    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env
                FASTFHIR_PRODUCTION_PROFILE=${FASTFHIR_PRODUCTION_PROFILE}
                ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/generator/make_lib.py
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        RESULT_VARIABLE FASTFHIR_GENERATE_RESULT
    )

    if(NOT FASTFHIR_GENERATE_RESULT EQUAL 0)
        message(FATAL_ERROR "FastFHIR generation failed during CMake configure")
    endif()

    add_custom_target(fastfhir_generate
        COMMAND ${CMAKE_COMMAND} -E env
                FASTFHIR_PRODUCTION_PROFILE=${FASTFHIR_PRODUCTION_PROFILE}
                ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/generator/make_lib.py
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Generating FastFHIR C++ sources (${FASTFHIR_PRODUCTION_PROFILE} profile)"
        VERBATIM
    )
endif()

# Collect all generated C++ implementation files.
file(GLOB FASTFHIR_GENERATED_SOURCES CONFIGURE_DEPENDS
    ${FASTFHIR_GENERATED_DIR}/*.cpp
)

# Generated headers (added to targets for IDE/Xcode visibility).
file(GLOB FASTFHIR_GENERATED_HEADERS CONFIGURE_DEPENDS
    ${FASTFHIR_GENERATED_DIR}/*.hpp
)

# Exclude the IngestMappings.cpp from the core library.
list(REMOVE_ITEM FASTFHIR_GENERATED_SOURCES "${FASTFHIR_GENERATED_DIR}/FF_IngestMappings.cpp")

if(NOT FASTFHIR_GENERATED_SOURCES)
    message(FATAL_ERROR "No generated .cpp files found. Run generator or enable FASTFHIR_RUN_GENERATOR.")
endif()

# ---------------------------------------------------------------------
# Intermediate Object Library (fastfhir_obj)
# ---------------------------------------------------------------------
add_library(fastfhir_obj 
    OBJECT
        ${FASTFHIR_CORE_SOURCES}
        ${FASTFHIR_CORE_HEADERS}       # Added for IDE visibility
        ${FASTFHIR_GENERATED_SOURCES}
        ${FASTFHIR_GENERATED_HEADERS}   # Added for IDE visibility
)

# Organize files in IDEs (especially Xcode) for easier navigation.
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" PREFIX "Sources/Core" FILES ${FASTFHIR_CORE_SOURCES})
source_group(TREE "${FASTFHIR_GENERATED_DIR}" PREFIX "Sources/Generated" FILES ${FASTFHIR_GENERATED_SOURCES})
source_group(TREE "${FASTFHIR_INCLUDE_DIR}" PREFIX "Headers/Public" FILES ${FASTFHIR_CORE_HEADERS})
source_group(TREE "${FASTFHIR_GENERATED_DIR}" PREFIX "Headers/Generated" FILES ${FASTFHIR_GENERATED_HEADERS})

target_include_directories(fastfhir_obj 
    PUBLIC 
        $<BUILD_INTERFACE:${FASTFHIR_INCLUDE_DIR}>
        $<BUILD_INTERFACE:${FASTFHIR_GENERATED_DIR}>
)

target_compile_features(fastfhir_obj PUBLIC cxx_std_20)

# Propagate version to every target that links fastfhir_obj (PUBLIC propagates
# through target_link_libraries to ff_export, ff_test_readme, fastfhir, etc.).
target_compile_definitions(fastfhir_obj
    PUBLIC
        FASTFHIR_VERSION_MAJOR=${FASTFHIR_VERSION_MAJOR}
        FASTFHIR_VERSION_MINOR=${FASTFHIR_VERSION_MINOR}
        FASTFHIR_VERSION_BUILD=${FASTFHIR_VERSION_BUILD}
)

if(FASTFHIR_ENABLE_EXTENSIONS)
    target_compile_definitions(fastfhir_obj PUBLIC FASTFHIR_ENABLE_EXTENSIONS)
    target_include_directories(fastfhir_obj PRIVATE ${wamr_SOURCE_DIR}/core/iwasm/include)
    target_link_libraries(fastfhir_obj PUBLIC vmlib)
endif()

target_precompile_headers(fastfhir_obj 
    PRIVATE
        <vector> 
        <string> 
        <string_view> 
        <cstdint> 
        <memory>
        ${FASTFHIR_INCLUDE_DIR}/FF_Primitives.hpp
)

# Optional: rerun generator before each build (off by default to avoid
# invalidating the precompiled header when generated headers change).
if(FASTFHIR_RUN_GENERATOR AND FASTFHIR_GENERATE_ON_BUILD)
    add_dependencies(fastfhir_obj fastfhir_generate)
endif()

# ---------------------------------------------------------------------
# Core Library (fastfhir)
# ---------------------------------------------------------------------
if(FASTFHIR_BUILD_SHARED)
    set(FASTFHIR_LIBRARY_TYPE SHARED)
else()
    set(FASTFHIR_LIBRARY_TYPE STATIC)
endif()

add_library(fastfhir ${FASTFHIR_LIBRARY_TYPE} $<TARGET_OBJECTS:fastfhir_obj>)
add_library(FastFHIR::fastfhir ALIAS fastfhir)

if(FASTFHIR_RUN_GENERATOR AND FASTFHIR_GENERATE_ON_BUILD)
    add_dependencies(fastfhir fastfhir_generate)
endif()

target_include_directories(fastfhir 
    PUBLIC
        $<BUILD_INTERFACE:${FASTFHIR_INCLUDE_DIR}>
        $<BUILD_INTERFACE:${FASTFHIR_GENERATED_DIR}>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(fastfhir PUBLIC cxx_std_20)

# ---------------------------------------------------------------------
# Ingestor Module (fastfhir_ingestor)
# ---------------------------------------------------------------------
if(FASTFHIR_BUILD_INGESTOR)
    # Always fetch simdjson via FetchContent to avoid picking up bundled copies
    # from Anaconda or other Python environments, which ship an incompatible
    # simdjson.h that breaks int8_t resolution on Apple Silicon (arm_neon.h
    # gets included before <cstdint> defines the fixed-width integer types).
    message(STATUS "FastFHIR: Fetching simdjson via FetchContent...")
    include(FetchContent)
    set(SIMDJSON_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(SIMDJSON_JUST_LIBRARY ON CACHE BOOL "" FORCE)
    FetchContent_Declare(simdjson
        GIT_REPOSITORY https://github.com/simdjson/simdjson.git
        GIT_TAG v3.9.5
    )
    FetchContent_MakeAvailable(simdjson)

    # Create an object library for the ingestor-specific logic
    add_library(fastfhir_ingest_obj 
        OBJECT
            ${CMAKE_CURRENT_SOURCE_DIR}/src/FF_Ingestor.cpp
            ${FASTFHIR_GENERATED_DIR}/FF_IngestMappings.cpp
    )
    
    # Propagate headers to standalone EXEs
    target_include_directories(fastfhir_ingest_obj 
        PUBLIC
            ${FASTFHIR_INCLUDE_DIR}
            ${FASTFHIR_GENERATED_DIR}
    )

    target_link_libraries(fastfhir_ingest_obj PRIVATE fastfhir_obj simdjson::simdjson)

    if(FASTFHIR_RUN_GENERATOR AND FASTFHIR_GENERATE_ON_BUILD)
        add_dependencies(fastfhir_ingest_obj fastfhir_generate)
    endif()

    # Ingestor Library Target
    add_library(fastfhir_ingestor 
        ${FASTFHIR_LIBRARY_TYPE} 
            $<TARGET_OBJECTS:fastfhir_ingest_obj>
    )
    target_link_libraries(fastfhir_ingestor 
        PUBLIC 
            fastfhir 
            simdjson::simdjson
    )
    add_library(FastFHIR::ingestor ALIAS fastfhir_ingestor)

    # ==========================================
    # CLI Ingestor Executable (Standalone)
    # ==========================================
    find_package(OpenSSL QUIET)

    if(NOT OpenSSL_FOUND)
        message(STATUS "FastFHIR: Fetching OpenSSL via ExternalProject...")
        include(ExternalProject)

        set(OPENSSL_INSTALL_DIR ${CMAKE_CURRENT_BINARY_DIR}/openssl_install)
        set(OPENSSL_CRYPTO_LIB  ${OPENSSL_INSTALL_DIR}/lib/libcrypto.a)

        ExternalProject_Add(openssl_external
            GIT_REPOSITORY https://github.com/openssl/openssl.git
            GIT_TAG        openssl-3.2.1 
            PREFIX         ${CMAKE_CURRENT_BINARY_DIR}/openssl
            CONFIGURE_COMMAND <SOURCE_DIR>/config --prefix=${OPENSSL_INSTALL_DIR} --libdir=lib no-shared no-tests
            BUILD_COMMAND     make
            INSTALL_COMMAND   make install_sw
            BUILD_BYPRODUCTS  ${OPENSSL_CRYPTO_LIB}
        )

        add_library(OpenSSL::Crypto STATIC IMPORTED GLOBAL)
        set_target_properties(OpenSSL::Crypto 
            PROPERTIES
                IMPORTED_LOCATION ${OPENSSL_CRYPTO_LIB}
                INTERFACE_INCLUDE_DIRECTORIES ${OPENSSL_INSTALL_DIR}/include
        )
        add_dependencies(OpenSSL::Crypto openssl_external)
    endif()

        add_executable(ff_ingest tools/ingestor/ff_ingest.cpp)
        target_include_directories(ff_ingest 
            PRIVATE
                ${FASTFHIR_INCLUDE_DIR}
                ${FASTFHIR_GENERATED_DIR}
        )
        target_link_libraries(ff_ingest 
            PRIVATE 
                $<TARGET_OBJECTS:fastfhir_obj>
                $<TARGET_OBJECTS:fastfhir_ingest_obj>
                simdjson::simdjson
                OpenSSL::Crypto
        )

endif()

# ---------------------------------------------------------------------
# Test Suite
# ---------------------------------------------------------------------
if(FASTFHIR_BUILD_TESTS)
    include(CTest)
    enable_testing()

    # Resolve standalone Asio for cross-platform socket testing in test_readme.cpp.
    find_path(ASIO_INCLUDE_DIR asio.hpp)
    if(NOT ASIO_INCLUDE_DIR)
        message(STATUS "FastFHIR: Local standalone Asio not found. Fetching via FetchContent...")
        include(FetchContent)
        FetchContent_Declare(asio
            GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
            GIT_TAG asio-1-30-2
        )
        FetchContent_MakeAvailable(asio)
        set(ASIO_INCLUDE_DIR "${asio_SOURCE_DIR}/asio/include")
    endif()

    add_executable(ff_test_readme tests/cpp/test_readme.cpp)
    target_include_directories(ff_test_readme
        PRIVATE
            ${FASTFHIR_INCLUDE_DIR}
            ${FASTFHIR_GENERATED_DIR}
            ${ASIO_INCLUDE_DIR}
    )
    target_compile_definitions(ff_test_readme
        PRIVATE
            ASIO_STANDALONE
            FASTFHIR_TEST_ARTIFACT_DIR="${CMAKE_BINARY_DIR}/tests/cpp"
    )

    if(WIN32)
        target_compile_definitions(ff_test_readme PRIVATE _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX)
    endif()

    target_link_libraries(ff_test_readme
        PRIVATE
            $<TARGET_OBJECTS:fastfhir_obj>
            $<TARGET_OBJECTS:fastfhir_ingest_obj>
            simdjson::simdjson
            OpenSSL::Crypto
    )

    if(WIN32)
        target_link_libraries(ff_test_readme PRIVATE ws2_32)
    endif()

    add_test(NAME readme_cpp COMMAND ff_test_readme)
endif()

# ---------------------------------------------------------------------
# CLI Exporter Executable (Standalone)
# ---------------------------------------------------------------------
add_executable(ff_export tools/exporter/FF_Export.cpp)

target_include_directories(ff_export 
    PRIVATE
        ${FASTFHIR_INCLUDE_DIR}
        ${FASTFHIR_GENERATED_DIR}
)

# Link directly to the intermediate object library for a standalone binary
target_link_libraries(ff_export 
    PRIVATE 
        $<TARGET_OBJECTS:fastfhir_obj>
)

# ---------------------------------------------------------------------
# Python Bindings (fastfhir)
# ---------------------------------------------------------------------
if (FASTFHIR_BUILD_PYTHON_BINDINGS)
    find_package(Python3 REQUIRED COMPONENTS Interpreter Development)

    set(PYBIND11_FINDPYTHON ON)
    include(FetchContent)
    FetchContent_Declare(
        pybind11
        GIT_REPOSITORY https://github.com/pybind/pybind11.git
        GIT_TAG        v2.11.1
        GIT_SHALLOW    ON
        FETCHCONTENT_QUIET ON
    )
    FetchContent_MakeAvailable(pybind11)

    if (NOT CMAKE_INSTALL_PYTHON_LIBDIR)
        set(CMAKE_INSTALL_PYTHON_LIBDIR "lib/python${Python3_VERSION_MAJOR}.${Python3_VERSION_MINOR}/site-packages")
    endif ()
    string(REPLACE "\\" "/" FASTFHIR_INSTALL_PYTHON_LIBDIR ${CMAKE_INSTALL_PYTHON_LIBDIR})

    # Compile the monolithic Python module
    pybind11_add_module(
        fastfhir_python
        $<TARGET_OBJECTS:fastfhir_obj>
        $<TARGET_OBJECTS:fastfhir_ingest_obj>
        ${CMAKE_CURRENT_SOURCE_DIR}/python/FF_PythonBindings.cpp
    )
    # Link Time Optimization (LTO) - Strips unused template instantiations
    include(CheckIPOSupported)
    check_ipo_supported(RESULT lto_supported)
    if(lto_supported)
        set_property(TARGET fastfhir_python PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
    # Strip Symbols - Removes heavy debugging metadata
    if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        if(APPLE)
            target_link_options(fastfhir_python PRIVATE "-Wl,-S") # macOS
        else()
            target_link_options(fastfhir_python PRIVATE "-Wl,-s") # Linux
        endif()
    endif()

    target_include_directories(fastfhir_python PRIVATE 
        ${FASTFHIR_INCLUDE_DIR}
        ${FASTFHIR_GENERATED_DIR}
    )

    # pybind11_add_module takes $<TARGET_OBJECTS:...> as source files, not as
    # linked targets, so PUBLIC compile definitions from fastfhir_obj do not
    # propagate automatically.  Inject the version definitions explicitly.
    target_compile_definitions(fastfhir_python PRIVATE
        FASTFHIR_VERSION_MAJOR=${FASTFHIR_VERSION_MAJOR}
        FASTFHIR_VERSION_MINOR=${FASTFHIR_VERSION_MINOR}
        FASTFHIR_VERSION_BUILD=${FASTFHIR_VERSION_BUILD}
    )

    # Unconditionally link the ingestor dependencies
    target_link_libraries(fastfhir_python PRIVATE 
        simdjson::simdjson
        OpenSSL::Crypto
    )

endif(FASTFHIR_BUILD_PYTHON_BINDINGS)

# ---------------------------------------------------------------------
# Install / Export Targets
# ---------------------------------------------------------------------
install(TARGETS fastfhir ff_export EXPORT FastFHIRTargets
    LIBRARY DESTINATION lib 
    ARCHIVE DESTINATION lib 
    RUNTIME DESTINATION bin 
    INCLUDES DESTINATION include
)

install(FILES
    ${FASTFHIR_INCLUDE_DIR}/FastFHIR.hpp
    ${FASTFHIR_INCLUDE_DIR}/FF_Parser.hpp
    ${FASTFHIR_INCLUDE_DIR}/FF_Builder.hpp
    ${FASTFHIR_INCLUDE_DIR}/FF_Primitives.hpp
    DESTINATION include
)

install(FILES
    ${FASTFHIR_GENERATED_DIR}/FF_FieldKeys.hpp
    ${FASTFHIR_GENERATED_DIR}/FF_Recovery.hpp
    DESTINATION include
)

if(FASTFHIR_BUILD_INGESTOR)
    install(TARGETS fastfhir_ingestor ff_ingest
        EXPORT FastFHIRTargets
        LIBRARY DESTINATION lib ARCHIVE DESTINATION lib RUNTIME DESTINATION bin
    )
endif()

install(EXPORT FastFHIRTargets 
    FILE FastFHIRTargets.cmake 
    NAMESPACE FastFHIR:: 
    DESTINATION lib/cmake/FastFHIR
) 

# ---------------------------------------------------------------------
# Python Package Installation
# ---------------------------------------------------------------------
if (FASTFHIR_BUILD_PYTHON_BINDINGS)

    # 1. Install the core __init__.py from the new python directory
    install(
        FILES ${CMAKE_CURRENT_SOURCE_DIR}/python/fastfhir/__init__.py
        DESTINATION ${FASTFHIR_INSTALL_PYTHON_LIBDIR}/fastfhir
    )

    # 2. Install the generated AST directory (fields/) into the package folder
    install(
        DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/generated_src/python/fields
        DESTINATION ${FASTFHIR_INSTALL_PYTHON_LIBDIR}/fastfhir
    )

    # 3. Install py.typed marker for PEP 561 type checking support
    install(
        FILES ${CMAKE_CURRENT_SOURCE_DIR}/generated_src/python/py.typed
        DESTINATION ${FASTFHIR_INSTALL_PYTHON_LIBDIR}/fastfhir
    )

    # 4. Install the compiled C++ extension (_core.so) into the package folder
    # Note: Ensure OUTPUT_NAME is set to _core so it matches 'from . import _core'
    set_property(TARGET fastfhir_python PROPERTY OUTPUT_NAME _core)
    install(
        TARGETS fastfhir_python 
        DESTINATION ${FASTFHIR_INSTALL_PYTHON_LIBDIR}/fastfhir
    )

endif()

# ---------------------------------------------------------------------
# Convenience Target — build_all
# Builds every enabled target in one shot: core library, ingestor,
# exporter CLI, tests, and (if enabled) Python bindings.
# ---------------------------------------------------------------------
set(_BUILD_ALL_TARGETS fastfhir ff_export)
if(FASTFHIR_BUILD_INGESTOR)
    list(APPEND _BUILD_ALL_TARGETS fastfhir_ingestor ff_ingest)
endif()
if(FASTFHIR_BUILD_TESTS)
    list(APPEND _BUILD_ALL_TARGETS ff_test_readme)
endif()
if(FASTFHIR_BUILD_PYTHON_BINDINGS)
    list(APPEND _BUILD_ALL_TARGETS fastfhir_python)
endif()
add_custom_target(build_all DEPENDS ${_BUILD_ALL_TARGETS})

# ---------------------------------------------------------------------
# Xcode Project Navigator Organization
# ---------------------------------------------------------------------
if(APPLE)
    # Automatically creates groups in Xcode matching the folder structure
    source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" PREFIX "Source Files" FILES ${FASTFHIR_CORE_SOURCES})
    # source_group(TREE "${FASTFHIR_INCLUDE_DIR}" PREFIX "Header Files" FILES 
    #     ${FASTFHIR_INCLUDE_DIR}/FastFHIR.hpp 
    #     ${FASTFHIR_INCLUDE_DIR}/FF_Parser.hpp 
    #     ${FASTFHIR_INCLUDE_DIR}/FF_Builder.hpp 
    #     ${FASTFHIR_INCLUDE_DIR}/FF_Primitives.hpp
    # )

    # Mirror the physical 'include' folder in Xcode
    source_group(TREE "${FASTFHIR_INCLUDE_DIR}" PREFIX "Header Files" FILES ${FASTFHIR_CORE_HEADERS})

    # Group the generated files separately so they are easy to find
    source_group(TREE "${FASTFHIR_GENERATED_DIR}" PREFIX "Generated" FILES ${FASTFHIR_GENERATED_SOURCES})
    
    if(FASTFHIR_RUN_GENERATOR)
        # Ensure the generated files are marked as 'GENERATED' so Xcode doesn't 
        # complain if they aren't on disk the first time you open the project.
        set_source_files_properties(${FASTFHIR_GENERATED_SOURCES} PROPERTIES GENERATED TRUE)
    endif()
endif()