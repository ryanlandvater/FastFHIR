// ============================================================
// FF_Ingestor.cpp
// Main Thread Ingestion Routing & Bundle Parsing
// ============================================================
#include "FF_Ingestor.hpp"
#include "../generated_src/FF_IngestMappings.hpp"
#include "../generated_src/FF_Bundle.hpp"
#include <simdjson.h>
#include <iostream>
#include <thread>
#include <vector>

namespace FastFHIR::Ingest {

FF_Result Ingestor::ingest_fhir_json(const IngestRequest& request, ObjectHandle& out_root, size_t& out_parsed_count) {
    if (is_faulted()) return FF_Result{FF_FAILURE, "Engine is faulted. Cannot ingest."};

    try {
        auto& m_builder = request.builder;
        m_logger.log("[Info] FastFHIR: Allocating simdjson tape...");
        
        auto& parser = m_parser_pool[0];
        simdjson::padded_string json_data = simdjson::padded_string::load(request.payload);
        simdjson::ondemand::document doc = parser.iterate(json_data);
        simdjson::ondemand::object root_obj = doc.get_object();

        std::string_view root_type;
        if (root_obj["resourceType"].get_string().get(root_type) != simdjson::SUCCESS) {
            return FF_Result{FF_FAILURE, "Invalid FHIR JSON: Missing resourceType at root."};
        }

        // =====================================================================
        // FAST PATH: SINGLE RESOURCE (Non-Bundle)
        // =====================================================================
        if (root_type != "Bundle") {
            m_logger.log(std::string("[Info] FastFHIR: Ingesting single ") + std::string(root_type) + " resource...");
            
            // Route directly to the generated C++ mapping function on the main thread
            out_root = dispatch_resource(root_type, root_obj, m_builder, &m_logger);
            out_parsed_count = 1;
            
            return FF_SUCCESS;
        }

        // =====================================================================
        // CONCURRENT PATH: BUNDLE
        // =====================================================================
        m_logger.log("[Info] FastFHIR: Ingesting Bundle metadata...");

        BundleData bundle_data;
        std::vector<std::string_view> task_payloads;

        // =====================================================================
        // 1. FORWARD-ONLY METADATA EXTRACTION
        // =====================================================================
        for (auto field : root_obj) {
            std::string_view key = field.unescaped_key().value_unsafe();
            
            if (key == "id") {
                std::string_view val;
                if (field.value().get_string().get(val) == simdjson::SUCCESS) bundle_data.id = val;
            } 
            else if (key == "type") {
                std::string_view val;
                if (field.value().get_string().get(val) == simdjson::SUCCESS) bundle_data.type = FF_ParseBundleType(std::string(val)); 
            } 
            else if (key == "timestamp") {
                std::string_view val;
                if (field.value().get_string().get(val) == simdjson::SUCCESS) bundle_data.timestamp = val;
            } 
            else if (key == "total") {
                uint64_t val;
                if (field.value().get_uint64().get(val) == simdjson::SUCCESS) bundle_data.total = static_cast<uint32_t>(val);
            } 
            else if (key == "entry") {
                simdjson::ondemand::array entries;
                if (field.value().get_array().get(entries) == simdjson::SUCCESS) {
                    for (auto entry : entries) {
                        simdjson::ondemand::object entry_obj;
                        if (entry.get_object().get(entry_obj) == simdjson::SUCCESS) {
                            
                            // Extract the raw JSON string boundary for the worker threads to parse independently (zero-copy)
                            simdjson::ondemand::value res_val;
                            if (entry_obj["resource"].get(res_val) == simdjson::SUCCESS) {
                                task_payloads.push_back(res_val.raw_json_token());
                            }
                        }
                    }
                }
            }
        }

        // =====================================================================
        // 2. CONCURRENT BUNDLE THREADS PROCESSING
        // =====================================================================
        out_parsed_count = task_payloads.size();
        std::vector<ObjectHandle> results(out_parsed_count, ObjectHandle(&m_builder, FF_NULL_OFFSET));
        std::atomic<size_t> current_task{0};
        
        std::vector<std::thread> workers;
        workers.reserve(m_num_threads);

        for (unsigned int i = 0; i < m_num_threads; ++i) {
            workers.emplace_back([this, i, &task_payloads, &results, &current_task, &m_builder]() {
                auto& local_parser = m_parser_pool[i]; 
                
                while (!m_engine_faulted.load(std::memory_order_relaxed)) {
                    size_t task_idx = current_task.fetch_add(1, std::memory_order_relaxed);
                    if (task_idx >= task_payloads.size()) break;

                    try {
                        simdjson::padded_string local_pad(task_payloads[task_idx]);
                        simdjson::ondemand::document local_doc = local_parser.iterate(local_pad);
                        
                        std::string_view child_type;
                        if (local_doc["resourceType"].get(child_type) == simdjson::SUCCESS) {
                            results[task_idx] = dispatch_resource(child_type, local_doc.get_object(), m_builder, &m_logger);
                        }
                    } catch (const std::exception& e) {
                        m_engine_faulted.store(true, std::memory_order_release);
                        m_logger.log(std::string("[Fatal] Thread crashed on resource index ") + std::to_string(task_idx) + ": " + e.what());
                        break; 
                    }
                }
            });
        }

        for (auto& worker : workers) worker.join();

        if (m_engine_faulted.load(std::memory_order_acquire)) {
            return FF_Result{FF_FAILURE, "Ingestion aborted due to worker thread crash."};
        }

        // =====================================================================
        // 3. ROOT FINALIZATION
        // =====================================================================
        
        // Pass the pre-serialized worker results into the bypass vector
        bundle_data.entry_handles = std::move(results); 
        
        out_root = m_builder.append_obj(bundle_data);
        
        m_logger.log("[Success] FastFHIR: Bundle ingestion complete.");
        return FF_Result{FF_SUCCESS, "Bundle parsed successfully."};

    } catch (const simdjson::simdjson_error& e) {
        return FF_Result{FF_FAILURE, std::string("simdjson Exception: ") + e.what()};
    } catch (const std::exception& e) {
        return FF_Result{FF_FAILURE, std::string("Standard Exception: ") + e.what()};
    }
}

} // namespace FastFHIR::Ingest