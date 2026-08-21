/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * @file FF_API.cpp
 * @brief Implementations of the FF_* external API surface (see FastFHIR.hpp).
 *
 * This file is the error boundary: every function is noexcept and converts
 * any exception raised by the engine below into an FF_Result with a message,
 * so nothing escapes to the caller. The ingest entry points (FF_CreateIngestor,
 * FF_Ingest, FF_IngestInsertAtField) live in FF_Ingestor.cpp — they are the
 * only FF_* functions that require simdjson, so they stay in the ingestor
 * target instead of dragging that dependency into the core library.
 */

#include "FastFHIR.hpp"
#include "FF_Compactor.hpp"
#include <exception>

namespace FastFHIR {

namespace {

/// Runs @p fn inside the API error boundary. Any exception becomes an
/// FF_Result; the catch-all exists only because "nothing escapes" is the
/// documented contract of the FF_* surface.
template <typename Fn>
FF_Result FF_Guard(const char* op, Fn&& fn) noexcept
{
    try {
        fn();
        return FF_Result{FF_SUCCESS};
    } catch (const std::exception& e) {
        return FF_Result{FF_FAILURE, std::string(op) + ": " + e.what()};
    } catch (...) {
        return FF_Result{FF_FAILURE, std::string(op) + ": unknown non-std exception"};
    }
}

FF_Result FF_Invalid(const char* op, const char* why) noexcept
{
    return FF_Result{FF_INVALID_ARGUMENT, std::string(op) + ": " + why};
}

} // namespace

// =====================================================================
// MEMORY (ARENA) API
// =====================================================================
FF_Result FF_CreateMemory(const FF_MemoryCreateInfo& info, FF_Memory& out_memory) noexcept
{
    out_memory.reset();
    if (info.shm_name && info.filepath)
        return FF_Invalid("FF_CreateMemory", "shm_name and filepath are mutually exclusive");
    return FF_Guard("FF_CreateMemory", [&] {
        if (info.filepath)
            out_memory = std::make_shared<Memory>(Memory::createFromFile(info.filepath, info.capacity));
        else
            out_memory = std::make_shared<Memory>(Memory::create(info.capacity, info.shm_name ? info.shm_name : ""));
    });
}

FF_Result FF_MemoryReset(const FF_Memory& memory, Size committed_size) noexcept
{
    if (!memory)
        return FF_Invalid("FF_MemoryReset", "null memory handle");
    return FF_Guard("FF_MemoryReset", [&] { memory->reset(committed_size); });
}

Size FF_MemorySize(const FF_Memory& memory) noexcept
{
    return memory ? memory->size() : 0;
}

Size FF_MemoryCapacity(const FF_Memory& memory) noexcept
{
    return memory ? memory->capacity() : 0;
}

// =====================================================================
// STREAM API
// =====================================================================
FF_Result FF_CreateStream(const FF_StreamCreateInfo& info, FF_Stream& out_stream) noexcept
{
    out_stream.reset();
    const bool has_backing = info.filepath != nullptr || info.shm_name != nullptr;
    if (info.arena && has_backing)
        return FF_Invalid("FF_CreateStream", "arena is exclusive with filepath/shm_name");
    if (info.shm_name && info.filepath)
        return FF_Invalid("FF_CreateStream", "shm_name and filepath are mutually exclusive");
    return FF_Guard("FF_CreateStream", [&] {
        Memory arena;
        if (info.arena)
            arena = *info.arena;
        else if (info.filepath)
            arena = Memory::createFromFile(info.filepath, info.capacity);
        else if (info.shm_name)
            arena = Memory::create(info.capacity, info.shm_name);
        else
            arena = Memory::create(info.capacity);
        out_stream = std::make_shared<Builder>(arena, info.version);
    });
}

FF_Result FF_StreamSetRoot(const FF_StreamSetRootInfo& info) noexcept
{
    if (!info.stream)
        return FF_Invalid("FF_StreamSetRoot", "null stream handle");
    return FF_Guard("FF_StreamSetRoot", [&] { info.stream->set_root(info.root); });
}

FF_Result FF_StreamFinalize(const FF_StreamFinalizeInfo& info, Memory::View& out_view) noexcept
{
    out_view = Memory::View();
    if (!info.stream)
        return FF_Invalid("FF_StreamFinalize", "null stream handle");
    return FF_Guard("FF_StreamFinalize", [&] {
        out_view = info.stream->finalize(info.algorithm, info.hasher);
    });
}

FF_Result FF_StreamQuery(const FF_StreamQueryInfo& info, Parser& out_parser) noexcept
{
    out_parser = Parser();
    if (!info.stream)
        return FF_Invalid("FF_StreamQuery", "null stream handle");
    return FF_Guard("FF_StreamQuery", [&] { out_parser = info.stream->query(); });
}

// =====================================================================
// PARSE API
// =====================================================================
FF_Result FF_Parse(const FF_ParseInfo& info, Parser& out_parser) noexcept
{
    out_parser = Parser();
    if (!info.buffer && info.size != 0)
        return FF_Invalid("FF_Parse", "null buffer");
    return FF_Guard("FF_Parse", [&] { out_parser = Parser(info.buffer, info.size); });
}

// =====================================================================
// COMPACT API
// =====================================================================
FF_Result FF_Compact(const FF_CompactInfo& info, Memory::View& out_view) noexcept
{
    out_view = Memory::View();
    if (!info.source)
        return FF_Invalid("FF_Compact", "invalid source parser");
    return FF_Guard("FF_Compact", [&] {
        // The compacted stream is strictly smaller than the source, so the
        // source size is a safe arena bound; the archive allocates it.
        Memory destination = Memory::create(info.source.size());
        out_view = Compactor::archive(info.source, destination, info.algorithm, info.hasher);
    });
}

} // namespace FastFHIR
