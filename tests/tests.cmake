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
    # ── ff_test_readme_examples: the README's OWN blocks, executed ─────
    # Generated from README.md at build time, not written by hand. This is the
    # parity half of the doc gate: py_readme_compiles proves the published
    # blocks still name real API, and this proves they still WORK -- by running
    # the published bytes rather than a re-implementation of them, which is how
    # test_readme.cpp above drifted onto a dead API while ctest stayed green.
    #
    # DEPENDS on README.md, so editing an example rebuilds and re-runs it.
    # BYPRODUCTS/generated file lives in the build tree: it is derived output
    # and must never be committed (CLAUDE.md invariant 2).
    set(_README_EXAMPLES_SRC "${CMAKE_BINARY_DIR}/generated_tests/readme_examples.generated.cpp")
    add_custom_command(
        OUTPUT "${_README_EXAMPLES_SRC}"
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/readme/generate_examples.py"
                --readme "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
                --out    "${_README_EXAMPLES_SRC}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/readme/generate_examples.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/readme/extract.py"
        COMMENT "Extracting runnable README.md examples"
        VERBATIM
    )
    add_executable(ff_test_readme_examples "${_README_EXAMPLES_SRC}")
    target_include_directories(ff_test_readme_examples PRIVATE
        ${FASTFHIR_INCLUDE_DIR} ${FASTFHIR_GENERATED_DIR} ${ASIO_INCLUDE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/readme
    )
    target_compile_definitions(ff_test_readme_examples PRIVATE
        ASIO_STANDALONE
        FASTFHIR_TEST_ARTIFACT_DIR="${CMAKE_BINARY_DIR}/tests/cpp"
    )
    if(WIN32)
        target_compile_definitions(ff_test_readme_examples PRIVATE
            _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX)
        target_link_libraries(ff_test_readme_examples PRIVATE ws2_32)
    endif()
    target_link_libraries(ff_test_readme_examples
        PRIVATE fastfhir_ingestor simdjson::simdjson OpenSSL::Crypto)
    _ff_enable_switch_warnings(ff_test_readme_examples)

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
    # APPEND-1: tail-rewrite append; its map checks use FF_Recovery (core).
    add_ff_cpp_test(ff_test_bundle_append tests/cpp/test_bundle_append.cpp)
    add_ff_cpp_test(ff_test_file_modes tests/cpp/test_file_modes.cpp)
    add_ff_cpp_test(ff_test_poco_values tests/cpp/test_poco_values.cpp)
    # T11: the Bundle reference index, checked against the O(N) scan it
    # replaces. Reads real writer output, so no ingestor and no fixtures.
    add_ff_cpp_test(ff_test_bundle_index tests/cpp/test_bundle_index.cpp)
    # Unlike the other standalone suites this one drives the JSON ingestor.
    target_link_libraries(ff_test_bundle PRIVATE fastfhir_ingestor simdjson::simdjson)
    add_ff_cpp_test(ff_test_simd       tests/cpp/test_simd.cpp)
    add_ff_cpp_test(ff_test_compactor  tests/cpp/test_compactor.cpp)
    add_ff_cpp_test(ff_test_graph_bounds tests/cpp/ff_test_graph_bounds.cpp)
    add_ff_cpp_test(ff_test_datetime   tests/cpp/test_datetime.cpp)
    add_ff_cpp_test(ff_test_identity   tests/cpp/test_identity.cpp)
    add_ff_cpp_test(ff_test_api        tests/cpp/test_api.cpp)
    add_ff_cpp_test(ff_test_dictionary tests/cpp/test_dictionary.cpp)
    # AR-4.4: FIFO::Queue had no direct test until a lock-free defect in it
    # silently dropped 2,000 tasks per ingest (AR-3). Header-only, no ingestor.
    add_ff_cpp_test(ff_test_queue      tests/cpp/ff_test_queue.cpp)
    # LOG-1: ConcurrentLogger's CAS claim, refusal and contention. Header-only.
    add_ff_cpp_test(ff_test_logger     tests/cpp/test_logger.cpp)
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
    # P0-1/C_API-13: the POCO (`as<T>()`) must agree with the reflective lens.
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
    # The conformance layer test links the OPT-IN layer library, so it exists
    # only when that option is on. Registered in all four places CLAUDE.md
    # names -- here, the ctest foreach below, _BUILD_ALL, and the IDE lists --
    # because add_ff_cpp_test() only creates the target.
    if(FASTFHIR_BUILD_CONFORMANCE)
        add_ff_cpp_test(ff_test_conformance tests/cpp/test_conformance.cpp)
        # The ingest byte-identity case (COV-1) drives FF_Ingest over real
        # Synthea bundles, so it needs the ingestor and the corpus path too.
        target_link_libraries(ff_test_conformance
            PRIVATE fastfhir_conformance fastfhir_ingestor simdjson::simdjson)
        target_compile_definitions(ff_test_conformance PRIVATE
            $<$<BOOL:${FASTFHIR_DOWNLOAD_SYNTHEA}>:FASTFHIR_SYNTHEA_DIR="${_SYNTHEA_DIR}">)
        # The worked example is registered as a test so it cannot rot: an
        # example that stops compiling is documentation that lies.
        add_ff_cpp_test(ff_example_conformance examples/conformance_layer.cpp)
        target_link_libraries(ff_example_conformance PRIVATE fastfhir_conformance)
    endif()

    add_ff_cpp_test(ff_test_views tests/cpp/test_views.cpp)
    target_link_libraries(ff_test_views
        PRIVATE fastfhir_ingestor simdjson::simdjson OpenSSL::Crypto)

    add_ff_cpp_test(ff_test_recovery tests/cpp/test_recovery.cpp)
    target_link_libraries(ff_test_recovery
        PRIVATE fastfhir_ingestor simdjson::simdjson OpenSSL::Crypto)
    # REC-25's census is checked on real Synthea bundles as well as the two
    # hand-built fixtures; without the corpus that part reports SKIP.
    target_compile_definitions(ff_test_recovery PRIVATE
        $<$<BOOL:${FASTFHIR_DOWNLOAD_SYNTHEA}>:FASTFHIR_SYNTHEA_DIR="${_SYNTHEA_DIR}">)
    # The URL-directory case ingests a Patient with an extension.
    target_link_libraries(ff_test_file_modes
        PRIVATE fastfhir_ingestor simdjson::simdjson OpenSSL::Crypto)
    # WO-1 out-param contract test drives FF_Ingest, so it needs the ingestor.
    target_link_libraries(ff_test_api PRIVATE fastfhir_ingestor simdjson::simdjson)

    # ── C ABI ──────────────────────────────────────────────────────
    # A .c file, so CMake drives the C COMPILER for it and the run proves the
    # FF_* C surface links and runs from C. Linked against the real `fastfhir`
    # library (not fastfhir_obj) so the extern "C" symbols are exercised across
    # a library boundary the way a third-party C consumer would.
    add_executable(ff_test_c_api tests/cpp/test_c_api.c)
    target_include_directories(ff_test_c_api PRIVATE ${FASTFHIR_INCLUDE_DIR})
    target_link_libraries(ff_test_c_api PRIVATE fastfhir)

    # The C surface cannot append a TYPED resource (the typed append is a C++
    # template), so a typed .ffhr is produced from C++ and consumed by the C
    # reader. CTest fixtures sequence the two.
    add_executable(ff_make_c_api_fixture tests/cpp/c_api_fixture.cpp)
    target_include_directories(ff_make_c_api_fixture PRIVATE
        ${FASTFHIR_INCLUDE_DIR} ${FASTFHIR_GENERATED_DIR})
    target_link_libraries(ff_make_c_api_fixture PRIVATE fastfhir_obj)
    _ff_enable_switch_warnings(ff_make_c_api_fixture)

    # ── CTest entries ──────────────────────────────────────────────
    # Standalone self-contained suites. These were built but never registered,
    # so they compiled and never ran; add_ff_cpp_test only creates the target.
    set(_FF_STANDALONE_TESTS ff_test_primitives ff_test_memory ff_test_simd ff_test_amend ff_test_cc ff_test_bundle ff_test_compactor ff_test_graph_bounds ff_test_datetime ff_test_identity ff_test_api ff_test_dictionary ff_test_roundtrip_validate ff_test_compact_roundtrip ff_test_queue ff_test_logger ff_test_abstraction_parity ff_test_views ff_test_bundle_append ff_test_file_modes ff_test_poco_values ff_test_bundle_index)
    if(FASTFHIR_BUILD_CONFORMANCE)
        list(APPEND _FF_STANDALONE_TESTS ff_test_conformance ff_example_conformance)
    endif()
    foreach(_standalone ${_FF_STANDALONE_TESTS})
        add_test(NAME "cpp_${_standalone}" COMMAND ${_standalone})
    endforeach()

    # RECOVERY: the legacy cases and the WP1 gates are registered SEPARATELY,
    # and the gates one at a time.
    #
    # The gates (../FastFHIR-benchmark/recovery_handoff.md §6) assert that a
    # repair never writes onto an undamaged byte and that damage it cannot fix
    # is REPORTED rather than dropped. Three of them are red today on purpose:
    # they are the progress meter for the repair work packages, and the measured
    # red list lives in the block comment above gate_set_options() in
    # tests/cpp/test_recovery.cpp.
    #
    # WILL_FAIL is what keeps that honest in BOTH directions. A red gate does
    # not drown the 251 checks guarding everything else, and the moment a work
    # package fixes one, its entry FLIPS TO FAILING -- so whoever fixed it has
    # to come back, clear the property and update the red list. A known-failing
    # test nobody is forced to revisit is how a suite starts lying, which is the
    # failure these gates exist to end.
    add_test(NAME cpp_ff_test_recovery COMMAND ff_test_recovery --gates off)
    foreach(_gate clean_stream_zero_writes repair_is_idempotent
                  hole_repoint_applies_both_witnesses tuple_self_and_tag_repair_together
                  inline_element_is_never_repointed census_single_flip_sample_synthea)
        add_test(NAME "cpp_recovery_gate_${_gate}"
                 COMMAND ff_test_recovery --gates only --filter ${_gate})
    endforeach()
    foreach(_gate single_flip_oracle paired_flip_oracle header_single_bit_enumeration)
        add_test(NAME "cpp_recovery_gate_${_gate}"
                 COMMAND ff_test_recovery --gates only --filter ${_gate})
        set_tests_properties("cpp_recovery_gate_${_gate}" PROPERTIES WILL_FAIL TRUE)
    endforeach()

    # XP-1.3: without the visited set the heavy-sharing case does not fail, it
    # hangs; the timeout is what turns that into a reported failure.
    set_tests_properties(cpp_ff_test_graph_bounds PROPERTIES TIMEOUT 60)

    # The C ABI: a typed fixture is sealed first, then the C reader consumes it.
    add_test(NAME c_api_fixture
        COMMAND ff_make_c_api_fixture ${CMAKE_BINARY_DIR}/c_api_fixture.ffhr)
    set_tests_properties(c_api_fixture PROPERTIES FIXTURES_SETUP c_api_fixture)
    add_test(NAME cpp_ff_test_c_api
        COMMAND ff_test_c_api ${CMAKE_BINARY_DIR}/c_api_fixture.ffhr)
    set_tests_properties(cpp_ff_test_c_api PROPERTIES
        FIXTURES_REQUIRED c_api_fixture TIMEOUT 60)

    # Leak check for the C surface. ONE mechanism: the platform's own leak tool,
    # run over the SAME c_api test binary (which already creates and destroys
    # every handle type). No hand-rolled allocation counter -- a second counter
    # disagreed with this tool and produced a false positive.
    #   macOS: `leaks --atExit` (exit code is always 0, so match its report).
    #   Linux: LeakSanitizer via the xcode-asan/ASan preset.
    if(APPLE)
        find_program(FF_LEAKS_PROGRAM leaks)
        if(FF_LEAKS_PROGRAM)
            add_test(NAME c_api_leaks
                COMMAND sh -c
                    "${FF_LEAKS_PROGRAM} --atExit -- $<TARGET_FILE:ff_test_c_api> ${CMAKE_BINARY_DIR}/c_api_fixture.ffhr 2>&1 | grep -q '0 leaks for 0 total leaked bytes'")
            set_tests_properties(c_api_leaks PROPERTIES
                FIXTURES_REQUIRED c_api_fixture TIMEOUT 120)
        endif()
    endif()

    # The installed package, consumed through find_package from a scratch
    # prefix under the build dir. See tests/install/install_smoke.cmake.
    add_test(NAME install_smoke
        COMMAND ${CMAKE_COMMAND}
            -DBUILD_DIR=${CMAKE_BINARY_DIR}
            -DWORK_DIR=${CMAKE_BINARY_DIR}/install_smoke
            -DCONSUMER_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/install/consumer
            -DCXX_COMPILER=${CMAKE_CXX_COMPILER}
            -DC_COMPILER=${CMAKE_C_COMPILER}
            -DCONFIG=$<CONFIG>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/install/install_smoke.cmake)
    set_tests_properties(install_smoke PROPERTIES TIMEOUT 300)

    macro(_add_cpp_test NAME FILTER)
        add_test(NAME "cpp_${NAME}"
            COMMAND ff_test_readme --filter "${FILTER}")
    endmacro()

    # One ctest entry per executed README block. The --filter values are the
    # block's own `run=` ids from README.md, so a failure names the block a
    # reader would have copied.
    foreach(_ex step1_parse step3_build example_1_ingest example_2_read
                example_3_enrich example_5_surgical example_6_concurrent
                example_6_backfill)
        add_test(NAME "cpp_readme_${_ex}"
            COMMAND ff_test_readme_examples --filter "${_ex}")
    endforeach()
    # They share patient.ffhr / bundle.ffhr in the artifact dir. Each block
    # seeds its own fixtures (see tests/readme/expect.hpp), so they are correct
    # in isolation -- but not concurrently, because two of them write the same
    # filename. Serialise on the same locks the hand-written suite uses.
    set_tests_properties(
        cpp_readme_step1_parse cpp_readme_example_1_ingest
        cpp_readme_example_2_read cpp_readme_example_3_enrich
        PROPERTIES RESOURCE_LOCK ff_readme_patient_ffhr)
    set_tests_properties(cpp_readme_example_5_surgical
        PROPERTIES RESOURCE_LOCK ff_readme_bundle_ffhr)

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

        # Python test entries. Everything in this block imports `fastfhir`,
        # which exists only when the bindings are built -- the xcode presets
        # leave them OFF, and registering these there reported every one as a
        # failure of the code when it was a property of the configuration.
        # py_readme_compiles and py_roundtrip import no fastfhir module and
        # stay registered below.
        set(_PY_BINDING_TESTS "")
        if(FASTFHIR_BUILD_PYTHON_BINDINGS)
            macro(_add_py_test NAME FN)
                add_test(NAME "py_${NAME}"
                    COMMAND "${_PY}" -m pytest "${_PY_DIR}/test_readme.py::${FN}" -v)
                list(APPEND _PY_BINDING_TESTS "py_${NAME}")
            endmacro()

            _add_py_test(getting_started test_getting_started)
            foreach(N RANGE 1 10)
                _add_py_test("test_${N}" "test_${N}")
            endforeach()

            # Executes the code blocks in python/README.md AS PUBLISHED, rather than
            # re-implementing them the way test_readme.py does. That distinction is
            # not academic: test_readme.py's test_1 sets `builder.root` before
            # finalize() and the README's Example 1 did not, so the suite was green
            # while the first example a Python user copies died with
            # "Cannot finalize because root is unset/invalid". Self-contained (it
            # seeds its own fixtures in a temp dir), so no DEPENDS ordering.
            add_test(NAME py_readme_examples
                COMMAND "${_PY}" -m pytest "${_PY_DIR}/test_readme_examples.py" -v)
            set_tests_properties(py_readme_examples PROPERTIES TIMEOUT 300)
            list(APPEND _PY_BINDING_TESTS py_readme_examples)
        endif()

        # The C++ counterpart of py_readme_examples: compiles every ```cpp block
        # in the ROOT README.md as published. ff_test_readme is a hand-written
        # parallel implementation of the same examples, so it proves they WORK
        # and cannot prove the README's own bytes are valid -- which is how all
        # six C++ examples drifted onto a dead Builder/Ingestor API while ctest
        # stayed green. -fsyntax-only, so it needs headers and no libraries.
        #
        # The include dirs are passed explicitly rather than probed: the gate
        # must see the SAME third-party headers this build compiles against.
        # Its standalone fallback reads them out of CMakeCache.txt.
        set(_README_GATE_ARGS
            --readme "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
            --build-dir "${CMAKE_CURRENT_BINARY_DIR}"
            --cxx "${CMAKE_CXX_COMPILER}"
            --cc "${CMAKE_C_COMPILER}"
            --include-dir "${FASTFHIR_INCLUDE_DIR}"
            --include-dir "${FASTFHIR_GENERATED_DIR}"
        )
        if(ASIO_INCLUDE_DIR)
            list(APPEND _README_GATE_ARGS --include-dir "${ASIO_INCLUDE_DIR}")
        endif()
        if(OPENSSL_INCLUDE_DIR)
            list(APPEND _README_GATE_ARGS --include-dir "${OPENSSL_INCLUDE_DIR}")
        endif()
        if(simdjson_SOURCE_DIR)
            list(APPEND _README_GATE_ARGS --include-dir "${simdjson_SOURCE_DIR}/include")
        endif()
        # Registered only with the ingestor: four of the README's blocks include
        # <FF_Ingestor.hpp>, which includes simdjson, and simdjson is only
        # fetched when FASTFHIR_BUILD_INGESTOR is ON. Without it the gate could
        # not check the examples that matter, and a gate that checks the
        # leftovers while reporting success is worse than no gate. Every preset
        # enables the ingestor. NOTE: deliberately absent from the PYTHONPATH
        # list below -- it imports no fastfhir module, so it runs without the
        # staged Python package and before the bindings are built.
        if(FASTFHIR_BUILD_INGESTOR)
            add_test(NAME py_readme_compiles
                COMMAND "${_PY}" "${_PY_DIR}/test_readme_compiles.py" ${_README_GATE_ARGS})
            set_tests_properties(py_readme_compiles PROPERTIES TIMEOUT 600)
        endif()

        if(FASTFHIR_BUILD_PYTHON_BINDINGS)
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

            set_tests_properties(py_setup py_getting_started
                py_test_1 py_test_2 py_test_3 py_test_4 py_test_5 py_test_7 py_test_8 py_test_9
                PROPERTIES RESOURCE_LOCK ff_py_patient_ffhr)
            set_tests_properties(py_setup
                py_test_6 py_test_9 py_test_10
                PROPERTIES RESOURCE_LOCK ff_py_bundle_ffhr)
        endif()

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
                --harness "$<TARGET_FILE:ff_roundtrip>"
                --debug-on-failure
        )
        # SKIP_RETURN_CODE: no Synthea corpus on this machine means the gate did
        # not run, and ctest must say "Skipped", never "Passed" (COV-2). A corpus
        # directory that yields no fixtures still fails.
        set_tests_properties(py_roundtrip PROPERTIES
            DEPENDS "py_setup;ff_roundtrip"
            SKIP_RETURN_CODE 77
        )

        # The gate's own failure paths (A22.2 / COV-2): a missing, non-executable,
        # hung or failing harness, and empty or absent corpora, each driven by a
        # stand-in harness. Needs neither the C++ build nor Synthea, so no DEPENDS.
        add_test(NAME py_roundtrip_errors
            COMMAND "${_PY}" -m pytest "${_PY_DIR}/test_roundtrip_errors.py" -v)
        set_tests_properties(py_roundtrip_errors PROPERTIES TIMEOUT 120)

        # PYTHONPATH for the py_* tests: the staged importable package
        # (build/python, assembled by fastfhir_python's POST_BUILD step — A21)
        # plus the tests' own directory (roundtrip_diff, fixtures).
        if(WIN32)
            set(_PYTHONPATH "${CMAKE_CURRENT_BINARY_DIR}/python\\;${_PY_DIR}")
        else()
            set(_PYTHONPATH "${CMAKE_CURRENT_BINARY_DIR}/python:${_PY_DIR}")
        endif()
        set_tests_properties(py_setup py_roundtrip py_roundtrip_errors ${_PY_BINDING_TESTS}
            PROPERTIES ENVIRONMENT "PYTHONPATH=${_PYTHONPATH}")
    endif()
endif()

