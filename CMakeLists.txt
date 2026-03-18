## CMakeLists.txt for FastFHIR C++ Library
## Copyright (c) 2026 Ryan Landvater. All rights reserved.
## This CMake configuration defines the build process for the FastFHIR C++ library, including:
## - Configurable option to run the Python code generator at build time.
## - Inclusion of generated source files and core primitives.
## - Setting up include directories and compiler options for C++20 standard.

cmake_minimum_required(VERSION 3.20)

# Define the project name, semantic version, and enabled language(s).
project(FastFHIR
    VERSION 0.1.0
    LANGUAGES CXX
)

# Toggle for running Python code generation from CMake.
# ON means CMake will invoke tools/generator/make_lib.py during configure and again via a build target.
option(FASTFHIR_RUN_GENERATOR "Run python code generation before building" ON)

# Build shared library by default; set OFF to build a static library.
option(FASTFHIR_BUILD_SHARED "Build FastFHIR as a shared library (dynamic linking)" ON)

# Canonical include directory for hand-written public headers.
set(FASTFHIR_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")

# Directory where Python generator emits C++ sources/headers.
set(FASTFHIR_GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated_src")

# Hand-written C++ source files that are always part of the library.
set(FASTFHIR_CORE_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/FF_Primitives.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/FF_Parser.cpp
)

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
)

# Require C++20 features for this target and downstream users.
target_compile_features(fastfhir PUBLIC cxx_std_20)

# Enable strict warnings for Clang/GCC while keeping compiler-specific behavior scoped.
target_compile_options(fastfhir PRIVATE
    $<$<CXX_COMPILER_ID:Clang,GNU>:-Wall -Wextra -Wpedantic>
)
