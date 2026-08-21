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

    # ── Round-trip harness (invoked by the Python DOM parity suite) ──────
    cc_binary(
        name = "ff_roundtrip",
        srcs = ["//:tests/cpp/ff_roundtrip.cpp"],
        copts = copts,
        deps = ["//:fastfhir_ingestor", "@boringssl//:crypto"],
    )
