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

    // ── Assertion 4: deferred CODE slots resolve ────────────────────────────
    // A code outside the permanent dictionary is stored as an FF_CODEABLE_
    // CONCEPT block with FF_CODEABLE_CONCEPT_FLAG set, which is the ONLY case
    // that makes the compactor defer a 4-byte code slot (Code32). That path
    // had no coverage at all: the whole suite never once reached it, so the
    // block-copy in write_compact_code_slot and the FF_PENDING_CODE guard that
    // protects it were both untested.
    //
    // The guard matters because the placeholder for a deferred code slot must
    // not be FF_CODE_NULL -- that is the reader's "no code present", so an
    // unresolved slot would read as a cleanly dropped clinical code rather
    // than as a failure. Pre-filling with FF_PENDING_CODE (dictionary ID 0,
    // permanently reserved and never assigned) makes the residual scan in
    // Compactor::archive able to tell "absent" from "never written".
    {
        Memory cmem = Memory::create(1ull << 22);
        Builder cb(cmem);

        CodingData coding;
        coding.system = "http://example.org/local-codes";
        coding.code   = "org-local-code-91827";   // deliberately not in the dictionary

        CodeableConceptData ccc;
        ccc.coding.push_back(coding);
        auto hccc = cb.append_obj(ccc);

        IdentifierData cid;
        auto hcid = cb.append_obj(cid);
        cb.amend_pointer(hcid.offset(), FF_IDENTIFIER::TYPE, hccc.offset());

        auto carr = cb.append_obj(std::vector<Offset>{hcid.offset()}, RECOVER_FF_IDENTIFIER);
        PatientData cp; cp.id = "p3";
        auto hcp = cb.append_obj(cp);
        cb.amend_pointer(hcp.offset(), FF_PATIENT::IDENTIFIER, carr.offset());
        cb.set_root(hcp);
        auto cview = cb.finalize();

        Parser csource(cview.data(), cview.size());
        const Reflective::Entry src_code =
            csource.root()[FastFHIR::Fields::PATIENT::IDENTIFIER].as_node()
                .entries()[0][FastFHIR::Fields::IDENTIFIER::TYPE].as_node()
                [FastFHIR::Fields::CODEABLECONCEPT::CODING].as_node()
                .entries()[0][FastFHIR::Fields::CODING::CODE];
        CHECK((bool)src_code, "source code slot is present");
        const uint32_t src_slot = LOAD_U32(csource.data() + src_code.absolute_offset());
        CHECK((src_slot & FF_CODEABLE_CONCEPT_FLAG) != 0,
              "source code slot is CodeableConcept-flagged (deferred path reached)");

        Memory cdst = Memory::create(1ull << 22);
        auto compact_cview = Compactor::archive(csource, cdst);
        CHECK(!compact_cview.empty(), "archive resolves the deferred code slot");

        Parser ccompact(compact_cview.data(), compact_cview.size());
        const Reflective::Entry out_code =
            ccompact.root()[FastFHIR::Fields::PATIENT::IDENTIFIER].as_node()
                .entries()[0][FastFHIR::Fields::IDENTIFIER::TYPE].as_node()
                [FastFHIR::Fields::CODEABLECONCEPT::CODING].as_node()
                .entries()[0][FastFHIR::Fields::CODING::CODE];
        CHECK((bool)out_code, "compact code slot is present");

        // The decisive assertion: neither sentinel survived. FF_PENDING_CODE
        // would mean the deferred write never landed; FF_CODE_NULL would mean
        // it landed as "absent" -- the silent-loss shape the sentinel exists
        // to make impossible.
        const uint32_t out_slot = LOAD_U32(ccompact.data() + out_code.absolute_offset());
        CHECK(out_slot != FF_PENDING_CODE, "no residual FF_PENDING_CODE in the sealed stream");
        CHECK(out_slot != FF_CODE_NULL, "deferred code did not degrade to 'absent'");
        CHECK(static_cast<std::string_view>(out_code) == "org-local-code-91827",
              "out-of-dictionary code round-trips through the archive");
    }

    printf("%s\n", failures ? "FAILURES" : "all compactor graph checks pass");
    return failures ? 1 : 0;
}
