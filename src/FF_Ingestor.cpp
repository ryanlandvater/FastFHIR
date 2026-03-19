// ============================================================================
// FF_Ingestor.cpp
// Implementation of the FastFHIR concurrent ingestion engine.
// ============================================================================

#include "FF_Ingestor.hpp"
#include "../generated_src/FF_IngestMappings.hpp"
#include "../generated_src/FF_Bundle.hpp" // Required for BundleData
#include <stdexcept>
#include <string>

namespace FastFHIR::Ingest {

FF_Result Ingestor::ingest(const IngestRequest& request, ObjectHandle& out_root, size_t& out_parsed_count) {
    // 1. Poison Pill Check
    if (m_engine_faulted.load(std::memory_order_acquire)) {
        return FF_Result{FF_FAILURE, "FastFHIR Ingestor is in a faulted state. Call reset() before reuse."};
    }

    // 2. Log Run Initiation
    m_logger.log("[Info] Starting FastFHIR ingestion run...");

    FF_Result result{FF_FAILURE, "Unknown Error"};

    // 3. Route to the appropriate format engine
    switch (request.source_type) {
        case SourceType::FHIR_JSON:
            result = ingest_fhir_json(request, out_root, out_parsed_count);
            break;
        case SourceType::HL7_V2:
            throw std::runtime_error("HL7 V2 ingestion is not yet implemented.");
        case SourceType::HL7_V3:
            throw std::runtime_error("HL7 V3 ingestion is not yet implemented.");
        default:
            result = FF_Result{FF_FAILURE, "Invalid SourceType provided."};
    }

    // 4. Log Run Completion or Failure
    if (result == FF_SUCCESS) {
        m_logger.log("[Info] Ingestion run completed successfully. Parsed " + std::to_string(out_parsed_count) + " resources.");
    } else {
        m_logger.log("[Error] Ingestion run failed: " + result.message);
    }

    return result;
}

FF_Result Ingestor::ingest_fhir_json(const IngestRequest& request, ObjectHandle& out_root, size_t& out_parsed_count) {
    try {
        // Pad the string for simdjson's SIMD requirements
        simdjson::padded_string padded_json(request.payload);
        simdjson::ondemand::document doc = m_parser_pool[0].iterate(padded_json);

        // Identify the root FHIR resource
        std::string_view resource_type;
        if (doc["resourceType"].get(resource_type) != simdjson::SUCCESS) {
            m_engine_faulted.store(true, std::memory_order_release);
            return FF_Result{FF_FAILURE, "Invalid FHIR JSON: Missing resourceType at root."};
        }

        // --- Fast Path: Single Resource ---
        if (resource_type != "Bundle") {
            out_root = dispatch_resource(resource_type, doc.get_object(), request.builder, &m_logger);
            out_parsed_count = 1;
            return FF_SUCCESS;
        }

        // --- Concurrent Path: Bundle Gang Processing ---
        std::vector<std::string_view> task_payloads;
        
        // Extract raw JSON boundaries for each child resource
        for (simdjson::ondemand::object entry : doc["entry"]) {
            task_payloads.push_back(entry["resource"].raw_json());
        }

        out_parsed_count = task_payloads.size();
        
        // Pre-allocate the lock-free array for worker threads to write handles into
        std::vector<ObjectHandle> results(out_parsed_count, ObjectHandle(&request.builder, FF_NULL_OFFSET));
        std::atomic<size_t> current_task{0};
        
        std::vector<std::thread> workers;
        workers.reserve(m_num_threads);

        // Spin up the thread gang
        for (unsigned int i = 0; i < m_num_threads; ++i) {
            workers.emplace_back([this, i, &task_payloads, &results, &current_task, &request]() {
                auto& local_parser = m_parser_pool[i]; 
                
                // Keep pulling tasks until the array is empty or the engine faults
                while (!m_engine_faulted.load(std::memory_order_relaxed)) {
                    // O(1) Lock-free task claim
                    size_t task_idx = current_task.fetch_add(1, std::memory_order_relaxed);
                    if (task_idx >= task_payloads.size()) break;

                    try {
                        simdjson::padded_string local_pad(task_payloads[task_idx]);
                        simdjson::ondemand::document local_doc = local_parser.iterate(local_pad);
                        
                        std::string_view child_type;
                        if (local_doc["resourceType"].get(child_type) == simdjson::SUCCESS) {
                            // Route dynamic JSON to the generated C++ POD struct and serialize it
                            results[task_idx] = dispatch_resource(child_type, local_doc.get_object(), request.builder, &m_logger);
                        }
                    } catch (const std::exception& e) {
                        // Poison the engine and record exactly which resource killed the thread
                        m_engine_faulted.store(true, std::memory_order_release);
                        m_logger.log(std::string("[Fatal] Thread crashed on resource index ") + std::to_string(task_idx) + ": " + e.what());
                        break; 
                    }
                }
            });
        }

        // Wait for the gang to finish processing
        for (auto& worker : workers) worker.join();

        // Abort assembly if a thread poisoned the pool
        if (m_engine_faulted.load(std::memory_order_acquire)) {
            return FF_Result{FF_FAILURE, "Ingestion aborted due to worker thread crash. Check logs."};
        }

        // --- Assembly ---
        BundleData root_bundle;
        // TODO: Map root_bundle.type, root_bundle.timestamp, etc. from the `doc` object here
        
        out_root = request.builder.append_obj(root_bundle);

        // Lock-free pointer patch to link the child resources to the parent Bundle
        for (const auto& child_handle : results) {
            if (child_handle.offset() != FF_NULL_OFFSET) {
                out_root["entry"] = child_handle;
            }
        }

        return FF_SUCCESS;

    } catch (const simdjson::simdjson_error& e) {
        m_engine_faulted.store(true, std::memory_order_release);
        m_logger.log(std::string("[Fatal] simdjson parsing error: ") + e.what());
        return FF_Result{FF_FAILURE, e.what()};
    } catch (const std::exception& e) {
        m_engine_faulted.store(true, std::memory_order_release);
        m_logger.log(std::string("[Fatal] Unexpected engine exception: ") + e.what());
        return FF_Result{FF_FAILURE, e.what()};
    }
}

} // namespace FastFHIR::Ingest