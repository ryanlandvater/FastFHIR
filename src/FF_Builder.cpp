/**
 * @file FF_Builder.cpp
 * @author Ryan Landvater (ryanlandvater[at]gmail[dot]com)
 * @brief Concurrent lock-free FastFHIR stream builder — implementation.
 * @version 0.1
 * @date 2026-03-18
 * @copyright Copyright (c) 2026 Ryan Landvater. All rights reserved.
 * @remark FastFHIR Shared Source License (FF-SSL) — see LICENSE file in the project root for terms.
 */

#include "FF_Utilities.hpp"
#include "FF_Builder.hpp"
#include "../generated_src/FF_Reflection.hpp"
#include <atomic>
#include <stdexcept>
#include <thread>
#include <string>
#include <iostream>

// OS-Specific Virtual Memory Headers
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace FastFHIR {
// =====================================================================
// PointerPatchProxy Implementation
// =====================================================================

PointerPatchProxy& PointerPatchProxy::operator=(const ObjectHandle& child)
{
    // --- STRICT SCHEMA VALIDATION ---
    if (m_ref.target_recovery == RECOVER_FF_RESOURCE) {
        // Polymorphic field: Ensure the child is actually a top-level Resource
        if (!FF_IsResourceTag(child.recovery()))
            throw std::invalid_argument("FastFHIR Schema Violation: Expected a top-level Resource (0x0200 range), but received a different block type.");
    } else if (child.recovery() != m_ref.target_recovery)
        // Strictly typed field: Ensure an exact match
        throw std::invalid_argument("FastFHIR Schema Violation: Attempted to assign an incompatible type.");

    m_builder->amend_pointer(m_ref.object_offset, m_ref.field_vtable_offset, child.offset());
    return *this;
}

void PointerPatchProxy::validate_assignment(RECOVERY_TAG child_tag) const
{
    if (m_ref.target_recovery == RECOVER_FF_RESOURCE) {
        // Polymorphic field: Ensure the child is actually a top-level Resource
        if (!FF_IsResourceTag(child_tag)) {
            throw std::invalid_argument("FastFHIR Schema Violation: Expected a top-level Resource (0x0200 range).");
        }
        // Strictly typed field: Ensure an exact match
    } else if (child_tag != m_ref.target_recovery)
        throw std::invalid_argument("FastFHIR Schema Violation: Attempted to assign an incompatible type.");
}

// =====================================================================
// Constructor / Destructor
// =====================================================================

Builder::Builder(const Memory& memory, uint16_t fhir_revision)
: m_memory(memory),
m_base(memory.base()),
m_checksum_offset(FF_NULL_OFFSET),
m_root_offset(FF_NULL_OFFSET),
m_root_recovery(FF_RECOVER_UNDEFINED),
m_fhir_rev(fhir_revision),
m_ff_version(FF_VERSION_MAJOR<<16|FF_VERSION_MINOR),
m_finalizing(false),
m_active_mutators(0)
{
    if (!m_memory) {
        throw std::invalid_argument("FastFHIR: Cannot initialize Builder with a null FF_Memory handle.");
    }
}

Builder::~Builder() = default; // m_memory handles its own OS cleanup

// =====================================================================
// Concurrency Guards
// =====================================================================

bool Builder::try_begin_mutation()
{
    if (m_finalizing.load(std::memory_order_acquire))
        return false;

    m_active_mutators.fetch_add(1, std::memory_order_acq_rel);

    // Close race where finalize starts after first check but before increment.
    if (m_finalizing.load(std::memory_order_acquire)) {
        m_active_mutators.fetch_sub(1, std::memory_order_acq_rel);
        return false;
    }

    return true;
}

void Builder::end_mutation()
{
    m_active_mutators.fetch_sub(1, std::memory_order_acq_rel);
}

// =====================================================================
// View Node & Amend Pointer
// =====================================================================

Node Builder::view_node(Offset offset, RECOVERY_TAG recovery, FF_FieldKind kind) const
{
    // 1. Snapshot the atomic boundary once
    Size size = m_memory.size();

    if (offset == FF_NULL_OFFSET || offset >= size)
        return Node();

    // 2. Use the exact same boundary for the Node's validation
    return Node(m_base, size, m_fhir_rev, offset, recovery, kind);
}

void Builder::amend_pointer(Offset object_offset, size_t field_vtable_offset, Offset new_target_offset)
{
    // Mutate the stream. Increment mutators if possible and store auto decrementor (guard) for autodestruction
    if (!try_begin_mutation())
        throw std::runtime_error("FastFHIR: Builder is finalizing; amend is no longer allowed.");
    struct MutationGuard {
        Builder *self;
        ~MutationGuard() { self->end_mutation(); }
    } guard{this};

    // Bounds checks
    size_t capacity = m_memory.capacity();
    if (object_offset > capacity || field_vtable_offset > (capacity - object_offset) ||
        sizeof(Offset) > (capacity - object_offset - field_vtable_offset))
        throw std::runtime_error("FastFHIR: Pointer amendment out of bounds.");

    // SAFELY load the current value to check if it has already been assigned
    Offset current_val = LOAD_U64(m_base + object_offset + field_vtable_offset);
    
    // NOTE: I don't like this. It's not concurrency protected and limits functionality
    if (current_val != FF_NULL_OFFSET) {
        std::string msg = "FastFHIR: Pointer amendment failed — attempted to insert offset " + std::to_string(new_target_offset) +
        " into object at offset " + std::to_string(object_offset) + ". This field was already assigned. " +
        "Attempting to patch an already-assigned pointer risks orphaning elements of the stream.";
        throw std::runtime_error(msg);
    }
    
    // SAFELY write the new offset without triggering hardware alignment traps
    // Note: Cast away constness if m_base remains const BYTE* in the header
    STORE_U64(const_cast<BYTE*>(m_base) + object_offset + field_vtable_offset, new_target_offset);
}

// =====================================================================
// ObjectHandle from Array Index Lookup
// =====================================================================

ObjectHandle PointerPatchProxy::operator[](size_t index) const
{
    // 1. Read the ABSOLUTE offset stored in the parent object's vtable
    Offset abs_array_off = LOAD_U64(m_builder->m_base + m_ref.object_offset + m_ref.field_vtable_offset);
    if (abs_array_off == FF_NULL_OFFSET)
        throw std::runtime_error("FastFHIR PointerPatchProxy: Cannot index into an unallocated array.");

    // 2. Instantiate the FF_ARRAY view object using the absolute offset
    FF_ARRAY array_block(abs_array_off, m_builder->m_memory.size(), m_builder->m_fhir_rev);

    // --- STRICT SAFETY: Verify it is actually an array ---
    if (array_block.validate_full(m_builder->m_base).code != FF_SUCCESS)
        throw std::runtime_error("FastFHIR PointerPatchProxy reports SCHEMA VIOLATION. Memory block at offset is not a valid FF_ARRAY.");

    // --- STRICT SAFETY: Prevent Buffer Overflows ---
    uint32_t count = array_block.entry_count(m_builder->m_base);
    if (index >= count)
        throw std::out_of_range("FastFHIR PointerPatchProxy: Array index out of bounds. Attempted to access index " +
                                std::to_string(index) + " in an array of size " + std::to_string(count) + ".");

    // 3. Use the encapsulated methods to find the data
    uint16_t step = array_block.entry_step(m_builder->m_base);
    const BYTE* entries_ptr = array_block.entries(m_builder->m_base);
    
    // Convert the raw memory pointer back into an absolute arena Offset
    Offset item_addr = static_cast<Offset>(entries_ptr - m_builder->m_base) + (index * step);

    // Array of entries or an array of offsets to entries?
    if (step == sizeof(Offset)) {
        Offset actual_object_off = LOAD_U64(m_builder->m_base + item_addr);
        return ObjectHandle(m_builder, actual_object_off, m_ref.target_recovery);
    } else {
        return ObjectHandle(m_builder, item_addr, m_ref.target_recovery);
    }
}

// =====================================================================
// ObjectHandle String Lookup (Reflection)
// =====================================================================

PointerPatchProxy ObjectHandle::operator[](std::string_view key_name) const
{
    if (m_offset == FF_NULL_OFFSET || m_recovery == FF_RECOVER_UNDEFINED)
        throw std::runtime_error("FastFHIR: Invalid ObjectHandle; cannot resolve field assignment.");

    // Use the generated reflection engine to find the field metadata
    const std::vector<FF_FieldInfo> fields = reflected_fields(m_recovery);

    for (const FF_FieldInfo &field : fields) {
        if (field.name && key_name == std::string_view(field.name)) {

            // FastFHIR only supports appending complex types/strings out-of-line
            if (field.kind != FF_FIELD_BLOCK && field.kind != FF_FIELD_ARRAY && field.kind != FF_FIELD_STRING)
                throw std::runtime_error("FastFHIR: Assignment supports only pointer-backed fields (BLOCK/ARRAY/STRING).");

            return PointerPatchProxy(m_builder, PointerRef{m_offset, field.field_offset, field.child_recovery});
        }
    }

    throw std::runtime_error("FastFHIR: Field key '" + std::string(key_name) + "' not found on current object.");
}

// =====================================================================
// Finalization & Checksums
// =====================================================================
void Builder::set_root(const ObjectHandle &handle)
{
    if (handle.offset() != FF_NULL_OFFSET && handle.recovery() == FF_RECOVER_UNDEFINED) {
        throw std::invalid_argument("FastFHIR: Cannot set a root resource with an UNDEFINED recovery tag.");
    }

    if (!try_begin_mutation()) {
        throw std::runtime_error("FastFHIR: Builder is finalizing; set_root is no longer allowed.");
    }

    struct MutationGuard {
        Builder *self;
        ~MutationGuard() { self->end_mutation(); }
    } guard{this};

    m_root_offset = handle.offset();
    m_root_recovery = handle.recovery();
}

Offset Builder::allocate_raw(Size size)
{
    return m_memory.claim_space(size);
}

FF_Result Builder::write_offset_at(Offset target_addr, Offset child_offset)
{
    STORE_U64(m_base + target_addr, child_offset);
    return FF_SUCCESS;
}

Memory::View Builder::finalize(FF_Checksum_Algorithm algo, const HashCallback &hasher)
{
    bool expected = false;
    if (!m_finalizing.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        throw std::runtime_error("FastFHIR: finalize() has already started or completed.");

    // Wait for in-flight append/amend/set_root calls to finish.
    while (m_active_mutators.load(std::memory_order_acquire) != 0)
        std::this_thread::yield();

    // Finalization sanity check: Ensure a root resource was set and is within bounds
    if (m_root_offset == FF_NULL_OFFSET || m_root_offset >= m_memory.capacity())
        throw std::runtime_error("FastFHIR: Cannot finalize without a valid in-range root resource.");
    else if (m_root_recovery == FF_RECOVER_UNDEFINED)
        throw std::runtime_error("FastFHIR: Cannot finalize stream. Root resource recovery tag is UNDEFINED.");

    // Reserve space for the checksum at the end of the stream and write the header metadata
    m_checksum_offset = m_memory.claim_space(FF_CHECKSUM::HEADER_SIZE);

    // If no hasher or algorithm is provided, default to FF_CHECKSUM_NONE and emit a warning. 
    // The stream will still be valid but with a zeroed checksum.
    if (hasher == nullptr || algo == FF_CHECKSUM_NONE) {
        std::cerr << "[FastFHIR] Warning: No hash function provided; file will be emitted with zeroed checksum.\n";
        algo = FF_CHECKSUM_NONE;
    }
    
    // Write the FF_HEADER with the root resource info and checksum location, 
    // which is needed for validation and recovery tools to find the root and checksum.
    STORE_FF_HEADER(
        m_base,             // BYTE *const __base
        m_fhir_rev,         // uint16_t fhir_revision
        m_memory.size(),    // Offset checksum_offset
        m_root_offset,      // Offset root_offset
        m_root_recovery,    // RECOVERY_TAG root_recovery
        m_checksum_offset   // Size payload_size
    );

    // Writes the 12 bytes of metadata, returns a pointer to byte 12 (the 32-byte slot)
    BYTE *hash_dst = STORE_FF_CHECKSUM_METADATA(m_base, m_checksum_offset, algo);

    // Seal the stream with the hash algorithm
    if (hasher != nullptr && algo != FF_CHECKSUM_NONE) {
        // Hash the payload + the 12 bytes of metadata, stopping exactly where the hash slot begins.
        Size bytes_to_hash = m_checksum_offset + FF_CHECKSUM::HASH_DATA;
        std::vector<BYTE> hash_value = hasher(m_base, bytes_to_hash);

        size_t copy_len = std::min(hash_value.size(), static_cast<size_t>(FF_MAX_HASH_BYTES));
        std::memcpy(hash_dst, hash_value.data(), copy_len);
    }

    // Return the lifetime-safe view to the memory
    return m_memory.view();
}
} // namespace FastFHIR
