/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * @file FF_C_API.cpp
 * @brief Implementation of the C ABI declared in <FastFHIR.h>.
 *
 * A thin facade: every function here translates C arguments into the C++ API
 * (declared in <FastFHIR.hpp>) and a C++ result back into an FF_ResultInfo. The
 * C++ API is not modified and does not know this file exists.
 *
 * HANDLE OWNERSHIP. Each opaque handle is one heap object holding the C++
 * value. The FF_Destroy* functions delete it. The C++ shared-ownership types
 * (Builder is a shared_ptr) keep their own object alive exactly as they do in
 * C++; destroying the C handle drops one owner and nothing else.
 *
 * MESSAGE LIFETIME. FF_ResultInfo.message points into a thread-local string.
 * Every call that returns a result overwrites it on that thread, which is what
 * the header documents.
 */

#include <FastFHIR.hpp>  // the C++ API this shim drives

#include "FastFHIR.h"    // the C ABI this shim implements (extern "C" declarations)

#include <cstring>
#include <new>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

/* =====================================================================
 * THE OPAQUE TYPES
 *
 * The C header forward-declares each as `struct FF_X_t*`; the definition
 * lives here and nowhere else. FF_Memory_t / FF_Builder_t etc. are free in
 * C++ — the C++ bodies are Memory_t / Builder_t (FastFHIR.hpp) — so the two
 * headers coexist without a clash.
 * ===================================================================== */
struct FF_Memory_t
{
    FastFHIR::Memory v;
};
struct FF_Builder_t
{
    FastFHIR::Builder v;  // shared_ptr<Builder_t>
};
struct FF_ObjectHandle_t
{
    FastFHIR::Reflective::ObjectHandle v;
};
struct FF_Parser_t
{
    FastFHIR::Parser v;
};
struct FF_View_t
{
    FastFHIR::Memory::View v;  // self-owning: keeps the arena alive
};

/* =====================================================================
 * Enum parity. These fail the build the moment a C++ value moves, so the C
 * header's documented numbers can never silently diverge from the wire.
 *
 * Each C++ operand is cast to int first. The C values are an anonymous enum and
 * the C++ ones are named enums, and comparing two different enumeration types
 * is deprecated in C++20 and ill-formed in C++26 -- Clang and GCC both diagnose
 * it, so an uncast comparison breaks any consumer building with -Werror.
 * ===================================================================== */
static_assert(FF_FHIR_R4 == static_cast<int>(FHIR_VERSION_R4), "FF_FHIR_R4 must mirror FHIR_VERSION_R4");
static_assert(FF_FHIR_R5 == static_cast<int>(FHIR_VERSION_R5), "FF_FHIR_R5 must mirror FHIR_VERSION_R5");
static_assert(FF_CHECKSUM_ALGO_NONE == static_cast<int>(FF_CHECKSUM_NONE), "checksum NONE diverged");
static_assert(FF_CHECKSUM_ALGO_CRC32 == static_cast<int>(FF_CHECKSUM_CRC32), "checksum CRC32 diverged");
static_assert(FF_CHECKSUM_ALGO_MD5 == static_cast<int>(FF_CHECKSUM_MD5), "checksum MD5 diverged");
static_assert(FF_CHECKSUM_ALGO_SHA256 == static_cast<int>(FF_CHECKSUM_SHA256), "checksum SHA256 diverged");

namespace
{

/// The one thread-local message buffer behind every FF_ResultInfo.message.
thread_local std::string g_message;

FF_ResultInfo make_result(FastFHIR::Result_Code code, std::string msg)
{
    g_message = std::move(msg);
    return FF_ResultInfo{
        static_cast<int32_t>(code),
        static_cast<int32_t>(FastFHIR::Result::severity_of(code)),
        g_message.c_str(),
    };
}

FF_ResultInfo ok_result()
{
    return make_result(FastFHIR::FF_SUCCESS, {});
}

FF_ResultInfo from_cpp(const FastFHIR::Result &r)
{
    return make_result(r.code, r.message);
}

FF_ResultInfo fail_result(const char *op, const char *why)
{
    return make_result(FastFHIR::FF_INVALID_ARGUMENT, std::string(op) + ": " + why);
}

} // namespace

extern "C"
{

/* ── Memory ─────────────────────────────────────────────────────────── */

FF_ResultInfo FF_CreateMemory(const FF_MemoryCreateInfo *info, FF_MemoryHandle *out)
{
    if (!info || !out) return fail_result("FF_CreateMemory", "null argument");
    *out = nullptr;
    try
    {
        FastFHIR::FF_MemoryCreateInfo ci;
        ci.capacity = info->capacity ? info->capacity : ci.capacity;
        ci.shm_name = info->shm_name;
        ci.filepath = info->filepath;
        FastFHIR::Memory memory;
        const FastFHIR::Result r = FastFHIR::FF_CreateMemory(ci, memory);
        if (!r.succeeded()) return from_cpp(r);
        *out = new FF_Memory_t{std::move(memory)};
        return ok_result();
    }
    catch (const std::exception &e) { return make_result(FastFHIR::FF_FAILURE, e.what()); }
}

FF_ResultInfo FF_OpenMemoryReadOnly(const char *filepath, FF_MemoryHandle *out)
{
    if (!filepath || !out) return fail_result("FF_OpenMemoryReadOnly", "null argument");
    *out = nullptr;
    try
    {
        *out = new FF_Memory_t{FastFHIR::Memory::openReadOnly(filepath)};
        return ok_result();
    }
    catch (const std::exception &e) { return make_result(FastFHIR::FF_FAILURE, e.what()); }
}

FF_ResultInfo FF_MemoryReset(FF_MemoryHandle memory, uint64_t committed_size)
{
    if (!memory) return fail_result("FF_MemoryReset", "null handle");
    return from_cpp(FastFHIR::FF_MemoryReset(memory->v, committed_size));
}

uint64_t FF_MemorySize(FF_MemoryHandle memory) { return memory ? FastFHIR::FF_MemorySize(memory->v) : 0; }
uint64_t FF_MemoryCapacity(FF_MemoryHandle memory) { return memory ? FastFHIR::FF_MemoryCapacity(memory->v) : 0; }
void     FF_DestroyMemory(FF_MemoryHandle memory) { delete memory; }

/* ── Builder ────────────────────────────────────────────────────────── */

FF_ResultInfo FF_CreateBuilder(const FF_BuilderCreateInfo *info, FF_BuilderHandle *out)
{
    if (!info || !out) return fail_result("FF_CreateBuilder", "null argument");
    *out = nullptr;
    try
    {
        FastFHIR::FF_BuilderCreateInfo ci;
        ci.capacity  = info->capacity ? info->capacity : ci.capacity;
        ci.version   = static_cast<FHIR_VERSION>(info->fhir_version ? info->fhir_version
                                                                    : FHIR_VERSION_R5);
        ci.arena     = info->arena ? info->arena->v : nullptr;
        ci.filepath  = info->filepath;
        ci.shm_name  = info->shm_name;
        FastFHIR::Builder builder;
        const FastFHIR::Result r = FastFHIR::FF_CreateBuilder(ci, builder);
        if (!r.succeeded()) return from_cpp(r);
        *out = new FF_Builder_t{std::move(builder)};
        return ok_result();
    }
    catch (const std::exception &e) { return make_result(FastFHIR::FF_FAILURE, e.what()); }
}

FF_ResultInfo FF_BuilderAppendOpaqueJson(const FF_BuilderAppendOpaqueJsonInfo *info,
                                         FF_ObjectHandle *out_object)
{
    if (!info || !info->builder || !info->json || !out_object)
        return fail_result("FF_BuilderAppendOpaqueJson", "null argument");
    *out_object = nullptr;
    try
    {
        FastFHIR::Reflective::ObjectHandle handle =
            info->builder->v->append_opaque_json(
                std::string_view(info->json, static_cast<size_t>(info->length)));
        *out_object = new FF_ObjectHandle_t{std::move(handle)};
        return ok_result();
    }
    catch (const std::exception &e) { return make_result(FastFHIR::FF_FAILURE, e.what()); }
}

FF_ResultInfo FF_BuilderSetRoot(const FF_BuilderSetRootInfo *info)
{
    if (!info || !info->builder || !info->root) return fail_result("FF_BuilderSetRoot", "null handle");
    return from_cpp(FastFHIR::FF_BuilderSetRoot(
        FastFHIR::FF_BuilderSetRootInfo{.builder = info->builder->v, .root = info->root->v}));
}

FF_ResultInfo FF_BuilderFinalize(const FF_BuilderFinalizeInfo *info, FF_ViewHandle *out_view)
{
    if (!info || !info->builder || !out_view) return fail_result("FF_BuilderFinalize", "null argument");
    *out_view = nullptr;
    try
    {
        FastFHIR::Memory::View view;
        // hasher stays null: it is a std::function the library calls back into,
        // which cannot cross the ABI. The header documents the consequence.
        const FastFHIR::Result r = FastFHIR::FF_BuilderFinalize(
            FastFHIR::FF_BuilderFinalizeInfo{
                .builder   = info->builder->v,
                .algorithm = static_cast<FF_Checksum_Algorithm>(info->algorithm),
                .hasher    = nullptr,
            },
            view);
        if (!r.succeeded()) return from_cpp(r);
        if (view.size() == 0) return make_result(FastFHIR::FF_FAILURE, "FF_BuilderFinalize: empty view");
        *out_view = new FF_View_t{std::move(view)};
        return ok_result();
    }
    catch (const std::exception &e) { return make_result(FastFHIR::FF_FAILURE, e.what()); }
}

void FF_DestroyBuilder(FF_BuilderHandle builder) { delete builder; }
void FF_DestroyObjectHandle(FF_ObjectHandle object) { delete object; }

/* ── Parse / read ───────────────────────────────────────────────────── */

FF_ResultInfo FF_CreateParserFromBuffer(const void *buffer, uint64_t size, FF_ParserHandle *out)
{
    if (!buffer || !out) return fail_result("FF_CreateParserFromBuffer", "null argument");
    *out = nullptr;
    try
    {
        *out = new FF_Parser_t{FastFHIR::Parser(buffer, static_cast<size_t>(size))};
        return ok_result();
    }
    catch (const std::exception &e) { return make_result(FastFHIR::FF_FAILURE, e.what()); }
}

FF_ResultInfo FF_CreateParserFromMemory(FF_MemoryHandle memory, FF_ParserHandle *out)
{
    if (!memory || !out) return fail_result("FF_CreateParserFromMemory", "null argument");
    *out = nullptr;
    try
    {
        *out = new FF_Parser_t{FastFHIR::Parser(memory->v)};
        return ok_result();
    }
    catch (const std::exception &e) { return make_result(FastFHIR::FF_FAILURE, e.what()); }
}

FF_ResultInfo FF_ValidateStream(FF_ParserHandle parser)
{
    if (!parser) return fail_result("FF_ValidateStream", "null handle");
    try { return from_cpp(parser->v.validate_FFHR_stream()); }
    catch (const std::exception &e) { return make_result(FastFHIR::FF_FAILURE, e.what()); }
}

uint64_t FF_ParserSize(FF_ParserHandle parser) { return parser ? parser->v.size_bytes() : 0; }
uint16_t FF_ParserRootTag(FF_ParserHandle parser) { return parser ? parser->v.root_type() : 0; }

FF_ResultInfo FF_ExportJson(const FF_ExportJsonInfo *info, uint64_t *out_length)
{
    if (!info || !info->parser || !out_length) return fail_result("FF_ExportJson", "null argument");
    try
    {
        std::ostringstream oss;
        info->parser->v.print_json(oss);
        const std::string json = oss.str();
        *out_length = json.size() + 1;  // include the NUL the buffer must hold
        if (!info->buffer) return ok_result();  // sizing query
        if (info->capacity < json.size() + 1)
            return make_result(FastFHIR::FF_CAPACITY_EXCEEDED, "FF_ExportJson: buffer too small");
        std::memcpy(info->buffer, json.data(), json.size());
        info->buffer[json.size()] = '\0';
        return ok_result();
    }
    catch (const std::exception &e) { return make_result(FastFHIR::FF_FAILURE, e.what()); }
}

void FF_DestroyParser(FF_ParserHandle parser) { delete parser; }

/* ── View ───────────────────────────────────────────────────────────── */

const void *FF_ViewData(FF_ViewHandle view) { return view ? view->v.data() : nullptr; }
uint64_t    FF_ViewSize(FF_ViewHandle view) { return view ? view->v.size() : 0; }
void        FF_DestroyView(FF_ViewHandle view) { delete view; }

/* ── Compact ────────────────────────────────────────────────────────── */

FF_ResultInfo FF_Compact(const FF_CompactInfo *info, FF_ViewHandle *out_view)
{
    if (!info || !info->source || !out_view) return fail_result("FF_Compact", "null argument");
    *out_view = nullptr;
    try
    {
        FastFHIR::FF_CompactInfo ci;
        ci.source    = info->source->v;
        ci.algorithm = static_cast<FF_Checksum_Algorithm>(info->algorithm);
        FastFHIR::Memory::View view;
        const FastFHIR::Result r = FastFHIR::FF_Compact(ci, view);
        if (!r.succeeded()) return from_cpp(r);
        *out_view = new FF_View_t{std::move(view)};
        return ok_result();
    }
    catch (const std::exception &e) { return make_result(FastFHIR::FF_FAILURE, e.what()); }
}

} // extern "C"
