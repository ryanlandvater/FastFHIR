// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Copyright (c) 2025 Ryan Landvater
//
// ─────────────────────────────────────────────────────────────────────────────
// Harness for the generated README example runner.
//
// The README's code blocks are executed verbatim by
// tests/readme/generate_examples.py. Everything that must NOT appear in the
// documentation lives here instead:
//
//   FF_README_SETUP_<ID>    expands BEFORE the block -- fixtures, and any file
//                           the block expects to already exist. Document order
//                           is not dependency order ("Step 1" reads
//                           patient.ffhr but is printed before the block that
//                           writes it), so each example owns its own
//                           preconditions rather than relying on its position.
//   FF_README_EXPECT_<ID>   expands AFTER the block, inside its scope, so it
//                           can assert on the block's own locals without the
//                           README ever mentioning a test.
//
// Both default to nothing, so a new `run=` block runs and asserts only what it
// crashes on until its expectations are written. That is deliberate: the
// generator's floor catches an empty runner, and a block with no expectations
// is reported by --report rather than silently counting as covered.
// ─────────────────────────────────────────────────────────────────────────────

#ifndef FF_README_EXPECT_HPP
#define FF_README_EXPECT_HPP

#include <FastFHIR.hpp>

#include "FF_AllTypes.hpp"
#include "FF_FieldKeys.hpp"

#include <openssl/sha.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "FFHR_tests.hpp"   // CHECK / REQUIRE / ff_test counters

namespace ff_readme {

namespace fs = std::filesystem;

// ── Where the examples run ───────────────────────────────────────────────────
// The blocks name relative paths ("patient.ffhr", "patient.json") because that
// is what reads well in documentation. Rather than rewrite those literals --
// which would break the parity this whole mechanism exists for -- the runner
// chdir's into a scratch directory, so the published text is also the correct
// text.
inline fs::path artifact_dir()
{
    fs::path p;
#ifdef FASTFHIR_TEST_ARTIFACT_DIR
    p = fs::path(FASTFHIR_TEST_ARTIFACT_DIR);
#endif
    if (p.empty())
        p = fs::temp_directory_path() / "fastfhir_test_artifacts";
    p /= "readme_examples";
    std::error_code ec;
    fs::create_directories(p, ec);
    return p;
}

inline void init(int argc, char** argv)
{
    ff_test::set_filter(argc, argv);
    TEST_GROUP("README examples");
    const auto dir = artifact_dir();
    fs::current_path(dir);
    std::cout << "  running in " << dir << "\n";
}

// One example, dispatched through the shared harness so --filter and the case
// counter behave as they do everywhere else.
//
// A throw is recorded as a failure of THIS example and does not abort the run.
// That matters more here than in a unit test: these blocks are chained by
// design (Example 1 writes the file Example 2 opens), and letting the first
// broken one hide the other six is how a documentation suite stops being read.
template <typename Fn>
inline void run(const char* id, const char* where, Fn fn)
{
    ff_test::run(id, [&]
    {
        std::cout << "\n  -- " << id << "  (" << where << ")\n";
        try
        {
            fn();
        }
        catch (const std::exception& e)
        {
            CHECK(false, id << " threw: " << e.what() << "  [" << where << "]");
        }
        catch (...)
        {
            CHECK(false, id << " threw a non-std exception  [" << where << "]");
        }
    });
}

inline int report()
{
    return ff_test::report("every README example ran and matched expectations");
}

// ── Fixture helpers ──────────────────────────────────────────────────────────

inline std::vector<BYTE> sha256_hasher(const unsigned char* p, Size n)
{
    std::vector<BYTE> out(SHA256_DIGEST_LENGTH);
    SHA256(p, n, out.data());
    return out;
}

inline constexpr std::string_view PATIENT_JSON = R"({
    "resourceType": "Patient",
    "id": "patient-1",
    "active": true,
    "gender": "male",
    "name": [{"use": "usual", "family": "Landvater", "given": ["Ryan", "Eric"]}],
    "address": [{"use": "home", "line": ["123 Main St"], "city": "Springfield",
                 "state": "IL", "postalCode": "62701", "country": "US"}],
    "identifier": [{"system": "http://example.org/fhir/ids", "value": "12345"}]
})";

inline constexpr std::string_view BUNDLE_JSON = R"({
    "resourceType": "Bundle",
    "type": "collection",
    "entry": [
        {"resource": {"resourceType": "Patient", "id": "patient-1",
                      "gender": "male", "active": true}},
        {"resource": {"resourceType": "Patient", "id": "patient-42",
                      "gender": "female", "active": true}}
    ]
})";

inline void write_file(const fs::path& at, std::string_view bytes)
{
    std::ofstream out(at, std::ios::binary | std::ios::trunc);
    if (!out)
        throw std::runtime_error("cannot write fixture: " + at.string());
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

/// Ingest @p json into @p path and seal it, so a read-path example has
/// something real to open. Uses the public FF_* surface only -- if this helper
/// stops compiling, the examples it feeds are wrong too.
inline void seal_resource(const fs::path& path, std::string_view json)
{
    std::error_code ec;
    fs::remove(path, ec);
    auto mem = FastFHIR::Memory::createFromFile(path.string(), 64 * 1024 * 1024);
    FastFHIR::FF_StreamCreateInfo stream_info;
    stream_info.arena = std::make_shared<FastFHIR::Memory>(mem);
    stream_info.version = FHIR_VERSION_R5;
    FastFHIR::FF_Stream stream;
    if (!FastFHIR::FF_CreateStream(stream_info, stream))
        throw std::runtime_error("fixture: FF_CreateStream failed");

    FastFHIR::FF_IngestorCreateInfo ingestor_info;
    FastFHIR::FF_Ingestor ingestor;
    if (!FastFHIR::FF_CreateIngestor(ingestor_info, ingestor))
        throw std::runtime_error("fixture: FF_CreateIngestor failed");

    FastFHIR::Reflective::ObjectHandle handle;
    Size count = 0;
    const auto r = FastFHIR::FF_Ingest(FastFHIR::FF_IngestInfo{
        .ingestor = ingestor,
        .stream = stream,
        .source_type = FF_SOURCE_FHIR_JSON,
        .payload = json,
    }, handle, count);
    if (r.code != FF_SUCCESS)
        throw std::runtime_error("fixture: ingest failed: " + r.message);

    if (!FastFHIR::FF_StreamSetRoot(FastFHIR::FF_StreamSetRootInfo{
            .stream = stream, .root = handle}))
        throw std::runtime_error("fixture: FF_StreamSetRoot failed");

    FastFHIR::Memory::View view;
    if (!FastFHIR::FF_StreamFinalize(FastFHIR::FF_StreamFinalizeInfo{
            .stream = stream,
            .algorithm = FF_CHECKSUM_SHA256,
            .hasher = sha256_hasher,
        }, view))
        throw std::runtime_error("fixture: FF_StreamFinalize failed");
}

/// Read back a sealed file and hand out the root node's JSON, for expectations
/// that want to assert on what actually landed on disk.
inline std::string exported_json(const fs::path& path)
{
    auto mem = FastFHIR::Memory::createFromFile(path.string(), 64 * 1024 * 1024);
    FastFHIR::Parser parser(mem);
    auto root = parser.root();
    if (!root)
        throw std::runtime_error("re-open: root node is null: " + path.string());
    std::ostringstream oss;
    root.print_json(oss);
    return oss.str();
}

} // namespace ff_readme

// ─────────────────────────────────────────────────────────────────────────────
// Per-example setup and expectations.
//
// Each pair is named for the block's `run=` id. Keep them in README order so
// this file reads alongside the page.
// ─────────────────────────────────────────────────────────────────────────────

// Default to nothing, so a new run= block does not fail to compile before its
// expectations are written. #undef + #define below for the ones that have them.
#define FF_README_SETUP_DEFAULT
#define FF_README_EXPECT_DEFAULT

// ── Step 1 — Parse raw bytes ────────────────────────────────────────────────
// A `program` block: the generator renames its main() and the expectation
// calls it. Needs patient.ffhr to exist, which "Step 3" does not write and
// Example 1 writes only later on the page.
#define FF_README_SETUP_STEP1_PARSE                                            \
    ff_readme::seal_resource("patient.ffhr", ff_readme::PATIENT_JSON);

#define FF_README_EXPECT_STEP1_PARSE                                           \
    REQUIRE(ff_readme_step1_parse_main() == 0,                                 \
            "Step 1 program returned non-zero");                               \
    {                                                                          \
        const std::string json = ff_readme::exported_json("patient.ffhr");     \
        REQUIRE(json.find("patient-1") != std::string::npos,                   \
                "Step 1: id missing from the file it parsed");                 \
        REQUIRE(json.find("Landvater") != std::string::npos,                   \
                "Step 1: family name missing");                                \
    }

// ── Step 3 — Build a FastFHIR record from FHIR JSON ─────────────────────────
// Self-contained: anonymous arena, inline JSON, reads itself back. The block
// leaves `id`, `gender`, `active` and `birthdate` in scope.
#define FF_README_SETUP_STEP3_BUILD

#define FF_README_EXPECT_STEP3_BUILD                                           \
    REQUIRE(!view.empty(), "Step 3: finalize produced an empty view");          \
    REQUIRE(id == "patient-1", "Step 3: unexpected id");                        \
    REQUIRE(gender == "male", "Step 3: unexpected gender");                     \
    REQUIRE(active, "Step 3: active should read back true");                    \
    REQUIRE(parsed_count > 0, "Step 3: ingest reported 0 resources");

// ── Example 1 — Ingest a FHIR JSON file and save as .ffhr ───────────────────
// The block opens "patient.json" through the Step 1 helper, so the fixture has
// to be on disk under that exact relative name.
#define FF_README_SETUP_EXAMPLE_1_INGEST                                        \
    ff_readme::write_file("patient.json", ff_readme::PATIENT_JSON);             \
    {                                                                          \
        std::error_code _ec;                                                   \
        std::filesystem::remove("patient.ffhr", _ec);                          \
    }

#define FF_README_EXPECT_EXAMPLE_1_INGEST                                       \
    REQUIRE(parsed_count > 0, "Example 1: ingest parsed 0 resources");         \
    REQUIRE(patient_handle, "Example 1: patient handle is null after ingest"); \
    REQUIRE(!view.empty(), "Example 1: finalize returned an empty view");      \
    {                                                                          \
        /* The point of this example is a portable file, so assert on the   */ \
        /* bytes that landed rather than on the handle still in scope.      */ \
        const std::string json = ff_readme::exported_json("patient.ffhr");     \
        REQUIRE(json.find("patient-1") != std::string::npos,                   \
                "Example 1: id absent from the sealed archive");                \
        REQUIRE(json.find("Landvater") != std::string::npos,                   \
                "Example 1: family name absent from the sealed archive");       \
    }

// ── Example 2 — Open and read a .ffhr file ──────────────────────────────────
// Pure read path. Depends on a sealed patient.ffhr; seeded here rather than
// relying on Example 1 having run, so --filter example_2 works alone.
#define FF_README_SETUP_EXAMPLE_2_READ                                          \
    ff_readme::seal_resource("patient.ffhr", ff_readme::PATIENT_JSON);

#define FF_README_EXPECT_EXAMPLE_2_READ                                         \
    REQUIRE(parser_says_patient, "Example 2: parser did not report a Patient"); \
    REQUIRE(root_is_patient, "Example 2: root node did not report a Patient");  \
    REQUIRE(id == "patient-1", "Example 2: unexpected id");                     \
    REQUIRE(gender == "male", "Example 2: unexpected gender");                  \
    REQUIRE(active, "Example 2: active should read back true");                 \
    REQUIRE(!patient_data.name.empty(),                                         \
            "Example 2: PatientData materialised an empty name array");         \
    REQUIRE(patient_data.name.front().family == "Landvater",                    \
            "Example 2: unexpected family name via PatientData");

// ── Example 3 — Re-open a .ffhr file and enrich it in place ─────────────────
// Needs a SEALED archive: the block calls stream->root_handle(), which is null
// until the stream has a root written. Assert on the re-opened file, because
// "enrich in place" is a claim about what is on disk afterwards.
#define FF_README_SETUP_EXAMPLE_3_ENRICH                                        \
    ff_readme::seal_resource("patient.ffhr", ff_readme::PATIENT_JSON);

#define FF_README_EXPECT_EXAMPLE_3_ENRICH                                       \
    REQUIRE(patient, "Example 3: root_handle is null — archive not sealed?");   \
    REQUIRE(!view.empty(), "Example 3: re-seal returned an empty view");        \
    {                                                                          \
        const std::string json = ff_readme::exported_json("patient.ffhr");      \
        REQUIRE(json.find("1990-03-21") != std::string::npos,                   \
                "Example 3: amended birthDate is not in the re-sealed file");   \
        REQUIRE(json.find("555-0199") != std::string::npos,                     \
                "Example 3: inserted telecom is not in the re-sealed file");    \
        REQUIRE(json.find("patient-1") != std::string::npos,                    \
                "Example 3: enrichment lost the original id");                  \
    }

// ── Example 5 — Surgically edit one patient in a bundle and reseal ──────────
// Two patients in, so the example can show that only the targeted one moves.
#define FF_README_SETUP_EXAMPLE_5_SURGICAL                                      \
    ff_readme::seal_resource("bundle.ffhr", ff_readme::BUNDLE_JSON);

#define FF_README_EXPECT_EXAMPLE_5_SURGICAL                                     \
    REQUIRE(!view.empty(), "Example 5: re-seal returned an empty view");        \
    {                                                                          \
        const std::string json = ff_readme::exported_json("bundle.ffhr");       \
        REQUIRE(json.find("patient-1") != std::string::npos,                    \
                "Example 5: patient-1 was lost — the edit was not surgical");    \
        REQUIRE(json.find("patient-42") != std::string::npos,                   \
                "Example 5: patient-42 missing after the surgical edit");        \
    }

// ── Example 6 — Lock-free concurrent generation ─────────────────────────────
// A `program` block: it defines serialize_bundle_parallel() and never calls it,
// which is right for documentation. The call belongs here.
#define FF_README_SETUP_EXAMPLE_6_CONCURRENT

#define FF_README_EXPECT_EXAMPLE_6_CONCURRENT                                   \
    {                                                                          \
        std::vector<ObservationData> raw(64);                                   \
        for (auto& o : raw)                                                     \
            o.status = FF_ObservationStatus::Preliminary;                        \
        const std::vector<uint8_t> sealed = serialize_bundle_parallel(raw);      \
        REQUIRE(!sealed.empty(),                                                \
                "Example 6: parallel bundle serialization produced no bytes");   \
        FastFHIR::Parser parser(sealed.data(), sealed.size());                   \
        auto root = parser.root();                                              \
        REQUIRE(root, "Example 6: sealed parallel bundle has a null root");      \
        std::size_t seen = 0;                                                    \
        for (auto& entry : root[FastFHIR::Fields::BUNDLE::ENTRY].entries())      \
        {                                                                       \
            auto res = entry[FastFHIR::Fields::BUNDLE_ENTRY::RESOURCE];          \
            if (!res)                                                           \
                continue;                                                       \
            auto node = res.as_node();                                          \
            if (node && node.is<FastFHIR::RESOURCETYPE::OBSERVATION>())          \
                ++seen;                                                         \
        }                                                                       \
        REQUIRE(seen == raw.size(),                                             \
                "Example 6: bundle held " << seen << " observations, expected "  \
                                          << raw.size());                        \
    }

#endif // FF_README_EXPECT_HPP
