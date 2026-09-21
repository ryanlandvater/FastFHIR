# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

# A consumer of the INSTALLED package, and nothing else: no source-tree include
# path, no build-tree library path. Driven by tests/install/install_smoke.cmake.
cmake_minimum_required(VERSION 3.20)
# C as well as CXX: the package ships a C ABI (FastFHIR.h), and only a C
# compiler can prove that header is valid C.
project(ff_install_consumer LANGUAGES C CXX)

find_package(FastFHIR CONFIG REQUIRED)

add_executable(ff_install_consumer main.cpp)
target_link_libraries(ff_install_consumer PRIVATE FastFHIR::fastfhir)
target_compile_features(ff_install_consumer PRIVATE cxx_std_20)

add_executable(ff_install_consumer_c main_c.c)
target_link_libraries(ff_install_consumer_c PRIVATE FastFHIR::fastfhir)
