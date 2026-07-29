/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * @file FF_Ingestor.hpp
 * @author Ryan Landvater
 * @brief Concurrent clinical data ingestion engine for FastFHIR.
 */
#pragma once

#include "FF_Builder.hpp"
#include "FF_Logger.hpp"
#include <cstdint>
#include <simdjson.h>
#include <string_view>
#include <vector>
#include <thread>
#include <atomic>
#include <string>

namespace FastFHIR::Ingest {

enum class FF_ExtensionFilterMode {
    FILTER_ALL_KNOWN,   // Suppress profile-native + HL7-known-safe (default)
    FILTER_NATIVE_ONLY, // Suppress only profile-native extensions
    FILTER_NONE,        // Store all extensions; dispatch everything to WASM
};

// =====================================================================
// PREDIGESTION
// Single-threaded consumer in a producer/consumer pipeline.
// Producers (per-thread) scan prechunked entries and push URLs into an
// MPSC queue; consumer drains the queue, deduplicates with a direct-mapped
// cache, builds a per-'/' radix trie, and writes FF_URL_DIRECTORY to arena.
// Discovered URLs are interned directly into the Builder's URL registry.
// =====================================================================
void FF_PredigestExtensionURLs(
    const std::vector<simdjson::padded_string>& prechunked_entries,
    Builder&                builder,
    FF_ExtensionFilterMode  mode = FF_ExtensionFilterMode::FILTER_ALL_KNOWN);

enum class SourceType {
    FHIR_JSON, // Standard FHIR JSON (R4/R5)
    HL7_V2,    // Pipe-delimited (ER7)
    HL7_V3     // XML-based (CDA)
};

struct IngestRequest {
    FastFHIR::Builder& builder;
    SourceType source_type;
    std::string_view json_string;

    /**
     * @brief Readable bytes at json_string.data(), including slack past json_string.size().
     *
     * simdjson reads up to simdjson::SIMDJSON_PADDING bytes past the logical end of a
     * document, so it needs that slack to exist. Set this to the real allocated size of
     * your buffer and the payload is parsed **in place, with no copy**:
     *
     *   simdjson::padded_string doc = simdjson::padded_string::load(path);
     *   IngestRequest req{builder, SourceType::FHIR_JSON,
     *                     std::string_view(doc.data(), doc.length()),
     *                     doc.length() + simdjson::SIMDJSON_PADDING};
     *
     * A std::string works too, since its buffer is contiguous:
     *   req.payload_capacity = s.capacity() + 1;   // only if capacity() - size() >= PADDING
     *
     * Left at 0 (the default) the ingestor makes one padded copy of the payload. That is
     * always safe, so existing callers keep working unchanged — it just costs a memcpy of
     * the whole document.
     */
    size_t payload_capacity = 0;
};

class Ingestor {
    ConcurrentLogger m_logger;
    std::vector<simdjson::ondemand::parser> m_parser_pool;
    unsigned int m_num_threads;
    
    std::atomic<bool> m_engine_faulted{false}; 

public:
    /**
     * @brief Initializes the FastFHIR Ingestor.
     * @param log_capacity Maximum bytes for the lock-free warning buffer.
     * @param concurrency Number of worker threads. Default (0) is replaced by hardware concurrency.
     */
    explicit Ingestor(size_t logger_byte_capacity = 64 * 1024 * 1024, unsigned int concurrency = 0) 
        : m_logger(logger_byte_capacity) 
    {
        m_num_threads = concurrency > 0 ? concurrency : std::thread::hardware_concurrency();
        if (m_num_threads == 0) m_num_threads = 4; // Absolute fallback
        
        m_parser_pool.resize(m_num_threads);
    }

    /**
     * @brief Main entry point for ingesting clinical data into a FastFHIR stream.
     * @param request Ingestion parameters including source type and payload.
     * @return Result code and message indicating success or failure details.
     */
    FF_Result ingest(const IngestRequest& request, Reflective::ObjectHandle& out_root, size_t& out_parsed_count);

    /**
     * @brief Parses a payload and inserts the resulting object at a specific field.
     * @param parent_object The mutable handle to the specific resource being amended.
     * @param key The field token for the field within parent_object being amended.
     * @param payload The raw string to parse.
     * @note Runtime supports FF_FIELD_BLOCK and FF_FIELD_ARRAY targets. Array element
     *       layout is irrelevant here: the whole array block is written by the generated
     *       *_from_json and only its offset is patched into the parent slot, and readers
     *       re-derive the element layout from the FF_ARRAY header on the wire.
     */
    FF_Result insert_at_field(Reflective::ObjectHandle& parent_object, const FF_FieldKey& key, std::string_view payload, SourceType fmt = SourceType::FHIR_JSON);

    /**
     * @brief Resets the engine state for a new file and returns all accumulated logs.
     * @return A string containing all warnings and errors from the previous run.
     */
    std::string reset() {
        std::string final_logs = m_logger.to_string();
        m_logger.clear(); 
        m_engine_faulted.store(false, std::memory_order_release);
        return final_logs;
    }

    bool is_faulted() const { return m_engine_faulted.load(std::memory_order_acquire); }
    const ConcurrentLogger& get_logger() const { return m_logger; }

private:
    FF_Result ingest_fhir_json(const IngestRequest& request, Reflective::ObjectHandle& out_root, size_t& out_parsed_count);
    FF_Result insert_at_field_json(Reflective::ObjectHandle& parent_object, const FF_FieldKey& key, std::string_view payload);
};

} // namespace FastFHIR::Ingest
