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
        # tests/cpp is on the path for the shared harness headers
        # (FFHR_tests.hpp, FFHR_test_corpus.hpp, FFHR_test_checksum.hpp),
        # included by bare name like every other FastFHIR header.
        target_include_directories(${NAME} PRIVATE
            ${FASTFHIR_INCLUDE_DIR} ${FASTFHIR_GENERATED_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/cpp
        )
        target_compile_definitions(${NAME} PRIVATE
            FF_TEST_ARTIFACT_DIR="${CMAKE_BINARY_DIR}/tests/cpp"
        )
        target_link_libraries(${NAME} PRIVATE fastfhir_obj)
        # First-party policy: the no-default enum-switch convention applies to
        # test code too (it exercises the same headers). Defined in CMakeLists.txt.
        _ff_enable_switch_warnings(${NAME})
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
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/cpp
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
    _ff_enable_switch_warnings(ff_test_readme)
    # ── Round-trip harness (invoked by Python DOM parity tests) ──
    add_executable(ff_roundtrip tests/cpp/ff_roundtrip.cpp)
    target_include_directories(ff_roundtrip PRIVATE
        ${FASTFHIR_INCLUDE_DIR} ${FASTFHIR_GENERATED_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/cpp
    )
    target_link_libraries(ff_roundtrip PRIVATE fastfhir_ingestor OpenSSL::Crypto)
    _ff_enable_switch_warnings(ff_roundtrip)

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
    add_ff_cpp_test(ff_test_dictionary tests/cpp/test_dictionary.cpp)
    # AR-4.4: FIFO::Queue had no direct test until a lock-free defect in it
    # silently dropped 2,000 tasks per ingest (AR-3). Header-only, no ingestor.
    add_ff_cpp_test(ff_test_queue      tests/cpp/ff_test_queue.cpp)
    # COV-1.1: validates streams the WRITER produced, so unlike the other
    # standalone suites it drives the ingestor, needs the checksum hasher, and
    # needs the fixture path -- it is deliberately fed real documents. Without
    # FASTFHIR_SYNTHEA_DIR it reports SKIP rather than passing on zero coverage.
    add_ff_cpp_test(ff_test_roundtrip_validate tests/cpp/ff_test_roundtrip_validate.cpp)
    target_link_libraries(ff_test_roundtrip_validate
        PRIVATE fastfhir_ingestor simdjson::simdjson OpenSSL::Crypto)
    target_compile_definitions(ff_test_roundtrip_validate PRIVATE
        $<$<BOOL:${FASTFHIR_DOWNLOAD_SYNTHEA}>:FASTFHIR_SYNTHEA_DIR="${_SYNTHEA_DIR}">)
    # COV-1.5: same shape, one stage further -- ingest a real bundle, compact it,
    # and require the compact export to be byte-identical to the standard one.
    # Same ingestor/hasher/fixture needs as COV-1.1 above, same SKIP behaviour.
    add_ff_cpp_test(ff_test_compact_roundtrip tests/cpp/ff_test_compact_roundtrip.cpp)
    target_link_libraries(ff_test_compact_roundtrip
        PRIVATE fastfhir_ingestor simdjson::simdjson OpenSSL::Crypto)
    target_compile_definitions(ff_test_compact_roundtrip PRIVATE
        $<$<BOOL:${FASTFHIR_DOWNLOAD_SYNTHEA}>:FASTFHIR_SYNTHEA_DIR="${_SYNTHEA_DIR}">)
    # P0-1/CAPI-13: the POCO (`as<T>()`) must agree with the reflective lens.
    # Same ingestor/hasher/fixture needs as the two above, same SKIP behaviour --
    # the lens is the oracle, so it has to be fed real documents.
    add_ff_cpp_test(ff_test_abstraction_parity tests/cpp/test_abstraction_parity.cpp)
    target_link_libraries(ff_test_abstraction_parity
        PRIVATE fastfhir_ingestor simdjson::simdjson OpenSSL::Crypto)
    target_compile_definitions(ff_test_abstraction_parity PRIVATE
        $<$<BOOL:${FASTFHIR_DOWNLOAD_SYNTHEA}>:FASTFHIR_SYNTHEA_DIR="${_SYNTHEA_DIR}">)
    # Recovery subsystem (TASKS.md REC-12 + review-round-2 regressions): clean
    # stream zero-false-positives, 1-bit VALIDATION flip, 1-bit offset flip,
    # both-halves never-silent. Needs the ingestor to build the fixture stream.
    # The generated lazy-view layer. Needs the ingestor + hasher because it
    # reads real writer output rather than a hand-built buffer (COV-1).
    add_ff_cpp_test(ff_test_views tests/cpp/test_views.cpp)
    target_link_libraries(ff_test_views
        PRIVATE fastfhir_ingestor simdjson::simdjson OpenSSL::Crypto)

    add_ff_cpp_test(ff_test_recovery tests/cpp/test_recovery.cpp)
    target_link_libraries(ff_test_recovery
        PRIVATE fastfhir_ingestor simdjson::simdjson OpenSSL::Crypto)
    # WO-1 out-param contract test drives FF_Ingest, so it needs the ingestor.
    target_link_libraries(ff_test_api PRIVATE fastfhir_ingestor simdjson::simdjson)

    # ── CTest entries ──────────────────────────────────────────────
    # Standalone self-contained suites. These were built but never registered,
    # so they compiled and never ran; add_ff_cpp_test only creates the target.
    foreach(_standalone ff_test_primitives ff_test_memory ff_test_simd ff_test_amend ff_test_cc ff_test_bundle ff_test_compactor ff_test_graph_bounds ff_test_datetime ff_test_api ff_test_dictionary ff_test_roundtrip_validate ff_test_compact_roundtrip ff_test_queue ff_test_abstraction_parity ff_test_recovery ff_test_views)
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

        # Executes the code blocks in python/README.md AS PUBLISHED, rather than
        # re-implementing them the way test_readme.py does. That distinction is
        # not academic: test_readme.py's test_1 sets `stream.root` before
        # finalize() and the README's Example 1 did not, so the suite was green
        # while the first example a Python user copies died with
        # "Cannot finalize because root is unset/invalid". Self-contained (it
        # seeds its own fixtures in a temp dir), so no DEPENDS ordering.
        add_test(NAME py_readme_examples
            COMMAND "${_PY}" -m pytest "${_PY_DIR}/test_readme_examples.py" -v)
        set_tests_properties(py_readme_examples PROPERTIES TIMEOUT 300)

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
        # --debug-on-failure re-runs a FAILING fixture through to_debug_json, so
        # the report names the recovery tag, field kind and byte offset behind
        # each difference instead of only the JSON path. Costs nothing while the
        # suite is green; running every fixture that way (--debug) took 249s
        # against 90s, which is not a tax worth paying forever for diagnostics
        # nobody reads on a pass. Needs a Debug build -- every preset is one, and
        # under NDEBUG the harness exits 2 with a clear message.
        add_test(NAME py_roundtrip
            COMMAND "${_PY}" "${_PY_DIR}/test_roundtrip.py"
                --synthea-dir "${_SYNTHEA_DIR}"
                --harness "${CMAKE_CURRENT_BINARY_DIR}/ff_roundtrip"
                --debug-on-failure
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
            py_readme_examples
            PROPERTIES ENVIRONMENT "PYTHONPATH=${_PYTHONPATH}")


        set_tests_properties(py_setup py_getting_started
            py_test_1 py_test_2 py_test_3 py_test_4 py_test_5 py_test_7 py_test_8 py_test_9
            PROPERTIES RESOURCE_LOCK ff_py_patient_ffhr)
        set_tests_properties(py_setup
            py_test_6 py_test_9 py_test_10
            PROPERTIES RESOURCE_LOCK ff_py_bundle_ffhr)
    endif()
endif()

