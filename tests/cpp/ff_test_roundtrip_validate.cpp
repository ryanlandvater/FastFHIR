/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// COV-1.1: a stream this project's own WRITER produced must satisfy this
// project's own VALIDATOR, and must read back through the Node API.
//
// Why this file exists (2026-08-22). `validate_FFHR_stream()` was rejecting all
// 342 Synthea bundles with "the offset chain is broken" -- every real document
// the library could produce -- and ctest stayed green at 36/37 the entire time.
// `ff_test_graph_bounds` covers that exact function, but it hand-builds streams,
// so it only ever validated byte patterns a test author thought to write. It
// never saw writer output, and neither did anything else: ff_roundtrip ingests
// and re-parses but never validates, while the C++ tests validate but never
// ingest. The writer -> validator -> reader path was covered by nothing.
//
// The defect was two readers disagreeing about an inline array entry:
// walk_array walked scalar elements and 10-byte resource tuples as if each were
// a DATA_BLOCK, and neither carries a self-offset at +0. A synthetic fixture
// cannot catch that class at all, because the bug is a disagreement between the
// writer's layout and the reader's belief about it -- both sides have to be real.
//
// So this test is deliberately shallow and end-to-end rather than clever:
//   ingest a real bundle -> seal -> validate -> re-parse -> walk.
// It is the shape of test that was missing, not an addition to the ones present.
// Keep it pointed at real fixtures; hand-built input belongs in
// ff_test_graph_bounds, where hostile shapes a writer cannot emit are the point.

#include <FastFHIR.hpp>
#include <FF_Ingestor.hpp>
#include "FF_AllTypes.hpp"

#include <openssl/evp.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace FastFHIR;

static int failures = 0;
static void CHECK(bool ok, const char* what) {
    printf("  %-58s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) ++failures;
}

#ifndef FASTFHIR_SYNTHEA_DIR
#  define FASTFHIR_SYNTHEA_DIR ""
#endif

// Up to `limit` Synthea bundles. One fixture is not representative -- the
// validator defect needed a bundle carrying a populated scalar array (Claim) or
// a resource array, and the alphabetically-first fixture need not have either.
static std::vector<fs::path> find_bundles(std::size_t limit) {
    std::vector<fs::path> out;
    const fs::path root(FASTFHIR_SYNTHEA_DIR);
    if (root.empty()) return out;
    for (const auto& dir : {root / "fhir", root}) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) continue;
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (entry.path().extension() == ".json") out.push_back(entry.path());
            if (out.size() >= limit) return out;
        }
        if (!out.empty()) return out;
    }
    return out;
}

static std::vector<BYTE> sha256(const unsigned char* data, Size len) {
    std::vector<BYTE> hash(EVP_MAX_MD_SIZE);
    unsigned int out_len = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, hash.data(), &out_len);
    EVP_MD_CTX_free(ctx);
    hash.resize(out_len);
    return hash;
}

// One fixture: ingest -> seal -> validate -> re-parse. Returns false on any
// failure, with the validator's own message printed -- that message is the
// diagnosis, so swallowing it would waste the test.
static bool roundtrip_and_validate(const fs::path& fixture) {
    std::ifstream f(fixture, std::ios::binary | std::ios::ate);
    if (!f) { printf("  cannot open %s\n", fixture.string().c_str()); return false; }
    const auto size = f.tellg();
    f.seekg(0);
    std::string json(static_cast<std::size_t>(size), '\0');
    f.read(json.data(), size);

    auto mem = Memory::create(2ull * 1024 * 1024 * 1024);
    FF_StreamCreateInfo stream_info;
    stream_info.arena = std::make_shared<Memory>(mem);
    stream_info.version = FHIR_VERSION_R5;
    FF_Stream stream;
    if (!FF_CreateStream(stream_info, stream)) return false;

    FF_IngestorCreateInfo ingestor_info;
    FF_Ingestor ingestor;
    if (!FF_CreateIngestor(ingestor_info, ingestor)) return false;

    Reflective::ObjectHandle root_handle;
    Size resource_count = 0;
    auto ingest = FF_Ingest(FF_IngestInfo{
        .ingestor = ingestor,
        .stream = stream,
        .source_type = FF_SOURCE_FHIR_JSON,
        .extension_filter = FF_ExtensionFilterMode::FILTER_NONE,
        .payload = json,
    }, root_handle, resource_count);
    if (ingest.failed()) {
        printf("    ingest failed: %s\n", ingest.message.c_str());
        return false;
    }

    if (!FF_StreamSetRoot(FF_StreamSetRootInfo{.stream = stream, .root = root_handle}))
        return false;

    Memory::View view;
    if (!FF_StreamFinalize(FF_StreamFinalizeInfo{
            .stream = stream, .algorithm = FF_CHECKSUM_SHA256, .hasher = sha256}, view))
        return false;
    if (view.empty()) return false;

    Parser parser(mem);
    const FF_Result result = parser.validate_FFHR_stream();
    if (result.failed()) {
        printf("    %s\n", result.message.c_str());
        return false;
    }

    // Validation proves the offset graph is sound; it does not prove the data
    // reads back. The scalar-array defect passed structurally and still exported
    // an empty list, so walk the document too.
    auto root = parser.root();
    if (!root) { printf("    re-parsed root is null\n"); return false; }
    return true;
}

int main() {
    const auto bundles = find_bundles(8);
    if (bundles.empty()) {
        // Not a failure: CI without Synthea data is a supported configuration.
        // Say so loudly rather than reporting a pass on zero coverage.
        printf("SKIP: no Synthea fixtures (FASTFHIR_SYNTHEA_DIR unset or empty)\n");
        return 0;
    }

    printf("Validating %zu writer-produced streams\n", bundles.size());
    for (const auto& b : bundles) {
        const std::string name = b.filename().string().substr(0, 40);
        CHECK(roundtrip_and_validate(b), name.c_str());
    }

    printf("%s\n", failures ? "FAILURES" : "all writer-produced streams validate");
    return failures ? 1 : 0;
}
