## CMakeLists.txt for FastFHIR C++ Library
## Copyright (c) 2026 Ryan Landvater. All rights reserved.
## This CMake configuration defines the build process for the FastFHIR C++ library, including:
## - Configurable option to run the Python code generator at build time. 
## - Inclusion of generated source files and core primitives. 
## - Setting up include directories and compiler options for C++20 standard.
## - Optional building of a high-speed JSON Ingestor module that depends on simdjson.
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
option(FASTFHIR_RUN_GENERATOR "Run python code generation before building" ON)

# Build shared library by default; set OFF to build a static library.
option(FASTFHIR_BUILD_SHARED "Build FastFHIR as a shared library (dynamic linking)" ON)

# Toggle for building the high-speed JSON Ingestor module
option(FASTFHIR_BUILD_INGESTOR "Build the FastFHIR Ingestor (requires simdjson)" OFF)

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

# Exclude the Ingestor from the pure serialization package
list(REMOVE_ITEM FASTFHIR_CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/FF_Ingestor.cpp")

# Optionally run generator logic when configuring the project.  
if(FASTFHIR_RUN_GENERATOR)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)  

    execute_process(
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/generator/make_lib.py
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        RESULT_VARIABLE FASTFHIR_GENERATE_RESULT
    )

    if(NOT FASTFHIR_GENERATE_RESULT EQUAL 0)
        message(FATAL_ERROR "FastFHIR generation failed during CMake configure")
    endif()

    add_custom_target(fastfhir_generate
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/generator/make_lib.py
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Generating FastFHIR C++ sources"
        VERBATIM
    )
endif()

# Collect all generated C++ implementation files.
file(GLOB FASTFHIR_GENERATED_SOURCES CONFIGURE_DEPENDS
    ${FASTFHIR_GENERATED_DIR}/*.cpp
)

# Exclude the IngestMappings.cpp from the core library.
list(REMOVE_ITEM FASTFHIR_GENERATED_SOURCES "${FASTFHIR_GENERATED_DIR}/FF_IngestMappings.cpp")

if(NOT FASTFHIR_GENERATED_SOURCES)
    message(FATAL_ERROR "No generated .cpp files found. Run generator or enable FASTFHIR_RUN_GENERATOR.")
endif()

# ---------------------------------------------------------------------
# Intermediate Object Library (fastfhir_obj)
# ---------------------------------------------------------------------
add_library(fastfhir_obj OBJECT
    ${FASTFHIR_CORE_SOURCES}
    ${FASTFHIR_GENERATED_SOURCES}
)

target_include_directories(fastfhir_obj 
    PUBLIC 
        $<BUILD_INTERFACE:${FASTFHIR_INCLUDE_DIR}>
        $<BUILD_INTERFACE:${FASTFHIR_GENERATED_DIR}>
)

target_compile_features(fastfhir_obj PUBLIC cxx_std_20)

target_precompile_headers(fastfhir_obj 
    PRIVATE
        <vector> 
        <string> 
        <string_view> 
        <cstdint> 
        <memory>
        ${FASTFHIR_INCLUDE_DIR}/FF_Primitives.hpp
)

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

if(FASTFHIR_RUN_GENERATOR)
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
    find_package(simdjson QUIET)

    if(NOT simdjson_FOUND)
        message(STATUS "FastFHIR: Local simdjson not found. Fetching via FetchContent...")
        include(FetchContent)
        set(SIMDJSON_BUILD_TESTING OFF CACHE BOOL "" FORCE)
        set(SIMDJSON_JUST_LIBRARY ON CACHE BOOL "" FORCE)
        FetchContent_Declare(simdjson
            GIT_REPOSITORY https://github.com/simdjson/simdjson.git
            GIT_TAG v3.9.5
        )
        FetchContent_MakeAvailable(simdjson)
    endif()

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
# Install / Export Targets
# ---------------------------------------------------------------------
install(TARGETS fastfhir EXPORT FastFHIRTargets
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