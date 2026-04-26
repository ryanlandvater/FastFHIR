/**
 * @file FF_Ingestor.hpp
 * @author Ryan Landvater
 * @brief Concurrent clinical data ingestion engine for FastFHIR.
 */
#pragma once

#include "FF_Builder.hpp"
#include "FF_Logger.hpp"
#include <simdjson.h>
#include <string_view>
#include <vector>
#include <thread>
#include <atomic>
#include <string>

namespace FastFHIR::Ingest {

enum class SourceType {
    FHIR_JSON, // Standard FHIR JSON (R4/R5)
    HL7_V2,    // Pipe-delimited (ER7)
    HL7_V3     // XML-based (CDA)
};

struct IngestRequest {
    FastFHIR::Builder& builder;
    SourceType source_type;
    std::string_view json_string;
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
     * @note Runtime supports FF_FIELD_BLOCK targets and FF_FIELD_ARRAY targets whose entries
     *       are inline blocks (`array_entries_are_offsets == 0`). Offset-array insertion is
     *       rejected with FF_FAILURE until implemented.
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
