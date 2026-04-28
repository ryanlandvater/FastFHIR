// ============================================================
// Public ingest dispatcher
// ============================================================

// ============================================================
// FF_Ingestor.cpp
// Main Thread Ingestion Routing & Bundle Parsing
// ============================================================
#include "FF_Ingestor.hpp"
#include "../include/FF_Queue.hpp"
#include "../generated_src/FF_Bundle.hpp"
#include "../generated_src/FF_IngestMappings.hpp"
#include <cctype>
#include <thread>
#include <vector>

namespace FastFHIR::Ingest {

// =====================================================================
// INGEST WORK QUEUE
// Complex field values (blocks and arrays) are enqueued rather than
// dispatched inline, so the calling frame stays shallow and the queue
// is drained iteratively after the initial parse.
// =====================================================================

enum class IngestPendingKind {
    BlockField,   // FF_FIELD_BLOCK  → dispatch_block → parent[key] = child
    ArrayField,   // FF_FIELD_ARRAY  → dispatch_block(synthetic owner) → extract array → parent[key] = array
};

struct IngestPending {
    IngestPendingKind          kind     = IngestPendingKind::BlockField;
    Reflective::ObjectHandle   parent;                            // target parent object
    FF_FieldKey                key;                               // field within parent to write
    std::string                payload;                           // owned JSON for this value
    RECOVERY_TAG               recovery = FF_RECOVER_UNDEFINED;  // dispatch recovery tag
};

using IngestQueue = FIFO::Queue<IngestPending, 256>;

// =====================================================================
// BUNDLE WORK QUEUE
// Each bundle entry is represented as a lightweight view-plus-index task.
// The queue is fully populated before any worker starts (all slots are
// PENDING), so workers never spin waiting for items to be written.
// Workers each hold an independent Consumer; pop() uses a CAS on every
// slot so exactly one thread claims each task — no coordinator needed.
// =====================================================================
struct BundleTask {
    std::string_view payload;   // view into the original request JSON (zero-copy)
    size_t           index;     // position within the preallocated inline entry array
};

// CAPACITY=64 → up to 64 concurrent live nodes × 2000 entries/node = 128 000 entries.
using BundleQueue = FIFO::Queue<BundleTask, 64>;

struct IngestContext {
    Builder&                      builder;
    simdjson::ondemand::parser&   parser;
    IngestQueue                   queue;
    IngestQueue::Injector         injector;
    IngestQueue::Consumer         consumer;
    ConcurrentLogger*             logger;

    IngestContext(Builder& b, simdjson::ondemand::parser& p, ConcurrentLogger* lg)
        : builder(b), parser(p), queue(),
          injector(queue.get_injector()), consumer(queue.get_consumer()), logger(lg) {}
};

static void enqueue_ingest_pending(IngestContext& ctx, IngestPending item) {
    ctx.injector.push(item);
}

static FF_Result process_ingest_pending(IngestPending& pending, IngestContext& ctx) {
    simdjson::padded_string safe_json(pending.payload);
    try {
        if (pending.kind == IngestPendingKind::BlockField) {
            simdjson::ondemand::document doc = ctx.parser.iterate(safe_json);
            simdjson::ondemand::value    val = doc.get_value();
            Reflective::ObjectHandle child =
                dispatch_block(pending.recovery, val, ctx.builder, ctx.logger);
            if (child.offset() == FF_NULL_OFFSET)
                return FF_Result{FF_FAILURE,
                    std::string("IngestPending: failed to build block '") + pending.key.name + "'."};
            pending.parent[pending.key] = child;

        } else /* ArrayField */ {
            // INLINE OBJECT ARRAYS ONLY.
            // FastFHIR never uses offset-arrays for object fields. Every element is
            // written contiguously into the arena as an inline block at the time the
            // generated *_from_json() function runs. The resulting layout is:
            //
            //   [ FF_ARRAY header | elem[0] block | elem[1] block | ... ]
            //
            // We call dispatch_block() with the owner_recovery tag, which resolves to
            // the generated function for the *parent* type (e.g. Patient for Patient.telecom).
            // That function writes the full owner object including the packed array block
            // into the arena in one call. We then read back the array offset from the
            // owner's vtable slot and hand a typed ObjectHandle to the MutableEntry
            // assignment, which patches the pointer in the real parent.
            simdjson::ondemand::document doc  = ctx.parser.iterate(safe_json);
            simdjson::ondemand::value    val  = doc.get_value();
            Reflective::ObjectHandle owner =
                dispatch_block(pending.recovery, val, ctx.builder, ctx.logger);
            if (owner.offset() == FF_NULL_OFFSET)
                return FF_Result{FF_FAILURE,
                    std::string("IngestPending: failed to build array owner for field '") + pending.key.name + "'."};

            // Read the array block offset from the owner's vtable slot for this field.
            // key.field_offset is the byte distance from the owner block base to the
            // 8-byte pointer slot that holds the FF_ARRAY block offset.
            const BYTE* base      = ctx.builder.memory().base();
            Offset      slot_addr = owner.offset() + pending.key.field_offset;
            Offset      array_off = LOAD_U64(base + slot_addr);
            if (array_off == FF_NULL_OFFSET)
                return FF_Result{FF_FAILURE,
                    std::string("IngestPending: array payload parsed to null for field '") + pending.key.name + "'."};

            // Confirm the block at array_off is a genuine FF_ARRAY (RECOVER_ARRAY_BIT set).
            // This catches any mismatch between owner_recovery and the actual field.
            RECOVERY_TAG stored_tag = static_cast<RECOVERY_TAG>(
                LOAD_U16(base + array_off + DATA_BLOCK::RECOVERY));
            if ((stored_tag & RECOVER_ARRAY_BIT) == 0)
                return FF_Result{FF_FAILURE,
                    std::string("IngestPending: payload is not an FF_ARRAY block for field '") + pending.key.name + "'."};

            // Wrap the existing inline array block in an ObjectHandle using the
            // semantic element recovery tag (child_recovery), then assign it to
            // the parent's field slot via MutableEntry::operator=(ObjectHandle).
            Reflective::ObjectHandle array_handle(&ctx.builder, array_off, pending.key.child_recovery);
            pending.parent[pending.key] = array_handle;
        }
    } catch (const simdjson::simdjson_error& e) {
        return FF_Result{FF_FAILURE,
            std::string("IngestPending simdjson error for field '") + pending.key.name + "': " + e.what()};
    } catch (const std::exception& e) {
        return FF_Result{FF_FAILURE,
            std::string("IngestPending error for field '") + pending.key.name + "': " + e.what()};
    }
    return FF_SUCCESS;
}

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
        // INLINE OBJECTS: Bundle.entry elements are stored inline — not as an
        // array of offsets pointing to heap-allocated objects. Each BundleentryData
        // is a fixed-size struct; by pre-populating N default-constructed entries
        // here, the Builder's append_obj() call below writes a single contiguous
        // arena region:
        //
        //   [ FF_ARRAY header | BundleentryData[0] | BundleentryData[1] | ... ]
        //
        // Worker threads then patch fields *within* each already-allocated slot
        // using MutableEntry assignments — no secondary allocation or pointer
        // chasing for the array elements themselves.
        size_t count = task_payloads.size();
        pre_bundle.entry = std::vector<BundleentryData>(count);

        // =====================================================================
        // 3. WRITE THE PREALLOCATED BUNDLE TOP-DOWN
        // =====================================================================
        // Writes strings, the Bundle header block, and the fully-packed inline
        // entry array block to the arena in one append_obj() call.
        Reflective::ObjectHandle root_handle = m_builder.append_obj(pre_bundle);
        Offset bundle_offset = root_handle.offset();
        (void)bundle_offset;

        // =====================================================================
        // 4. LOCATE THE PREALLOCATED ARRAY BLOCK FOR PATCHING
        // =====================================================================
        // entry_array is an ObjectHandle pointing at the inline FF_ARRAY block.
        // Indexing it with operator[](size_t) returns a MutableEntry whose
        // parent_offset is the array block itself and whose vtable_offset is the
        // byte position of that element within the contiguous block.
        Reflective::ObjectHandle entry_array = root_handle[Fields::BUNDLE::ENTRY];

        // =====================================================================
        // 5. CONCURRENT ARRAY PATCHING — SHARED QUEUE / DYNAMIC LOAD BALANCING
        // =====================================================================
        // All N entry tasks are pushed into a single BundleQueue before any
        // worker is spawned. Workers race to pop() tasks via an atomic CAS on
        // every queue slot: the first thread to successfully CAS a slot from
        // PENDING → READING owns that entry; all other threads skip it and
        // advance to the next slot. This gives perfect dynamic load balancing
        // — faster threads naturally pull more work with zero coordination
        // overhead, and no thread ever idles while tasks remain in the queue.
        out_parsed_count = count;

        // Push all tasks before spawning workers so every slot is PENDING
        // (not WRITING) when consumers start, eliminating any spin-wait.
        BundleQueue bundle_queue;
        {
            auto injector = bundle_queue.get_injector();
            for (size_t i = 0; i < count; ++i) {
                injector.push(BundleTask{task_payloads[i], i});
            }
            // Injector destructs here; all slots are now PENDING.
        }

        std::vector<std::thread> workers;
        for (unsigned int i = 0; i < m_num_threads; ++i) {
            workers.emplace_back([this, i, &bundle_queue, entry_array, &m_builder]() mutable {
                auto& local_parser = m_parser_pool[i];

                // Each worker gets its own Consumer starting at the queue head.
                // pop() uses CAS so no two workers ever claim the same task.
                // Faster workers simply pop more entries — no static partition.
                auto consumer = bundle_queue.get_consumer();
                BundleTask task;

                while (!m_engine_faulted.load(std::memory_order_relaxed)) {
                    if (consumer.pop(task)) {
                        try {
                            simdjson::padded_string       local_pad(task.payload);
                            simdjson::ondemand::document  local_doc = local_parser.iterate(local_pad);
                            simdjson::ondemand::object    local_obj = local_doc.get_object();

                            // Locate the pre-allocated inline arena slot for this entry.
                            Reflective::MutableEntry entry_wrapper = entry_array[task.index];

                            // Write all entry fields into the existing inline block.
                            // All objects are written inline — no heap indirection.
                            Ingest::patch_Bundle_entry_from_json(local_obj, entry_wrapper, m_builder, &m_logger);

                        } catch (const simdjson::simdjson_error& e) {
                            m_engine_faulted.store(true, std::memory_order_release);
                            m_logger.log(std::string("[Fatal] Worker thread crashed on bundle entry ") +
                                         std::to_string(task.index) + " (simdjson): " + e.what());
                            break;
                        } catch (const std::exception& e) {
                            m_engine_faulted.store(true, std::memory_order_release);
                            m_logger.log(std::string("[Fatal] Worker thread crashed on bundle entry ") +
                                         std::to_string(task.index) + " (std::exception): " + e.what());
                            break;
                        } catch (...) {
                            m_engine_faulted.store(true, std::memory_order_release);
                            m_logger.log(std::string("[Fatal] Worker thread crashed on bundle entry ") +
                                         std::to_string(task.index) + " with unknown exception.");
                            break;
                        }
                        continue;
                    }
                    // Queue exhausted — all entries claimed by this or other workers.
                    if (consumer.at_end()) break;
                    // pop() returned false but queue not yet exhausted: another
                    // thread holds the next slot mid-claim. Spin briefly.
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

    // 2. Build an IngestContext and enqueue the work item rather than dispatching inline.
    //    The parent slot is recorded, and the actual JSON parse + builder write happens 
    //    during the queue drain below.
    IngestContext ctx(*parent_object.get_builder(), m_parser_pool[0], &m_logger);

    if (key.kind == FF_FIELD_ARRAY) {
        // Build the synthetic owner JSON that wraps the array payload.
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

        enqueue_ingest_pending(ctx, IngestPending{
            IngestPendingKind::ArrayField,
            parent_object,
            key,
            std::move(owner_json),
            key.owner_recovery,
        });
    } else {
        // Block field path.
        enqueue_ingest_pending(ctx, IngestPending{
            IngestPendingKind::BlockField,
            parent_object,
            key,
            std::string(payload),
            key.child_recovery,
        });
    }

    // 3. Drain the queue
    //    Each item records the MutableEntry slot (parent + key) and is updated
    //    once its JSON is parsed and the child block written to the arena.
    IngestPending pending;
    while (true) {
        if (ctx.consumer.pop(pending)) {
            FF_Result result = process_ingest_pending(pending, ctx);
            if (!result) return result;
            continue;
        }
        if (ctx.consumer.at_end()) break;
    }

    return FF_SUCCESS;
}
} // namespace FastFHIR::Ingest
