/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * @file FastFHIR.h
 * @brief The C ABI: FastFHIR callable from a C compiler.
 *
 * This header is PURE C. It exists so a C program can link libfastfhir and
 * drive it — create an arena, build a stream, seal it, parse it back, validate
 * it, export JSON — without a C++ compiler.
 *
 * <FastFHIR.hpp> is the C++ API and this is not a replacement for it: the C++
 * side keeps its classes, references and templates, and the two are different
 * surfaces over one library. Do NOT include both headers in one translation
 * unit; they name the same concepts with different types. Concretely: this
 * header's FF_ParseInfo is global and FastFHIR.hpp's is in namespace FastFHIR,
 * so one `using namespace FastFHIR;` makes the name ambiguous and the
 * translation unit stops compiling. Pick the surface you need.
 *
 * HANDLES ARE OPAQUE. A C caller only ever sees the pointer; the library owns
 * the object behind it. Every entry point that produces a handle
 * (FF_CreateMemory, FF_OpenMemoryReadOnly, FF_CreateBuilder, FF_Parse,
 * FF_Compact) has a matching FF_Destroy*, and the caller MUST call it. Handles
 * are independent: destroying one never invalidates another.
 *
 * NOTHING THROWS ACROSS THE BOUNDARY. Every function that can fail returns an
 * FF_ResultInfo; the void-returning FF_Destroy* functions and the size/accessor
 * getters cannot fail.
 *
 * FF_ResultInfo.message points into a THREAD-LOCAL buffer owned by the library.
 * It is valid only until the next FastFHIR C call on the same thread, so copy
 * it if it must outlive that. The message is UTF-8 and NUL-terminated; it is
 * "" on success.
 */
#ifndef FASTFHIR_H
#define FASTFHIR_H

#include <stdint.h>
#include <stddef.h>

/* FF_EXPORT — the symbol-visibility macro, defined once for both surfaces.
 * Pure preprocessor, so this C header and the C++ FF_Primitives.hpp share the
 * one definition instead of each carrying its own copy. */
#include "FF_Export.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =====================================================================
 * OPAQUE HANDLES
 *
 * Each names a library-owned object behind a pointer. The struct tag is never
 * defined in this header — that is what makes it opaque.
 * ===================================================================== */
typedef struct FF_Memory_t *      FF_MemoryHandle;   /* virtual memory arena */
typedef struct FF_Builder_t *     FF_BuilderHandle;  /* writes a stream into an arena */
typedef struct FF_ObjectHandle_t *FF_ObjectHandle;   /* a written object, for FF_BuilderSetRoot */
typedef struct FF_Parser_t *      FF_ParserHandle;   /* read lens over a sealed stream */
typedef struct FF_View_t *        FF_ViewHandle;     /* sealed/compacted bytes, owning */

/* =====================================================================
 * STATUS
 * ===================================================================== */
typedef struct FF_ResultInfo {
    int32_t     code;     /* a FastFHIR::Result_Code value: FF_SUCCESS, FF_FAILURE, ... */
    int32_t     severity; /* 0 success, 1 warning, 2 failure */
    const char *message;  /* thread-local, valid until the next call on this thread */
} FF_ResultInfo;

/* Warnings count as success — a result is "failed" only on severity 2. */
static inline int FF_ResultSucceeded(const FF_ResultInfo *r) { return r->severity != 2; }
static inline int FF_ResultFailed(const FF_ResultInfo *r)    { return r->severity == 2; }

/* =====================================================================
 * ENUM VALUES
 *
 * These mirror FastFHIR's own enums. They are spelled distinctly
 * (FF_CHECKSUM_ALGO_*, FF_FHIR_R*) because the C++ header already occupies
 * FF_CHECKSUM_*, FHIR_VERSION_R* at global scope, and the two headers must be
 * includable without a name clash. The shim static_asserts that every value
 * here equals its C++ counterpart, so they cannot drift.
 *
 * Only the enums a C entry point below actually takes are declared. There is no
 * FF_SOURCE_* here because there is no C ingest call to pass one to.
 * ===================================================================== */
enum { FF_FHIR_R4 = 0x0400, FF_FHIR_R5 = 0x0500 };

enum {
    FF_CHECKSUM_ALGO_NONE   = 0,
    FF_CHECKSUM_ALGO_CRC32  = 1,
    FF_CHECKSUM_ALGO_MD5    = 2,
    FF_CHECKSUM_ALGO_SHA256 = 3
};

/* =====================================================================
 * PARAMETER STRUCTS
 *
 * All-nullable/defaulted: zero-initialize a struct and set only the fields
 * you need. A 0 capacity means the library default (4 GiB sparse).
 * ===================================================================== */
typedef struct FF_MemoryCreateInfo {
    uint64_t    capacity;  /* sparse reservation in bytes; 0 = default */
    const char *shm_name;  /* nullable; cross-process SHM segment name */
    const char *filepath;  /* nullable; writable file-backed arena */
} FF_MemoryCreateInfo;

typedef struct FF_BuilderCreateInfo {
    uint64_t        capacity;     /* 0 = default */
    int32_t         fhir_version; /* FF_FHIR_R4 / FF_FHIR_R5; 0 = R5 */
    FF_MemoryHandle arena;        /* nullable; exclusive with filepath/shm_name */
    const char     *filepath;     /* nullable; file-backed arena */
    const char     *shm_name;     /* nullable; SHM arena */
} FF_BuilderCreateInfo;

typedef struct FF_ParseInfo {
    const void     *buffer; /* sealed stream bytes; nullable if memory is set */
    uint64_t        size;   /* bytes available at buffer */
    FF_MemoryHandle memory; /* nullable; exclusive with buffer */
} FF_ParseInfo;

/* =====================================================================
 * MEMORY (ARENA) API
 * ===================================================================== */

/** @brief Creates an arena. Writable (RAM/SHM/file). @p out is null on failure. */
FF_EXPORT FF_ResultInfo FF_CreateMemory(const FF_MemoryCreateInfo *info, FF_MemoryHandle *out);

/** @brief Mounts a file read-only — the reader's arena. The file is never written. */
FF_EXPORT FF_ResultInfo FF_OpenMemoryReadOnly(const char *filepath, FF_MemoryHandle *out);

/** @brief Resets the committed stream boundary (0 before streaming raw bytes in). */
FF_EXPORT FF_ResultInfo FF_MemoryReset(FF_MemoryHandle memory, uint64_t committed_size);

/** @brief Committed, globally visible bytes in the arena. */
FF_EXPORT uint64_t FF_MemorySize(FF_MemoryHandle memory);

/** @brief Total requested capacity of the sparse mapping. */
FF_EXPORT uint64_t FF_MemoryCapacity(FF_MemoryHandle memory);

/** @brief Destroys an arena handle. NULL is a safe no-op. */
FF_EXPORT void FF_DestroyMemory(FF_MemoryHandle memory);

/* =====================================================================
 * BUILDER API
 * ===================================================================== */

/** @brief Creates a stream builder. @p out is null on failure. */
FF_EXPORT FF_ResultInfo FF_CreateBuilder(const FF_BuilderCreateInfo *info, FF_BuilderHandle *out);

typedef struct FF_BuilderAppendOpaqueJsonInfo {
    FF_BuilderHandle builder; /* destination builder */
    const char      *json;    /* complete serialized JSON value, braces included */
    uint64_t         length;  /* bytes available at json */
} FF_BuilderAppendOpaqueJsonInfo;

/**
 * @brief Retains a raw JSON value this build cannot type, verbatim.
 *
 * The C producer's append: `info->json` is copied into the arena as an
 * opaque-JSON block whose bytes export back unquoted, so the document
 * round-trips. It is the C-accessible half of the append path -- the typed
 * `append_obj<T>` is a template and cannot cross a C ABI. @p out_object is null
 * on failure.
 */
FF_EXPORT FF_ResultInfo FF_BuilderAppendOpaqueJson(const FF_BuilderAppendOpaqueJsonInfo *info,
                                                        FF_ObjectHandle *out_object);

typedef struct FF_BuilderSetRootInfo {
    FF_BuilderHandle builder; /* the builder being sealed */
    FF_ObjectHandle  root;    /* the object that becomes the stream's root */
} FF_BuilderSetRootInfo;

/** @brief Assigns the stream's root resource (must precede finalize). */
FF_EXPORT FF_ResultInfo FF_BuilderSetRoot(const FF_BuilderSetRootInfo *info);

typedef struct FF_BuilderFinalizeInfo {
    FF_BuilderHandle builder;   /* the builder to seal */
    int32_t          algorithm; /* FF_CHECKSUM_ALGO_*; 0 (NONE) emits a zeroed checksum */
} FF_BuilderFinalizeInfo;

/**
 * @brief Seals the stream. @p out_view owns the sealed bytes.
 *
 * The C++ FF_BuilderFinalizeInfo also carries a `hasher` callback. There is no
 * C equivalent: it is a std::function the library invokes, which does not cross
 * an ABI, so a C caller gets the algorithm recorded in the header and a zeroed
 * hash slot. Hashing from C is the one thing this surface cannot do.
 */
FF_EXPORT FF_ResultInfo FF_BuilderFinalize(const FF_BuilderFinalizeInfo *info,
                                                FF_ViewHandle *out_view);

/** @brief Destroys a builder handle. NULL is a safe no-op. */
FF_EXPORT void FF_DestroyBuilder(FF_BuilderHandle builder);

/** @brief Destroys an object handle. NULL is a safe no-op. */
FF_EXPORT void FF_DestroyObjectHandle(FF_ObjectHandle object);

/* =====================================================================
 * PARSE / READ API
 * ===================================================================== */

/** @brief Parses and validates a stream header. @p out is null on failure. */
FF_EXPORT FF_ResultInfo FF_Parse(const FF_ParseInfo *info, FF_ParserHandle *out);

/**
 * @brief Walks the whole offset graph and checks it is structurally sound.
 *
 * Call this on any stream you did not produce. FF_Parse validates the header
 * and root offset only; every other offset is untrusted until this returns.
 */
FF_EXPORT FF_ResultInfo FF_ValidateStream(FF_ParserHandle parser);

/** @brief Total stream size in bytes. */
FF_EXPORT uint64_t FF_ParserSize(FF_ParserHandle parser);

/** @brief The root resource's wire recovery tag (0 when the parser is empty). */
FF_EXPORT uint16_t FF_ParserRootTag(FF_ParserHandle parser);

typedef struct FF_ExportJsonInfo {
    FF_ParserHandle parser;
    char           *buffer;   /* NULL for a sizing query */
    uint64_t        capacity; /* bytes available at buffer */
} FF_ExportJsonInfo;

/**
 * @brief Exports the parsed stream as JSON text.
 *
 * Two calls: pass @p info->buffer NULL to read the required size (including the
 * NUL) into @p out_length, then call again with a buffer of at least that size.
 * A too-small buffer writes nothing and returns FF_CAPACITY_EXCEEDED with the
 * required size in @p out_length.
 *
 * EACH CALL RENDERS THE WHOLE DOCUMENT. Nothing is cached between them, so the
 * size-then-write pair serializes twice -- on a 50 MiB bundle that is two full
 * renders. A caller that minds this should skip the sizing call, pass a buffer
 * it expects to fit, and re-run only on FF_CAPACITY_EXCEEDED, which reports the
 * exact size it needs.
 */
FF_EXPORT FF_ResultInfo FF_ExportJson(const FF_ExportJsonInfo *info, uint64_t *out_length);

/** @brief Destroys a parser handle. NULL is a safe no-op. */
FF_EXPORT void FF_DestroyParser(FF_ParserHandle parser);

/* =====================================================================
 * VIEW API (sealed / compacted bytes)
 * ===================================================================== */

/** @brief Pointer to the first byte of the view. Valid while the view lives. */
FF_EXPORT const void *FF_ViewData(FF_ViewHandle view);

/** @brief Size of the view in bytes. */
FF_EXPORT uint64_t FF_ViewSize(FF_ViewHandle view);

/** @brief Destroys a view handle. NULL is a safe no-op. */
FF_EXPORT void FF_DestroyView(FF_ViewHandle view);

/* =====================================================================
 * COMPACT API
 * ===================================================================== */

typedef struct FF_CompactInfo {
    FF_ParserHandle source;    /* the parsed stream to archive */
    int32_t         algorithm; /* FF_CHECKSUM_ALGO_*; see FF_BuilderFinalize on hashing */
} FF_CompactInfo;

/** @brief Archives @p info->source into a denser layout. @p out_view owns the bytes. */
FF_EXPORT FF_ResultInfo FF_Compact(const FF_CompactInfo *info, FF_ViewHandle *out_view);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* FASTFHIR_H */
