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

#include <algorithm>
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

// The first `limit` Synthea bundles in NAME order. One fixture is not
// representative -- the validator defect needed a bundle carrying a populated
// scalar array (Claim) or a resource array.
//
// Sorted, because `directory_iterator` yields in filesystem order: truncating
// that to `limit` picked an arbitrary subset that differed between machines, so
// a red here would not reproduce from the same command elsewhere. Same rule as
// ff_test_datetime's pinned seed.
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

struct WalkStats {
    std::size_t nodes       = 0;   // proves the walk actually descended
    std::size_t arrays      = 0;   // proves the array readers were reached
    std::size_t void_arrays = 0;   // N>0 entries on the wire, all reading back empty
};

// Descend the whole stored document through the public Node API.
//
// Deliberately the reader's own view, not a byte walk: the point is to make the
// READER disagree with itself where it can. `size()` comes from the array
// header, `entries()` from the element decoder -- when the second loses what the
// first counted, that is the AR-1 defect class, and it is invisible to the
// validator because the bytes are perfectly well-formed.
//
// Depth-bounded for the same reason DeepValidator is: this runs on real files,
// but a bound costs nothing and a runaway recursion in a test is a hang, not a
// failure.
static void walk_document(const Reflective::Node& node, WalkStats& stats, int depth) {
    static constexpr int MAX_DEPTH = 64;
    if (!node || depth > MAX_DEPTH) return;
    ++stats.nodes;

    if (node.is_array()) {
        ++stats.arrays;
        const auto items = node.entries();
        if (!items.empty()) {
            bool any_readable = false;
            for (const auto& item : items) {
                if (!item.is_empty()) { any_readable = true; break; }
            }
            if (!any_readable) ++stats.void_arrays;
        }
        for (const auto& item : items) walk_document(item, stats, depth + 1);
        return;
    }

    if (!node.is_object()) return;
    for (const auto& f : node.fields()) {
        const FF_FieldKey key = FF_FieldKey::from_cstr(
            node.recovery(), f.kind, f.field_offset,
            f.child_recovery, f.array_entries_are_offsets, f.name);
        const Reflective::Entry entry = node[key];
        if (!entry) continue;
        // Inline scalars have no child node to descend into; they are counted
        // by their parent's presence and rendered from the Entry.
        if (ff_kind_is_inline_scalar(f.kind)) { ++stats.nodes; continue; }
        walk_document(entry.as_node(), stats, depth + 1);
    }
}

// One fixture: ingest -> seal -> validate -> re-parse -> WALK. Returns false on
// any failure, with the validator's own message printed -- that message is the
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
    //
    // This block used to be the two lines below and nothing else -- it checked
    // that `root` was non-null and returned, while the comment above claimed it
    // walked the document. The regression it names could therefore have sailed
    // straight through it. Now it actually walks.
    auto root = parser.root();
    if (!root) { printf("    re-parsed root is null\n"); return false; }

    WalkStats stats;
    walk_document(root, stats, 0);

    // A bundle that reaches here has thousands of nodes; single digits mean the
    // walk stopped at the root and the assertions below are vacuous.
    if (stats.nodes < 100) {
        printf("    walk visited only %zu nodes — the document did not traverse\n", stats.nodes);
        return false;
    }
    // THE assertion, and the one AR-1 would have failed: an array whose header
    // says it holds N>0 entries, every one of which reads back empty. That is
    // exactly how `"diagnosisSequence": [1]` exported as `[]` -- structurally
    // valid, silently void. Comparing counts to the source JSON is py_roundtrip's
    // job; this only needs the stream to disagree with itself.
    if (stats.void_arrays > 0) {
        printf("    %zu array(s) hold entries on the wire but read back entirely empty\n",
               stats.void_arrays);
        return false;
    }
    if (stats.arrays == 0) {
        printf("    no arrays reached — this fixture cannot cover the array readers\n");
        return false;
    }
    // Printed, not just asserted: "it passed" and "it looked at anything" are
    // different claims, and this suite has already spent a session being the
    // second while reading as the first.
    printf("      walked %zu nodes, %zu arrays\n", stats.nodes, stats.arrays);
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
