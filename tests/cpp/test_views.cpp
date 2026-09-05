/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The generated lazy VIEW layer: the third way to read a stream, alongside the
// Node lens and eager PatientData materialization. Callers reach for it to pull
// a whole block out at once; it is slower than the lens and it is generated, so
// nothing here is hand-maintained.
//
// IT HAD NO TESTS, and that is how it came to hold 1,645 fabricated extents.
// The view struct was declared {base, offset} -- no stream size -- so every
// accessor that needed one wrote `0`, which reads as "the buffer is zero bytes"
// and switched off the only bounds check on a wire value. The deserializer path
// threads __size correctly and always did; the lens carries m_size; only the
// view could not say how big the buffer was, so only the view was unsafe. A
// single test constructing a view and reading a string through it would have
// caught it the day it was written.
//
// So this asserts both halves of the contract:
//   1. a view reads the same values the lens does (it is not a second truth);
//   2. a view bounds-checks, because it now knows the extent -- damage reads
//      back as empty rather than as a wild pointer or an invented megabyte.

#include <FastFHIR.hpp>
#include <FF_Ingestor.hpp>
#include "FF_AllTypes.hpp"

#include <openssl/evp.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "FFHR_test_checksum.hpp"

using namespace FastFHIR;

static int g_tests = 0, g_failures = 0;
static void CHECK(bool ok, const std::string &what)
{
    ++g_tests;
    std::printf("  %-62s %s\n", what.c_str(), ok ? "PASS" : "FAIL");
    if (!ok)
        ++g_failures;
}


// A one-Patient bundle, sealed, so the view reads real writer output rather
// than a hand-built buffer (COV-1: a synthetic fixture proves the reader agrees
// with the test author, not with the writer).
static std::shared_ptr<Memory> build_patient(Memory::View &view_out)
{
    auto arena = std::make_shared<Memory>(Memory::create(16 * 1024 * 1024));
    FF_StreamCreateInfo stream_info;
    stream_info.arena = arena;
    stream_info.version = FHIR_VERSION_R5;
    FF_Stream stream;
    if (!FF_CreateStream(stream_info, stream))
        return nullptr;

    FF_IngestorCreateInfo ingestor_info;
    FF_Ingestor ingestor;
    if (!FF_CreateIngestor(ingestor_info, ingestor))
        return nullptr;

    const std::string json = R"({
        "resourceType": "Patient",
        "id": "view-patient-1",
        "gender": "male",
        "active": true
    })";

    Reflective::ObjectHandle root;
    Size count = 0;
    auto res = FF_Ingest(FF_IngestInfo{.ingestor = ingestor,
                                       .stream = stream,
                                       .source_type = FF_SOURCE_FHIR_JSON,
                                       .extension_filter = FF_ExtensionFilterMode::FILTER_NONE,
                                       .payload = json},
                         root, count);
    if (res.failed())
        return nullptr;
    if (!FF_StreamSetRoot(FF_StreamSetRootInfo{.stream = stream, .root = root}))
        return nullptr;
    if (!FF_StreamFinalize(
            FF_StreamFinalizeInfo{
                .stream = stream, .algorithm = FF_CHECKSUM_SHA256, .hasher = ff_test::sha256},
            view_out))
        return nullptr;
    return arena;
}

// 1. The view agrees with the lens, and carries the extent it needs to check.
static void test_view_reads_agree_with_lens()
{
    Memory::View view;
    auto arena = build_patient(view);
    CHECK(arena != nullptr && !view.empty(), "fixture built");
    if (!arena || view.empty())
        return;

    const BYTE *const base = reinterpret_cast<const BYTE *>(view.data());
    Parser parser(view.data(), view.size());
    const auto root = parser.root();

    // The view is constructed the way a caller would: base, the block's offset,
    // and THE STREAM EXTENT. That third argument is the whole point -- it did
    // not exist, and every accessor invented it as 0.
    PATIENTView<FHIR_VERSION_R5> v{base, FF_HEADER(view.size()).get_root(base), view.size()};

    const std::string_view lens_id = root[Fields::PATIENT::ID];
    CHECK(v.get_id() == lens_id, "view id matches the lens");
    CHECK(!v.get_id().empty(), "view id is non-empty");
}

// 2. The extent is actually USED: a view pointed at a block that cannot fit
//    reads back empty instead of dereferencing a wild offset.
static void test_view_bounds_check_is_live()
{
    Memory::View view;
    auto arena = build_patient(view);
    CHECK(arena != nullptr && !view.empty(), "fixture built");
    if (!arena || view.empty())
        return;

    const BYTE *const base = reinterpret_cast<const BYTE *>(view.data());
    Parser parser(view.data(), view.size());

    // Same block, but the view is told the stream is only as long as its own
    // header. Every string it reaches for now lies outside the declared extent,
    // so the bound must refuse them. Before the extent existed this returned
    // whatever bytes happened to follow.
    PATIENTView<FHIR_VERSION_R5> truncated{base, FF_HEADER(view.size()).get_root(base),
                                           static_cast<Size>(DATA_BLOCK::HEADER_SIZE)};
    CHECK(truncated.get_id().empty(), "a string outside the declared extent reads empty");

    // And the honest control: with the real extent the same read succeeds, so
    // the check above is measuring the bound and not a broken accessor.
    PATIENTView<FHIR_VERSION_R5> whole{base, FF_HEADER(view.size()).get_root(base), view.size()};
    CHECK(!whole.get_id().empty(), "the same read succeeds with the true extent");
}

int main()
{
    std::printf("\nViews\n");
    test_view_reads_agree_with_lens();
    test_view_bounds_check_is_live();
    std::printf("\n%d test(s), %d failure(s)\n", g_tests, g_failures);
    if (g_tests == 0)
    {
        std::fprintf(stderr, "  FAIL no tests ran -- a pass on zero coverage is not a pass\n");
        return 1;
    }
    return g_failures == 0 ? 0 : 1;
}
