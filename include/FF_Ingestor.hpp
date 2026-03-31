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
     * @param concurrency Number of worker threads. Defaults to hardware concurrency.
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
    FF_Result ingest(const IngestRequest& request, ObjectHandle& out_root, size_t& out_parsed_count);

    /**
     * @brief Parses a payload and inserts the resulting complex object at a specific Field token.
     * @param parent_builder The builder containing the parent object frame.
     * @param registry_index The O(1) field token from FastFHIR::FieldKeys::Registry.
     * @param payload The raw string to parse.
     * @return Result code indicating success or failure.
     */
    FF_Result insert_at_field(Builder& parent_builder, uint32_t registry_index, std::string_view payload);

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
    FF_Result ingest_fhir_json(const IngestRequest& request, ObjectHandle& out_root, size_t& out_parsed_count);
};

} // namespace FastFHIR::Ingest