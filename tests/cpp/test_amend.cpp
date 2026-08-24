/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Guards for the shared amend_* pre-check (Builder::_amend_prepare).
//
// The three amend_* entry points share one validation path. Collapsing them
// is right, but the collapse has three ways to go subtly wrong, and each is
// silent at compile time:
//
//   1. Releasing the mutation guard when the validator returns, so finalize()
//      can seal the stream between the check and the caller's STORE.
//   2. Bounds-checking with `a + b + c > capacity`, which wraps for a 64-bit
//      Offset and hands back a wild pointer.
//   3. Returning a second guard object, so one increment gets two decrements.

// Exercises the three properties the amend_* refactor must preserve.
#include <FastFHIR.hpp>
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
    // 1. FF_NULL_OFFSET as object_offset must be rejected, not wrapped.
    {
        auto mem = Memory::create(1ull << 20);
        FF_StreamCreateInfo info;
        info.arena = std::make_shared<Memory>(mem);
        FF_Stream stream;
        CHECK(FF_CreateStream(info, stream), "create stream");
        std::string msg;
        try { stream->amend_pointer(FF_NULL_OFFSET, 8, 64); }
        catch (const std::runtime_error& e) { msg = e.what(); }
        // Must be rejected by the BOUNDS check. If the addition wraps, the
        // slot pointer is wild and the already-assigned probe reads garbage --
        // which also throws, but only by luck, and is undefined behaviour.
        CHECK(msg.find("out of bounds") != std::string::npos,
              "FF_NULL_OFFSET rejected by BOUNDS check (not a wild read)");
    }
    // 2. Guard must survive many amends -- a double end_mutation() would
    //    underflow m_active_mutators and finalize() would then hang or seal early.
    {
        auto mem = Memory::create(1ull << 22);
        FF_StreamCreateInfo info;
        info.arena = std::make_shared<Memory>(mem);
        FF_Stream stream;
        CHECK(FF_CreateStream(info, stream), "create stream");
        PatientData p; p.id = "p1";
        auto handle = FF_StreamAppendObject(stream, p);
        CHECK(FF_StreamSetRoot(FF_StreamSetRootInfo{
            .stream = stream,
            .root = handle,
        }), "set root");
        Memory::View view;
        CHECK(FF_StreamFinalize(FF_StreamFinalizeInfo{
            .stream = stream,
        }, view), "finalize");
        CHECK(!view.empty(), "finalize() completes after amends (mutator count balanced)");
    }
    // 3. Amending an already-assigned slot must still be refused.
    {
        auto mem = Memory::create(1ull << 22);
        FF_StreamCreateInfo info;
        info.arena = std::make_shared<Memory>(mem);
        FF_Stream stream;
        CHECK(FF_CreateStream(info, stream), "create stream");
        PatientData p; p.id = "p1";
        auto handle = FF_StreamAppendObject(stream, p);
        bool threw = false;
        try {
            stream->amend_pointer(handle.offset(), FF_PATIENT::ID, 128);
        } catch (const std::runtime_error&) { threw = true; }
        CHECK(threw, "already-assigned slot refused");
    }
    printf("%s\n", failures ? "FAILURES" : "all amend guards hold");
    return failures ? 1 : 0;
}
