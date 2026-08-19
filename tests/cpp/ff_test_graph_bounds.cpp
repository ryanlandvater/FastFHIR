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

    printf("%s\n", failures ? "FAILURES" : "all graph-bounds checks pass");
    return failures ? 1 : 0;
}
