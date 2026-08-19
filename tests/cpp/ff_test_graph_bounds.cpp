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
#include <cstdio>
#include <string>
#include <vector>

using namespace FastFHIR;

static int failures = 0;
static void CHECK(bool ok, const char* what) {
    printf("  %-58s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) ++failures;
}

// Wire a Patient root to a list of Identifier blocks through identifier[].
static void wire_root(Builder& b, const std::vector<Offset>& ids, std::string_view id) {
    auto harr = b.append_obj(ids, RECOVER_FF_IDENTIFIER);
    // Assign the view directly. `p.id = std::string(id)` would bind the view to
    // a temporary that dies at the semicolon, so append_obj below would copy
    // freed memory into the arena (-Wdangling-assignment-gsl). Every caller
    // passes a string literal, so the view outlives the append that reads it.
    PatientData p; p.id = id;
    auto hp = b.append_obj(p);
    b.amend_pointer(hp.offset(), FF_PATIENT::IDENTIFIER, harr.offset());
    b.set_root(hp);
}

int main() {
    // ── 1. Cycle -> rejected, message names the cycle ───────────────────────
    // X.type -> Y, Y.type -> X: a two-node cycle. The queue-based traversal
    // must see X again while X is still an ancestor of the node enqueuing it.
    {
        Memory mem = Memory::create(1ull << 22);
        Builder b(mem);
        IdentifierData ix, iy;
        auto hx = b.append_obj(ix);
        auto hy = b.append_obj(iy);
        b.amend_pointer(hx.offset(), FF_IDENTIFIER::TYPE, hy.offset());
        b.amend_pointer(hy.offset(), FF_IDENTIFIER::TYPE, hx.offset());
        wire_root(b, {hx.offset(), hy.offset()}, "cycle");
        auto view = b.finalize();
        Parser source(view.data(), view.size());
        Memory dst = Memory::create(1ull << 22);
        std::string msg;
        bool threw = false;
        try { Compactor::archive(source, dst); }
        catch (const std::runtime_error& e) { threw = true; msg = e.what(); }
        CHECK(threw, "cycle in the stored graph is rejected");
        CHECK(msg.find("cycle") != std::string::npos, "error message names the cycle");
    }

    // ── 2. Chain past MAX_NODE_DEPTH -> rejected, message names the depth ───
    // id0.type -> id1.type -> ... : a 70-deep chain. Depth of idK is K+2
    // (Patient 0, identifier[] 1, id0 2, ...), so id63 sits at depth 65, past
    // ArchiveContext::MAX_NODE_DEPTH (64).
    {
        Memory mem = Memory::create(1ull << 22);
        Builder b(mem);
        constexpr int CHAIN = 70;
        std::vector<Reflective::ObjectHandle> ids;
        for (int i = 0; i < CHAIN; ++i) {
            IdentifierData d;
            ids.push_back(b.append_obj(d));
        }
        for (int i = 0; i + 1 < CHAIN; ++i) {
            b.amend_pointer(ids[i].offset(), FF_IDENTIFIER::TYPE, ids[i + 1].offset());
        }
        wire_root(b, {ids[0].offset()}, "deep");
        auto view = b.finalize();
        Parser source(view.data(), view.size());
        Memory dst = Memory::create(1ull << 22);
        std::string msg;
        bool threw = false;
        try { Compactor::archive(source, dst); }
        catch (const std::runtime_error& e) { threw = true; msg = e.what(); }
        CHECK(threw, "chain past MAX_NODE_DEPTH is rejected");
        CHECK(msg.find("depth") != std::string::npos, "error message names the depth");
    }

    // ── 3. Legal DAG with heavy sharing -> accepted, and fast ───────────────
    // 200 Identifiers all point their `type` slot at ONE CodeableConcept: a
    // legal shared subtree. Each node must be visited once (the done-set);
    // without it, the re-archival of the shared block 200 times is merely slow
    // here, but the same sharing shape on a deeper DAG hangs -- which is what
    // the ctest TIMEOUT catches.
    {
        Memory mem = Memory::create(1ull << 22);
        Builder b(mem);
        CodeableConceptData cc;                       // the shared subtree
        auto hcc = b.append_obj(cc);
        constexpr int N = 200;
        std::vector<Offset> ids;
        for (int i = 0; i < N; ++i) {
            IdentifierData d;
            auto h = b.append_obj(d);
            b.amend_pointer(h.offset(), FF_IDENTIFIER::TYPE, hcc.offset());
            ids.push_back(h.offset());
        }
        wire_root(b, ids, "dag");
        auto view = b.finalize();
        Parser source(view.data(), view.size());
        Memory dst = Memory::create(1ull << 22);
        bool ok = true;
        std::string msg;
        try { Compactor::archive(source, dst); }
        catch (const std::runtime_error& e) { ok = false; msg = e.what(); }
        CHECK(ok, "legal DAG with heavy sharing is accepted");
        if (!ok) printf("      unexpected rejection: %s\n", msg.c_str());
    }

    // ── 4. XP-2.1: the root offset is bounds-checked before it is stored ────
    // ROOT_OFFSET is the entry point to every traversal, so an out-of-bounds
    // value there is the cheapest way to aim the reader at arbitrary memory.
    // The Parser used to store it unchecked while its docstring claimed to
    // "validate file structure" (XP-2.2).
    {
        Memory mem = Memory::create(1ull << 22);
        Builder b(mem);
        IdentifierData id;
        auto hid = b.append_obj(id);
        wire_root(b, {hid.offset()}, "root-bounds");
        auto view = b.finalize();

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
        Builder b(mem);
        IdentifierData ix, iy;
        auto hx = b.append_obj(ix);
        auto hy = b.append_obj(iy);
        CodeableConceptData cc;
        auto hcc = b.append_obj(cc);
        b.amend_pointer(hx.offset(), FF_IDENTIFIER::TYPE, hcc.offset());
        wire_root(b, {hx.offset(), hy.offset()}, "deep");
        auto view = b.finalize();

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
        Builder b(mem);
        MetaData meta;
        auto hmeta = b.append_obj(meta);
        PatientData p; p.id = "meta-case";
        auto hp = b.append_obj(p);
        b.amend_pointer(hp.offset(), FF_PATIENT::META, hmeta.offset());
        b.set_root(hp);
        auto view = b.finalize();

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
        Builder b(mem);
        PatientData p; p.id = "scalar-split"; p.active = true;
        auto hp = b.append_obj(p);
        b.set_root(hp);
        auto view = b.finalize();

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

    printf("%s\n", failures ? "FAILURES" : "all graph-bounds checks pass");
    return failures ? 1 : 0;
}
