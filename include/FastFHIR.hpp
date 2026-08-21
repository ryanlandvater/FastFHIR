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
 * FastFHIR::FF_StreamCreateInfo create_info;          // defaults: 4 GB arena, R5
 * FastFHIR::FF_Stream stream;
 * FastFHIR::FF_CreateStream(create_info, stream);
 *
 * ObservationData obs;
 * auto root = FastFHIR::FF_StreamAppendObject(stream, obs); // never throws
 * root["status"] = "final";                                  // mutable handle path
 * FastFHIR::FF_StreamSetRoot(FastFHIR::FF_StreamSetRootInfo{
 *     .stream = stream,
 *     .root = root,
 * });
 *
 * // Seal the file with a lambda crypto callback
 * FastFHIR::Memory::View payload;
 * FastFHIR::FF_StreamFinalize(FastFHIR::FF_StreamFinalizeInfo{
 *     .stream = stream,
 *     .algorithm = FF_CHECKSUM_SHA256,
 *     .hasher = [](const unsigned char* byte_start, Size bytes_to_hash) -> std::vector<BYTE> {
 *         std::vector<BYTE> hash(SHA256_DIGEST_LENGTH);
 *         SHA256(byte_start, bytes_to_hash, hash.data());
 *         return hash;
 *     },
 * }, payload);
 *
 * // 2. Parse the zero-copy stream
 * FastFHIR::Parser parser;
 * FastFHIR::FF_Parse(FastFHIR::FF_ParseInfo{
 *     .buffer = payload.data(),
 *     .size = payload.size(),
 * }, parser);
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
 * FastFHIR::FF_StreamCreateInfo create_info;
 * create_info.capacity = 2ULL * 1024 * 1024 * 1024;   // 2GB Virtual Arena
 * FastFHIR::FF_Stream stream;
 * FastFHIR::FF_CreateStream(create_info, stream);
 * std::vector<std::thread> pool;
 *
 * // 32 threads simultaneously serializing AI inferences into the same stream
 * for (int i = 0; i < 32; ++i) {
 *      pool.emplace_back([stream, i]() {
 *      // 1. Thread-local work (AI inference, data fetching, etc.)
 *      
 *      ObservationData local_obs;
 *      local_obs.status = "preliminary";
 *      // 2. Lock-free 1-clock-cycle atomic claim and concurrent write
 *      // No mutexes. No heap allocations. No pointer invalidation.
 *      auto handle = FastFHIR::FF_StreamAppendObject(stream, local_obs);
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
// arena, the Parser value type, and the Reflective mutation handles.
#include "FF_Primitives.hpp"
#include "FF_Parser.hpp"
#include "FF_Builder.hpp"

#include <functional>
#include <string_view>

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
//   - Handles are shared-ownership value types: FF_Memory, FF_Stream,
//     FF_Ingestor. Copy them freely; they refer to one underlying object.
//   - Create/lifecycle functions follow the Vulkan pattern: an Info struct
//     in, a `T& out` parameter, an FF_Result out. Errors never throw — the
//     implementation catches everything below this boundary.
//   - Read/query returns (Parser, Memory::View) are cheap value types.
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
// HANDLES
// =====================================================================
// FF_Memory  — virtual memory arena (RAM, SHM, or file-backed).
// FF_Stream  — a buildable FastFHIR stream (the old Builder).
// FF_Ingestor — concurrent clinical-data ingestion engine.
//
// FF_Ingestor_t is intentionally opaque: its definition lives in the internal
// FF_Ingestor.hpp so this header never drags in simdjson/WAMR.

using FF_Memory   = std::shared_ptr<Memory>;
using FF_Stream   = std::shared_ptr<Builder>;
class FF_Ingestor_t;
using FF_Ingestor = std::shared_ptr<FF_Ingestor_t>;

// =====================================================================
// SHARED CALLBACK
// =====================================================================
/** @brief Checksum callback: hashes [byte_start, byte_start + bytes_to_hash). */
using FF_HashCallback = std::function<std::vector<BYTE>(const unsigned char* byte_start, Size bytes_to_hash)>;

// =====================================================================
// MEMORY (ARENA) API
// =====================================================================
/** @brief Parameters for creating a virtual memory arena. */
struct FF_MemoryCreateInfo {
    Size        capacity = 4ULL * 1024 * 1024 * 1024; ///< Sparse virtual reservation.
    const char* shm_name = nullptr;  ///< Cross-process SHM segment name; null = anonymous RAM.
    const char* filepath = nullptr;  ///< File-backed arena; exclusive with shm_name.
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
// STREAM API
// =====================================================================
/** @brief Parameters for creating a buildable FastFHIR stream. */
struct FF_StreamCreateInfo {
    Size        capacity = 4ULL * 1024 * 1024 * 1024; ///< Sparse virtual reservation.
    FHIR_VERSION version  = FHIR_VERSION_R5;          ///< FHIR schema revision for the stream.
    FF_Memory   arena    = nullptr;                   ///< Existing arena to build into; exclusive with filepath/shm_name.
    const char* filepath = nullptr;                   ///< File-backed arena; exclusive with arena/shm_name.
    const char* shm_name = nullptr;                   ///< Cross-process SHM arena; exclusive with arena/filepath.
};

/** @brief Creates a stream. @p out_stream is null on failure. */
FF_EXPORT FF_Result FF_CreateStream(const FF_StreamCreateInfo& info, FF_Stream& out_stream) noexcept;

/** @brief Appends a typed resource/backbone value and returns a mutable handle for `[]` access.
 *
 * The one two-argument entry point: the payload is a template parameter, so it
 * cannot live in an Info struct without forcing a copy. Mirrors the Iris
 * handle-plus-parameters shape (`viewer_engine_translate(viewer, scope)`).
 *
 * @return A valid ObjectHandle, or a null handle on failure (never throws).
 */
template <typename T_Data>
inline Reflective::ObjectHandle FF_StreamAppendObject(FF_Stream stream, const T_Data& data) noexcept
{
    if (!stream) return {};
    try {
        return stream->append_obj(data);
    } catch (const std::exception&) {
        return {};
    }
}

/** @brief Parameters for assigning the stream's root resource. */
struct FF_StreamSetRootInfo {
    FF_Stream               stream = nullptr;
    Reflective::ObjectHandle root;  ///< Handle returned by FF_StreamAppendObject.
};

/** @brief Assigns the root resource of the stream (must precede finalize). */
FF_EXPORT FF_Result FF_StreamSetRoot(const FF_StreamSetRootInfo& info) noexcept;

/** @brief Parameters for sealing a stream into its final on-disk form. */
struct FF_StreamFinalizeInfo {
    FF_Stream             stream    = nullptr;
    FF_Checksum_Algorithm algorithm = FF_CHECKSUM_NONE;
    FF_HashCallback       hasher    = nullptr;  ///< Optional; required when algorithm != NONE.
};

/** @brief Seals the stream (header + optional checksum) and returns a zero-copy view of it. */
FF_EXPORT FF_Result FF_StreamFinalize(const FF_StreamFinalizeInfo& info, Memory::View& out_view) noexcept;

/** @brief Parameters for snapshotting a stream's current state mid-build. */
struct FF_StreamQueryInfo {
    FF_Stream stream = nullptr;
};

/** @brief Returns a read-only Parser over the stream's current state (nearly zero-cost). */
FF_EXPORT FF_Result FF_StreamQuery(const FF_StreamQueryInfo& info, Parser& out_parser) noexcept;

// =====================================================================
// PARSE API
// =====================================================================
/** @brief Parameters for parsing a sealed FastFHIR byte stream. */
struct FF_ParseInfo {
    const void* buffer = nullptr;  ///< First byte of a sealed FastFHIR stream.
    Size        size   = 0;        ///< Total bytes available at @p buffer.
};

/** @brief Parses and validates a stream header. @p out_parser is invalid (bool false) on failure. */
FF_EXPORT FF_Result FF_Parse(const FF_ParseInfo& info, Parser& out_parser) noexcept;

// =====================================================================
// COMPACT API
// =====================================================================
/** @brief Parameters for compacting a parsed stream into dense field form. */
struct FF_CompactInfo {
    Parser                source;    ///< Parsed stream to archive.
    FF_Checksum_Algorithm algorithm = FF_CHECKSUM_NONE;
    FF_HashCallback       hasher    = nullptr;
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
    FF_Stream      stream    = nullptr;  ///< Destination stream.
    FF_SourceType  source_type = FF_SOURCE_FHIR_JSON;
    FF_ExtensionFilterMode extension_filter = FF_ExtensionFilterMode::FILTER_ALL_KNOWN; ///< URL-directory suppression policy.
    const std::string_view payload;      ///< Raw source document — never modified.
    Size           payload_capacity = 0; ///< Allocated bytes at payload.data() incl. simdjson slack; 0 = safe copy.
};

/** @brief Parses @p payload and appends the resulting object(s) to @p info.stream.
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