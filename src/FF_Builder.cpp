/**
 * @file FF_Builder.cpp
 * @author Ryan Landvater (ryanlandvater[at]gmail[dot]com)
 * @brief Concurrent lock-free FastFHIR stream builder — implementation.
 * @version 0.1
 * @date 2026-03-18
 * @copyright Copyright (c) 2026 Ryan Landvater. All rights reserved.
 * @remark This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0 (MPL-2.0) — see LICENSE or http://mozilla.org/MPL/2.0/.
 */

#include "FF_Utilities.hpp"
#include "FF_Builder.hpp"
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
// Constructor / Destructor
// =====================================================================

Builder::Builder(const Memory& memory, FHIR_VERSION fhir_revision)
: m_memory(memory),
m_base(memory.base()),
m_root_offset(FF_NULL_OFFSET),
m_root_recovery(FF_RECOVER_UNDEFINED),
m_fhir_rev(fhir_revision),
m_ff_version(FASTFHIR_VERSION_MAJOR<<16|FASTFHIR_VERSION_MINOR),
m_finalizing(false),
m_active_mutators(0)
{
    if (!m_memory) {
        throw std::invalid_argument("FastFHIR: Cannot initialize Builder with a null FF_Memory handle.");
    }

    // If the provided memory already contains a valid finalized FastFHIR archive,
    // hydrate root metadata from the stream header so callers can immediately
    // access stream.root without an explicit set_root() call.
    // Parser throws on fresh/provisional memory (header validation fails) —
    // treat that as a new writable stream.
    bool parsed_existing_stream = false;
    FF_StreamCompaction existing_layout = FF_STREAM_COMPACTION_NONE;
    try {
        Parser p(m_memory);
        parsed_existing_stream = true;
        existing_layout = p.stream_layout();
        if (p.m_root_offset   != FF_NULL_OFFSET &&
            p.m_root_recovery != FF_RECOVER_UNDEFINED) {
            m_root_offset   = p.m_root_offset;
            m_root_recovery = p.m_root_recovery;
            m_fhir_rev      = static_cast<FHIR_VERSION>(p.m_version);
        }

        // Re-open for append: reclaim the old checksum footer so new writes
        // extend from the payload tail rather than accumulating stale checksum
        // blocks in the middle of the stream.
        const Size sealed_size = p.size_bytes();
        FF_HEADER header(sealed_size);
        FF_CHECKSUM checksum = header.get_checksum(m_base);
        if (checksum &&
            checksum.__offset >= FF_HEADER::HEADER_SIZE &&
            checksum.__offset <= sealed_size) {
            // Rewind write head to the start of the existing checksum block.
            m_memory.reset(checksum.__offset);
            // Mark checksum as absent until finalize() appends a new one.
            STORE_U64(const_cast<BYTE*>(m_base) + FF_HEADER::CHECKSUM_OFFSET, FF_NULL_OFFSET);
        }
        
    } catch (const std::exception&) {
        // No valid FastFHIR stream detected — this is a new stream, leave root unset.
    }

    if (parsed_existing_stream && existing_layout == FF_STREAM_COMPACTED) {
        throw std::runtime_error(
            "FastFHIR: Cannot open Builder on a compact archive. "
            "Decompact to a standard stream before append/mutation."
        );
    }

    // Fresh writable streams start with committed size 0. Reserve exactly
    // FF_HEADER::HEADER_SIZE bytes (currently 54) at offset 0 so that all
    // appended blocks begin after the header. The header fields themselves are
    // written lazily during finalize() via STORE_FF_HEADER. URL_DIR_OFFSET and
    // MODULE_REG_OFFSET start as FF_NULL_OFFSET and are patched by
    // FF_PredigestExtensionURLs / WASM subsystem before finalize() is called.
    if (m_memory.size() == 0) {
        m_memory.claim_space(FF_HEADER::HEADER_SIZE);
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
// View Reflective::Node & Amend Pointer
// =====================================================================

Reflective::Node Builder::view_node(Offset offset, RECOVERY_TAG recovery, FF_FieldKind kind) const
{
    // 1. Snapshot the atomic boundary once
    Size size = m_memory.size();

    if (offset == FF_NULL_OFFSET || offset >= size)
        return Reflective::Node();

    // 2. Use the exact same boundary for the Reflective::Node's validation
    return Reflective::Node(m_base, size, m_fhir_rev, offset, recovery, kind);
}

Builder::AmendScope Builder::_amend_prepare(Offset object_offset, size_t field_vtable_offset,
                                            size_t total_bytes, AssignedProbe probe,
                                            const char *what)
{
    // Take the guard FIRST and hand it back to the caller still open.
    // finalize() waits for m_active_mutators to reach zero and then seals the
    // stream, so releasing the guard when this function returns would let
    // finalize() seal between the check below and the caller's STORE.
    if (!try_begin_mutation())
        throw std::runtime_error("FastFHIR: Builder is finalizing; amend is no longer allowed.");
    AmendScope scope(this, nullptr);

    // Bounds, written as subtractions rather than `a + b + c > capacity`.
    // Offset is 64-bit: a caller passing FF_NULL_OFFSET would wrap the addition
    // to a small number, pass the test, and hand back a wild pointer to STORE.
    const size_t capacity = m_memory.capacity();
    if (object_offset > capacity || field_vtable_offset > (capacity - object_offset) ||
        total_bytes > (capacity - object_offset - field_vtable_offset)) {
        throw std::runtime_error(std::string("FastFHIR: ") + what + " amendment out of bounds.");
    }

    BYTE *slot = const_cast<BYTE *>(m_base) + object_offset + field_vtable_offset;

    // Reject an already-assigned slot: patching one orphans whatever the stream
    // already points at -- the old target stays in the arena, unreferenced.
    //
    // NOTE: this read-then-write is not atomic. Two threads amending the same
    // slot can both observe it unassigned. See TASKS.md C6/Q9.
    const bool assigned = (probe == AssignedProbe::TagIsZero)
                              // A variant's 8 payload bytes are raw bits, so any
                              // value is legal -- only the tag says "set".
                              ? LOAD_U16(slot + DATA_BLOCK::RECOVERY) != 0
                              : LOAD_U64(slot) != FF_NULL_OFFSET;
    if (assigned) {
        throw std::runtime_error(
            std::string("FastFHIR: ") + what + " amendment failed — the field at offset " +
            std::to_string(object_offset) + "+" + std::to_string(field_vtable_offset) +
            " was already assigned. Patching an assigned slot risks orphaning elements "
            "of the stream.");
    }

    // Hand back the SAME guard, not a second one -- constructing a fresh
    // AmendScope here would leave two objects owning one increment, and both
    // destructors would call end_mutation().
    scope.bind(slot);
    return scope;
}

void Builder::amend_pointer(Offset object_offset, size_t field_vtable_offset, Offset new_target_offset)
{
    AmendScope scope = _amend_prepare(object_offset, field_vtable_offset,
                                      sizeof(Offset), AssignedProbe::OffsetIsNull, "Pointer");
    STORE_U64(scope.slot(), new_target_offset);
}

void Builder::amend_resource(Offset object_offset, size_t field_vtable_offset, Offset new_target_offset, RECOVERY_TAG new_tag)
{
    AmendScope scope = _amend_prepare(object_offset, field_vtable_offset,
                                      sizeof(Offset) + sizeof(RECOVERY_TAG),
                                      AssignedProbe::OffsetIsNull, "Resource");
    STORE_U64(scope.slot(), new_target_offset);
    STORE_U16(scope.slot() + DATA_BLOCK::RECOVERY, new_tag);
}

void Builder::amend_variant(Offset object_offset, size_t field_vtable_offset, uint64_t raw_bits, RECOVERY_TAG new_tag)
{
    AmendScope scope = _amend_prepare(object_offset, field_vtable_offset,
                                      sizeof(uint64_t) + sizeof(RECOVERY_TAG),
                                      AssignedProbe::TagIsZero, "Variant");
    STORE_U64(scope.slot(), raw_bits);
    STORE_U16(scope.slot() + DATA_BLOCK::RECOVERY, new_tag);
}

// =====================================================================
// Finalization & Checksums
// =====================================================================
void Builder::set_root(const Reflective::ObjectHandle &handle)
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
        throw std::runtime_error("FastFHIR: Cannot finalize because root is unset/invalid. Calling application must set root explicitly.");
    else if (m_root_recovery == FF_RECOVER_UNDEFINED)
        throw std::runtime_error("FastFHIR: Cannot finalize stream. Root recovery tag is UNDEFINED. Calling application must set root explicitly.");

    // If no hasher or algorithm is provided, default to FF_CHECKSUM_NONE and emit a warning. 
    // The stream will still be valid but with a zeroed checksum.
    if (hasher == nullptr || algo == FF_CHECKSUM_NONE) {
        std::cerr << "[FastFHIR] Warning: No hash function provided; file will be emitted with zeroed checksum.\n";
        algo = FF_CHECKSUM_NONE;
    }

    // Shared sealing (header + checksum + hash) with Compactor::archive. The
    // URL/module directory offsets are builder state; the stream stays standard
    // layout. The backing file is truncated to the sealed size afterwards.
    const Memory::View view = seal_stream(m_memory, m_fhir_rev, m_root_offset,
                                          m_root_recovery, algo, hasher,
                                          FF_STREAM_COMPACTION_NONE,
                                          m_url_dir_offset, m_module_reg_offset);
    m_memory.truncate_file(view.size());
    return view;
}
// =====================================================================
// Mutable Entry Implementation
// =====================================================================
namespace Reflective {
Entry MutableEntry::as_entry() const {
    return Entry(m_base, m_parent_offset, m_vtable_offset, m_recovery, m_kind);
}

ObjectHandle MutableEntry::as_handle() const {
    if (!m_builder) return ObjectHandle();
    
    auto base_ptr = m_builder->memory().base();
    
    if (m_kind == FF_FIELD_RESOURCE || m_kind == FF_FIELD_CHOICE || m_recovery == RECOVER_FF_RESOURCE) {
        Offset target = LOAD_U64(base_ptr + m_parent_offset + m_vtable_offset); 
        if (target == FF_NULL_OFFSET) 
            return ObjectHandle(m_builder, FF_NULL_OFFSET, FF_RECOVER_UNDEFINED);
            
        RECOVERY_TAG actual_tag = static_cast<RECOVERY_TAG>
            (LOAD_U16(base_ptr + m_parent_offset + m_vtable_offset + DATA_BLOCK::RECOVERY));
        
        return ObjectHandle(m_builder, target, actual_tag);
    }
    
    Offset target = LOAD_U64(base_ptr + m_parent_offset + m_vtable_offset);
    if (target == FF_NULL_OFFSET)
        return ObjectHandle(m_builder, FF_NULL_OFFSET, FF_RECOVER_UNDEFINED);
        
    RECOVERY_TAG actual_tag = static_cast<RECOVERY_TAG>
        (LOAD_U16(base_ptr + target + DATA_BLOCK::RECOVERY));
    return ObjectHandle(m_builder, target, actual_tag);
}

MutableEntry& MutableEntry::operator=(const ObjectHandle& child) {
    validate_assignment(child.recovery());
    
    // --- INLINE POLYMORPHIC TUPLE ---
    if (m_recovery == RECOVER_FF_RESOURCE) {
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
    if (m_recovery == RECOVER_FF_RESOURCE) {
        // Polymorphic field: Ensure the child is actually a top-level Resource
        if (!FF_IsResourceTag(child_tag)) {
            throw std::invalid_argument("FastFHIR Schema Violation: Expected a top-level Resource (0x0200 range).");
        }
        // Strictly typed field: Ensure an exact match
    } else if (m_recovery != FF_RECOVER_UNDEFINED && child_tag != m_recovery)
        throw std::invalid_argument("FastFHIR Schema Violation: MutableEntry attempted to assign an incompatible ObjectHandle type. Assigned types must match current types");
}

// =====================================================================
// Object Handle Implementation
// =====================================================================

MutableEntry ObjectHandle::operator[](size_t index) const
{
    // 1. High-level bounds checking
    Reflective::Node arr_node = as_node();
    if (!arr_node.is_array())
        throw std::runtime_error("FastFHIR: Memory block is not an array.");
    if (index >= arr_node.size())
        throw std::out_of_range("FastFHIR: Array index out of bounds.");

    // 2. Low-level geometry calculation
    FF_ARRAY array_block(m_offset, 0, 0);
    auto base = m_builder->memory().base();
    
    uint16_t step = array_block.entry_step(base);
    const BYTE* entries_ptr = array_block.entries(base);
    
    uint32_t entry_vtable_offset = static_cast<uint32_t>(static_cast<size_t>(entries_ptr - (base + m_offset)) + (index * step));

    // Return the bridge to the slot.
    // Type discovery may happen lazily in as_handle(), and schema safety is
    // enforced at compile-time by the ffc.py generated code.
    return MutableEntry(
        m_builder,
        m_offset,
        entry_vtable_offset,
        GetTypeFromTag(m_recovery),
        Recovery_to_Kind(m_recovery)
    );
}
} // namespace Reflective
} // namespace FastFHIR
