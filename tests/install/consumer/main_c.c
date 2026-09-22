/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * @file main_c.c
 * @brief The installed package's C ABI, compiled by a C compiler.
 *
 * The C++ consumer beside this file cannot catch a broken C header: it is
 * compiled as C++, where the whole point of FastFHIR.h -- that it is valid C --
 * is never tested. So this is a .c translation unit that sees ONLY the install
 * prefix. It builds a stream, seals it, reads it back and destroys every
 * handle, which is enough to prove the header parses as C, the FF_* symbols
 * export from the shared library, and FF_Export.h came along with them.
 *
 * The exhaustive C-surface test is tests/cpp/test_c_api.c, in-tree. This one
 * exists to fail when a header is missing from the package.
 */

#include <FastFHIR.h>

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

static void check(int ok, const char *what)
{
    if (ok) return;
    fprintf(stderr, "install consumer (C): FAILED: %s\n", what);
    ++g_failures;
}

static void check_result(FF_ResultInfo r, const char *what)
{
    check(FF_ResultSucceeded(&r), what);
    if (FF_ResultFailed(&r)) fprintf(stderr, "  %s\n", r.message);
}

int main(void)
{
    FF_BuilderCreateInfo binfo;
    memset(&binfo, 0, sizeof(binfo));
    binfo.capacity     = 1u << 20;
    binfo.fhir_version = FF_FHIR_R5;

    FF_BuilderHandle builder = NULL;
    check_result(FF_CreateBuilder(&binfo, &builder), "FF_CreateBuilder");
    if (!builder) return 1;

    static const char *const json = "{\"resourceType\":\"Patient\",\"id\":\"installed\"}";
    FF_ObjectHandle root = NULL;
    check_result(FF_BuilderAppendOpaqueJson(
                     &(FF_BuilderAppendOpaqueJsonInfo){.builder = builder, .json = json,
                                                       .length = (uint64_t)strlen(json)},
                     &root),
                 "FF_BuilderAppendOpaqueJson");
    check_result(FF_BuilderSetRoot(&(FF_BuilderSetRootInfo){.builder = builder, .root = root}), "FF_BuilderSetRoot");

    FF_ViewHandle sealed = NULL;
    check_result(FF_BuilderFinalize(&(FF_BuilderFinalizeInfo){.builder = builder, .algorithm = FF_CHECKSUM_ALGO_NONE}, &sealed), "FF_BuilderFinalize");
    check(FF_ViewData(sealed) != NULL && FF_ViewSize(sealed) > 0, "the sealed view carries bytes");

    FF_ParserHandle parser = NULL;
    check_result(FF_CreateParserFromBuffer(FF_ViewData(sealed), FF_ViewSize(sealed), &parser),
                 "FF_CreateParserFromBuffer");
    check_result(FF_ValidateStream(parser), "FF_ValidateStream");

    FF_DestroyParser(parser);
    FF_DestroyView(sealed);
    FF_DestroyObjectHandle(root);
    FF_DestroyBuilder(builder);

    if (g_failures == 0) puts("install consumer (C): OK");
    return g_failures == 0 ? 0 : 1;
}
