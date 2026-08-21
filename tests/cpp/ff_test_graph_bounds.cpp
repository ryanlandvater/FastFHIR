/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// XP-1.3: the stored-graph traversal must reject cycles and over-deep chains,
// and accept legal DAGs with heavy sharing -- fast.
//
// Before XP-1 the compactor walked the stored arena with no depth bound and no
// visited set; a crafted .ffhr whose offset graph contains a cycle recursed
// until the stack was gone. XP-1.1 added the guards; this file pins their
// observable behavior:
//   1. a cycle        -> rejected, message names the cycle
//   2. depth > bound  -> rejected, message names the depth
//   3. a legal DAG    -> accepted, and fast (the ctest TIMEOUT is what turns
//      with heavy        the no-visited-set hang into a reported failure)
//      sharing
//
// Documents are built with the Builder (standard layout), which can express
// cycles via amend_pointer -- the same shape a crafted file would carry.

#include <FastFHIR.hpp>
#include "FF_AllTypes.hpp"
#include "FF_Reflection.hpp"   // reflected_fields_view, for the DT-1.5 tripwire
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

using namespace FastFHIR;

static int failures = 0;
static void CHECK(bool ok, const char* what) {
    printf("  %-58s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) ++failures;
}

// Wire a Patient root to a list of Identifier blocks through identifier[].
static void wire_root(FF_Stream stream, const std::vector<Offset>& ids, std::string_view id) {
    auto harr = stream->append_obj(ids, RECOVER_FF_IDENTIFIER);
    // Assign the view directly. `p.id = std::string(id)` would bind the view to
    // a temporary that dies at the semicolon, so append_obj below would copy
    // freed memory into the arena (-Wdangling-assignment-gsl). Every caller
    // passes a string literal, so the view outlives the append that reads it.
    PatientData p; p.id = id;
    auto hp = FF_StreamAppendObject(stream, p);
    stream->amend_pointer(hp.offset(), FF_PATIENT::IDENTIFIER, harr.offset());
    FF_StreamSetRoot(FF_StreamSetRootInfo{
        .stream = stream,
        .root = hp,
    });
}

int main() {
    // ── 1. Cycle -> rejected, message names the cycle ───────────────────────
    // X.type -> Y, Y.type -> X: a two-node cycle. The queue-based traversal
    // must see X again while X is still an ancestor of the node enqueuing it.
    {
        Memory mem = Memory::create(1ull << 22);
        FF_StreamCreateInfo info;
        info.arena = std::make_shared<Memory>(mem);
        FF_Stream stream;
        CHECK(FF_CreateStream(info, stream), "create stream");
        IdentifierData ix, iy;
        auto hx = FF_StreamAppendObject(stream, ix);
        auto hy = FF_StreamAppendObject(stream, iy);
        stream->amend_pointer(hx.offset(), FF_IDENTIFIER::TYPE, hy.offset());
        stream->amend_pointer(hy.offset(), FF_IDENTIFIER::TYPE, hx.offset());
        wire_root(stream, {hx.offset(), hy.offset()}, "cycle");
        Memory::View view;
        CHECK(FF_StreamFinalize(FF_StreamFinalizeInfo{
            .stream = stream,
        }, view), "finalize");
        Parser source;
        CHECK(FF_Parse(FF_ParseInfo{
            .buffer = view.data(),
            .size = view.size(),
        }, source), "parse");
        Memory::View compact_view;
        FF_Result compact_result = FF_Compact(FF_CompactInfo{
            .source = source,
        }, compact_view);
        CHECK(!compact_result, "cycle in the stored graph is rejected");
        CHECK(compact_result.message.find("cycle") != std::string::npos,
              "error message names the cycle");
    }

    // ── 2. Chain past MAX_NODE_DEPTH -> rejected, message names the depth ───
    // id0.type -> id1.type -> ... : a 70-deep chain. Depth of idK is K+2
    // (Patient 0, identifier[] 1, id0 2, ...), so id63 sits at depth 65, past
    // ArchiveContext::MAX_NODE_DEPTH (64).
    {
        Memory mem = Memory::create(1ull << 22);
        FF_StreamCreateInfo info;
        info.arena = std::make_shared<Memory>(mem);
        FF_Stream stream;
        CHECK(FF_CreateStream(info, stream), "create stream");
        constexpr int CHAIN = 70;
        std::vector<Reflective::ObjectHandle> ids;
        for (int i = 0; i < CHAIN; ++i) {
            IdentifierData d;
            ids.push_back(FF_StreamAppendObject(stream, d));
        }
        for (int i = 0; i + 1 < CHAIN; ++i) {
            stream->amend_pointer(ids[i].offset(), FF_IDENTIFIER::TYPE, ids[i + 1].offset());
        }
        wire_root(stream, {ids[0].offset()}, "deep");
        Memory::View view;
        CHECK(FF_StreamFinalize(FF_StreamFinalizeInfo{
            .stream = stream,
        }, view), "finalize");
        Parser source;
        CHECK(FF_Parse(FF_ParseInfo{
            .buffer = view.data(),
            .size = view.size(),
        }, source), "parse");
        Memory::View compact_view;
        FF_Result compact_result = FF_Compact(FF_CompactInfo{
            .source = source,
        }, compact_view);
        CHECK(!compact_result, "chain past MAX_NODE_DEPTH is rejected");
        CHECK(compact_result.message.find("depth") != std::string::npos,
              "error message names the depth");
    }

    // ── 3. Legal DAG with heavy sharing -> accepted, and fast ───────────────
    // 200 Identifiers all point their `type` slot at ONE CodeableConcept: a
    // legal shared subtree. Each node must be visited once (the done-set);
    // without it, the re-archival of the shared block 200 times is merely slow
    // here, but the same sharing shape on a deeper DAG hangs -- which is what
    // the ctest TIMEOUT catches.
    {
        Memory mem = Memory::create(1ull << 22);
        FF_StreamCreateInfo info;
        info.arena = std::make_shared<Memory>(mem);
        FF_Stream stream;
        CHECK(FF_CreateStream(info, stream), "create stream");
        CodeableConceptData cc;                       // the shared subtree
        auto hcc = FF_StreamAppendObject(stream, cc);
        constexpr int N = 200;
        std::vector<Offset> ids;
        for (int i = 0; i < N; ++i) {
            IdentifierData d;
            auto h = FF_StreamAppendObject(stream, d);
            stream->amend_pointer(h.offset(), FF_IDENTIFIER::TYPE, hcc.offset());
            ids.push_back(h.offset());
        }
        wire_root(stream, ids, "dag");
        Memory::View view;
        CHECK(FF_StreamFinalize(FF_StreamFinalizeInfo{
            .stream = stream,
        }, view), "finalize");
        Parser source;
        CHECK(FF_Parse(FF_ParseInfo{
            .buffer = view.data(),
            .size = view.size(),
        }, source), "parse");
        Memory::View compact_view;
        FF_Result compact_result = FF_Compact(FF_CompactInfo{
            .source = source,
        }, compact_view);
        CHECK(static_cast<bool>(compact_result), "legal DAG with heavy sharing is accepted");
        if (!compact_result) printf("      unexpected rejection: %s\n", compact_result.message.c_str());
    }

    // ── 4. XP-2.1: the root offset is bounds-checked before it is stored ────
    // ROOT_OFFSET is the entry point to every traversal, so an out-of-bounds
    // value there is the cheapest way to aim the reader at arbitrary memory.
    // The Parser used to store it unchecked while its docstring claimed to
    // "validate file structure" (XP-2.2).
    {
        Memory mem = Memory::create(1ull << 22);
        FF_StreamCreateInfo info;
        info.arena = std::make_shared<Memory>(mem);
        FF_Stream stream;
        CHECK(FF_CreateStream(info, stream), "create stream");
        IdentifierData id;
        auto hid = FF_StreamAppendObject(stream, id);
        wire_root(stream, {hid.offset()}, "root-bounds");
        Memory::View view;
        CHECK(FF_StreamFinalize(FF_StreamFinalizeInfo{
            .stream = stream,
        }, view), "finalize");

        // A well-formed stream still parses.
        bool ok = true;
        try { Parser good(view.data(), view.size()); } catch (const std::exception&) { ok = false; }
        CHECK(ok, "valid root offset is accepted");

        // Corrupt ROOT_OFFSET (FF_HEADER bytes 16-23) to just past the buffer.
        std::vector<BYTE> bytes(view.data(), view.data() + view.size());
        STORE_U64(bytes.data() + FF_HEADER::ROOT_OFFSET, view.size() + 4096);
        std::string msg;
        bool threw = false;
        try { Parser bad(bytes.data(), bytes.size()); }
        catch (const std::exception& e) { threw = true; msg = e.what(); }
        CHECK(threw, "out-of-bounds root offset is rejected");
        CHECK(msg.find("ROOT_OFFSET") != std::string::npos, "message names ROOT_OFFSET");

        // An offset inside the buffer but with no room for the 10-byte block
        // header is equally unusable -- `off < size` alone would admit it.
        std::vector<BYTE> tight(view.data(), view.data() + view.size());
        STORE_U64(tight.data() + FF_HEADER::ROOT_OFFSET, view.size() - 4);
        bool threw_tight = false;
        try { Parser bad2(tight.data(), tight.size()); }
        catch (const std::exception&) { threw_tight = true; }
        CHECK(threw_tight, "root with no room for a block header is rejected");
    }

    // ── 5. XP-2.3: validate_FFHR_stream() walks the graph on demand ───────────────
    // Explicit, not automatic: construction must stay cheap, so each case below
    // constructs a Parser successfully and only then asks for the walk.
    {
        Memory mem = Memory::create(1ull << 22);
        FF_StreamCreateInfo info;
        info.arena = std::make_shared<Memory>(mem);
        FF_Stream stream;
        CHECK(FF_CreateStream(info, stream), "create stream");
        IdentifierData ix, iy;
        auto hx = FF_StreamAppendObject(stream, ix);
        auto hy = FF_StreamAppendObject(stream, iy);
        CodeableConceptData cc;
        auto hcc = FF_StreamAppendObject(stream, cc);
        stream->amend_pointer(hx.offset(), FF_IDENTIFIER::TYPE, hcc.offset());
        wire_root(stream, {hx.offset(), hy.offset()}, "deep");
        Memory::View view;
        CHECK(FF_StreamFinalize(FF_StreamFinalizeInfo{
            .stream = stream,
        }, view), "finalize");

        FastFHIR::Parser good(view.data(), view.size());
        CHECK(static_cast<bool>(good.validate_FFHR_stream()), "clean stream passes validate_FFHR_stream");

        // (a) broken self-offset chain: corrupt a block's VALIDATION word.
        {
            std::vector<BYTE> bytes(view.data(), view.data() + view.size());
            STORE_U64(bytes.data() + hcc.offset() + DATA_BLOCK::VALIDATION,
                      hcc.offset() + 8);
            FastFHIR::Parser p(bytes.data(), bytes.size());
            auto r = p.validate_FFHR_stream();
            CHECK(!r, "broken self-offset is rejected");
            CHECK(r.message.find("self-offset") != std::string::npos,
                  "message names the self-offset break");
        }

        // (b) wrong recovery tag on an otherwise well-placed block.
        {
            std::vector<BYTE> bytes(view.data(), view.data() + view.size());
            STORE_U16(bytes.data() + hcc.offset() + DATA_BLOCK::RECOVERY,
                      static_cast<uint16_t>(RECOVER_FF_PATIENT));
            FastFHIR::Parser p(bytes.data(), bytes.size());
            auto r = p.validate_FFHR_stream();
            CHECK(!r, "wrong recovery tag is rejected");
            CHECK(r.message.find("recovery tag") != std::string::npos,
                  "message names the recovery tag");
        }

        // (c) out-of-bounds child offset behind a valid root.
        {
            std::vector<BYTE> bytes(view.data(), view.data() + view.size());
            STORE_U64(bytes.data() + hx.offset() + FF_IDENTIFIER::TYPE,
                      view.size() + 4096);
            FastFHIR::Parser p(bytes.data(), bytes.size());
            auto r = p.validate_FFHR_stream();
            CHECK(!r, "out-of-bounds child offset is rejected");
            CHECK(r.message.find("out of bounds") != std::string::npos,
                  "message names the bounds failure");
        }

        // (d) a cycle reachable only by walking: X.type -> X.
        {
            std::vector<BYTE> bytes(view.data(), view.data() + view.size());
            STORE_U64(bytes.data() + hx.offset() + FF_IDENTIFIER::TYPE, hx.offset());
            FastFHIR::Parser p(bytes.data(), bytes.size());
            auto r = p.validate_FFHR_stream();
            CHECK(!r, "cycle is rejected by validate_FFHR_stream");
        }
    }

    // ── 6. The schema-declared child type is what gets enforced ────────────
    // Patient.meta is FF_FIELD_BLOCK with child_recovery = RECOVER_FF_META, so
    // a non-null meta offset must point at a block whose VALIDATION word is its
    // own offset AND whose RECOVERY is RECOVER_FF_META. Nothing else is
    // accepted there, however well-formed it is in isolation.
    {
        Memory mem = Memory::create(1ull << 22);
        FF_StreamCreateInfo info;
        info.arena = std::make_shared<Memory>(mem);
        FF_Stream stream;
        CHECK(FF_CreateStream(info, stream), "create stream");
        MetaData meta;
        auto hmeta = FF_StreamAppendObject(stream, meta);
        PatientData p; p.id = "meta-case";
        auto hp = FF_StreamAppendObject(stream, p);
        stream->amend_pointer(hp.offset(), FF_PATIENT::META, hmeta.offset());
        CHECK(FF_StreamSetRoot(FF_StreamSetRootInfo{
            .stream = stream,
            .root = hp,
        }), "set root");
        Memory::View view;
        CHECK(FF_StreamFinalize(FF_StreamFinalizeInfo{
            .stream = stream,
        }, view), "finalize");

        FastFHIR::Parser good(view.data(), view.size());
        CHECK(static_cast<bool>(good.validate_FFHR_stream()),
              "Patient.meta -> Meta validates");

        // Re-tag the meta block as a Patient: structurally intact, schema-wrong.
        std::vector<BYTE> bytes(view.data(), view.data() + view.size());
        STORE_U16(bytes.data() + hmeta.offset() + DATA_BLOCK::RECOVERY,
                  static_cast<uint16_t>(RECOVER_FF_PATIENT));
        FastFHIR::Parser bad(bytes.data(), bytes.size());
        auto r = bad.validate_FFHR_stream();
        CHECK(!r, "Patient.meta pointing at a non-Meta block is rejected");
        CHECK(r.message.find("meta") != std::string::npos,
              "message names the field it was reached through");
    }

    // ── 7. The structural / deep split is real ─────────────────────────────
    // A scalar slot cannot aim the reader anywhere -- it is inline data inside
    // a V-Table already bounds-checked as a whole -- so a nonsense scalar is
    // NOT an attack and validate_FFHR_stream() deliberately ignores it.
    // validate_FFHR_stream_deep() is the pass that looks.
    {
        Memory mem = Memory::create(1ull << 22);
        FF_StreamCreateInfo info;
        info.arena = std::make_shared<Memory>(mem);
        FF_Stream stream;
        CHECK(FF_CreateStream(info, stream), "create stream");
        PatientData p; p.id = "scalar-split"; p.active = true;
        auto hp = FF_StreamAppendObject(stream, p);
        CHECK(FF_StreamSetRoot(FF_StreamSetRootInfo{
            .stream = stream,
            .root = hp,
        }), "set root");
        Memory::View view;
        CHECK(FF_StreamFinalize(FF_StreamFinalizeInfo{
            .stream = stream,
        }, view), "finalize");

        std::vector<BYTE> bytes(view.data(), view.data() + view.size());
        bytes[hp.offset() + FF_PATIENT::ACTIVE] = 7;   // not 0, 1 or the sentinel

        FastFHIR::Parser p2(bytes.data(), bytes.size());
        CHECK(static_cast<bool>(p2.validate_FFHR_stream()),
              "structural pass ignores a nonsense scalar");
        auto d = p2.validate_FFHR_stream_deep();
        CHECK(!d, "deep pass rejects a nonsense scalar");
        CHECK(d.message.find("bool") != std::string::npos,
              "deep message names the offending slot kind");
    }

    // ── 6. A FLAGGED VALUE SLOT IS AN EDGE, AND MUST BE WALKED (DT-1.5) ─────
    // FF_FIELD_CODE and FF_FIELD_DATETIME hold their value inline *most* of the
    // time and degrade to a signed relative offset when the MSB is set. The
    // structural pass skips inline scalars -- they cannot aim the reader
    // anywhere -- but a flagged slot is not inline data, and skipping it would
    // leave an attacker-controlled offset unchecked.
    //
    // These cases corrupt a CODE slot because it is the half of the mechanism
    // that is reachable today: 102 code slots are declared across the generated
    // blocks and ZERO date/time slots are, until DT-2 routes those types off
    // STRING_TYPES. Section 7 below is the tripwire for that.
    {
        Memory mem = Memory::create(1ull << 22);
        FF_StreamCreateInfo info;
        info.arena = std::make_shared<Memory>(mem);
        FF_Stream stream;
        CHECK(FF_CreateStream(info, stream), "create stream");
        IdentifierData idd;
        auto hid = FF_StreamAppendObject(stream, idd);
        auto harr = stream->append_obj(std::vector<Offset>{hid.offset()}, RECOVER_FF_IDENTIFIER);
        PatientData p; p.id = "flagged-slot";
        auto hp = FF_StreamAppendObject(stream, p);
        stream->amend_pointer(hp.offset(), FF_PATIENT::IDENTIFIER, harr.offset());
        CHECK(FF_StreamSetRoot(FF_StreamSetRootInfo{
            .stream = stream,
            .root = hp,
        }), "set root");
        Memory::View view;
        CHECK(FF_StreamFinalize(FF_StreamFinalizeInfo{
            .stream = stream,
        }, view), "finalize");

        const Offset pat = hp.offset();
        const Offset gender = pat + FF_PATIENT::GENDER;   // FF_FIELD_CODE, 4 bytes

        FastFHIR::Parser good(view.data(), view.size());
        CHECK(static_cast<bool>(good.validate_FFHR_stream()),
              "baseline stream with an unset code slot passes");

        // Pack a signed relative offset the way ENCODE_FF_CODE does.
        auto flagged = [](int64_t rel) {
            return (static_cast<uint32_t>(static_cast<int32_t>(rel)) & FF_CODE_PAYLOAD_MASK)
                   | FF_CODEABLE_CONCEPT_FLAG;
        };

        // (a) flagged offset pointing past the end of the stream.
        {
            std::vector<BYTE> bytes(view.data(), view.data() + view.size());
            const int64_t rel = static_cast<int64_t>(bytes.size() + 4096) -
                                static_cast<int64_t>(pat);
            STORE_U32(bytes.data() + gender, flagged(rel));
            FastFHIR::Parser px(bytes.data(), bytes.size());
            auto r = px.validate_FFHR_stream();
            CHECK(!r, "out-of-bounds flagged code offset is rejected");
            CHECK(r.message.find("out of bounds") != std::string::npos,
                  "message names the bounds failure");
            CHECK(r.message.find("gender") != std::string::npos,
                  "message names the offending field");
        }

        // (b) flagged offset pointing at a real block that is not a
        //     CodeableConcept. In bounds and self-consistent, so only the
        //     expected-tag check can catch it.
        {
            std::vector<BYTE> bytes(view.data(), view.data() + view.size());
            const int64_t rel = static_cast<int64_t>(hid.offset()) - static_cast<int64_t>(pat);
            STORE_U32(bytes.data() + gender, flagged(rel));
            FastFHIR::Parser px(bytes.data(), bytes.size());
            auto r = px.validate_FFHR_stream();
            CHECK(!r, "flagged code offset to a non-CodeableConcept block is rejected");
            CHECK(r.message.find("recovery tag") != std::string::npos,
                  "message names the recovery tag mismatch");
        }

        // (c) CONTROL: the same slot with the flag CLEAR is a dictionary id --
        //     inline data, no edge, nothing to walk. This is what proves the
        //     discriminator (not the slot kind) is what makes it an edge; if
        //     this failed, the two cases above would be proving nothing.
        {
            std::vector<BYTE> bytes(view.data(), view.data() + view.size());
            STORE_U32(bytes.data() + gender, 4082u);   // any plain dictionary id
            FastFHIR::Parser px(bytes.data(), bytes.size());
            CHECK(static_cast<bool>(px.validate_FFHR_stream()),
                  "an unflagged code slot is inline data and is not walked");
        }
    }

    // ── 6b. THE SAME RULE INSIDE A CHOICE ([x]) SLOT ───────────────────────
    // A choice slot is 8 raw bytes + a 2-byte tag naming the active variant.
    // For most scalar variants the raw bytes ARE the value and there is nothing
    // to follow -- but a RECOVER_FF_CODE variant carries the same MSB
    // discriminator as a dedicated code slot, so those bytes may be a fallback
    // offset instead. The validator used to skip every scalar-band variant on
    // band membership alone, which left that offset unchecked while
    // Node::print_json still dereferenced it.
    {
        Memory mem = Memory::create(1ull << 22);
        FF_StreamCreateInfo info;
        info.arena = std::make_shared<Memory>(mem);
        FF_Stream stream;
        CHECK(FF_CreateStream(info, stream), "create stream");
        ObservationData o; o.id = "choice-variant";
        auto ho = FF_StreamAppendObject(stream, o);
        CHECK(FF_StreamSetRoot(FF_StreamSetRootInfo{
            .stream = stream,
            .root = ho,
        }), "set root");
        Memory::View view;
        CHECK(FF_StreamFinalize(FF_StreamFinalizeInfo{
            .stream = stream,
        }, view), "finalize");

        const Offset obs = ho.offset();
        const Offset choice = obs + FF_OBSERVATION::EFFECTIVE;   // FF_FIELD_CHOICE

        FastFHIR::Parser good(view.data(), view.size());
        CHECK(static_cast<bool>(good.validate_FFHR_stream()),
              "baseline Observation with an unset choice slot passes");

        // (a) choice variant = code, flagged, pointing past the end.
        {
            std::vector<BYTE> bytes(view.data(), view.data() + view.size());
            const int64_t rel = static_cast<int64_t>(bytes.size() + 4096) -
                                static_cast<int64_t>(obs);
            STORE_U64(bytes.data() + choice,
                      static_cast<uint64_t>(
                          (static_cast<uint32_t>(static_cast<int32_t>(rel)) & FF_CODE_PAYLOAD_MASK)
                          | FF_CODEABLE_CONCEPT_FLAG));
            STORE_U16(bytes.data() + choice + DATA_BLOCK::RECOVERY,
                      static_cast<uint16_t>(RECOVER_FF_CODE));
            FastFHIR::Parser px(bytes.data(), bytes.size());
            auto r = px.validate_FFHR_stream();
            CHECK(!r, "flagged code variant in a choice slot is walked, not skipped");
            CHECK(r.message.find("out of bounds") != std::string::npos,
                  "choice-variant message names the bounds failure");
        }

        // (b) CONTROL: an unflagged (dictionary id) code variant is inline data
        //     and must still validate -- otherwise (a) would just be proving
        //     that the validator rejects every code variant.
        {
            std::vector<BYTE> bytes(view.data(), view.data() + view.size());
            STORE_U64(bytes.data() + choice, uint64_t{4082});
            STORE_U16(bytes.data() + choice + DATA_BLOCK::RECOVERY,
                      static_cast<uint16_t>(RECOVER_FF_CODE));
            FastFHIR::Parser px(bytes.data(), bytes.size());
            CHECK(static_cast<bool>(px.validate_FFHR_stream()),
                  "unflagged code variant in a choice slot stays inline");
        }

        // (c) CONTROL: a genuinely inert scalar variant is still skipped.
        {
            std::vector<BYTE> bytes(view.data(), view.data() + view.size());
            STORE_U64(bytes.data() + choice, uint64_t{0x4059000000000000ull});  // a double
            STORE_U16(bytes.data() + choice + DATA_BLOCK::RECOVERY,
                      static_cast<uint16_t>(RECOVER_FF_FLOAT64));
            FastFHIR::Parser px(bytes.data(), bytes.size());
            CHECK(static_cast<bool>(px.validate_FFHR_stream()),
                  "an inert scalar variant is still treated as inline");
        }
    }

    // ── 6c. A FLAGGED CODE DECODES FROM THE RIGHT ADDRESS (DT-1.7) ─────────
    // The fallback offset is relative to the CONTAINING BLOCK. Four sites spell
    // that arithmetic; Node::as<string_view>() used to resolve against the
    // node's own offset instead, which for a choice variant is the SLOT -- a
    // V-Table width away from the real block. It never showed on the ordinary
    // field path because Entry reads the code itself, without building a Node.
    //
    // This lives here rather than in test_codeable_concept.cpp because it needs
    // a parsed stream, which that file has no machinery for.
    {
        Memory mem = Memory::create(1ull << 22);
        FF_StreamCreateInfo info;
        info.arena = std::make_shared<Memory>(mem);
        FF_Stream stream;
        CHECK(FF_CreateStream(info, stream), "create stream");
        ObservationData o; o.id = "flagged-code-decode";
        auto ho = FF_StreamAppendObject(stream, o);
        CHECK(FF_StreamSetRoot(FF_StreamSetRootInfo{
            .stream = stream,
            .root = ho,
        }), "set root");
        Memory::View view;
        CHECK(FF_StreamFinalize(FF_StreamFinalizeInfo{
            .stream = stream,
        }, view), "finalize");

        const Offset obs = ho.offset();
        const char* LOCAL_CODE = "org-local-code-91827";
        CHECK(FF_GetDictionaryCode(LOCAL_CODE, FHIR_VERSION_R5) == FF_CODE_NULL,
              "fixture code must NOT be a dictionary entry, or no block is written");

        // Grow the buffer and let ENCODE_FF_CODE write a genuine
        // FF_CODEABLE_CONCEPT into the tail -- the same call the writer makes,
        // so the offset convention under test is the real one, not a guess.
        std::vector<BYTE> bytes(view.data(), view.data() + view.size());
        const Offset cc_at = bytes.size();
        bytes.resize(bytes.size() + 256, BYTE{0});
        Offset child = cc_at;
        const uint32_t packed = ENCODE_FF_CODE(bytes.data(), obs, child,
                                               std::string(LOCAL_CODE), FHIR_VERSION_R5,
                                               FF_CodeableConceptSystem::UNKNOWN);
        CHECK((packed & FF_CODEABLE_CONCEPT_FLAG) != 0, "fixture slot is flagged");

        // Put it in the choice slot as an active RECOVER_FF_CODE variant.
        const Offset choice = obs + FF_OBSERVATION::EFFECTIVE;
        STORE_U64(bytes.data() + choice, static_cast<uint64_t>(packed));
        STORE_U16(bytes.data() + choice + DATA_BLOCK::RECOVERY,
                  static_cast<uint16_t>(RECOVER_FF_CODE));

        FastFHIR::Parser px(bytes.data(), bytes.size());
        CHECK(static_cast<bool>(px.validate_FFHR_stream()),
              "a correctly-based flagged code variant validates");

        auto entry = px.root()[FastFHIR::Fields::OBSERVATION::EFFECTIVE];
        const std::string decoded(entry.as_node().as<std::string_view>());
        const std::string msg = "choice-embedded flagged code decodes (got '" + decoded + "')";
        CHECK(decoded == LOCAL_CODE, msg.c_str());

        // And the ordinary field path must agree with it -- the two readers
        // disagreeing by a V-Table width is the whole defect.
        std::ostringstream js;
        px.root().print_json(js);
        CHECK(js.str().find(LOCAL_CODE) != std::string::npos,
              "print_json renders the same code");
    }

    // ── 7. TRIPWIRE: the date/time half of DT-1.5 (see section 6) ──────────
    // validate_FFHR_stream() handles FF_FIELD_DATETIME exactly as it handles
    // FF_FIELD_CODE, but that branch is UNREACHABLE from any stream today
    // because no generated block declares such a slot -- so it has no
    // corruption test, and cannot have one yet.
    //
    // This check fails the moment that stops being true. When it does, DT-2 has
    // landed and the two corruption cases in section 6 must be duplicated for a
    // date/time slot: bit 63 set with (a) an out-of-bounds relative offset and
    // (b) an offset to a block whose tag is not RECOVER_FF_STRING. Delete this
    // tripwire in the same commit that adds them.
    {
        size_t datetime_slots = 0;
        for (uint32_t tag = 0; tag <= 0xFFFF; ++tag) {
            for (const FF_FieldInfo& f :
                 FastFHIR::reflected_fields_view(static_cast<uint16_t>(tag))) {
                if (f.kind == FF_FIELD_DATETIME) ++datetime_slots;
            }
        }
        CHECK(datetime_slots == 0,
              "TRIPWIRE: a block now declares an FF_FIELD_DATETIME slot -- write the "
              "two flagged-offset corruption cases for it (see section 7 comment)");
    }

    printf("%s\n", failures ? "FAILURES" : "all graph-bounds checks pass");
    return failures ? 1 : 0;
}
