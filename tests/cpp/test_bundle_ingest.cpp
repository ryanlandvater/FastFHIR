/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Direct guard for concurrent bundle ingest (TASKS.md A6).
//
// The ingestor asked the generated Bundle_from_json to slice Bundle.entry into
// a task vector. It never did -- the generated function only threaded its
// `concurrent_queue` parameter down to child deserializers -- so the vector came
// back empty, the count guard tripped, and EVERY bundle failed to ingest.
//
// The README example tests exercise bundles, but they also exercise surgical
// amendment and extension URLs, so they were red for unrelated reasons and did
// not isolate this. This asserts the narrow property: N entries in, N entries
// out, each one readable.

#include <FastFHIR.hpp>
#include <FF_Ingestor.hpp>

#include "FF_AllTypes.hpp"

#include <cstdio>
#include <cstring>
#include <string>

using namespace FastFHIR;

static int failures = 0;

static void CHECK(bool ok, const std::string& what)
{
    printf("  %-58s %s\n", what.c_str(), ok ? "PASS" : "FAIL");
    if (!ok) ++failures;
}

int main()
{
    static const char* kBundle = R"({
      "resourceType":"Bundle","id":"b1","type":"collection",
      "entry":[
        {"resource":{"resourceType":"Patient","id":"p1","gender":"male"}},
        {"resource":{"resourceType":"Patient","id":"p2","gender":"female"}},
        {"resource":{"resourceType":"Patient","id":"p3","gender":"other"}}
      ]})";

    auto mem = Memory::create(64ull * 1024 * 1024);
    FF_StreamCreateInfo stream_info;
    stream_info.arena = std::make_shared<Memory>(mem);
    FF_Stream stream;
    CHECK(FF_CreateStream(stream_info, stream), "create stream");
    FF_IngestorCreateInfo ingestor_info;
    FF_Ingestor ingestor;
    CHECK(FF_CreateIngestor(ingestor_info, ingestor), "create ingestor");
    Reflective::ObjectHandle root;
    Size parsed = 0;

    FF_Result result = FF_Ingest(FF_IngestInfo{
        .ingestor = ingestor,
        .stream = stream,
        .source_type = FF_SOURCE_FHIR_JSON,
        .payload = kBundle,
    }, root, parsed);

    CHECK(result.code == FF_SUCCESS,
          std::string("bundle ingests (") +
              (result.code == FF_SUCCESS ? "ok" : result.message) + ")");
    if (result.code != FF_SUCCESS) {
        printf("FAILURES\n");
        return 1;
    }

    CHECK(FF_StreamSetRoot(FF_StreamSetRootInfo{
        .stream = stream,
        .root = root,
    }), "set root");
    Memory::View view;
    CHECK(FF_StreamFinalize(FF_StreamFinalizeInfo{
        .stream = stream,
    }, view), "finalize");
    CHECK(!view.empty(), "sealed stream is non-empty");

    Parser parser;
    CHECK(FF_Parse(FF_ParseInfo{
        .buffer = view.data(),
        .size = view.size(),
    }, parser), "parse");
    auto entries = parser.root()[Fields::BUNDLE::ENTRY].entries();
    CHECK(entries.size() == 3,
          "all 3 entries present (got " + std::to_string(entries.size()) + ")");

    // Each entry's resource must be readable -- an empty or misaligned entry
    // array would still report the right count.
    const char* expect_id[] = {"p1", "p2", "p3"};
    const char* expect_gender[] = {"male", "female", "other"};
    for (size_t i = 0; i < entries.size() && i < 3; ++i) {
        auto resource = entries[i][Fields::BUNDLE_ENTRY::RESOURCE];
        std::string_view id = resource[Fields::PATIENT::ID];
        std::string_view gender = resource[Fields::PATIENT::GENDER];
        CHECK(id == expect_id[i],
              "entry " + std::to_string(i) + " id == " + expect_id[i] + " (got '" +
                  std::string(id) + "')");
        CHECK(gender == expect_gender[i],
              "entry " + std::to_string(i) + " gender == " + expect_gender[i] + " (got '" +
                  std::string(gender) + "')");
    }

    // ── Zero-copy payload path ────────────────────────────────────────────────
    // IngestRequest::payload_capacity lets a caller who already owns a padded
    // buffer skip the ingestor's defensive memcpy of the whole document. The two
    // paths must produce identical streams; if they diverge, the in-place parse is
    // reading different bytes than the copy did.
    //
    // This also pins the padding contract itself: the old code asserted
    // `size + SIMDJSON_PADDING` of readable slack on every caller buffer without
    // owning it, which was an out-of-bounds read for an unpadded payload.
    {
        simdjson::padded_string padded(kBundle, std::strlen(kBundle));

        auto mem2 = Memory::create(64ull * 1024 * 1024);
        FF_StreamCreateInfo stream_info2;
        stream_info2.arena = std::make_shared<Memory>(mem2);
        FF_Stream stream2;
        CHECK(FF_CreateStream(stream_info2, stream2), "create stream (zero-copy)");
        FF_IngestorCreateInfo ingestor_info2;
        FF_Ingestor ingestor2;
        CHECK(FF_CreateIngestor(ingestor_info2, ingestor2), "create ingestor (zero-copy)");
        Reflective::ObjectHandle root2;
        Size parsed2 = 0;

        FF_Result r2 = FF_Ingest(FF_IngestInfo{
            .ingestor = ingestor2,
            .stream = stream2,
            .source_type = FF_SOURCE_FHIR_JSON,
            .payload = std::string_view(padded.data(), padded.length()),
            .payload_capacity = padded.length() + simdjson::SIMDJSON_PADDING,
        }, root2, parsed2);
        CHECK(r2.code == FF_SUCCESS,
              std::string("zero-copy ingest succeeds (") +
                  (r2.code == FF_SUCCESS ? "ok" : r2.message) + ")");

        if (r2.code == FF_SUCCESS) {
            CHECK(FF_StreamSetRoot(FF_StreamSetRootInfo{
                .stream = stream2,
                .root = root2,
            }), "set root (zero-copy)");
            Memory::View view2;
            CHECK(FF_StreamFinalize(FF_StreamFinalizeInfo{
                .stream = stream2,
            }, view2), "finalize (zero-copy)");
            Parser parser2;
            CHECK(FF_Parse(FF_ParseInfo{
                .buffer = view2.data(),
                .size = view2.size(),
            }, parser2), "parse (zero-copy)");
            auto entries2 = parser2.root()[Fields::BUNDLE::ENTRY].entries();
            CHECK(entries2.size() == 3,
                  "zero-copy path yields 3 entries (got " +
                      std::to_string(entries2.size()) + ")");
            for (size_t i = 0; i < entries2.size() && i < 3; ++i) {
                auto resource = entries2[i][Fields::BUNDLE_ENTRY::RESOURCE];
                std::string_view id = resource[Fields::PATIENT::ID];
                std::string_view gender = resource[Fields::PATIENT::GENDER];
                CHECK(id == expect_id[i] && gender == expect_gender[i],
                      "zero-copy entry " + std::to_string(i) + " matches copy path (got '" +
                          std::string(id) + "'/'" + std::string(gender) + "')");
            }
        }
    }

    // ── Tiny-input case (TASKS.md A14 / WO-2) ─────────────────────────────────
    // ff_ingest sized its arena at 2x the input JSON: a 66-byte Patient asked
    // for 132 bytes and needed 245 (54-byte FF_HEADER + 191-byte Patient vtable)
    // before a single string byte, so every tiny input failed with "VMA Capacity
    // Exceeded". The CLI now floors the arena at FastFHIR::Ingest::FF_MIN_ARENA.
    // Pin the smallest real input through the same pipeline the CLI runs, at
    // THE SAME constant -- this used to hard-code `1ull << 20` while claiming in
    // this comment to mirror the CLI, which had moved to 2 MiB. A regression
    // test that restates a value instead of sharing it stops testing the thing
    // it names the moment that value changes.
    {
        static const char* kTiny =
            R"({"resourceType":"Patient","id":"p1","active":true,"gender":"male"})";

        auto mem3 = Memory::create(FastFHIR::Ingest::FF_MIN_ARENA);
        FF_StreamCreateInfo stream_info3;
        stream_info3.arena = std::make_shared<Memory>(mem3);
        FF_Stream stream3;
        CHECK(FF_CreateStream(stream_info3, stream3), "create stream (tiny)");
        FF_IngestorCreateInfo ingestor_info3;
        FF_Ingestor ingestor3;
        CHECK(FF_CreateIngestor(ingestor_info3, ingestor3), "create ingestor (tiny)");
        Reflective::ObjectHandle root3;
        Size parsed3 = 0;

        FF_Result r3 = FF_Ingest(FF_IngestInfo{
            .ingestor = ingestor3,
            .stream = stream3,
            .source_type = FF_SOURCE_FHIR_JSON,
            .payload = kTiny,
        }, root3, parsed3);
        CHECK(r3.code == FF_SUCCESS,
              std::string("tiny patient ingests (") +
                  (r3.code == FF_SUCCESS ? "ok" : r3.message) + ")");

        if (r3.code == FF_SUCCESS) {
            CHECK(FF_StreamSetRoot(FF_StreamSetRootInfo{
                .stream = stream3,
                .root = root3,
            }), "set root (tiny)");
            Memory::View view3;
            CHECK(FF_StreamFinalize(FF_StreamFinalizeInfo{
                .stream = stream3,
            }, view3), "finalize (tiny)");
            Parser parser3;
            CHECK(FF_Parse(FF_ParseInfo{
                .buffer = view3.data(),
                .size = view3.size(),
            }, parser3), "parse (tiny)");
            std::string_view id = parser3.root()[Fields::PATIENT::ID];
            CHECK(id == "p1", "tiny patient id == p1 (got '" + std::string(id) + "')");
        }
    }

    printf("%s\n", failures ? "FAILURES" : "bundle ingest holds");
    return failures ? 1 : 0;
}
