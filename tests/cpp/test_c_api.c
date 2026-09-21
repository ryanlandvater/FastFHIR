/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * @file test_c_api.c
 * @brief The C ABI, used the way a C program uses it.
 *
 * Compiled by the C COMPILER (this is a .c file) and linked against
 * libfastfhir, so a green run proves the FF_* C surface is genuinely callable
 * from C -- not merely a header a C++ program could also read.
 *
 * Two halves:
 *   1. Produce -- create an arena and a builder, retain a resource, seal it,
 *      and parse the sealed bytes back. The C producer's only append is
 *      FF_BuilderAppendOpaqueJson, because the typed append is a C++ template;
 *      the round trip here is checked structurally (parse + validate + size).
 *   2. Consume -- open a typed .ffhr written by c_api_fixture.cpp, validate it,
 *      export its JSON, and compact it. This is the C reader, on real content.
 */

#include <FastFHIR.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

static int check_result(FF_ResultInfo r, const char *what)
{
    if (FF_ResultSucceeded(&r)) return 1;
    fprintf(stderr, "C ABI: FAILED %s: %s (code=%d)\n", what, r.message, r.code);
    ++g_failures;
    return 0;
}

static void check(int ok, const char *what)
{
    if (ok) return;
    fprintf(stderr, "C ABI: FAILED %s\n", what);
    ++g_failures;
}

/* -- Half 1: produce a stream and read it back structurally ---------- */
static void produce_round_trip(void)
{
    FF_BuilderCreateInfo binfo;
    memset(&binfo, 0, sizeof(binfo));
    binfo.capacity     = 1u << 20;
    binfo.fhir_version = FF_FHIR_R5;

    FF_BuilderHandle builder = NULL;
    if (!check_result(FF_CreateBuilder(&binfo, &builder), "FF_CreateBuilder")) return;

    static const char *const resource_json = "{\"resourceType\":\"Patient\",\"id\":\"p1\"}";
    FF_ObjectHandle root = NULL;
    if (check_result(FF_BuilderAppendOpaqueJson(
                         &(FF_BuilderAppendOpaqueJsonInfo){.builder = builder,
                                                           .json    = resource_json,
                                                           .length  = (uint64_t)strlen(resource_json)},
                         &root),
                     "FF_BuilderAppendOpaqueJson"))
    {
        check_result(FF_BuilderSetRoot(&(FF_BuilderSetRootInfo){.builder = builder, .root = root}), "FF_BuilderSetRoot");

        FF_ViewHandle sealed_view = NULL;
        check_result(FF_BuilderFinalize(&(FF_BuilderFinalizeInfo){.builder = builder, .algorithm = FF_CHECKSUM_ALGO_NONE}, &sealed_view),
                     "FF_BuilderFinalize");

        const void *bytes = FF_ViewData(sealed_view);
        const uint64_t nbytes = FF_ViewSize(sealed_view);
        check(bytes != NULL && nbytes > 0, "the sealed view carries bytes");

        FF_ParseInfo pinfo;
        memset(&pinfo, 0, sizeof(pinfo));
        pinfo.buffer = bytes;
        pinfo.size   = nbytes;

        FF_ParserHandle parser = NULL;
        check_result(FF_Parse(&pinfo, &parser), "FF_Parse");
        check(FF_ParserSize(parser) == nbytes, "the parser reports the sealed size");
        check_result(FF_ValidateStream(parser), "FF_ValidateStream");

        FF_DestroyParser(parser);
        FF_DestroyView(sealed_view);
    }

    FF_DestroyObjectHandle(root);
    FF_DestroyBuilder(builder);
}

/* -- Half 2: the C reader, on a typed stream ------------------------- */
static void consume_typed_stream(const char *path)
{
    FF_MemoryHandle memory = NULL;
    if (!check_result(FF_OpenMemoryReadOnly(path, &memory), "FF_OpenMemoryReadOnly")) return;

    FF_ParseInfo pinfo;
    memset(&pinfo, 0, sizeof(pinfo));
    pinfo.memory = memory;

    FF_ParserHandle parser = NULL;
    if (!check_result(FF_Parse(&pinfo, &parser), "FF_Parse (memory)"))
    {
        FF_DestroyMemory(memory);
        return;
    }
    check(FF_ParserSize(parser) > 0, "the parser sees the mounted file");
    check_result(FF_ValidateStream(parser), "FF_ValidateStream (typed)");

    /* Export JSON: size query, then the real call. */
    uint64_t needed = 0;
    check_result(FF_ExportJson(&(FF_ExportJsonInfo){.parser = parser, .buffer = NULL, .capacity = 0}, &needed),
                 "FF_ExportJson (sizing)");
    check(needed > 1, "the sizing call reports a non-empty document");

    char *json = (char *)malloc((size_t)needed);
    check(json != NULL, "malloc for the export buffer");
    if (json)
    {
        uint64_t written = 0;
        check_result(FF_ExportJson(&(FF_ExportJsonInfo){.parser = parser, .buffer = json, .capacity = needed},
                                   &written),
                     "FF_ExportJson");
        check(strstr(json, "Observation") != NULL, "the exported JSON names the resource");
        check(strstr(json, "c_api-1") != NULL, "the exported JSON carries the id");
        check(strstr(json, "mg/dL") != NULL, "the exported JSON carries the value unit");
        free(json);
    }

    FF_ViewHandle compacted = NULL;
    check_result(FF_Compact(&(FF_CompactInfo){.source = parser, .algorithm = FF_CHECKSUM_ALGO_NONE}, &compacted), "FF_Compact");
    check(FF_ViewSize(compacted) > 0, "the compacted view carries bytes");
    FF_DestroyView(compacted);

    FF_DestroyParser(parser);
    FF_DestroyMemory(memory);
}

static void memory_lifecycle(void)
{
    FF_MemoryCreateInfo minfo;
    memset(&minfo, 0, sizeof(minfo));
    minfo.capacity = 1u << 20;

    FF_MemoryHandle memory = NULL;
    if (!check_result(FF_CreateMemory(&minfo, &memory), "FF_CreateMemory")) return;
    check(FF_MemoryCapacity(memory) >= (1u << 20), "arena capacity is at least what was asked");
    check(FF_MemorySize(memory) == 0, "a fresh arena has no committed bytes");
    FF_DestroyMemory(memory);
}

/* Every Destroy* is documented as a safe no-op on NULL. A caller that frees in
 * a cleanup path that may not have run must not have to guard each one. */
static void null_destroy_is_safe(void)
{
    FF_DestroyMemory(NULL);
    FF_DestroyBuilder(NULL);
    FF_DestroyObjectHandle(NULL);
    FF_DestroyParser(NULL);
    FF_DestroyView(NULL);
    check(1, "every FF_Destroy*(NULL) returns without crashing");
}

/* THE SHARED-PTR CONTRACT, half 1: the Builder holds its own reference to the
 * arena, so a caller dropping the FF_MemoryHandle must not free the arena a
 * live Builder is still writing into. */
static void builder_outlives_memory_handle(void)
{
    FF_MemoryCreateInfo minfo;
    memset(&minfo, 0, sizeof(minfo));
    minfo.capacity = 1u << 20;

    FF_MemoryHandle memory = NULL;
    if (!check_result(FF_CreateMemory(&minfo, &memory), "FF_CreateMemory (owned)")) return;

    FF_BuilderCreateInfo binfo;
    memset(&binfo, 0, sizeof(binfo));
    binfo.arena         = memory;
    binfo.fhir_version  = FF_FHIR_R5;

    FF_BuilderHandle builder = NULL;
    if (!check_result(FF_CreateBuilder(&binfo, &builder), "FF_CreateBuilder (arena)"))
    {
        FF_DestroyMemory(memory);
        return;
    }

    /* Drop the only external arena reference. If the Builder did not hold a
     * shared_ptr of its own this would unmap the arena underneath it. */
    FF_DestroyMemory(memory);
    memory = NULL;

    static const char *const json = "{\"resourceType\":\"Patient\",\"id\":\"held\"}";
    FF_ObjectHandle root = NULL;
    check_result(FF_BuilderAppendOpaqueJson(
                     &(FF_BuilderAppendOpaqueJsonInfo){.builder = builder, .json = json,
                                                       .length = (uint64_t)strlen(json)},
                     &root),
                 "append after the memory handle is gone");
    check_result(FF_BuilderSetRoot(&(FF_BuilderSetRootInfo){.builder = builder, .root = root}), "set root after the memory handle is gone");

    FF_ViewHandle view = NULL;
    check_result(FF_BuilderFinalize(&(FF_BuilderFinalizeInfo){.builder = builder, .algorithm = FF_CHECKSUM_ALGO_NONE}, &view),
                 "finalize after the memory handle is gone");
    check(FF_ViewSize(view) > 0, "the build completed on an arena no handle names");

    FF_DestroyView(view);
    FF_DestroyObjectHandle(root);
    FF_DestroyBuilder(builder);
}

/* THE SHARED-PTR CONTRACT, half 2: the sealed View is self-owning, so it stays
 * valid after the Builder that produced it is destroyed. */
static void view_outlives_builder(void)
{
    FF_BuilderCreateInfo binfo;
    memset(&binfo, 0, sizeof(binfo));
    binfo.capacity     = 1u << 20;
    binfo.fhir_version = FF_FHIR_R5;

    FF_BuilderHandle builder = NULL;
    if (!check_result(FF_CreateBuilder(&binfo, &builder), "FF_CreateBuilder (view)")) return;

    static const char *const json = "{\"resourceType\":\"Patient\",\"id\":\"sealed\"}";
    FF_ObjectHandle root = NULL;
    check_result(FF_BuilderAppendOpaqueJson(
                     &(FF_BuilderAppendOpaqueJsonInfo){.builder = builder, .json = json,
                                                       .length = (uint64_t)strlen(json)},
                     &root),
                 "append for the view case");
    check_result(FF_BuilderSetRoot(&(FF_BuilderSetRootInfo){.builder = builder, .root = root}), "set root for the view case");

    FF_ViewHandle view = NULL;
    check_result(FF_BuilderFinalize(&(FF_BuilderFinalizeInfo){.builder = builder, .algorithm = FF_CHECKSUM_ALGO_NONE}, &view), "finalize for the view case");

    /* Snapshot the sealed bytes while the writer is still alive. */
    const uint64_t nbytes = FF_ViewSize(view);
    const void *bytes = FF_ViewData(view);
    check(bytes != NULL && nbytes > 0, "the sealed view carries bytes");
    unsigned char *before = (unsigned char *)malloc((size_t)nbytes);
    check(before != NULL, "malloc for the pre-destruction snapshot");
    if (before) memcpy(before, bytes, (size_t)nbytes);

    /* Destroy the last writer. The View must keep the arena mapped. */
    FF_DestroyBuilder(builder);
    builder = NULL;

    check(FF_ViewSize(view) == nbytes, "view size is unchanged after the Builder is gone");
    check(before != NULL && memcmp(before, FF_ViewData(view), (size_t)nbytes) == 0,
          "the sealed bytes are byte-identical after the Builder is gone");
    free(before);

    FF_ParseInfo pinfo;
    memset(&pinfo, 0, sizeof(pinfo));
    pinfo.buffer = FF_ViewData(view);
    pinfo.size   = nbytes;

    FF_ParserHandle parser = NULL;
    check_result(FF_Parse(&pinfo, &parser), "parse the view after the Builder is gone");
    check_result(FF_ValidateStream(parser), "validate the view after the Builder is gone");

    FF_DestroyParser(parser);
    FF_DestroyObjectHandle(root);
    FF_DestroyView(view);
}

int main(int argc, char **argv)
{
    memory_lifecycle();
    null_destroy_is_safe();
    builder_outlives_memory_handle();
    view_outlives_builder();
    produce_round_trip();

    if (argc > 1) consume_typed_stream(argv[1]);
    else fprintf(stderr, "C ABI: no fixture path; skipped the typed read path\n");

    if (g_failures == 0) puts("C ABI: OK");
    return g_failures == 0 ? 0 : 1;
}
