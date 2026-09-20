# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

# install_smoke: install the build into a scratch prefix, then build and run a
# consumer that sees ONLY that prefix, through find_package(FastFHIR CONFIG).
#
# This is the test the install rules never had. The header list was written by
# hand, FF_Builder.hpp gained an include it did not name, and the installed
# FastFHIR.hpp stopped compiling while every in-tree test stayed green -- the
# in-tree tests compile against include/ and generated_src/, so they cannot see
# what a consumer of the package sees.
#
# Required -D arguments: BUILD_DIR, WORK_DIR, CONSUMER_DIR, CXX_COMPILER.
# Optional: CONFIG (multi-config generators).

foreach(_required BUILD_DIR WORK_DIR CONSUMER_DIR CXX_COMPILER)
    if(NOT DEFINED ${_required})
        message(FATAL_ERROR "install_smoke: -D${_required}=... is required")
    endif()
endforeach()

set(_prefix "${WORK_DIR}/prefix")
file(REMOVE_RECURSE "${WORK_DIR}")

function(_ff_run step)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "install_smoke: ${step} failed (${_rc})\n${_out}\n${_err}")
    endif()
endfunction()

set(_config_args "")
if(DEFINED CONFIG AND NOT CONFIG STREQUAL "")
    set(_config_args --config "${CONFIG}")
endif()

_ff_run("install" ${CMAKE_COMMAND} --install "${BUILD_DIR}" --prefix "${_prefix}" ${_config_args})

# The package must carry the public surface and must NOT carry the internal
# byte-arithmetic header (CLAUDE.md invariant 9).
foreach(_public FastFHIR.hpp FF_Conformance.hpp FF_DataTypes.hpp FF_Observation.hpp
                FF_Bundle.hpp FF_String.hpp FF_BundleIndex.hpp)
    if(NOT EXISTS "${_prefix}/include/${_public}")
        message(FATAL_ERROR "install_smoke: public header missing from the package: ${_public}")
    endif()
endforeach()
foreach(_internal FF_Ops.hpp FF_AllTypes.hpp FF_Observation_internal.hpp)
    if(EXISTS "${_prefix}/include/${_internal}")
        message(FATAL_ERROR "install_smoke: internal header shipped in the package: ${_internal}")
    endif()
endforeach()

_ff_run("consumer configure" ${CMAKE_COMMAND}
    -S "${CONSUMER_DIR}" -B "${WORK_DIR}/consumer"
    -DCMAKE_PREFIX_PATH=${_prefix}
    -DCMAKE_CXX_COMPILER=${CXX_COMPILER}
    -DCMAKE_BUILD_TYPE=Debug)
_ff_run("consumer build" ${CMAKE_COMMAND} --build "${WORK_DIR}/consumer")
_ff_run("consumer run" "${WORK_DIR}/consumer/ff_install_consumer")
message(STATUS "install_smoke: package at ${_prefix} builds and runs a consumer")
