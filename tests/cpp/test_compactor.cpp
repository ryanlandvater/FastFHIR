/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// XP-1.1 regression: archiving a document with a shared subtree must visit
// each node exactly once.
//
// Before the fix, Compactor::archive walked the stored graph with no depth
// bound and no visited set, so a subtree referenced from two parents was
// archived twice (once per reference) and a cyclic offset graph looped
// forever. The observable contract after the fix: a shared child block is
// archived exactly once, and every reference to it resolves to that single
// copy. Node identity (Node::offset()) is internal -- Compactor-friend only --
// so the test asserts the contract through public bytes: the two parent slots
// name the same block, and the shared document archives strictly smaller than
// an otherwise-identical document that duplicates the subtree.

#include <FastFHIR.hpp>
#include <FF_Ops.hpp>  // LOAD_U64 for the raw slot reads
#include "FF_AllTypes.hpp"
#include <cstdio>
#include <string>

using namespace FastFHIR;

static int failures = 0;
static void CHECK(bool ok, const char* what) {
    printf("  %-58s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) ++failures;
}

int main() {
    // ── Shared-subtree document ─────────────────────────────────────────────
    // Patient -> identifier[] -> { X, Y }, and both X and Y point their
    // `type` slot at the SAME CodeableConcept block C.
    auto mem = Memory::create(1ull << 22);
    Builder b(mem);

    CodeableConceptData cc;                       // the shared subtree
    auto hcc = b.append_obj(cc);

    IdentifierData ix, iy;                        // the two parents
    auto hx = b.append_obj(ix);
    auto hy = b.append_obj(iy);
    b.amend_pointer(hx.offset(), FF_IDENTIFIER::TYPE, hcc.offset());
    b.amend_pointer(hy.offset(), FF_IDENTIFIER::TYPE, hcc.offset());

    auto harr = b.append_obj(std::vector<Offset>{hx.offset(), hy.offset()},
                             RECOVER_FF_IDENTIFIER);
    PatientData p; p.id = "p1";
    auto hp = b.append_obj(p);
    b.amend_pointer(hp.offset(), FF_PATIENT::IDENTIFIER, harr.offset());
    b.set_root(hp);

    auto view = b.finalize();
    CHECK(!view.empty(), "builder finalize produced a stream");

    Parser source(view.data(), view.size());
    CHECK((bool)source.root(), "source stream parses");

    // ── Archive ─────────────────────────────────────────────────────────────
    Memory dst = Memory::create(1ull << 22);
    auto compact_view = Compactor::archive(source, dst);
    CHECK(!compact_view.empty(), "archive produced a compact stream");

    Parser compact(compact_view.data(), compact_view.size());
    auto root = compact.root();
    CHECK((bool)root, "compact stream parses");

    // ── Assertion 1: the shared subtree was archived once (slot bytes) ──────
    // Compact object slots hold the absolute target block offset, so the raw
    // slot values are directly comparable: both parents must name the SAME
    // block. A per-reference archive would have written two copies and the
    // slots would name different blocks.
    const Reflective::Node arr_node =
        root[FastFHIR::Fields::PATIENT::IDENTIFIER].as_node();
    const auto entries = arr_node.entries();
    CHECK(entries.size() == 2, "identifier array keeps both entries");
    const Reflective::Entry x_type = entries[0][FastFHIR::Fields::IDENTIFIER::TYPE];
    const Reflective::Entry y_type = entries[1][FastFHIR::Fields::IDENTIFIER::TYPE];
    CHECK((bool)x_type && (bool)y_type, "both type slots are present");
    const uint64_t x_slot = LOAD_U64(compact.data() + x_type.absolute_offset());
    const uint64_t y_slot = LOAD_U64(compact.data() + y_type.absolute_offset());
    CHECK(x_slot != FF_NULL_OFFSET, "X.type slot points at a real block");
    CHECK(x_slot == y_slot, "shared subtree archived once (both slots name one block)");

    // ── Assertion 2: each node visited exactly once (output-size counter) ───
    // Twin document: identical structure but NO sharing (X->C1, Y->C2). With
    // the visited set the shared doc archives 5 blocks and the twin 6, so the
    // shared output is strictly smaller. Without it both archive 6 and the
    // sizes are equal.
    {
        Memory mem2 = Memory::create(1ull << 22);
        Builder b2(mem2);
        CodeableConceptData cc1, cc2;
        auto hcc1 = b2.append_obj(cc1);
        auto hcc2 = b2.append_obj(cc2);
        IdentifierData tx, ty;
        auto htx = b2.append_obj(tx);
        auto hty = b2.append_obj(ty);
        b2.amend_pointer(htx.offset(), FF_IDENTIFIER::TYPE, hcc1.offset());
        b2.amend_pointer(hty.offset(), FF_IDENTIFIER::TYPE, hcc2.offset());
        auto harr2 = b2.append_obj(std::vector<Offset>{htx.offset(), hty.offset()},
                                   RECOVER_FF_IDENTIFIER);
        PatientData p2; p2.id = "p2";
        auto hp2 = b2.append_obj(p2);
        b2.amend_pointer(hp2.offset(), FF_PATIENT::IDENTIFIER, harr2.offset());
        b2.set_root(hp2);
        auto view2 = b2.finalize();
        Parser source2(view2.data(), view2.size());
        Memory dst2 = Memory::create(1ull << 22);
        auto twin_view = Compactor::archive(source2, dst2);
        CHECK(compact_view.size() < twin_view.size(),
              "shared subtree archived once (output smaller than unshared twin)");
    }

    // ── Assertion 3: payload survived the archive ───────────────────────────
    std::string_view id = root[FastFHIR::Fields::PATIENT::ID];
    CHECK(id == "p1", "patient id round-trips through the archive");

    printf("%s\n", failures ? "FAILURES" : "all compactor graph checks pass");
    return failures ? 1 : 0;
}
