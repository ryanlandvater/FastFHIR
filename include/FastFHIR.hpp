/**
 * @file FastFHIR.hpp
 * @author Ryan Landvater (ryanlandvater[at]gmail[dot]com)
 * @copyright Copyright (c) 2026 Ryan Landvater. All rights reserved.
 * @remark This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0 (MPL-2.0) — see LICENSE or http://mozilla.org/MPL/2.0/.
 * @version 0.1
 *
 * @brief FastFHIR — Public API
 *
 * This is the only header consumers of the FastFHIR library need to include.
 *
 * 
 * 
 * =====================================================================
 * Quick Start Example 1: Standard Build and Parse
 * =====================================================================
 * @code
 * #include <FastFHIR.hpp>
 * #include <openssl/sha.h>
 *
 * // 1. Build a stream
 * FastFHIR::FF_BuilderCreateInfo create_info;          // defaults: 4 GB arena, R5
 * FF_Builder builder;
 * FastFHIR::FF_CreateBuilder(create_info, builder);
 *
 * ObservationData obs;
 * auto root = FastFHIR::FF_BuilderAppendObject(builder, obs); // never throws
 * root["status"] = "final";                                  // mutable handle path
 * FastFHIR::FF_BuilderSetRoot(FastFHIR::FF_BuilderSetRootInfo{
 *     .builder = builder,
 *     .root = root,
 * });
 *
 * // Seal the file with a lambda crypto callback
 * FastFHIR::Memory::View payload;
 * FastFHIR::FF_BuilderFinalize(FastFHIR::FF_BuilderFinalizeInfo{
 *     .builder = builder,
 *     .algorithm = FF_CHECKSUM_SHA256,
 *     .hasher = [](const unsigned char* byte_start, Size bytes_to_hash) -> std::vector<BYTE> {
 *         std::vector<BYTE> hash(SHA256_DIGEST_LENGTH);
 *         SHA256(byte_start, bytes_to_hash, hash.data());
 *         return hash;
 *     },
 * }, payload);
 *
 * // 2. Parse the zero-copy stream
 * FastFHIR::Parser parser(payload.data(), payload.size());
 * 
 * // 3. Access the root resource and its fields with zero-copy accessors
 * // Access by generated field key for better performance and safety:
 * auto status = parser.root()[FastFHIR::FieldKeys::Observation::STATUS].value().as_string();
 * // Or access by string key:
 * auto status = parser.root()["status"].value().as_string();
 * 
 * @endcode
 *
 * =====================================================================
 * Quick Start Example 2: Lock-Free Concurrent Generation
 * =====================================================================
 * @code
 * #include <FastFHIR.hpp>
 * #include <thread>
 * #include <vector>
 *
 * FastFHIR::FF_BuilderCreateInfo create_info;
 * create_info.capacity = 2ULL * 1024 * 1024 * 1024;   // 2GB Virtual Arena
 * FF_Builder builder;
 * FastFHIR::FF_CreateBuilder(create_info, builder);
 * std::vector<std::thread> pool;
 *
 * // 32 threads simultaneously serializing AI inferences into the same stream
 * for (int i = 0; i < 32; ++i) {
 *      pool.emplace_back([builder, i]() {
 *      // 1. Thread-local work (AI inference, data fetching, etc.)
 *      
 *      ObservationData local_obs;
 *      local_obs.status = "preliminary";
 *      // 2. Lock-free 1-clock-cycle atomic claim and concurrent write
 *      // No mutexes. No heap allocations. No pointer invalidation.
 *      auto handle = FastFHIR::FF_BuilderAppendObject(builder, local_obs);
 *      // 3. (Optional) push handle.offset() to a lock-free queue to link to a Bundle later
 * });
 * }
 *
 * for (auto& thread : pool) thread.join();
 * @endcode
 */

#pragma once

// =====================================================================
// Version — injected at compile time by CMake, which reads
// FASTFHIR_VERSION_MAJOR / _MINOR / _BUILD environment variables
// (set by CI from the GitHub release tag, e.g. v1.2.3 → 1, 2, 3).
// The #ifndef guards keep local / offline builds working without CI.
// =====================================================================
#include "FF_Version.hpp"

// NOTE:
// Generic FF_* key registry is intentionally not included by default to avoid
// accidental use in C++ mutation paths. Include <FF_FieldKeys.hpp> explicitly
// ONLY when you need generic reflective keys. It's confusing and not recommended.

// The type layer the FF_* surface references: wire primitives, the Memory
// arena, the Parser value type, the Reflective mutation handles, and the
// archive recovery surface (REC-10…17). Recovery does no work at construction
// — the scan is explicit — so carrying it on the core surface costs nothing.
#include "FF_Primitives.hpp"
#include "FF_Parser.hpp"
#include "FF_Builder.hpp"
#include "FF_Recovery.hpp"

#include <functional>
#include <string_view>

// =====================================================================
// HANDLES AND SHARED ALIASES
// =====================================================================
// A handle is the type a caller passes around; the heap body it owns carries
// the _t suffix. Builder and Ingestor are always heap-allocated, so their
// handle is a shared_ptr and these names alias it. Memory IS its own handle --
// a copyable value that owns the arena by shared_ptr -- so it keeps its name.
//
// Declared above the FF_*Info structs, because those name the handles by
// their global FF_ spelling (the block just below).
namespace FastFHIR
{
class Ingestor_t;  ///< opaque; defined in the internal FF_Ingestor.hpp
using Builder  = std::shared_ptr<Builder_t>;
using Ingestor = std::shared_ptr<Ingestor_t>;

/** @brief Checksum callback: hashes [byte_start, byte_start + bytes_to_hash). */
using HashCallback = std::function<std::vector<BYTE>(const unsigned char* byte_start, Size bytes_to_hash)>;
} // namespace FastFHIR

// =====================================================================
// GLOBAL C-STYLE ALIASES -- the ONE list
// =====================================================================
// Inside the namespace every type has its plain C++ name; these FF_-prefixed
// names are that type's C-style spelling, at global scope, for a consumer that
// does not want to qualify with FastFHIR:: on every line. The FF_ prefix is
// the namespace spelled out; it is never a second type.
//
// Not aliased here: the wire block structs (FF_HEADER, FF_STRING, ...) and the
// C-ABI Info structs (FF_BuilderCreateInfo, ...), which keep their FF_ names
// because they ARE the C surface, not a C++ type behind one. There is no
// FF_ParseInfo on either side: the parse surface is Parser's two constructors.
using FF_Memory       = FastFHIR::Memory;        // shared_ptr<Memory_t>, the arena handle
using FF_Builder      = FastFHIR::Builder;       // shared_ptr<Builder_t>
using FF_Ingestor     = FastFHIR::Ingestor;      // shared_ptr<Ingestor_t>
using FF_String       = FastFHIR::String;        // POCO string field
using FF_DateTime     = FastFHIR::DateTime;      // POCO date/time field: components or text
using FF_UcumUnit     = FastFHIR::UcumUnit;      // a UCUM unit constant (FF_CODE::UCUM::*)
using FF_HashCallback = FastFHIR::HashCallback;  // checksum callback

// The result types (FF_Result, FF_Result_Code, FF_Result_Severity) and their
// enumerators alias in FF_Primitives.hpp, beside those definitions, because
// FF_Builder.hpp returns FF_Result without seeing this header. One list, split
// only where the include graph forces it.

/// The POCO 0..1 child block. A template alias, so `FF_Optional<T>` is the
/// consumer spelling of `FastFHIR::Optional<T>`.
template <typename T>
using FF_Optional = FastFHIR::Optional<T>;

namespace FastFHIR {

// =====================================================================
// FF_* EXTERNAL API
//
// This namespace block IS the public interface of FastFHIR. Every entry
// point is a free function that either returns an FF_Result or returns a
// lightweight value, never throws across the API boundary, and — except for
// the one templated append — takes a single `const FF_XxxInfo&` argument so
// the surface can grow by adding struct fields instead of changing
// signatures.
//
// Design conventions (see Iris-Headers for the origin of this shape):
//   - Handles are shared-ownership value types: Memory, Builder, Ingestor.
//     Copy them freely; they refer to one underlying object. Inside the
//     namespace they carry their plain C++ names; the FF_-prefixed aliases at
//     the top of this header are the C-style spelling for consumers.
//   - Create/lifecycle functions follow the Vulkan pattern: an Info struct
//     in, a `T& out` parameter, an FF_Result out. Errors never throw — the
//     implementation catches everything below this boundary.
//   - Read/query returns (Parser, Memory::View) are cheap value types. Parser
//     is the one surface that is NOT an Info struct: it is built directly with
//     `Parser(buffer, size)` or `Parser(memory)`, because the two forms are
//     mutually exclusive and two constructors make that a compile-time fact
//     rather than a runtime check. A bad stream throws `FastFHIR Parsing
//     Error: ...` from the constructor.
//   - The Reflective mutation path (`handle["field"] = value`) is unchanged
//     and remains the way fields are written once an object is appended.
// =====================================================================

// =====================================================================
// VERSION
// =====================================================================
/** @brief FastFHIR engine version, resolved from the compile-time macros. */
struct FF_Version {
    uint32_t major = 0;
    uint32_t minor = 0;
    uint32_t build = 0;
};

/** @brief Returns the engine version baked in at compile time. */
inline FF_Version FF_GetVersion() noexcept
{
    return { FASTFHIR_VERSION_MAJOR, FASTFHIR_VERSION_MINOR, FASTFHIR_VERSION_BUILD };
}

// =====================================================================
// MEMORY (ARENA) API
// =====================================================================
/** @brief Parameters for creating a virtual memory arena. */
struct FF_MemoryCreateInfo {
    Size        capacity = 4ULL * 1024 * 1024 * 1024; ///< Sparse virtual reservation.
    const char* shm_name = nullptr;  ///< Cross-process SHM segment name; null = anonymous RAM.
    /// File-backed arena, writable (Memory::createFromFile). To read a file
    /// without being able to change it, use Memory::openReadOnly() and hand the
    /// arena to `Parser(memory)`. Exclusive with shm_name.
    const char* filepath = nullptr;
};

/** @brief Creates a memory arena. @p out_memory is null on failure. */
FF_EXPORT FF_Result FF_CreateMemory(const FF_MemoryCreateInfo& info, FF_Memory& out_memory) noexcept;

/** @brief Resets the committed stream boundary of the arena (0 before streaming raw bytes in). */
FF_EXPORT FF_Result FF_MemoryReset(const FF_Memory& memory, Size committed_size) noexcept;

/** @brief Committed, globally visible bytes in the arena. */
FF_EXPORT Size FF_MemorySize(const FF_Memory& memory) noexcept;

/** @brief Total requested capacity of the sparse mapping. */
FF_EXPORT Size FF_MemoryCapacity(const FF_Memory& memory) noexcept;

// =====================================================================
// BUILDER API
// =====================================================================
/** @brief Parameters for creating a builder (and the arena it writes into). */
struct FF_BuilderCreateInfo {
    Size        capacity  = 4ULL * 1024 * 1024 * 1024; ///< Sparse virtual reservation.
    FHIR_VERSION version   = FHIR_VERSION_R5;          ///< FHIR schema revision for the stream.
    FF_Memory   arena     = nullptr;                   ///< Existing arena to build into; exclusive with filepath/shm_name.
    const char* filepath  = nullptr;                   ///< File-backed arena; exclusive with arena/shm_name.
    const char* shm_name  = nullptr;                   ///< Cross-process SHM arena; exclusive with arena/filepath.
};

/** @brief Creates a stream. @p out_stream is null on failure. */
FF_EXPORT FF_Result FF_CreateBuilder(const FF_BuilderCreateInfo& info, FF_Builder& out_builder) noexcept;

/** @brief Appends a typed resource/backbone value and returns a mutable handle for `[]` access.
 *
 * The one two-argument entry point: the payload is a template parameter, so it
 * cannot live in an Info struct without forcing a copy. Mirrors the Iris
 * handle-plus-parameters shape (`viewer_engine_translate(viewer, scope)`).
 *
 * @return A valid ObjectHandle, or a null handle on failure (never throws).
 */
template <typename T_Data>
inline Reflective::ObjectHandle FF_BuilderAppendObject(FF_Builder builder, const T_Data& data) noexcept
{
    if (!builder) return {};
    try {
        return builder->append_obj(data);
    } catch (const std::exception&) {
        return {};
    }
}

/** @brief Parameters for assigning the stream's root resource. */
struct FF_BuilderSetRootInfo {
    FF_Builder               builder = nullptr;
    Reflective::ObjectHandle root;  ///< Handle returned by FF_BuilderAppendObject.
};

/** @brief Assigns the root resource of the builder's stream (must precede finalize). */
FF_EXPORT FF_Result FF_BuilderSetRoot(const FF_BuilderSetRootInfo& info) noexcept;

/** @brief Parameters for sealing a stream into its final on-disk form. */
struct FF_BuilderFinalizeInfo {
    FF_Builder            builder   = nullptr;
    FF_Checksum_Algorithm algorithm = FF_CHECKSUM_NONE;
    HashCallback          hasher    = nullptr;  ///< Optional; required when algorithm != NONE.
};

/** @brief Seals the stream (header + optional checksum) and returns a zero-copy view of it. */
FF_EXPORT FF_Result FF_BuilderFinalize(const FF_BuilderFinalizeInfo& info, Memory::View& out_view) noexcept;

/** @brief Parameters for snapshotting a stream's current state mid-build. */
struct FF_BuilderQueryInfo {
    FF_Builder builder = nullptr;
};

/** @brief Returns a read-only Parser over the stream's current state (nearly zero-cost). */
FF_EXPORT FF_Result FF_BuilderQuery(const FF_BuilderQueryInfo& info, Parser& out_parser) noexcept;

/** @brief Parameters for attaching the optional conformance layer to a builder.
 *
 * The layer is FHIR-level conformance — cardinality, required elements, bound
 * ValueSets — not structure, which the builder checks unconditionally. It
 * OBSERVES ONLY: the check runs before arena space is claimed, so a stream
 * written with a layer attached is byte-identical to one written without it,
 * including on the failing path. Attach before the first append.
 */
struct FF_BuilderAttachLayerInfo {
    FF_Builder builder = nullptr;  ///< The builder to attach to.
    /// Borrowed, and it must outlive the builder. Null detaches. Copy
    /// FastFHIR::Conformance::conformance_layer()'s struct before setting
    /// policy/next/diagnostic/failures on it — the one it returns is shared and
    /// immutable. Every layer in the chain must speak this release's
    /// FastFHIR::Conformance::CONFORMANCE_ABI.
    const FastFHIR::Conformance::ValidationHooks* hooks = nullptr;
};

/** @brief Attaches the conformance layer; a null handle or an ABI mismatch is an FF_Result. */
FF_EXPORT FF_Result FF_BuilderAttachLayer(const FF_BuilderAttachLayerInfo& info) noexcept;

// =====================================================================
// PARSE API
// =====================================================================
// There is no FF_ParseInfo and no FF_Parse here: `Parser` IS the parse
// surface, built with one of its two explicit constructors —
//   Parser(buffer, size)  bytes the caller already holds (a socket, a vector)
//   Parser(memory)        an arena from Memory::openReadOnly(), or a Builder
// The two forms are mutually exclusive by construction, so the old runtime
// "buffer and memory are mutually exclusive" check is gone rather than moved.
// Construction validates the header and root, and throws `FastFHIR Parsing
// Error: ...` on a bad stream (including a null buffer or arena). The C ABI
// mirrors the two constructors with two factories, FF_CreateParserFromBuffer
// and FF_CreateParserFromMemory (FastFHIR.h).

// =====================================================================
// COMPACT API
// =====================================================================
/** @brief Parameters for compacting a parsed stream into dense field form. */
struct FF_CompactInfo {
    Parser                source;    ///< Parsed stream to archive.
    FF_Checksum_Algorithm algorithm = FF_CHECKSUM_NONE;
    HashCallback          hasher    = nullptr;
};

/** @brief Archives @p source into a fresh arena and returns a view of the compacted stream. */
FF_EXPORT FF_Result FF_Compact(const FF_CompactInfo& info, Memory::View& out_view) noexcept;

// =====================================================================
// INGEST API
// =====================================================================
/** @brief Parameters for creating the ingestion engine. */
struct FF_IngestorCreateInfo {
    Size       logger_capacity = 64ULL * 1024 * 1024; ///< Lock-free warning buffer bytes.
    uint32_t   concurrency     = 0;                   ///< Worker threads; 0 = hardware concurrency.
};

/** @brief Creates an ingestor. @p out_ingestor is null on failure. */
FF_EXPORT FF_Result FF_CreateIngestor(const FF_IngestorCreateInfo& info, FF_Ingestor& out_ingestor) noexcept;

/** @brief Parameters for ingesting one clinical payload into a stream. */
struct FF_IngestInfo {
    FF_Ingestor    ingestor  = nullptr;
    FF_Builder     builder   = nullptr;  ///< Destination builder.
    FF_SourceType  source_type = FF_SOURCE_FHIR_JSON;
    FF_ExtensionFilterMode extension_filter = FF_ExtensionFilterMode::FILTER_ALL_KNOWN; ///< URL-directory suppression policy.
    const std::string_view payload;      ///< Raw source document — never modified.
    Size           payload_capacity = 0; ///< Allocated bytes at payload.data() incl. simdjson slack; 0 = safe copy.
};

/** @brief Parses @p payload and appends the resulting object(s) to @p info.builder.
 *
 * @param out_root        Mutable handle of the inserted root object.
 * @param out_parsed_count Number of top-level resources parsed.
 */
FF_EXPORT FF_Result FF_Ingest(const FF_IngestInfo& info,
                              Reflective::ObjectHandle& out_root,
                              Size& out_parsed_count) noexcept;

/** @brief Parameters for parsing a payload directly into one field of an existing object. */
struct FF_IngestInsertInfo {
    FF_Ingestor              ingestor    = nullptr;
    Reflective::ObjectHandle parent;      ///< Object being amended.
    FF_FieldKey              key;         ///< Target field within @p parent.
    const std::string_view   payload;     ///< Raw source document — never modified.
    FF_SourceType            source_type = FF_SOURCE_FHIR_JSON;
};

/** @brief Parses @p payload and patches it into @p parent[@p key]. */
FF_EXPORT FF_Result FF_IngestInsertAtField(const FF_IngestInsertInfo& info) noexcept;

} // namespace FastFHIR