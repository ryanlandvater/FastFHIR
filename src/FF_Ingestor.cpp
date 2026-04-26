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
#include <cctype>
#include <iostream>
#include <thread>
#include <vector>

namespace FastFHIR::Ingest {

FF_Result Ingestor::ingest(const IngestRequest& request, Reflective::ObjectHandle& out_root, size_t& out_parsed_count)
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

FF_Result Ingestor::insert_at_field(Reflective::ObjectHandle& parent_object, const FF_FieldKey& key, std::string_view payload, SourceType fmt)
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

FF_Result Ingestor::ingest_fhir_json(const IngestRequest& request, Reflective::ObjectHandle& out_root, size_t& out_parsed_count) {
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
        Reflective::ObjectHandle root_handle = m_builder.append_obj(pre_bundle);
        Offset bundle_offset = root_handle.offset();

        // =====================================================================
        // 4. LOCATE THE PREALLOCATED ARRAY BLOCK FOR PATCHING
        // =====================================================================
        // Safely retrieve the proxy using the generated reflection key
        Reflective::ObjectHandle entry_array = root_handle[Fields::BUNDLE::ENTRY];

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
                        Reflective::MutableEntry entry_wrapper = entry_array[task_idx];

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
FF_Result Ingestor::insert_at_field_json(Reflective::ObjectHandle& parent_object, const FF_FieldKey& key, std::string_view payload) {
    if (is_faulted()) return FF_Result{FF_FAILURE, "Engine is faulted. Reset() before ingesting new data."};

    // 1. Prevent unsupported writes up front.
    if (key.kind == FF_FIELD_ARRAY) {
        if (key.owner_recovery == FF_RECOVER_UNDEFINED) {
            return FF_Result{FF_FAILURE,
                std::string("insert_at_field array target '") + key.name +
                "' requires a typed FF_FieldKey (owner recovery is undefined)."};
        }
        if (key.array_entries_are_offsets != 0) {
            return FF_Result{FF_FAILURE,
                std::string("insert_at_field does not support offset-array field '") + key.name +
                "' yet. Only inline-block arrays are supported."};
        }
    }
    if (key.kind != FF_FIELD_BLOCK) {
        if (key.kind == FF_FIELD_ARRAY) {
            // Handled below.
        } else {
        return FF_Result{FF_FAILURE,
            "insert_at_field currently supports only object/block fields. Use direct assignment for scalars like " +
            std::string(key.name)};
        }
    }

    try {
        auto& parser = m_parser_pool[0];
        
        // STRICT SAFETY: Ensure padding for AVX-512 instructions
        simdjson::padded_string safe_json(payload);
        simdjson::ondemand::document doc = parser.iterate(safe_json);
        simdjson::ondemand::value json_val = doc.get_value();

        // 2. Build and link according to target field kind.
        if (key.kind == FF_FIELD_ARRAY) {
            // Parse as a single item or an already-formed JSON array.
            bool is_array_payload = false;
            for (char ch : payload) {
                if (!std::isspace(static_cast<unsigned char>(ch))) {
                    is_array_payload = (ch == '[');
                    break;
                }
            }

            std::string owner_json;
            owner_json.reserve(key.name_len + payload.size() + 16);
            owner_json += "{\"";
            owner_json.append(key.name, key.name_len);
            owner_json += "\":";
            if (is_array_payload) {
                owner_json.append(payload.data(), payload.size());
            } else {
                owner_json += "[";
                owner_json.append(payload.data(), payload.size());
                owner_json += "]";
            }
            owner_json += "}";

            simdjson::padded_string owner_safe_json(owner_json);
            simdjson::ondemand::document owner_doc = parser.iterate(owner_safe_json);
            simdjson::ondemand::value owner_val = owner_doc.get_value();

            Reflective::ObjectHandle owner_handle =
                dispatch_block(key.owner_recovery, owner_val, *parent_object.get_builder(), &m_logger);
            if (owner_handle.offset() == FF_NULL_OFFSET) {
                return FF_Result{FF_FAILURE,
                    std::string("Failed to build inline array payload for field '") + key.name + "'."};
            }

            const BYTE* base = parent_object.get_builder()->memory().base();
            Offset slot_addr = owner_handle.offset() + key.field_offset;
            Offset array_offset = LOAD_U64(base + slot_addr);

            if (array_offset == FF_NULL_OFFSET) {
                return FF_Result{FF_FAILURE,
                    std::string("Array payload parsed to null for field '") + key.name + "'."};
            }

            RECOVERY_TAG stored_tag = static_cast<RECOVERY_TAG>(
                LOAD_U16(base + array_offset + DATA_BLOCK::RECOVERY));
            if ((stored_tag & RECOVER_ARRAY_BIT) == 0) {
                return FF_Result{FF_FAILURE,
                    std::string("Internal array insert error for field '") + key.name +
                    "': generated payload is not an FF_ARRAY block."};
            }

            // MutableEntry validates against child_recovery, so use semantic element tag here.
            Reflective::ObjectHandle array_handle(
                parent_object.get_builder(), array_offset, key.child_recovery);
            parent_object[key] = array_handle;
        } else {
            // Block field path.
            Reflective::ObjectHandle child_handle =
                dispatch_block(key.child_recovery, json_val, *parent_object.get_builder(), &m_logger);

            if (child_handle.offset() == FF_NULL_OFFSET)
                return FF_Result{FF_FAILURE, "Failed to parse child JSON payload into FastFHIR::Memory arena."};

            parent_object[key] = child_handle;
        }

        return FF_SUCCESS;

    } catch (const simdjson::simdjson_error& e) {
        return FF_Result{FF_FAILURE, std::string("simdjson Exception during field insert: ") + e.what()};
    } catch (const std::exception& e) {
        return FF_Result{FF_FAILURE, std::string("Standard Exception during field insert: ") + e.what()};
    }
}
} // namespace FastFHIR::Ingest
