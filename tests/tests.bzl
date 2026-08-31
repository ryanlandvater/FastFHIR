"""FastFHIR test definitions (consumed by //:BUILD.bazel).

Every label is a root-package form (`//:...`): `tests/` has no BUILD file, so
`//tests:...` labels would be invalid, and labels in a .bzl resolve against the
package containing the .bzl — `//tests` — so even a bare `:fastfhir` would
silently point at the wrong package. Root-package labels are unambiguous for
both targets (`//:fastfhir`) and sources (`//:tests/cpp/test_x.cpp`).
"""

load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_test")

def fastfhir_tests(copts = []):
    """Registers the C++ unit suites, the README examples suite, and the
    round-trip harness. Mirrors the test set registered in tests/tests.cmake
    (CTest splits the README suite into per-example tests via --filter; Bazel
    runs the binary as one test)."""

    # ── Core-only suites ─────────────────────────────────────────────────
    for name, src in [
        ("test_primitives", "test_primitives.cpp"),
        ("test_memory", "test_memory.cpp"),
        ("test_simd", "test_simd.cpp"),
        ("test_amend", "test_amend.cpp"),
        ("test_cc", "test_codeable_concept.cpp"),
        ("test_compactor", "test_compactor.cpp"),
        ("test_graph_bounds", "ff_test_graph_bounds.cpp"),
        ("test_datetime", "test_datetime.cpp"),
        # UCUM/dictionary regression suite (68 enumerated checks, no seed).
        ("test_dictionary", "test_dictionary.cpp"),
        # AR-4.4: the first direct FIFO::Queue test. Header-only, no ingestor.
        ("test_queue", "ff_test_queue.cpp"),
    ]:
        cc_test(
            name = name,
            srcs = ["//:tests/cpp/" + src],
            copts = copts,
            deps = ["//:fastfhir"],
        )

    # ── Ingestor-driven suites ───────────────────────────────────────────
    cc_test(
        name = "test_bundle",
        srcs = ["//:tests/cpp/test_bundle_ingest.cpp"],
        copts = copts,
        deps = ["//:fastfhir_ingestor", "@simdjson//:simdjson"],
    )

    # WO-1 out-param contract test drives FF_Ingest, so it needs the ingestor.
    cc_test(
        name = "test_api",
        srcs = ["//:tests/cpp/test_api.cpp"],
        copts = copts,
        deps = ["//:fastfhir_ingestor", "@simdjson//:simdjson"],
    )

    cc_test(
        name = "test_readme",
        srcs = ["//:tests/cpp/test_readme.cpp"],
        copts = copts,
        deps = ["//:fastfhir_ingestor", "@asio//:asio", "@boringssl//:crypto"],
    )

    # ── End-to-end coverage suites (COV-1) ───────────────────────────────
    # These drive the ingestor AND need the checksum hasher, because they are
    # deliberately fed real Synthea documents rather than hand-built buffers.
    # Without FASTFHIR_SYNTHEA_DIR they print SKIP instead of passing on zero
    # coverage, so they are safe to register unconditionally here even though
    # Bazel does not download the corpus.
    #
    # test_recovery belongs here for the same reason and was simply missing:
    # ctest ran it as cpp_ff_test_recovery (tests/tests.cmake:127) while Bazel
    # had no target at all, so the whole recovery engine -- the two-sided
    # reconciliation, the hole analysis, the repair ranker -- built and ran on
    # exactly one of the two build systems. A defect reachable only through the
    # Bazel arm (which is what the benchmark repo links) would have been
    # invisible.
    for name, src in [
        ("test_roundtrip_validate", "ff_test_roundtrip_validate.cpp"),
        ("test_compact_roundtrip", "ff_test_compact_roundtrip.cpp"),
        ("test_recovery", "test_recovery.cpp"),
    ]:
        cc_test(
            name = name,
            srcs = ["//:tests/cpp/" + src],
            copts = copts,
            deps = [
                "//:fastfhir_ingestor",
                "@simdjson//:simdjson",
                "@boringssl//:crypto",
            ],
        )

    # ── Round-trip harness (invoked by the Python DOM parity suite) ──────
    cc_binary(
        name = "ff_roundtrip",
        srcs = ["//:tests/cpp/ff_roundtrip.cpp"],
        copts = copts,
        deps = ["//:fastfhir_ingestor", "@boringssl//:crypto"],
    )
