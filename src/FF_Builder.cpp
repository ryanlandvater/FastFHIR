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
// Mutable Entry Implementation
// =====================================================================
ObjectHandle MutableEntry::as_handle() const {
    auto base = m_builder->memory().base();
    
    // --- INLINE POLYMORPHIC TUPLE ---
    if (m_target_recovery == RECOVER_FF_RESOURCE) {
        Offset target = LOAD_U64(base + m_parent_offset + m_vtable_offset); 
        RECOVERY_TAG tag = static_cast<RECOVERY_TAG>
            (LOAD_U16(base + m_parent_offset + m_vtable_offset + DATA_BLOCK::RECOVERY));
        
        if (target == FF_NULL_OFFSET) 
            return ObjectHandle(m_builder, FF_NULL_OFFSET, FF_RECOVER_UNDEFINED);
            
        return ObjectHandle(m_builder, target, tag);
    }
    
    // --- STANDARD 8-BYTE POINTER ---
    Offset target = LOAD_U64(base + m_parent_offset + m_vtable_offset);
    if (target == FF_NULL_OFFSET)
        return ObjectHandle(m_builder, FF_NULL_OFFSET, FF_RECOVER_UNDEFINED);
        
    RECOVERY_TAG actual_tag = static_cast<RECOVERY_TAG>
        (LOAD_U16(base + target + DATA_BLOCK::RECOVERY));
    return ObjectHandle(m_builder, target, actual_tag);
}

MutableEntry& MutableEntry::operator=(const ObjectHandle& child) {
    validate_assignment(child.recovery());
    auto base = const_cast<BYTE*>(m_builder->memory().base());
    
    // --- INLINE POLYMORPHIC TUPLE ---
    if (m_target_recovery == RECOVER_FF_RESOURCE) {
        m_builder->amend_resource(m_parent_offset, m_vtable_offset, child.offset(), child.recovery());
        return *this;
    }
    
    // --- STANDARD 8-BYTE POINTER ---
    m_builder->amend_pointer(m_parent_offset, m_vtable_offset, child.offset());
    return *this;
}

// Inlined in header MutableEntry::operator[](size_t index) const

void MutableEntry::validate_assignment(RECOVERY_TAG child_tag) const
{
    if (m_target_recovery == RECOVER_FF_RESOURCE) {
        // Polymorphic field: Ensure the child is actually a top-level Resource
        if (!FF_IsResourceTag(child_tag)) {
            throw std::invalid_argument("FastFHIR Schema Violation: Expected a top-level Resource (0x0200 range).");
        }
        // Strictly typed field: Ensure an exact match
    } else if (child_tag != m_target_recovery)
        throw std::invalid_argument("FastFHIR Schema Violation: MutableEntry attempted to assign an incompatible ObjectHandle type. Assigned types must match current types");
}

// =====================================================================
// Object Handle Implementation
// =====================================================================

MutableEntry ObjectHandle::operator[](size_t index) const
{
    // 1. High-level bounds checking
    Node arr_node = as_node();
    if (!arr_node.is_array())
        throw std::runtime_error("FastFHIR: Memory block is not an array.");
    if (index >= arr_node.size())
        throw std::out_of_range("FastFHIR: Array index out of bounds.");

    // 2. Low-level geometry calculation
    FF_ARRAY array_block(m_offset, 0, 0);
    auto base = m_builder->memory().base();
    
    uint16_t step = array_block.entry_step(base);
    const BYTE* entries_ptr = array_block.entries(base);
    
    size_t entry_vtable_offset = static_cast<size_t>(entries_ptr - (base + m_offset)) + (index * step);

    // Return the bridge to the slot.
    // Type discovery happens lazily in as_handle(), and schema safety is
    // enforced at compile-time by the ffc.py generated code.
    return MutableEntry(
        m_builder,
        m_offset,
        entry_vtable_offset,
        FF_RECOVER_UNDEFINED
    );
}

// =====================================================================
// Constructor / Destructor
// =====================================================================

Builder::Builder(const Memory& memory, FHIR_VERSION fhir_revision)
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
    
    // Standard, non-atomic memory write
    STORE_U64(const_cast<BYTE*>(m_base) + object_offset + field_vtable_offset, new_target_offset);
}

void Builder::amend_resource(Offset object_offset, size_t field_vtable_offset, Offset new_target_offset, RECOVERY_TAG new_tag)
{
    size_t capacity = m_memory.capacity();
    
    // Validate bounds for the full 10-byte span
    if (object_offset > capacity || field_vtable_offset > (capacity - object_offset) ||
        (sizeof(Offset) + sizeof(RECOVERY_TAG)) > (capacity - object_offset - field_vtable_offset)) {
        throw std::runtime_error("FastFHIR: Resource amendment out of bounds.");
    }

    Offset current_val = LOAD_U64(m_base + object_offset + field_vtable_offset);
    
    if (current_val != FF_NULL_OFFSET) {
        throw std::runtime_error("FastFHIR: Resource amendment failed — field already assigned.");
    }
    
    // Write both pieces natively
    STORE_U64(const_cast<BYTE*>(m_base) + object_offset + field_vtable_offset, new_target_offset);
    STORE_U16(const_cast<BYTE*>(m_base) + object_offset + field_vtable_offset + DATA_BLOCK::RECOVERY, new_tag);
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
