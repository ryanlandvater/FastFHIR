// ============================================================
// Public ingest dispatcher
// ============================================================

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

FF_Result Ingestor::ingest(const IngestRequest& request, ObjectHandle& out_root, size_t& out_parsed_count)
{
    switch (request.source_type) {
        case SourceType::FHIR_JSON:
            return ingest_fhir_json(request, out_root, out_parsed_count);
        case SourceType::HL7_V2:
            return FF_Result{FF_FAILURE, "HL7 v2 ingestion not implemented."};
        case SourceType::HL7_V3:
            return FF_Result{FF_FAILURE, "HL7 v3 ingestion not implemented."};
        default:
            return FF_Result{FF_FAILURE, "Unknown source type."};
    }
}

FF_Result Ingestor::insert_at_field(ObjectHandle& parent_object, const FF_FieldKey& key, std::string_view payload, SourceType fmt)
{
    switch (fmt) {
        case SourceType::FHIR_JSON:
            return insert_at_field_json(parent_object, key, payload);
        case SourceType::HL7_V2:
            return FF_Result{FF_FAILURE, "HL7 v2 field ingestion not implemented."};
        case SourceType::HL7_V3:
            return FF_Result{FF_FAILURE, "HL7 v3 field ingestion not implemented."};
        default:
            return FF_Result{FF_FAILURE, "Unknown source type."};
    }
} 

FF_Result Ingestor::ingest_fhir_json(const IngestRequest& request, ObjectHandle& out_root, size_t& out_parsed_count) {
    if (is_faulted()) return FF_Result{FF_FAILURE, "Engine is faulted. Reset() before ingesting new data."};

    try {
        auto& m_builder = request.builder;
        m_logger.log("[Info] FastFHIR: Allocating simdjson tape...");
        
        // Create a local parser instance for the main thread to handle the initial routing and metadata extraction
        auto& parser = m_parser_pool[0];

        // Parse the root JSON object to determine if it's a Bundle or a single resource
        simdjson::ondemand::document doc = parser.iterate(
            request.json_string.data(), 
            request.json_string.size(), 
            request.json_string.size() + simdjson::SIMDJSON_PADDING
        );
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
            if (out_root.offset() == FF_NULL_OFFSET) 
                return FF_Result{FF_FAILURE, "Failed to parse root resource."};
            out_parsed_count = 1;
            
            return FF_SUCCESS;
        }

        // =====================================================================
        // CONCURRENT PATH: BUNDLE (Top-Down)
        // =====================================================================
        m_logger.log("[Info] FastFHIR: Allocating Top-Down Bundle structure...");
        root_obj.reset(); 
        std::vector<std::string_view> task_payloads;

        // =====================================================================
        // 1. EXTRACT DATA & QUEUE TASKS
        // =====================================================================
        // This single line parses all metadata AND slices the entry array into our task vector.
        BundleData pre_bundle = Bundle_from_json(root_obj, &m_logger, &task_payloads);
        
        // =====================================================================
        // 2. PREPARE THE PREALLOCATED ARRAY
        // =====================================================================
        // Force the Builder to allocate exactly N inline 82-byte structs!
        size_t count = task_payloads.size();
        pre_bundle.entry = std::vector<BundleentryData>(count);

        // =====================================================================
        // 3. WRITE THE PREALLOCATED BUNDLE TOP-DOWN
        // =====================================================================
        // This safely writes the strings, the header, and the perfectly packed array block to mmap.
        ObjectHandle root_handle = m_builder.append_obj(pre_bundle);
        Offset bundle_offset = root_handle.offset();

        // =====================================================================
        // 4. LOCATE THE PREALLOCATED ARRAY BLOCK FOR PATCHING
        // =====================================================================
        // Safely retrieve the proxy using the generated reflection key
        ObjectHandle entry_array = root_handle[Fields::BUNDLE::ENTRY];

        // =====================================================================
        // 5. CONCURRENT ARRAY PATCHING
        // =====================================================================
        out_parsed_count = count;
        std::atomic<size_t> current_task{0};
        std::vector<std::thread> workers;
        
        for (unsigned int i = 0; i < m_num_threads; ++i) {
            workers.emplace_back([this, i, &task_payloads, entry_array, &current_task, &m_builder]() mutable {
                auto& local_parser = m_parser_pool[i];
                
                while (!m_engine_faulted.load(std::memory_order_relaxed)) {
                    size_t task_idx = current_task.fetch_add(1, std::memory_order_relaxed);
                    if (task_idx >= task_payloads.size()) break;

                    try {
                        simdjson::padded_string local_pad(task_payloads[task_idx]);
                        simdjson::ondemand::document local_doc = local_parser.iterate(local_pad);
                        simdjson::ondemand::object local_obj = local_doc.get_object();
                        
                        // 1. Get the handle to the absolute memory offset of this specific array slot
                        MutableEntry entry_wrapper = entry_array[task_idx];

                        // 2. Pass the JSON tape and the wrapper handle directly to the auto-generated patcher
                        Ingest::patch_Bundle_entry_from_json(local_obj, entry_wrapper, m_builder, &m_logger);
                            
                    } catch (const simdjson::simdjson_error& e) {
                        m_engine_faulted.store(true, std::memory_order_release);
                        m_logger.log(std::string("[Fatal] Worker thread crashed on resource index ") + 
                                     std::to_string(task_idx) + " (simdjson): " + e.what());
                        break;
                    } catch (const std::exception& e) {
                        m_engine_faulted.store(true, std::memory_order_release);
                        m_logger.log(std::string("[Fatal] Worker thread crashed on resource index ") + 
                                     std::to_string(task_idx) + " (std::exception): " + e.what());
                        break;
                    } catch (...) {
                        m_engine_faulted.store(true, std::memory_order_release);
                        m_logger.log(std::string("[Fatal] Worker thread crashed on resource index ") + 
                                     std::to_string(task_idx) + " with an unknown exception.");
                        break;
                    }
                }
            });
        }

        for (auto& worker : workers) worker.join();

        // Check if the engine faulted during the worker runs
        if (m_engine_faulted.load(std::memory_order_acquire)) {
            return FF_Result{FF_FAILURE, "Ingestion aborted due to worker thread crash. Check ingestor engine logs for error details."};
        }

        // =====================================================================
        // 6. Return Root
        // =====================================================================
        out_root = root_handle;
        return FF_SUCCESS;

    } catch (const simdjson::simdjson_error& e) {
        return FF_Result{FF_FAILURE, std::string("simdjson Exception: ") + e.what()};
    } catch (const std::exception& e) {
        return FF_Result{FF_FAILURE, std::string("Standard Exception: ") + e.what()};
    }
}


// =====================================================================
// FIELD-LEVEL JSON ENGINE
// =====================================================================
FF_Result Ingestor::insert_at_field_json(ObjectHandle& parent_object, const FF_FieldKey& key, std::string_view payload) {
    if (is_faulted()) return FF_Result{FF_FAILURE, "Engine is faulted. Reset() before ingesting new data."};

    // 1. Prevent scalar waste (Bounds check eliminated by using FF_FieldKey directly)
    if (key.kind != FF_FIELD_BLOCK && key.kind != FF_FIELD_ARRAY)
        return FF_Result{FF_FAILURE, "insert_at_field is strictly for Objects/Arrays. Use Builder.append for scalars like " + std::string(key.name)};

    try {
        auto& parser = m_parser_pool[0];
        
        // STRICT SAFETY: Ensure padding for AVX-512 instructions
        simdjson::padded_string safe_json(payload);
        simdjson::ondemand::document doc = parser.iterate(safe_json);
        simdjson::ondemand::value json_val = doc.get_value();

        // 2. Dispatch to the generated memory allocator based on the EXPECTED child tag
        // We use the new get_builder() accessor to pass the memory arena context
        ObjectHandle child_handle = dispatch_block(key.child_recovery, json_val, *parent_object.get_builder(), &m_logger);
        
        if (child_handle.offset() == FF_NULL_OFFSET)
            return FF_Result{FF_FAILURE, "Failed to parse child JSON payload into FastFHIR::Memory arena."};

        // 3. Link the newly written block to the parent using the thread-safe PointerPatchProxy
        parent_object[key] = child_handle;

        return FF_SUCCESS;

    } catch (const simdjson::simdjson_error& e) {
        return FF_Result{FF_FAILURE, std::string("simdjson Exception during field insert: ") + e.what()};
    } catch (const std::exception& e) {
        return FF_Result{FF_FAILURE, std::string("Standard Exception during field insert: ") + e.what()};
    }
}
} // namespace FastFHIR::Ingest
