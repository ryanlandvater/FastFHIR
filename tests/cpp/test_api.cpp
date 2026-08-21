/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// FF_* external API surface contract tests.
//
// WO-1 (TASKS.md): every FF_* function with an out-parameter must clear it
// BEFORE its argument checks, so a caller reusing a handle never keeps a
// stale object when the call fails validation. FF_CreateIngestor was the
// precedent; this suite pins the other seven.

#include <FastFHIR.hpp>
#include "FF_AllTypes.hpp"

#include <cstdio>
#include <string>

using namespace FastFHIR;

static int failures = 0;
static void CHECK(bool ok, const char* what) {
    printf("  %-64s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) ++failures;
}

int main() {
    // ── Shared fixture: one sealed stream + parsed view, for the Parser and
    //    Memory::View out-params that need a real object to be "populated".
    FF_StreamCreateInfo stream_info;
    FF_Stream stream;
    CHECK(FF_CreateStream(stream_info, stream), "create fixture stream");

    PatientData p; p.id = "p1";
    auto root = FF_StreamAppendObject(stream, p);
    CHECK(FF_StreamSetRoot(FF_StreamSetRootInfo{
        .stream = stream,
        .root = root,
    }), "set fixture root");

    Memory::View sealed_view;
    CHECK(FF_StreamFinalize(FF_StreamFinalizeInfo{
        .stream = stream,
    }, sealed_view), "finalize fixture stream");
    CHECK(!sealed_view.empty(), "fixture stream sealed non-empty");

    Parser sealed_parser;
    CHECK(FF_Parse(FF_ParseInfo{
        .buffer = sealed_view.data(),
        .size = sealed_view.size(),
    }, sealed_parser), "parse fixture stream");
    CHECK(static_cast<bool>(sealed_parser), "fixture parser is valid");

    // ── 1. FF_CreateMemory — shm_name + filepath mutually exclusive ─────────
    {
        FF_MemoryCreateInfo ok;
        FF_Memory memory;
        CHECK(FF_CreateMemory(ok, memory) && memory, "pre-populate memory handle");

        FF_MemoryCreateInfo bad;
        bad.shm_name = "shm";
        bad.filepath = "/tmp/fastfhir-would-never-exist";
        FF_Result r = FF_CreateMemory(bad, memory);
        CHECK(r.failed(), "FF_CreateMemory rejects shm_name + filepath");
        CHECK(!memory, "FF_CreateMemory cleared out_memory on invalid args");
    }

    // ── 2. FF_CreateStream — arena + filepath mutually exclusive ────────────
    {
        FF_StreamCreateInfo ok;
        FF_Stream s;
        CHECK(FF_CreateStream(ok, s) && s, "pre-populate stream handle");

        FF_StreamCreateInfo bad;
        bad.arena = std::make_shared<Memory>(Memory::create(1ull << 20));
        bad.filepath = "/tmp/fastfhir-would-never-exist";
        FF_Result r = FF_CreateStream(bad, s);
        CHECK(r.failed(), "FF_CreateStream rejects arena + filepath");
        CHECK(!s, "FF_CreateStream cleared out_stream on invalid args");
    }

    // ── 3. FF_StreamFinalize — null stream ──────────────────────────────────
    {
        Memory::View view = sealed_view;  // populated
        CHECK(!view.empty(), "pre-populate out_view");
        FF_Result r = FF_StreamFinalize(FF_StreamFinalizeInfo{}, view);
        CHECK(r.failed(), "FF_StreamFinalize rejects null stream");
        CHECK(view.data() == nullptr && view.size() == 0,
              "FF_StreamFinalize cleared out_view on invalid args");
    }

    // ── 4. FF_StreamQuery — null stream ─────────────────────────────────────
    {
        Parser parser = sealed_parser;  // populated
        CHECK(static_cast<bool>(parser), "pre-populate out_parser");
        FF_Result r = FF_StreamQuery(FF_StreamQueryInfo{}, parser);
        CHECK(r.failed(), "FF_StreamQuery rejects null stream");
        CHECK(!parser, "FF_StreamQuery cleared out_parser on invalid args");
    }

    // ── 5. FF_Parse — null buffer with nonzero size ─────────────────────────
    {
        Parser parser = sealed_parser;  // populated
        CHECK(static_cast<bool>(parser), "pre-populate out_parser (parse)");
        FF_Result r = FF_Parse(FF_ParseInfo{nullptr, 16}, parser);
        CHECK(r.failed(), "FF_Parse rejects null buffer");
        CHECK(!parser, "FF_Parse cleared out_parser on invalid args");
    }

    // ── 6. FF_Compact — invalid source parser ───────────────────────────────
    {
        Memory::View view = sealed_view;  // populated
        CHECK(!view.empty(), "pre-populate out_view (compact)");
        FF_Result r = FF_Compact(FF_CompactInfo{}, view);
        CHECK(r.failed(), "FF_Compact rejects invalid source");
        CHECK(view.data() == nullptr && view.size() == 0,
              "FF_Compact cleared out_view on invalid args");
    }

    // ── 7. FF_Ingest — null ingestor / null destination stream ──────────────
    {
        // Pre-populate out_root via a real ingest.
        FF_StreamCreateInfo stream_info2;
        FF_Stream stream2;
        CHECK(FF_CreateStream(stream_info2, stream2), "create ingest stream");
        FF_IngestorCreateInfo ingestor_info;
        FF_Ingestor ingestor;
        CHECK(FF_CreateIngestor(ingestor_info, ingestor), "create ingestor");

        constexpr std::string_view kPatient =
            R"({"resourceType":"Patient","id":"p1"})";
        Reflective::ObjectHandle out_root;
        Size out_count = 0;
        FF_Result ok = FF_Ingest(FF_IngestInfo{
            .ingestor = ingestor,
            .stream = stream2,
            .source_type = FF_SOURCE_FHIR_JSON,
            .payload = kPatient,
        }, out_root, out_count);
        CHECK(ok && out_root, "pre-populate out_root + out_count via ingest");

        out_count = 42;  // deliberately stale
        FF_Result r = FF_Ingest(FF_IngestInfo{}, out_root, out_count);
        CHECK(r.failed(), "FF_Ingest rejects null ingestor");
        CHECK(!out_root && out_count == 0,
              "FF_Ingest cleared out_root + out_count on invalid args");
    }

    printf("%s\n", failures ? "FAILURES" : "all FF_* out-param contracts hold");
    return failures ? 1 : 0;
}
