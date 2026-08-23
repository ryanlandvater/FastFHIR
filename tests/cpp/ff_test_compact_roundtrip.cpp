/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// COV-1.5: a document compacted by this project's own COMPACTOR must present
// the same document its own READER presented before compaction.
//
// Why this file exists (2026-08-23). `test_compactor.cpp` hand-builds a
// four-block Patient and asserts on raw slot bytes -- it proves the visited-set
// contract (XP-1.1) and nothing about whether a real document survives the
// transform. Nothing anywhere fed writer output to the compactor. That is the
// same hole COV-1.1 closed for the validator, where `ff_test_graph_bounds`
// passed the entire time `validate_FFHR_stream()` was rejecting all 342 Synthea
// bundles, because it too only ever saw byte patterns a test author wrote.
//
// The immediate prompt was OPQ-1. Retaining an out-of-profile resource as an
// opaque-JSON block gave `archive_string` a second tag to carry, and it was
// carrying a hardcoded RECOVER_FF_STRING -- so the compact copy would have
// re-tagged the blob a plain string and exported the resource quoted and
// escaped. That was corrected by inspection, with no test that could have
// caught it. This is that test.
//
// The assertion is deliberately blunt: compact `print_json` must be
// BYTE-IDENTICAL to standard `print_json`. Both walk `reflected_fields_view` in
// the same order over the same values, so any difference at all is a compaction
// defect, and a byte compare needs no JSON differ in C++ and cannot be argued
// with. When it fails it prints the first differing offset with context, which
// localises the field immediately.
//
// NOT asserted here: `validate_FFHR_stream()` over the compact stream. The
// validator walks V-Tables at the SCHEMA's field offsets, and the compact layout
// is dense -- those offsets do not apply, so running it would test the wrong
// thing and fail for the wrong reason. A compaction-aware validator is a
// separate piece of work (TASKS.md COV-1.4); this test does not pretend to
// cover it.

#include <FastFHIR.hpp>
#include <FF_Ingestor.hpp>
#include "FF_AllTypes.hpp"
#include "FF_FieldKeys.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
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

// The first `limit` bundles in NAME order.
//
// `directory_iterator` yields in filesystem order, which is neither sorted nor
// stable across machines -- so truncating it to `limit` picks an arbitrary
// subset, and a failure here would not reproduce from the same command on
// another box. Same reason ff_test_datetime pins its seed and prints it: a suite
// whose input varies run to run is a suite whose red gets ignored. Sort first,
// then truncate.
static std::vector<fs::path> find_bundles(std::size_t limit) {
    std::vector<fs::path> out;
    const fs::path root(FASTFHIR_SYNTHEA_DIR);
    if (root.empty()) return out;
    for (const auto& dir : {root / "fhir", root}) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) continue;
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (entry.path().extension() == ".json") out.push_back(entry.path());
        }
        if (!out.empty()) break;
    }
    std::sort(out.begin(), out.end());
    if (out.size() > limit) out.resize(limit);
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

// How many Bundle.entry resources are retained opaque-JSON blocks?
//
// This is the coverage counter, not a correctness check. A test that exercises
// the opaque path only by accident is a test that stops exercising it the day
// someone widens the build profile -- silently, while still passing. main()
// requires the corpus total to be non-zero and PRINTS it, so the coverage is
// visible in the log rather than assumed.
static std::size_t count_opaque_resources(const Reflective::Node& root) {
    const Reflective::Node entries = root[Fields::BUNDLE::ENTRY].as_node();
    if (!entries) return 0;

    std::size_t opaque = 0;
    for (const Reflective::Node& entry : entries.entries()) {
        const Reflective::Entry slot = entry[Fields::BUNDLE_ENTRY::RESOURCE];
        if (!slot) continue;
        if (slot.as_node().recovery() == RECOVER_FF_OPAQUE_JSON) ++opaque;
    }
    return opaque;
}

// The first byte at which two documents diverge, with surrounding context --
// "they differ" localises nothing across a 50 MB export.
static void report_first_difference(const std::string& lhs, const std::string& rhs) {
    const std::size_t n = std::min(lhs.size(), rhs.size());
    std::size_t i = 0;
    while (i < n && lhs[i] == rhs[i]) ++i;

    printf("    diverges at byte %zu (standard %zu bytes, compact %zu bytes)\n",
           i, lhs.size(), rhs.size());
    const std::size_t from = i > 60 ? i - 60 : 0;
    printf("      standard: ...%s\n", lhs.substr(from, 140).c_str());
    printf("      compact : ...%s\n", rhs.substr(from, 140).c_str());
}

struct FixtureResult {
    bool        ok      = false;
    std::size_t opaque  = 0;
};

static FixtureResult compact_roundtrip(const fs::path& fixture) {
    FixtureResult result;

    std::ifstream f(fixture, std::ios::binary | std::ios::ate);
    if (!f) { printf("    cannot open %s\n", fixture.string().c_str()); return result; }
    const auto size = f.tellg();
    f.seekg(0);
    std::string json(static_cast<std::size_t>(size), '\0');
    f.read(json.data(), size);

    auto mem = Memory::create(2ull * 1024 * 1024 * 1024);
    FF_StreamCreateInfo stream_info;
    stream_info.arena = std::make_shared<Memory>(mem);
    stream_info.version = FHIR_VERSION_R5;
    FF_Stream stream;
    if (!FF_CreateStream(stream_info, stream)) return result;

    FF_IngestorCreateInfo ingestor_info;
    FF_Ingestor ingestor;
    if (!FF_CreateIngestor(ingestor_info, ingestor)) return result;

    Reflective::ObjectHandle root_handle;
    Size resource_count = 0;
    const auto ingest = FF_Ingest(FF_IngestInfo{
        .ingestor         = ingestor,
        .stream           = stream,
        .source_type      = FF_SOURCE_FHIR_JSON,
        .extension_filter = FF_ExtensionFilterMode::FILTER_NONE,
        .payload          = json,
    }, root_handle, resource_count);
    if (ingest.failed()) {
        printf("    ingest failed: %s\n", ingest.message.c_str());
        return result;
    }

    if (!FF_StreamSetRoot(FF_StreamSetRootInfo{.stream = stream, .root = root_handle}))
        return result;

    Memory::View view;
    if (!FF_StreamFinalize(FF_StreamFinalizeInfo{
            .stream = stream, .algorithm = FF_CHECKSUM_SHA256, .hasher = sha256}, view))
        return result;
    if (view.empty()) return result;

    Parser source;
    if (!FF_Parse(FF_ParseInfo{.buffer = view.data(), .size = view.size()}, source))
        return result;
    if (!source.root()) { printf("    source root is null\n"); return result; }

    std::ostringstream source_json;
    source.print_json(source_json);
    result.opaque = count_opaque_resources(source.root());

    Memory::View compact_view;
    const auto compacted = FF_Compact(FF_CompactInfo{.source = source}, compact_view);
    if (compacted.failed()) {
        printf("    compact failed: %s\n", compacted.message.c_str());
        return result;
    }
    if (compact_view.empty()) { printf("    compact produced no stream\n"); return result; }

    Parser compact;
    if (!FF_Parse(FF_ParseInfo{
            .buffer = compact_view.data(), .size = compact_view.size()}, compact))
        return result;
    if (!compact.root()) { printf("    compact root is null\n"); return result; }

    std::ostringstream compact_json;
    compact.print_json(compact_json);

    const std::string lhs = source_json.str();
    const std::string rhs = compact_json.str();
    if (lhs != rhs) {
        report_first_difference(lhs, rhs);
        return result;
    }

    // The blob must still be a blob on the other side. Equal documents already
    // imply it -- a re-tagged opaque block would have exported quoted, and the
    // byte compare above would have caught it -- but asserting the count
    // directly names WHICH property broke instead of leaving it to be read out
    // of a diff offset.
    if (count_opaque_resources(compact.root()) != result.opaque) {
        printf("    opaque-resource count changed across compaction (%zu -> %zu)\n",
               result.opaque, count_opaque_resources(compact.root()));
        return result;
    }

    result.ok = true;
    return result;
}

int main() {
    // 8, not 4: the whole run is under a second, and a wider sample is the
    // difference between exercising 4 opaque-JSON resources and 11.
    const auto bundles = find_bundles(8);
    if (bundles.empty()) {
        // Not a failure: CI without Synthea data is a supported configuration.
        // Say so loudly rather than reporting a pass on zero coverage.
        printf("SKIP: no Synthea fixtures (FASTFHIR_SYNTHEA_DIR unset or empty)\n");
        return 0;
    }

    printf("Compacting %zu writer-produced streams\n", bundles.size());
    std::size_t opaque_total = 0;
    for (const auto& b : bundles) {
        const FixtureResult r = compact_roundtrip(b);
        opaque_total += r.opaque;
        const std::string name = b.filename().string().substr(0, 40);
        CHECK(r.ok, name.c_str());
    }

    printf("  opaque-JSON resources exercised: %zu\n", opaque_total);
    // Vacuous coverage is the failure this repo keeps paying for. If the build
    // profile ever grows to cover every type in the corpus, this test would
    // still pass while testing nothing about the fallback -- so say so instead.
    // The fix is NOT to widen the profile: CMakePresets.json omits the `imaging`
    // grouping deliberately (TASKS.md OPQ-1).
    CHECK(opaque_total > 0, "corpus exercised the opaque-JSON path at all");

    printf("%s\n", failures ? "FAILURES" : "compaction preserves the document");
    return failures ? 1 : 0;
}
