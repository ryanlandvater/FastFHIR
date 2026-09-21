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
#include <cstring>
#include <set>
#include <string>

#include "FFHR_tests.hpp"

using namespace FastFHIR;


int main() {
    // ── Shared fixture: one sealed stream + parsed view, for the Parser and
    //    Memory::View out-params that need a real object to be "populated".
    FF_BuilderCreateInfo builder_info;
    FF_Builder builder;
    CHECK(FF_CreateBuilder(builder_info, builder), "create fixture stream");

    PatientData p; p.id = "p1";
    auto root = FF_BuilderAppendObject(builder, p);
    CHECK(FF_BuilderSetRoot(FF_BuilderSetRootInfo{
        .builder = builder,
        .root = root,
    }), "set fixture root");

    Memory::View sealed_view;
    CHECK(FF_BuilderFinalize(FF_BuilderFinalizeInfo{
        .builder = builder,
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

    // ── 2. FF_CreateBuilder — arena + filepath mutually exclusive ────────────
    {
        FF_BuilderCreateInfo ok;
        FF_Builder s;
        CHECK(FF_CreateBuilder(ok, s) && s, "pre-populate stream handle");

        FF_BuilderCreateInfo bad;
        bad.arena = Memory::create(1ull << 20);
        bad.filepath = "/tmp/fastfhir-would-never-exist";
        FF_Result r = FF_CreateBuilder(bad, s);
        CHECK(r.failed(), "FF_CreateBuilder rejects arena + filepath");
        CHECK(!s, "FF_CreateBuilder cleared out_stream on invalid args");
    }

    // ── 3. FF_BuilderFinalize — null stream ──────────────────────────────────
    {
        Memory::View view = sealed_view;  // populated
        CHECK(!view.empty(), "pre-populate out_view");
        FF_Result r = FF_BuilderFinalize(FF_BuilderFinalizeInfo{}, view);
        CHECK(r.failed(), "FF_BuilderFinalize rejects null stream");
        CHECK(view.data() == nullptr && view.size() == 0,
              "FF_BuilderFinalize cleared out_view on invalid args");
    }

    // ── 4. FF_BuilderQuery — null stream ─────────────────────────────────────
    {
        Parser parser = sealed_parser;  // populated
        CHECK(static_cast<bool>(parser), "pre-populate out_parser");
        FF_Result r = FF_BuilderQuery(FF_BuilderQueryInfo{}, parser);
        CHECK(r.failed(), "FF_BuilderQuery rejects null stream");
        CHECK(!parser, "FF_BuilderQuery cleared out_parser on invalid args");
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

    // ── 7. FF_Ingest — null ingestor / null destination builder─────────────
    {
        // Pre-populate out_root via a real ingest.
        FF_BuilderCreateInfo builder_info2;
        FF_Builder builder2;
        CHECK(FF_CreateBuilder(builder_info2, builder2), "create ingest stream");
        FF_IngestorCreateInfo ingestor_info;
        FF_Ingestor ingestor;
        CHECK(FF_CreateIngestor(ingestor_info, ingestor), "create ingestor");

        constexpr std::string_view kPatient =
            R"({"resourceType":"Patient","id":"p1"})";
        Reflective::ObjectHandle out_root;
        Size out_count = 0;
        FF_Result ok = FF_Ingest(FF_IngestInfo{
            .ingestor = ingestor,
            .builder = builder2,
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

    // ── API-1.2 — Entry::concrete_recovery() ───────────────────────────────
    //
    // The type of a resource slot is on the wire TWICE: in the 10-byte tuple
    // {offset(8), tag(2)} beside the offset, and again in the target block's
    // own header. concrete_recovery() reads the FIRST copy without following
    // the offset; this asserts it agrees with the SECOND.
    //
    // ⚠ THE OBVIOUS TEST IS VACUOUS. `slot.as_node().recovery()` reads the tuple
    // tag too -- standard_entry_as_node's FF_FIELD_RESOURCE branch builds the
    // Node with `FF_GET_RECOVERY_TAG(base, slot_offset)`, the very bytes
    // concrete_recovery() returns. Comparing those two compares a value with
    // itself and passes even if both are wrong. Ground truth is the TARGET
    // BLOCK'S HEADER, read from the sealed buffer at the node's own offset.
    {
        FF_BuilderCreateInfo builder_info;
        FF_Builder builder;
        CHECK(FF_CreateBuilder(builder_info, builder), "create bundle stream");
        FF_IngestorCreateInfo ingestor_info;
        FF_Ingestor ingestor;
        CHECK(FF_CreateIngestor(ingestor_info, ingestor), "create bundle ingestor");

        // Deliberately mixed, and deliberately including ImagingStudy: the
        // shipped profile omits the `imaging` grouping, so that entry is
        // retained as an opaque-JSON blob and its tuple tag is
        // RECOVER_FF_OPAQUE_JSON rather than a resource tag. A slot whose tag
        // varies by CONTENT is the whole reason this accessor exists.
        constexpr std::string_view kMixed = R"({
            "resourceType":"Bundle","id":"mixed","type":"collection",
            "entry":[
                {"resource":{"resourceType":"Patient","id":"p1"}},
                {"resource":{"resourceType":"Observation","id":"o1","status":"final"}},
                {"resource":{"resourceType":"Encounter","id":"e1"}},
                {"resource":{"resourceType":"Condition","id":"c1"}},
                {"resource":{"resourceType":"ImagingStudy","id":"i1","status":"available"}}
            ]})";

        Reflective::ObjectHandle root_handle;
        Size ingested = 0;
        CHECK(FF_Ingest(FF_IngestInfo{
            .ingestor = ingestor,
            .builder = builder,
            .source_type = FF_SOURCE_FHIR_JSON,
            .payload = kMixed,
        }, root_handle, ingested), "ingest the mixed bundle");
        CHECK(FF_BuilderSetRoot(FF_BuilderSetRootInfo{
            .builder = builder, .root = root_handle}), "set mixed bundle root");

        Memory::View sealed;
        CHECK(FF_BuilderFinalize(FF_BuilderFinalizeInfo{.builder = builder}, sealed),
              "finalize mixed bundle");

        Parser parser;
        CHECK(FF_Parse(FF_ParseInfo{
            .buffer = sealed.data(), .size = sealed.size()}, parser),
            "parse mixed bundle");

        const BYTE* const bytes = reinterpret_cast<const BYTE*>(sealed.data());
        const auto entries = parser.root()[Fields::BUNDLE::ENTRY].entries();
        CHECK(entries.size() == 5, "five bundle entries");

        std::set<RECOVERY_TAG> seen;
        size_t compared = 0;
        for (const Reflective::Node& entry : entries) {
            const Reflective::Entry slot = entry[Fields::BUNDLE_ENTRY::RESOURCE];

            // 1. The slot's own copy -- no dereference.
            const RECOVERY_TAG from_slot = slot.concrete_recovery();
            CHECK(from_slot != FF_RECOVER_UNDEFINED,
                  "a populated resource slot answers a concrete tag");
            // It must never be the polymorphic base -- that is the value
            // target_recovery already carries, and the reason this exists.
            CHECK(from_slot != RECOVER_FF_RESOURCE,
                  "concrete_recovery() is the CONCRETE type, not RECOVER_FF_RESOURCE");
            CHECK(slot.target_recovery == RECOVER_FF_RESOURCE,
                  "target_recovery still carries the static base (unchanged by API-1)");

            // 2. The independent second copy, read straight from the bytes.
            //    as_node() is deliberately NOT used to locate the target. The
            //    Node it returns was built from the SAME tuple tag, so it is
            //    not a second witness at all. (Node::offset() is protected BY
            //    DESIGN -- a consumer is not handed raw arena offsets -- so
            //    this is not a gap to file; a test asserting against bytes
            //    reads the bytes.) The tuple's first 8 bytes are the
            //    target's absolute offset; the tag in the block header there is
            //    the independent copy. Reading it is the whole point of the
            //    redundancy (B7: assert against bytes, not against another
            //    description of them).
            uint64_t target_offset = 0;
            std::memcpy(&target_offset, bytes + slot.absolute_offset(), sizeof(target_offset));
            CHECK(target_offset != FF_NULL_OFFSET && target_offset < sealed.size(),
                  "the tuple's offset half is in bounds");
            const RECOVERY_TAG from_header = FF_GET_RECOVERY_TAG(bytes, target_offset);
            CHECK(from_slot == from_header,
                  "slot tag " << from_slot << " == target header tag " << from_header);

            seen.insert(from_slot);
            ++compared;
        }
        // P0-2: an empty walk would satisfy every assertion above.
        CHECK(compared == 5, "compared all five entries");
        CHECK(seen.size() >= 4, "the bundle really is mixed: " << seen.size()
                                << " distinct concrete tags");
        CHECK(seen.count(RECOVER_FF_OPAQUE_JSON) == 1,
              "the out-of-profile ImagingStudy reports the opaque-JSON tag");

        // 3. Non-tuple slots answer FF_RECOVER_UNDEFINED rather than
        //    reinterpreting whatever 2 bytes happen to sit at slot+8.
        const Reflective::Node first = entries[0];
        CHECK(first[Fields::BUNDLE_ENTRY::FULL_URL].concrete_recovery() == FF_RECOVER_UNDEFINED,
              "a STRING slot is not a tuple");
        CHECK(first[Fields::BUNDLE_ENTRY::REQUEST].concrete_recovery() == FF_RECOVER_UNDEFINED,
              "a BLOCK slot is not a tuple (8 bytes, no tag half)");
        CHECK(parser.root()[Fields::BUNDLE::TYPE].concrete_recovery() == FF_RECOVER_UNDEFINED,
              "a CODE slot is not a tuple");
        CHECK(parser.root()[Fields::BUNDLE::ENTRY].concrete_recovery() == FF_RECOVER_UNDEFINED,
              "an ARRAY slot is not a tuple");
    }

    return ff_test::report("all FF_* out-param contracts hold");
}
