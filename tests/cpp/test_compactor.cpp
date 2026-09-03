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
    FF_StreamCreateInfo stream_info;
    stream_info.arena = std::make_shared<Memory>(mem);
    FF_Stream stream;
    CHECK(FF_CreateStream(stream_info, stream), "create stream");

    CodeableConceptData cc;                       // the shared subtree
    auto hcc = FF_StreamAppendObject(stream, cc);

    IdentifierData ix, iy;                        // the two parents
    auto hx = FF_StreamAppendObject(stream, ix);
    auto hy = FF_StreamAppendObject(stream, iy);
    stream->amend_pointer(hx.offset(), FF_IDENTIFIER::TYPE, hcc.offset());
    stream->amend_pointer(hy.offset(), FF_IDENTIFIER::TYPE, hcc.offset());

    auto harr = stream->append_obj(std::vector<Offset>{hx.offset(), hy.offset()},
                                   RECOVER_FF_IDENTIFIER);
    PatientData p; p.id = "p1";
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
    CHECK(!view.empty(), "builder finalize produced a stream");

    Parser source;
    CHECK(FF_Parse(FF_ParseInfo{
        .buffer = view.data(),
        .size = view.size(),
    }, source), "parse source");
    CHECK((bool)source.root(), "source stream parses");

    // ── Archive ─────────────────────────────────────────────────────────────
    Memory::View compact_view;
    CHECK(FF_Compact(FF_CompactInfo{
        .source = source,
    }, compact_view), "compact");
    CHECK(!compact_view.empty(), "archive produced a compact stream");

    Parser compact;
    CHECK(FF_Parse(FF_ParseInfo{
        .buffer = compact_view.data(),
        .size = compact_view.size(),
    }, compact), "parse compact");
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
        FF_StreamCreateInfo stream_info2;
        stream_info2.arena = std::make_shared<Memory>(mem2);
        FF_Stream stream2;
        CHECK(FF_CreateStream(stream_info2, stream2), "create stream (twin)");
        CodeableConceptData cc1, cc2;
        auto hcc1 = FF_StreamAppendObject(stream2, cc1);
        auto hcc2 = FF_StreamAppendObject(stream2, cc2);
        IdentifierData tx, ty;
        auto htx = FF_StreamAppendObject(stream2, tx);
        auto hty = FF_StreamAppendObject(stream2, ty);
        stream2->amend_pointer(htx.offset(), FF_IDENTIFIER::TYPE, hcc1.offset());
        stream2->amend_pointer(hty.offset(), FF_IDENTIFIER::TYPE, hcc2.offset());
        auto harr2 = stream2->append_obj(std::vector<Offset>{htx.offset(), hty.offset()},
                                         RECOVER_FF_IDENTIFIER);
        PatientData p2; p2.id = "p2";
        auto hp2 = FF_StreamAppendObject(stream2, p2);
        stream2->amend_pointer(hp2.offset(), FF_PATIENT::IDENTIFIER, harr2.offset());
        CHECK(FF_StreamSetRoot(FF_StreamSetRootInfo{
            .stream = stream2,
            .root = hp2,
        }), "set root (twin)");
        Memory::View view2;
        CHECK(FF_StreamFinalize(FF_StreamFinalizeInfo{
            .stream = stream2,
        }, view2), "finalize (twin)");
        Parser source2;
        CHECK(FF_Parse(FF_ParseInfo{
            .buffer = view2.data(),
            .size = view2.size(),
        }, source2), "parse source (twin)");
        Memory::View twin_view;
        CHECK(FF_Compact(FF_CompactInfo{
            .source = source2,
        }, twin_view), "compact (twin)");
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
        FF_StreamCreateInfo stream_info3;
        stream_info3.arena = std::make_shared<Memory>(cmem);
        FF_Stream stream3;
        CHECK(FF_CreateStream(stream_info3, stream3), "create stream (deferred)");

        CodingData coding;
        coding.system = "http://example.org/local-codes";
        coding.code   = "org-local-code-91827";   // deliberately not in the dictionary

        CodeableConceptData ccc;
        // std::move: the datatype structs are move-only now that a block-typed
        // choice carries its DECODED value (ChoiceBlock wraps structs holding
        // unique_ptr members), so a copy is no longer available to take.
        ccc.coding.push_back(std::move(coding));
        auto hccc = FF_StreamAppendObject(stream3, ccc);

        IdentifierData cid;
        auto hcid = FF_StreamAppendObject(stream3, cid);
        stream3->amend_pointer(hcid.offset(), FF_IDENTIFIER::TYPE, hccc.offset());

        auto carr = stream3->append_obj(std::vector<Offset>{hcid.offset()}, RECOVER_FF_IDENTIFIER);
        PatientData cp; cp.id = "p3";
        auto hcp = FF_StreamAppendObject(stream3, cp);
        stream3->amend_pointer(hcp.offset(), FF_PATIENT::IDENTIFIER, carr.offset());
        CHECK(FF_StreamSetRoot(FF_StreamSetRootInfo{
            .stream = stream3,
            .root = hcp,
        }), "set root (deferred)");
        Memory::View cview;
        CHECK(FF_StreamFinalize(FF_StreamFinalizeInfo{
            .stream = stream3,
        }, cview), "finalize (deferred)");

        Parser csource;
        CHECK(FF_Parse(FF_ParseInfo{
            .buffer = cview.data(),
            .size = cview.size(),
        }, csource), "parse source (deferred)");
        const Reflective::Entry src_code =
            csource.root()[FastFHIR::Fields::PATIENT::IDENTIFIER].as_node()
                .entries()[0][FastFHIR::Fields::IDENTIFIER::TYPE].as_node()
                [FastFHIR::Fields::CODEABLECONCEPT::CODING].as_node()
                .entries()[0][FastFHIR::Fields::CODING::CODE];
        CHECK((bool)src_code, "source code slot is present");
        const uint32_t src_slot = LOAD_U32(csource.data() + src_code.absolute_offset());
        CHECK((src_slot & FF_CODEABLE_CONCEPT_FLAG) != 0,
              "source code slot is CodeableConcept-flagged (deferred path reached)");

        Memory::View compact_cview;
        CHECK(FF_Compact(FF_CompactInfo{
            .source = csource,
        }, compact_cview), "compact (deferred)");
        CHECK(!compact_cview.empty(), "archive resolves the deferred code slot");

        Parser ccompact;
        CHECK(FF_Parse(FF_ParseInfo{
            .buffer = compact_cview.data(),
            .size = compact_cview.size(),
        }, ccompact), "parse compact (deferred)");
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
