## CMakeLists.txt for FastFHIR C++ Library
## Copyright (c) 2026 Ryan Landvater. All rights reserved.

## This CMake configuration defines the build process for the FastFHIR C++ library, including:
## - Configurable option to run the Python code generator at build time.
## - Inclusion of generated source files and core primitives.
## - Setting up include directories and compiler options for C++20 standard.
## - Optional building of a high-speed JSON Ingestor module that depends on simdjson, with automatic fetching if not found.

cmake_minimum_required(VERSION 3.20)

# Define the project name, semantic version, and enabled language(s).
project(FastFHIR
    VERSION 0.1.0
    LANGUAGES CXX
)

# Make the language mode explicit for build tools and editor integrations.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Toggle for running Python code generation from CMake.
# ON means CMake will invoke tools/generator/make_lib.py during configure and again via a build target.
option(FASTFHIR_RUN_GENERATOR "Run python code generation before building" ON)

# Build shared library by default; set OFF to build a static library.
option(FASTFHIR_BUILD_SHARED "Build FastFHIR as a shared library (dynamic linking)" ON)

# Toggle for building the high-speed JSON Ingestor module
option(FASTFHIR_BUILD_INGESTOR "Build the FastFHIR Ingestor (requires simdjson)" OFF)

# Canonical include directory for hand-written public headers.
set(FASTFHIR_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")

# Directory where Python generator emits C++ sources/headers.
set(FASTFHIR_GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated_src")

# Hand-written C++ source files that are always part of the library.
file(GLOB FASTFHIR_CORE_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
)

# Exclude the Ingestor from the pure serialization package
list(REMOVE_ITEM FASTFHIR_CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/FF_Ingestor.cpp")

# Optionally run generator logic when configuring the project.
if(FASTFHIR_RUN_GENERATOR)
    # Locate a Python interpreter for running generation scripts.
    find_package(Python3 REQUIRED COMPONENTS Interpreter)

    # Run generation during configure so generated .cpp files exist before globbing source list.
    execute_process(
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/generator/make_lib.py
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        RESULT_VARIABLE FASTFHIR_GENERATE_RESULT
    )

    # Fail configure early if generation did not succeed.
    if(NOT FASTFHIR_GENERATE_RESULT EQUAL 0)
        message(FATAL_ERROR "FastFHIR generation failed during CMake configure")
    endif()

    # Create a build target so generation can also be run as part of normal build dependency graph.
    add_custom_target(fastfhir_generate
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/generator/make_lib.py
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Generating FastFHIR C++ sources"
        VERBATIM
    )
endif()

# Collect all generated C++ implementation files.
# CONFIGURE_DEPENDS tells CMake to re-run configure if the glob result changes.
file(GLOB FASTFHIR_GENERATED_SOURCES CONFIGURE_DEPENDS
    ${FASTFHIR_GENERATED_DIR}/*.cpp
)

# Exclude the IngestMappings.cpp from the core library; it will be compiled separately in the ingestor module.
list(REMOVE_ITEM FASTFHIR_GENERATED_SOURCES "${FASTFHIR_GENERATED_DIR}/FF_IngestMappings.cpp")

# Guardrail: stop immediately if generation produced no C++ files.
if(NOT FASTFHIR_GENERATED_SOURCES)
    message(FATAL_ERROR "No generated .cpp files found in ${FASTFHIR_GENERATED_DIR}. Run tools/generator/make_lib.py first or enable FASTFHIR_RUN_GENERATOR.")
endif()

# Select library type based on build option.
if(FASTFHIR_BUILD_SHARED)
    set(FASTFHIR_LIBRARY_TYPE SHARED)
else()
    set(FASTFHIR_LIBRARY_TYPE STATIC)
endif()

# ---------------------------------------------------------------------
# Core Library (fastfhir)
# ---------------------------------------------------------------------
# Build FastFHIR from core + generated sources.
add_library(fastfhir ${FASTFHIR_LIBRARY_TYPE}
    ${FASTFHIR_CORE_SOURCES}
    ${FASTFHIR_GENERATED_SOURCES}
)

# Provide a namespaced alias for consumers using modern CMake target style.
add_library(FastFHIR::fastfhir ALIAS fastfhir)

# Ensure generation target runs before library compilation when generation is enabled.
if(FASTFHIR_RUN_GENERATOR)
    add_dependencies(fastfhir fastfhir_generate)
endif()

# Expose include paths to users of this target during build tree usage.
target_include_directories(fastfhir
    PUBLIC
        $<BUILD_INTERFACE:${FASTFHIR_INCLUDE_DIR}>
        $<BUILD_INTERFACE:${FASTFHIR_GENERATED_DIR}>
    $<INSTALL_INTERFACE:include>
    $<INSTALL_INTERFACE:generated_src>
)

# Require C++20 features for this target and downstream users.
target_compile_features(fastfhir PUBLIC cxx_std_20)

# Compiles these heavy headers once and injects the binary AST into all TUs
target_precompile_headers(fastfhir PRIVATE
    <vector>
    <string_view>
    <string>
    <cstdint>
    "${FASTFHIR_INCLUDE_DIR}/FF_Primitives.hpp"
)

# Enable strict warnings for Clang/GCC while keeping compiler-specific behavior scoped.
target_compile_options(fastfhir PRIVATE
    $<$<CXX_COMPILER_ID:Clang,GNU>:-Wall -Wextra -Wpedantic>
)

# ---------------------------------------------------------------------
# Ingestor Module (fastfhir_ingestor)
# ---------------------------------------------------------------------
if(FASTFHIR_BUILD_INGESTOR)
    # 1. Attempt to find a system-installed version of simdjson quietly
    find_package(simdjson)

    # 2. If not found, download and build it from source automatically
    if(NOT simdjson_FOUND)
        message(STATUS "FastFHIR: Local simdjson not found. Fetching via FetchContent...")
        include(FetchContent)
        
        # Disable simdjson's internal tests and tools to speed up our build
        set(SIMDJSON_BUILD_TESTING OFF CACHE BOOL "Disable simdjson tests" FORCE)
        set(SIMDJSON_JUST_LIBRARY ON CACHE BOOL "Only build the simdjson library" FORCE)

        FetchContent_Declare(
            simdjson
            GIT_REPOSITORY https://github.com/simdjson/simdjson.git
            GIT_TAG v3.9.5 # Stable v3 release
        )
        FetchContent_MakeAvailable(simdjson)
    else()
        message(STATUS "FastFHIR: Found local simdjson version ${simdjson_VERSION}")
    endif()

    # Build the distinct ingestor library
    add_library(fastfhir_ingestor ${FASTFHIR_LIBRARY_TYPE}
        ${CMAKE_CURRENT_SOURCE_DIR}/src/FF_Ingestor.cpp
        ${FASTFHIR_GENERATED_DIR}/FF_IngestMappings.cpp
    )

    # Require C++20 features
    target_compile_features(fastfhir_ingestor PUBLIC cxx_std_20)

    # Link against the pure FastFHIR core and simdjson
    # Note: simdjson::simdjson works whether it came from find_package or FetchContent
    target_link_libraries(fastfhir_ingestor PUBLIC
        fastfhir
        simdjson::simdjson
    )

    # Provide a modern namespaced alias
    add_library(FastFHIR::ingestor ALIAS fastfhir_ingestor)
endif()

# ---------------------------------------------------------------------
# Install / Export Targets
# ---------------------------------------------------------------------
install(TARGETS fastfhir
    EXPORT FastFHIRTargets
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
    INCLUDES DESTINATION include
)

# Install only the minimal hand-written public headers for the core library.
install(FILES
    ${FASTFHIR_INCLUDE_DIR}/FastFHIR.hpp
    ${FASTFHIR_INCLUDE_DIR}/FF_Parser.hpp
    ${FASTFHIR_INCLUDE_DIR}/FF_Builder.hpp
    ${FASTFHIR_INCLUDE_DIR}/FF_Primitives.hpp
    DESTINATION include
)

# Install only generated headers required by the public API.
install(FILES
    ${FASTFHIR_GENERATED_DIR}/FF_FieldKeys.hpp
    ${FASTFHIR_GENERATED_DIR}/FF_Recovery.hpp
    DESTINATION include
)

# Install Ingestor targets and headers if enabled
if(FASTFHIR_BUILD_INGESTOR)
    install(TARGETS fastfhir_ingestor
        EXPORT FastFHIRTargets
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib
        RUNTIME DESTINATION bin
    )
    
    install(FILES
        ${FASTFHIR_INCLUDE_DIR}/FF_Ingestor.hpp
        DESTINATION include
    )
endif()

install(EXPORT FastFHIRTargets
    FILE FastFHIRTargets.cmake
    NAMESPACE FastFHIR::
    DESTINATION lib/cmake/FastFHIR
)