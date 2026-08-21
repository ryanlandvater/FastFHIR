# =============================================================================
# tests/tests.cmake — all FastFHIR test registration (C++ + Python).
#
# Extracted from CMakeLists.txt so the root build file stays a readable
# build definition. Included by CMakeLists.txt; self-gating on
# FASTFHIR_BUILD_TESTS. Expects, from the caller scope: FASTFHIR_INCLUDE_DIR,
# FASTFHIR_GENERATED_DIR, and the fastfhir_obj / fastfhir_ingestor targets.
# =============================================================================


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
            ${FASTFHIR_INCLUDE_DIR} ${FASTFHIR_GENERATED_DIR}
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
        ${FASTFHIR_INCLUDE_DIR} ${FASTFHIR_GENERATED_DIR} ${ASIO_INCLUDE_DIR}
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
        ${FASTFHIR_INCLUDE_DIR} ${FASTFHIR_GENERATED_DIR}
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
    add_ff_cpp_test(ff_test_compactor  tests/cpp/test_compactor.cpp)
    add_ff_cpp_test(ff_test_graph_bounds tests/cpp/ff_test_graph_bounds.cpp)
    add_ff_cpp_test(ff_test_datetime   tests/cpp/test_datetime.cpp)
    add_ff_cpp_test(ff_test_api        tests/cpp/test_api.cpp)
    # WO-1 out-param contract test drives FF_Ingest, so it needs the ingestor.
    target_link_libraries(ff_test_api PRIVATE fastfhir_ingestor simdjson::simdjson)

    # ── CTest entries ──────────────────────────────────────────────
    # Standalone self-contained suites. These were built but never registered,
    # so they compiled and never ran; add_ff_cpp_test only creates the target.
    foreach(_standalone ff_test_primitives ff_test_memory ff_test_simd ff_test_amend ff_test_cc ff_test_bundle ff_test_compactor ff_test_graph_bounds ff_test_datetime ff_test_api)
        add_test(NAME "cpp_${_standalone}" COMMAND ${_standalone})
    endforeach()

    # XP-1.3: without the visited set the heavy-sharing case does not fail, it
    # hangs; the timeout is what turns that into a reported failure.
    set_tests_properties(cpp_ff_test_graph_bounds PROPERTIES TIMEOUT 60)

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
        # A21.3: no .venv preference. The tree's .venv historically held only
        # pip and was silently selected, breaking every py_* test. A19 floors
        # the interpreter at 3.11; a dev who wants a venv can override
        # Python3_EXECUTABLE explicitly.
        set(_PY "${Python3_EXECUTABLE}")

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

        # PYTHONPATH for the py_* tests: the staged importable package
        # (build/python, assembled by fastfhir_python's POST_BUILD step — A21)
        # plus the tests' own directory (roundtrip_diff, fixtures).
        if(WIN32)
            set(_PYTHONPATH "${CMAKE_CURRENT_BINARY_DIR}/python\\;${_PY_DIR}")
        else()
            set(_PYTHONPATH "${CMAKE_CURRENT_BINARY_DIR}/python:${_PY_DIR}")
        endif()
        set_tests_properties(py_setup py_getting_started
            py_test_1 py_test_2 py_test_3 py_test_4 py_test_5 py_test_6
            py_test_7 py_test_8 py_test_9 py_test_10 py_roundtrip
            PROPERTIES ENVIRONMENT "PYTHONPATH=${_PYTHONPATH}")


        set_tests_properties(py_setup py_getting_started
            py_test_1 py_test_2 py_test_3 py_test_4 py_test_5 py_test_7 py_test_8 py_test_9
            PROPERTIES RESOURCE_LOCK ff_py_patient_ffhr)
        set_tests_properties(py_setup
            py_test_6 py_test_9 py_test_10
            PROPERTIES RESOURCE_LOCK ff_py_bundle_ffhr)
    endif()
endif()

