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
    Builder builder(mem);
    Ingest::Ingestor ingestor;
    Reflective::ObjectHandle root;
    size_t parsed = 0;

    Ingest::IngestRequest request{builder, Ingest::SourceType::FHIR_JSON, kBundle};
    const FF_Result result = ingestor.ingest(request, root, parsed);

    CHECK(result.code == FF_SUCCESS,
          std::string("bundle ingests (") +
              (result.code == FF_SUCCESS ? "ok" : result.message) + ")");
    if (result.code != FF_SUCCESS) {
        printf("FAILURES\n");
        return 1;
    }

    builder.set_root(root);
    auto view = builder.finalize();
    CHECK(!view.empty(), "sealed stream is non-empty");

    Parser parser(view.data(), view.size());
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
        Builder builder2(mem2);
        Ingest::Ingestor ingestor2;
        Reflective::ObjectHandle root2;
        size_t parsed2 = 0;

        Ingest::IngestRequest zc{
            builder2,
            Ingest::SourceType::FHIR_JSON,
            std::string_view(padded.data(), padded.length()),
            padded.length() + simdjson::SIMDJSON_PADDING};

        const FF_Result r2 = ingestor2.ingest(zc, root2, parsed2);
        CHECK(r2.code == FF_SUCCESS,
              std::string("zero-copy ingest succeeds (") +
                  (r2.code == FF_SUCCESS ? "ok" : r2.message) + ")");

        if (r2.code == FF_SUCCESS) {
            builder2.set_root(root2);
            auto view2 = builder2.finalize();
            Parser parser2(view2.data(), view2.size());
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

    printf("%s\n", failures ? "FAILURES" : "bundle ingest holds");
    return failures ? 1 : 0;
}
